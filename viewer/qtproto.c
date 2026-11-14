/*
 * Relios Mirror: Apple's USB screen-capture protocol (see qtproto.h).
 *
 * The stream from the device is a sequence of messages, each with a 32-bit little-endian length (counting itself)
 * and a four-letter code, also written as a little-endian number ('sync' arrives as the bytes "cnys"):
 *   PING            the device says hello; answered with the same
 *   SYNC <subtype>  needs an answer (RPLY with the same correlation number): clocks, the sound format, time, stop
 *   ASYN <subtype>  needs none: pictures (FEED), sound (EAT!), properties, releases (RELS)
 * Pictures and sound come as serialized CoreMedia sample buffers ("sbuf"): chunks of length + code + data.
 * After every picture the receiver asks for the next one (NEED).
 */
#include "qtproto.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define QT_MAX_PACKET (32u * 1024u * 1024u)
#define FCC(a, b, c, d) (((uint32_t)(a) << 24) | ((uint32_t)(b) << 16) | ((uint32_t)(c) << 8) | (uint32_t)(d))
#define EMPTY_CLOCK 1u

struct qt_session {
	struct qt_sink sink;
	struct qt_options opt;
	enum qt_state state;
	uint64_t epoch_ns, now_ns;
	bool epoch_set;
	uint64_t dev_audio_clock, dev_video_clock;
	uint64_t video_frames, audio_packets, replies;
	int releases;
	unsigned bad_samples, logged, traced;

	/* a message split over reads */
	uint8_t *rbuf;
	size_t rlen, rcap;

	/* the picture format */
	bool have_video_format, h265;
	int nal_len, width, height;
	uint8_t *params; /* the parameter sets, Annex B */
	size_t params_len;
	bool params_sent;

	/* the sound format */
	double rate;
	int channels, bits;
	uint32_t flags;

	uint8_t *vout, *aout;
	size_t vout_cap, aout_cap;
};

/* ------------------------------------------------------------------ bytes */

static uint32_t rd32(const uint8_t *p)
{
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint64_t rd64(const uint8_t *p)
{
	return (uint64_t)rd32(p) | ((uint64_t)rd32(p + 4) << 32);
}

static double rdf64(const uint8_t *p)
{
	uint64_t bits = rd64(p);
	double v;
	memcpy(&v, &bits, sizeof(v));
	return v;
}

static void wr32(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)v;
	p[1] = (uint8_t)(v >> 8);
	p[2] = (uint8_t)(v >> 16);
	p[3] = (uint8_t)(v >> 24);
}

static bool grow(uint8_t **p, size_t *cap, size_t want)
{
	if (want <= *cap)
		return true;
	size_t n = *cap ? *cap : 256;
	while (n < want) {
		if (n > SIZE_MAX / 2)
			return false;
		n *= 2;
	}
	uint8_t *bigger = realloc(*p, n);
	if (!bigger)
		return false;
	*p = bigger;
	*cap = n;
	return true;
}

void qt_out_clear(struct qt_out *o)
{
	o->n = 0;
	o->failed = false;
}

void qt_out_free(struct qt_out *o)
{
	free(o->p);
	memset(o, 0, sizeof(*o));
}

static void put(struct qt_out *o, const void *data, size_t len)
{
	if (o->failed || !grow(&o->p, &o->cap, o->n + len)) {
		o->failed = true;
		return;
	}
	memcpy(o->p + o->n, data, len);
	o->n += len;
}

static void put32(struct qt_out *o, uint32_t v)
{
	uint8_t b[4];
	wr32(b, v);
	put(o, b, 4);
}

static void put64(struct qt_out *o, uint64_t v)
{
	put32(o, (uint32_t)v);
	put32(o, (uint32_t)(v >> 32));
}

static void putf64(struct qt_out *o, double v)
{
	uint64_t bits;
	memcpy(&bits, &v, sizeof(bits));
	put64(o, bits);
}

/* a length-prefixed piece: its length is written by end() */
static size_t begin(struct qt_out *o, uint32_t code)
{
	size_t at = o->n;
	put32(o, 0);
	put32(o, code);
	return at;
}

static void end(struct qt_out *o, size_t at)
{
	if (!o->failed && at + 4 <= o->n)
		wr32(o->p + at, (uint32_t)(o->n - at));
}

static void say(struct qt_session *s, const char *fmt, ...)
{
	if (!s->sink.log)
		return;
	char text[300];
	va_list args;
	va_start(args, fmt);
	vsnprintf(text, sizeof(text), fmt, args);
	va_end(args);
	s->sink.log(s->sink.ctx, text);
}

static void code_text(uint32_t code, char out[5])
{
	for (int i = 0; i < 4; i++) {
		char c = (char)(code >> (24 - 8 * i));
		out[i] = c >= 32 && c < 127 ? c : '?';
	}
	out[4] = '\0';
}

/* ------------------------------------------------------------------ dictionaries (keyed values) */

static size_t key(struct qt_out *o, const char *name)
{
	size_t kv = begin(o, FCC('k', 'e', 'y', 'v'));
	size_t k = begin(o, FCC('s', 't', 'r', 'k'));
	put(o, name, strlen(name));
	end(o, k);
	return kv;
}

static void val_bool(struct qt_out *o, bool v)
{
	size_t at = begin(o, FCC('b', 'u', 'l', 'v'));
	uint8_t b = v ? 1 : 0;
	put(o, &b, 1);
	end(o, at);
}

static void val_f64(struct qt_out *o, double v)
{
	size_t at = begin(o, FCC('n', 'm', 'b', 'v'));
	uint8_t type = 6; /* (a 64-bit float) */
	put(o, &type, 1);
	putf64(o, v);
	end(o, at);
}

static void val_u32(struct qt_out *o, uint32_t v)
{
	size_t at = begin(o, FCC('n', 'm', 'b', 'v'));
	uint8_t type = 3; /* (a 32-bit integer) */
	put(o, &type, 1);
	put32(o, v);
	end(o, at);
}

static void val_str(struct qt_out *o, const char *v)
{
	size_t at = begin(o, FCC('s', 't', 'r', 'v'));
	put(o, v, strlen(v));
	end(o, at);
}

static void val_data(struct qt_out *o, const void *v, size_t len)
{
	size_t at = begin(o, FCC('d', 'a', 't', 'v'));
	put(o, v, len);
	end(o, at);
}

/* ------------------------------------------------------------------ messages to the device */

void qt_make_ping(struct qt_out *o)
{
	put32(o, 16);
	put32(o, FCC('p', 'i', 'n', 'g'));
	put32(o, 0);
	put32(o, 1);
}

static size_t async_begin(struct qt_out *o, uint64_t clock, uint32_t subtype)
{
	size_t at = begin(o, FCC('a', 's', 'y', 'n'));
	put64(o, clock);
	put32(o, subtype);
	return at;
}

static void async_control(struct qt_out *o, uint64_t clock, uint32_t subtype)
{
	end(o, async_begin(o, clock, subtype));
}

static void make_need(struct qt_out *o, uint64_t device_video_clock)
{
	async_control(o, device_video_clock, FCC('n', 'e', 'e', 'd'));
}

/* the display offered to the device */
static void make_hpd1(struct qt_session *s, struct qt_out *o)
{
	size_t m = async_begin(o, EMPTY_CLOCK, FCC('h', 'p', 'd', '1'));
	size_t d = begin(o, FCC('d', 'i', 'c', 't'));
	size_t kv = key(o, "Valeria");
	val_bool(o, s->opt.valeria);
	end(o, kv);
	kv = key(o, "HEVCDecoderSupports444");
	val_bool(o, false);
	end(o, kv);
	kv = key(o, "DisplaySize");
	size_t size = begin(o, FCC('d', 'i', 'c', 't'));
	size_t kw = key(o, "Width");
	val_f64(o, (double)s->opt.width);
	end(o, kw);
	size_t kh = key(o, "Height");
	val_f64(o, (double)s->opt.height);
	end(o, kh);
	end(o, size);
	end(o, kv);
	end(o, d);
	end(o, m);
}

/* the sound requested: 48 kHz, stereo, 16-bit PCM */
static void make_hpa1(struct qt_out *o, uint64_t device_audio_clock)
{
	struct qt_out asbd = {0};
	putf64(&asbd, 48000.0);
	put32(&asbd, FCC('l', 'p', 'c', 'm'));
	put32(&asbd, 12); /* signed integer, packed */
	put32(&asbd, 4);  /* bytes per packet */
	put32(&asbd, 1);  /* frames per packet */
	put32(&asbd, 4);  /* bytes per frame */
	put32(&asbd, 2);  /* channels */
	put32(&asbd, 16); /* bits per channel */
	put32(&asbd, 0);
	putf64(&asbd, 48000.0);
	putf64(&asbd, 48000.0);

	size_t m = async_begin(o, device_audio_clock, FCC('h', 'p', 'a', '1'));
	size_t d = begin(o, FCC('d', 'i', 'c', 't'));
	size_t kv = key(o, "BufferAheadInterval");
	val_f64(o, 0.07300000000000001);
	end(o, kv);
	kv = key(o, "deviceUID");
	val_str(o, "Valeria");
	end(o, kv);
	kv = key(o, "ScreenLatency");
	val_f64(o, 0.04);
	end(o, kv);
	kv = key(o, "formats");
	if (asbd.failed)
		o->failed = true;
	else
		val_data(o, asbd.p, asbd.n);
	end(o, kv);
	kv = key(o, "EDIDAC3Support");
	val_u32(o, 0);
	end(o, kv);
	kv = key(o, "deviceName");
	val_str(o, "Valeria");
	end(o, kv);
	end(o, d);
	end(o, m);
	qt_out_free(&asbd);
}

static void reply_clock(struct qt_out *o, uint64_t correlation, uint64_t clock)
{
	size_t at = begin(o, FCC('r', 'p', 'l', 'y'));
	put64(o, correlation);
	put32(o, 0);
	put64(o, clock);
	end(o, at);
}

static void reply_zero(struct qt_out *o, uint64_t correlation)
{
	size_t at = begin(o, FCC('r', 'p', 'l', 'y'));
	put64(o, correlation);
	put64(o, 0);
	end(o, at);
}

static void reply_afmt(struct qt_out *o, uint64_t correlation)
{
	size_t at = begin(o, FCC('r', 'p', 'l', 'y'));
	put64(o, correlation);
	put32(o, 0);
	size_t d = begin(o, FCC('d', 'i', 'c', 't'));
	size_t kv = key(o, "Error");
	val_u32(o, 0);
	end(o, kv);
	end(o, d);
	end(o, at);
}

static void reply_time(struct qt_out *o, uint64_t correlation, uint64_t nanoseconds)
{
	size_t at = begin(o, FCC('r', 'p', 'l', 'y'));
	put64(o, correlation);
	put32(o, 0);
	/* a CMTime: value, timescale, flags (valid), epoch */
	put64(o, nanoseconds);
	put32(o, 1000000000u);
	put32(o, 1);
	put64(o, 0);
	end(o, at);
}

static void reply_skew(struct qt_out *o, uint64_t correlation, double skew)
{
	size_t at = begin(o, FCC('r', 'p', 'l', 'y'));
	put64(o, correlation);
	put32(o, 0);
	putf64(o, skew);
	end(o, at);
}

/* ------------------------------------------------------------------ formats */

static void set_audio_format(struct qt_session *s, const uint8_t *asbd)
{
	double rate = rdf64(asbd);
	uint32_t format = rd32(asbd + 8), flags = rd32(asbd + 12);
	uint32_t channels = rd32(asbd + 28), bits = rd32(asbd + 32);
	if (format != FCC('l', 'p', 'c', 'm') || !(rate >= 8000.0 && rate <= 192000.0) || channels < 1 || channels > 8 ||
	    (bits != 16 && bits != 32)) {
		char f[5];
		code_text(format, f);
		say(s, "sound format not used: %s, %.0f Hz, %u channels, %u bits", f, rate, (unsigned)channels,
		    (unsigned)bits);
		return;
	}
	if (s->rate != rate || s->channels != (int)channels || s->bits != (int)bits || s->flags != flags)
		say(s, "sound: %.0f Hz, %u channels, %u bits (flags 0x%x)", rate, (unsigned)channels, (unsigned)bits,
		    (unsigned)flags);
	s->rate = rate;
	s->channels = (int)channels;
	s->bits = (int)bits;
	s->flags = flags;
}

static void append_nal(uint8_t **buf, size_t *len, size_t *cap, const uint8_t *nal, size_t n, bool *ok)
{
	if (!*ok || !grow(buf, cap, *len + 4 + n)) {
		*ok = false;
		return;
	}
	static const uint8_t start[4] = {0, 0, 0, 1};
	memcpy(*buf + *len, start, 4);
	memcpy(*buf + *len + 4, nal, n);
	*len += 4 + n;
}

/* an AVCDecoderConfigurationRecord: the NAL length size, SPS and PPS (into Annex B) */
static bool parse_avcc(const uint8_t *c, size_t n, int *nal_len, uint8_t **out, size_t *out_len)
{
	if (n < 7 || c[0] != 1)
		return false;
	size_t pos = 4, cap = 0, len = 0;
	uint8_t *buf = NULL;
	bool ok = true;
	*nal_len = (c[pos++] & 3) + 1;
	int sps = c[pos++] & 0x1f, count = 0;
	for (int i = 0; i < sps && ok; i++) {
		if (n - pos < 2)
			ok = false;
		else {
			size_t l = (size_t)((c[pos] << 8) | c[pos + 1]);
			pos += 2;
			if (l == 0 || l > n - pos)
				ok = false;
			else {
				append_nal(&buf, &len, &cap, c + pos, l, &ok);
				pos += l;
				count++;
			}
		}
	}
	if (ok && n - pos >= 1) {
		int pps = c[pos++];
		for (int i = 0; i < pps && ok; i++) {
			if (n - pos < 2)
				ok = false;
			else {
				size_t l = (size_t)((c[pos] << 8) | c[pos + 1]);
				pos += 2;
				if (l == 0 || l > n - pos)
					ok = false;
				else {
					append_nal(&buf, &len, &cap, c + pos, l, &ok);
					pos += l;
					count++;
				}
			}
		}
	} else {
		ok = false;
	}
	if (!ok || count < 2) {
		free(buf);
		return false;
	}
	*out = buf;
	*out_len = len;
	return true;
}

/* an HEVCDecoderConfigurationRecord: the NAL length size, VPS, SPS and PPS */
static bool parse_hvcc(const uint8_t *c, size_t n, int *nal_len, uint8_t **out, size_t *out_len)
{
	if (n < 23 || c[0] != 1)
		return false;
	size_t pos = 23, cap = 0, len = 0;
	uint8_t *buf = NULL;
	bool ok = true, sps = false, pps = false;
	*nal_len = (c[21] & 3) + 1;
	int arrays = c[22];
	for (int a = 0; a < arrays && ok; a++) {
		if (n - pos < 3) {
			ok = false;
			break;
		}
		int type = c[pos] & 0x3f;
		int count = (c[pos + 1] << 8) | c[pos + 2];
		pos += 3;
		for (int i = 0; i < count && ok; i++) {
			if (n - pos < 2) {
				ok = false;
				break;
			}
			size_t l = (size_t)((c[pos] << 8) | c[pos + 1]);
			pos += 2;
			if (l == 0 || l > n - pos) {
				ok = false;
				break;
			}
			if (type == 32 || type == 33 || type == 34) {
				append_nal(&buf, &len, &cap, c + pos, l, &ok);
				sps = sps || type == 33;
				pps = pps || type == 34;
			}
			pos += l;
		}
	}
	if (!ok || !sps || !pps) {
		free(buf);
		return false;
	}
	*out = buf;
	*out_len = len;
	return true;
}

/* the decoder configuration among the format's extensions: a data value ("datv") holding the record */
static bool find_config(const uint8_t *ext, size_t n, bool h265, int *nal_len, uint8_t **out, size_t *out_len)
{
	for (size_t i = 4; i + 4 <= n; i++) {
		if (rd32(ext + i) != FCC('d', 'a', 't', 'v'))
			continue;
		uint32_t clen = rd32(ext + i - 4);
		if (clen < 8 || clen > n - (i - 4))
			continue;
		const uint8_t *c = ext + i + 4;
		size_t cn = clen - 8;
		if (h265 ? parse_hvcc(c, cn, nal_len, out, out_len) : parse_avcc(c, cn, nal_len, out, out_len))
			return true;
	}
	return false;
}

static void parse_format(struct qt_session *s, const uint8_t *d, size_t n)
{
	uint32_t media = 0, codec = 0;
	int width = 0, height = 0;
	const uint8_t *ext = NULL, *asbd = NULL;
	size_t ext_len = 0;
	for (size_t off = 0; off + 8 <= n;) {
		uint32_t clen = rd32(d + off), code = rd32(d + off + 4);
		if (clen < 8 || clen > n - off)
			return;
		const uint8_t *p = d + off + 8;
		size_t pn = clen - 8;
		if (code == FCC('m', 'd', 'i', 'a') && pn >= 4)
			media = rd32(p);
		else if (code == FCC('v', 'd', 'i', 'm') && pn >= 8) {
			width = (int)rd32(p);
			height = (int)rd32(p + 4);
		} else if (code == FCC('c', 'o', 'd', 'c') && pn >= 4)
			codec = rd32(p);
		else if (code == FCC('e', 'x', 't', 'n')) {
			ext = p;
			ext_len = pn;
		} else if (code == FCC('a', 's', 'b', 'd') && pn >= 40)
			asbd = p;
		off += clen;
	}
	if (media == FCC('s', 'o', 'u', 'n')) {
		if (asbd)
			set_audio_format(s, asbd);
		return;
	}
	if (media != FCC('v', 'i', 'd', 'e'))
		return;
	bool h265;
	if (codec == FCC('a', 'v', 'c', '1') || codec == FCC('a', 'v', 'c', '3'))
		h265 = false;
	else if (codec == FCC('h', 'v', 'c', '1') || codec == FCC('h', 'e', 'v', '1'))
		h265 = true;
	else {
		char c[5];
		code_text(codec, c);
		say(s, "picture format not supported: %s", c);
		return;
	}
	int nal_len = 4;
	uint8_t *params = NULL;
	size_t params_len = 0;
	if (!ext || !find_config(ext, ext_len, h265, &nal_len, &params, &params_len)) {
		s->bad_samples++;
		if (s->logged++ < 20)
			say(s, "picture format without a decoder configuration");
		return;
	}
	bool changed = !s->have_video_format || s->h265 != h265 || s->width != width || s->height != height ||
		       s->params_len != params_len || memcmp(s->params, params, params_len) != 0;
	if (changed) {
		free(s->params);
		s->params = params;
		s->params_len = params_len;
		s->params_sent = false;
		s->h265 = h265;
		s->width = width;
		s->height = height;
		s->nal_len = nal_len;
		s->have_video_format = true;
		say(s, "picture: %dx%d %s, NAL length %d", width, height, h265 ? "H.265" : "H.264", nal_len);
	} else {
		free(params);
	}
}

/* ------------------------------------------------------------------ media */

static void deliver_video(struct qt_session *s, const uint8_t *data, size_t n)
{
	if (!s->have_video_format || n == 0 || !s->sink.video)
		return;
	int nl = s->nal_len >= 1 && s->nal_len <= 4 ? s->nal_len : 4;
	size_t pos = 0, size = 0;
	bool key_frame = false;
	while (pos < n) {
		if (n - pos < (size_t)nl)
			goto bad;
		size_t l = 0;
		for (int i = 0; i < nl; i++)
			l = (l << 8) | data[pos + (size_t)i];
		pos += (size_t)nl;
		if (l == 0 || l > n - pos)
			goto bad;
		uint8_t head = data[pos];
		if (s->h265) {
			int type = (head >> 1) & 0x3f;
			key_frame = key_frame || (type >= 16 && type <= 21);
		} else {
			key_frame = key_frame || (head & 0x1f) == 5;
		}
		size += 4 + l;
		pos += l;
	}
	bool with_params = s->params_len && (!s->params_sent || key_frame);
	if (with_params)
		size += s->params_len;
	if (!grow(&s->vout, &s->vout_cap, size + 64))
		return;
	size_t out = 0;
	if (with_params) {
		memcpy(s->vout, s->params, s->params_len);
		out = s->params_len;
	}
	for (pos = 0; pos < n;) {
		size_t l = 0;
		for (int i = 0; i < nl; i++)
			l = (l << 8) | data[pos + (size_t)i];
		pos += (size_t)nl;
		static const uint8_t start[4] = {0, 0, 0, 1};
		memcpy(s->vout + out, start, 4);
		memcpy(s->vout + out + 4, data + pos, l);
		out += 4 + l;
		pos += l;
	}
	memset(s->vout + out, 0, 64);
	if (with_params)
		s->params_sent = true;
	s->sink.video(s->sink.ctx, s->vout, out, s->h265, s->width, s->height);
	return;
bad:
	s->bad_samples++;
	if (s->logged++ < 20)
		say(s, "a picture with a broken NAL length was skipped");
}

static void deliver_audio(struct qt_session *s, const uint8_t *data, size_t n)
{
	if (n == 0 || !s->sink.audio)
		return;
	int ch = s->channels >= 1 && s->channels <= 8 ? s->channels : 2;
	int bits = s->bits == 32 ? 32 : 16;
	double rate = s->rate > 0 ? s->rate : 48000.0;
	bool is_float = (s->flags & 1u) != 0, big_endian = (s->flags & 2u) != 0;
	size_t in_frame = (size_t)ch * (size_t)(bits / 8);
	size_t frames = n / in_frame;
	if (frames == 0 || frames > 1000000)
		return;
	if (!grow(&s->aout, &s->aout_cap, frames * (size_t)ch * 2))
		return;
	uint8_t *o = s->aout;
	for (size_t i = 0; i < frames * (size_t)ch; i++) {
		const uint8_t *p = data + i * (size_t)(bits / 8);
		int16_t v;
		if (bits == 16) {
			v = (int16_t)(big_endian ? (uint16_t)((p[0] << 8) | p[1]) : (uint16_t)(p[0] | (p[1] << 8)));
		} else {
			uint32_t u = big_endian ? ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3] :
						  rd32(p);
			if (is_float) {
				float f;
				memcpy(&f, &u, sizeof(f));
				if (!(f == f))
					f = 0.0f;
				f = f > 1.0f ? 1.0f : f < -1.0f ? -1.0f : f;
				v = (int16_t)(f * 32767.0f);
			} else {
				v = (int16_t)((int32_t)u >> 16);
			}
		}
		o[i * 2] = (uint8_t)((uint16_t)v);
		o[i * 2 + 1] = (uint8_t)((uint16_t)v >> 8);
	}
	s->sink.audio(s->sink.ctx, s->aout, (int)frames, ch, (int)(rate + 0.5));
}

/* FEED / EAT!: an envelope around a serialized CMSampleBuffer */
static void handle_media(struct qt_session *s, const uint8_t *p, size_t n, bool video)
{
	if (n < 24)
		goto bad;
	uint32_t slen = rd32(p + 16);
	if (slen < 8 || 16u + (uint64_t)slen > n || rd32(p + 20) != FCC('s', 'b', 'u', 'f'))
		goto bad;
	const uint8_t *sb = p + 20; /* from its code on */
	size_t sbn = slen - 4;
	const uint8_t *sdat = NULL, *fdsc = NULL;
	size_t sdat_len = 0, fdsc_len = 0;
	for (size_t off = 4; off + 8 <= sbn;) {
		uint32_t clen = rd32(sb + off), code = rd32(sb + off + 4);
		if (clen < 8 || clen > sbn - off)
			goto bad;
		if (code == FCC('s', 'd', 'a', 't')) {
			sdat = sb + off + 8;
			sdat_len = clen - 8;
		} else if (code == FCC('f', 'd', 's', 'c')) {
			fdsc = sb + off + 8;
			fdsc_len = clen - 8;
		}
		/* (opts, stia, nsmp, ssiz, satt, sary: timing and attachments, not needed to show it now) */
		off += clen;
	}
	if (fdsc)
		parse_format(s, fdsc, fdsc_len);
	if (video) {
		s->video_frames++;
		if (sdat)
			deliver_video(s, sdat, sdat_len);
	} else {
		s->audio_packets++;
		if (sdat)
			deliver_audio(s, sdat, sdat_len);
	}
	return;
bad:
	s->bad_samples++;
	if (s->logged++ < 20)
		say(s, "a broken %s sample was skipped", video ? "picture" : "sound");
}

/* ------------------------------------------------------------------ messages from the device */

static void handle_sync(struct qt_session *s, const uint8_t *p, size_t n, struct qt_out *o)
{
	uint64_t clock = rd64(p + 4);
	uint32_t subtype = rd32(p + 12);
	uint64_t correlation = rd64(p + 16);
	if (subtype == FCC('c', 'w', 'p', 'a')) {
		if (n < 32)
			return;
		s->dev_audio_clock = rd64(p + 24);
		make_hpd1(s, o);
		reply_clock(o, correlation, s->dev_audio_clock + 0x1000);
		if (s->opt.audio)
			make_hpa1(o, s->dev_audio_clock);
		s->state = QT_NEGOTIATING;
		say(s, "the device's clock arrived; display %dx%d%s and sound requested", s->opt.width, s->opt.height,
		    s->opt.valeria ? " (capture mode)" : "");
	} else if (subtype == FCC('a', 'f', 'm', 't')) {
		if (n < 64)
			return;
		set_audio_format(s, p + 24);
		reply_afmt(o, correlation);
	} else if (subtype == FCC('c', 'v', 'r', 'p')) {
		if (n < 32)
			return;
		s->dev_video_clock = rd64(p + 24);
		make_need(o, s->dev_video_clock);
		reply_clock(o, correlation, s->dev_video_clock + 0x1000af);
	} else if (subtype == FCC('c', 'l', 'o', 'k')) {
		reply_clock(o, correlation, clock + 0x10000);
	} else if (subtype == FCC('t', 'i', 'm', 'e')) {
		reply_time(o, correlation, s->now_ns - s->epoch_ns);
	} else if (subtype == FCC('s', 'k', 'e', 'w')) {
		reply_skew(o, correlation, 48000.0);
	} else if (subtype == FCC('g', 'o', '!', ' ')) {
		reply_zero(o, correlation);
	} else if (subtype == FCC('s', 't', 'o', 'p')) {
		reply_zero(o, correlation);
		s->state = QT_STOPPED;
		say(s, "the device stopped the capture");
	} else {
		char c[5];
		code_text(subtype, c);
		if (s->logged++ < 20)
			say(s, "unknown request from the device: %s (answered with zero)", c);
		reply_zero(o, correlation);
	}
}

static void handle_async(struct qt_session *s, const uint8_t *p, size_t n)
{
	uint32_t subtype = rd32(p + 12);
	if (subtype == FCC('f', 'e', 'e', 'd')) {
		handle_media(s, p, n, true);
	} else if (subtype == FCC('e', 'a', 't', '!')) {
		handle_media(s, p, n, false);
	} else if (subtype == FCC('r', 'e', 'l', 's')) {
		s->releases++;
	}
	/* (sprp, srat, tbas, tjmp: properties and clock rates; nothing to answer) */
}

#define QT_TRACE_MAX 40

/* one message (from its code on, n bytes) in the log, while the session is young */
static void trace(struct qt_session *s, const char *way, const uint8_t *p, size_t n)
{
	if (s->traced >= QT_TRACE_MAX || n < 4)
		return;
	s->traced++;
	uint32_t code = rd32(p);
	char c[5], sub[5] = "";
	code_text(code, c);
	if ((code == FCC('s', 'y', 'n', 'c') || code == FCC('a', 's', 'y', 'n')) && n >= 16)
		code_text(rd32(p + 12), sub);
	say(s, "%s: %s%s%s, %zu bytes", way, c, sub[0] ? " " : "", sub, n + 4);
}

void qt_session_trace_out(struct qt_session *s, const struct qt_out *o)
{
	for (size_t pos = 0; pos + 8 <= o->n && s->traced < QT_TRACE_MAX;) {
		uint32_t len = rd32(o->p + pos);
		if (len < 8 || len > o->n - pos)
			break;
		trace(s, "to the device", o->p + pos + 4, len - 4);
		pos += len;
	}
}

static void process(struct qt_session *s, const uint8_t *p, size_t n, struct qt_out *o)
{
	if (n < 4)
		return;
	trace(s, "from the device", p, n);
	uint32_t code = rd32(p);
	if (code == FCC('p', 'i', 'n', 'g')) {
		qt_make_ping(o);
		if (s->state == QT_WAIT_PING)
			s->state = QT_WAIT_CLOCK;
	} else if (code == FCC('s', 'y', 'n', 'c')) {
		if (n >= 24)
			handle_sync(s, p, n, o);
	} else if (code == FCC('a', 's', 'y', 'n')) {
		if (n < 16)
			return;
		bool video = rd32(p + 12) == FCC('f', 'e', 'e', 'd');
		handle_async(s, p, n);
		if (video) {
			bool tearing_down = s->state == QT_STOPPING || s->state == QT_STOPPED;
			if (!tearing_down && s->dev_video_clock)
				make_need(o, s->dev_video_clock);
			if (!tearing_down)
				s->state = QT_STREAMING;
		}
	} else if (code == FCC('r', 'p', 'l', 'y')) {
		s->replies++;
	} else if (s->logged++ < 20) {
		char c[5];
		code_text(code, c);
		say(s, "unknown message from the device: %s", c);
	}
}

/* ------------------------------------------------------------------ the session */

struct qt_session *qt_session_new(const struct qt_sink *sink, const struct qt_options *options)
{
	struct qt_session *s = calloc(1, sizeof(*s));
	if (!s)
		return NULL;
	if (sink)
		s->sink = *sink;
	s->opt = *options;
	if (s->opt.width < 320 || s->opt.width > 8192)
		s->opt.width = 1640;
	if (s->opt.height < 320 || s->opt.height > 8192)
		s->opt.height = 2360;
	s->state = QT_WAIT_PING;
	s->nal_len = 4;
	return s;
}

void qt_session_free(struct qt_session *s)
{
	if (!s)
		return;
	free(s->rbuf);
	free(s->params);
	free(s->vout);
	free(s->aout);
	free(s);
}

/* why the stream was given up: written to the log, so that a next report shows what came */
static void broken_length(struct qt_session *s, const uint8_t *p, size_t n, uint32_t plen)
{
	char hex[3 * 16 + 1];
	size_t k = n < 16 ? n : 16;
	for (size_t i = 0; i < k; i++)
		snprintf(hex + 3 * i, 4, "%02x ", p[i]);
	hex[3 * k] = 0;
	say(s, "a message with the length %u (%zu bytes at hand; after %llu pictures, %llu sound packets): %s", plen, n,
	    (unsigned long long)s->video_frames, (unsigned long long)s->audio_packets, hex);
}

bool qt_session_feed(struct qt_session *s, const uint8_t *data, size_t len, struct qt_out *o, uint64_t now_ns)
{
	if (!s->epoch_set) {
		s->epoch_ns = now_ns;
		s->epoch_set = true;
	}
	s->now_ns = now_ns >= s->epoch_ns ? now_ns : s->epoch_ns;
	while (len > 0) {
		if (s->rlen == 0 && len >= 4) {
			uint32_t plen = rd32(data);
			if (plen < 8 || plen > QT_MAX_PACKET) {
				broken_length(s, data, len, plen);
				return false;
			}
			if (len >= plen) {
				process(s, data + 4, plen - 4, o);
				data += plen;
				len -= plen;
				continue;
			}
		}
		if (s->rlen < 4) {
			size_t take = 4 - s->rlen < len ? 4 - s->rlen : len;
			if (!grow(&s->rbuf, &s->rcap, 4))
				return false;
			memcpy(s->rbuf + s->rlen, data, take);
			s->rlen += take;
			data += take;
			len -= take;
			if (s->rlen < 4)
				break;
		}
		uint32_t plen = rd32(s->rbuf);
		if (plen < 8 || plen > QT_MAX_PACKET) {
			broken_length(s, s->rbuf, s->rlen, plen);
			s->rlen = 0;
			return false;
		}
		if (!grow(&s->rbuf, &s->rcap, plen))
			return false;
		size_t take = plen - s->rlen < len ? plen - s->rlen : len;
		memcpy(s->rbuf + s->rlen, data, take);
		s->rlen += take;
		data += take;
		len -= take;
		if (s->rlen < plen)
			break;
		s->rlen = 0;
		process(s, s->rbuf + 4, plen - 4, o);
	}
	return true;
}

void qt_session_stop(struct qt_session *s, struct qt_out *o)
{
	if (s->opt.audio)
		async_control(o, s->dev_audio_clock ? s->dev_audio_clock : EMPTY_CLOCK, FCC('h', 'p', 'a', '0'));
	async_control(o, EMPTY_CLOCK, FCC('h', 'p', 'd', '0'));
	s->state = QT_STOPPING;
}

void qt_session_finish(struct qt_session *s, struct qt_out *o)
{
	async_control(o, EMPTY_CLOCK, FCC('h', 'p', 'd', '0'));
	s->state = QT_STOPPED;
}

size_t qt_session_missing(const struct qt_session *s)
{
	if (s->rlen == 0)
		return 0;
	if (s->rlen < 4)
		return 4 - s->rlen;
	uint32_t plen = rd32(s->rbuf);
	return plen > s->rlen ? plen - s->rlen : 0;
}

enum qt_state qt_session_state(const struct qt_session *s)
{
	return s->state;
}

uint64_t qt_session_video_frames(const struct qt_session *s)
{
	return s->video_frames;
}

uint64_t qt_session_audio_packets(const struct qt_session *s)
{
	return s->audio_packets;
}

int qt_session_releases(const struct qt_session *s)
{
	return s->releases;
}

unsigned qt_session_bad_samples(const struct qt_session *s)
{
	return s->bad_samples;
}

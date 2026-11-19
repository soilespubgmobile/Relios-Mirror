/*
 * Fuzzes the USB screen-capture protocol (viewer/qtproto.c): what a device (or something that pretends to be one)
 * sends is untrusted. Two kinds of input, many times over:
 *   - the captured messages of real devices (tests/fixtures/qvh), joined and damaged at random places;
 *   - messages made up from scratch with the right outline and random insides (lengths, counts, formats, the
 *     decoder configuration records, sound formats), so that the deeper parsers are reached too.
 * Every input is fed in pieces of random sizes; sometimes the release is started halfway. What comes out is checked:
 * pictures are Annex B with the padding after them, sound is 16-bit with 1-2 channels at a sane rate.
 *
 *   cc -std=c11 -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all -Iviewer \
 *      tests/qtproto_fuzz.c viewer/qtproto.c -o qtproto_fuzz
 *   ./qtproto_fuzz tests/fixtures/qvh <iterations> [seed]
 *
 * Exits with 0 when every input went through; a crash, a sanitizer report or a failed check stops it.
 */
#include "qtproto.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FCC(a, b, c, d) (((uint32_t)(a) << 24) | ((uint32_t)(b) << 16) | ((uint32_t)(c) << 8) | (uint32_t)(d))

static uint64_t rng = 0x9e3779b97f4a7c15ull;

static uint64_t next(void)
{
	rng ^= rng >> 12;
	rng ^= rng << 25;
	rng ^= rng >> 27;
	return rng * 0x2545f4914f6cdd1dull;
}

static uint32_t below(uint32_t n)
{
	return n ? (uint32_t)(next() % n) : 0;
}

static void fail(const char *what)
{
	fprintf(stderr, "FAIL: %s (seed state %llx)\n", what, (unsigned long long)rng);
	abort();
}

/* ------------------------------------------------------------------ a growing byte buffer */

struct buf {
	uint8_t *p;
	size_t n, cap;
};

static void reserve(struct buf *b, size_t more)
{
	if (b->n + more <= b->cap)
		return;
	size_t cap = b->cap ? b->cap : 256;
	while (cap < b->n + more)
		cap *= 2;
	b->p = realloc(b->p, cap);
	if (!b->p)
		fail("out of memory");
	b->cap = cap;
}

static void put(struct buf *b, const void *d, size_t n)
{
	reserve(b, n);
	if (n)
		memcpy(b->p + b->n, d, n);
	b->n += n;
}

static void put8(struct buf *b, uint32_t v)
{
	uint8_t c = (uint8_t)v;
	put(b, &c, 1);
}

static void put32(struct buf *b, uint32_t v)
{
	uint8_t c[4] = {(uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16), (uint8_t)(v >> 24)};
	put(b, c, 4);
}

static void put32be(struct buf *b, uint32_t v)
{
	uint8_t c[4] = {(uint8_t)(v >> 24), (uint8_t)(v >> 16), (uint8_t)(v >> 8), (uint8_t)v};
	put(b, c, 4);
}

static void put64(struct buf *b, uint64_t v)
{
	put32(b, (uint32_t)v);
	put32(b, (uint32_t)(v >> 32));
}

static void put_random(struct buf *b, size_t n)
{
	reserve(b, n);
	for (size_t i = 0; i < n; i++)
		b->p[b->n++] = (uint8_t)next();
}

static void set32(struct buf *b, size_t at, uint32_t v)
{
	b->p[at] = (uint8_t)v;
	b->p[at + 1] = (uint8_t)(v >> 8);
	b->p[at + 2] = (uint8_t)(v >> 16);
	b->p[at + 3] = (uint8_t)(v >> 24);
}

/* a chunk: length (counting itself), code, then whatever the caller adds; end() writes the length */
static size_t begin(struct buf *b, uint32_t code)
{
	size_t at = b->n;
	put32(b, 0);
	put32(b, code);
	return at;
}

static void end(struct buf *b, size_t at)
{
	uint32_t len = (uint32_t)(b->n - at);
	/* (now and then a wrong length: too short, too long, zero, huge) */
	switch (below(40)) {
	case 0:
		len = len > 0 ? len - 1 - below(len) : 0;
		break;
	case 1:
		len += 1 + below(64);
		break;
	case 2:
		len = 0;
		break;
	case 3:
		len = 0xffffffffu - below(16);
		break;
	default:
		break;
	}
	set32(b, at, len);
}

/* ------------------------------------------------------------------ the fixtures */

static const char *const fixture_names[] = {
	"afmt-request", "asyn-eat", "asyn-eat-nofdsc", "asyn-feed", "asyn-feed-nofdsc", "asyn-feed-ttas-only",
	"asyn-feed-unknown1", "asyn-hpa0", "asyn-hpa1", "asyn-hpd0", "asyn-hpd1", "asyn-need", "asyn-rels",
	"asyn-sprp", "asyn-sprp2", "asyn-srat", "asyn-tbas", "asyn-tjmp", "clok-request", "cvrp-request",
	"cwpa-request1", "cwpa-request2", "og-request", "skew-request", "stop-request", "time-request1",
};
#define FIXTURES (sizeof(fixture_names) / sizeof(fixture_names[0]))
static struct buf fixtures[FIXTURES];

static void load_fixtures(const char *folder)
{
	for (size_t i = 0; i < FIXTURES; i++) {
		char path[1024];
		snprintf(path, sizeof(path), "%s/%s", folder, fixture_names[i]);
		FILE *f = fopen(path, "rb");
		if (!f) {
			fprintf(stderr, "missing fixture %s\n", path);
			exit(2);
		}
		uint8_t tmp[1 << 16];
		struct buf raw = {0};
		size_t got;
		while ((got = fread(tmp, 1, sizeof(tmp), f)) > 0)
			put(&raw, tmp, got);
		fclose(f);
		/* (some are stored without their length) */
		uint32_t first = raw.n >= 4 ? (uint32_t)raw.p[0] | ((uint32_t)raw.p[1] << 8) |
						      ((uint32_t)raw.p[2] << 16) | ((uint32_t)raw.p[3] << 24) :
					      0;
		if (first != raw.n)
			put32(&fixtures[i], (uint32_t)raw.n + 4);
		put(&fixtures[i], raw.p, raw.n);
		free(raw.p);
	}
}

/* ------------------------------------------------------------------ made-up messages */

static const uint32_t interesting32[] = {0, 1, 2, 3, 4, 7, 8, 12, 16, 20, 23, 24, 28, 31, 32, 36, 40, 63, 64, 127,
					 128, 255, 256, 511, 512, 1023, 1024, 4096, 65535, 65536, 0x7fffffffu,
					 0x80000000u, 0xfffffffeu, 0xffffffffu, 32u * 1024u * 1024u,
					 32u * 1024u * 1024u + 1u};

static uint32_t some32(void)
{
	return below(3) ? interesting32[below(sizeof(interesting32) / sizeof(interesting32[0]))] : (uint32_t)next();
}

/* an avcC / hvcC record with random counts and lengths (often plausible, sometimes not) */
static void make_config(struct buf *b, bool h265)
{
	if (!h265) {
		put8(b, below(8) ? 1 : (uint32_t)next());
		put_random(b, 3);
		put8(b, 0xfc | below(4));
		uint32_t sps = below(4);
		put8(b, 0xe0 | sps);
		for (uint32_t i = 0; i < sps; i++) {
			uint32_t l = below(5) ? below(40) : some32() & 0xffff;
			put8(b, l >> 8);
			put8(b, l);
			put_random(b, below(6) ? l : below(l + 1));
		}
		uint32_t pps = below(4);
		put8(b, pps);
		for (uint32_t i = 0; i < pps; i++) {
			uint32_t l = below(5) ? below(20) : some32() & 0xffff;
			put8(b, l >> 8);
			put8(b, l);
			put_random(b, below(6) ? l : below(l + 1));
		}
	} else {
		put8(b, below(8) ? 1 : (uint32_t)next());
		put_random(b, 20);
		put8(b, 0xfc | below(4));
		uint32_t arrays = below(5);
		put8(b, arrays);
		for (uint32_t a = 0; a < arrays; a++) {
			put8(b, below(4) ? 32 + below(3) : (uint32_t)next());
			uint32_t count = below(4) ? below(3) : some32() & 0xffff;
			put8(b, count >> 8);
			put8(b, count);
			for (uint32_t i = 0; i < count && i < 8; i++) {
				uint32_t l = below(5) ? below(40) : some32() & 0xffff;
				put8(b, l >> 8);
				put8(b, l);
				put_random(b, below(6) ? l : below(l + 1));
			}
		}
	}
	if (!below(6))
		put_random(b, below(16));
}

/* a format description: media, picture size, codec, extensions (with the configuration), sound format */
static void make_fdsc(struct buf *b, bool video)
{
	size_t at = begin(b, FCC('f', 'd', 's', 'c'));
	size_t c = begin(b, FCC('m', 'd', 'i', 'a'));
	put32(b, video ? FCC('v', 'i', 'd', 'e') : below(8) ? FCC('s', 'o', 'u', 'n') : (uint32_t)next());
	end(b, c);
	if (video) {
		bool h265 = below(2);
		c = begin(b, FCC('c', 'o', 'd', 'c'));
		put32(b, below(8) ? (h265 ? FCC('h', 'v', 'c', '1') : FCC('a', 'v', 'c', '1')) : (uint32_t)next());
		end(b, c);
		c = begin(b, FCC('v', 'd', 'i', 'm'));
		put32(b, some32());
		put32(b, some32());
		end(b, c);
		c = begin(b, FCC('e', 'x', 't', 'n'));
		uint32_t n = below(4);
		for (uint32_t i = 0; i < n; i++) {
			if (below(3))
				put_random(b, below(24));
			size_t d = begin(b, FCC('d', 'a', 't', 'v'));
			make_config(b, h265);
			end(b, d);
		}
		end(b, c);
	} else {
		c = begin(b, FCC('a', 's', 'b', 'd'));
		/* rate (f64): plausible or odd (0, negative, NaN, infinity, huge) */
		static const double rates[] = {48000.0, 44100.0, 24000.0, 16000.0, 8000.0, 192000.0, 0.0, -1.0,
					       1e300, 7999.0, 192001.0};
		double rate = below(3) ? rates[below(sizeof(rates) / sizeof(rates[0]))] : (double)next();
		if (!below(20)) {
			uint64_t nan = 0x7ff8000000000001ull;
			memcpy(&rate, &nan, 8);
		}
		uint64_t bits;
		memcpy(&bits, &rate, 8);
		put64(b, bits);
		put32(b, below(6) ? FCC('l', 'p', 'c', 'm') : (uint32_t)next()); /* format */
		put32(b, below(2) ? (uint32_t)(below(64)) : some32());           /* flags */
		put32(b, some32() & (below(2) ? 0xff : 0xffffffffu));             /* bytes per packet */
		put32(b, below(2) ? 1 : some32());                                /* frames per packet */
		put32(b, below(2) ? below(33) : some32());                        /* bytes per frame */
		put32(b, below(2) ? below(10) : some32());                        /* channels */
		put32(b, below(2) ? 8 * below(5) : some32());                     /* bits */
		put32(b, 0);
		end(b, c);
	}
	if (!below(5))
		put_random(b, below(32));
	end(b, at);
}

/* sample data: NAL units with lengths of 1-4 bytes (pictures), or raw bytes (sound) */
static void make_sdat(struct buf *b, bool video)
{
	size_t at = begin(b, FCC('s', 'd', 'a', 't'));
	if (video) {
		uint32_t nals = below(5);
		for (uint32_t i = 0; i < nals; i++) {
			uint32_t l = below(6) ? 1 + below(300) : some32();
			put32be(b, l);
			put_random(b, l < 2000 ? (below(8) ? l : below(l + 1)) : below(64));
		}
		if (!below(8))
			put_random(b, 1 + below(3)); /* (less than a NAL length at the end) */
	} else {
		put_random(b, below(4) ? below(4096) : below(64));
	}
	end(b, at);
}

/* a FEED / EAT!: the message, the "sbuf" with its chunks in random order and number */
static void make_media(struct buf *b, bool video)
{
	size_t at = begin(b, FCC('a', 's', 'y', 'n'));
	put64(b, below(2) ? 0x1122334455667788ull : next());
	put32(b, video ? FCC('f', 'e', 'e', 'd') : FCC('e', 'a', 't', '!'));
	size_t sb = begin(b, FCC('s', 'b', 'u', 'f'));
	uint32_t chunks = 1 + below(5);
	for (uint32_t i = 0; i < chunks; i++) {
		switch (below(6)) {
		case 0:
			make_fdsc(b, video);
			break;
		case 1:
		case 2:
			make_sdat(b, video);
			break;
		case 3: {
			size_t c = begin(b, FCC('o', 'p', 't', 's'));
			put64(b, next());
			put32(b, some32());
			put32(b, 1);
			put64(b, 0);
			end(b, c);
			break;
		}
		case 4: {
			size_t c = begin(b, (uint32_t)next());
			put_random(b, below(64));
			end(b, c);
			break;
		}
		default:
			put_random(b, below(16));
			break;
		}
	}
	end(b, sb);
	end(b, at);
}

static void make_sync(struct buf *b)
{
	static const uint32_t subtypes[] = {FCC('c', 'w', 'p', 'a'), FCC('a', 'f', 'm', 't'), FCC('c', 'v', 'r', 'p'),
					    FCC('c', 'l', 'o', 'k'), FCC('t', 'i', 'm', 'e'), FCC('s', 'k', 'e', 'w'),
					    FCC('g', 'o', '!', ' '), FCC('s', 't', 'o', 'p')};
	size_t at = begin(b, FCC('s', 'y', 'n', 'c'));
	put64(b, below(2) ? 0x1122334455667788ull : next());
	put32(b, below(10) ? subtypes[below(8)] : (uint32_t)next());
	put64(b, next()); /* correlation */
	if (below(3))
		put64(b, next());
	if (!below(3)) {
		/* (an AFMT's sound format, as in the captured one, or random) */
		put_random(b, below(2) ? 40 : below(80));
	}
	end(b, at);
}

static void make_message(struct buf *b)
{
	switch (below(13)) {
	case 12: { /* a reply from the device (it should not send one) */
		size_t at = begin(b, FCC('r', 'p', 'l', 'y'));
		put64(b, next());
		put_random(b, below(24));
		end(b, at);
		break;
	}
	case 0: {
		size_t at = begin(b, FCC('p', 'i', 'n', 'g'));
		put32(b, 0);
		put32(b, 1);
		end(b, at);
		break;
	}
	case 1:
	case 2:
	case 3:
		make_sync(b);
		break;
	case 4:
	case 5:
	case 6:
		make_media(b, true);
		break;
	case 7:
	case 8:
		make_media(b, false);
		break;
	case 9: {
		static const uint32_t subtypes[] = {FCC('r', 'e', 'l', 's'), FCC('s', 'p', 'r', 'p'),
						    FCC('s', 'r', 'a', 't'), FCC('t', 'j', 'm', 'p'),
						    FCC('t', 'b', 'a', 's'), FCC('n', 'e', 'e', 'd')};
		size_t at = begin(b, FCC('a', 's', 'y', 'n'));
		put64(b, next());
		put32(b, subtypes[below(6)]);
		put_random(b, below(80));
		end(b, at);
		break;
	}
	case 10: {
		const struct buf *f = &fixtures[below(FIXTURES)];
		put(b, f->p, f->n);
		break;
	}
	default:
		put_random(b, below(40));
		break;
	}
}

/* ------------------------------------------------------------------ damage */

static void mutate(struct buf *b)
{
	if (!b->n)
		return;
	switch (below(8)) {
	case 0: /* a bit */
		b->p[below((uint32_t)b->n)] ^= (uint8_t)(1u << below(8));
		break;
	case 1: /* a byte */
		b->p[below((uint32_t)b->n)] = (uint8_t)(below(2) ? next() : (below(2) ? 0 : 0xff));
		break;
	case 2: /* a 32-bit field */
		if (b->n >= 4)
			set32(b, below((uint32_t)b->n - 3), some32());
		break;
	case 3: { /* cut the end */
		b->n = below((uint32_t)b->n + 1);
		break;
	}
	case 4: { /* remove a range */
		size_t from = below((uint32_t)b->n), len = below((uint32_t)(b->n - from) + 1);
		memmove(b->p + from, b->p + from + len, b->n - from - len);
		b->n -= len;
		break;
	}
	case 5: { /* repeat a range */
		size_t from = below((uint32_t)b->n), len = below((uint32_t)(b->n - from) % 4096 + 1);
		uint8_t *copy = malloc(len ? len : 1);
		if (!copy)
			fail("out of memory");
		memcpy(copy, b->p + from, len);
		put(b, copy, len);
		free(copy);
		break;
	}
	case 6: /* random bytes in between */
		if (b->n < (1u << 20)) {
			size_t at = below((uint32_t)b->n + 1), len = 1 + below(16);
			reserve(b, len);
			memmove(b->p + at + len, b->p + at, b->n - at);
			for (size_t i = 0; i < len; i++)
				b->p[at + i] = (uint8_t)next();
			b->n += len;
		}
		break;
	default: /* swap two bytes */
		{
			size_t i = below((uint32_t)b->n), j = below((uint32_t)b->n);
			uint8_t t = b->p[i];
			b->p[i] = b->p[j];
			b->p[j] = t;
		}
		break;
	}
}

/* ------------------------------------------------------------------ what comes out */

static unsigned long long pictures, sounds;
static volatile uint8_t sink_hole;

static void on_video(void *ctx, const uint8_t *d, size_t n, bool h265, int w, int h)
{
	(void)ctx;
	(void)h265;
	(void)w;
	(void)h;
	if (n < 5 || d[0] != 0 || d[1] != 0 || d[2] != 0 || d[3] != 1)
		fail("a picture that is not Annex B");
	uint8_t x = 0;
	for (size_t i = 0; i < n; i++)
		x ^= d[i];
	/* (the decoder may read 64 bytes past the end: they are there and zero) */
	for (size_t i = 0; i < 64; i++)
		if (d[n + i] != 0)
			fail("no zero padding after a picture");
	sink_hole ^= x;
	pictures++;
}

static void on_audio(void *ctx, const uint8_t *pcm, int frames, int channels, int rate)
{
	(void)ctx;
	if (frames <= 0 || frames > 1000000 || channels < 1 || channels > 2 || rate < 8000 || rate > 192000)
		fail("sound with odd parameters");
	uint8_t x = 0;
	for (size_t i = 0; i < (size_t)frames * (size_t)channels * 2; i++)
		x ^= pcm[i];
	sink_hole ^= x;
	sounds++;
}

static void on_log(void *ctx, const char *text)
{
	(void)ctx;
	if (!text || strlen(text) > 4000)
		fail("an odd log line");
}

/* ------------------------------------------------------------------ one input */

static void run(const struct buf *in)
{
	struct qt_sink sink = {on_video, on_audio, on_log, NULL};
	struct qt_options opt = {below(4) ? 1640 : (int)some32(), below(4) ? 2360 : (int)some32(), below(2) != 0,
				 below(4) != 0};
	struct qt_session *s = qt_session_new(&sink, &opt);
	if (!s)
		fail("no session");
	struct qt_out out = {0};
	uint64_t t = 1000 + below(1000);
	bool stopped = false;
	for (size_t pos = 0; pos < in->n;) {
		size_t piece = below(4) ? 1 + below(below(2) ? 600 : 70000) : in->n - pos;
		if (piece > in->n - pos)
			piece = in->n - pos;
		qt_out_clear(&out);
		bool ok = qt_session_feed(s, in->p + pos, piece, &out, t);
		pos += piece;
		t += below(3) ? below(40000000) : (uint64_t)next() % 10000000000ull;
		if (out.n > (64u << 20))
			fail("too many answer bytes");
		qt_session_trace_out(s, &out);
		if (qt_session_missing(s) > 32u * 1024u * 1024u)
			fail("missing more than a message can be");
		(void)qt_session_state(s);
		(void)qt_session_video_stopped_ns(s);
		unsigned stops[4];
		qt_session_stops(s, stops);
		if (!ok)
			break;
		if (!stopped && !below(200)) {
			qt_out_clear(&out);
			qt_session_stop(s, &out);
			stopped = true;
		}
	}
	qt_out_clear(&out);
	qt_session_finish(s, &out);
	(void)qt_session_video_frames(s);
	(void)qt_session_audio_packets(s);
	(void)qt_session_releases(s);
	(void)qt_session_bad_samples(s);
	qt_out_free(&out);
	qt_session_free(s);
}

int main(int argc, char **argv)
{
	if (argc < 3) {
		fprintf(stderr, "usage: %s <fixtures folder> <iterations> [seed]\n", argv[0]);
		return 2;
	}
	load_fixtures(argv[1]);
	unsigned long long iterations = strtoull(argv[2], NULL, 10);
	if (argc > 3)
		rng ^= strtoull(argv[3], NULL, 0) * 0x9e3779b97f4a7c15ull + 1;
	struct buf in = {0};
	for (unsigned long long it = 0; it < iterations; it++) {
		in.n = 0;
		if (below(2)) {
			/* captured messages, joined and damaged */
			uint32_t count = 1 + below(12);
			for (uint32_t i = 0; i < count; i++) {
				const struct buf *f = &fixtures[below(FIXTURES)];
				put(&in, f->p, f->n);
			}
			uint32_t damage = below(4) ? 1 + below(4) : below(40);
			for (uint32_t i = 0; i < damage; i++)
				mutate(&in);
		} else {
			/* made up: a start as a device makes it, then anything */
			if (below(2)) {
				put(&in, fixtures[20].p, fixtures[20].n); /* cwpa */
				put(&in, fixtures[0].p, fixtures[0].n);   /* afmt */
				put(&in, fixtures[19].p, fixtures[19].n); /* cvrp */
			}
			/* (sometimes long: past the first 40 messages, which the session logs differently) */
			uint32_t count = below(8) ? 1 + below(16) : 40 + below(80);
			for (uint32_t i = 0; i < count; i++)
				make_message(&in);
			if (!below(4))
				mutate(&in);
		}
		run(&in);
		if ((it + 1) % 100000 == 0)
			fprintf(stderr, "%llu inputs, %llu pictures and %llu sound packets came out\n", it + 1, pictures,
				sounds);
	}
	free(in.p);
	for (size_t i = 0; i < FIXTURES; i++)
		free(fixtures[i].p);
	printf("fuzz: %llu inputs, %llu pictures, %llu sound packets: no fault\n", iterations, pictures, sounds);
	return 0;
}

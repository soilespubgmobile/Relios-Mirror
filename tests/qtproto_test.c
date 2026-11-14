/*
 * Tests the USB screen-capture protocol (viewer/qtproto.c) with messages captured from real devices: the fixtures of
 * quicktime_video_hack (MIT, https://github.com/danielpaulus/quicktime_video_hack, screencapture/packet/fixtures).
 *
 *   cc -std=c11 -Wall -Wextra -Iviewer tests/qtproto_test.c viewer/qtproto.c -o qtproto_test
 *   ./qtproto_test <quicktime_video_hack>/screencapture/packet/fixtures
 *
 * Prints what it checks; the last line says how many checks failed (0 failure(s) is a pass).
 */
#include "qtproto.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures, checks;
static const char *folder;

#define CHECK(cond, ...)                                                                                             \
	do {                                                                                                         \
		checks++;                                                                                            \
		if (!(cond)) {                                                                                       \
			failures++;                                                                                  \
			printf("FAIL %s:%d: ", __FILE__, __LINE__);                                                  \
			printf(__VA_ARGS__);                                                                         \
			printf("\n");                                                                                \
		}                                                                                                    \
	} while (0)

static uint32_t rd32(const uint8_t *p)
{
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint64_t rd64(const uint8_t *p)
{
	return (uint64_t)rd32(p) | ((uint64_t)rd32(p + 4) << 32);
}

/* a fixture as a whole message (some are stored without their length) */
static uint8_t *load(const char *name, size_t *len)
{
	char path[1024];
	snprintf(path, sizeof(path), "%s/%s", folder, name);
	FILE *f = fopen(path, "rb");
	if (!f) {
		printf("FAIL: missing fixture %s\n", path);
		failures++;
		exit(1);
	}
	fseek(f, 0, SEEK_END);
	long size = ftell(f);
	fseek(f, 0, SEEK_SET);
	uint8_t *d = malloc((size_t)size + 4);
	if (!d || fread(d + 4, 1, (size_t)size, f) != (size_t)size) {
		fclose(f);
		exit(1);
	}
	fclose(f);
	if (size >= 4 && rd32(d + 4) == (uint32_t)size) {
		memmove(d, d + 4, (size_t)size);
		*len = (size_t)size;
	} else {
		uint32_t n = (uint32_t)size + 4;
		d[0] = (uint8_t)n;
		d[1] = (uint8_t)(n >> 8);
		d[2] = (uint8_t)(n >> 16);
		d[3] = (uint8_t)(n >> 24);
		*len = (size_t)size + 4;
	}
	return d;
}

struct seen {
	int videos, audios;
	size_t video_bytes;
	int width, height;
	bool h265, first_has_sps, nal_ok;
	int audio_frames, audio_channels, audio_rate;
	int logs;
};

static void on_video(void *ctx, const uint8_t *d, size_t n, bool h265, int w, int h)
{
	struct seen *s = ctx;
	if (s->videos == 0) {
		/* the first one starts with the parameter sets */
		int type = n > 4 ? (h265 ? (d[4] >> 1) & 0x3f : d[4] & 0x1f) : -1;
		s->first_has_sps = h265 ? type == 32 || type == 33 : type == 7;
	}
	/* every NAL starts with 00 00 00 01 and the next start code is where the NAL ends */
	bool ok = n >= 5 && d[0] == 0 && d[1] == 0 && d[2] == 0 && d[3] == 1;
	s->nal_ok = s->videos == 0 ? ok : s->nal_ok && ok;
	s->videos++;
	s->video_bytes += n;
	s->width = w;
	s->height = h;
	s->h265 = h265;
}

static void on_audio(void *ctx, const uint8_t *pcm, int frames, int channels, int rate)
{
	struct seen *s = ctx;
	(void)pcm;
	s->audios++;
	s->audio_frames += frames;
	s->audio_channels = channels;
	s->audio_rate = rate;
}

static void on_log(void *ctx, const char *text)
{
	struct seen *s = ctx;
	s->logs++;
	printf("  log: %s\n", text);
}

static struct qt_session *session(struct seen *seen)
{
	struct qt_sink sink = {on_video, on_audio, on_log, seen};
	struct qt_options opt = {1640, 2360, true, true};
	return qt_session_new(&sink, &opt);
}

/* the answers split into messages: count, and the n-th one's place */
static int messages(const struct qt_out *o, size_t at[], size_t len[], int max)
{
	int count = 0;
	for (size_t off = 0; off + 8 <= o->n && count < max;) {
		uint32_t l = rd32(o->p + off);
		if (l < 8 || l > o->n - off)
			return -1;
		at[count] = off;
		len[count] = l;
		count++;
		off += l;
	}
	return count;
}

static uint32_t code_of(const uint8_t *msg, size_t off)
{
	return rd32(msg + off);
}

#define FCC(a, b, c, d) (((uint32_t)(a) << 24) | ((uint32_t)(b) << 16) | ((uint32_t)(c) << 8) | (uint32_t)(d))

static void answer_equals(const char *request, const char *expected)
{
	struct seen seen = {0};
	struct qt_session *s = session(&seen);
	struct qt_out o = {0};
	size_t rl, el;
	uint8_t *r = load(request, &rl), *e = load(expected, &el);
	CHECK(qt_session_feed(s, r, rl, &o, 1000), "%s: feed failed", request);
	CHECK(o.n == el && memcmp(o.p, e, el) == 0, "%s: answer differs from %s (%zu vs %zu bytes)", request, expected,
	      o.n, el);
	printf("ok? %s -> %s (%zu bytes)\n", request, expected, o.n);
	free(r);
	free(e);
	qt_out_free(&o);
	qt_session_free(s);
}

int main(int argc, char **argv)
{
	if (argc != 2) {
		fprintf(stderr, "usage: %s <fixtures folder>\n", argv[0]);
		return 2;
	}
	folder = argv[1];

	/* answers that must match the captured ones byte for byte */
	answer_equals("afmt-request", "afmt-reply");
	answer_equals("skew-request", "skew-reply");
	answer_equals("stop-request", "stop-reply");
	answer_equals("og-request", "og-reply");

	/* the audio clock: the display (HPD1), the clock answer, the sound request (HPA1) */
	{
		struct seen seen = {0};
		struct qt_session *s = session(&seen);
		struct qt_out o = {0};
		size_t rl;
		uint8_t *r = load("cwpa-request1", &rl);
		CHECK(qt_session_feed(s, r, rl, &o, 1000), "cwpa feed");
		size_t at[8], len[8];
		int n = messages(&o, at, len, 8);
		CHECK(n == 3, "cwpa: %d answers (3 expected)", n);
		if (n == 3) {
			CHECK(code_of(o.p, at[0] + 4) == FCC('a', 's', 'y', 'n') &&
				      code_of(o.p, at[0] + 16) == FCC('h', 'p', 'd', '1'),
			      "cwpa: the first answer is not HPD1");
			CHECK(len[1] == 28 && code_of(o.p, at[1] + 4) == FCC('r', 'p', 'l', 'y'), "cwpa: no clock reply");
			CHECK(rd64(o.p + at[1] + 8) == rd64(r + 20), "cwpa: the correlation differs");
			CHECK(rd64(o.p + at[1] + 20) == rd64(r + 28) + 0x1000, "cwpa: the clock is not the device's + 0x1000");
			CHECK(code_of(o.p, at[2] + 16) == FCC('h', 'p', 'a', '1'), "cwpa: the third answer is not HPA1");
			CHECK(rd64(o.p + at[2] + 8) == rd64(r + 28), "cwpa: HPA1 does not carry the device's audio clock");
			/* the sound request is the same as the captured one, apart from its clock */
			size_t hl;
			uint8_t *h = load("asyn-hpa1", &hl);
			CHECK(len[2] == hl && memcmp(o.p + at[2] + 16, h + 16, hl - 16) == 0,
			      "HPA1 differs from the captured one (%zu vs %zu bytes)", len[2], hl);
			free(h);
			/* the display request has the captured one's shape (its size values are ours) */
			uint8_t *d = load("asyn-hpd1", &hl);
			printf("HPD1: ours %zu bytes, captured %zu bytes\n", len[0], hl);
			free(d);
		}
		CHECK(qt_session_state(s) == QT_NEGOTIATING, "cwpa: state %d", (int)qt_session_state(s));
		free(r);
		qt_out_free(&o);
		qt_session_free(s);
	}

	/* the video clock: NEED and the clock answer */
	{
		struct seen seen = {0};
		struct qt_session *s = session(&seen);
		struct qt_out o = {0};
		size_t rl;
		uint8_t *r = load("cvrp-request", &rl);
		CHECK(qt_session_feed(s, r, rl, &o, 1000), "cvrp feed");
		size_t at[8], len[8];
		int n = messages(&o, at, len, 8);
		CHECK(n == 2, "cvrp: %d answers (2 expected)", n);
		if (n == 2) {
			size_t nl;
			uint8_t *need = load("asyn-need", &nl);
			CHECK(len[0] == 20 && code_of(o.p, at[0] + 16) == FCC('n', 'e', 'e', 'd'), "cvrp: no NEED");
			CHECK(rd64(o.p + at[0] + 8) == rd64(r + 28), "cvrp: NEED does not carry the device's video clock");
			CHECK(len[1] == 28 && rd64(o.p + at[1] + 8) == rd64(r + 20), "cvrp: the clock reply is wrong");
			free(need);
		}
		free(r);
		qt_out_free(&o);
		qt_session_free(s);
	}

	/* time and clock */
	{
		struct seen seen = {0};
		struct qt_session *s = session(&seen);
		struct qt_out o = {0};
		size_t rl;
		uint8_t *r = load("time-request1", &rl);
		qt_session_feed(s, r, rl, &o, 5000);
		CHECK(o.n == 44 && rd64(o.p + 8) == rd64(r + 20) && rd32(o.p + 28) == 1000000000u && rd32(o.p + 32) == 1,
		      "time: the answer is not a CMTime in nanoseconds");
		free(r);
		qt_out_clear(&o);
		r = load("clok-request", &rl);
		qt_session_feed(s, r, rl, &o, 6000);
		CHECK(o.n == 28 && rd64(o.p + 8) == rd64(r + 20) && rd64(o.p + 20) == rd64(r + 8) + 0x10000,
		      "clok: the answer is wrong");
		free(r);
		qt_out_free(&o);
		qt_session_free(s);
	}

	/* a picture with its format, then pictures without it; then sound */
	{
		struct seen seen = {0};
		struct qt_session *s = session(&seen);
		struct qt_out o = {0};
		size_t rl;
		uint8_t *r = load("cvrp-request", &rl);
		qt_session_feed(s, r, rl, &o, 1000); /* (so that NEED can follow the pictures) */
		free(r);
		qt_out_clear(&o);
		r = load("asyn-feed", &rl);
		CHECK(qt_session_feed(s, r, rl, &o, 2000), "feed: failed");
		CHECK(seen.videos == 1, "feed: %d pictures", seen.videos);
		CHECK(seen.first_has_sps, "feed: the first picture does not start with the parameter sets");
		CHECK(seen.nal_ok, "feed: not Annex B");
		CHECK(seen.width > 0 && seen.height > 0, "feed: no size");
		CHECK(o.n == 20 && rd32(o.p + 16) == FCC('n', 'e', 'e', 'd'), "feed: no NEED after the picture");
		printf("picture: %dx%d %s, %zu bytes\n", seen.width, seen.height, seen.h265 ? "H.265" : "H.264",
		       seen.video_bytes);
		free(r);
		r = load("asyn-feed-nofdsc", &rl);
		CHECK(qt_session_feed(s, r, rl, &o, 3000), "feed without format: failed");
		CHECK(seen.videos == 2, "feed without format: %d pictures", seen.videos);
		free(r);
		r = load("asyn-eat", &rl);
		CHECK(qt_session_feed(s, r, rl, &o, 4000), "eat: failed");
		CHECK(seen.audios == 1 && seen.audio_frames > 0, "eat: no sound");
		printf("sound: %d frames, %d channels, %d Hz\n", seen.audio_frames, seen.audio_channels, seen.audio_rate);
		free(r);
		r = load("asyn-eat-nofdsc", &rl);
		qt_session_feed(s, r, rl, &o, 4500);
		CHECK(seen.audios == 2, "eat without format: %d", seen.audios);
		free(r);
		CHECK(qt_session_bad_samples(s) == 0, "%u broken samples", qt_session_bad_samples(s));
		CHECK(qt_session_state(s) == QT_STREAMING, "state %d after pictures", (int)qt_session_state(s));
		qt_out_free(&o);
		qt_session_free(s);
	}

	/* the same stream cut into pieces at every possible size: the same result */
	{
		const char *names[] = {"cwpa-request1", "afmt-request", "cvrp-request", "asyn-feed", "asyn-eat",
				       "time-request1", "asyn-feed-nofdsc", "asyn-rels", "asyn-sprp", "asyn-tjmp",
				       "asyn-srat", "asyn-tbas", "stop-request"};
		size_t total = 0, lens[13];
		uint8_t *parts[13];
		for (int i = 0; i < 13; i++) {
			parts[i] = load(names[i], &lens[i]);
			total += lens[i];
		}
		uint8_t *all = malloc(total);
		size_t off = 0;
		for (int i = 0; i < 13; i++) {
			memcpy(all + off, parts[i], lens[i]);
			off += lens[i];
			free(parts[i]);
		}
		struct seen whole = {0};
		struct qt_session *s = session(&whole);
		struct qt_out o1 = {0};
		CHECK(qt_session_feed(s, all, total, &o1, 1000), "whole stream: failed");
		CHECK(qt_session_releases(s) == 1, "whole stream: %d releases", qt_session_releases(s));
		CHECK(qt_session_state(s) == QT_STOPPED, "whole stream: state %d", (int)qt_session_state(s));
		qt_session_free(s);
		static const size_t sizes[] = {1, 2, 3, 7, 13, 512, 4096, 16384, 65536, 100000};
		for (size_t k = 0; k < sizeof(sizes) / sizeof(sizes[0]); k++) {
			struct seen cut = {0};
			struct qt_session *c = session(&cut);
			struct qt_out o2 = {0};
			bool ok = true;
			for (size_t p = 0; p < total; p += sizes[k]) {
				size_t n = total - p < sizes[k] ? total - p : sizes[k];
				ok = ok && qt_session_feed(c, all + p, n, &o2, 1000);
			}
			CHECK(ok && cut.videos == whole.videos && cut.audios == whole.audios &&
				      cut.video_bytes == whole.video_bytes && o2.n == o1.n,
			      "cut into %zu-byte pieces: different (%d/%d pictures, %zu/%zu answer bytes)", sizes[k],
			      cut.videos, whole.videos, o2.n, o1.n);
			/* (the time answer differs only in its value: same length) */
			qt_out_free(&o2);
			qt_session_free(c);
		}
		/* read the way the USB reader reads (viewer/usbqt.c, read_wanted): one packet when a message begins, then
		 * what it still lacks in whole packets. Every message is one transfer of the device, without an empty packet
		 * after it; a read gets at most what it asked for and ends at the end of the transfer. No read may ask past
		 * the message being read (else it would wait for bytes the device sends only after an answer) */
		static const size_t packets[] = {512, 1024};
		for (size_t k = 0; k < 2; k++) {
			size_t pk = packets[k];
			struct seen paced = {0};
			struct qt_session *c = session(&paced);
			struct qt_out o2 = {0};
			bool ok = true, overreach = false;
			size_t msg = 0, reads = 0;
			for (int i = 0; i < 13 && ok; i++) {
				size_t mlen = rd32(all + msg), got = 0;
				while (got < mlen && ok) {
					size_t missing = qt_session_missing(c);
					size_t want = missing ? (missing + pk - 1) / pk * pk : pk;
					if (want > 65536)
						want = 65536 - 65536 % pk;
					size_t n = mlen - got < want ? mlen - got : want;
					/* a request that is neither full nor ended by a short packet would wait */
					if (n < want && n % pk == 0)
						overreach = true;
					ok = qt_session_feed(c, all + msg + got, n, &o2, 1000);
					got += n;
					reads++;
				}
				msg += mlen;
			}
			CHECK(ok && !overreach && qt_session_missing(c) == 0 && paced.videos == whole.videos &&
				      paced.audios == whole.audios && paced.video_bytes == whole.video_bytes && o2.n == o1.n,
			      "read in %zu-byte packets: different (%d/%d pictures, %zu/%zu answer bytes%s)", pk, paced.videos,
			      whole.videos, o2.n, o1.n, overreach ? ", a read waited past a message" : "");
			printf("read in %zu-byte packets: %zu reads\n", pk, reads);
			qt_out_free(&o2);
			qt_session_free(c);
		}
		printf("whole stream: %d pictures, %d sounds, %zu answer bytes\n", whole.videos, whole.audios, o1.n);
		qt_out_free(&o1);
		free(all);
	}

	/* broken input: a length that cannot be is refused; a short one too */
	{
		struct seen seen = {0};
		struct qt_session *s = session(&seen);
		struct qt_out o = {0};
		uint8_t bad[8] = {4, 0, 0, 0, 'g', 'n', 'i', 'p'};
		CHECK(!qt_session_feed(s, bad, sizeof(bad), &o, 1), "a length below 8 was accepted");
		CHECK(seen.logs > 0, "a broken length was not written to the log");
		qt_session_free(s);
		s = session(&seen);
		uint8_t huge[8] = {0xff, 0xff, 0xff, 0x7f, 'n', 'y', 's', 'a'};
		CHECK(!qt_session_feed(s, huge, sizeof(huge), &o, 1), "a huge length was accepted");
		qt_session_free(s);
		/* a FEED whose sample length points outside it: skipped, counted, no crash */
		s = session(&seen);
		size_t rl;
		uint8_t *r = load("asyn-feed", &rl);
		r[4 + 16] = 0xff;
		r[4 + 17] = 0xff;
		r[4 + 18] = 0xff;
		CHECK(qt_session_feed(s, r, rl, &o, 1), "broken sample: the stream was given up");
		CHECK(qt_session_bad_samples(s) == 1, "broken sample: not counted");
		free(r);
		qt_out_free(&o);
		qt_session_free(s);
	}

	printf("%d check(s), %d failure(s)\n", checks, failures);
	return failures ? 1 : 0;
}

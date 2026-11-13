/*
 * Relios Mirror: the protocol of Apple's screen capture over USB (the one QuickTime uses on a Mac), without the USB
 * part: messages from the device in, answers to it and the media out. Portable C, no operating system calls, so that
 * it can be tested with captured messages on any computer (tests/qtproto_test.c).
 *
 * What it follows: quicktime_video_hack by Daniel Paulus (MIT, https://github.com/danielpaulus/quicktime_video_hack)
 * and iPhoneMirror by RayrenSX (GPL-3.0-only, https://github.com/RayrenSX/iPhoneMirror): their descriptions of the
 * message layouts and the order of the answers.
 */
#ifndef RELIOS_QTPROTO_H
#define RELIOS_QTPROTO_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* answers to the device, appended in the order they are to be sent */
struct qt_out {
	uint8_t *p;
	size_t n, cap;
	bool failed; /* (out of memory) */
};

void qt_out_clear(struct qt_out *o);
void qt_out_free(struct qt_out *o);

struct qt_sink {
	/* a picture: H.264 / H.265 in Annex B (start codes), parameter sets in front of the first one and of key frames */
	void (*video)(void *ctx, const uint8_t *annexb, size_t len, bool h265, int width, int height);
	/* sound: 16-bit signed little-endian, interleaved */
	void (*audio)(void *ctx, const uint8_t *pcm, int frames, int channels, int rate);
	void (*log)(void *ctx, const char *text);
	void *ctx;
};

enum qt_state {
	QT_WAIT_PING,   /* nothing from the device yet */
	QT_WAIT_CLOCK,  /* PING answered: the device sends its audio clock next */
	QT_NEGOTIATING, /* the display and sound were requested */
	QT_STREAMING,   /* pictures arrive */
	QT_STOPPING,    /* the release was requested */
	QT_STOPPED,     /* the device said stop */
};

struct qt_options {
	int width, height; /* the size of the screen offered to the device (portrait) */
	bool valeria;      /* Apple's capture mode with a demo status bar (9:41, full battery): the most reliable start */
	bool audio;        /* ask for the sound too */
};

struct qt_session *qt_session_new(const struct qt_sink *sink, const struct qt_options *options);
void qt_session_free(struct qt_session *s);

/* bytes read from the device (any boundaries); answers are appended to out. now_ns: a steady clock.
 * false: the stream cannot be followed any more (a broken length) */
bool qt_session_feed(struct qt_session *s, const uint8_t *data, size_t len, struct qt_out *out, uint64_t now_ns);

void qt_session_stop(struct qt_session *s, struct qt_out *out);   /* asks for the release: HPA0, HPD0 */
void qt_session_finish(struct qt_session *s, struct qt_out *out); /* after both releases: the last HPD0 */
void qt_make_ping(struct qt_out *out);

enum qt_state qt_session_state(const struct qt_session *s);
uint64_t qt_session_video_frames(const struct qt_session *s);
uint64_t qt_session_audio_packets(const struct qt_session *s);
int qt_session_releases(const struct qt_session *s);
unsigned qt_session_bad_samples(const struct qt_session *s);

#endif

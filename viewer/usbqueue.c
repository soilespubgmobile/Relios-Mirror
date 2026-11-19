/*
 * Relios Mirror: the cable's pictures, decoded on a thread of their own (see usbqueue.h).
 */
#include "usbqueue.h"

#include <SDL.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct item {
	uint8_t *data; /* the picture, followed by USBQ_PADDING zero bytes */
	size_t len;
	bool h265;
};

static struct {
	usbq_decode_fn decode;
	usbq_log_fn log;
	SDL_mutex *mutex;
	SDL_cond *cond; /* a picture to decode, room in the queue, or the thread idle again */
	SDL_Thread *thread;
	struct item items[USBQ_ITEMS];
	int head, count;
	size_t bytes;
	bool busy, quit, want_key;
	/* for the log, per session: how the pictures came, how long decoding took */
	uint64_t last_ms, decode_ns, decode_max_ns, window_ms;
	unsigned arrived, decoded, dropped, deepest, gaps50, gaps100, window_n, fps_lines;
	int shown_fps;
} Q = {.shown_fps = -1};

static uint64_t ns_now(void)
{
	Uint64 c = SDL_GetPerformanceCounter(), f = SDL_GetPerformanceFrequency();
	return f ? (uint64_t)(c / f) * 1000000000ull + (uint64_t)(c % f) * 1000000000ull / (uint64_t)f : 0;
}

static void say(const char *fmt, ...)
{
	if (!Q.log)
		return;
	char text[400];
	va_list args;
	va_start(args, fmt);
	vsnprintf(text, sizeof(text), fmt, args);
	va_end(args);
	Q.log(text);
}

bool usbq_key_frame(const uint8_t *d, size_t n, bool h265)
{
	for (size_t i = 0; i + 3 < n; i++) {
		if (d[i] != 0 || d[i + 1] != 0 || d[i + 2] != 1)
			continue;
		int type = h265 ? (d[i + 3] >> 1) & 0x3f : d[i + 3] & 0x1f;
		if (h265 ? type >= 16 && type <= 21 : type == 5)
			return true;
		i += 2;
	}
	return false;
}

/* (under Q.mutex) what waits is dropped; how many */
static unsigned drop_locked(void)
{
	unsigned n = (unsigned)Q.count;
	while (Q.count > 0) {
		free(Q.items[Q.head].data);
		Q.items[Q.head].data = NULL;
		Q.head = (Q.head + 1) % USBQ_ITEMS;
		Q.count--;
	}
	Q.bytes = 0;
	Q.dropped += n;
	return n;
}

/* (under Q.mutex) a session's figures start from nothing */
static void reset_locked(void)
{
	Q.last_ms = Q.decode_ns = Q.decode_max_ns = Q.window_ms = 0;
	Q.arrived = Q.decoded = Q.dropped = Q.deepest = Q.gaps50 = Q.gaps100 = Q.window_n = Q.fps_lines = 0;
	Q.shown_fps = -1;
	Q.want_key = false;
}

static int thread_main(void *unused)
{
	(void)unused;
	SDL_LockMutex(Q.mutex);
	for (;;) {
		while (!Q.quit && Q.count == 0)
			SDL_CondWait(Q.cond, Q.mutex);
		if (Q.quit)
			break;
		struct item it = Q.items[Q.head];
		Q.items[Q.head].data = NULL;
		Q.head = (Q.head + 1) % USBQ_ITEMS;
		Q.count--;
		Q.bytes -= it.len;
		Q.busy = true;
		SDL_CondBroadcast(Q.cond);
		SDL_UnlockMutex(Q.mutex);
		uint64_t t0 = ns_now();
		Q.decode(it.data, it.len, it.h265);
		uint64_t took = ns_now() - t0;
		free(it.data);
		SDL_LockMutex(Q.mutex);
		Q.busy = false;
		Q.decoded++;
		Q.decode_ns += took;
		if (took > Q.decode_max_ns)
			Q.decode_max_ns = took;
		SDL_CondBroadcast(Q.cond);
	}
	drop_locked();
	Q.busy = false;
	SDL_CondBroadcast(Q.cond);
	SDL_UnlockMutex(Q.mutex);
	return 0;
}

bool usbq_start(usbq_decode_fn decode, usbq_log_fn log)
{
	Q.decode = decode;
	Q.log = log;
	if (Q.thread)
		return true;
	Q.mutex = SDL_CreateMutex();
	Q.cond = SDL_CreateCond();
	if (Q.mutex && Q.cond)
		Q.thread = SDL_CreateThread(thread_main, "usb pictures", NULL);
	if (Q.thread)
		return true;
	/* (no thread: usbq_push decodes at once) */
	if (Q.cond)
		SDL_DestroyCond(Q.cond);
	if (Q.mutex)
		SDL_DestroyMutex(Q.mutex);
	Q.cond = NULL;
	Q.mutex = NULL;
	return false;
}

void usbq_push(const uint8_t *annexb, size_t len, bool h265)
{
	if (!annexb || len == 0 || len > USBQ_BYTES / 2)
		return;
	if (!Q.mutex) {
		/* (no thread: decoded here; the caller leaves the padding after the picture) */
		if (Q.decode)
			Q.decode(annexb, len, h265);
		return;
	}
	uint8_t *copy = malloc(len + USBQ_PADDING);
	if (!copy)
		return;
	memcpy(copy, annexb, len);
	memset(copy + len, 0, USBQ_PADDING);
	uint64_t t = SDL_GetTicks64();
	int fps = -1;
	unsigned dropped_now = 0;
	SDL_LockMutex(Q.mutex);
	/* how the pictures come: gaps, and pictures a second over five seconds (written when it changes much) */
	Q.arrived++;
	if (Q.last_ms) {
		uint64_t gap = t - Q.last_ms;
		Q.gaps50 += gap > 50;
		Q.gaps100 += gap > 100;
	}
	Q.last_ms = t ? t : 1;
	if (!Q.window_ms)
		Q.window_ms = t ? t : 1;
	Q.window_n++;
	if (t - Q.window_ms >= 5000) {
		int now_fps = (int)((double)Q.window_n * 1000.0 / (double)(t - Q.window_ms) + 0.5);
		if ((Q.shown_fps < 0 || abs(now_fps - Q.shown_fps) >= 8) && Q.fps_lines < 60) {
			Q.shown_fps = now_fps;
			Q.fps_lines++;
			fps = now_fps;
		}
		Q.window_ms = t;
		Q.window_n = 0;
	}
	/* room in the queue: waited for; a queue that stays full for a second is emptied */
	uint64_t until = SDL_GetTicks64() + 1000;
	while (!Q.quit && Q.count > 0 && (Q.count >= USBQ_ITEMS || Q.bytes + len > USBQ_BYTES)) {
		if (SDL_GetTicks64() >= until) {
			dropped_now = drop_locked();
			Q.want_key = true;
			break;
		}
		SDL_CondWaitTimeout(Q.cond, Q.mutex, 50);
	}
	bool queued = false;
	if (!Q.quit && Q.want_key && usbq_key_frame(copy, len, h265))
		Q.want_key = false;
	if (!Q.quit && !Q.want_key) {
		Q.items[(Q.head + Q.count) % USBQ_ITEMS] = (struct item){copy, len, h265};
		Q.count++;
		Q.bytes += len;
		if ((unsigned)Q.count > Q.deepest)
			Q.deepest = (unsigned)Q.count;
		queued = true;
		SDL_CondBroadcast(Q.cond);
	} else if (!Q.quit) {
		Q.dropped++;
	}
	SDL_UnlockMutex(Q.mutex);
	if (!queued)
		free(copy);
	if (fps >= 0)
		say("USB: %d pictures a second", fps);
	if (dropped_now)
		say("USB: decoding fell behind for a second: %u pictures dropped, waiting for a key frame", dropped_now);
}

void usbq_end_session(void)
{
	if (!Q.mutex)
		return;
	SDL_LockMutex(Q.mutex);
	drop_locked();
	uint64_t until = SDL_GetTicks64() + 2000;
	while (Q.busy && SDL_GetTicks64() < until)
		SDL_CondWaitTimeout(Q.cond, Q.mutex, 50);
	unsigned arrived = Q.arrived, decoded = Q.decoded, dropped = Q.dropped, deepest = Q.deepest;
	unsigned gaps50 = Q.gaps50, gaps100 = Q.gaps100;
	double avg = decoded ? (double)Q.decode_ns / decoded / 1e6 : 0.0, max = (double)Q.decode_max_ns / 1e6;
	reset_locked();
	SDL_UnlockMutex(Q.mutex);
	if (arrived)
		say("USB pictures: %u came, %u decoded (%.1f ms on average, %.1f ms at most), at most %u waiting, "
		    "%u dropped; gaps over 50 ms: %u (over 100 ms: %u)",
		    arrived, decoded, avg, max, deepest, dropped, gaps50, gaps100);
}

void usbq_stop(void)
{
	if (!Q.thread)
		return;
	SDL_LockMutex(Q.mutex);
	Q.quit = true;
	SDL_CondBroadcast(Q.cond);
	SDL_UnlockMutex(Q.mutex);
	SDL_WaitThread(Q.thread, NULL);
	Q.thread = NULL;
	/* (the mutex stays: a USB thread that was left running may still come by; it finds Q.quit and drops) */
}

/*
 * Tests the cable's picture queue (viewer/usbqueue.c) with real threads: order, waiting for room, emptying a queue
 * that stays full (and skipping to the next key frame), the end of a session while a picture is being decoded, the
 * end of the program, the byte limit, and the case without a thread. Runs with tests/sdl_shim/SDL.h (POSIX threads)
 * in place of SDL; meant to be run with ThreadSanitizer and with AddressSanitizer as well:
 *
 *   cc -std=gnu11 -O1 -g -fsanitize=thread -Itests/sdl_shim -Iviewer tests/usbqueue_test.c viewer/usbqueue.c \
 *      -lpthread -o usbqueue_test && ./usbqueue_test
 *   cc -std=gnu11 -O1 -g -fsanitize=address,undefined -Itests/sdl_shim -Iviewer tests/usbqueue_test.c \
 *      viewer/usbqueue.c -lpthread -o usbqueue_test && ./usbqueue_test
 *   ./usbqueue_test nothread      (the same program, thread creation failing)
 */
#include "usbqueue.h"

#include <SDL.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

int sdl_shim_fail_threads; /* (tests/sdl_shim/SDL.h) */
static int failures, checks;

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

/* what the decoder saw (written on the queue's thread, read by the test after waiting: under this mutex) */
static pthread_mutex_t seen_mutex = PTHREAD_MUTEX_INITIALIZER;
static uint32_t seen_seq[200000];
static size_t seen_n;
static bool seen_bad_padding, seen_bad_order;
static atomic_int decode_delay_us;        /* how long each decode takes */
static atomic_uint stall_on = 0xffffffff; /* the picture whose decoding stalls */
static atomic_int stall_ms;
static atomic_int decoding;               /* 1 while the decoder runs */
static pthread_t main_thread;
static atomic_bool decoded_on_main;

static pthread_mutex_t log_mutex = PTHREAD_MUTEX_INITIALIZER;
static char last_log[8][400];
static int logs;

static void on_log(const char *text)
{
	pthread_mutex_lock(&log_mutex);
	snprintf(last_log[logs % 8], sizeof(last_log[0]), "%s", text);
	logs++;
	pthread_mutex_unlock(&log_mutex);
}

/* the session line's "at most N waiting" */
static int deepest_logged(void)
{
	int n = -1;
	pthread_mutex_lock(&log_mutex);
	for (int i = 0; i < 8; i++) {
		const char *at = strstr(last_log[(logs - 1 - i + 64) % 8], "at most ");
		if (at && strstr(last_log[(logs - 1 - i + 64) % 8], "USB pictures:")) {
			sscanf(at, "at most %d waiting", &n);
			break;
		}
	}
	pthread_mutex_unlock(&log_mutex);
	return n;
}

static bool logged(const char *part)
{
	bool found = false;
	pthread_mutex_lock(&log_mutex);
	for (int i = 0; i < 8; i++)
		found = found || strstr(last_log[i], part) != NULL;
	pthread_mutex_unlock(&log_mutex);
	return found;
}

/* a picture: start code, NAL type (5: key frame), sequence number, filler */
static uint8_t *picture(uint32_t seq, bool key, size_t len)
{
	uint8_t *p = calloc(1, len + USBQ_PADDING);
	p[0] = p[1] = p[2] = 0;
	p[3] = 1;
	p[4] = key ? 0x65 : 0x41;
	memcpy(p + 5, &seq, 4);
	for (size_t i = 9; i < len; i++)
		p[i] = (uint8_t)(i * 31 + seq);
	return p;
}

static void on_decode(const uint8_t *d, size_t n, bool h265)
{
	(void)h265;
	atomic_store(&decoding, 1);
	if (pthread_equal(pthread_self(), main_thread))
		atomic_store(&decoded_on_main, true);
	uint32_t seq;
	memcpy(&seq, d + 5, 4);
	bool padding_ok = true;
	for (size_t i = 0; i < USBQ_PADDING; i++)
		padding_ok = padding_ok && d[n + i] == 0;
	for (size_t i = 9; i < n; i++)
		padding_ok = padding_ok && d[i] == (uint8_t)(i * 31 + seq);
	if (seq == atomic_load(&stall_on))
		usleep((useconds_t)atomic_load(&stall_ms) * 1000);
	int delay = atomic_load(&decode_delay_us);
	if (delay)
		usleep((useconds_t)delay);
	pthread_mutex_lock(&seen_mutex);
	if (!padding_ok)
		seen_bad_padding = true;
	if (seen_n && seq <= seen_seq[seen_n - 1])
		seen_bad_order = true;
	if (seen_n < sizeof(seen_seq) / sizeof(seen_seq[0]))
		seen_seq[seen_n++] = seq;
	pthread_mutex_unlock(&seen_mutex);
	atomic_store(&decoding, 0);
}

static void reset_seen(void)
{
	pthread_mutex_lock(&seen_mutex);
	seen_n = 0;
	seen_bad_padding = seen_bad_order = false;
	pthread_mutex_unlock(&seen_mutex);
}

static size_t seen_count(void)
{
	pthread_mutex_lock(&seen_mutex);
	size_t n = seen_n;
	pthread_mutex_unlock(&seen_mutex);
	return n;
}

/* waits until the decoder has seen n pictures (or the time is up) */
static bool wait_seen(size_t n, int ms)
{
	for (int i = 0; i < ms; i++) {
		if (seen_count() >= n)
			return true;
		usleep(1000);
	}
	return seen_count() >= n;
}

static void push(uint32_t seq, bool key, size_t len)
{
	uint8_t *p = picture(seq, key, len);
	usbq_push(p, len, false);
	free(p);
}

static void test_key_frames(void)
{
	static const uint8_t idr[] = {0, 0, 0, 1, 0x67, 1, 2, 0, 0, 1, 0x65, 9};
	static const uint8_t p_frame[] = {0, 0, 0, 1, 0x41, 1, 2, 3};
	static const uint8_t hevc_idr[] = {0, 0, 0, 1, 0x40, 1, 0, 0, 1, 0x26, 1}; /* VPS, then type 19 */
	static const uint8_t hevc_p[] = {0, 0, 0, 1, 0x02, 1, 7};                  /* type 1 */
	static const uint8_t cut[] = {0, 0, 1};
	CHECK(usbq_key_frame(idr, sizeof(idr), false), "H.264 IDR not found");
	CHECK(!usbq_key_frame(p_frame, sizeof(p_frame), false), "H.264 P frame taken for a key frame");
	CHECK(usbq_key_frame(hevc_idr, sizeof(hevc_idr), true), "H.265 IDR not found");
	CHECK(!usbq_key_frame(hevc_p, sizeof(hevc_p), true), "H.265 P frame taken for a key frame");
	CHECK(!usbq_key_frame(cut, sizeof(cut), false), "a start code without a NAL taken for a key frame");
	CHECK(!usbq_key_frame(NULL, 0, false), "nothing taken for a key frame");
}

int main(int argc, char **argv)
{
	main_thread = pthread_self();
	test_key_frames();

	if (argc > 1 && strcmp(argv[1], "nothread") == 0) {
		/* no thread: decoded at once, on the caller's thread */
		sdl_shim_fail_threads = 1;
		CHECK(!usbq_start(on_decode, on_log), "a thread although none could be made");
		push(1, true, 100);
		push(2, false, 100);
		CHECK(seen_count() == 2 && atomic_load(&decoded_on_main), "without a thread: not decoded at once here");
		usbq_end_session();
		usbq_stop();
		printf("%d check(s), %d failure(s)\n", checks, failures);
		return failures ? 1 : 0;
	}

	CHECK(usbq_start(on_decode, on_log), "no thread");

	/* 1: many pictures, a fast decoder: all of them, in order, with their padding, not on this thread */
	for (uint32_t i = 1; i <= 20000; i++)
		push(i, i % 60 == 1, 50 + (i * 7919) % 3000);
	CHECK(wait_seen(20000, 20000), "fast: %zu of 20000 decoded", seen_count());
	pthread_mutex_lock(&seen_mutex);
	CHECK(!seen_bad_order && !seen_bad_padding, "fast: order %d, padding %d", seen_bad_order, seen_bad_padding);
	pthread_mutex_unlock(&seen_mutex);
	CHECK(!atomic_load(&decoded_on_main), "decoded on the pushing thread");
	usbq_end_session();
	CHECK(logged("USB pictures: 20000 came, 20000 decoded"), "fast: the session line is missing");

	/* 2: a slow decoder: the queue fills, the pusher waits, nothing is dropped */
	reset_seen();
	atomic_store(&decode_delay_us, 2000);
	for (uint32_t i = 1; i <= 300; i++)
		push(i, i == 1, 1000);
	CHECK(wait_seen(300, 10000), "slow: %zu of 300 decoded", seen_count());
	atomic_store(&decode_delay_us, 0);
	pthread_mutex_lock(&seen_mutex);
	CHECK(!seen_bad_order && seen_n == 300, "slow: order %d, %zu decoded", seen_bad_order, seen_n);
	pthread_mutex_unlock(&seen_mutex);
	usbq_end_session();
	CHECK(logged("at most 32 waiting, 0 dropped"), "slow: the queue did not fill or dropped");

	/* 3: a decoder stuck for 1.5 s: after a second the queue is emptied, then up to the next key frame nothing is
	 * taken; the first picture after the stuck one is a key frame */
	reset_seen();
	atomic_store(&stall_ms, 1500);
	atomic_store(&stall_on, 2);
	for (uint32_t i = 1; i <= 200; i++)
		push(i, i == 1 || i == 150, 500);
	atomic_store(&stall_on, 0xffffffff);
	CHECK(wait_seen(52, 5000), "stuck: %zu decoded", seen_count());
	usleep(100000);
	pthread_mutex_lock(&seen_mutex);
	bool resumes_at_key = seen_n >= 3 && seen_seq[0] == 1 && seen_seq[1] == 2 && seen_seq[2] == 150;
	CHECK(resumes_at_key && !seen_bad_order && seen_n == 53, "stuck: %zu decoded, then picture %u (150 expected)",
	      seen_n, seen_n >= 3 ? seen_seq[2] : 0);
	pthread_mutex_unlock(&seen_mutex);
	CHECK(logged("decoding fell behind for a second"), "stuck: no line about the dropped pictures");
	usbq_end_session();

	/* 4: the session ends while a picture is decoded: the call waits for it; the rest is dropped, nothing is decoded
	 * after the call returns */
	reset_seen();
	atomic_store(&decode_delay_us, 50000);
	for (uint32_t i = 1; i <= 20; i++)
		push(i, i == 1, 200);
	usleep(20000);
	usbq_end_session();
	CHECK(!atomic_load(&decoding), "end: a picture is still being decoded after the session ended");
	size_t at_end = seen_count();
	usleep(200000);
	CHECK(seen_count() == at_end && at_end <= 2, "end: %zu decoded at the end, %zu later", at_end, seen_count());
	atomic_store(&decode_delay_us, 0);

	/* 5: the byte limit: big pictures with a slow decoder; never more than 64 MB waiting */
	reset_seen();
	atomic_store(&decode_delay_us, 60000);
	for (uint32_t i = 1; i <= 30; i++)
		push(i, i == 1, 9u << 20);
	CHECK(wait_seen(30, 30000), "bytes: %zu of 30 decoded", seen_count());
	atomic_store(&decode_delay_us, 0);
	usbq_end_session();
	int deepest = deepest_logged();
	/* (9 MB each: 7 make 63 MB, an eighth would pass 64 MB) */
	CHECK(deepest >= 2 && deepest <= 7, "bytes: at most %d waited (2-7 expected)", deepest);
	/* (too big for the queue: not taken) */
	reset_seen();
	push(1, true, USBQ_BYTES / 2 + 1);
	usleep(50000);
	CHECK(seen_count() == 0, "a picture bigger than half the queue was taken");

	/* 6: the end of the program: the thread ends; pictures after that are dropped */
	reset_seen();
	for (uint32_t i = 1; i <= 50; i++)
		push(i, i == 1, 300);
	usbq_stop();
	size_t at_stop = seen_count();
	push(51, true, 300);
	usleep(50000);
	CHECK(seen_count() == at_stop, "a picture was decoded after the end");
	usbq_end_session();

	printf("%d check(s), %d failure(s)\n", checks, failures);
	return failures ? 1 : 0;
}

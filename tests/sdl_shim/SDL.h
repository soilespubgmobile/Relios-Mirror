/*
 * A stand-in for the few SDL functions viewer/usbqueue.c uses (threads, mutexes, condition variables, clocks), on
 * POSIX threads: so that tests/usbqueue_test.c can run the queue on Linux, with ThreadSanitizer too.
 * Only for the tests; the program uses SDL itself.
 */
#ifndef RELIOS_TEST_SDL_SHIM_H
#define RELIOS_TEST_SDL_SHIM_H

#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <time.h>

typedef uint64_t Uint64;
typedef uint32_t Uint32;
typedef int32_t Sint32;
typedef pthread_mutex_t SDL_mutex;
typedef pthread_cond_t SDL_cond;
typedef int (*SDL_ThreadFunction)(void *data);
typedef struct SDL_Thread {
	pthread_t id;
	SDL_ThreadFunction fn;
	void *data;
	int result;
} SDL_Thread;

/* the tests can make thread creation fail (the queue then decodes on the caller's thread); defined once, in the
 * test (shared by every file that includes this) */
extern int sdl_shim_fail_threads;

static inline SDL_mutex *SDL_CreateMutex(void)
{
	SDL_mutex *m = malloc(sizeof(*m));
	if (m && pthread_mutex_init(m, NULL) != 0) {
		free(m);
		m = NULL;
	}
	return m;
}

static inline void SDL_DestroyMutex(SDL_mutex *m)
{
	if (m) {
		pthread_mutex_destroy(m);
		free(m);
	}
}

static inline int SDL_LockMutex(SDL_mutex *m)
{
	return pthread_mutex_lock(m);
}

static inline int SDL_UnlockMutex(SDL_mutex *m)
{
	return pthread_mutex_unlock(m);
}

static inline SDL_cond *SDL_CreateCond(void)
{
	SDL_cond *c = malloc(sizeof(*c));
	if (!c)
		return NULL;
	pthread_condattr_t a;
	pthread_condattr_init(&a);
	pthread_condattr_setclock(&a, CLOCK_MONOTONIC);
	if (pthread_cond_init(c, &a) != 0) {
		free(c);
		c = NULL;
	}
	pthread_condattr_destroy(&a);
	return c;
}

static inline void SDL_DestroyCond(SDL_cond *c)
{
	if (c) {
		pthread_cond_destroy(c);
		free(c);
	}
}

static inline int SDL_CondWait(SDL_cond *c, SDL_mutex *m)
{
	return pthread_cond_wait(c, m);
}

#define SDL_MUTEX_TIMEDOUT 1

static inline int SDL_CondWaitTimeout(SDL_cond *c, SDL_mutex *m, Uint32 ms)
{
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	t.tv_sec += ms / 1000;
	t.tv_nsec += (long)(ms % 1000) * 1000000L;
	if (t.tv_nsec >= 1000000000L) {
		t.tv_sec++;
		t.tv_nsec -= 1000000000L;
	}
	int r = pthread_cond_timedwait(c, m, &t);
	return r == ETIMEDOUT ? SDL_MUTEX_TIMEDOUT : r;
}

static inline int SDL_CondBroadcast(SDL_cond *c)
{
	return pthread_cond_broadcast(c);
}

static inline void *sdl_shim_start(void *arg)
{
	SDL_Thread *t = arg;
	t->result = t->fn(t->data);
	return NULL;
}

static inline SDL_Thread *SDL_CreateThread(SDL_ThreadFunction fn, const char *name, void *data)
{
	(void)name;
	if (sdl_shim_fail_threads)
		return NULL;
	SDL_Thread *t = calloc(1, sizeof(*t));
	if (!t)
		return NULL;
	t->fn = fn;
	t->data = data;
	if (pthread_create(&t->id, NULL, sdl_shim_start, t) != 0) {
		free(t);
		return NULL;
	}
	return t;
}

static inline void SDL_WaitThread(SDL_Thread *t, int *status)
{
	if (!t)
		return;
	pthread_join(t->id, NULL);
	if (status)
		*status = t->result;
	free(t);
}

static inline Uint64 SDL_GetPerformanceFrequency(void)
{
	return 1000000000ull;
}

static inline Uint64 SDL_GetPerformanceCounter(void)
{
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return (Uint64)t.tv_sec * 1000000000ull + (Uint64)t.tv_nsec;
}

static inline Uint64 SDL_GetTicks64(void)
{
	return SDL_GetPerformanceCounter() / 1000000ull;
}

#endif

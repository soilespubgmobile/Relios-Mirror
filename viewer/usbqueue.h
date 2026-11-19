/*
 * Relios Mirror: the cable's pictures, decoded on a thread of their own (usbqueue.c).
 *
 * The device sends its next picture only when it is asked for one (NEED), and the USB thread asks only after its
 * picture callback returns. Decoding there (the graphics card, the picture brought back to memory, the window, the
 * recording) held every request back by some milliseconds, and a busy device then missed frames. So the USB thread
 * only copies the picture into this queue and goes on; the queue's thread decodes.
 *
 * Uses SDL's threads, mutexes and clocks only (tests/usbqueue_test.c runs it with a stand-in for them).
 */
#ifndef RELIOS_USBQUEUE_H
#define RELIOS_USBQUEUE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define USBQ_PADDING 64     /* zero bytes after each picture (FFmpeg's AV_INPUT_BUFFER_PADDING_SIZE) */
#define USBQ_ITEMS 32       /* at most this many pictures wait */
#define USBQ_BYTES (64u << 20) /* and at most this many bytes */

/* decodes one picture: Annex B, followed by USBQ_PADDING zero bytes. Called on the queue's thread only (or, without
 * a thread, on the caller's) */
typedef void (*usbq_decode_fn)(const uint8_t *annexb, size_t len, bool h265);
/* one line for the log (from any thread) */
typedef void (*usbq_log_fn)(const char *text);

/* starts the thread; false: none could be made (usbq_push then decodes on the caller's thread, as before 1.3.14) */
bool usbq_start(usbq_decode_fn decode, usbq_log_fn log);

/* a picture from the USB thread (followed by USBQ_PADDING zero bytes, which are needed only without a thread): copied
 * into the queue. A full queue is waited for (the device is then asked later and sends fewer); one that stays full for
 * a second is emptied, and pictures are skipped up to the next key frame (no broken pictures) */
void usbq_push(const uint8_t *annexb, size_t len, bool h265);

/* the cable's mirroring ended: what waits is dropped, the picture being decoded is finished (so none is shown after
 * the window was cleared), and the session's figures go to the log */
void usbq_end_session(void);

/* the end of the program: the thread ends (what waits is dropped). usbq_push after this drops what it gets */
void usbq_stop(void);

/* a key frame (H.264 IDR, H.265 IRAP) among the NAL units of an Annex B picture */
bool usbq_key_frame(const uint8_t *annexb, size_t len, bool h265);

#endif

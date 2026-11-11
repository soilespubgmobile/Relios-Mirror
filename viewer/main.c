/*
 * Relios Mirror: the iPad's AirPlay screen mirroring in a window of its own, without OBS.
 *
 * The network side is ReliosOBS's receiver program, unchanged (relios-airplay-receiver, ../receiver): this
 * program starts it (on Windows at low integrity, in a job that ends with this program), keeps the list of
 * paired devices and the receiver's key in its settings folder, decodes the video and audio the receiver passes
 * on (FFmpeg) and shows / plays them (SDL2). The window can be captured by Discord, OBS or anything else.
 *
 * Also: volume, mute, picture size / frame rate / codec asked from the iPad, recording to a file (MKV).
 *
 * GPL-3.0 (as the receiver).
 */
#ifdef _WIN32
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
#include <windows.h>
#include <bcrypt.h>
#include <shellapi.h>
#else
#include <sys/stat.h>
#include <unistd.h>
#endif

#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <SDL.h>
#include <SDL_ttf.h>

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/dict.h>
#include <libavutil/hwcontext.h>
#include <libavutil/imgutils.h>
#include <libavutil/log.h>
#include <libswscale/swscale.h>

#include "protocol.h"
#include "proc.h"

#define APP_NAME "Relios Mirror"
#define RECEIVER_NAME "Relios Mirror" /* what the iPad shows in Screen Mirroring */
#define CONFIG_FOLDER "ReliosMirror"

#ifdef _WIN32
#define RECEIVER_EXE "relios-airplay-receiver.exe"
#define PATH_SEP "\\"
#else
#define RECEIVER_EXE "relios-airplay-receiver"
#define PATH_SEP "/"
#endif

enum app_state {
	MIRROR_STARTING,
	MIRROR_LISTENING,
	MIRROR_PIN,
	MIRROR_CONNECTED,
	MIRROR_PAIRING_OFF,
	MIRROR_ENDED,
	MIRROR_ERROR,
};

enum { EV_FRAME = 1, EV_STATE = 2, EV_ENDED = 3 };

enum {
	CMD_NONE = 0,
	CMD_FULLSCREEN,
	CMD_ONTOP,
	CMD_MUTE,
	CMD_VOL_UP,
	CMD_VOL_DOWN,
	CMD_VOL_100,
	CMD_RECORD,
	CMD_OPEN_FOLDER,
	CMD_PAIR,
	CMD_FORGET,
	CMD_QUIT,
	CMD_RES_NEXT,
	CMD_FPS_NEXT,
	CMD_CODEC_NEXT,
	CMD_REC_ENCODER,
	CMD_HW_DECODE,
};

static struct {
	SDL_Window *win;
	SDL_Renderer *ren;
	SDL_Texture *tex;
	int tex_w, tex_h;
	Uint32 event_type;
	bool fullscreen;

	/* shown state (under mutex) */
	SDL_mutex *mutex;
	enum app_state state;
	char pin[8];
	char client[160];
	char format[64];
	char error[200];
	bool pair_requested;
	uint64_t win_bytes, win_frames;

	/* settings */
	int width, height, fps;
	bool h265;
	bool on_top;
	bool rec_gpu;   /* record with the graphics card's encoder */
	bool hw_decode; /* decode with the graphics card */
	SDL_atomic_t volume; /* percent, 0..200 */
	SDL_atomic_t muted;

	/* the picture: from the reading thread to the window thread */
	SDL_mutex *frame_mutex;
	AVFrame *pending; /* the newest decoded picture, not yet in the texture */
	bool pending_full, pending_709;
	bool have_frame, frame_dirty;
	SDL_atomic_t frame_event;
	AVFrame *upload; /* (window thread) */
	Uint32 tex_format;
	int tex_mode;

	/* the buttons */
	TTF_Font *font;
	int font_px;
	Uint32 bar_until; /* shown until then (SDL ticks) after the mouse moved */
	bool bar_drawn;
	Uint32 apply_at; /* a changed size / rate / codec restarts the receiver then */

	/* measured, for the title */
	double mbit, fps_now;
	Uint32 stats_at;

	/* receiver */
	struct proc *proc;
	SDL_Thread *out_thread, *err_thread;
	SDL_atomic_t stopping;
	Uint32 restart_at; /* SDL ticks; 0: none planned */
	int generation;    /* of the receiver: an "ended" from an earlier one is ignored */
	char exe[2048];

	/* audio output (reading thread) */
	SDL_AudioDeviceID adev;
	int arate, ach;

	char video_folder[1024];
} A;

/* recording (under R.mutex) */
static struct {
	SDL_mutex *mutex;
	bool want, failed;
	AVFormatContext *oc;
	AVCodecContext *enc;
	AVStream *vst, *ast;
	AVFrame *frame;
	AVPacket *pkt;
	struct SwsContext *sws;
	int w, h;
	uint64_t t0;
	int64_t last_vpts, next_apts;
	bool audio_started, header_written;
	char encoder[32];
	char path[1400];
} R;

/* ------------------------------------------------------------------ small helpers */

static FILE *log_file;
static SDL_mutex *log_mutex;

void app_log(const char *fmt, ...)
{
	char text[2300];
	va_list args;
	va_start(args, fmt);
	vsnprintf(text, sizeof(text), fmt, args);
	va_end(args);
	if (!log_file)
		return;
	if (log_mutex)
		SDL_LockMutex(log_mutex);
	char stamp[32];
	time_t now = time(NULL);
	struct tm *tm_now = localtime(&now);
	if (tm_now)
		strftime(stamp, sizeof(stamp), "%H:%M:%S", tm_now);
	else
		stamp[0] = '\0';
	fprintf(log_file, "%s %s\n", stamp, text);
	fflush(log_file);
	if (log_mutex)
		SDL_UnlockMutex(log_mutex);
}

static uint64_t now_ns(void)
{
	Uint64 c = SDL_GetPerformanceCounter(), f = SDL_GetPerformanceFrequency();
	return (uint64_t)(c / f) * 1000000000ull + (uint64_t)(c % f) * 1000000000ull / (uint64_t)f;
}

static void push_event(int code)
{
	SDL_Event e;
	SDL_zero(e);
	e.type = A.event_type;
	e.user.code = code;
	SDL_PushEvent(&e);
}

#ifdef _WIN32
static wchar_t *to_wide(const char *utf8)
{
	int n = MultiByteToWideChar(CP_UTF8, 0, utf8, -1, NULL, 0);
	wchar_t *w = calloc((size_t)(n > 0 ? n : 0) + 1, sizeof(wchar_t));
	if (w && n > 0)
		MultiByteToWideChar(CP_UTF8, 0, utf8, -1, w, n);
	return w;
}
#endif

static void make_folder(const char *utf8)
{
#ifdef _WIN32
	wchar_t *w = to_wide(utf8);
	if (w)
		CreateDirectoryW(w, NULL);
	free(w);
#else
	mkdir(utf8, 0755);
#endif
}

/* The settings folder becomes the current folder: the files in it are then opened by their plain names
 * (also by the receiver, which inherits it), whatever letters the user's name has. */
static bool enter_config_folder(void)
{
#ifdef _WIN32
	wchar_t path[1024];
	const wchar_t *base = _wgetenv(L"APPDATA");
	if (!base)
		return false;
	_snwprintf(path, 1023, L"%ls\\ReliosMirror", base);
	path[1023] = 0;
	CreateDirectoryW(path, NULL);
	return SetCurrentDirectoryW(path) != 0;
#else
	char path[1024];
	const char *home = getenv("HOME");
	snprintf(path, sizeof(path), "%s/.config", home ? home : ".");
	mkdir(path, 0755);
	snprintf(path, sizeof(path), "%s/.config/" CONFIG_FOLDER, home ? home : ".");
	mkdir(path, 0755);
	return chdir(path) == 0;
#endif
}

static void find_video_folder(void)
{
#ifdef _WIN32
	char home[800] = "";
	const wchar_t *w = _wgetenv(L"USERPROFILE");
	if (w)
		WideCharToMultiByte(CP_UTF8, 0, w, -1, home, (int)sizeof(home), NULL, NULL);
	snprintf(A.video_folder, sizeof(A.video_folder), "%s\\Videos", home);
	make_folder(A.video_folder);
	snprintf(A.video_folder, sizeof(A.video_folder), "%s\\Videos\\" CONFIG_FOLDER, home);
#else
	const char *home = getenv("HOME");
	snprintf(A.video_folder, sizeof(A.video_folder), "%s/" CONFIG_FOLDER, home ? home : ".");
#endif
	make_folder(A.video_folder);
}

static bool file_exists(const char *name)
{
	FILE *f = fopen(name, "rb");
	if (f)
		fclose(f);
	return f != NULL;
}

/* the whole file as text ("" if there is none); free() it */
static char *read_text(const char *name)
{
	FILE *f = fopen(name, "rb");
	char *text = NULL;
	size_t len = 0;
	if (f) {
		char chunk[4096];
		size_t n;
		while ((n = fread(chunk, 1, sizeof(chunk), f)) > 0) {
			char *bigger = realloc(text, len + n + 1);
			if (!bigger)
				break;
			text = bigger;
			memcpy(text + len, chunk, n);
			len += n;
		}
		fclose(f);
	}
	if (!text)
		text = malloc(1);
	if (text)
		text[len] = '\0';
	return text;
}

static void random_bytes(uint8_t *buf, size_t n)
{
	bool ok = false;
#ifdef _WIN32
	ok = BCryptGenRandom(NULL, buf, (ULONG)n, BCRYPT_USE_SYSTEM_PREFERRED_RNG) == 0;
#else
	FILE *f = fopen("/dev/urandom", "rb");
	if (f) {
		ok = fread(buf, 1, n, f) == n;
		fclose(f);
	}
#endif
	if (!ok) {
		uint64_t t = now_ns() ^ ((uint64_t)time(NULL) << 20);
		for (size_t i = 0; i < n; i++) {
			t = t * 6364136223846793005ull + 1442695040888963407ull;
			buf[i] = (uint8_t)(t >> 33);
		}
	}
}

/* ------------------------------------------------------------------ settings and paired devices */

static void load_settings(void)
{
	A.width = 1920;
	A.height = 1080;
	A.fps = 60;
	A.h265 = false;
	A.on_top = false;
	A.rec_gpu = false;
	A.hw_decode = true;
	SDL_AtomicSet(&A.volume, 100);
	SDL_AtomicSet(&A.muted, 0);
	FILE *f = fopen("ayarlar.txt", "r");
	if (!f)
		return;
	char line[200];
	while (fgets(line, sizeof(line), f)) {
		int a = 0, b = 0;
		if (sscanf(line, "cozunurluk=%dx%d", &a, &b) == 2 && a >= 320 && b >= 240 && a <= 3840 && b <= 2160) {
			A.width = a;
			A.height = b;
		} else if (sscanf(line, "fps=%d", &a) == 1 && a >= 15 && a <= 60) {
			A.fps = a;
		} else if (sscanf(line, "h265=%d", &a) == 1) {
			A.h265 = a != 0;
		} else if (sscanf(line, "ses=%d", &a) == 1 && a >= 0 && a <= 200) {
			SDL_AtomicSet(&A.volume, a);
		} else if (sscanf(line, "sessiz=%d", &a) == 1) {
			SDL_AtomicSet(&A.muted, a != 0);
		} else if (sscanf(line, "ustte=%d", &a) == 1) {
			A.on_top = a != 0;
		} else if (sscanf(line, "kayit_gpu=%d", &a) == 1) {
			A.rec_gpu = a != 0;
		} else if (sscanf(line, "gpu_cozme=%d", &a) == 1) {
			A.hw_decode = a != 0;
		}
	}
	fclose(f);
}

static void save_settings(void)
{
	FILE *f = fopen("ayarlar.txt", "w");
	if (!f)
		return;
	fprintf(f, "cozunurluk=%dx%d\nfps=%d\nh265=%d\nses=%d\nsessiz=%d\nustte=%d\nkayit_gpu=%d\ngpu_cozme=%d\n", A.width,
		A.height, A.fps, A.h265 ? 1 : 0, SDL_AtomicGet(&A.volume), SDL_AtomicGet(&A.muted), A.on_top ? 1 : 0,
		A.rec_gpu ? 1 : 0, A.hw_decode ? 1 : 0);
	fclose(f);
}

/* a random, locally administered address the iPad remembers the receiver by */
static void device_id(char *out, size_t size)
{
	char *text = read_text("device-id.txt");
	unsigned b[6];
	if (text && sscanf(text, "%2x:%2x:%2x:%2x:%2x:%2x", &b[0], &b[1], &b[2], &b[3], &b[4], &b[5]) == 6) {
		snprintf(out, size, "%02x:%02x:%02x:%02x:%02x:%02x", b[0], b[1], b[2], b[3], b[4], b[5]);
	} else {
		uint8_t r[6];
		random_bytes(r, sizeof(r));
		r[0] = (uint8_t)((r[0] & 0xfc) | 0x02);
		snprintf(out, size, "%02x:%02x:%02x:%02x:%02x:%02x", r[0], r[1], r[2], r[3], r[4], r[5]);
		FILE *f = fopen("device-id.txt", "w");
		if (f) {
			fprintf(f, "%s\n", out);
			fclose(f);
		}
	}
	free(text);
}

/* "<public key>\t<device id>\t<name>\t<date>" per line */
static void add_paired_device(const char *line)
{
	FILE *f = fopen("paired-devices.txt", "a");
	if (!f)
		return;
	char date[32] = "";
	time_t now = time(NULL);
	struct tm *tm_now = localtime(&now);
	if (tm_now)
		strftime(date, sizeof(date), "%Y-%m-%d %H:%M", tm_now);
	fprintf(f, "%s\t%s\n", line, date);
	fclose(f);
}

static size_t paired_count(const char *list)
{
	size_t n = 0;
	for (const char *p = list; p && *p;) {
		const char *end = strchr(p, '\n');
		size_t len = end ? (size_t)(end - p) : strlen(p);
		const char *tab = memchr(p, '\t', len);
		if (tab && tab - p > 10)
			n++;
		p = end ? end + 1 : p + len;
	}
	return n;
}

/* ------------------------------------------------------------------ recording */

static void rec_drain(void)
{
	while (avcodec_receive_packet(R.enc, R.pkt) == 0) {
		av_packet_rescale_ts(R.pkt, R.enc->time_base, R.vst->time_base);
		R.pkt->stream_index = R.vst->index;
		av_interleaved_write_frame(R.oc, R.pkt);
		av_packet_unref(R.pkt);
	}
}

static void rec_close_locked(void)
{
	if (R.oc && R.enc && R.header_written) {
		avcodec_send_frame(R.enc, NULL);
		rec_drain();
		av_write_trailer(R.oc);
		app_log("recording closed: %s", R.path);
	}
	if (R.oc && R.oc->pb)
		avio_closep(&R.oc->pb);
	if (R.oc)
		avformat_free_context(R.oc);
	R.oc = NULL;
	R.header_written = false;
	R.vst = R.ast = NULL;
	avcodec_free_context(&R.enc);
	av_frame_free(&R.frame);
	av_packet_free(&R.pkt);
	sws_freeContext(R.sws);
	R.sws = NULL;
}

static bool rec_open_locked(const AVFrame *f, bool full, bool bt709)
{
	char stamp[40] = "kayit";
	time_t now = time(NULL);
	struct tm *tm_now = localtime(&now);
	if (tm_now)
		strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", tm_now);
	snprintf(R.path, sizeof(R.path), "%s" PATH_SEP "ReliosMirror-%s.mkv", A.video_folder, stamp);

	R.w = f->width;
	R.h = f->height;
	if (avformat_alloc_output_context2(&R.oc, NULL, "matroska", R.path) < 0 || !R.oc)
		return false;
	R.frame = av_frame_alloc();
	R.pkt = av_packet_alloc();
	if (!R.frame || !R.pkt)
		return false;
	/* the graphics card's encoders first if asked (NVIDIA, AMD, Intel), then the processor's (x264) */
	static const char *const names[] = {"h264_nvenc", "h264_amf", "h264_qsv", "libx264", NULL};
	const AVCodec *codec = NULL;
	for (int n = A.rec_gpu ? 0 : 3; names[n] && !R.enc; n++) {
		bool gpu = n < 3;
		codec = avcodec_find_encoder_by_name(names[n]);
		if (!codec)
			continue;
		R.enc = avcodec_alloc_context3(codec);
		if (!R.enc)
			return false;
		R.enc->width = f->width & ~1;
		R.enc->height = f->height & ~1;
		R.enc->pix_fmt = gpu ? AV_PIX_FMT_NV12 : AV_PIX_FMT_YUV420P;
		R.enc->time_base = (AVRational){1, 1000};
		R.enc->framerate = (AVRational){A.fps, 1};
		R.enc->gop_size = 120;
		R.enc->color_range = full ? AVCOL_RANGE_JPEG : AVCOL_RANGE_MPEG;
		R.enc->colorspace = bt709 ? AVCOL_SPC_BT709 : AVCOL_SPC_SMPTE170M;
		if (R.oc->oformat->flags & AVFMT_GLOBALHEADER)
			R.enc->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
		AVDictionary *opts = NULL;
		if (gpu) {
			R.enc->bit_rate = (int64_t)R.enc->width * R.enc->height * A.fps / 10; /* 1080p60: ~12 Mbit/s */
		} else {
			av_dict_set(&opts, "preset", "veryfast", 0);
			av_dict_set(&opts, "crf", "20", 0);
		}
		int ret = avcodec_open2(R.enc, codec, &opts);
		av_dict_free(&opts);
		if (ret < 0) {
			app_log("recording: the encoder %s did not open (%d)", names[n], ret);
			avcodec_free_context(&R.enc);
		} else {
			snprintf(R.encoder, sizeof(R.encoder), "%s", gpu ? "GPU" : "CPU");
		}
	}
	if (!R.enc) {
		app_log("recording: no H.264 encoder could be used");
		return false;
	}

	R.vst = avformat_new_stream(R.oc, NULL);
	R.ast = avformat_new_stream(R.oc, NULL);
	if (!R.vst || !R.ast)
		return false;
	R.vst->time_base = R.enc->time_base;
	if (avcodec_parameters_from_context(R.vst->codecpar, R.enc) < 0)
		return false;
	/* the sound as it is decoded (16-bit PCM): AirPlay's is 44.1 kHz stereo */
	R.ast->codecpar->codec_type = AVMEDIA_TYPE_AUDIO;
	R.ast->codecpar->codec_id = AV_CODEC_ID_PCM_S16LE;
	R.ast->codecpar->sample_rate = 44100;
	av_channel_layout_default(&R.ast->codecpar->ch_layout, 2);
	R.ast->codecpar->format = AV_SAMPLE_FMT_S16;
	R.ast->codecpar->bits_per_coded_sample = 16;
	R.ast->codecpar->bit_rate = 44100 * 2 * 16;
	R.ast->time_base = (AVRational){1, 44100};

	R.frame->format = R.enc->pix_fmt;
	R.frame->width = R.enc->width;
	R.frame->height = R.enc->height;
	if (av_frame_get_buffer(R.frame, 0) < 0)
		return false;

	if (avio_open(&R.oc->pb, R.path, AVIO_FLAG_WRITE) < 0) {
		app_log("recording: could not write %s", R.path);
		return false;
	}
	if (avformat_write_header(R.oc, NULL) < 0) {
		app_log("recording: could not start the file");
		return false;
	}
	R.header_written = true;
	R.t0 = now_ns();
	R.last_vpts = -1;
	R.next_apts = 0;
	R.audio_started = false;
	app_log("recording started: %s (%dx%d, %s)", R.path, R.enc->width, R.enc->height, codec->name);
	return true;
}

static void rec_video(const AVFrame *f, bool full, bool bt709)
{
	SDL_LockMutex(R.mutex);
	if (R.want && !R.failed) {
		bool opened = false;
		if (R.oc && (R.w != f->width || R.h != f->height))
			rec_close_locked(); /* the iPad turned: a new file */
		if (!R.oc) {
			if (!rec_open_locked(f, full, bt709)) {
				rec_close_locked();
				R.failed = true;
			} else {
				opened = true;
			}
		}
		if (R.oc && R.header_written && av_frame_make_writable(R.frame) >= 0) {
			int w = R.enc->width, h = R.enc->height;
			if (R.enc->pix_fmt == AV_PIX_FMT_YUV420P &&
			    (f->format == AV_PIX_FMT_YUV420P || f->format == AV_PIX_FMT_YUVJ420P)) {
				av_image_copy_plane(R.frame->data[0], R.frame->linesize[0], f->data[0], f->linesize[0], w, h);
				av_image_copy_plane(R.frame->data[1], R.frame->linesize[1], f->data[1], f->linesize[1], w / 2,
						    h / 2);
				av_image_copy_plane(R.frame->data[2], R.frame->linesize[2], f->data[2], f->linesize[2], w / 2,
						    h / 2);
			} else {
				R.sws = sws_getCachedContext(R.sws, f->width, f->height, (enum AVPixelFormat)f->format, w, h,
							     R.enc->pix_fmt, SWS_BILINEAR, NULL, NULL, NULL);
				if (R.sws)
					sws_scale(R.sws, (const uint8_t *const *)f->data, f->linesize, 0, f->height,
						  R.frame->data, R.frame->linesize);
			}
			int64_t pts = (int64_t)((now_ns() - R.t0) / 1000000ull);
			if (pts <= R.last_vpts)
				pts = R.last_vpts + 1;
			R.last_vpts = pts;
			R.frame->pts = pts;
			if (avcodec_send_frame(R.enc, R.frame) >= 0)
				rec_drain();
		}
		if (opened || R.failed)
			push_event(EV_STATE);
	}
	SDL_UnlockMutex(R.mutex);
}

/* interleaved float samples, as they are played (before the volume) */
static void rec_audio(const float *samples, int frames, int rate, int channels)
{
	SDL_LockMutex(R.mutex);
	if (R.oc && R.header_written && rate == 44100 && (channels == 1 || channels == 2) && frames > 0) {
		int64_t t = (int64_t)((double)(now_ns() - R.t0) * 44100.0 / 1e9) - frames;
		if (t < 0)
			t = 0;
		/* by the sample count; by the clock only at the start and after a gap */
		if (!R.audio_started || t - R.next_apts > 44100 / 5)
			R.next_apts = t > R.next_apts ? t : R.next_apts;
		R.audio_started = true;
		if (av_new_packet(R.pkt, frames * 4) >= 0) {
			int16_t *out = (int16_t *)R.pkt->data;
			for (int i = 0; i < frames; i++) {
				for (int c = 0; c < 2; c++) {
					float v = samples[i * channels + (channels == 2 ? c : 0)] * 32767.0f;
					if (v > 32767.0f)
						v = 32767.0f;
					if (v < -32768.0f)
						v = -32768.0f;
					out[i * 2 + c] = (int16_t)v;
				}
			}
			R.pkt->pts = R.pkt->dts = R.next_apts;
			R.pkt->duration = frames;
			R.pkt->stream_index = R.ast->index;
			av_interleaved_write_frame(R.oc, R.pkt);
			av_packet_unref(R.pkt);
			R.next_apts += frames;
		}
	}
	SDL_UnlockMutex(R.mutex);
}

static void rec_toggle(void)
{
	SDL_LockMutex(R.mutex);
	if (R.want) {
		R.want = false;
		rec_close_locked();
	} else {
		R.want = true;
	}
	R.failed = false;
	SDL_UnlockMutex(R.mutex);
}

/* ------------------------------------------------------------------ decoding (the reading thread) */

struct decoder {
	AVCodecContext *vdec;
	bool vdec_h265;
	AVCodecContext *adec;
	int adec_ct;
	AVPacket *pkt;
	AVFrame *frame;
	AVFrame *sw; /* a picture brought from the graphics card */
	struct SwsContext *sws;
	float *mix;
	size_t mix_cap;
	bool warned_audio;
};

#ifdef _WIN32
static enum AVPixelFormat pick_format(AVCodecContext *ctx, const enum AVPixelFormat *formats)
{
	for (const enum AVPixelFormat *p = formats; *p != AV_PIX_FMT_NONE; p++) {
		if (*p == AV_PIX_FMT_D3D11)
			return *p;
	}
	return avcodec_default_get_format(ctx, formats);
}
#endif

static bool open_video_decoder(struct decoder *d, bool h265)
{
	if (d->vdec && d->vdec_h265 == h265)
		return true;
	avcodec_free_context(&d->vdec);
	const AVCodec *codec = avcodec_find_decoder(h265 ? AV_CODEC_ID_HEVC : AV_CODEC_ID_H264);
	if (!codec)
		return false;
	d->vdec = avcodec_alloc_context3(codec);
	if (!d->vdec)
		return false;
	d->vdec->flags |= AV_CODEC_FLAG_LOW_DELAY;
	d->vdec->thread_count = 0;
	d->vdec->thread_type = FF_THREAD_SLICE; /* (frame threads would hold frames back) */
	bool hw = false;
#ifdef _WIN32
	if (A.hw_decode) {
		AVBufferRef *device = NULL;
		if (av_hwdevice_ctx_create(&device, AV_HWDEVICE_TYPE_D3D11VA, NULL, NULL, 0) >= 0) {
			d->vdec->hw_device_ctx = device;
			d->vdec->get_format = pick_format;
			hw = true;
		}
	}
#endif
	if (avcodec_open2(d->vdec, codec, NULL) < 0) {
		avcodec_free_context(&d->vdec);
		return false;
	}
	d->vdec_h265 = h265;
	app_log("video decoder: %s (%s)", codec->name, hw ? "graphics card if it can" : "processor");
	return true;
}

static void frame_colors(const AVFrame *f, bool *full, bool *bt709)
{
	*full = f->format == AV_PIX_FMT_YUVJ420P || f->color_range == AVCOL_RANGE_JPEG;
	*bt709 = f->height >= 720;
	if (f->colorspace == AVCOL_SPC_BT709)
		*bt709 = true;
	else if (f->colorspace == AVCOL_SPC_SMPTE170M || f->colorspace == AVCOL_SPC_BT470BG)
		*bt709 = false;
}

/* the decoded picture (YUV, as it is) to the window thread, which puts it into a texture on the graphics card */
static void present_frame(struct decoder *d, const AVFrame *f, bool full, bool bt709)
{
	int w = f->width, h = f->height;
	if (w <= 0 || h <= 0 || w > 8192 || h > 8192)
		return;
	AVFrame *converted = NULL;
	if (f->format != AV_PIX_FMT_YUV420P && f->format != AV_PIX_FMT_YUVJ420P && f->format != AV_PIX_FMT_NV12) {
		/* (10-bit and other rare ones) */
		converted = av_frame_alloc();
		if (!converted)
			return;
		converted->format = AV_PIX_FMT_YUV420P;
		converted->width = w;
		converted->height = h;
		d->sws = sws_getCachedContext(d->sws, w, h, (enum AVPixelFormat)f->format, w, h, AV_PIX_FMT_YUV420P,
					      SWS_BILINEAR, NULL, NULL, NULL);
		if (!d->sws || av_frame_get_buffer(converted, 0) < 0) {
			av_frame_free(&converted);
			return;
		}
		sws_scale(d->sws, (const uint8_t *const *)f->data, f->linesize, 0, h, converted->data, converted->linesize);
		f = converted;
	}
	SDL_LockMutex(A.frame_mutex);
	av_frame_unref(A.pending);
	if (av_frame_ref(A.pending, f) >= 0) {
		A.pending_full = full;
		A.pending_709 = bt709;
		A.have_frame = true;
		A.frame_dirty = true;
	}
	SDL_UnlockMutex(A.frame_mutex);
	av_frame_free(&converted);
	if (SDL_AtomicCAS(&A.frame_event, 0, 1))
		push_event(EV_FRAME);
}

static void handle_video(struct decoder *d, const uint8_t *payload, size_t len)
{
	if (len <= sizeof(struct ra_video_header))
		return;
	struct ra_video_header h;
	memcpy(&h, payload, sizeof(h));
	const uint8_t *data = payload + sizeof(h);
	size_t data_len = len - sizeof(h);

	SDL_LockMutex(A.mutex);
	A.win_bytes += data_len;
	A.win_frames++;
	SDL_UnlockMutex(A.mutex);

	if (!open_video_decoder(d, h.is_h265 != 0))
		return;
	d->pkt->data = (uint8_t *)data;
	d->pkt->size = (int)data_len;
	d->pkt->pts = AV_NOPTS_VALUE;
	d->pkt->dts = AV_NOPTS_VALUE;
	if (avcodec_send_packet(d->vdec, d->pkt) < 0)
		return;
	while (avcodec_receive_frame(d->vdec, d->frame) == 0) {
		AVFrame *f = d->frame;
		if (f->format == AV_PIX_FMT_D3D11) {
			/* decoded on the graphics card: brought to memory (NV12) */
			av_frame_unref(d->sw);
			if (av_hwframe_transfer_data(d->sw, f, 0) < 0) {
				av_frame_unref(f);
				continue;
			}
			d->sw->color_range = f->color_range;
			d->sw->colorspace = f->colorspace;
			f = d->sw;
		}
		bool full, bt709;
		frame_colors(f, &full, &bt709);
		present_frame(d, f, full, bt709);
		rec_video(f, full, bt709);
		av_frame_unref(d->frame);
		av_frame_unref(d->sw);
	}
}

static bool open_audio_decoder(struct decoder *d, int ct)
{
	if (d->adec && d->adec_ct == ct)
		return true;
	avcodec_free_context(&d->adec);
	/* AAC-ELD / AAC-LC, 44.1 kHz stereo, as AirPlay sends them (UxPlay's audio renderer's settings) */
	static const uint8_t eld[] = {0xf8, 0xe8, 0x50, 0x00};
	static const uint8_t lc[] = {0x12, 0x10};
	const uint8_t *config = ct == 8 ? eld : lc;
	size_t config_len = ct == 8 ? sizeof(eld) : sizeof(lc);
	const AVCodec *codec = avcodec_find_decoder(AV_CODEC_ID_AAC);
	if (!codec)
		return false;
	d->adec = avcodec_alloc_context3(codec);
	if (!d->adec)
		return false;
	d->adec->extradata = av_mallocz(config_len + AV_INPUT_BUFFER_PADDING_SIZE);
	if (!d->adec->extradata) {
		avcodec_free_context(&d->adec);
		return false;
	}
	memcpy(d->adec->extradata, config, config_len);
	d->adec->extradata_size = (int)config_len;
	d->adec->sample_rate = 44100;
	av_channel_layout_default(&d->adec->ch_layout, 2);
	if (avcodec_open2(d->adec, codec, NULL) < 0) {
		avcodec_free_context(&d->adec);
		return false;
	}
	d->adec_ct = ct;
	app_log("audio decoder: %s (%s)", codec->name, ct == 8 ? "AAC-ELD" : "AAC-LC");
	return true;
}

static bool open_audio_output(int rate, int channels)
{
	if (A.adev && A.arate == rate && A.ach == channels)
		return true;
	if (A.adev)
		SDL_CloseAudioDevice(A.adev);
	SDL_AudioSpec want, have;
	SDL_zero(want);
	SDL_zero(have);
	want.freq = rate;
	want.format = AUDIO_F32SYS;
	want.channels = (Uint8)channels;
	want.samples = 512; /* small: the sound must not lag behind the picture */
	A.adev = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
	if (!A.adev) {
		app_log("no sound output: %s", SDL_GetError());
		A.arate = A.ach = 0;
		return false;
	}
	A.arate = rate;
	A.ach = channels;
	SDL_PauseAudioDevice(A.adev, 0);
	app_log("sound output: %d Hz, %d channels, buffer %d samples", rate, channels, (int)have.samples);
	return true;
}

static void handle_audio(struct decoder *d, const uint8_t *payload, size_t len)
{
	if (len <= sizeof(struct ra_audio_header))
		return;
	struct ra_audio_header h;
	memcpy(&h, payload, sizeof(h));
	if (h.ct != 8 && h.ct != 4) {
		if (!d->warned_audio)
			app_log("audio type %u is not played (only AAC from screen mirroring)", (unsigned)h.ct);
		d->warned_audio = true;
		return;
	}
	if (!open_audio_decoder(d, h.ct))
		return;
	d->pkt->data = (uint8_t *)payload + sizeof(h);
	d->pkt->size = (int)(len - sizeof(h));
	d->pkt->pts = AV_NOPTS_VALUE;
	d->pkt->dts = AV_NOPTS_VALUE;
	if (avcodec_send_packet(d->adec, d->pkt) < 0)
		return;
	while (avcodec_receive_frame(d->adec, d->frame) == 0) {
		AVFrame *f = d->frame;
		int ch = f->ch_layout.nb_channels, n = f->nb_samples;
		if (f->format == AV_SAMPLE_FMT_FLTP && ch >= 1 && ch <= 2 && n > 0 && f->sample_rate > 0) {
			size_t count = (size_t)n * (size_t)ch;
			if (count > d->mix_cap) {
				float *bigger = realloc(d->mix, count * sizeof(float));
				if (bigger) {
					d->mix = bigger;
					d->mix_cap = count;
				}
			}
			if (count <= d->mix_cap) {
				for (int c = 0; c < ch; c++) {
					const float *src = (const float *)f->extended_data[c];
					for (int i = 0; i < n; i++)
						d->mix[(size_t)i * (size_t)ch + (size_t)c] = src[i];
				}
				rec_audio(d->mix, n, f->sample_rate, ch);

				float gain = SDL_AtomicGet(&A.muted) ? 0.0f : (float)SDL_AtomicGet(&A.volume) / 100.0f;
				if (gain != 1.0f) {
					for (size_t i = 0; i < count; i++) {
						float v = d->mix[i] * gain;
						d->mix[i] = v > 1.0f ? 1.0f : v < -1.0f ? -1.0f : v;
					}
				}
				if (open_audio_output(f->sample_rate, ch)) {
					/* never more than ~0.15 s waiting to be played: what piled up is dropped */
					Uint32 limit = (Uint32)(f->sample_rate * ch * (int)sizeof(float) * 15 / 100);
					if (SDL_GetQueuedAudioSize(A.adev) > limit)
						SDL_ClearQueuedAudio(A.adev);
					SDL_QueueAudio(A.adev, d->mix, (Uint32)(count * sizeof(float)));
				}
			}
		}
		av_frame_unref(f);
	}
}

/* ------------------------------------------------------------------ messages from the receiver */

static void set_state(enum app_state state)
{
	SDL_LockMutex(A.mutex);
	A.state = state;
	SDL_UnlockMutex(A.mutex);
	push_event(EV_STATE);
}

static void clear_picture(void)
{
	SDL_LockMutex(A.frame_mutex);
	A.have_frame = false;
	A.frame_dirty = false;
	av_frame_unref(A.pending);
	SDL_UnlockMutex(A.frame_mutex);
}

static void handle_status(const char *text)
{
	if (!strncmp(text, "integrity", 9)) {
		app_log("receiver %s", text);
	} else if (!strncmp(text, "listening", 9)) {
		app_log("receiver listening (%s)", text + 9);
		set_state(MIRROR_LISTENING);
	} else if (!strncmp(text, "client", 6)) {
		/* (the name may hold spaces: everything between "name=" and " model=") */
		char name[96] = "?", model[48] = "?";
		const char *n0 = strstr(text, "name="), *m0 = strstr(text, " model=");
		if (n0 && m0 && m0 > n0) {
			size_t len = (size_t)(m0 - (n0 + 5));
			if (len >= sizeof(name))
				len = sizeof(name) - 1;
			memcpy(name, n0 + 5, len);
			name[len] = '\0';
			snprintf(model, sizeof(model), "%s", m0 + 7);
		}
		SDL_LockMutex(A.mutex);
		snprintf(A.client, sizeof(A.client), "%s (%s)", name, model);
		SDL_UnlockMutex(A.mutex);
		app_log("connected: %s (%s)", name, model);
		push_event(EV_STATE);
	} else if (!strncmp(text, "trusted", 7)) {
		app_log("device %s", strstr(text, "new=1") ? "paired with the code" : "paired before");
		SDL_LockMutex(A.mutex);
		A.pin[0] = '\0';
		A.pair_requested = false;
		SDL_UnlockMutex(A.mutex);
		set_state(MIRROR_CONNECTED);
	} else if (!strncmp(text, "mirroring", 9)) {
		bool on = strstr(text, " on") != NULL;
		app_log("screen mirroring %s", on ? "started" : "stopped");
		if (!on) {
			clear_picture();
			if (A.adev)
				SDL_ClearQueuedAudio(A.adev);
		}
		push_event(EV_STATE);
	} else if (!strncmp(text, "pairing_failed", 14)) {
		app_log("a pairing with a wrong or missing code was refused (%s)", text + 14);
	} else if (!strncmp(text, "pairing_off", 11)) {
		app_log("pairing a new device turned off after three wrong codes");
		SDL_LockMutex(A.mutex);
		A.pin[0] = '\0';
		A.pair_requested = false;
		SDL_UnlockMutex(A.mutex);
		set_state(MIRROR_PAIRING_OFF);
	} else if (!strncmp(text, "ended", 5)) {
		app_log("the receiver ended (its ports are closed)");
	} else if (!strncmp(text, "error", 5)) {
		const char *t = strstr(text, "text=");
		SDL_LockMutex(A.mutex);
		snprintf(A.error, sizeof(A.error), "%s", t ? t + 5 : text);
		SDL_UnlockMutex(A.mutex);
		app_log("receiver error: %s", t ? t + 5 : text);
		set_state(MIRROR_ERROR);
	}
}

static void handle_pin(const char *pin)
{
	SDL_LockMutex(A.mutex);
	snprintf(A.pin, sizeof(A.pin), "%s", pin);
	bool was_pin = A.state == MIRROR_PIN;
	SDL_UnlockMutex(A.mutex);
	if (pin[0]) {
		app_log("the iPad asks for a pairing code: shown in the window"); /* (the code itself is not logged) */
		set_state(MIRROR_PIN);
	} else if (was_pin) {
		set_state(MIRROR_LISTENING);
	} else {
		push_event(EV_STATE);
	}
}

static bool read_full(uint8_t *buf, size_t size)
{
	size_t got = 0;
	while (got < size) {
		size_t n = proc_read_out(A.proc, buf + got, size - got);
		if (n == 0)
			return false;
		got += n;
	}
	return true;
}

static int out_thread(void *generation)
{
	struct decoder d;
	memset(&d, 0, sizeof(d));
	uint8_t header[RA_HEADER_SIZE];
	size_t cap = 0;
	uint8_t *buf = NULL;

	d.pkt = av_packet_alloc();
	d.frame = av_frame_alloc();
	d.sw = av_frame_alloc();

	while (d.pkt && d.frame && d.sw && read_full(header, sizeof(header))) {
		if (header[0] != RA_MAGIC0 || header[1] != RA_MAGIC1) {
			app_log("unexpected data from the receiver; stopping it");
			break;
		}
		uint32_t len = (uint32_t)header[4] | ((uint32_t)header[5] << 8) | ((uint32_t)header[6] << 16) |
			       ((uint32_t)header[7] << 24);
		if (len > RA_MAX_PAYLOAD) {
			app_log("oversized message from the receiver; stopping it");
			break;
		}
		if ((size_t)len + AV_INPUT_BUFFER_PADDING_SIZE + 1 > cap) {
			size_t want = (size_t)len + AV_INPUT_BUFFER_PADDING_SIZE + 1;
			uint8_t *bigger = realloc(buf, want);
			if (!bigger)
				break;
			buf = bigger;
			cap = want;
		}
		if (len && !read_full(buf, len))
			break;
		memset(buf + len, 0, AV_INPUT_BUFFER_PADDING_SIZE + 1); /* (also ends text) */

		switch (header[2]) {
		case RA_MSG_STATUS:
			handle_status((const char *)buf);
			break;
		case RA_MSG_PIN:
			handle_pin((const char *)buf);
			break;
		case RA_MSG_REGISTER:
			add_paired_device((const char *)buf);
			app_log("a new device was paired and remembered");
			break;
		case RA_MSG_VIDEO_FORMAT:
			if (len >= sizeof(struct ra_video_format)) {
				struct ra_video_format vf;
				memcpy(&vf, buf, sizeof(vf));
				SDL_LockMutex(A.mutex);
				snprintf(A.format, sizeof(A.format), "%ux%u %s", (unsigned)vf.width, (unsigned)vf.height,
					 vf.codec == 2 ? "H.265" : "H.264");
				SDL_UnlockMutex(A.mutex);
				app_log("video: %s, picture %ux%u (iPad screen %ux%u)", vf.codec == 2 ? "H.265" : "H.264",
					(unsigned)vf.width, (unsigned)vf.height, (unsigned)vf.width_source,
					(unsigned)vf.height_source);
				push_event(EV_STATE);
			}
			break;
		case RA_MSG_VIDEO:
			handle_video(&d, buf, len);
			break;
		case RA_MSG_AUDIO:
			handle_audio(&d, buf, len);
			break;
		default:
			break;
		}
	}

	free(buf);
	free(d.mix);
	sws_freeContext(d.sws);
	avcodec_free_context(&d.vdec);
	avcodec_free_context(&d.adec);
	av_packet_free(&d.pkt);
	av_frame_free(&d.frame);
	av_frame_free(&d.sw);

	/* the file ends with the session; recording stays switched on for the next one */
	SDL_LockMutex(R.mutex);
	rec_close_locked();
	SDL_UnlockMutex(R.mutex);

	clear_picture();
	SDL_LockMutex(A.mutex);
	if (A.state != MIRROR_ERROR)
		A.state = MIRROR_ENDED;
	A.pin[0] = '\0';
	A.client[0] = A.format[0] = '\0';
	SDL_UnlockMutex(A.mutex);
	SDL_Event e;
	SDL_zero(e);
	e.type = A.event_type;
	e.user.code = EV_ENDED;
	e.user.data1 = generation;
	SDL_PushEvent(&e);
	return 0;
}

static int err_thread(void *unused)
{
	(void)unused;
	char chunk[4096], line[2100];
	size_t used = 0, n;
	while ((n = proc_read_err(A.proc, chunk, sizeof(chunk))) > 0) {
		for (size_t i = 0; i < n; i++) {
			if (chunk[i] == '\n') {
				line[used] = '\0';
				if (used)
					app_log("receiver: %s", line);
				used = 0;
			} else if (chunk[i] != '\r' && used + 1 < sizeof(line)) {
				line[used++] = chunk[i];
			}
		}
	}
	return 0;
}

/* ------------------------------------------------------------------ start / stop */

static void session_stop(void)
{
	if (!A.proc)
		return;
	SDL_AtomicSet(&A.stopping, 1);
	proc_write(A.proc, "quit\n");
	proc_stop(A.proc, 3000);
	if (A.out_thread)
		SDL_WaitThread(A.out_thread, NULL);
	if (A.err_thread)
		SDL_WaitThread(A.err_thread, NULL);
	A.out_thread = A.err_thread = NULL;
	proc_destroy(A.proc);
	A.proc = NULL;
	SDL_AtomicSet(&A.stopping, 0);
	if (A.adev)
		SDL_ClearQueuedAudio(A.adev);
	app_log("receiver stopped");
}

static void session_fail(const char *text)
{
	SDL_LockMutex(A.mutex);
	snprintf(A.error, sizeof(A.error), "%s", text);
	A.state = MIRROR_ERROR;
	SDL_UnlockMutex(A.mutex);
	app_log("could not start: %s", text);
	A.restart_at = SDL_GetTicks() + 5000;
}

static void session_start(void)
{
	session_stop();
	A.restart_at = 0;
	SDL_LockMutex(A.mutex);
	A.pin[0] = A.client[0] = A.format[0] = A.error[0] = '\0';
	A.state = MIRROR_STARTING;
	bool pair = A.pair_requested;
	SDL_UnlockMutex(A.mutex);

	SDL_RWops *rw = SDL_RWFromFile(A.exe, "rb");
	if (!rw) {
		session_fail(RECEIVER_EXE " bulunamadı (programla aynı klasörde olmalı)");
		return;
	}
	SDL_RWclose(rw);

	char *paired = read_text("paired-devices.txt");
	size_t paired_n = paired_count(paired);
	/* With no device paired nothing depends on the receiver's identity (its key and device id): it gets a new
	 * one, so an iPad that paired with an earlier one (forgotten here) pairs again with a code */
	if (paired_n == 0) {
		remove("receiver-key.pem");
		remove("device-id.txt");
	}
	if (!file_exists("receiver-key.pem")) {
		int code = proc_run(A.exe, "--make-key receiver-key.pem", 15000);
		app_log("receiver key made (%s)", code == 0 ? "ok" : "failed");
	}
	char id[32];
	device_id(id, sizeof(id));
	/* the first device pairs with the code shown here; later ones only after "pair a new device" */
	bool allow = pair || paired_n == 0;

	A.proc = proc_start(A.exe, "--serve", true);
	if (!A.proc) {
		app_log("the receiver did not start at low integrity; starting it normally");
		A.proc = proc_start(A.exe, "--serve", false);
	}
	if (!A.proc) {
		free(paired);
		session_fail("alıcı program başlatılamadı");
		return;
	}
	app_log("receiver started; paired devices: %d, pairing a new device: %s, asked %dx%d@%d %s", (int)paired_n,
		allow ? "on" : "off", A.width, A.height, A.fps, A.h265 ? "H.264/H.265" : "H.264");

	char config[512];
	snprintf(config, sizeof(config),
		 "name " RECEIVER_NAME "\ndeviceid %s\nkeyfile receiver-key.pem\nsize %dx%d@%d\nh265 %d\nallow_pairing %d\n",
		 id, A.width, A.height, A.fps, A.h265 ? 1 : 0, allow ? 1 : 0);
	proc_write(A.proc, config);
	for (const char *p = paired; p && *p;) {
		const char *end = strchr(p, '\n');
		size_t len = end ? (size_t)(end - p) : strlen(p);
		const char *tab = memchr(p, '\t', len);
		if (tab && tab - p > 10 && (size_t)(tab - p) < 3000) {
			char line[3100];
			snprintf(line, sizeof(line), "register %.*s\n", (int)(tab - p), p);
			proc_write(A.proc, line);
		}
		p = end ? end + 1 : p + len;
	}
	proc_write(A.proc, "start\n");
	free(paired);

	A.generation++;
	A.out_thread = SDL_CreateThread(out_thread, "receiver output", (void *)(intptr_t)A.generation);
	A.err_thread = SDL_CreateThread(err_thread, "receiver log", NULL);
	if (!A.out_thread || !A.err_thread) {
		session_stop();
		session_fail("iş parçacığı başlatılamadı");
	}
}

/* ------------------------------------------------------------------ the window */

static void status_text(char *status, size_t size)
{
	SDL_LockMutex(A.mutex);
	switch (A.state) {
	case MIRROR_STARTING:
		snprintf(status, size, "başlatılıyor…");
		break;
	case MIRROR_LISTENING:
		snprintf(status, size, "hazır — iPad'de Ekran Yansıtma → " RECEIVER_NAME "%s",
			 A.pair_requested ? " (yeni cihaz eşleştirme açık)" : "");
		break;
	case MIRROR_PIN:
		snprintf(status, size, "penceredeki kodu iPad'e girin");
		break;
	case MIRROR_CONNECTED:
		if (A.format[0])
			snprintf(status, size, "bağlı: %s | %s, %.0f fps, %.1f Mbit/s", A.client[0] ? A.client : "iPad",
				 A.format, A.fps_now, A.mbit);
		else
			snprintf(status, size, "bağlı: %s", A.client[0] ? A.client : "iPad");
		break;
	case MIRROR_PAIRING_OFF:
		snprintf(status, size, "üç yanlış kod: eşleştirme kapandı (\"Yeni cihaz\" ile yeniden açın)");
		break;
	case MIRROR_ENDED:
		snprintf(status, size, "yeniden başlatılıyor…");
		break;
	default:
		if (strstr(A.error, "TCP 7000"))
			snprintf(status, size,
				 "hata: 7000 portu kullanımda (OBS'teki iPad Yansıtma ya da başka bir AirPlay alıcısı açık olabilir)");
		else
			snprintf(status, size, "hata: %s", A.error);
		break;
	}
	SDL_UnlockMutex(A.mutex);

	char rec[80] = "";
	SDL_LockMutex(R.mutex);
	if (R.oc && R.header_written)
		snprintf(rec, sizeof(rec), " | KAYIT (%s)", R.encoder);
	else if (R.failed)
		snprintf(rec, sizeof(rec), " | kayıt başlatılamadı");
	else if (R.want)
		snprintf(rec, sizeof(rec), " | kayıt: görüntü bekleniyor");
	SDL_UnlockMutex(R.mutex);
	size_t used = strlen(status);
	snprintf(status + used, size - used, "%s", rec);
}

static void update_title(void)
{
	char title[900], status[600];
	status_text(status, sizeof(status));
	snprintf(title, sizeof(title), APP_NAME " — %s", status);
	SDL_SetWindowTitle(A.win, title);
}

/* ---- text: a system font, each text drawn once into a texture */

struct text_entry {
	char text[160];
	SDL_Texture *tex;
	int w, h;
};
static struct text_entry texts[64];
static int text_count;

static void texts_clear(void)
{
	for (int i = 0; i < text_count; i++)
		SDL_DestroyTexture(texts[i].tex);
	text_count = 0;
}

static void font_open(int px)
{
	if (A.font && A.font_px == px)
		return;
	if (A.font)
		TTF_CloseFont(A.font);
	texts_clear();
	A.font = NULL;
	A.font_px = px;
#ifdef _WIN32
	static const char *const files[] = {"segoeui.ttf", "arial.ttf", "tahoma.ttf", NULL};
	const char *windir = getenv("WINDIR");
	for (int i = 0; files[i] && !A.font; i++) {
		char path[600];
		snprintf(path, sizeof(path), "%s\\Fonts\\%s", windir ? windir : "C:\\Windows", files[i]);
		A.font = TTF_OpenFont(path, px);
	}
#else
	A.font = TTF_OpenFont("/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf", px);
#endif
}

static struct text_entry *text_get(const char *text)
{
	if (!A.font || !text[0])
		return NULL;
	for (int i = 0; i < text_count; i++) {
		if (!strcmp(texts[i].text, text))
			return &texts[i];
	}
	if (text_count == (int)(sizeof(texts) / sizeof(texts[0])))
		texts_clear();
	SDL_Color white = {240, 240, 245, 255};
	SDL_Surface *surface = TTF_RenderUTF8_Blended(A.font, text, white);
	if (!surface)
		return NULL;
	struct text_entry *t = &texts[text_count];
	t->tex = SDL_CreateTextureFromSurface(A.ren, surface);
	t->w = surface->w;
	t->h = surface->h;
	SDL_FreeSurface(surface);
	if (!t->tex)
		return NULL;
	snprintf(t->text, sizeof(t->text), "%s", text);
	text_count++;
	return t;
}

/* ---- the buttons: a bar at the bottom, shown while the mouse moves over the window (and while nothing is mirrored) */

struct button {
	int cmd;
	bool on;
	char text[64];
	SDL_Rect r;
};
static struct button buttons[20];
static int button_count;

static void add_button(int cmd, bool on, const char *fmt, ...)
{
	if (button_count >= (int)(sizeof(buttons) / sizeof(buttons[0])))
		return;
	struct button *b = &buttons[button_count++];
	b->cmd = cmd;
	b->on = on;
	va_list args;
	va_start(args, fmt);
	vsnprintf(b->text, sizeof(b->text), fmt, args);
	va_end(args);
}

static void build_buttons(void)
{
	SDL_LockMutex(R.mutex);
	bool recording = R.want;
	SDL_UnlockMutex(R.mutex);
	bool muted = SDL_AtomicGet(&A.muted) != 0;
	button_count = 0;
	add_button(CMD_RECORD, recording, recording ? "Kaydı durdur" : "Kayıt başlat");
	add_button(CMD_REC_ENCODER, false, A.rec_gpu ? "Kayıt: ekran kartı" : "Kayıt: işlemci");
	add_button(CMD_OPEN_FOLDER, false, "Kayıt klasörü");
	add_button(CMD_VOL_DOWN, false, "Ses -");
	add_button(CMD_VOL_100, false, "Ses %%%d", SDL_AtomicGet(&A.volume));
	add_button(CMD_VOL_UP, false, "Ses +");
	add_button(CMD_MUTE, muted, "Sessiz");
	add_button(CMD_RES_NEXT, false, "%d×%d", A.width, A.height);
	add_button(CMD_FPS_NEXT, false, "%d fps", A.fps);
	add_button(CMD_CODEC_NEXT, false, A.h265 ? "H.265" : "H.264");
	add_button(CMD_HW_DECODE, false, A.hw_decode ? "Çözme: ekran kartı" : "Çözme: işlemci");
	add_button(CMD_FULLSCREEN, A.fullscreen, "Tam ekran");
	add_button(CMD_ONTOP, A.on_top, "Üstte");
	add_button(CMD_PAIR, false, "Yeni cihaz");
}

static bool bar_wanted(void)
{
	SDL_LockMutex(A.frame_mutex);
	bool mirroring = A.have_frame;
	SDL_UnlockMutex(A.frame_mutex);
	return !mirroring || (A.bar_until && !SDL_TICKS_PASSED(SDL_GetTicks(), A.bar_until));
}

static void draw_bar(int ow, int oh)
{
	int ww = 0, wh = 0;
	SDL_GetWindowSize(A.win, &ww, &wh);
	int px = ww > 0 ? 15 * ow / ww : 15;
	font_open(px < 10 ? 10 : px);
	build_buttons();

	int pad = A.font_px * 2 / 3, bh = A.font_px * 2, gap = A.font_px / 2;
	/* the buttons from the left, in as many rows as they need */
	int x = gap, rows = 1;
	for (int i = 0; i < button_count; i++) {
		struct text_entry *t = text_get(buttons[i].text);
		int bw = (t ? t->w : A.font_px * 4) + pad * 2;
		if (x + bw + gap > ow && x > gap) {
			x = gap;
			rows++;
		}
		buttons[i].r.x = x;
		buttons[i].r.y = rows - 1; /* (the row, for now) */
		buttons[i].r.w = bw;
		buttons[i].r.h = bh;
		x += bw + gap;
	}
	int status_h = A.font_px * 2;
	int bar_h = status_h + rows * (bh + gap);
	int y0 = oh - bar_h;
	for (int i = 0; i < button_count; i++)
		buttons[i].r.y = y0 + status_h + buttons[i].r.y * (bh + gap);

	SDL_SetRenderDrawBlendMode(A.ren, SDL_BLENDMODE_BLEND);
	SDL_Rect bar = {0, y0, ow, bar_h};
	SDL_SetRenderDrawColor(A.ren, 12, 14, 20, 215);
	SDL_RenderFillRect(A.ren, &bar);

	char status[600];
	status_text(status, sizeof(status));
	struct text_entry *st = text_get(status);
	if (st) {
		SDL_Rect dst = {gap, y0 + (status_h - st->h) / 2, st->w, st->h};
		if (dst.w > ow - 2 * gap) {
			SDL_Rect src = {0, 0, ow - 2 * gap, st->h};
			dst.w = src.w;
			SDL_RenderCopy(A.ren, st->tex, &src, &dst);
		} else {
			SDL_RenderCopy(A.ren, st->tex, NULL, &dst);
		}
	}
	for (int i = 0; i < button_count; i++) {
		struct button *b = &buttons[i];
		if (b->on)
			SDL_SetRenderDrawColor(A.ren, 200, 60, 60, 255);
		else
			SDL_SetRenderDrawColor(A.ren, 58, 64, 78, 255);
		SDL_RenderFillRect(A.ren, &b->r);
		struct text_entry *t = text_get(b->text);
		if (t) {
			SDL_Rect dst = {b->r.x + (b->r.w - t->w) / 2, b->r.y + (b->r.h - t->h) / 2, t->w, t->h};
			SDL_RenderCopy(A.ren, t->tex, NULL, &dst);
		}
	}
	SDL_SetRenderDrawBlendMode(A.ren, SDL_BLENDMODE_NONE);
}

/* the button at this place of the window (window coordinates), or CMD_NONE */
static int button_at(int mx, int my)
{
	int ow = 0, oh = 0, ww = 0, wh = 0;
	if (!A.bar_drawn)
		return CMD_NONE;
	SDL_GetRendererOutputSize(A.ren, &ow, &oh);
	SDL_GetWindowSize(A.win, &ww, &wh);
	if (ww <= 0 || wh <= 0)
		return CMD_NONE;
	SDL_Point p = {mx * ow / ww, my * oh / wh};
	for (int i = 0; i < button_count; i++) {
		if (SDL_PointInRect(&p, &buttons[i].r))
			return buttons[i].cmd;
	}
	return CMD_NONE;
}

/* the pairing code, as large seven-segment digits */
static void draw_pin(const char *pin, int ow, int oh)
{
	static const uint8_t segments[10] = {0x3f, 0x06, 0x5b, 0x4f, 0x66, 0x6d, 0x7d, 0x07, 0x7f, 0x6f};
	int count = (int)strlen(pin);
	if (count <= 0)
		return;
	int dh = oh / 3;
	if (dh * count * 7 / 10 > ow * 8 / 10)
		dh = ow * 8 / 10 * 10 / (count * 7);
	int dw = dh / 2, gap = dh / 5, t = dh / 9 > 2 ? dh / 9 : 2;
	int total = count * dw + (count - 1) * gap;
	int x0 = (ow - total) / 2, y0 = (oh - dh) / 2;
	SDL_SetRenderDrawColor(A.ren, 18, 22, 30, 255);
	SDL_RenderClear(A.ren);
	SDL_SetRenderDrawColor(A.ren, 240, 240, 245, 255);
	for (int i = 0; i < count; i++) {
		if (pin[i] < '0' || pin[i] > '9')
			continue;
		uint8_t s = segments[pin[i] - '0'];
		int x = x0 + i * (dw + gap), half = dh / 2;
		SDL_Rect r[7] = {
			{x, y0, dw, t},                       /* a: top */
			{x + dw - t, y0, t, half},            /* b: top right */
			{x + dw - t, y0 + half, t, dh - half}, /* c: bottom right */
			{x, y0 + dh - t, dw, t},              /* d: bottom */
			{x, y0 + half, t, dh - half},         /* e: bottom left */
			{x, y0, t, half},                     /* f: top left */
			{x, y0 + half - t / 2, dw, t},        /* g: middle */
		};
		for (int k = 0; k < 7; k++) {
			if (s & (1 << k))
				SDL_RenderFillRect(A.ren, &r[k]);
		}
	}
}

static void draw(void)
{
	int ow = 0, oh = 0;
	SDL_GetRendererOutputSize(A.ren, &ow, &oh);

	char pin[8];
	SDL_LockMutex(A.mutex);
	snprintf(pin, sizeof(pin), "%s", A.pin);
	SDL_UnlockMutex(A.mutex);

	/* the newest picture into the texture (YUV: the graphics card makes the colours and the size) */
	bool got = false, full = false, bt709 = true, show;
	SDL_LockMutex(A.frame_mutex);
	if (A.have_frame && A.frame_dirty && A.pending->data[0]) {
		av_frame_unref(A.upload);
		av_frame_move_ref(A.upload, A.pending);
		full = A.pending_full;
		bt709 = A.pending_709;
		A.frame_dirty = false;
		got = true;
	}
	show = A.have_frame;
	SDL_UnlockMutex(A.frame_mutex);
	if (got) {
		AVFrame *f = A.upload;
		Uint32 format = f->format == AV_PIX_FMT_NV12 ? SDL_PIXELFORMAT_NV12 : SDL_PIXELFORMAT_IYUV;
		int mode = full ? SDL_YUV_CONVERSION_JPEG : bt709 ? SDL_YUV_CONVERSION_BT709 : SDL_YUV_CONVERSION_BT601;
		if (!A.tex || A.tex_w != f->width || A.tex_h != f->height || A.tex_format != format || A.tex_mode != mode) {
			if (A.tex)
				SDL_DestroyTexture(A.tex);
			SDL_SetYUVConversionMode((SDL_YUV_CONVERSION_MODE)mode);
			A.tex = SDL_CreateTexture(A.ren, format, SDL_TEXTUREACCESS_STREAMING, f->width, f->height);
			A.tex_w = f->width;
			A.tex_h = f->height;
			A.tex_format = format;
			A.tex_mode = mode;
		}
		if (A.tex && format == SDL_PIXELFORMAT_NV12)
			SDL_UpdateNVTexture(A.tex, NULL, f->data[0], f->linesize[0], f->data[1], f->linesize[1]);
		else if (A.tex)
			SDL_UpdateYUVTexture(A.tex, NULL, f->data[0], f->linesize[0], f->data[1], f->linesize[1], f->data[2],
					     f->linesize[2]);
		av_frame_unref(f);
	}

	SDL_SetRenderDrawColor(A.ren, 0, 0, 0, 255);
	SDL_RenderClear(A.ren);
	if (pin[0]) {
		draw_pin(pin, ow, oh);
	} else if (show && A.tex && ow > 0 && oh > 0) {
		SDL_Rect dst;
		if ((int64_t)ow * A.tex_h > (int64_t)oh * A.tex_w) {
			dst.h = oh;
			dst.w = (int)((int64_t)oh * A.tex_w / A.tex_h);
		} else {
			dst.w = ow;
			dst.h = (int)((int64_t)ow * A.tex_h / A.tex_w);
		}
		dst.x = (ow - dst.w) / 2;
		dst.y = (oh - dst.h) / 2;
		SDL_RenderCopy(A.ren, A.tex, NULL, &dst);
	}
	A.bar_drawn = bar_wanted();
	if (A.bar_drawn)
		draw_bar(ow, oh);
	SDL_RenderPresent(A.ren);
}

static void change_volume(int delta)
{
	int v = SDL_AtomicGet(&A.volume) + delta;
	v = v < 0 ? 0 : v > 200 ? 200 : v;
	SDL_AtomicSet(&A.volume, v);
	SDL_AtomicSet(&A.muted, 0);
	save_settings();
}

static void open_video_folder(void)
{
#ifdef _WIN32
	wchar_t *w = to_wide(A.video_folder);
	if (w)
		ShellExecuteW(NULL, L"open", w, NULL, NULL, SW_SHOWNORMAL);
	free(w);
#endif
}

static void run_command(int cmd)
{
	bool restart = false;
	switch (cmd) {
	case CMD_FULLSCREEN:
		A.fullscreen = !A.fullscreen;
		SDL_SetWindowFullscreen(A.win, A.fullscreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
		break;
	case CMD_ONTOP:
		A.on_top = !A.on_top;
		SDL_SetWindowAlwaysOnTop(A.win, A.on_top ? SDL_TRUE : SDL_FALSE);
		save_settings();
		break;
	case CMD_MUTE:
		SDL_AtomicSet(&A.muted, !SDL_AtomicGet(&A.muted));
		save_settings();
		break;
	case CMD_VOL_UP:
		change_volume(10);
		break;
	case CMD_VOL_DOWN:
		change_volume(-10);
		break;
	case CMD_VOL_100:
		change_volume(100 - SDL_AtomicGet(&A.volume));
		break;
	case CMD_RECORD:
		rec_toggle();
		break;
	case CMD_REC_ENCODER:
		/* (for the next recording) */
		A.rec_gpu = !A.rec_gpu;
		save_settings();
		break;
	case CMD_OPEN_FOLDER:
		open_video_folder();
		break;
	case CMD_PAIR:
		SDL_LockMutex(A.mutex);
		A.pair_requested = true;
		if (A.state == MIRROR_PAIRING_OFF)
			A.state = MIRROR_LISTENING;
		SDL_UnlockMutex(A.mutex);
		if (A.proc)
			proc_write(A.proc, "allow_pairing 1\n");
		break;
	case CMD_RES_NEXT:
		if (A.height < 1080)
			A.width = 1920, A.height = 1080;
		else if (A.height < 1440)
			A.width = 2560, A.height = 1440;
		else if (A.height < 2160)
			A.width = 3840, A.height = 2160;
		else
			A.width = 1280, A.height = 720;
		restart = true;
		break;
	case CMD_FPS_NEXT:
		A.fps = A.fps >= 60 ? 30 : 60;
		restart = true;
		break;
	case CMD_CODEC_NEXT:
		A.h265 = !A.h265;
		restart = true;
		break;
	case CMD_HW_DECODE:
		A.hw_decode = !A.hw_decode;
		restart = true;
		break;
	default:
		break;
	}
	if (restart) {
		/* the iPad learns the new size / rate / codec when it connects again: the receiver restarts a moment
		 * after the last click */
		save_settings();
		A.apply_at = SDL_GetTicks() + 1500;
	}
	update_title();
}

static int key_command(SDL_Keycode key)
{
	switch (key) {
	case SDLK_f:
	case SDLK_F11:
		return CMD_FULLSCREEN;
	case SDLK_ESCAPE:
		return A.fullscreen ? CMD_FULLSCREEN : CMD_NONE;
	case SDLK_m:
		return CMD_MUTE;
	case SDLK_UP:
		return CMD_VOL_UP;
	case SDLK_DOWN:
		return CMD_VOL_DOWN;
	case SDLK_r:
		return CMD_RECORD;
	case SDLK_p:
		return CMD_PAIR;
	case SDLK_t:
		return CMD_ONTOP;
	default:
		return CMD_NONE;
	}
}

#ifdef _WIN32
/* Without a rule Windows Firewall drops what the iPad sends (the iPad then sees the name but cannot connect and no
 * code is shown). Once, with the user's consent (UAC): rules for the receiver's fixed ports, and the "block" rules
 * Windows made for the receiver if its own question was dismissed are removed. */
static void ensure_firewall_rule(void)
{
	if (proc_run("netsh", "advfirewall firewall show rule name=\"Relios Mirror\"", 10000) == 0)
		return;
	SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_INFORMATION, APP_NAME,
				 "iPad'in bağlanabilmesi için Windows Güvenlik Duvarı'nda izin gerekiyor.\n\n"
				 "Tamam'a bastıktan sonra çıkacak Windows sorusunda \"Evet\"i seçin (bir kez sorulur).",
				 A.win);
	char cmd[6000];
	snprintf(cmd, sizeof(cmd),
		 "/c \"netsh advfirewall firewall delete rule name=all program=\"%s\""
		 " & netsh advfirewall firewall add rule name=\"Relios Mirror\" dir=in action=allow protocol=TCP localport=7000,7100"
		 " & netsh advfirewall firewall add rule name=\"Relios Mirror\" dir=in action=allow protocol=UDP localport=5353,6000,6001,7011\"",
		 A.exe);
	wchar_t *w = to_wide(cmd);
	if (!w)
		return;
	SHELLEXECUTEINFOW info;
	memset(&info, 0, sizeof(info));
	info.cbSize = sizeof(info);
	info.fMask = SEE_MASK_NOCLOSEPROCESS;
	info.lpVerb = L"runas";
	info.lpFile = L"cmd.exe";
	info.lpParameters = w;
	info.nShow = SW_HIDE;
	if (ShellExecuteExW(&info) && info.hProcess) {
		WaitForSingleObject(info.hProcess, 20000);
		CloseHandle(info.hProcess);
		app_log("firewall rules asked for");
	} else {
		app_log("firewall rules not added (error %lu)", (unsigned long)GetLastError());
	}
	free(w);
}
#endif

int main(int argc, char *argv[])
{
	(void)argc;
	(void)argv;
	SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "1");
	if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_EVENTS) != 0) {
		SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, APP_NAME, SDL_GetError(), NULL);
		return 1;
	}
	TTF_Init();
	A.mutex = SDL_CreateMutex();
	A.frame_mutex = SDL_CreateMutex();
	R.mutex = SDL_CreateMutex();
	log_mutex = SDL_CreateMutex();
	A.event_type = SDL_RegisterEvents(1);
	A.pending = av_frame_alloc();
	A.upload = av_frame_alloc();
	if (!A.pending || !A.upload)
		return 1;

	char *base = SDL_GetBasePath();
	snprintf(A.exe, sizeof(A.exe), "%s" RECEIVER_EXE, base ? base : "");
	SDL_free(base);

	if (!enter_config_folder()) {
		SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, APP_NAME, "Ayar klasörü açılamadı.", NULL);
		return 1;
	}
	log_file = fopen("log.txt", "w");
	app_log("Relios Mirror started; receiver: %s", A.exe);
	av_log_set_level(AV_LOG_ERROR);
	load_settings();
	find_video_folder();

	A.win = SDL_CreateWindow(APP_NAME, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 1280, 720,
				 SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);
	if (A.win)
		A.ren = SDL_CreateRenderer(A.win, -1, SDL_RENDERER_ACCELERATED);
	if (A.win && !A.ren)
		A.ren = SDL_CreateRenderer(A.win, -1, 0);
	if (!A.win || !A.ren) {
		SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, APP_NAME, SDL_GetError(), NULL);
		return 1;
	}
	if (A.on_top)
		SDL_SetWindowAlwaysOnTop(A.win, SDL_TRUE);

#ifdef _WIN32
	ensure_firewall_rule();
#endif
	session_start();
	update_title();
	draw();

	bool quit = false;
	A.stats_at = SDL_GetTicks();
	while (!quit) {
		SDL_Event e;
		bool redraw = false, retitle = false;
		if (SDL_WaitEventTimeout(&e, 100)) {
			do {
				int cmd = CMD_NONE;
				if (e.type == SDL_QUIT) {
					quit = true;
				} else if (e.type == A.event_type) {
					if (e.user.code == EV_FRAME) {
						SDL_AtomicSet(&A.frame_event, 0);
						redraw = true;
					} else if (e.user.code == EV_STATE) {
						redraw = retitle = true;
					} else if (e.user.code == EV_ENDED &&
						   (int)(intptr_t)e.user.data1 == A.generation && A.proc) {
						/* the receiver ended (the iPad left, or an error): a new one after a moment */
						SDL_LockMutex(A.mutex);
						bool failed = A.state == MIRROR_ERROR;
						SDL_UnlockMutex(A.mutex);
						session_stop();
						A.restart_at = SDL_GetTicks() + (failed ? 5000 : 700);
						redraw = retitle = true;
					}
				} else if (e.type == SDL_WINDOWEVENT) {
					if (e.window.event == SDL_WINDOWEVENT_LEAVE)
						A.bar_until = 0;
					redraw = true;
				} else if (e.type == SDL_MOUSEMOTION) {
					A.bar_until = SDL_GetTicks() + 2500;
					if (!A.bar_drawn)
						redraw = true;
				} else if (e.type == SDL_KEYDOWN &&
					   (!e.key.repeat || e.key.keysym.sym == SDLK_UP || e.key.keysym.sym == SDLK_DOWN)) {
					cmd = key_command(e.key.keysym.sym);
				} else if (e.type == SDL_MOUSEBUTTONDOWN && e.button.button == SDL_BUTTON_LEFT) {
					A.bar_until = SDL_GetTicks() + 2500;
					cmd = button_at(e.button.x, e.button.y);
					if (cmd == CMD_NONE && e.button.clicks == 2)
						cmd = CMD_FULLSCREEN;
					redraw = true;
				}
				if (cmd != CMD_NONE) {
					run_command(cmd);
					redraw = true;
				}
			} while (!quit && SDL_PollEvent(&e));
		}

		Uint32 now = SDL_GetTicks();
		if (!quit && A.apply_at && SDL_TICKS_PASSED(now, A.apply_at)) {
			A.apply_at = 0;
			session_start();
			redraw = retitle = true;
		}
		if (!quit && A.restart_at && SDL_TICKS_PASSED(now, A.restart_at) && !A.proc) {
			session_start();
			retitle = true;
		}
		if (now - A.stats_at >= 1000) {
			double seconds = (double)(now - A.stats_at) / 1000.0;
			SDL_LockMutex(A.mutex);
			A.mbit = (double)A.win_bytes * 8.0 / seconds / 1e6;
			A.fps_now = (double)A.win_frames / seconds;
			A.win_bytes = A.win_frames = 0;
			SDL_UnlockMutex(A.mutex);
			A.stats_at = now;
			retitle = true;
			if (A.bar_drawn)
				redraw = true;
		}
		if (A.bar_drawn != bar_wanted())
			redraw = true;
		if (retitle)
			update_title();
		if (redraw)
			draw();
	}

	session_stop();
	SDL_LockMutex(R.mutex);
	rec_close_locked();
	SDL_UnlockMutex(R.mutex);
	if (A.adev)
		SDL_CloseAudioDevice(A.adev);
	save_settings();
	app_log("ended");
	texts_clear();
	if (A.tex)
		SDL_DestroyTexture(A.tex);
	SDL_DestroyRenderer(A.ren);
	SDL_DestroyWindow(A.win);
	SDL_Quit();
	return 0;
}

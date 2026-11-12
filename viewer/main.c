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
#include <shlobj.h>
#include <tlhelp32.h>
#else
#include <sys/stat.h>
#include <unistd.h>
#endif

#include <math.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <SDL.h>
#include <SDL_ttf.h>
#ifdef _WIN32
#include <SDL_syswm.h>
#endif

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
	CMD_REC_FORMAT,
	CMD_REC_BITRATE,
	CMD_REC_MODE,
	CMD_HW_DECODE,
	CMD_SETTINGS,
	CMD_TAB,
	CMD_SCREENSHOT,
	CMD_AUDIO_DEVICE,
	CMD_VCAM,
	CMD_ASPECT,
	CMD_HIDE,
	CMD_CHOOSE_FOLDER,
	CMD_REC_AUDIO,
	CMD_AUTOSTART,
	CMD_VISIBLE,
	CMD_RESET,
	CMD_VCAM_REMOVE,
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
	bool rec_audio;      /* the sound goes into the recording */
	bool to_tray;        /* a minimized window goes to the notification area */
	bool in_tray;
	bool res_auto;       /* the size is left to the iPad (the largest is asked for) */
	bool visible;        /* announced on the network (in the Apple device's Screen Mirroring list) */
	bool hide_connected; /* the name leaves the Screen Mirroring lists while a paired device is mirroring */
	bool keep_awake;     /* the screen is kept on (while something is mirrored) */
	char custom_folder[1024]; /* where recordings go, if chosen */
	SDL_atomic_t fit169; /* a landscape picture is spread to 16:9 (window, recording, virtual camera) */
	bool rec_gpu;   /* record with the graphics card's encoder */
	int rec_format; /* 0: .mp4, 1: .mov (QuickTime), 2: .mkv */
	int rec_mode;   /* 0: by quality, 1: CBR, 2: VBR */
	int rec_kbit;   /* the recording's bit rate for CBR / VBR */
	SDL_Texture *logo;
	int logo_w, logo_h;
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
	AVFrame *last;   /* the newest picture, kept for a screenshot */
	bool last_full, last_709;
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

	char audio_device[256]; /* "": the default one (under mutex) */
	SDL_atomic_t audio_reopen;
	SDL_atomic_t register_ok; /* a new device may be remembered (pairing was switched on here) */
	SDL_atomic_t vcam_on;
	bool camera_saved;       /* the camera switch as it is remembered */
	bool camera_arming;      /* switched on again at the start: remembered again only once it runs */
	SDL_atomic_t camera_ok;  /* the camera has started */
	Uint32 hidden_at;        /* when the receiver took its name off the network (SDL ticks; 0: it is announced) */
	SDL_atomic_t hide_broke; /* the mirroring stopped right after that */
	int hover;               /* the button under the mouse (its index), for redrawing when it changes */
	char notice[300]; /* a short message shown for a few seconds at the top of the window (under mutex) */
	Uint32 notice_until;
	bool notice_opens_folder; /* a click on it opens the recordings folder */
	bool notice_drawn;
	SDL_Rect notice_rect;
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
	char encoder[48];
	int fps;
	AVCodecContext *aenc; /* the sound: AAC */
	AVFrame *aframe;
	float *abuf; /* interleaved stereo waiting for a full AAC frame */
	int abuf_n, abuf_cap;
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
	static size_t log_bytes;
	static const size_t log_limit = 5 * 1024 * 1024;
	if (!log_file)
		return;
	if (log_mutex)
		SDL_LockMutex(log_mutex);
	if (log_bytes > log_limit) {
		if (log_mutex)
			SDL_UnlockMutex(log_mutex);
		return;
	}
	log_bytes += strlen(text) + 10;
	if (log_bytes > log_limit)
		snprintf(text, sizeof(text), "(the log is full; nothing more is written until the program starts again)");
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

static void push_event(int code);

static void set_notice(const char *text, bool opens_folder)
{
	SDL_LockMutex(A.mutex);
	snprintf(A.notice, sizeof(A.notice), "%s", text);
	A.notice_opens_folder = opens_folder;
	A.notice_until = SDL_GetTicks() + 6000;
	SDL_UnlockMutex(A.mutex);
	push_event(EV_STATE);
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

#ifdef _WIN32
/* a program of Windows itself by its full path: never one of the same name that someone put in another folder */
static void system_program(const char *name, char *out, size_t size)
{
	wchar_t dir[MAX_PATH];
	char dir8[800] = "C:\\Windows\\System32";
	UINT n = GetSystemDirectoryW(dir, MAX_PATH);
	if (n > 0 && n < MAX_PATH)
		WideCharToMultiByte(CP_UTF8, 0, dir, -1, dir8, (int)sizeof(dir8), NULL, NULL);
	snprintf(out, size, "%s\\%s", dir8, name);
}

/* runs a program with administrator rights (Windows asks the user: UAC) and waits; its exit code, or -1 */
static int run_elevated(const char *program_utf8, const char *args_utf8)
{
	wchar_t *program = to_wide(program_utf8), *args = to_wide(args_utf8);
	wchar_t dir[MAX_PATH] = L"";
	int code = -1;
	GetSystemDirectoryW(dir, MAX_PATH);
	if (program && args) {
		SHELLEXECUTEINFOW info;
		memset(&info, 0, sizeof(info));
		info.cbSize = sizeof(info);
		info.fMask = SEE_MASK_NOCLOSEPROCESS;
		info.lpVerb = L"runas";
		info.lpFile = program;
		info.lpParameters = args;
		info.lpDirectory = dir;
		info.nShow = SW_HIDE;
		if (ShellExecuteExW(&info) && info.hProcess) {
			DWORD exit_code = 1;
			WaitForSingleObject(info.hProcess, 30000);
			if (GetExitCodeProcess(info.hProcess, &exit_code))
				code = (int)exit_code;
			CloseHandle(info.hProcess);
		}
	}
	free(program);
	free(args);
	return code;
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
	if (A.custom_folder[0]) {
		SDL_LockMutex(A.mutex);
		snprintf(A.video_folder, sizeof(A.video_folder), "%s", A.custom_folder);
		SDL_UnlockMutex(A.mutex);
		make_folder(A.custom_folder);
		return;
	}
	char folder[1024];

#ifdef _WIN32
	char home[800] = "";
	const wchar_t *w = _wgetenv(L"USERPROFILE");
	if (w)
		WideCharToMultiByte(CP_UTF8, 0, w, -1, home, (int)sizeof(home), NULL, NULL);
	snprintf(folder, sizeof(folder), "%s\\Videos", home);
	make_folder(folder);
	snprintf(folder, sizeof(folder), "%s\\Videos\\" CONFIG_FOLDER, home);
#else
	const char *home = getenv("HOME");
	snprintf(folder, sizeof(folder), "%s/" CONFIG_FOLDER, home ? home : ".");
#endif
	make_folder(folder);
	SDL_LockMutex(A.mutex);
	snprintf(A.video_folder, sizeof(A.video_folder), "%s", folder);
	SDL_UnlockMutex(A.mutex);
}

/* (the folder may be changed by the window thread while another thread starts a file) */
static void video_folder(char *out, size_t size)
{
	SDL_LockMutex(A.mutex);
	snprintf(out, size, "%s", A.video_folder);
	SDL_UnlockMutex(A.mutex);
}


/* (a path anywhere, whatever letters it has) */
static bool file_exists_utf8(const char *path)
{
	SDL_RWops *rw = SDL_RWFromFile(path, "rb");
	if (rw)
		SDL_RWclose(rw);
	return rw != NULL;
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
	A.res_auto = false;
	A.rec_audio = true;
	A.to_tray = false;
	A.hide_connected = true; /* (on unless switched off; it switches itself off if it ends a mirroring) */
	A.camera_saved = false;
	A.visible = true;
	SDL_AtomicSet(&A.vcam_on, 0);
	A.rec_gpu = true;
	A.rec_format = 1;
	A.rec_mode = 0;
	A.rec_kbit = 15000;
	A.hw_decode = true;
	SDL_AtomicSet(&A.volume, 100);
	SDL_AtomicSet(&A.muted, 0);
	FILE *f = fopen("ayarlar.txt", "r");
	if (!f)
		return;
	char line[1400];
	int version = 1;
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
		} else if (sscanf(line, "sanal_kamera=%d", &a) == 1) {
			A.camera_saved = a != 0;
		} else if (sscanf(line, "listede_goster=%d", &a) == 1) {
			A.visible = a != 0;
		} else if (sscanf(line, "kayit_ses=%d", &a) == 1) {
			A.rec_audio = a != 0;
		} else if (sscanf(line, "tepsi=%d", &a) == 1) {
			A.to_tray = a != 0;
		} else if (sscanf(line, "cozunurluk_oto=%d", &a) == 1) {
			A.res_auto = a != 0;
		} else if (sscanf(line, "bagliyken_gizle=%d", &a) == 1) {
			A.hide_connected = a != 0;
		} else if (!strncmp(line, "kayit_klasoru=", 14)) {
			snprintf(A.custom_folder, sizeof(A.custom_folder), "%s", line + 14);
			A.custom_folder[strcspn(A.custom_folder, "\r\n")] = '\0';
#ifdef _WIN32
			{
				const char *c = A.custom_folder;
				bool drive = ((c[0] >= 'A' && c[0] <= 'Z') || (c[0] >= 'a' && c[0] <= 'z')) && c[1] == ':' &&
					     (c[2] == '\\' || c[2] == '/');
				bool share = c[0] == '\\' && c[1] == '\\';
				if (!drive && !share)
					A.custom_folder[0] = '\0';
			}
#endif
		} else if (sscanf(line, "surum=%d", &a) == 1) {
			version = a;
		} else if (sscanf(line, "oran169=%d", &a) == 1) {
			SDL_AtomicSet(&A.fit169, a != 0);
		} else if (!strncmp(line, "ses_aygiti=", 11)) {
			snprintf(A.audio_device, sizeof(A.audio_device), "%s", line + 11);
			A.audio_device[strcspn(A.audio_device, "\r\n")] = '\0';
		} else if (sscanf(line, "kayit_bicim=%d", &a) == 1 && a >= 0 && a <= 2) {
			A.rec_format = a;
		} else if (sscanf(line, "kayit_mod=%d", &a) == 1 && a >= 0 && a <= 2) {
			A.rec_mode = a;
		} else if (sscanf(line, "kayit_kbit=%d", &a) == 1 && a >= 500 && a <= 200000) {
			A.rec_kbit = a;
		}
	}
	fclose(f);
	if (version < 2) {
		/* (settings saved before these became the defaults: the graphics card and .mov once) */
		A.rec_gpu = true;
		A.rec_format = 1;
	}
	if (version < 4) {
		/* (1920x1080 is the default; "automatic" is gone) */
		A.width = 1920;
		A.height = 1080;
	}
	A.res_auto = false;
	if (version < 5)
		A.hide_connected = true; /* (earlier builds saved it without having the option: on, as at a first start) */
	A.visible = true; /* (always announced while the program runs: a hidden receiver cannot be connected to) */
}

static void save_settings(void)
{
	FILE *f = fopen("ayarlar.txt", "w");
	if (!f)
		return;
	fprintf(f, "cozunurluk=%dx%d\nfps=%d\nh265=%d\nses=%d\nsessiz=%d\nustte=%d\nkayit_gpu=%d\ngpu_cozme=%d\nkayit_bicim=%d\nkayit_mod=%d\nkayit_kbit=%d\n", A.width,
		A.height, A.fps, A.h265 ? 1 : 0, SDL_AtomicGet(&A.volume), SDL_AtomicGet(&A.muted), A.on_top ? 1 : 0,
		A.rec_gpu ? 1 : 0, A.hw_decode ? 1 : 0, A.rec_format, A.rec_mode, A.rec_kbit);
	fprintf(f, "ses_aygiti=%s\noran169=%d\ncozunurluk_oto=%d\nbagliyken_gizle=%d\nkayit_klasoru=%s\nkayit_ses=%d\ntepsi=%d\nlistede_goster=%d\nsurum=5\n", A.audio_device,
		SDL_AtomicGet(&A.fit169), A.res_auto ? 1 : 0, A.hide_connected ? 1 : 0, A.custom_folder, A.rec_audio ? 1 : 0,
		A.to_tray ? 1 : 0, A.visible ? 1 : 0);
	fprintf(f, "sanal_kamera=%d\n", A.camera_saved ? 1 : 0);
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

/* "<public key (base64)>\t<device id>\t<name>": nothing that could start a line of its own or pass for another key */
static bool register_line_ok(const char *line, size_t len)
{
	if (len < 20 || len > 2000 || strlen(line) != len)
		return false;
	const char *tab = strchr(line, '\t');
	if (!tab || tab - line < 16 || tab - line > 200)
		return false;
	for (const char *p = line; p < tab; p++) {
		bool ok = (*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') || *p == '+' ||
			  *p == '/' || *p == '=';
		if (!ok)
			return false;
	}
	for (const char *p = tab; *p; p++) {
		if ((unsigned char)*p < 0x20 && *p != '\t')
			return false;
	}
	return true;
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

static SDL_atomic_t rec_on; /* recording is switched on (read by the reading thread without waiting) */

static void rec_drain(void)
{
	while (avcodec_receive_packet(R.enc, R.pkt) == 0) {
		av_packet_rescale_ts(R.pkt, R.enc->time_base, R.vst->time_base);
		R.pkt->stream_index = R.vst->index;
		av_interleaved_write_frame(R.oc, R.pkt);
		av_packet_unref(R.pkt);
	}
}

static const char *const rec_ext[] = {"mp4", "mov", "mkv"};
static const char *const rec_muxer[] = {"mp4", "mov", "matroska"};
static const char *const rec_format_name[] = {"MP4", "MOV", "MKV"};

static void rec_drain_audio(void)
{
	while (avcodec_receive_packet(R.aenc, R.pkt) == 0) {
		av_packet_rescale_ts(R.pkt, R.aenc->time_base, R.ast->time_base);
		R.pkt->stream_index = R.ast->index;
		av_interleaved_write_frame(R.oc, R.pkt);
		av_packet_unref(R.pkt);
	}
}

static void rec_close_locked(void)
{
	if (R.oc && R.enc && R.header_written) {
		if (R.aenc) {
			avcodec_send_frame(R.aenc, NULL);
			rec_drain_audio();
		}
		avcodec_send_frame(R.enc, NULL);
		rec_drain();
		av_write_trailer(R.oc);
		app_log("recording closed: %s", R.path);
		const char *name = strrchr(R.path, PATH_SEP[0]);
		char text[300];
		snprintf(text, sizeof(text), "Kayıt kaydedildi: %s  •  Klasörü açmak için tıklayın", name ? name + 1 : R.path);
		set_notice(text, true);
	}
	if (R.oc && R.oc->pb)
		avio_closep(&R.oc->pb);
	if (R.oc)
		avformat_free_context(R.oc);
	R.oc = NULL;
	R.header_written = false;
	R.vst = R.ast = NULL;
	avcodec_free_context(&R.enc);
	avcodec_free_context(&R.aenc);
	av_frame_free(&R.aframe);
	R.abuf_n = 0;
	av_frame_free(&R.frame);
	av_packet_free(&R.pkt);
	sws_freeContext(R.sws);
	R.sws = NULL;
}

static bool rec_open_locked(const AVFrame *f, bool full, bool bt709, uint64_t t0)
{
	char stamp[40] = "kayit";
	time_t now = time(NULL);
	struct tm *tm_now = localtime(&now);
	if (tm_now)
		strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", tm_now);
	char folder[1024];
	video_folder(folder, sizeof(folder));
	snprintf(R.path, sizeof(R.path), "%s" PATH_SEP "ReliosMirror-%s.%s", folder, stamp, rec_ext[A.rec_format]);

	R.w = f->width;
	R.h = f->height;
	R.fps = A.fps;
	if (avformat_alloc_output_context2(&R.oc, NULL, rec_muxer[A.rec_format], R.path) < 0 || !R.oc)
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
		if (SDL_AtomicGet(&A.fit169) && f->width > f->height)
			R.enc->width = (f->height * 16 / 9) & ~1; /* landscape: spread to 16:9 */
		R.enc->pix_fmt = gpu ? AV_PIX_FMT_NV12 : AV_PIX_FMT_YUV420P;
		R.enc->time_base = (AVRational){1, R.fps}; /* a constant frame rate: one tick per frame */
		R.enc->framerate = (AVRational){R.fps, 1};
		R.enc->gop_size = 120;
		R.enc->color_range = full ? AVCOL_RANGE_JPEG : AVCOL_RANGE_MPEG;
		R.enc->colorspace = bt709 ? AVCOL_SPC_BT709 : AVCOL_SPC_SMPTE170M;
		if (R.oc->oformat->flags & AVFMT_GLOBALHEADER)
			R.enc->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
		AVDictionary *opts = NULL;
		if (!gpu)
			av_dict_set(&opts, "preset", "veryfast", 0);
		if (A.rec_mode == 1) {
			/* CBR: a constant bit rate */
			R.enc->bit_rate = (int64_t)A.rec_kbit * 1000;
			R.enc->rc_max_rate = R.enc->bit_rate;
			R.enc->rc_buffer_size = A.rec_kbit * 1000;
			if (n == 0)
				av_dict_set(&opts, "rc", "cbr", 0);
		} else if (A.rec_mode == 2) {
			/* VBR: this bit rate on average, up to one and a half times it where the picture needs it */
			R.enc->bit_rate = (int64_t)A.rec_kbit * 1000;
			R.enc->rc_max_rate = R.enc->bit_rate * 3 / 2;
			R.enc->rc_buffer_size = A.rec_kbit * 2000;
			if (n == 0)
				av_dict_set(&opts, "rc", "vbr", 0);
		} else if (gpu) {
			R.enc->bit_rate = (int64_t)R.enc->width * R.enc->height * R.fps / 10; /* 1080p60: ~12 Mbit/s */
		} else {
			av_dict_set(&opts, "crf", "20", 0);
		}
		int ret = avcodec_open2(R.enc, codec, &opts);
		av_dict_free(&opts);
		if (ret < 0) {
			app_log("recording: the encoder %s did not open (%d)", names[n], ret);
			avcodec_free_context(&R.enc);
		} else {
			snprintf(R.encoder, sizeof(R.encoder), "%s, %s", gpu ? "Ekran Kartı" : "İşlemci", rec_format_name[A.rec_format]);
		}
	}
	if (!R.enc) {
		app_log("recording: no H.264 encoder could be used");
		return false;
	}

	R.vst = avformat_new_stream(R.oc, NULL);
	if (!R.vst)
		return false;
	R.vst->time_base = R.enc->time_base;
	if (avcodec_parameters_from_context(R.vst->codecpar, R.enc) < 0)
		return false;
	/* the sound: AAC, 44.1 kHz stereo (what every player and editor reads in .mp4 / .mov / .mkv) */
	const AVCodec *acodec = A.rec_audio ? avcodec_find_encoder(AV_CODEC_ID_AAC) : NULL;
	if (A.rec_audio && !acodec)
		return false;
	if (acodec) {
	R.ast = avformat_new_stream(R.oc, NULL);
	if (!R.ast)
		return false;
	R.aenc = avcodec_alloc_context3(acodec);
	R.aframe = av_frame_alloc();
	if (!R.aenc || !R.aframe)
		return false;
	R.aenc->sample_rate = 44100;
	av_channel_layout_default(&R.aenc->ch_layout, 2);
	R.aenc->sample_fmt = AV_SAMPLE_FMT_FLTP;
	R.aenc->bit_rate = 192000;
	R.aenc->time_base = (AVRational){1, 44100};
	if (R.oc->oformat->flags & AVFMT_GLOBALHEADER)
		R.aenc->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
	if (avcodec_open2(R.aenc, acodec, NULL) < 0) {
		app_log("recording: the AAC encoder did not open");
		return false;
	}
	if (avcodec_parameters_from_context(R.ast->codecpar, R.aenc) < 0)
		return false;
	R.ast->time_base = (AVRational){1, 44100};
	R.aframe->format = AV_SAMPLE_FMT_FLTP;
	R.aframe->sample_rate = 44100;
	av_channel_layout_default(&R.aframe->ch_layout, 2);
	R.aframe->nb_samples = R.aenc->frame_size > 0 ? R.aenc->frame_size : 1024;
	if (av_frame_get_buffer(R.aframe, 0) < 0)
		return false;
	}
	R.abuf_n = 0;

	R.frame->format = R.enc->pix_fmt;
	R.frame->width = R.enc->width;
	R.frame->height = R.enc->height;
	if (av_frame_get_buffer(R.frame, 0) < 0)
		return false;

	char local[1500];
	snprintf(local, sizeof(local), "file:%s", R.path); /* (always a file on this computer, never an address) */
	if (avio_open(&R.oc->pb, local, AVIO_FLAG_WRITE) < 0) {
		app_log("recording: could not write %s", R.path);
		return false;
	}
	if (avformat_write_header(R.oc, NULL) < 0) {
		app_log("recording: could not start the file");
		return false;
	}
	R.header_written = true;
	R.t0 = t0;
	R.last_vpts = -1;
	R.next_apts = 0;
	R.audio_started = false;
	app_log("recording started: %s (%dx%d, %s)", R.path, R.enc->width, R.enc->height, codec->name);
	return true;
}

static void rec_video(const AVFrame *f, bool full, bool bt709, uint64_t t)
{
	SDL_LockMutex(R.mutex);
	if (R.want && !R.failed) {
		bool opened = false;
		if (R.oc && (R.w != f->width || R.h != f->height))
			rec_close_locked(); /* the iPad turned: a new file */
		if (!R.oc) {
			if (!rec_open_locked(f, full, bt709, t)) {
				rec_close_locked();
				R.failed = true;
			} else {
				opened = true;
			}
		}
		if (R.oc && R.header_written && av_frame_make_writable(R.frame) >= 0) {
			int w = R.enc->width, h = R.enc->height;
			if (R.enc->pix_fmt == AV_PIX_FMT_YUV420P && w == (f->width & ~1) && h == (f->height & ~1) &&
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
			/* constant frame rate: this picture fills every frame time up to now (the iPad sends none while
			 * its screen does not change), and is left out if its frame time is already filled */
			int64_t index = (int64_t)((t > R.t0 ? t - R.t0 : 0) / 1000ull * (uint64_t)R.fps / 1000000ull);
			int64_t first = R.last_vpts + 1;
			if (index - first > R.fps * 2)
				first = index - R.fps * 2;
			for (int64_t k = first; k <= index; k++) {
				R.frame->pts = k;
				if (avcodec_send_frame(R.enc, R.frame) >= 0)
					rec_drain();
				R.last_vpts = k;
			}
		}
		if (opened || R.failed)
			push_event(EV_STATE);
	}
	SDL_UnlockMutex(R.mutex);
}

/* interleaved float samples, as they are played (before the volume) */
static void rec_audio(const float *samples, int frames, int rate, int channels, uint64_t now)
{
	SDL_LockMutex(R.mutex);
	if (R.oc && R.header_written && R.aenc && rate == 44100 && (channels == 1 || channels == 2) && frames > 0) {
		int64_t t = (int64_t)((double)(now > R.t0 ? now - R.t0 : 0) * 44100.0 / 1e9) - frames - R.abuf_n;
		if (t < 0)
			t = 0;
		/* by the sample count; by the clock only at the start and after a gap */
		if (!R.audio_started || t - R.next_apts > 44100 / 5)
			R.next_apts = t > R.next_apts ? t : R.next_apts;
		R.audio_started = true;
		if (R.abuf_n + frames > R.abuf_cap) {
			int cap = (R.abuf_n + frames) * 2;
			float *bigger = realloc(R.abuf, (size_t)cap * 2 * sizeof(float));
			if (bigger) {
				R.abuf = bigger;
				R.abuf_cap = cap;
			}
		}
		if (R.abuf_n + frames <= R.abuf_cap) {
			for (int i = 0; i < frames; i++) {
				R.abuf[(R.abuf_n + i) * 2] = samples[i * channels];
				R.abuf[(R.abuf_n + i) * 2 + 1] = samples[i * channels + (channels == 2 ? 1 : 0)];
			}
			R.abuf_n += frames;
		}
		int size = R.aframe->nb_samples, used = 0;
		while (R.abuf_n - used >= size && av_frame_make_writable(R.aframe) >= 0) {
			float *left = (float *)R.aframe->data[0], *right = (float *)R.aframe->data[1];
			for (int i = 0; i < size; i++) {
				left[i] = R.abuf[(used + i) * 2];
				right[i] = R.abuf[(used + i) * 2 + 1];
			}
			R.aframe->pts = R.next_apts;
			R.next_apts += size;
			used += size;
			if (avcodec_send_frame(R.aenc, R.aframe) >= 0)
				rec_drain_audio();
		}
		if (used > 0) {
			memmove(R.abuf, R.abuf + (size_t)used * 2, (size_t)(R.abuf_n - used) * 2 * sizeof(float));
			R.abuf_n -= used;
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
	SDL_AtomicSet(&rec_on, R.want ? 1 : 0);
	SDL_UnlockMutex(R.mutex);
}

/* Recording has a thread of its own: compressing must never hold back the picture and the sound that are shown
 * and played. The reading thread only hands over what it decoded (if the encoder cannot keep up, pictures are
 * left out of the file, not out of the window). */
enum { REC_VIDEO = 1, REC_AUDIO, REC_CLOSE, REC_SHOT, REC_SYNC };
struct rec_item {
	int kind;
	AVFrame *frame;
	bool full, bt709;
	float *audio;
	int frames, rate, channels;
	uint64_t t;
};
#define REC_QUEUE 96
static struct {
	SDL_mutex *mutex;
	SDL_cond *cond;
	struct rec_item items[REC_QUEUE];
	int head, count, videos;
	bool quit;
	SDL_Thread *thread;
} Q;

static bool rec_push(struct rec_item *item)
{
	bool ok = false;
	SDL_LockMutex(Q.mutex);
	if (Q.count < REC_QUEUE && !(item->kind == REC_VIDEO && Q.videos >= 8)) {
		Q.items[(Q.head + Q.count) % REC_QUEUE] = *item;
		Q.count++;
		if (item->kind == REC_VIDEO)
			Q.videos++;
		ok = true;
		SDL_CondSignal(Q.cond);
	}
	SDL_UnlockMutex(Q.mutex);
	return ok;
}

static void rec_queue_video(const AVFrame *f, bool full, bool bt709)
{
	if (!SDL_AtomicGet(&rec_on) && !SDL_AtomicGet(&A.vcam_on))
		return;
	struct rec_item item;
	memset(&item, 0, sizeof(item));
	item.kind = REC_VIDEO;
	item.frame = av_frame_clone(f);
	item.full = full;
	item.bt709 = bt709;
	item.t = now_ns();
	if (!item.frame || !rec_push(&item))
		av_frame_free(&item.frame);
}

static void rec_queue_audio(const float *samples, int frames, int rate, int channels)
{
	if (!SDL_AtomicGet(&rec_on) || frames <= 0 || channels <= 0)
		return;
	struct rec_item item;
	memset(&item, 0, sizeof(item));
	size_t bytes = (size_t)frames * (size_t)channels * sizeof(float);
	item.kind = REC_AUDIO;
	item.audio = malloc(bytes);
	if (!item.audio)
		return;
	memcpy(item.audio, samples, bytes);
	item.frames = frames;
	item.rate = rate;
	item.channels = channels;
	item.t = now_ns();
	if (!rec_push(&item))
		free(item.audio);
}

/* the session ended: the file is closed after what is still waiting was written */
static void rec_queue_close(void)
{
	struct rec_item item;
	memset(&item, 0, sizeof(item));
	item.kind = REC_CLOSE;
	rec_push(&item);
}

/* ---- a screenshot: the newest picture as a PNG next to the recordings */
static void save_png(const AVFrame *f, bool full, bool bt709)
{
	char path[1400], local[1500], stamp[40] = "goruntu";
	time_t now = time(NULL);
	struct tm *tm_now = localtime(&now);
	if (tm_now)
		strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", tm_now);
	char folder[1024];
	video_folder(folder, sizeof(folder));
	snprintf(path, sizeof(path), "%s" PATH_SEP "ReliosMirror-%s.png", folder, stamp);

	const AVCodec *codec = avcodec_find_encoder(AV_CODEC_ID_PNG);
	AVCodecContext *enc = codec ? avcodec_alloc_context3(codec) : NULL;
	AVFrame *rgb = av_frame_alloc();
	AVPacket *pkt = av_packet_alloc();
	struct SwsContext *sws = NULL;
	AVIOContext *out = NULL;
	bool ok = false;
	if (!enc || !rgb || !pkt)
		goto done;
	enc->width = f->width;
	enc->height = f->height;
	enc->pix_fmt = AV_PIX_FMT_RGB24;
	enc->time_base = (AVRational){1, 25};
	if (avcodec_open2(enc, codec, NULL) < 0)
		goto done;
	rgb->format = AV_PIX_FMT_RGB24;
	rgb->width = f->width;
	rgb->height = f->height;
	if (av_frame_get_buffer(rgb, 0) < 0)
		goto done;
	sws = sws_getContext(f->width, f->height, (enum AVPixelFormat)f->format, f->width, f->height, AV_PIX_FMT_RGB24,
			     SWS_BILINEAR, NULL, NULL, NULL);
	if (!sws)
		goto done;
	sws_setColorspaceDetails(sws, sws_getCoefficients(bt709 ? SWS_CS_ITU709 : SWS_CS_ITU601), full ? 1 : 0,
				 sws_getCoefficients(SWS_CS_DEFAULT), 1, 0, 1 << 16, 1 << 16);
	sws_scale(sws, (const uint8_t *const *)f->data, f->linesize, 0, f->height, rgb->data, rgb->linesize);
	rgb->pts = 0;
	if (avcodec_send_frame(enc, rgb) < 0)
		goto done;
	avcodec_send_frame(enc, NULL);
	if (avcodec_receive_packet(enc, pkt) < 0)
		goto done;
	snprintf(local, sizeof(local), "file:%s", path); /* (always a file on this computer, never an address) */
	if (avio_open(&out, local, AVIO_FLAG_WRITE) < 0)
		goto done;
	avio_write(out, pkt->data, pkt->size);
	avio_closep(&out);
	ok = true;
done:
	sws_freeContext(sws);
	av_packet_free(&pkt);
	av_frame_free(&rgb);
	avcodec_free_context(&enc);
	app_log("screenshot %s: %s", ok ? "saved" : "failed", path);
	set_notice(ok ? "Ekran görüntüsü kaydedildi  •  Klasörü açmak için tıklayın" : "Ekran görüntüsü kaydedilemedi", ok);
}

/* ---- the virtual camera: our own, "Relios Mirror Camera"
 * softcam (github.com/tshino/softcam, MIT; this project's workflow builds a fixed revision of it, with this camera's
 * own name and class id): a DirectShow camera in one DLL, which also has the small interface the picture is sent
 * through (24-bit BGR, top row first). The DLL is installed once, with the user's consent, in "Program Files\Relios
 * Mirror" (where only administrators can change files) and made known to Windows there (install_camera); this
 * program loads that same file, and only while the camera is switched on. Nothing of OBS is used. */
#ifdef _WIN32
#define VCAM_W 1920
#define VCAM_H 1080
#define CAMERA_CLASS_KEY L"CLSID\\{A50EB610-EEB2-4C26-A2E9-B5D00E01EC2D}"

typedef void *sc_camera;
static struct {
	HMODULE dll;
	sc_camera (*create)(int, int, float);
	void (*destroy)(sc_camera);
	void (*send)(sc_camera, const void *);
	sc_camera cam;
	uint8_t *bgr;
	struct SwsContext *sws;
	int src_w, src_h, src_format, dst_w, dst_h;
	bool full, bt709;
	bool failed; /* (until the camera is switched off and on again) */
} S;

/* where the camera's DLL is installed: "<Program Files>\Relios Mirror\ReliosMirrorCamera.dll" */
static bool camera_installed_path(char *out, size_t size)
{
	wchar_t folder[MAX_PATH];
	char folder8[800];
	if (SHGetFolderPathW(NULL, CSIDL_PROGRAM_FILES, NULL, SHGFP_TYPE_CURRENT, folder) != S_OK)
		return false;
	if (WideCharToMultiByte(CP_UTF8, 0, folder, -1, folder8, (int)sizeof(folder8), NULL, NULL) <= 1)
		return false;
	snprintf(out, size, "%s\\Relios Mirror\\ReliosMirrorCamera.dll", folder8);
	return true;
}

/* installed, and known to Windows as a camera */
static bool camera_installed(void)
{
	char path[1200];
	HKEY key;
	if (!camera_installed_path(path, sizeof(path)) || !file_exists_utf8(path))
		return false;
	if (RegOpenKeyExW(HKEY_CLASSES_ROOT, CAMERA_CLASS_KEY, 0, KEY_READ, &key) != ERROR_SUCCESS)
		return false;
	RegCloseKey(key);
	return true;
}

static void camera_stop(void)
{
	if (S.cam && S.destroy) {
		S.destroy(S.cam);
		app_log("virtual camera: stopped");
	}
	S.cam = NULL;
	sws_freeContext(S.sws);
	S.sws = NULL;
	S.failed = false;
}

/* the camera exists (programs can open it) and shows black until the first picture; false: it cannot be used */
static bool camera_start(void)
{
	if (S.cam)
		return true;
	if (S.failed)
		return false;
	if (!S.dll) {
		char path[1200];
		wchar_t *w = camera_installed() && camera_installed_path(path, sizeof(path)) ? to_wide(path) : NULL;
		/* (the installed file, by its full path: the same file Windows gives to the programs that use the camera) */
		S.dll = w ? LoadLibraryExW(w, NULL, LOAD_WITH_ALTERED_SEARCH_PATH) : NULL;
		free(w);
		if (S.dll) {
			S.create = (sc_camera (*)(int, int, float))GetProcAddress(S.dll, "scCreateCamera");
			S.destroy = (void (*)(sc_camera))GetProcAddress(S.dll, "scDeleteCamera");
			S.send = (void (*)(sc_camera, const void *))GetProcAddress(S.dll, "scSendFrame");
		}
		app_log("virtual camera: %s", S.dll && S.create && S.destroy && S.send ? "loaded" : "not installed, or not usable");
	}
	if (!S.bgr)
		S.bgr = calloc(1, (size_t)VCAM_W * VCAM_H * 3 + 64);
	if (!S.dll || !S.create || !S.destroy || !S.send || !S.bgr) {
		S.failed = true;
		return false;
	}
	/* (frame rate 0: each picture is passed on as it comes) */
	S.cam = S.create(VCAM_W, VCAM_H, 0.0f);
	if (!S.cam) {
		app_log("virtual camera: could not be started (in use by another program?)");
		S.failed = true;
		return false;
	}
	memset(S.bgr, 0, (size_t)VCAM_W * VCAM_H * 3);
	S.send(S.cam, S.bgr);
	app_log("virtual camera: started");
	SDL_AtomicSet(&A.camera_ok, 1);
	return true;
}

/* the picture to the camera, fitted into 1920x1080 with black beside it (no picture: only makes sure the camera is
 * there) */
static void camera_send(const AVFrame *f, bool full, bool bt709)
{
	if (!camera_start()) {
		SDL_AtomicSet(&A.vcam_on, 0);
		set_notice("Sanal kamera başlatılamadı (kurulu değil ya da başka bir program kullanıyor)", false);
		return;
	}
	if (!f)
		return;
	int aw = f->width, ah = f->height;
	if (SDL_AtomicGet(&A.fit169) && aw > ah)
		aw = 16, ah = 9; /* landscape: spread to 16:9 */
	int dw = VCAM_W, dh = (int)((int64_t)VCAM_W * ah / aw);
	if (dh > VCAM_H) {
		dh = VCAM_H;
		dw = (int)((int64_t)VCAM_H * aw / ah);
	}
	dw &= ~1;
	dh &= ~1;
	if (dw < 2 || dh < 2 || dw > VCAM_W || dh > VCAM_H)
		return;
	if (!S.sws || S.src_w != f->width || S.src_h != f->height || S.src_format != f->format || S.dst_w != dw ||
	    S.dst_h != dh || S.full != full || S.bt709 != bt709) {
		sws_freeContext(S.sws);
		S.sws = sws_getContext(f->width, f->height, (enum AVPixelFormat)f->format, dw, dh, AV_PIX_FMT_BGR24,
				       SWS_BILINEAR, NULL, NULL, NULL);
		if (!S.sws)
			return;
		sws_setColorspaceDetails(S.sws, sws_getCoefficients(bt709 ? SWS_CS_ITU709 : SWS_CS_ITU601), full ? 1 : 0,
					 sws_getCoefficients(SWS_CS_DEFAULT), 1, 0, 1 << 16, 1 << 16);
		S.src_w = f->width;
		S.src_h = f->height;
		S.src_format = f->format;
		S.dst_w = dw;
		S.dst_h = dh;
		S.full = full;
		S.bt709 = bt709;
		memset(S.bgr, 0, (size_t)VCAM_W * VCAM_H * 3); /* (black beside a picture of another shape) */
	}
	int dx = (VCAM_W - dw) / 2, dy = (VCAM_H - dh) / 2;
	uint8_t *dst[4] = {S.bgr + ((size_t)dy * VCAM_W + (size_t)dx) * 3, NULL, NULL, NULL};
	int stride[4] = {VCAM_W * 3, 0, 0, 0};
	sws_scale(S.sws, (const uint8_t *const *)f->data, f->linesize, 0, f->height, dst, stride);
	S.send(S.cam, S.bgr);
}
#else
static bool camera_installed(void)
{
	return false;
}
static void camera_stop(void)
{
}
static void camera_send(const AVFrame *f, bool full, bool bt709)
{
	(void)f;
	(void)full;
	(void)bt709;
	SDL_AtomicSet(&A.vcam_on, 0);
}
#endif

static int rec_thread(void *unused)
{
	(void)unused;
	for (;;) {
		struct rec_item item;
		SDL_LockMutex(Q.mutex);
		while (Q.count == 0 && !Q.quit)
			SDL_CondWait(Q.cond, Q.mutex);
		if (Q.count == 0) {
			SDL_UnlockMutex(Q.mutex);
			break;
		}
		item = Q.items[Q.head];
		Q.head = (Q.head + 1) % REC_QUEUE;
		Q.count--;
		if (item.kind == REC_VIDEO)
			Q.videos--;
		SDL_UnlockMutex(Q.mutex);

		if (!SDL_AtomicGet(&A.vcam_on))
			camera_stop();
		if (item.kind == REC_VIDEO) {
			if (SDL_AtomicGet(&A.vcam_on))
				camera_send(item.frame, item.full, item.bt709);
			rec_video(item.frame, item.full, item.bt709, item.t);
			av_frame_free(&item.frame);
		} else if (item.kind == REC_SHOT) {
			save_png(item.frame, item.full, item.bt709);
			av_frame_free(&item.frame);
		} else if (item.kind == REC_SYNC) {
			/* (a switch changed: the camera is there as soon as it is on, before the first picture) */
			if (SDL_AtomicGet(&A.vcam_on))
				camera_send(NULL, false, false);
		} else if (item.kind == REC_AUDIO) {
			rec_audio(item.audio, item.frames, item.rate, item.channels, item.t);
			free(item.audio);
		} else {
			SDL_LockMutex(R.mutex);
			rec_close_locked();
			SDL_UnlockMutex(R.mutex);
			push_event(EV_STATE);
		}
	}
	camera_stop();
	return 0;
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
	av_frame_unref(A.last);
	if (av_frame_ref(A.last, f) >= 0) {
		A.last_full = full;
		A.last_709 = bt709;
	}
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
		if (f->width < 16 || f->height < 16 || f->width > 8192 || f->height > 8192) {
			av_frame_unref(d->frame);
			av_frame_unref(d->sw);
			continue;
		}
		bool full, bt709;
		frame_colors(f, &full, &bt709);
		present_frame(d, f, full, bt709);
		rec_queue_video(f, full, bt709);
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
	if (SDL_AtomicCAS(&A.audio_reopen, 1, 0) && A.adev) {
		/* another output was chosen */
		SDL_CloseAudioDevice(A.adev);
		A.adev = 0;
	}
	if (A.adev && A.arate == rate && A.ach == channels)
		return true;
	if (A.adev)
		SDL_CloseAudioDevice(A.adev);
	char device[256];
	SDL_LockMutex(A.mutex);
	snprintf(device, sizeof(device), "%s", A.audio_device);
	SDL_UnlockMutex(A.mutex);
	SDL_AudioSpec want, have;
	SDL_zero(want);
	SDL_zero(have);
	want.freq = rate;
	want.format = AUDIO_F32SYS;
	want.channels = (Uint8)channels;
	want.samples = 512; /* small: the sound must not lag behind the picture */
	A.adev = SDL_OpenAudioDevice(device[0] ? device : NULL, 0, &want, &have, 0);
	if (!A.adev && device[0])
		A.adev = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0); /* (the chosen one is gone: the default) */
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
				rec_queue_audio(d->mix, n, f->sample_rate, ch);

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
	av_frame_unref(A.last);
	SDL_UnlockMutex(A.frame_mutex);
}

/* The mirroring stopped (or the receiver ended). If that was within twenty seconds of the name leaving the network,
 * hiding is taken to be what stopped it: the window thread switches the option off and says so. (Someone who stops
 * a mirroring that early finds the option off too, and can switch it on again.) */
static void hide_check(void)
{
	SDL_LockMutex(A.mutex);
	Uint32 at = A.hidden_at;
	A.hidden_at = 0;
	SDL_UnlockMutex(A.mutex);
	/* (not when the program itself is stopping the receiver: a changed setting, the end of the program) */
	if (at && !SDL_AtomicGet(&A.stopping) && SDL_GetTicks() - at < 20000)
		SDL_AtomicSet(&A.hide_broke, 1);
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
			hide_check();
			clear_picture();
			if (A.adev)
				SDL_ClearQueuedAudio(A.adev);
		}
		push_event(EV_STATE);
	} else if (!strncmp(text, "hidden", 6)) {
		bool hidden = strstr(text, " 1") != NULL;
		app_log("the receiver's name is %s", hidden ? "off the network (hidden while mirroring)" : "announced again");
		SDL_LockMutex(A.mutex);
		A.hidden_at = hidden ? (SDL_GetTicks() | 1) : 0;
		SDL_UnlockMutex(A.mutex);
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
			if (SDL_AtomicCAS(&A.register_ok, 1, 0) && register_line_ok((const char *)buf, len)) {
				add_paired_device((const char *)buf);
				app_log("a new device was paired and remembered");
			} else {
				app_log("a pairing record that was not asked for (or is malformed) was ignored");
			}
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
	rec_queue_close();
	hide_check();

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

	SDL_AtomicSet(&A.register_ok, allow ? 1 : 0);
	/* (the part that reads what the network sends runs at low integrity, or not at all) */
	A.proc = proc_start(A.exe, "--serve", true);
	if (!A.proc) {
		free(paired);
		session_fail("Alıcı korumalı modda başlatılamadı");
		return;
	}
	app_log("receiver started; paired devices: %d, pairing a new device: %s, asked %dx%d@%d %s", (int)paired_n,
		allow ? "on" : "off", A.width, A.height, A.fps, A.h265 ? "H.264/H.265" : "H.264");

	char config[512];
	snprintf(config, sizeof(config),
		 "name " RECEIVER_NAME "\ndeviceid %s\nkeyfile receiver-key.pem\nsize %dx%d@%d\nh265 %d\nallow_pairing %d\nvisible %d\nhide_connected %d\n",
		 id, A.res_auto ? 3840 : A.width, A.res_auto ? 2160 : A.height, A.fps, A.h265 ? 1 : 0, allow ? 1 : 0,
		 A.visible ? 1 : 0, A.hide_connected ? 1 : 0);
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
		session_fail("İş parçacığı başlatılamadı");
	}
}

/* ------------------------------------------------------------------ the window */

static void status_text(char *status, size_t size)
{
	SDL_LockMutex(A.mutex);
	switch (A.state) {
	case MIRROR_STARTING:
		snprintf(status, size, "Başlatılıyor…");
		break;
	case MIRROR_LISTENING:
		snprintf(status, size, A.visible ? "Hazır  •  Apple cihazında Ekran Yansıtma'dan " RECEIVER_NAME "'ı seçin%s" :
				     "Hazır  •  Listede gizli (Ayarlar > Cihaz'dan gösterin)%s",
			 A.pair_requested ? "  •  Yeni cihaz eşleştirme açık" : "");
		break;
	case MIRROR_PIN:
		snprintf(status, size, "Penceredeki kodu Apple cihazına girin");
		break;
	case MIRROR_CONNECTED:
		if (A.format[0])
			snprintf(status, size, "Bağlı: %s  •  %s, %.0f fps, %.1f Mbit/s", A.client[0] ? A.client : "Apple cihazı",
				 A.format, A.fps_now, A.mbit);
		else
			snprintf(status, size, "Bağlı: %s", A.client[0] ? A.client : "Apple cihazı");
		break;
	case MIRROR_PAIRING_OFF:
		snprintf(status, size, "Üç yanlış kod girildi, eşleştirme kapandı (Ayarlar > Cihaz'dan yeniden açın)");
		break;
	case MIRROR_ENDED:
		snprintf(status, size, "Yeniden başlatılıyor…");
		break;
	default:
		if (strstr(A.error, "TCP 7000"))
			snprintf(status, size,
				 "Hata: 7000 portu kullanımda (OBS'teki iPad Yansıtma ya da başka bir AirPlay alıcısı açık olabilir)");
		else
			snprintf(status, size, "Hata: %s", A.error);
		break;
	}
	SDL_UnlockMutex(A.mutex);

	char rec[160] = "";
	SDL_LockMutex(R.mutex);
	if (R.oc && R.header_written) {
		uint64_t now = now_ns();
		int seconds = (int)((now > R.t0 ? now - R.t0 : 0) / 1000000000ull);
		int64_t bytes = R.oc->pb ? avio_tell(R.oc->pb) : 0;
		snprintf(rec, sizeof(rec), "  •  Kayıt %02d:%02d:%02d  %.1f MB (%s)", seconds / 3600, seconds / 60 % 60,
			 seconds % 60, (double)bytes / 1048576.0, R.encoder);
	}
	else if (R.failed)
		snprintf(rec, sizeof(rec), "  •  Kayıt başlatılamadı");
	else if (R.want)
		snprintf(rec, sizeof(rec), "  •  Kayıt görüntüyü bekliyor");
	SDL_UnlockMutex(R.mutex);
	size_t used = strlen(status);
	SDL_LockMutex(A.mutex);
	bool hidden = A.hidden_at != 0;
	SDL_UnlockMutex(A.mutex);
	snprintf(status + used, size - used, "%s%s%s", rec, SDL_AtomicGet(&A.vcam_on) ? "  •  Sanal kamera açık" : "",
		 hidden ? "  •  Listede gizli" : "");

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
	SDL_Color white = {255, 255, 255, 255}; /* (coloured when drawn) */
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

/* the paired devices' names, for the settings ("Bayram iPad'i, ..."); read again every two seconds */
static const char *paired_names(int *count)
{
	static char names[300];
	static int n;
	static Uint32 read_at;
	Uint32 now = SDL_GetTicks();
	if (!read_at || now - read_at > 2000) {
		char *list = read_text("paired-devices.txt");
		read_at = now ? now : 1;
		names[0] = '\0';
		n = 0;
		for (char *line = list; line && *line;) {
			char *end = strchr(line, '\n');
			if (end)
				*end = '\0';
			/* <key> TAB <device id> TAB <name> TAB <date> */
			char *tab1 = strchr(line, '\t'), *tab2 = tab1 ? strchr(tab1 + 1, '\t') : NULL;
			if (tab1 && tab1 - line > 10) {
				const char *name = tab2 ? tab2 + 1 : "?";
				size_t len = strcspn(name, "\t\r"), used = strlen(names);
				snprintf(names + used, sizeof(names) - used, "%s%.*s", n ? ", " : "", (int)len, name);
				n++;
			}
			line = end ? end + 1 : NULL;
		}
		free(list);
	}
	*count = n;
	return names;
}

static bool autostart_get(void);

/* (asked while the settings are drawn: looked up every two seconds) */
static bool camera_installed_cached(void)
{
	static Uint32 at;
	static bool value;
	Uint32 now = SDL_GetTicks();
	if (!at || now - at > 2000) {
		value = camera_installed();
		at = now ? now : 1;
	}
	return value;
}

/* ---- the interface: a bar at the bottom (shown while the mouse moves over the window, and while nothing is
 * mirrored) with the few things used all the time, and a settings panel for the rest */

enum { KIND_BUTTON, KIND_CHOICE, KIND_SWITCH };

struct button {
	int cmd, arg;
	int kind;
	bool on;
	int row; /* (settings: its row; -1: the bar or a folder's tab) */
	char text[64];
	SDL_Rect r;
};
static struct button buttons[64];
static int button_count;
static bool settings_open;
static int settings_tab; /* 0: picture, 1: recording, 2: device, 3: program */
static bool editing_kbit; /* the recording bit rate is being typed */
static char kbit_text[8];
static SDL_Rect panel_rect, bar_rect;
static char row_labels[12][80];
static int row_count;

static struct button *add_button(int cmd, int arg, int kind, bool on, const char *fmt, ...)
{
	static struct button spare;
	struct button *b = button_count < (int)(sizeof(buttons) / sizeof(buttons[0])) ? &buttons[button_count++] : &spare;
	memset(b, 0, sizeof(*b));
	b->cmd = cmd;
	b->arg = arg;
	b->kind = kind;
	b->on = on;
	b->row = -1;
	va_list args;
	va_start(args, fmt);
	vsnprintf(b->text, sizeof(b->text), fmt, args);
	va_end(args);
	return b;
}

/* settings: a row with its name at the left; what follows (choices, a switch, buttons) sits at its right */
static void row(const char *label)
{
	if (row_count < (int)(sizeof(row_labels) / sizeof(row_labels[0])))
		snprintf(row_labels[row_count++], sizeof(row_labels[0]), "%s", label);
}

static void choice(int cmd, int arg, bool selected, const char *text)
{
	add_button(cmd, arg, KIND_CHOICE, selected, "%s", text)->row = row_count - 1;
}

static void toggle(const char *label, int cmd, bool on)
{
	row(label);
	add_button(cmd, 0, KIND_SWITCH, on, "%s", "")->row = row_count - 1;
}

static void action(const char *label, int cmd, const char *text)
{
	row(label);
	add_button(cmd, 0, KIND_BUTTON, false, "%s", text)->row = row_count - 1;
}

static bool bar_wanted(void)
{
	SDL_LockMutex(A.frame_mutex);
	bool mirroring = A.have_frame;
	SDL_UnlockMutex(A.frame_mutex);
	return !mirroring || settings_open || (A.bar_until && !SDL_TICKS_PASSED(SDL_GetTicks(), A.bar_until));
}

/* a rectangle with round corners (row by row along the corners' circle) */
static void fill_round(const SDL_Rect *r, int rad, Uint8 red, Uint8 green, Uint8 blue, Uint8 alpha)
{
	if (r->w <= 0 || r->h <= 0)
		return;
	if (rad * 2 > r->h)
		rad = r->h / 2;
	if (rad * 2 > r->w)
		rad = r->w / 2;
	if (rad < 0)
		rad = 0;
	SDL_SetRenderDrawColor(A.ren, red, green, blue, alpha);
	SDL_Rect middle = {r->x, r->y + rad, r->w, r->h - 2 * rad};
	SDL_RenderFillRect(A.ren, &middle);
	for (int i = 0; i < rad; i++) {
		double dy = (double)rad - (double)i - 0.5;
		int inset = (int)((double)rad - sqrt((double)rad * (double)rad - dy * dy) + 0.5);
		SDL_Rect top = {r->x + inset, r->y + i, r->w - 2 * inset, 1};
		SDL_Rect bottom = {r->x + inset, r->y + r->h - 1 - i, r->w - 2 * inset, 1};
		SDL_RenderFillRect(A.ren, &top);
		SDL_RenderFillRect(A.ren, &bottom);
	}
}

static void draw_text(const char *text, int x, int y, int max_w, Uint8 red, Uint8 green, Uint8 blue)
{
	struct text_entry *t = text_get(text);
	if (!t || max_w <= 0)
		return;
	SDL_SetTextureColorMod(t->tex, red, green, blue);
	SDL_Rect src = {0, 0, t->w < max_w ? t->w : max_w, t->h};
	SDL_Rect dst = {x, y, src.w, src.h};
	SDL_RenderCopy(A.ren, t->tex, &src, &dst);
}

static void draw_centered(const char *text, const SDL_Rect *r, Uint8 red, Uint8 green, Uint8 blue)
{
	struct text_entry *t = text_get(text);
	if (t)
		draw_text(text, r->x + (r->w - t->w) / 2, r->y + (r->h - t->h) / 2, r->w, red, green, blue);
}

/* the mouse, in the picture's coordinates */
static SDL_Point mouse_point(int ow, int oh)
{
	int mx = 0, my = 0, ww = 0, wh = 0;
	SDL_GetMouseState(&mx, &my);
	SDL_GetWindowSize(A.win, &ww, &wh);
	SDL_Point p = {ww > 0 ? mx * ow / ww : -1, wh > 0 ? my * oh / wh : -1};
	return p;
}

static void draw_button(const struct button *b, const SDL_Point *mouse)
{
	bool hover = SDL_PointInRect(mouse, &b->r) == SDL_TRUE;
	int f = A.font_px;
	if (b->kind == KIND_SWITCH) {
		/* a switch: a pill with a round knob, at the right when on */
		int h = b->r.h, knob = h - 6;
		if (b->on)
			fill_round(&b->r, h / 2, 0, hover ? 196 : 178, hover ? 150 : 135, 255);
		else
			fill_round(&b->r, h / 2, hover ? 66 : 52, hover ? 80 : 66, hover ? 76 : 62, 255);
		SDL_Rect k = {b->on ? b->r.x + b->r.w - knob - 3 : b->r.x + 3, b->r.y + 3, knob, knob};
		fill_round(&k, knob / 2, 244, 248, 246, 255);
	} else if (b->kind == KIND_CHOICE) {
		/* one of a strip of choices: the chosen one is filled */
		SDL_Rect in = {b->r.x + 2, b->r.y + 2, b->r.w - 4, b->r.h - 4};
		if (b->on)
			fill_round(&in, f / 3, 0, 178, 135, 255);
		else if (hover)
			fill_round(&in, f / 3, 36, 50, 46, 255);
		if (b->on)
			draw_centered(b->text, &b->r, 4, 20, 16);
		else
			draw_centered(b->text, &b->r, hover ? 232 : 170, hover ? 240 : 186, hover ? 237 : 180);
	} else {
		/* a button: a quiet surface with a hairline edge; filled when it is "on" */
		SDL_Rect in = {b->r.x + 1, b->r.y + 1, b->r.w - 2, b->r.h - 2};
		if (b->on) {
			fill_round(&b->r, f / 2, 0, hover ? 196 : 178, hover ? 150 : 135, 255);
			draw_centered(b->text, &b->r, 4, 20, 16);
		} else {
			fill_round(&b->r, f / 2, hover ? 72 : 46, hover ? 94 : 62, hover ? 88 : 58, 255);
			fill_round(&in, f / 2 - 1, hover ? 34 : 23, hover ? 48 : 34, hover ? 44 : 31, 255);
			draw_centered(b->text, &b->r, 232, 240, 237);
		}
	}
}

/* the settings of the open folder: rows of names with their controls */
static const char *build_settings(void)
{
	const char *note = "";
	if (settings_tab == 0) {
		row("Çözünürlük");
		choice(CMD_RES_NEXT, 0, A.height == 720, "720p");
		choice(CMD_RES_NEXT, 1, A.height == 1080, "1080p");
		choice(CMD_RES_NEXT, 2, A.height == 1440, "1440p");
		choice(CMD_RES_NEXT, 3, A.height == 2160, "4K");
		row("Kare Hızı");
		choice(CMD_FPS_NEXT, 30, A.fps == 30, "30 fps");
		choice(CMD_FPS_NEXT, 60, A.fps != 30, "60 fps");
		row("Kodek");
		choice(CMD_CODEC_NEXT, 0, !A.h265, "H.264");
		choice(CMD_CODEC_NEXT, 1, A.h265, "H.265");
		row("Görüntü Çözme");
		choice(CMD_HW_DECODE, 1, A.hw_decode, "Ekran Kartı");
		choice(CMD_HW_DECODE, 0, !A.hw_decode, "İşlemci");
		bool fit = SDL_AtomicGet(&A.fit169) != 0;
		row("Görüntü Oranı");
		choice(CMD_ASPECT, 0, !fit, "Özgün");
		choice(CMD_ASPECT, 1, fit, "16:9");
		note = "İlk dört ayar değişince cihazdan yeniden bağlanın. 16:9 yalnızca yatay görüntüye uygulanır.";
	} else if (settings_tab == 1) {
		row("Biçim");
		choice(CMD_REC_FORMAT, 0, A.rec_format == 0, "MP4");
		choice(CMD_REC_FORMAT, 1, A.rec_format == 1, "MOV");
		choice(CMD_REC_FORMAT, 2, A.rec_format == 2, "MKV");
		row("Kodlayıcı");
		choice(CMD_REC_ENCODER, 1, A.rec_gpu, "Ekran Kartı");
		choice(CMD_REC_ENCODER, 0, !A.rec_gpu, "İşlemci");
		row("Bit Hızı");
		choice(CMD_REC_MODE, 0, A.rec_mode == 0, "Otomatik");
		choice(CMD_REC_MODE, 1, A.rec_mode == 1, "CBR");
		choice(CMD_REC_MODE, 2, A.rec_mode == 2, "VBR");
		if (A.rec_mode != 0 || editing_kbit) {
			row("Bit Hızı Değeri");
			if (editing_kbit)
				add_button(CMD_REC_BITRATE, 0, KIND_BUTTON, true, "%s_ kbit", kbit_text)->row = row_count - 1;
			else
				add_button(CMD_REC_BITRATE, 0, KIND_BUTTON, false, "%d kbit", A.rec_kbit)->row = row_count - 1;
		}
		toggle("Sesi Kaydet", CMD_REC_AUDIO, A.rec_audio);
		row("Kayıt Klasörü");
		add_button(CMD_OPEN_FOLDER, 0, KIND_BUTTON, false, "Aç")->row = row_count - 1;
		add_button(CMD_CHOOSE_FOLDER, 0, KIND_BUTTON, false, "Değiştir")->row = row_count - 1;
		note = editing_kbit ? "Rakamları yazın, Enter ile onaylayın." :
		       A.rec_mode != 0 ? "Bit hızını değiştirmek için değerine tıklayıp yazın." :
					 "Ayarlar bir sonraki kayıtta geçerli olur.";
	} else if (settings_tab == 3) {
		toggle("Windows İle Başlat", CMD_AUTOSTART, autostart_get());
		toggle("Pencere Her Zaman Üstte", CMD_ONTOP, A.on_top);
		action("Varsayılan Ayarlara Dön (İlk Açılış Gibi)", CMD_RESET, "Sıfırla");
		note = "Sıfırlama ayarları ve eşleşmiş cihazları siler; önce onay ister.";
	} else {
		int paired = 0;
		const char *names = paired_names(&paired);
		SDL_LockMutex(A.mutex);
		bool pairing = A.pair_requested || paired == 0; /* (the first device always pairs with the code) */
		SDL_UnlockMutex(A.mutex);
		toggle("Yeni Cihaz Eşleştirme", CMD_PAIR, pairing);
		char paired_label[80];
		if (paired > 0) {
			/* (shortened to fit, at a whole letter) */
			size_t len = strlen(names), keep = len > 44 ? 44 : len;
			while (keep > 0 && keep < len && ((unsigned char)names[keep] & 0xC0) == 0x80)
				keep--;
			snprintf(paired_label, sizeof(paired_label), "Eşleşmiş: %.*s%s", (int)keep, names, keep < len ? "…" : "");
			action(paired_label, CMD_FORGET, "Unut");
		} else {
			row("Eşleşmiş Cihaz Yok");
		}
		toggle("Bağlıyken Listeden Gizle", CMD_HIDE, A.hide_connected);
		char device[64] = "Varsayılan";
		if (A.audio_device[0]) {
			size_t len = strlen(A.audio_device), keep = len > 22 ? 22 : len;
			while (keep > 0 && keep < len && ((unsigned char)A.audio_device[keep] & 0xC0) == 0x80)
				keep--;
			snprintf(device, sizeof(device), "%.*s%s", (int)keep, A.audio_device, keep < len ? "…" : "");
		}
		action("Ses Çıkışı", CMD_AUDIO_DEVICE, device);
		toggle("Sanal Kamera", CMD_VCAM, SDL_AtomicGet(&A.vcam_on) != 0);
		if (camera_installed_cached())
			action("Sanal Kamerayı Windows'tan Kaldır", CMD_VCAM_REMOVE, "Kaldır");
		note = "Sanal kamerayı önce burada açın, sonra programda \"Relios Mirror Camera\"yı seçin.";
	}
	return note;
}

static void draw_bar(int ow, int oh)
{
	SDL_LockMutex(R.mutex);
	bool recording = R.want;
	SDL_UnlockMutex(R.mutex);
	bool muted = SDL_AtomicGet(&A.muted) != 0;
	int f = A.font_px, pad = f, bh = f * 2, gap = f / 2;
	SDL_Point mouse = mouse_point(ow, oh);

	button_count = 0;
	row_count = 0;
	add_button(CMD_RECORD, 0, KIND_BUTTON, recording, recording ? "Kaydı Durdur" : "Kayıt");
	add_button(CMD_SCREENSHOT, 0, KIND_BUTTON, false, "Ekran Görüntüsü");
	add_button(CMD_VOL_DOWN, 0, KIND_BUTTON, false, "-");
	add_button(CMD_VOL_100, 0, KIND_BUTTON, false, "Ses %%%d", SDL_AtomicGet(&A.volume));
	add_button(CMD_VOL_UP, 0, KIND_BUTTON, false, "+");
	add_button(CMD_MUTE, 0, KIND_BUTTON, muted, "Sessiz");
	add_button(CMD_FULLSCREEN, 0, KIND_BUTTON, A.fullscreen, "Tam Ekran");
	add_button(CMD_SETTINGS, 0, KIND_BUTTON, settings_open, "Ayarlar");
	int bar_buttons = button_count;

	/* one row: the state at the left, the buttons at the right */
	int bar_h = bh + 2 * gap, y0 = oh - bar_h, x = ow - gap;
	for (int i = bar_buttons - 1; i >= 0; i--) {
		struct text_entry *t = text_get(buttons[i].text);
		int bw = (t ? t->w : f * 2) + pad * 2;
		x -= bw;
		buttons[i].r = (SDL_Rect){x, y0 + gap, bw, bh};
		/* (volume: - value + sit together) */
		x -= (buttons[i].cmd == CMD_VOL_100 || buttons[i].cmd == CMD_VOL_UP) ? 3 : gap;
	}
	bar_rect = (SDL_Rect){0, y0, ow, bar_h};

	SDL_SetRenderDrawBlendMode(A.ren, SDL_BLENDMODE_BLEND);
	SDL_SetRenderDrawColor(A.ren, 8, 13, 12, 238);
	SDL_RenderFillRect(A.ren, &bar_rect);
	SDL_Rect line = {0, y0, ow, 1};
	SDL_SetRenderDrawColor(A.ren, 36, 52, 48, 255);
	SDL_RenderFillRect(A.ren, &line);

	char status[600];
	status_text(status, sizeof(status));
	struct text_entry *st = text_get(status);
	if (st)
		draw_text(status, gap * 2, y0 + (bar_h - st->h) / 2, x - gap * 3, 150, 170, 164);
	for (int i = 0; i < bar_buttons; i++)
		draw_button(&buttons[i], &mouse);

	panel_rect = (SDL_Rect){0, 0, 0, 0};
	if (settings_open) {
		static const char *const tab_names[] = {"Görüntü", "Kayıt", "Cihaz", "Program"};
		int tabs = button_count;
		for (int t = 0; t < 4; t++)
			add_button(CMD_TAB, t, KIND_CHOICE, settings_tab == t, "%s", tab_names[t]);
		int first = button_count;
		const char *note = build_settings();

		int edge = f * 3 / 2, row_h = f * 3, ctl_h = f * 2;
		int pw = f * 36 < ow - 2 * gap ? f * 36 : ow - 2 * gap;
		int head_h = f * 4, foot_h = f * 3;
		int ph = head_h + 7 * row_h + foot_h;
		int px = ow - pw - gap, py = y0 - ph - gap;
		if (py < gap)
			py = gap;
		panel_rect = (SDL_Rect){px, py, pw, ph};
		SDL_Rect inner = {px + 1, py + 1, pw - 2, ph - 2};
		fill_round(&panel_rect, f, 40, 56, 52, 255);
		fill_round(&inner, f - 1, 11, 17, 16, 252);

		/* the folders: a strip of choices at the top */
		int tx = px + edge;
		for (int i = tabs; i < first; i++) {
			struct text_entry *t = text_get(buttons[i].text);
			int bw = (t ? t->w : f * 3) + pad * 2;
			buttons[i].r = (SDL_Rect){tx, py + f, bw, ctl_h};
			tx += bw;
		}
		SDL_Rect strip = {px + edge - 2, py + f - 2, tx - (px + edge) + 4, ctl_h + 4};
		fill_round(&strip, f / 2, 22, 32, 29, 255);
		for (int i = tabs; i < first; i++)
			draw_button(&buttons[i], &mouse);

		for (int r = 0; r < row_count; r++) {
			int ry = py + head_h + r * row_h;
			if (r > 0) {
				SDL_Rect divider = {px + edge, ry, pw - 2 * edge, 1};
				SDL_SetRenderDrawColor(A.ren, 26, 38, 35, 255);
				SDL_RenderFillRect(A.ren, &divider);
			}
			/* its controls, from the right */
			int right = px + pw - edge, cx = right, left = right, n = 0;
			bool choices = false;
			for (int i = button_count - 1; i >= first; i--) {
				struct button *b = &buttons[i];
				if (b->row != r)
					continue;
				int bw;
				if (b->kind == KIND_SWITCH) {
					bw = f * 5 / 2;
					b->r = (SDL_Rect){cx - bw, ry + (row_h - f * 3 / 2) / 2, bw, f * 3 / 2};
				} else {
					struct text_entry *t = text_get(b->text);
					bw = (t ? t->w : f * 2) + pad * 2;
					if (b->kind == KIND_BUTTON && bw < f * 5)
						bw = f * 5;
					b->r = (SDL_Rect){cx - bw, ry + (row_h - ctl_h) / 2, bw, ctl_h};
				}
				choices = b->kind == KIND_CHOICE;
				left = b->r.x;
				cx = left - (choices ? 0 : gap);
				n++;
			}
			if (choices && n > 0) {
				SDL_Rect group = {left - 2, ry + (row_h - ctl_h) / 2 - 2, right - left + 4, ctl_h + 4};
				fill_round(&group, f / 2, 22, 32, 29, 255);
			}
			for (int i = first; i < button_count; i++) {
				if (buttons[i].row == r)
					draw_button(&buttons[i], &mouse);
			}
			struct text_entry *t = text_get(row_labels[r]);
			draw_text(row_labels[r], px + edge, ry + (row_h - (t ? t->h : f)) / 2, left - gap - (px + edge), 222, 232, 228);
		}
		draw_text(note, px + edge, py + ph - foot_h + f / 2, pw - 2 * edge, 120, 142, 135);
	}
	SDL_SetRenderDrawBlendMode(A.ren, SDL_BLENDMODE_NONE);
}

/* nothing is mirrored: the logo and what to do on the iPad */
static void draw_idle(int ow, int oh)
{
	SDL_SetRenderDrawColor(A.ren, 3, 13, 11, 255);
	SDL_RenderClear(A.ren);
	int size = oh / 4, y = oh / 5;
	if (A.logo && size > 0) {
		SDL_Rect dst = {(ow - size) / 2, y, size, size};
		SDL_RenderCopy(A.ren, A.logo, NULL, &dst);
	}
	y += size + A.font_px;
	static const char *const lines[] = {APP_NAME, "iPhone / iPad  →  Denetim Merkezi  →  Ekran Yansıtma  →  " RECEIVER_NAME};
	for (int i = 0; i < 2; i++) {
		struct text_entry *t = text_get(lines[i]);
		if (!t)
			continue;
		if (i == 0)
			SDL_SetTextureColorMod(t->tex, 0, 190, 145);
		else
			SDL_SetTextureColorMod(t->tex, 150, 205, 190);
		SDL_Rect dst = {(ow - t->w) / 2, y, t->w, t->h};
		SDL_RenderCopy(A.ren, t->tex, NULL, &dst);
		y += t->h + A.font_px / 2;
	}
}

/* the button at this place of the window (window coordinates), or NULL; inside: on the bar or the panel */
static const struct button *button_at(int mx, int my, bool *inside)
{
	int ow = 0, oh = 0, ww = 0, wh = 0;
	*inside = false;
	if (!A.bar_drawn)
		return NULL;
	SDL_GetRendererOutputSize(A.ren, &ow, &oh);
	SDL_GetWindowSize(A.win, &ww, &wh);
	if (ww <= 0 || wh <= 0)
		return NULL;
	SDL_Point p = {mx * ow / ww, my * oh / wh};
	*inside = SDL_PointInRect(&p, &bar_rect) || SDL_PointInRect(&p, &panel_rect);
	for (int i = 0; i < button_count; i++) {
		if (SDL_PointInRect(&p, &buttons[i].r))
			return &buttons[i];
	}
	return NULL;
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
	SDL_SetRenderDrawColor(A.ren, 3, 13, 11, 255);
	SDL_RenderClear(A.ren);
	SDL_SetRenderDrawColor(A.ren, 0, 190, 145, 255);
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

	int ww = 0, wh = 0;
	SDL_GetWindowSize(A.win, &ww, &wh);
	int px = ww > 0 ? 15 * ow / ww : 15;
	font_open(px < 10 ? 10 : px);

	SDL_SetRenderDrawColor(A.ren, 0, 0, 0, 255);
	SDL_RenderClear(A.ren);
	if (pin[0]) {
		draw_pin(pin, ow, oh);
	} else if (!show) {
		draw_idle(ow, oh);
	} else if (A.tex && ow > 0 && oh > 0) {
		SDL_Rect dst;
		int aw = A.tex_w, ah = A.tex_h;
		if (SDL_AtomicGet(&A.fit169) && aw > ah)
			aw = 16, ah = 9; /* landscape: spread to 16:9; portrait stays as it is */
		if ((int64_t)ow * ah > (int64_t)oh * aw) {
			dst.h = oh;
			dst.w = (int)((int64_t)oh * aw / ah);
		} else {
			dst.w = ow;
			dst.h = (int)((int64_t)ow * ah / aw);
		}
		dst.x = (ow - dst.w) / 2;
		dst.y = (oh - dst.h) / 2;
		SDL_RenderCopy(A.ren, A.tex, NULL, &dst);
	}
	A.bar_drawn = bar_wanted();
	if (A.bar_drawn)
		draw_bar(ow, oh);

	/* a message for a few seconds, at the top in the middle */
	char notice[300] = "";
	SDL_LockMutex(A.mutex);
	if (A.notice[0] && !SDL_TICKS_PASSED(SDL_GetTicks(), A.notice_until))
		snprintf(notice, sizeof(notice), "%s", A.notice);
	SDL_UnlockMutex(A.mutex);
	A.notice_drawn = false;
	struct text_entry *nt = notice[0] ? text_get(notice) : NULL;
	if (nt) {
		int f = A.font_px, w = nt->w + 2 * f < ow - f ? nt->w + 2 * f : ow - f;
		A.notice_rect = (SDL_Rect){(ow - w) / 2, f, w, f * 5 / 2};
		SDL_SetRenderDrawBlendMode(A.ren, SDL_BLENDMODE_BLEND);
		fill_round(&A.notice_rect, f / 2, 0, 166, 126, 240);
		draw_text(notice, A.notice_rect.x + f, A.notice_rect.y + (A.notice_rect.h - nt->h) / 2, w - 2 * f, 2, 16, 13);
		SDL_SetRenderDrawBlendMode(A.ren, SDL_BLENDMODE_NONE);
		A.notice_drawn = true;
	}
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

/* another folder for the recordings and screenshots (Windows' folder dialog) */
static void choose_video_folder(void)
{
#ifdef _WIN32
	BROWSEINFOW info;
	memset(&info, 0, sizeof(info));
	info.lpszTitle = L"Kayıtların ve ekran görüntülerinin kaydedileceği klasörü seçin";
	info.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
	CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
	PIDLIST_ABSOLUTE chosen = SHBrowseForFolderW(&info);
	if (!chosen)
		return;
	wchar_t wide[MAX_PATH];
	char folder[1024];
	if (SHGetPathFromIDListW(chosen, wide) &&
	    WideCharToMultiByte(CP_UTF8, 0, wide, -1, folder, (int)sizeof(folder), NULL, NULL) > 1) {
		snprintf(A.custom_folder, sizeof(A.custom_folder), "%s", folder);
		find_video_folder();
		save_settings();
		set_notice("Kayıt yeri değişti  •  Klasörü açmak için tıklayın", true);
	}
	CoTaskMemFree(chosen);
#endif
}

#ifdef _WIN32
/* The camera's DLL (next to the program) is copied to "Program Files\Relios Mirror" and made known to Windows there,
 * with the user's consent (Windows asks: UAC), once. false: not installed. */
static bool install_camera(void)
{
	char src[2100], dst[1200], dir[1200], cmd_exe[900], regsvr[900], args[9000];
	if (camera_installed())
		return true;
	char *base = SDL_GetBasePath();
	snprintf(src, sizeof(src), "%ssoftcam.dll", base ? base : "");
	SDL_free(base);
	if (!file_exists_utf8(src) || !camera_installed_path(dst, sizeof(dst))) {
		app_log("virtual camera: this build has no camera file");
		return false;
	}
	snprintf(dir, sizeof(dir), "%s", dst);
	char *slash = strrchr(dir, '\\');
	if (slash)
		*slash = '\0';
	system_program("cmd.exe", cmd_exe, sizeof(cmd_exe));
	system_program("regsvr32.exe", regsvr, sizeof(regsvr));
	if (strpbrk(src, "%^!\"&") || strpbrk(dst, "%^!\"&") || strpbrk(regsvr, "%^!\"&")) {
		app_log("virtual camera: not installed (unusual letters in a folder's name)");
		return false;
	}
	SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_INFORMATION, APP_NAME,
				 "Sanal kamera (\"Relios Mirror Camera\") Windows'a bir kez kurulacak.\n\n"
				 "Tamam'a bastıktan sonra çıkacak Windows sorusunda \"Evet\"i seçin.\n"
				 "Kamerayı kullanacak program (Discord vb.) açıksa kapatıp yeniden açın.",
				 A.win);
	int n = snprintf(args, sizeof(args),
			 "/c \"mkdir \"%s\" 2>nul & copy /y \"%s\" \"%s\" >nul 2>&1 & \"%s\" /s \"%s\"\"", dir, src, dst,
			 regsvr, dst);
	if (n < 0 || n >= (int)sizeof(args))
		return false;
	int code = run_elevated(cmd_exe, args);
	bool ok = code == 0 && camera_installed();
	app_log("virtual camera: installation %s (code %d)", ok ? "done" : "failed or refused", code);
	return ok;
}

/* the camera is taken out of Windows again (the user is asked: UAC) */
static void remove_camera(void)
{
	char dst[1200], dir[1200], cmd_exe[900], regsvr[900], args[9000];
	if (!camera_installed_path(dst, sizeof(dst)))
		return;
	snprintf(dir, sizeof(dir), "%s", dst);
	char *slash = strrchr(dir, '\\');
	if (slash)
		*slash = '\0';
	system_program("cmd.exe", cmd_exe, sizeof(cmd_exe));
	system_program("regsvr32.exe", regsvr, sizeof(regsvr));
	if (strpbrk(dst, "%^!\"&") || strpbrk(regsvr, "%^!\"&"))
		return;
	int n = snprintf(args, sizeof(args),
			 "/c \"\"%s\" /u /s \"%s\" & del /f /q \"%s\" >nul 2>&1 & rmdir \"%s\" >nul 2>&1\"", regsvr, dst, dst,
			 dir);
	if (n < 0 || n >= (int)sizeof(args))
		return;
	run_elevated(cmd_exe, args);
	bool gone = !camera_installed();
	app_log("virtual camera: removal %s", gone ? "done" : "failed or refused");
	set_notice(gone ? "Sanal kamera Windows'tan kaldırıldı" : "Sanal kamera kaldırılamadı", false);
}

/* "Relios Mirror" under HKCU\...\Run: started with Windows (its window minimized) */
static const wchar_t run_key[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";

static bool autostart_get(void)
{
	HKEY key;
	bool on = false;
	if (RegOpenKeyExW(HKEY_CURRENT_USER, run_key, 0, KEY_QUERY_VALUE, &key) == ERROR_SUCCESS) {
		on = RegQueryValueExW(key, L"Relios Mirror", NULL, NULL, NULL, NULL) == ERROR_SUCCESS;
		RegCloseKey(key);
	}
	return on;
}

static void autostart_set(bool on)
{
	HKEY key;
	if (RegCreateKeyExW(HKEY_CURRENT_USER, run_key, 0, NULL, 0, KEY_SET_VALUE, NULL, &key, NULL) != ERROR_SUCCESS)
		return;
	if (on) {
		/* (the one-file program if this was started by it: it finds the newest version's folder) */
		static wchar_t exe[2048], command[2200];
		const wchar_t *launcher = _wgetenv(L"RELIOS_LAUNCHER");
		if (launcher && launcher[0])
			_snwprintf(exe, 2047, L"%ls", launcher);
		else
			GetModuleFileNameW(NULL, exe, 2047);
		exe[2047] = 0;
		_snwprintf(command, 2199, L"\"%ls\" --tepsi", exe);
		command[2199] = 0;
		RegSetValueExW(key, L"Relios Mirror", 0, REG_SZ, (const BYTE *)command,
			       (DWORD)((wcslen(command) + 1) * sizeof(wchar_t)));
	} else {
		RegDeleteValueW(key, L"Relios Mirror");
	}
	RegCloseKey(key);
}
#else
static bool install_camera(void)
{
	return false;
}
static void remove_camera(void)
{
}
static bool autostart_get(void)
{
	return false;
}
static void autostart_set(bool on)
{
	(void)on;
}
#endif

/* a question with "Evet" and "Vazgeç" (Esc: Vazgeç; Enter: Vazgeç too, unless yes_is_default); true: Evet */
static bool confirm(const char *text, bool yes_is_default)
{
	const SDL_MessageBoxButtonData buttons[] = {
		{SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT | (yes_is_default ? 0 : SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT), 0,
		 "Vazgeç"},
		{yes_is_default ? SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT : 0, 1, "Evet"},
	};
	SDL_MessageBoxData data;
	int hit = 0;
	SDL_zero(data);
	data.flags = SDL_MESSAGEBOX_WARNING;
	data.window = A.win;
	data.title = APP_NAME;
	data.message = text;
	data.numbuttons = 2;
	data.buttons = buttons;
	return SDL_ShowMessageBox(&data, &hit) == 0 && hit == 1;
}

/* everything as at the first start: the settings, and no paired device (the firewall permission and an installed
 * virtual camera stay as they are) */
static void reset_everything(void)
{
	if (!confirm("Bütün ayarlar ilk açılıştaki haline dönecek ve eşleşmiş cihazlar unutulacak.\n"
		     "(Güvenlik duvarı izni ve kurulu sanal kamera yerinde kalır.)\n\nDevam edilsin mi?",
		     false))
		return;
	session_stop();
	A.restart_at = 0;
	A.apply_at = 0;
	SDL_LockMutex(R.mutex);
	R.want = false;
	R.failed = false;
	rec_close_locked();
	SDL_AtomicSet(&rec_on, 0);
	SDL_UnlockMutex(R.mutex);
	SDL_AtomicSet(&A.vcam_on, 0);
	A.camera_saved = false;
	A.camera_arming = false;
	struct rec_item item;
	memset(&item, 0, sizeof(item));
	item.kind = REC_SYNC;
	rec_push(&item);
	remove("ayarlar.txt");
	remove("paired-devices.txt");
	remove("receiver-key.pem");
	remove("device-id.txt");
	autostart_set(false);
	SDL_LockMutex(A.mutex);
	A.audio_device[0] = '\0';
	A.pair_requested = false;
	A.pin[0] = '\0';
	SDL_UnlockMutex(A.mutex);
	A.custom_folder[0] = '\0';
	SDL_AtomicSet(&A.fit169, 0);
	SDL_AtomicSet(&A.audio_reopen, 1);
	load_settings();
	find_video_folder();
	SDL_SetWindowAlwaysOnTop(A.win, SDL_FALSE);
	editing_kbit = false;
	settings_tab = 0;
	save_settings();
	session_start();
	set_notice("Ayarlar ilk açılıştaki haline döndü", false);
}

static void run_command(int cmd, int arg)
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
		/* (these: for the next recording) */
		A.rec_gpu = arg != 0;
		save_settings();
		break;
	case CMD_REC_FORMAT:
		A.rec_format = arg >= 0 && arg <= 2 ? arg : 0;
		save_settings();
		break;
	case CMD_REC_MODE:
		A.rec_mode = arg >= 0 && arg <= 2 ? arg : 0;
		editing_kbit = false;
		save_settings();
		break;
	case CMD_REC_BITRATE:
		editing_kbit = true;
		kbit_text[0] = '\0';
		break;
	case CMD_REC_AUDIO:
		A.rec_audio = !A.rec_audio;
		save_settings();
		break;
	case CMD_OPEN_FOLDER:
		open_video_folder();
		break;
	case CMD_CHOOSE_FOLDER:
		choose_video_folder();
		break;
	case CMD_SETTINGS:
		settings_open = !settings_open;
		editing_kbit = false;
		break;
	case CMD_TAB:
		settings_tab = arg >= 0 && arg <= 3 ? arg : 0;
		editing_kbit = false;
		break;
	case CMD_FORGET:
		if (!confirm("Eşleşmiş cihazlar unutulacak; yeniden bağlanırken kod sorulacak.\n\nDevam edilsin mi?", false))
			break;
		remove("paired-devices.txt");
		SDL_LockMutex(A.mutex);
		A.pair_requested = false;
		SDL_UnlockMutex(A.mutex);
		restart = true;
		break;
	case CMD_PAIR: {
		/* on / off: while on, one new device can pair with the code shown here */
		int paired = 0;
		paired_names(&paired);
		if (paired == 0) {
			/* with no paired device it stays on: the first device always pairs with the code (and after three
			 * wrong codes this switches it on again) */
			SDL_LockMutex(A.mutex);
			if (A.state == MIRROR_PAIRING_OFF)
				A.state = MIRROR_LISTENING;
			SDL_UnlockMutex(A.mutex);
			SDL_AtomicSet(&A.register_ok, 1);
			if (A.proc)
				proc_write(A.proc, "allow_pairing 1\n");
			set_notice("Eşleşmiş cihaz yokken eşleştirme açık kalır; ilk cihaz kodla eşleşir", false);
			break;
		}
		SDL_LockMutex(A.mutex);
		A.pair_requested = !A.pair_requested;
		bool on = A.pair_requested;
		if (on && A.state == MIRROR_PAIRING_OFF)
			A.state = MIRROR_LISTENING;
		if (!on)
			A.pin[0] = '\0';
		SDL_UnlockMutex(A.mutex);
		SDL_AtomicSet(&A.register_ok, on ? 1 : 0);
		if (A.proc)
			proc_write(A.proc, on ? "allow_pairing 1\n" : "allow_pairing 0\n");
		break;
	}
	case CMD_HIDE:
		A.hide_connected = !A.hide_connected;
		save_settings();
		if (A.proc)
			proc_write(A.proc, A.hide_connected ? "hide_connected 1\n" : "hide_connected 0\n");
		if (A.hide_connected)
			set_notice("Yansıtma başladıktan 10 sn sonra ad listeden çekilir; yansıtma kesilirse seçenek kendini kapatır",
				   false);
		break;
	case CMD_SCREENSHOT: {
		struct rec_item item;
		memset(&item, 0, sizeof(item));
		item.kind = REC_SHOT;
		SDL_LockMutex(A.frame_mutex);
		if (A.have_frame && A.last->data[0]) {
			item.frame = av_frame_clone(A.last);
			item.full = A.last_full;
			item.bt709 = A.last_709;
		}
		SDL_UnlockMutex(A.frame_mutex);
		if (!item.frame)
			set_notice("Ekran görüntüsü için önce cihazı yansıtın", false);
		else if (!rec_push(&item))
			av_frame_free(&item.frame);
		break;
	}
	case CMD_AUDIO_DEVICE: {
		/* the next output: default, then each one Windows lists */
		int count = SDL_GetNumAudioDevices(0), at = -1;
		for (int i = 0; i < count; i++) {
			const char *name = SDL_GetAudioDeviceName(i, 0);
			if (name && !strcmp(name, A.audio_device))
				at = i;
		}
		const char *next = at + 1 < count ? SDL_GetAudioDeviceName(at + 1, 0) : NULL;
		SDL_LockMutex(A.mutex);
		snprintf(A.audio_device, sizeof(A.audio_device), "%s", next ? next : "");
		SDL_UnlockMutex(A.mutex);
		SDL_AtomicSet(&A.audio_reopen, 1);
		save_settings();
		break;
	}
	case CMD_VCAM: {
		bool on = !SDL_AtomicGet(&A.vcam_on);
		if (on && !install_camera()) {
			set_notice("Sanal kamera kurulamadı (izin verilmedi ya da bu sürümde kamera dosyası yok)", false);
			break;
		}
		SDL_AtomicSet(&A.vcam_on, on ? 1 : 0);
		A.camera_saved = on;
		A.camera_arming = false;
		save_settings();
		struct rec_item item;
		memset(&item, 0, sizeof(item));
		item.kind = REC_SYNC;
		rec_push(&item);
		if (on)
			set_notice("Sanal kamera açık: programlarda \"Relios Mirror Camera\"yı seçin", false);
		break;
	}
	case CMD_VCAM_REMOVE: {
		if (!confirm("Sanal kamera Windows'tan kaldırılacak.\n\nDevam edilsin mi?", false))
			break;
		SDL_AtomicSet(&A.vcam_on, 0);
		A.camera_saved = false;
		A.camera_arming = false;
		save_settings();
		struct rec_item item;
		memset(&item, 0, sizeof(item));
		item.kind = REC_SYNC;
		rec_push(&item);
		remove_camera();
		break;
	}
	case CMD_ASPECT:
		/* (the window and the virtual camera at once; a recording from its next file) */
		SDL_AtomicSet(&A.fit169, arg != 0);
		save_settings();
		break;
	case CMD_AUTOSTART:
		autostart_set(!autostart_get());
		break;
	case CMD_RESET:
		reset_everything();
		break;
	case CMD_RES_NEXT: {
		static const int sizes[4][2] = {{1280, 720}, {1920, 1080}, {2560, 1440}, {3840, 2160}};
		int at = arg >= 0 && arg <= 3 ? arg : 1;
		restart = A.width != sizes[at][0] || A.height != sizes[at][1];
		A.width = sizes[at][0];
		A.height = sizes[at][1];
		break;
	}
	case CMD_FPS_NEXT: {
		int fps = arg == 30 ? 30 : 60;
		restart = A.fps != fps;
		A.fps = fps;
		break;
	}
	case CMD_CODEC_NEXT:
		restart = A.h265 != (arg != 0);
		A.h265 = arg != 0;
		break;
	case CMD_HW_DECODE:
		restart = A.hw_decode != (arg != 0);
		A.hw_decode = arg != 0;
		break;
	default:
		break;
	}
	if (restart) {
		/* the device learns the new size / rate / codec when it connects again: the receiver restarts a moment
		 * after the last click */
		save_settings();
		A.apply_at = SDL_GetTicks() + 1500;
	}
	update_title();
}

/* typing the recording's bit rate: digits, Backspace, Enter (accept), Esc (leave as it was) */
static void kbit_key(SDL_Keycode key)
{
	int digit = -1;
	size_t len = strlen(kbit_text);
	if (key >= SDLK_0 && key <= SDLK_9)
		digit = (int)(key - SDLK_0);
	else if (key >= SDLK_KP_1 && key <= SDLK_KP_9)
		digit = (int)(key - SDLK_KP_1) + 1;
	else if (key == SDLK_KP_0)
		digit = 0;
	if (digit >= 0 && len < 6) {
		kbit_text[len] = (char)('0' + digit);
		kbit_text[len + 1] = '\0';
	} else if (key == SDLK_BACKSPACE && len > 0) {
		kbit_text[len - 1] = '\0';
	} else if (key == SDLK_RETURN || key == SDLK_KP_ENTER) {
		int v = atoi(kbit_text);
		if (v > 0) {
			A.rec_kbit = v < 500 ? 500 : v > 200000 ? 200000 : v;
			save_settings();
		}
		editing_kbit = false;
	} else if (key == SDLK_ESCAPE) {
		editing_kbit = false;
	}
}

static int key_command(SDL_Keycode key)
{
	switch (key) {
	case SDLK_f:
	case SDLK_F11:
		return CMD_FULLSCREEN;
	case SDLK_ESCAPE:
		return settings_open ? CMD_SETTINGS : A.fullscreen ? CMD_FULLSCREEN : CMD_NONE;
	case SDLK_m:
		return CMD_MUTE;
	case SDLK_UP:
		return CMD_VOL_UP;
	case SDLK_DOWN:
		return CMD_VOL_DOWN;
	case SDLK_r:
		return CMD_RECORD;
	case SDLK_s:
		return CMD_SCREENSHOT;
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
	char netsh[900], cmd_exe[900];
	system_program("netsh.exe", netsh, sizeof(netsh));
	system_program("cmd.exe", cmd_exe, sizeof(cmd_exe));
	/* (the rules name the programs: made again if the folder moved; "v2": with the rules that keep both
	 * programs off the internet) */
	char self[2100], marker[2200];
	char *folder = SDL_GetBasePath();
	snprintf(self, sizeof(self), "%sReliosMirror.exe", folder ? folder : "");
	SDL_free(folder);
	snprintf(marker, sizeof(marker), "v2|%s", A.exe);
	char *made_for = read_text("firewall.txt");
	bool same = made_for && !strcmp(made_for, marker);
	free(made_for);
	if (same && proc_run(netsh, "advfirewall firewall show rule name=\"Relios Mirror\"", 10000) == 0)
		return;
	if (strpbrk(A.exe, "%^!\"&") || strpbrk(netsh, "%^!\"&") || strpbrk(self, "%^!\"&")) {
		app_log("firewall rule not made by the program: unusual letters in a folder's name");
		return;
	}
	SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_INFORMATION, APP_NAME,
				 "Apple cihazının bağlanabilmesi için Windows Güvenlik Duvarı'nda izin gerekiyor.\n\n"
				 "İzin yalnızca alıcı programa ve yalnızca yerel ağdan gelen bağlantılara verilir;\n"
				 "ayrıca programın internete çıkışı Windows Güvenlik Duvarı'nda kapatılır.\n"
				 "Tamam'a bastıktan sonra çıkacak Windows sorusunda \"Evet\"i seçin.",
				 A.win);
	/* Out: nothing of the two programs leaves for the internet (every address that is not a private, link-local
	 * or multicast one is blocked; a block rule wins over any allow rule). In: the receiver, from the local
	 * network only. The rule that mirroring needs comes last: its result is the command's result. */
	static const char internet4[] = "0.0.0.0-9.255.255.255,11.0.0.0-100.63.255.255,100.128.0.0-169.253.255.255,"
					"169.255.0.0-172.15.255.255,172.32.0.0-192.167.255.255,192.169.0.0-223.255.255.255";
	static const char internet6[] = "2000::/3";
	char cmd[12000];
	snprintf(cmd, sizeof(cmd),
		 "/c \"\"%s\" advfirewall firewall delete rule name=\"Relios Mirror\""
		 " & \"%s\" advfirewall firewall delete rule name=all program=\"%s\""
		 " & \"%s\" advfirewall firewall add rule name=\"Relios Mirror\" dir=out action=block program=\"%s\" remoteip=%s enable=yes"
		 " & \"%s\" advfirewall firewall add rule name=\"Relios Mirror\" dir=out action=block program=\"%s\" remoteip=%s enable=yes"
		 " & \"%s\" advfirewall firewall add rule name=\"Relios Mirror\" dir=out action=block program=\"%s\" remoteip=%s enable=yes"
		 " & \"%s\" advfirewall firewall add rule name=\"Relios Mirror\" dir=out action=block program=\"%s\" remoteip=%s enable=yes"
		 " & \"%s\" advfirewall firewall add rule name=\"Relios Mirror\" dir=in action=allow program=\"%s\" remoteip=localsubnet enable=yes\"",
		 netsh, netsh, A.exe, netsh, A.exe, internet6, netsh, self, internet6, netsh, A.exe, internet4, netsh, self,
		 internet4, netsh, A.exe);
	if (strlen(cmd) + 1 >= sizeof(cmd)) {
		app_log("firewall rules not made: the folder names are too long");
		return;
	}
	if (run_elevated(cmd_exe, cmd) == 0) {
		FILE *f = fopen("firewall.txt", "w");
		if (f) {
			fputs(marker, f);
			fclose(f);
		}
		app_log("firewall rules made (in: the receiver, local network only; out: no internet for both programs)");
	} else {
		app_log("firewall rule not made (refused, or an error)");
	}
}
#endif

#ifdef _WIN32
/* Other running copies of this program (or of its receiver). With two, the Apple device reaches now one, now the
 * other (on Windows both can listen on the same port), and nothing works as expected. Counts them; ends them if
 * asked to. (Not this process, and not the one-file starter that started it.) */
static int other_copies(bool end_them)
{
	DWORD self = GetCurrentProcessId(), parent = 0;
	int found = 0;
	HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
	if (snap == INVALID_HANDLE_VALUE)
		return 0;
	PROCESSENTRY32W e;
	memset(&e, 0, sizeof(e));
	e.dwSize = sizeof(e);
	if (Process32FirstW(snap, &e)) {
		do {
			if (e.th32ProcessID == self)
				parent = e.th32ParentProcessID;
		} while (Process32NextW(snap, &e));
	}
	memset(&e, 0, sizeof(e));
	e.dwSize = sizeof(e);
	if (Process32FirstW(snap, &e)) {
		do {
			bool program = !_wcsicmp(e.szExeFile, L"ReliosMirror.exe");
			bool receiver = !_wcsicmp(e.szExeFile, L"relios-airplay-receiver.exe");
			if ((!program && !receiver) || e.th32ProcessID == self || (program && e.th32ProcessID == parent))
				continue;
			found++;
			if (end_them) {
				HANDLE process = OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE, FALSE, e.th32ProcessID);
				if (process) {
					TerminateProcess(process, 0);
					WaitForSingleObject(process, 3000);
					CloseHandle(process);
				}
			}
		} while (Process32NextW(snap, &e));
	}
	CloseHandle(snap);
	return found;
}
#endif

int main(int argc, char *argv[])
{
	SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "1");
	/* (the program may stay open all day: the screen may switch off and the computer may sleep, except while
	 * something is mirrored; see the loop) */
	SDL_SetHint(SDL_HINT_VIDEO_ALLOW_SCREENSAVER, "1");
	if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_EVENTS) != 0) {
		SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, APP_NAME, SDL_GetError(), NULL);
		return 1;
	}
	TTF_Init();
#ifdef _WIN32
	if (other_copies(false) > 0) {
		if (!confirm("Relios Mirror (ya da OBS'teki iPad Yansıtma) zaten çalışıyor; penceresi simge durumunda ya da\n"
			     "gizli olabilir. İkisi aynı anda açıkken Apple cihazı doğru bağlanamaz.\n\n"
			     "Öteki kapatılıp bu açılsın mı?",
			     true))
			return 0;
		other_copies(true);
		SDL_Delay(500);
	}
#endif
	A.mutex = SDL_CreateMutex();
	A.frame_mutex = SDL_CreateMutex();
	R.mutex = SDL_CreateMutex();
	Q.mutex = SDL_CreateMutex();
	Q.cond = SDL_CreateCond();
	Q.thread = SDL_CreateThread(rec_thread, "recording", NULL);
	log_mutex = SDL_CreateMutex();
	A.event_type = SDL_RegisterEvents(1);
	A.pending = av_frame_alloc();
	A.upload = av_frame_alloc();
	A.last = av_frame_alloc();
	if (!A.pending || !A.upload || !A.last)
		return 1;

	char *base = SDL_GetBasePath();
	snprintf(A.exe, sizeof(A.exe), "%s" RECEIVER_EXE, base ? base : "");
	SDL_free(base);

	if (!enter_config_folder()) {
		SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, APP_NAME, "Ayar klasörü açılamadı.", NULL);
		return 1;
	}
	remove("log-onceki.txt"); /* (the log of the run before is kept: what went wrong can be looked up) */
	rename("log.txt", "log-onceki.txt");
	log_file = fopen("log.txt", "w");
	app_log("Relios Mirror started; receiver: %s", A.exe);
	av_log_set_level(AV_LOG_ERROR);
	load_settings();
	find_video_folder();
	A.hover = -1;
	if (A.camera_saved) {
		/* The camera was left on: it is switched on again, and remembered again only once it runs (a fault in it
		 * could otherwise repeat at every start). */
		A.camera_saved = false;
		save_settings();
		if (camera_installed()) {
			struct rec_item item;
			memset(&item, 0, sizeof(item));
			item.kind = REC_SYNC;
			SDL_AtomicSet(&A.vcam_on, 1);
			A.camera_arming = true;
			rec_push(&item);
		}
	}
	if (autostart_get())
		autostart_set(true); /* (starting with Windows follows the copy that was used last) */
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
	SDL_SetWindowMinimumSize(A.win, 900, 540);
	if (A.on_top)
		SDL_SetWindowAlwaysOnTop(A.win, SDL_TRUE);
	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--tepsi"))
			SDL_MinimizeWindow(A.win); /* started with Windows */
	}
	{
		char path[2100];
		char *folder = SDL_GetBasePath();
		snprintf(path, sizeof(path), "%slogo.bmp", folder ? folder : "");
		SDL_free(folder);
		SDL_Surface *logo = SDL_LoadBMP(path);
		if (logo) {
			SDL_SetWindowIcon(A.win, logo);
			A.logo = SDL_CreateTextureFromSurface(A.ren, logo);
			SDL_FreeSurface(logo);
		}
	}

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
				int cmd = CMD_NONE, arg = 0;
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
					bool over = false;
					const struct button *under = button_at(e.motion.x, e.motion.y, &over);
					int hover = under ? (int)(under - buttons) : -1;
					if (!A.bar_drawn || hover != A.hover)
						redraw = true; /* (what the mouse is over lights up) */
					A.hover = hover;
				} else if (e.type == SDL_KEYDOWN && editing_kbit) {
					kbit_key(e.key.keysym.sym);
					redraw = true;
				} else if (e.type == SDL_KEYDOWN &&
					   (!e.key.repeat || e.key.keysym.sym == SDLK_UP || e.key.keysym.sym == SDLK_DOWN)) {
					cmd = key_command(e.key.keysym.sym);
				} else if (e.type == SDL_MOUSEBUTTONDOWN && e.button.button == SDL_BUTTON_LEFT) {
					A.bar_until = SDL_GetTicks() + 2500;
					bool inside = false;
					if (A.notice_drawn && A.notice_opens_folder) {
						int ow = 0, oh = 0, ww = 0, wh = 0;
						SDL_GetRendererOutputSize(A.ren, &ow, &oh);
						SDL_GetWindowSize(A.win, &ww, &wh);
						SDL_Point p = {ww > 0 ? e.button.x * ow / ww : 0, wh > 0 ? e.button.y * oh / wh : 0};
						if (SDL_PointInRect(&p, &A.notice_rect)) {
							open_video_folder();
							SDL_LockMutex(A.mutex);
							A.notice[0] = '\0';
							SDL_UnlockMutex(A.mutex);
							redraw = true;
							continue;
						}
					}
					if (editing_kbit)
						kbit_key(SDLK_RETURN); /* a click elsewhere accepts what was typed */
					const struct button *hit = button_at(e.button.x, e.button.y, &inside);
					if (hit) {
						cmd = hit->cmd;
						arg = hit->arg;
					}
					if (cmd == CMD_NONE && settings_open && !inside)
						cmd = CMD_SETTINGS; /* a click beside the panel closes it */
					else if (cmd == CMD_NONE && !inside && e.button.clicks == 2)
						cmd = CMD_FULLSCREEN;
					redraw = true;
				}
				if (cmd != CMD_NONE) {
					run_command(cmd, arg);
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
		if (A.camera_arming) {
			if (SDL_AtomicGet(&A.camera_ok)) {
				A.camera_arming = false;
				A.camera_saved = true;
				save_settings();
			} else if (!SDL_AtomicGet(&A.vcam_on)) {
				A.camera_arming = false;
			}
		}
		if (SDL_AtomicCAS(&A.hide_broke, 1, 0) && A.hide_connected) {
			/* hiding the name ended the mirroring: the option is switched off (until someone switches it on) */
			A.hide_connected = false;
			save_settings();
			if (A.proc)
				proc_write(A.proc, "hide_connected 0\n");
			set_notice("Yansıtma, ad gizlenince kesildi: \"Bağlıyken Listeden Gizle\" kapatıldı. Cihazdan yeniden bağlanın.",
				   false);
			SDL_LockMutex(A.mutex);
			A.notice_until = SDL_GetTicks() + 20000;
			SDL_UnlockMutex(A.mutex);
			app_log("hiding the name while mirroring ended the mirroring: the option was switched off");
			redraw = retitle = true;
		}
		{
			SDL_LockMutex(A.frame_mutex);
			bool mirroring = A.have_frame;
			SDL_UnlockMutex(A.frame_mutex);
			if (mirroring != A.keep_awake) {
				A.keep_awake = mirroring;
				if (mirroring)
					SDL_DisableScreenSaver();
				else
					SDL_EnableScreenSaver();
			}
		}
		if (A.bar_drawn != bar_wanted())
			redraw = true;
		if (A.notice_drawn && SDL_TICKS_PASSED(now, A.notice_until))
			redraw = true;
		if (retitle)
			update_title();
		if (redraw)
			draw();
	}

	session_stop();
	SDL_LockMutex(Q.mutex);
	Q.quit = true;
	SDL_CondSignal(Q.cond);
	SDL_UnlockMutex(Q.mutex);
	if (Q.thread)
		SDL_WaitThread(Q.thread, NULL);
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

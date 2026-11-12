/*
 * relios-airplay-receiver: ReliosOBS's iPad mirroring receiver (trial).
 *
 * A separate program so that what comes from the network is handled outside OBS: OBS starts it at low
 * integrity in a job that ends with OBS, only while the user has pressed "Start mirroring". It receives the
 * iPad's AirPlay screen mirroring with UxPlay's server library (uxplay/lib, with ReliosOBS's changes) and
 * hands the encoded video and audio to OBS through its standard output (protocol.h). It decodes nothing.
 *
 * Who may connect:
 *  - only addresses on the local network (lib/relios_guard.c);
 *  - only devices paired with OBS (their keys are sent by OBS on standard input), or a new device that
 *    enters the PIN shown in OBS while OBS allows pairing a new device; three wrong PINs turn pairing off;
 *  - the video stream only from the device that set it up.
 * When the paired device stops mirroring and disconnects, the receiver ends: its ports close.
 *
 *   relios-airplay-receiver --make-key <file>    makes the receiver's key (run by OBS, not at low integrity)
 *   relios-airplay-receiver --serve [--test-allow-loopback]
 *
 * GPL-3.0 (it links UxPlay's playfair code).
 */
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#include <winsock2.h>
#include <windows.h>
#endif
#include <pthread.h>
#include <unistd.h>

#include "raop.h"
#include "dnssd.h"
#include "logger.h"
#include "relios_guard.h"
#include "protocol.h"

#include <openssl/evp.h>
#include <openssl/pem.h>

#define MAX_REGISTERED 64

static pthread_mutex_t out_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t state_mutex = PTHREAD_MUTEX_INITIALIZER;

static char *registered[MAX_REGISTERED];
static int registered_count = 0;
static bool allow_pairing = false;
static bool hide_when_connected = false; /* Relios Mirror: not in the iPad's list while a paired device is connected */
static bool trusted_connected = false;
static bool visible = true; /* Relios Mirror: announced on the network at all */
static int pairing_failures = 0;
static int open_connections = 0;
static bool had_trusted_client = false;
static volatile bool quit_requested = false;
/* when the last connection of a trusted device closed (wall clock, ns); 0 while one is open. The receiver ends
 * RECONNECT_GRACE after it, so that the iPad can reconnect (it does after a first pairing, and when the screen
 * or the app changes); an iPad that went away for good closes the ports a little later. */
static volatile uint64_t idle_since = 0;
#define RECONNECT_GRACE_NS (45ull * 1000000000ull)

static char server_name[64] = "ReliosOBS";
static char device_id[32] = "";
static char keyfile[1024] = "";
static int req_width = 1920, req_height = 1080, req_fps = 60;
static bool offer_h265 = false; /* "h265 1": the iPad may send H.265 (AirPlay feature bit 42) */

/* ------------------------------------------------------------------ output to OBS */

static uint64_t wall_ns(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_REALTIME, &ts);
	return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static void send_message(uint8_t type, const void *head, size_t head_len, const void *data, size_t data_len)
{
	size_t len = head_len + data_len;
	if (len > RA_MAX_PAYLOAD)
		return;
	uint8_t header[RA_HEADER_SIZE] = {RA_MAGIC0, RA_MAGIC1, type, 0, (uint8_t)(len & 0xff),
					  (uint8_t)((len >> 8) & 0xff), (uint8_t)((len >> 16) & 0xff),
					  (uint8_t)((len >> 24) & 0xff)};
	pthread_mutex_lock(&out_mutex);
	bool ok = fwrite(header, 1, sizeof(header), stdout) == sizeof(header);
	if (ok && head_len)
		ok = fwrite(head, 1, head_len, stdout) == head_len;
	if (ok && data_len)
		ok = fwrite(data, 1, data_len, stdout) == data_len;
	fflush(stdout);
	pthread_mutex_unlock(&out_mutex);
	if (!ok)
		quit_requested = true; /* OBS is gone */
}

static void send_text(uint8_t type, const char *fmt, ...)
{
	char text[1024];
	va_list args;
	va_start(args, fmt);
	vsnprintf(text, sizeof(text), fmt, args);
	va_end(args);
	send_message(type, NULL, 0, text, strlen(text));
}

static void log_line(const char *fmt, ...)
{
	char text[2048];
	va_list args;
	va_start(args, fmt);
	vsnprintf(text, sizeof(text), fmt, args);
	va_end(args);
	/* one line per message: OBS reads standard error line by line */
	for (char *p = text; *p; p++) {
		if (*p == '\r' || *p == '\n')
			*p = ' ';
	}
	fprintf(stderr, "%s\n", text);
	fflush(stderr);
}

/* ------------------------------------------------------------------ the library's callbacks */

static void cb_log(void *cls, int level, const char *msg)
{
	(void)cls;
	const char *name = level <= LOGGER_ERR ? "error" : level == LOGGER_WARNING ? "warning" : "info";
	/* the pairing code is shown in OBS, never written to a log */
	if (msg && strstr(msg, "ENTER PIN")) {
		log_line("[%s] the iPad must now enter the code shown in OBS", name);
		return;
	}
	/* the iPad's own streaming reports (clientFPSdata): one every 10 seconds, without the XML layout */
	if (msg && !strncmp(msg, "<?xml", 5)) {
		static uint64_t last = 0;
		uint64_t now = wall_ns();
		if (last && now - last < 10000000000ull)
			return;
		last = now;
		char compact[1900];
		size_t n = 0;
		const char *p = strstr(msg, "<dict>");
		for (p = p ? p : msg; *p && n + 1 < sizeof(compact); p++) {
			if (!strncmp(p, "<key>", 5)) {
				if (n && n + 1 < sizeof(compact))
					compact[n++] = ' ';
				p += 4;
			} else if (!strncmp(p, "</key>", 6)) {
				compact[n++] = '=';
				p += 5;
			} else if (*p == '<') {
				while (*p && *p != '>')
					p++;
				if (!*p)
					break;
			} else if (*p != '\n' && *p != '\r' && *p != '\t') {
				compact[n++] = *p;
			}
		}
		compact[n] = '\0';
		log_line("[info] iPad's streaming report: %s", compact);
		return;
	}
	log_line("[%s] %s", name, msg);
}

static void cb_video_process(void *cls, raop_ntp_t *ntp, video_decode_struct *data)
{
	(void)cls;
	(void)ntp;
	if (!data || !data->data || data->data_len <= 0)
		return;
	struct ra_video_header h = {0};
	h.ntp_time_remote = data->ntp_time_remote;
	h.ntp_time_local = data->ntp_time_local;
	h.received_local = wall_ns();
	h.is_h265 = data->is_h265 ? 1 : 0;
	h.nal_count = data->nal_count;
	send_message(RA_MSG_VIDEO, &h, sizeof(h), data->data, (size_t)data->data_len);
}

static void cb_audio_process(void *cls, raop_ntp_t *ntp, audio_decode_struct *data)
{
	(void)cls;
	(void)ntp;
	if (!data || !data->data || data->data_len <= 0)
		return;
	struct ra_audio_header h = {0};
	h.ntp_time_remote = data->ntp_time_remote;
	h.ntp_time_local = data->ntp_time_local;
	h.received_local = wall_ns();
	h.rtp_time = data->rtp_time;
	h.seqnum = data->seqnum;
	h.ct = data->ct;
	send_message(RA_MSG_AUDIO, &h, sizeof(h), data->data, (size_t)data->data_len);
}

static int cb_video_set_codec_and_size(void *cls, video_codec_t codec, unsigned short width_source,
				       unsigned short height_source, unsigned short x_offset, unsigned short y_offset,
				       unsigned short width, unsigned short height)
{
	(void)cls;
	struct ra_video_format f = {0};
	f.codec = codec == VIDEO_CODEC_H265 ? 2 : 1;
	f.width_source = width_source;
	f.height_source = height_source;
	f.x_offset = x_offset;
	f.y_offset = y_offset;
	f.width = width;
	f.height = height;
	send_message(RA_MSG_VIDEO_FORMAT, &f, sizeof(f), NULL, 0);
	return 0;
}

static void cb_conn_init(void *cls)
{
	(void)cls;
	pthread_mutex_lock(&state_mutex);
	open_connections++;
	idle_since = 0;
	pthread_mutex_unlock(&state_mutex);
}

static void cb_conn_destroy(void *cls)
{
	(void)cls;
	pthread_mutex_lock(&state_mutex);
	open_connections--;
	if (open_connections <= 0)
		trusted_connected = false;
	bool idle = open_connections <= 0 && had_trusted_client;
	if (idle)
		idle_since = wall_ns();
	pthread_mutex_unlock(&state_mutex);
	if (idle)
		log_line("[info] the iPad disconnected; waiting %d s for it to reconnect", (int)(RECONNECT_GRACE_NS / 1000000000ull));
}

static void cb_conn_reset(void *cls, int reason)
{
	(void)cls;
	log_line("[info] connection reset (reason %d)", reason);
}

static void cb_conn_feedback(void *cls)
{
	(void)cls;
}

static void cb_mirror_video_running(void *cls, bool is_running)
{
	(void)cls;
	send_text(RA_MSG_STATUS, "mirroring %s", is_running ? "on" : "off");
}

static void cb_report_client_request(void *cls, char *deviceid, char *model, char *name, bool *admit)
{
	(void)cls;
	(void)deviceid;
	/* (the library only gets here for a connection that is trusted: paired with the PIN or paired before) */
	*admit = true;
	send_text(RA_MSG_STATUS, "client name=%s model=%s", name ? name : "?", model ? model : "?");
}

static void cb_display_pin(void *cls, char *pin)
{
	(void)cls;
	send_text(RA_MSG_PIN, "%s", pin ? pin : "");
}

static void cb_register_client(void *cls, const char *dev_id, const char *pk_str, const char *name)
{
	(void)cls;
	if (!pk_str)
		return;
	pthread_mutex_lock(&state_mutex);
	bool known = false;
	for (int i = 0; i < registered_count; i++) {
		if (!strcmp(registered[i], pk_str))
			known = true;
	}
	if (!known && registered_count < MAX_REGISTERED)
		registered[registered_count++] = strdup(pk_str);
	pthread_mutex_unlock(&state_mutex);
	if (!known)
		send_text(RA_MSG_REGISTER, "%s\t%s\t%s", pk_str, dev_id ? dev_id : "", name ? name : "");
}

static bool cb_check_register(void *cls, const char *pk_str)
{
	(void)cls;
	bool found = false;
	pthread_mutex_lock(&state_mutex);
	for (int i = 0; pk_str && i < registered_count; i++) {
		if (!strcmp(registered[i], pk_str))
			found = true;
	}
	pthread_mutex_unlock(&state_mutex);
	return found;
}

static bool cb_allow_new_pairing(void *cls)
{
	(void)cls;
	pthread_mutex_lock(&state_mutex);
	bool allowed = allow_pairing;
	pthread_mutex_unlock(&state_mutex);
	return allowed;
}

static void cb_pairing_failed(void *cls)
{
	(void)cls;
	pthread_mutex_lock(&state_mutex);
	int failures = ++pairing_failures;
	if (failures >= 3)
		allow_pairing = false;
	pthread_mutex_unlock(&state_mutex);
	send_text(RA_MSG_PIN, "");
	send_text(RA_MSG_STATUS, "pairing_failed count=%d", failures);
	if (failures >= 3)
		send_text(RA_MSG_STATUS, "pairing_off");
}

static void cb_client_trusted(void *cls, bool new_pairing)
{
	(void)cls;
	pthread_mutex_lock(&state_mutex);
	had_trusted_client = true;
	trusted_connected = true;
	if (new_pairing) {
		allow_pairing = false; /* one new device per "Pair a new iPad" */
		pairing_failures = 0;
	}
	pthread_mutex_unlock(&state_mutex);
	send_text(RA_MSG_PIN, "");
	send_text(RA_MSG_STATUS, "trusted new=%d", new_pairing ? 1 : 0);
}

static const char *cb_passwd(void *cls, int *len)
{
	(void)cls;
	*len = 0; /* no password: the PIN pairing is the access control */
	return NULL;
}

/* the library calls these without checking for NULL */
static void cb_noop(void *cls)
{
	(void)cls;
}
static void cb_video_reset(void *cls, reset_type_t type)
{
	(void)cls;
	(void)type;
}
static double cb_audio_set_client_volume(void *cls)
{
	(void)cls;
	return 0.0;
}
static void cb_audio_set_volume(void *cls, float volume)
{
	(void)cls;
	(void)volume;
}
static void cb_audio_buffer(void *cls, const void *buffer, int buflen)
{
	(void)cls;
	(void)buffer;
	(void)buflen;
}
static void cb_audio_set_progress(void *cls, uint32_t *start, uint32_t *curr, uint32_t *end)
{
	(void)cls;
	(void)start;
	(void)curr;
	(void)end;
}
static void cb_audio_get_format(void *cls, unsigned char *ct, unsigned short *spf, bool *using_screen,
				bool *is_media, uint64_t *audio_format)
{
	(void)cls;
	(void)spf;
	(void)is_media;
	(void)audio_format;
	log_line("[info] audio format: ct=%u%s", ct ? *ct : 0, using_screen && *using_screen ? " (with screen)" : "");
}
static void cb_export_dacp(void *cls, const char *active_remote, const char *dacp_id)
{
	(void)cls;
	(void)active_remote;
	(void)dacp_id;
}
static void cb_remote_control_id(void *cls, const char *dacp_id, const char *active_remote_header)
{
	(void)cls;
	(void)dacp_id;
	(void)active_remote_header;
}
static void cb_get_custom_profile(void *cls, const char **custom_profile)
{
	(void)cls;
	*custom_profile = NULL;
}

/* ------------------------------------------------------------------ input from OBS */

static void handle_command(char *line)
{
	char *nl = strpbrk(line, "\r\n");
	if (nl)
		*nl = '\0';
	if (!strncmp(line, "name ", 5)) {
		snprintf(server_name, sizeof(server_name), "%s", line + 5);
	} else if (!strncmp(line, "deviceid ", 9)) {
		snprintf(device_id, sizeof(device_id), "%s", line + 9);
	} else if (!strncmp(line, "keyfile ", 8)) {
		snprintf(keyfile, sizeof(keyfile), "%s", line + 8);
	} else if (!strncmp(line, "register ", 9)) {
		pthread_mutex_lock(&state_mutex);
		if (registered_count < MAX_REGISTERED && strlen(line + 9) > 0)
			registered[registered_count++] = strdup(line + 9);
		pthread_mutex_unlock(&state_mutex);
	} else if (!strncmp(line, "visible ", 8)) {
		pthread_mutex_lock(&state_mutex);
		visible = atoi(line + 8) != 0;
		pthread_mutex_unlock(&state_mutex);
	} else if (!strncmp(line, "hide_connected ", 15)) {
		pthread_mutex_lock(&state_mutex);
		hide_when_connected = atoi(line + 15) != 0;
		pthread_mutex_unlock(&state_mutex);
	} else if (!strncmp(line, "allow_pairing ", 14)) {
		pthread_mutex_lock(&state_mutex);
		allow_pairing = atoi(line + 14) != 0;
		if (allow_pairing)
			pairing_failures = 0;
		pthread_mutex_unlock(&state_mutex);
		log_line("[info] pairing a new device: %s", atoi(line + 14) ? "on" : "off");
	} else if (!strncmp(line, "size ", 5)) {
		int w = 0, h = 0, fps = 0;
		if (sscanf(line + 5, "%dx%d@%d", &w, &h, &fps) == 3 && w >= 320 && h >= 240 && w <= 3840 &&
		    h <= 2160 && fps >= 15 && fps <= 60) {
			req_width = w;
			req_height = h;
			req_fps = fps;
		}
	} else if (!strncmp(line, "h265 ", 5)) {
		offer_h265 = atoi(line + 5) != 0;
	} else if (!strcmp(line, "quit")) {
		quit_requested = true;
	}
}

static bool read_config(void)
{
	char line[4096];
	while (fgets(line, sizeof(line), stdin)) {
		if (!strncmp(line, "start", 5))
			return true;
		handle_command(line);
	}
	return false;
}

static void *stdin_thread(void *arg)
{
	(void)arg;
	char line[4096];
	while (fgets(line, sizeof(line), stdin))
		handle_command(line);
	quit_requested = true; /* standard input closed: OBS is gone or stopped mirroring */
	return NULL;
}

/* ------------------------------------------------------------------ */

static int make_key(const char *path)
{
	EVP_PKEY *pkey = NULL;
	EVP_PKEY_CTX *ctx = EVP_PKEY_CTX_new_id(EVP_PKEY_ED25519, NULL);
	if (!ctx || EVP_PKEY_keygen_init(ctx) <= 0 || EVP_PKEY_keygen(ctx, &pkey) <= 0) {
		log_line("[error] could not make a key");
		return 1;
	}
	EVP_PKEY_CTX_free(ctx);
	FILE *f = fopen(path, "wb");
	if (!f) {
		log_line("[error] could not write the key file");
		return 1;
	}
	int ok = PEM_write_PrivateKey(f, pkey, NULL, NULL, 0, NULL, NULL);
	fclose(f);
	EVP_PKEY_free(pkey);
	return ok ? 0 : 1;
}

static const char *integrity_level(void)
{
#ifdef _WIN32
	HANDLE token = NULL;
	static char level[16] = "unknown";
	if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
		unsigned char buffer[256];
		DWORD len = 0;
		if (GetTokenInformation(token, TokenIntegrityLevel, buffer, sizeof(buffer), &len)) {
			TOKEN_MANDATORY_LABEL *label = (TOKEN_MANDATORY_LABEL *)buffer;
			DWORD rid = *GetSidSubAuthority(label->Label.Sid,
							(DWORD)(UCHAR)(*GetSidSubAuthorityCount(label->Label.Sid) - 1));
			snprintf(level, sizeof(level), "%s",
				 rid < SECURITY_MANDATORY_LOW_RID      ? "untrusted"
				 : rid < SECURITY_MANDATORY_MEDIUM_RID ? "low"
				 : rid < SECURITY_MANDATORY_HIGH_RID   ? "medium"
								       : "high");
		}
		CloseHandle(token);
	}
	return level;
#else
	return "n/a";
#endif
}

static int parse_device_id(const char *text, char hw[6])
{
	unsigned int b[6];
	if (sscanf(text, "%2x:%2x:%2x:%2x:%2x:%2x", &b[0], &b[1], &b[2], &b[3], &b[4], &b[5]) != 6)
		return -1;
	for (int i = 0; i < 6; i++)
		hw[i] = (char)b[i];
	return 0;
}

static void set_features(dnssd_t *dnssd)
{
	/* UxPlay's set (uxplay.cpp start_dnssd), with PIN pairing and without HLS video; H.265 (bit 42,
	 * "SupportsScreenMultiCodec", as UxPlay's -h265) when OBS asks for it: newer iOS / iPadOS may otherwise
	 * not send a picture */
	static const int on[] = {1, 2, 5, 6, 7, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 25, 27, 28, 30};
	static const int off[] = {0, 3, 4, 8, 23, 24, 26, 29, 31};
	for (size_t i = 0; i < sizeof(on) / sizeof(on[0]); i++)
		dnssd_set_airplay_features(dnssd, on[i], 1);
	for (size_t i = 0; i < sizeof(off) / sizeof(off[0]); i++)
		dnssd_set_airplay_features(dnssd, off[i], 0);
	dnssd_set_airplay_features(dnssd, 42, offer_h265 ? 1 : 0);
	log_line("[info] video offered to the iPad: %s", offer_h265 ? "H.264 or H.265" : "H.264 only");
}

int main(int argc, char **argv)
{
	bool serve = false;
	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--make-key") && i + 1 < argc)
			return make_key(argv[i + 1]);
		if (!strcmp(argv[i], "--serve"))
			serve = true;
		else if (!strcmp(argv[i], "--test-allow-loopback"))
			relios_guard_allow_loopback(true);
	}
	if (!serve) {
		fprintf(stderr, "usage: %s --serve | --make-key <file>\n", argv[0]);
		return 2;
	}
#ifdef _WIN32
	_setmode(_fileno(stdout), _O_BINARY);
	_setmode(_fileno(stdin), _O_BINARY);
#endif
	if (!read_config()) {
		log_line("[error] no configuration on standard input");
		return 2;
	}
	send_text(RA_MSG_STATUS, "integrity level=%s", integrity_level());

	char hw_addr[6];
	if (parse_device_id(device_id, hw_addr) < 0) {
		send_text(RA_MSG_STATUS, "error text=bad device id");
		return 2;
	}
	if (!keyfile[0]) {
		send_text(RA_MSG_STATUS, "error text=no key file");
		return 2;
	}

	ntp_global_init();

	raop_callbacks_t cbs;
	memset(&cbs, 0, sizeof(cbs));
	cbs.audio_process = cb_audio_process;
	cbs.video_process = cb_video_process;
	cbs.video_pause = cb_noop;
	cbs.video_resume = cb_noop;
	cbs.conn_feedback = cb_conn_feedback;
	cbs.conn_reset = cb_conn_reset;
	cbs.video_reset = cb_video_reset;
	cbs.video_set_codec_and_size = cb_video_set_codec_and_size;
	cbs.conn_init = cb_conn_init;
	cbs.conn_destroy = cb_conn_destroy;
	cbs.audio_flush = cb_noop;
	cbs.video_flush = cb_noop;
	cbs.audio_set_client_volume = cb_audio_set_client_volume;
	cbs.audio_set_volume = cb_audio_set_volume;
	cbs.audio_set_metadata = cb_audio_buffer;
	cbs.audio_set_coverart = cb_audio_buffer;
	cbs.audio_stop_coverart_rendering = cb_noop;
	cbs.audio_remote_control_id = cb_remote_control_id;
	cbs.audio_set_progress = cb_audio_set_progress;
	cbs.audio_get_format = cb_audio_get_format;
	cbs.mirror_video_running = cb_mirror_video_running;
	cbs.report_client_request = cb_report_client_request;
	cbs.display_pin = cb_display_pin;
	cbs.register_client = cb_register_client;
	cbs.check_register = cb_check_register;
	cbs.passwd = cb_passwd;
	cbs.export_dacp = cb_export_dacp;
	cbs.get_custom_profile = cb_get_custom_profile;
	cbs.relios_allow_new_pairing = cb_allow_new_pairing;
	cbs.relios_pairing_failed = cb_pairing_failed;
	cbs.relios_client_trusted = cb_client_trusted;

	raop_t *raop = raop_init(&cbs);
	if (!raop) {
		send_text(RA_MSG_STATUS, "error text=raop_init");
		return 1;
	}
	raop_set_log_callback(raop, cb_log, NULL);
	raop_set_log_level(raop, LOGGER_INFO);
	if (raop_init2(raop, 0, device_id, keyfile)) {
		send_text(RA_MSG_STATUS, "error text=raop_init2");
		return 1;
	}
	raop_set_plist(raop, "width", req_width);
	raop_set_plist(raop, "height", req_height);
	raop_set_plist(raop, "refreshRate", 60);
	raop_set_plist(raop, "maxFPS", req_fps);
	raop_set_plist(raop, "clientFPSdata", 1); /* the iPad's own reports, for the measurements (cb_log) */
	raop_set_plist(raop, "pin", 0); /* PIN pairing; the PIN is random each time (pair-pin-start) */

	/* fixed ports, so that the firewall rule can name them: TCP 7000 (requests) and 7100 (video),
	 * UDP 6000 (timing), 6001 (control), 7011 (audio) */
	unsigned short tcp[2] = {7100, 7000};
	unsigned short udp[3] = {6000, 6001, 7011};
	raop_set_tcp_ports(raop, tcp);
	raop_set_udp_ports(raop, udp);
	unsigned short port = raop_get_port(raop);
	if (raop_start_httpd(raop, &port) < 0) {
		send_text(RA_MSG_STATUS, "error text=could not listen on TCP 7000");
		return 1;
	}
	raop_set_port(raop, port);

	int error = 0;
	dnssd_t *dnssd = dnssd_init(server_name, (int)strlen(server_name), hw_addr, 6, 1, &error);
	if (error || !dnssd) {
		send_text(RA_MSG_STATUS, "error text=dnssd_init %d", error);
		return 1;
	}
	set_features(dnssd);
	raop_set_dnssd(raop, dnssd);
	if (dnssd_register_raop(dnssd, port) || dnssd_register_airplay(dnssd, port)) {
		send_text(RA_MSG_STATUS, "error text=could not announce the receiver on the network");
		return 1;
	}
	send_text(RA_MSG_STATUS, "listening port=%u", (unsigned)port);

	pthread_t input;
	pthread_create(&input, NULL, stdin_thread, NULL);

	bool announced = true;
	for (;;) {
		if (quit_requested)
			break;
		pthread_mutex_lock(&state_mutex);
		uint64_t since = idle_since;
		bool announce = visible && !(hide_when_connected && trusted_connected && open_connections > 0);
		pthread_mutex_unlock(&state_mutex);
		if (announce != announced) {
			/* (the connection that is open does not depend on the announcement) */
			if (announce) {
				if (dnssd_register_raop(dnssd, port) || dnssd_register_airplay(dnssd, port))
					log_line("[warning] could not announce the receiver again");
				else
					log_line("[info] announced on the network again");
			} else {
				dnssd_unregister_raop(dnssd);
				dnssd_unregister_airplay(dnssd);
				log_line("[info] no longer announced on the network (hidden, or a paired device is connected)");
			}
			announced = announce;
		}
		if (since && wall_ns() - since > RECONNECT_GRACE_NS) {
			log_line("[info] the iPad did not reconnect: ending");
			break;
		}
		usleep(100 * 1000);
	}

	send_text(RA_MSG_STATUS, "ended");
	dnssd_unregister_raop(dnssd);
	dnssd_unregister_airplay(dnssd);
	/* (the process ends here: the job and the operating system close every socket) */
	fflush(stdout);
	_exit(0);
}

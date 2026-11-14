/*
 * Relios Mirror: wired (USB) mirroring of an iPad / iPhone on Windows.
 *
 * An Apple device can send its screen and sound over a hidden USB configuration: the one QuickTime uses on a Mac to
 * record the device's screen. This file switches the device to it, opens its two bulk pipes and lets qtproto.c speak
 * the protocol; pictures and sound go to the program through the callbacks (usbqt.h).
 *
 * USB access: the libusb-win32 filter driver (libusb0), installed for the Apple device alone (Settings > USB).
 * Apple's own driver and service (from Apple Devices or iTunes) stay in place, and they are needed: Apple's driver
 * makes Windows use the device's capture configuration after the switch (Windows on its own uses only its photo
 * camera), and Apple's service pairs with the device ("Trust This Computer"). A device that does not trust this
 * computer sends nothing, and in its capture configuration Apple's service cannot reach it to be trusted; so a device
 * is left to Apple's service for a moment before it is switched, and given back at once when it does not answer.
 * Without Apple's support nothing is switched at all (the program says what is missing).
 *
 * The sequence (activation request, the device leaving and coming back, claiming its capture interface, giving it
 * back afterwards) follows iPhoneMirror by RayrenSX (GPL-3.0-only, https://github.com/RayrenSX/iPhoneMirror), which
 * worked it out on real devices; the protocol follows quicktime_video_hack by Daniel Paulus (MIT,
 * https://github.com/danielpaulus/quicktime_video_hack).
 *
 * The switch request makes the device disappear and come back as a new USB node. The handle it was sent through
 * names the node being torn down and must not be closed while that happens; so the request is sent by a short
 * helper process (this program started with --usb-switch), which waits until the device is back and settled and
 * then simply ends. The streaming handle is closed before the device is given back, so it never sends the request.
 *
 * The driver is installed by a small program of its own, relios-usb-kurucu.exe (usbdriver.c), with administrator
 * rights. This file only reads Windows' device list and uses the installed driver.
 */
#ifdef _WIN32
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
#include <windows.h>
#include <setupapi.h>
#include <cfgmgr32.h>
#endif

#include "usbqt.h"
#include "usbids.h"
#include "qtproto.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32

/* ------------------------------------------------------------------ libusb-win32, loaded when needed */

/* the library's structures (lusb0_usb.h of libusb-win32 1.2.6.0 declares them between pshpack1.h / poppack.h) */
#pragma pack(push, 1)
struct lu_endpoint {
	unsigned char bLength, bDescriptorType, bEndpointAddress, bmAttributes;
	unsigned short wMaxPacketSize;
	unsigned char bInterval, bRefresh, bSynchAddress;
	unsigned char *extra;
	int extralen;
};
struct lu_altsetting {
	unsigned char bLength, bDescriptorType, bInterfaceNumber, bAlternateSetting, bNumEndpoints, bInterfaceClass,
		bInterfaceSubClass, bInterfaceProtocol, iInterface;
	struct lu_endpoint *endpoint;
	unsigned char *extra;
	int extralen;
};
struct lu_interface {
	struct lu_altsetting *altsetting;
	int num_altsetting;
};
struct lu_config {
	unsigned char bLength, bDescriptorType;
	unsigned short wTotalLength;
	unsigned char bNumInterfaces, bConfigurationValue, iConfiguration, bmAttributes, MaxPower;
	struct lu_interface *interfaces;
	unsigned char *extra;
	int extralen;
};
struct lu_device_descriptor {
	unsigned char bLength, bDescriptorType;
	unsigned short bcdUSB;
	unsigned char bDeviceClass, bDeviceSubClass, bDeviceProtocol, bMaxPacketSize0;
	unsigned short idVendor, idProduct, bcdDevice;
	unsigned char iManufacturer, iProduct, iSerialNumber, bNumConfigurations;
};
struct lu_bus;
struct lu_device {
	struct lu_device *next, *prev;
	char filename[512];
	struct lu_bus *bus;
	struct lu_device_descriptor descriptor;
	struct lu_config *config;
	void *dev;
	unsigned char devnum, num_children;
	struct lu_device **children;
};
struct lu_bus {
	struct lu_bus *next, *prev;
	char dirname[512];
	struct lu_device *devices;
	unsigned long location;
	struct lu_device *root_dev;
};
#pragma pack(pop)

typedef struct lu_handle lu_handle;

static struct {
	HMODULE dll;
	void (*init)(void);
	int (*find_busses)(void);
	int (*find_devices)(void);
	struct lu_bus *(*get_busses)(void);
	lu_handle *(*open)(struct lu_device *);
	int (*close)(lu_handle *);
	int (*get_string_simple)(lu_handle *, int, char *, size_t);
	int (*get_descriptor)(lu_handle *, unsigned char, unsigned char, void *, int);
	int (*control_msg)(lu_handle *, int, int, int, int, char *, int, int);
	int (*set_configuration)(lu_handle *, int);
	int (*claim_interface)(lu_handle *, int);
	int (*set_altinterface)(lu_handle *, int);
	int (*bulk_read)(lu_handle *, int, char *, int, int);
	int (*bulk_write)(lu_handle *, int, char *, int, int);
	int (*release_interface)(lu_handle *, int);
	char *(*strerror)(void);
} L;

/* libusb0's results (negative errno values) */
#define LU_TIMEOUT (-116)
#define LU_NO_DEVICE (-5)
#define LU_INVALID (-22)

#define APPLE_VID 0x05AC
#define CAPTURE_SUBCLASS 0x2A
/* One read is at most 64 KiB: libusb-win32 cuts larger ones into 64 KiB requests, and when a request after the
 * first runs into the time limit, the bytes the earlier ones brought are lost with the error it returns.
 * (How the capture pipe is read so that nothing is lost: see struct reader) */
#define READ_SIZE 65536

/* the library the driver installation put into System32 (it belongs to the installed driver; without the driver it
 * would find nothing anyway). Nothing is loaded from the program's folder; its dependencies come from System32 */
static bool load_libusb(void)
{
	if (L.dll)
		return true;
	wchar_t path[MAX_PATH + 32];
	UINT n = GetSystemDirectoryW(path, MAX_PATH);
	if (n == 0 || n >= MAX_PATH)
		return false;
	wcscat(path, L"\\libusb0.dll");
	if (GetFileAttributesW(path) == INVALID_FILE_ATTRIBUTES)
		return false;
	L.dll = LoadLibraryExW(path, NULL, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
	if (!L.dll)
		return false;
#define LOAD(field, name) (L.field = (__typeof__(L.field))(void (*)(void))GetProcAddress(L.dll, name))
	bool ok = LOAD(init, "usb_init") && LOAD(find_busses, "usb_find_busses") &&
		  LOAD(find_devices, "usb_find_devices") && LOAD(get_busses, "usb_get_busses") &&
		  LOAD(open, "usb_open") && LOAD(close, "usb_close") &&
		  LOAD(get_string_simple, "usb_get_string_simple") && LOAD(get_descriptor, "usb_get_descriptor") &&
		  LOAD(control_msg, "usb_control_msg") && LOAD(set_configuration, "usb_set_configuration") &&
		  LOAD(claim_interface, "usb_claim_interface") && LOAD(set_altinterface, "usb_set_altinterface") &&
		  LOAD(bulk_read, "usb_bulk_read") && LOAD(bulk_write, "usb_bulk_write") &&
		  LOAD(release_interface, "usb_release_interface") && LOAD(strerror, "usb_strerror");
#undef LOAD
	if (!ok) {
		FreeLibrary(L.dll);
		memset(&L, 0, sizeof(L));
		return false;
	}
	L.init();
	return true;
}

static const char *lu_error(void)
{
	const char *e = L.strerror ? L.strerror() : NULL;
	return e ? e : "?";
}

/* ------------------------------------------------------------------ small helpers */

static struct {
	HANDLE thread;
	/* detached: the program stopped waiting for this thread (it is ending): nothing is called back any more.
	 * retry: "try again now" (usbqt_retry) */
	volatile LONG stop, quick, streaming, detached, retry;
	struct usbqt_callbacks cb;
	enum usbqt_state state;
} U;

static bool detached(void)
{
	return InterlockedCompareExchange(&U.detached, 0, 0) != 0;
}

static void ulog(const char *fmt, ...)
{
	if (!U.cb.log || detached())
		return;
	char text[400];
	va_list args;
	va_start(args, fmt);
	vsnprintf(text, sizeof(text), fmt, args);
	va_end(args);
	U.cb.log(text);
}

static bool stopping(void)
{
	return InterlockedCompareExchange(&U.stop, 0, 0) != 0;
}

static bool quick(void)
{
	return InterlockedCompareExchange(&U.quick, 0, 0) != 0;
}

static void set_state(enum usbqt_state s)
{
	if (s == U.state)
		return;
	U.state = s;
	if (U.cb.state && !detached())
		U.cb.state(s);
}

static bool retry_asked(void)
{
	return InterlockedCompareExchange(&U.retry, 0, 0) != 0;
}

/* "try again now" asked (and taken) */
static bool take_retry(void)
{
	return InterlockedExchange(&U.retry, 0) != 0;
}

/* a wait between attempts: also ends when "try again now" is asked */
static void pause_ms(DWORD ms)
{
	for (DWORD t = 0; t < ms && !stopping() && !retry_asked(); t += 50)
		Sleep(50);
}

static uint64_t now_ns(void)
{
	static LARGE_INTEGER freq;
	LARGE_INTEGER c;
	if (!freq.QuadPart)
		QueryPerformanceFrequency(&freq);
	QueryPerformanceCounter(&c);
	uint64_t f = (uint64_t)freq.QuadPart, v = (uint64_t)c.QuadPart;
	return f ? v / f * 1000000000ull + v % f * 1000000000ull / f : 0;
}

/* a serial number as Windows writes it in device paths: letters and digits, upper case, without dashes and spaces.
 * Anything else makes it empty (it also goes on the helper's command line) */
static void normalize_serial(const char *in, char *out, size_t size)
{
	size_t o = 0;
	for (; *in; in++) {
		unsigned char c = (unsigned char)*in;
		if (c == '-' || c == ' ')
			continue;
		if (!isalnum(c) || c > 0x7f || o + 1 >= size) {
			out[0] = '\0';
			return;
		}
		out[o++] = (char)toupper(c);
	}
	out[o] = '\0';
}

/* ------------------------------------------------------------------ Windows' device list (no USB traffic) */

static void ascii_upper(const wchar_t *w, char *out, size_t size, bool *ok)
{
	size_t n = 0;
	*ok = true;
	for (; *w && n + 1 < size; w++) {
		if (*w > 0x7f) {
			*ok = false;
			break;
		}
		out[n++] = (char)toupper((unsigned char)*w);
	}
	out[n] = '\0';
}

/* the serial in a device path or instance ID after its "VID_05AC&PID_xxxx" part and a separator */
static bool path_serial(const char *after_pid, char sep, char *norm, size_t size)
{
	const char *end = strchr(after_pid, sep);
	size_t n = end ? (size_t)(end - after_pid) : strlen(after_pid);
	char segment[128];
	if (n == 0 || n >= sizeof(segment))
		return false;
	memcpy(segment, after_pid, n);
	segment[n] = '\0';
	normalize_serial(segment, norm, size);
	return norm[0] != '\0';
}

/* libusb0's device interface: present while the filter has the device */
static const GUID libusb0_guid = {0xf9f3ff14, 0xae21, 0x48a0, {0x8a, 0x25, 0x80, 0x11, 0xa7, 0xa9, 0x31, 0xd9}};

static bool link_matches(const wchar_t *link, unsigned pid, const char *serial)
{
	char a[600], prefix[64], norm[128];
	bool ok;
	ascii_upper(link, a, sizeof(a), &ok);
	if (!ok)
		return false;
	snprintf(prefix, sizeof(prefix), "\\\\?\\USB#VID_05AC&PID_%04X#", pid);
	size_t pl = strlen(prefix);
	if (strncmp(a, prefix, pl) || !path_serial(a + pl, '#', norm, sizeof(norm)))
		return false;
	const char *hash = strchr(a + pl, '#');
	return hash && !strcmp(norm, serial) && !strcmp(hash, "#{F9F3FF14-AE21-48A0-8A25-8011A7A931D9}");
}

/* whether libusb0 has this device now */
static bool interface_present(unsigned pid, const char *serial)
{
	ULONG chars = 0;
	if (CM_Get_Device_Interface_List_SizeW(&chars, (LPGUID)&libusb0_guid, NULL,
					       CM_GET_DEVICE_INTERFACE_LIST_PRESENT) != CR_SUCCESS ||
	    chars < 2 || chars > 1000000)
		return false;
	wchar_t *list = calloc(chars + 1, sizeof(wchar_t));
	if (!list)
		return false;
	bool found = false;
	if (CM_Get_Device_Interface_ListW((LPGUID)&libusb0_guid, NULL, list, chars,
					  CM_GET_DEVICE_INTERFACE_LIST_PRESENT) == CR_SUCCESS) {
		for (wchar_t *p = list; *p && !found; p += wcslen(p) + 1)
			found = link_matches(p, pid, serial);
	}
	free(list);
	return found;
}

static void wide_to_utf8(const wchar_t *w, char *out, size_t size)
{
	if (!WideCharToMultiByte(CP_UTF8, 0, w, -1, out, (int)size, NULL, NULL))
		out[0] = '\0';
	out[size - 1] = '\0';
}

/* a REG_MULTI_SZ property of a device, its strings joined with ", " (ASCII; anything else makes it "?") */
static void multi_sz(HDEVINFO set, SP_DEVINFO_DATA *info, DWORD property, char *out, size_t size)
{
	wchar_t buf[256];
	DWORD type = 0, got = 0;
	out[0] = '\0';
	memset(buf, 0, sizeof(buf));
	if (!SetupDiGetDeviceRegistryPropertyW(set, info, property, &type, (PBYTE)buf, sizeof(buf) - 4, &got) ||
	    type != REG_MULTI_SZ)
		return;
	size_t o = 0;
	for (const wchar_t *p = buf; p < buf + 254 && *p; p += wcslen(p) + 1) {
		for (const wchar_t *c = p; *c && o + 3 < size; c++)
			out[o++] = *c > 0x20 && *c < 0x7f ? (char)*c : '?';
		if (o + 3 < size && p[wcslen(p) + 1]) {
			out[o++] = ',';
			out[o++] = ' ';
		}
	}
	out[o] = '\0';
}

/* the iPad / iPhone with that serial (NULL: the first) in Windows' device list */
static bool find_entry(const char *serial, struct usbqt_device *out)
{
	memset(out, 0, sizeof(*out));
	HDEVINFO set = SetupDiGetClassDevsW(NULL, L"USB", NULL, DIGCF_ALLCLASSES | DIGCF_PRESENT);
	if (set == INVALID_HANDLE_VALUE)
		return false;
	SP_DEVINFO_DATA info;
	info.cbSize = sizeof(info);
	bool found = false;
	for (DWORD i = 0; !found && SetupDiEnumDeviceInfo(set, i, &info); i++) {
		wchar_t id[400];
		char a[400], norm[128];
		bool ok;
		if (!SetupDiGetDeviceInstanceIdW(set, &info, id, 400, NULL))
			continue;
		ascii_upper(id, a, sizeof(a), &ok);
		if (!ok || !valid_instance(a))
			continue;
		/* (USB\VID_05AC&PID_xxxx\<serial>: the serial after the second backslash) */
		if (serial && (!path_serial(a + 22, '\\', norm, sizeof(norm)) || strcmp(norm, serial)))
			continue;
		snprintf(out->instance, sizeof(out->instance), "%s", a);
		ULONG status = 0, problem = 0;
		if (CM_Get_DevNode_Status(&status, &problem, info.DevInst, 0) == CR_SUCCESS && (status & DN_HAS_PROBLEM))
			out->problem = problem ? (unsigned)problem : 1;
		wchar_t buf[512];
		DWORD type = 0, size = 0;
		memset(buf, 0, sizeof(buf));
		if (SetupDiGetDeviceRegistryPropertyW(set, &info, SPDRP_UPPERFILTERS, &type, (PBYTE)buf, sizeof(buf) - 4,
						      &size) &&
		    type == REG_MULTI_SZ) {
			for (const wchar_t *p = buf; *p && p < buf + 510; p += wcslen(p) + 1) {
				if (!_wcsicmp(p, L"libusb0"))
					out->filtered = true;
			}
		}
		memset(buf, 0, sizeof(buf));
		if (SetupDiGetDeviceRegistryPropertyW(set, &info, SPDRP_FRIENDLYNAME, &type, (PBYTE)buf, sizeof(buf) - 4,
						      &size) ||
		    SetupDiGetDeviceRegistryPropertyW(set, &info, SPDRP_DEVICEDESC, &type, (PBYTE)buf, sizeof(buf) - 4,
						      &size))
			wide_to_utf8(buf, out->name, sizeof(out->name));
		memset(buf, 0, sizeof(buf));
		if (SetupDiGetDeviceRegistryPropertyW(set, &info, SPDRP_SERVICE, &type, (PBYTE)buf, sizeof(buf) - 4,
						      &size) &&
		    type == REG_SZ) {
			bool plain;
			ascii_upper(buf, out->service, sizeof(out->service), &plain);
			if (!plain)
				out->service[0] = '\0';
		}
		multi_sz(set, &info, SPDRP_LOWERFILTERS, out->lower, sizeof(out->lower));
		found = true;
	}
	SetupDiDestroyDeviceInfoList(set);
	return found;
}

bool usbqt_find_device(struct usbqt_device *out)
{
	return find_entry(NULL, out);
}

bool usbqt_apple_driver(const struct usbqt_device *d)
{
	/* (no driver at all is not Apple's either; one that could not be read is not held against it) */
	return d->service[0] ? !strcmp(d->service, "USBCCGP") : d->problem != 28;
}

/* ------------------------------------------------------------------ Apple's own support (no USB traffic) */

/* the Microsoft Store packages that bring Apple's service (Apple Devices, iTunes): installed for anyone? */
static bool apple_package_installed(void)
{
	typedef LONG(WINAPI * by_family_fn)(PCWSTR, UINT32 *, PWSTR *, UINT32 *, WCHAR *);
	HMODULE kernel = GetModuleHandleW(L"kernel32.dll");
	by_family_fn by_family =
		kernel ? (by_family_fn)(void (*)(void))GetProcAddress(kernel, "GetPackagesByPackageFamily") : NULL;
	if (!by_family)
		return false;
	static const wchar_t *const families[] = {L"AppleInc.AppleDevices_nzyj5cx40ttqa", L"AppleInc.iTunes_nzyj5cx40ttqa"};
	for (size_t i = 0; i < sizeof(families) / sizeof(families[0]); i++) {
		UINT32 count = 0, length = 0;
		LONG r = by_family(families[i], &count, NULL, &length, NULL);
		if (count > 0 || r == ERROR_INSUFFICIENT_BUFFER)
			return true;
	}
	return false;
}

enum usbqt_apple usbqt_apple_service(void)
{
	SC_HANDLE scm = OpenSCManagerW(NULL, NULL, SC_MANAGER_CONNECT);
	if (!scm)
		return USBQT_APPLE_OK; /* (cannot be asked: not held against it) */
	enum usbqt_apple result = USBQT_APPLE_MISSING;
	/* (iTunes names it so; some packages by its internal name) */
	static const wchar_t *const names[] = {L"Apple Mobile Device Service", L"AppleMobileDeviceService"};
	for (size_t i = 0; i < sizeof(names) / sizeof(names[0]) && result == USBQT_APPLE_MISSING; i++) {
		SC_HANDLE s = OpenServiceW(scm, names[i], SERVICE_QUERY_STATUS | SERVICE_QUERY_CONFIG);
		if (!s) {
			if (GetLastError() != ERROR_SERVICE_DOES_NOT_EXIST)
				result = USBQT_APPLE_OK; /* (there, but not ours to look at) */
			continue;
		}
		result = USBQT_APPLE_OK;
		SERVICE_STATUS_PROCESS st;
		DWORD need = 0;
		memset(&st, 0, sizeof(st));
		if (QueryServiceStatusEx(s, SC_STATUS_PROCESS_INFO, (BYTE *)&st, sizeof(st), &need) &&
		    st.dwCurrentState == SERVICE_STOPPED) {
			/* stopped: wrong if it should run with Windows (or is switched off); one started on demand may
			 * simply not be needed yet */
			union {
				QUERY_SERVICE_CONFIGW c;
				BYTE space[8192];
			} cfg;
			if (QueryServiceConfigW(s, &cfg.c, sizeof(cfg), &need) &&
			    (cfg.c.dwStartType == SERVICE_AUTO_START || cfg.c.dwStartType == SERVICE_DISABLED))
				result = USBQT_APPLE_NOT_RUNNING;
		}
		CloseServiceHandle(s);
	}
	CloseServiceHandle(scm);
	if (result == USBQT_APPLE_MISSING && apple_package_installed())
		result = USBQT_APPLE_OK;
	return result;
}

/* ------------------------------------------------------------------ the helpers' marks */

/* A helper holds two named mutexes while it runs: one that says "a helper of this program is running" (a new run of
 * the program waits for it before it touches the device) and one with its process ID (so that it is never taken for
 * a second copy of the program and ended halfway through a switch). */
#define HELPER_MARK L"Local\\ReliosMirrorUsbHelper"

static void mark_helper(void)
{
	wchar_t name[80];
	_snwprintf(name, 79, HELPER_MARK L".%lu", (unsigned long)GetCurrentProcessId());
	name[79] = L'\0';
	/* (kept until the process ends) */
	CreateMutexW(NULL, FALSE, name);
	CreateMutexW(NULL, FALSE, HELPER_MARK);
}

bool usbqt_is_helper(unsigned long pid)
{
	wchar_t name[80];
	_snwprintf(name, 79, HELPER_MARK L".%lu", pid);
	name[79] = L'\0';
	HANDLE m = OpenMutexW(SYNCHRONIZE, FALSE, name);
	if (!m)
		return false;
	CloseHandle(m);
	return true;
}

/* waits (at most ~20 s) while a helper of an earlier run still settles the device */
static void wait_for_helpers(void)
{
	for (int i = 0; i < 100 && !stopping(); i++) {
		HANDLE m = OpenMutexW(SYNCHRONIZE, FALSE, HELPER_MARK);
		if (!m)
			return;
		CloseHandle(m);
		Sleep(200);
	}
}

/* ------------------------------------------------------------------ the device through libusb0 */

struct found {
	struct lu_device *dev;
	char serial[80];
	unsigned pid;
};

static bool apple_capable(const struct lu_device *d)
{
	return d->descriptor.idVendor == APPLE_VID && apple_mobile_product(d->descriptor.idProduct);
}

static void read_serial(lu_handle *h, const struct lu_device *d, char *norm, size_t size)
{
	char raw[256];
	norm[0] = '\0';
	if (!d->descriptor.iSerialNumber)
		return;
	memset(raw, 0, sizeof(raw));
	int n = L.get_string_simple(h, d->descriptor.iSerialNumber, raw, sizeof(raw) - 1);
	if (n <= 0 || n >= (int)sizeof(raw))
		return;
	raw[n] = '\0';
	normalize_serial(raw, norm, size);
}

/* an Apple device the filter gives access to (want: its serial, or NULL for the first), verified by its serial */
static bool find_apple(const char *want, unsigned want_pid, struct found *f)
{
	L.find_busses();
	L.find_devices();
	for (struct lu_bus *b = L.get_busses(); b; b = b->next) {
		for (struct lu_device *d = b->devices; d; d = d->next) {
			if (!apple_capable(d) || (want_pid && d->descriptor.idProduct != want_pid))
				continue;
			lu_handle *h = L.open(d);
			if (!h)
				continue;
			char serial[80];
			read_serial(h, d, serial, sizeof(serial));
			L.close(h);
			if (!serial[0] || strlen(serial) > 64 || (want && strcmp(serial, want)))
				continue;
			f->dev = d;
			snprintf(f->serial, sizeof(f->serial), "%s", serial);
			f->pid = d->descriptor.idProduct;
			return true;
		}
	}
	return false;
}

struct capture_iface {
	int config, iface, alt, ep_in, ep_out;
	int in_packet; /* the largest packet of the pipe from the device (wMaxPacketSize) */
};

/* the capture interface (vendor class, subclass 0x2A, a bulk pipe each way) in one configuration descriptor */
static bool parse_capture(const uint8_t *d, int n, struct capture_iface *q)
{
	if (n < 9 || d[0] < 9 || d[1] != 2)
		return false;
	int cls = -1, sub = -1, iface = 0, alt = 0, in = 0, out = 0, in_packet = 0;
	for (int pos = d[0]; pos + 2 <= n;) {
		int len = d[pos], type = d[pos + 1];
		if (len < 2 || pos + len > n)
			break;
		if (type == 4 && len >= 9) {
			if (cls == 0xFF && sub == CAPTURE_SUBCLASS && in && out)
				break;
			iface = d[pos + 2];
			alt = d[pos + 3];
			cls = d[pos + 5];
			sub = d[pos + 6];
			in = out = in_packet = 0;
		} else if (type == 5 && len >= 7 && cls == 0xFF && sub == CAPTURE_SUBCLASS && (d[pos + 3] & 3) == 2) {
			if (d[pos + 2] & 0x80) {
				in = d[pos + 2];
				in_packet = (d[pos + 4] | (d[pos + 5] << 8)) & 0x7FF;
			} else
				out = d[pos + 2];
		}
		pos += len;
	}
	if (cls != 0xFF || sub != CAPTURE_SUBCLASS || !in || !out)
		return false;
	q->config = d[5];
	q->iface = iface;
	q->alt = alt;
	q->ep_in = in;
	q->ep_out = out;
	q->in_packet = in_packet;
	return true;
}

/* the same in the descriptors libusb0 read when it listed the device (config: 0 = any) */
static bool capture_in_descriptors(const struct lu_device *d, int config, struct capture_iface *q)
{
	if (!d->config)
		return false;
	int configs = d->descriptor.bNumConfigurations > 8 ? 8 : d->descriptor.bNumConfigurations;
	for (int c = 0; c < configs; c++) {
		const struct lu_config *cfg = &d->config[c];
		if ((config && cfg->bConfigurationValue != config) || !cfg->interfaces)
			continue;
		for (int i = 0; i < cfg->bNumInterfaces && i < 32; i++) {
			const struct lu_interface *itf = &cfg->interfaces[i];
			for (int a = 0; itf->altsetting && a < itf->num_altsetting && a < 16; a++) {
				const struct lu_altsetting *s = &itf->altsetting[a];
				if (s->bInterfaceClass != 0xFF || s->bInterfaceSubClass != CAPTURE_SUBCLASS || !s->endpoint)
					continue;
				int in = 0, out = 0, in_packet = 0;
				for (int e = 0; e < s->bNumEndpoints && e < 32; e++) {
					const struct lu_endpoint *ep = &s->endpoint[e];
					if ((ep->bmAttributes & 3) != 2)
						continue;
					if (ep->bEndpointAddress & 0x80) {
						in = ep->bEndpointAddress;
						in_packet = ep->wMaxPacketSize & 0x7FF;
					} else
						out = ep->bEndpointAddress;
				}
				if (in && out) {
					q->config = cfg->bConfigurationValue;
					q->iface = s->bInterfaceNumber;
					q->alt = s->bAlternateSetting;
					q->ep_in = in;
					q->ep_out = out;
					q->in_packet = in_packet;
					return true;
				}
			}
		}
	}
	return false;
}

/* whether a configuration (its value) offers an interface of Apple's own class with this subclass: 0xFE is the
 * channel Apple's service talks to the device through (and pairs over); Apple's driver makes Windows use a
 * configuration with it. Windows on its own picks the first, the camera for photos only */
#define MUX_SUBCLASS 0xFE

/* whether libusb0's copy of the device's descriptors has this configuration at all (else nothing is concluded) */
static bool config_listed(const struct lu_device *d, int config)
{
	int configs = d->descriptor.bNumConfigurations > 8 ? 8 : d->descriptor.bNumConfigurations;
	for (int c = 0; d->config && c < configs; c++) {
		if (d->config[c].bConfigurationValue == config && d->config[c].interfaces)
			return true;
	}
	return false;
}

static bool config_has(const struct lu_device *d, int config, int subclass)
{
	if (!d->config)
		return false;
	int configs = d->descriptor.bNumConfigurations > 8 ? 8 : d->descriptor.bNumConfigurations;
	for (int c = 0; c < configs; c++) {
		const struct lu_config *cfg = &d->config[c];
		if (cfg->bConfigurationValue != config || !cfg->interfaces)
			continue;
		for (int i = 0; i < cfg->bNumInterfaces && i < 32; i++) {
			const struct lu_interface *itf = &cfg->interfaces[i];
			for (int a = 0; itf->altsetting && a < itf->num_altsetting && a < 16; a++) {
				if (itf->altsetting[a].bInterfaceClass == 0xFF &&
				    itf->altsetting[a].bInterfaceSubClass == subclass)
					return true;
			}
		}
	}
	return false;
}

/* the same in the configuration descriptors read from the device now */
static bool capture_from_device(lu_handle *h, int configs, struct capture_iface *q)
{
	static uint8_t d[4096];
	if (configs > 8)
		configs = 8;
	for (int index = 0; index < configs; index++) {
		uint8_t head[9];
		if (L.get_descriptor(h, 2, (unsigned char)index, head, (int)sizeof(head)) < 9 || head[0] < 9 ||
		    head[1] != 2)
			continue;
		int total = head[2] | (head[3] << 8);
		if (total < 9 || total > (int)sizeof(d))
			continue;
		int n = L.get_descriptor(h, 2, (unsigned char)index, d, total);
		if (n >= 9 && parse_capture(d, n > total ? total : n, q))
			return true;
	}
	return false;
}

static int active_config(lu_handle *h)
{
	unsigned char v = 0;
	return L.control_msg(h, 0x80, 0x08, 0, 0, (char *)&v, 1, 1000) == 1 ? v : -1;
}

static bool write_all(lu_handle *h, int ep, const uint8_t *p, size_t n)
{
	while (n > 0) {
		int chunk = n > READ_SIZE ? READ_SIZE : (int)n;
		int r = L.bulk_write(h, ep, (char *)p, chunk, 1000);
		if (r <= 0)
			return false;
		p += r;
		n -= (size_t)r;
	}
	return true;
}

/* ------------------------------------------------------------------ the switch (a helper process) */

/* runs this program as the helper and waits for it (at most timeout_ms). A helper that has not finished by then is
 * left to finish on its own: it ends by itself within about 20 s, and ending it early could close its handle while
 * the device is being torn down */
static int run_switch(const char *op, const char *serial, unsigned pid, DWORD timeout_ms)
{
	wchar_t exe[MAX_PATH * 2], cmd[MAX_PATH * 2 + 128];
	DWORD n = GetModuleFileNameW(NULL, exe, MAX_PATH * 2);
	if (!n || n >= MAX_PATH * 2)
		return -1;
	char args[128];
	/* (the serial is letters and digits only, the operation is ours: nothing here needs quoting) */
	snprintf(args, sizeof(args), " --usb-switch %s %s %04x", op, serial, pid & 0xFFFF);
	int len = _snwprintf(cmd, MAX_PATH * 2, L"\"%ls\"", exe);
	if (len < 0)
		return -1;
	for (const char *p = args; *p; p++)
		cmd[len++] = (wchar_t)(unsigned char)*p;
	cmd[len] = L'\0';

	STARTUPINFOW si;
	PROCESS_INFORMATION pi;
	memset(&si, 0, sizeof(si));
	si.cb = sizeof(si);
	memset(&pi, 0, sizeof(pi));
	if (!CreateProcessW(exe, cmd, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi))
		return -1;
	DWORD code = (DWORD)-2;
	if (WaitForSingleObject(pi.hProcess, timeout_ms) == WAIT_OBJECT_0)
		GetExitCodeProcess(pi.hProcess, &code);
	CloseHandle(pi.hThread);
	CloseHandle(pi.hProcess);
	return (int)code;
}

/* exit codes: 0 switched, 20 nothing to do, 32 the device left but did not settle back in time, 33 no departure
 * seen (10 if the request also failed), 4 no configuration readable, 21-24 bad arguments, 25 no library,
 * 26 device not found */
int usbqt_switch_main(int argc, char **argv)
{
	if (argc != 5)
		return 21;
	bool activate = !strcmp(argv[2], "activate");
	if (!activate && strcmp(argv[2], "restore"))
		return 22;
	char serial[80];
	normalize_serial(argv[3], serial, sizeof(serial));
	if (!serial[0] || strlen(serial) > 64)
		return 23;
	char *end = NULL;
	unsigned long pid = strtoul(argv[4], &end, 16);
	if (!end || *end || !apple_mobile_product((unsigned)pid))
		return 24;
	mark_helper();
	if (!load_libusb())
		return 25;

	L.find_busses();
	L.find_devices();
	lu_handle *h = NULL;
	struct lu_device *dev = NULL;
	for (struct lu_bus *b = L.get_busses(); b && !h; b = b->next) {
		for (struct lu_device *d = b->devices; d && !h; d = d->next) {
			if (!apple_capable(d) || d->descriptor.idProduct != pid)
				continue;
			lu_handle *candidate = L.open(d);
			if (!candidate)
				continue;
			char s[80];
			read_serial(candidate, d, s, sizeof(s));
			if (s[0] && !strcmp(s, serial)) {
				h = candidate;
				dev = d;
			} else {
				L.close(candidate);
			}
		}
	}
	if (!h)
		return 26;
	int active = active_config(h);
	if (active < 0) {
		L.close(h);
		return 4;
	}
	struct capture_iface q;
	memset(&q, 0, sizeof(q));
	bool capturing = capture_in_descriptors(dev, active, &q);
	if (activate == capturing) {
		L.close(h);
		return 20;
	}
	int result = L.control_msg(h, 0x40, 0x52, 0, activate ? 2 : 0, NULL, 0, 1000);
	/* From here on the handle stays open until the process ends: the device leaves and comes back, ready when it
	 * has been back for 750 ms (or after 15 s at most) */
	ULONGLONG deadline = GetTickCount64() + 15000, back_since = 0;
	bool left = false;
	while (GetTickCount64() < deadline) {
		bool present = interface_present((unsigned)pid, serial);
		if (!left) {
			left = !present;
		} else if (present) {
			if (!back_since)
				back_since = GetTickCount64();
			if (GetTickCount64() - back_since >= 750)
				return 0;
		} else {
			back_since = 0;
		}
		Sleep(20);
	}
	return left ? 32 : result < 0 ? 10 : 33;
}

/* ------------------------------------------------------------------ a session */

static void sink_video(void *ctx, const uint8_t *d, size_t n, bool h265, int w, int h)
{
	(void)ctx;
	if (U.cb.video && !detached())
		U.cb.video(d, n, h265, w, h);
}

static void sink_audio(void *ctx, const uint8_t *pcm, int frames, int channels, int rate)
{
	(void)ctx;
	if (U.cb.audio && !detached())
		U.cb.audio(pcm, frames, channels, rate);
}

static void sink_log(void *ctx, const char *text)
{
	(void)ctx;
	ulog("%s", text);
}

/* the device in its capture configuration: found again by its serial, its capture interface claimed.
 * newly: this program has just switched it (only then may libusb0's view of it be initialized) */
static lu_handle *open_capture(const char *serial, unsigned *pid, bool newly, struct capture_iface *q)
{
	struct found f;
	bool got = false;
	for (int i = 0; i < 40 && !stopping(); i++) {
		/* (by its serial; its product ID is expected to stay, but is not relied on) */
		if (find_apple(serial, *pid, &f) || find_apple(serial, 0, &f)) {
			got = true;
			break;
		}
		Sleep(200);
	}
	if (!got) {
		ulog("the device was not found again after the switch");
		return NULL;
	}
	if (f.pid != *pid) {
		ulog("after the switch the device shows product %04x", f.pid);
		*pid = f.pid;
	}
	lu_handle *h = L.open(f.dev);
	if (!h) {
		ulog("the device cannot be opened: %s", lu_error());
		return NULL;
	}
	/* both have to be seen: the capture interface described and its configuration active (after the switch the
	 * device may need a moment) */
	memset(q, 0, sizeof(*q));
	int active = -1;
	bool ready = false, configured = false;
	ULONGLONG deadline = GetTickCount64() + 5000, unconfigured_since = 0;
	do {
		struct capture_iface probe;
		memset(&probe, 0, sizeof(probe));
		if (capture_from_device(h, f.dev->descriptor.bNumConfigurations, &probe))
			*q = probe;
		active = active_config(h);
		if (q->config && active == q->config) {
			ready = true;
			break;
		}
		/* Back in no configuration at all (0): nothing on this computer chose one for it. Apple's driver would
		 * (it is not the one running the device then: e.g. libusb0 runs it instead of only filtering it), so
		 * the capture configuration is chosen here, once, after the device has shown 0 for a second. A
		 * configuration someone else chose is never changed */
		if (q->config && active == 0) {
			ULONGLONG t = GetTickCount64();
			if (!unconfigured_since)
				unconfigured_since = t;
			if (!configured && t - unconfigured_since >= 1000) {
				configured = true;
				int r = L.set_configuration(h, q->config);
				ulog("the device came back without a configuration: configuration %d chosen (%d%s%s)", q->config,
				     r, r < 0 ? ", " : "", r < 0 ? lu_error() : "");
				if (r >= 0 && deadline < t + 3000)
					deadline = t + 3000;
			}
		} else {
			unconfigured_since = 0;
		}
		Sleep(100);
	} while (GetTickCount64() < deadline && !stopping());
	ulog("capture interface: configuration %d, interface %d.%d, pipes 0x%02x / 0x%02x; active configuration %d",
	     q->config, q->iface, q->alt, q->ep_in, q->ep_out, active);
	if (!ready) {
		ulog(q->config ? "the device is not in its capture configuration" :
				 "the device shows no screen-capture interface");
		L.close(h);
		return NULL;
	}
	int r = L.claim_interface(h, q->iface);
	if (r < 0) {
		const char *why = lu_error();
		ulog("claiming the capture interface: %d (%s)", r, why);
		/* a freshly switched device can show its configuration before libusb0 has set up its pipes for this
		 * handle ("invalid configuration 0"). Only then, and only after our own switch, is the (already
		 * active) configuration selected once more (as iPhoneMirror does) */
		if (!newly || r != LU_INVALID || !strstr(why, "invalid configuration 0")) {
			L.close(h);
			return NULL;
		}
		if (L.set_configuration(h, q->config) < 0) {
			ulog("setting up configuration %d: %s", q->config, lu_error());
			L.close(h);
			return NULL;
		}
		r = L.claim_interface(h, q->iface);
		if (r < 0) {
			ulog("claiming the capture interface: %d (%s)", r, lu_error());
			L.close(h);
			return NULL;
		}
	}
	if (q->alt && L.set_altinterface(h, q->alt) < 0) {
		ulog("alternate setting %d: %s", q->alt, lu_error());
		L.close(h);
		return NULL;
	}
	/* (1.3.3 reset the pipe to the device here (usb_clear_halt): after that the device never said hello. Not done) */
	return h;
}

/* Reads from the capture pipe. When the time limit of usb_bulk_read runs out, libusb-win32 aborts the request and
 * the bytes it had already received are lost (it returns only the error); then the rest of a message is missing,
 * the next message is taken for its rest and the stream cannot be followed any more (1.3.0).
 * So a read never asks for more than the message being read still lacks (rounded up to whole packets): it ends
 * either full or with the message's last, short packet, as soon as the device has sent that message. Between
 * messages a read asks for one packet: when it runs into the short time limit nothing has arrived in it (a message
 * that had begun would have filled it or ended it), so nothing is lost. Within a message the device keeps sending;
 * there the time limit is long. (1.3.1-1.3.2 read with libusb0's asynchronous requests instead; on a real device
 * the stream stopped after the first message there, and a cancelled request did not end: not used) */
struct reader {
	uint8_t *buf;
	int ep;
	int packet; /* the pipe's packet size: a request is a whole number of packets */
};

static bool reader_open(struct reader *r, const struct capture_iface *q)
{
	memset(r, 0, sizeof(*r));
	r->ep = q->ep_in;
	r->packet = q->in_packet >= 8 && q->in_packet <= 1024 ? q->in_packet : 512;
	r->buf = malloc(READ_SIZE);
	return r->buf != NULL;
}

static void reader_close(struct reader *r)
{
	free(r->buf);
	memset(r, 0, sizeof(*r));
}

/* the bytes to ask for: what the message being read lacks, in whole packets (one packet when a new one begins) */
static int read_wanted(const struct reader *r, size_t missing)
{
	size_t p = (size_t)r->packet;
	size_t want = missing ? (missing + p - 1) / p * p : p;
	if (want > READ_SIZE)
		want = READ_SIZE - READ_SIZE % p;
	return (int)want;
}

/* one read: bytes (> 0), 0 (an empty packet), LU_TIMEOUT (nothing yet) or an error.
 * Rides out the few polls in which libusb0 reports the device missing while its node settles */
static int read_some(lu_handle *h, struct reader *r, size_t missing)
{
	int want = read_wanted(r, missing);
	int timeout = missing ? 3000 : 250;
	for (int attempt = 0;; attempt++) {
		int n = L.bulk_read(h, r->ep, (char *)r->buf, want, timeout);
		if (n != LU_NO_DEVICE || attempt >= 12 || stopping())
			return n;
		Sleep(50);
	}
}

/* The device says hello (PING) right after its capture configuration has been selected, once. A device found
 * already in that configuration (an earlier session, given back without leaving) may have said it long ago and
 * says nothing now. Its configuration is then taken away and selected anew (libusb0 skips selecting the one
 * already active, so first none), and it starts afresh. The interface is claimed again after that */
static bool select_anew(lu_handle *h, const struct capture_iface *q)
{
	int rel = L.release_interface(h, q->iface);
	int none = L.set_configuration(h, 0);
	Sleep(200);
	int set = L.set_configuration(h, q->config);
	int claim = set >= 0 ? L.claim_interface(h, q->iface) : -1;
	bool ok = claim >= 0 && (!q->alt || L.set_altinterface(h, q->alt) >= 0);
	ulog("no hello from the device: configuration %d selected anew (%d %d %d %d%s%s)", q->config, rel, none, set,
	     claim, ok ? "" : ": ", ok ? "" : lu_error());
	return ok;
}

enum end_reason { END_STOP, END_GONE, END_ERROR, END_LOCKED, END_DEVICE_STOPPED };

/* reads until stopped, unplugged or broken; then the release. The handle is closed here */
static enum end_reason stream(lu_handle *h, const struct capture_iface *q, bool newly, const char *serial,
			      unsigned pid)
{
	struct qt_sink sink = {sink_video, sink_audio, sink_log, NULL};
	struct qt_options opt = {1640, 2360, true, true};
	struct qt_session *s = qt_session_new(&sink, &opt);
	struct reader rd;
	struct qt_out out = {0};
	enum end_reason why = END_ERROR;
	if (!s || !reader_open(&rd, q)) {
		qt_session_free(s);
		L.close(h);
		return END_ERROR;
	}
	const uint8_t *buf = rd.buf;
	ULONGLONG start = GetTickCount64(), last_data = start, answered = 0;
	bool nudged = false, greeted = false, pinged_again = false, reselected = false, got_any = false;
	unsigned reads_logged = 0; /* (the first reads in the log: how the device's messages come) */
	for (;;) {
		if (stopping()) {
			why = END_STOP;
			break;
		}
		size_t missing = qt_session_missing(s);
		int n = read_some(h, &rd, missing);
		ULONGLONG now = GetTickCount64();
		if (n >= 0 && reads_logged < 12) {
			reads_logged++;
			ulog("read: %d bytes (asked %d, after %llu ms)", n, read_wanted(&rd, missing),
			     (unsigned long long)(now - start));
		}
		if (n < 0 && n != LU_TIMEOUT) {
			ulog("reading stopped: %d (%s)", n, lu_error());
			why = interface_present(pid, serial) ? END_ERROR : END_GONE;
			break;
		}
		if (n > 0) {
			last_data = now;
			got_any = true;
			qt_out_clear(&out);
			if (!qt_session_feed(s, buf, (size_t)n, &out, now_ns())) {
				ulog("the stream from the device could not be followed");
				break;
			}
			qt_session_trace_out(s, &out);
			if (out.failed || (out.n && !write_all(h, q->ep_out, out.p, out.n))) {
				ulog("writing to the device failed: %s", lu_error());
				why = interface_present(pid, serial) ? END_ERROR : END_GONE;
				break;
			}
			enum qt_state st = qt_session_state(s);
			if (st != QT_WAIT_PING && !greeted) {
				greeted = true;
				ulog("the device answered (it trusts this computer) after %llu ms",
				     (unsigned long long)(now - start));
			}
			if (st == QT_STREAMING && !InterlockedCompareExchange(&U.streaming, 0, 0)) {
				InterlockedExchange(&U.streaming, 1);
				set_state(USBQT_STREAMING);
				ulog("pictures arrive");
			}
			if (st == QT_STOPPED) {
				why = END_DEVICE_STOPPED;
				break;
			}
		}
		/* (after every read: a device that keeps sending sound but no picture is not waited for forever) */
		enum qt_state st = qt_session_state(s);
		if (st == QT_WAIT_PING) {
			if (n <= 0 && !got_any && !reselected && now - start > 1500) {
				reselected = true;
				if (!select_anew(h, q)) {
					why = END_ERROR;
					break;
				}
				continue;
			}
			if (n <= 0 && !nudged && now - start > 4000) {
				/* a freshly switched device can open its pipes before it starts; one control nudge, and a
				 * PING of ours (iPhoneMirror). The nudge is never sent to a session found running */
				nudged = true;
				if (newly)
					L.control_msg(h, 0x40, 0x40, 0x6400, 0x6400, NULL, 0, 1000);
				qt_out_clear(&out);
				qt_make_ping(&out);
				write_all(h, q->ep_out, out.p, out.n);
				ulog("no hello from the device yet: nudged");
			}
			if (now - start > 9000) {
				ulog("no answer from the device (it should be unlocked and trust this computer)");
				why = END_LOCKED;
				break;
			}
			continue;
		}
		if (!answered)
			answered = now;
		/* the device said hello and was answered, but its clock has not come: our answer may not have reached it.
		 * Said once more (iPhoneMirror sends a PING of its own too) */
		if (st == QT_WAIT_CLOCK && !pinged_again && now - answered > 3000) {
			pinged_again = true;
			qt_out_clear(&out);
			qt_make_ping(&out);
			bool sent = write_all(h, q->ep_out, out.p, out.n);
			ulog("the device has not gone on after its hello: hello sent again (%s)", sent ? "sent" : lu_error());
		}
		if (!qt_session_video_frames(s) && now - answered > 12000) {
			ulog("the device answered but sent no picture in 12 s");
			break;
		}
		if (now - last_data > 15000) {
			ulog("nothing from the device for 15 s");
			break;
		}
	}
	ulog("session: %llu pictures, %llu sound packets, %u broken samples",
	     (unsigned long long)qt_session_video_frames(s), (unsigned long long)qt_session_audio_packets(s),
	     qt_session_bad_samples(s));
	if (InterlockedCompareExchange(&U.streaming, 0, 0)) {
		InterlockedExchange(&U.streaming, 0);
		set_state(stopping() ? USBQT_OFF : USBQT_STARTING);
	}

	if (why != END_GONE) {
		/* the release: HPA0 + HPD0, the device's two RELS (its requests answered meanwhile), then HPD0 again */
		bool started = qt_session_state(s) != QT_WAIT_PING;
		qt_out_clear(&out);
		qt_session_stop(s, &out);
		bool ok = write_all(h, q->ep_out, out.p, out.n);
		DWORD wait = why == END_DEVICE_STOPPED || !started ? 1000 : quick() ? 2000 : 6000;
		ULONGLONG until = GetTickCount64() + wait;
		while (ok && qt_session_releases(s) < 2 && GetTickCount64() < until) {
			int n = read_some(h, &rd, qt_session_missing(s));
			if (n < 0 && n != LU_TIMEOUT)
				break;
			if (n > 0) {
				qt_out_clear(&out);
				if (!qt_session_feed(s, buf, (size_t)n, &out, now_ns()))
					break;
				if (out.n)
					write_all(h, q->ep_out, out.p, out.n);
			}
		}
		if (qt_session_releases(s) >= 2) {
			qt_out_clear(&out);
			qt_session_finish(s, &out);
			write_all(h, q->ep_out, out.p, out.n);
		}
		ulog("released (%d of 2 answers)", qt_session_releases(s));
	}
	reader_close(&rd);
	/* (closing releases the interface; releasing it separately first could make libusb0 release it twice) */
	L.close(h);
	qt_out_free(&out);
	qt_session_free(s);
	return why;
}

static void give_back(const char *serial, unsigned pid)
{
	if (!interface_present(pid, serial))
		return;
	int code = run_switch("restore", serial, pid, quick() ? 6000 : 20000);
	ulog("device given back to its usual configuration (%d)", code);
}

/* why libusb0 does not show an iPad / iPhone: the state to show */
static enum usbqt_state waiting_state(unsigned *problem)
{
	struct usbqt_device d;
	*problem = 0;
	if (!usbqt_find_device(&d))
		return USBQT_NO_DEVICE;
	*problem = d.problem;
	if (d.filtered && d.problem)
		return USBQT_DRIVER_FAILED;
	/* (Apple's support first: the USB driver of this program is of no use without it) */
	enum usbqt_apple apple = usbqt_apple_service();
	if (apple == USBQT_APPLE_MISSING || !usbqt_apple_driver(&d))
		return USBQT_NO_APPLE;
	if (apple == USBQT_APPLE_NOT_RUNNING)
		return USBQT_APPLE_STOPPED;
	if (!d.filtered)
		return USBQT_NO_DRIVER;
	return USBQT_REPLUG;
}

/* what the log needs to tell why a device does not mirror: its driver in Windows, Apple's service, its configuration */
static void describe(const struct found *f, int active, bool capturing, bool mux, enum usbqt_apple apple)
{
	struct usbqt_device d;
	bool listed = find_entry(f->serial, &d);
	ulog("iPad / iPhone on USB (product %04x): %d configurations, active %d (%s); Windows driver %s, lower filters "
	     "[%s]; Apple Mobile Device Service %s",
	     f->pid, f->dev->descriptor.bNumConfigurations, active,
	     capturing ? "screen capture" : mux ? "usual, with Apple's channel" :
	     active > 0 ? "without Apple's channel" : "unknown",
	     listed && d.service[0] ? d.service : "?", listed ? d.lower : "?",
	     apple == USBQT_APPLE_OK ? "present" : apple == USBQT_APPLE_NOT_RUNNING ? "stopped" : "missing");
}

/* A wait in which the device is not touched: only Windows' device list is looked at (each second), and the wait ends
 * when the device leaves it (unplugged, or coming back anew), when "try again now" is asked, or when stopping.
 * (Opening the device again and again is avoided on purpose: with Apple's filters under libusb0, frequent opening has
 * been seen to make a race in the drivers more likely; iPhoneMirror 1.5.10) */
static void wait_quietly(ULONGLONG ms, unsigned pid, const char *serial)
{
	ULONGLONG until = GetTickCount64() + ms, check = GetTickCount64() + 1000;
	while (GetTickCount64() < until && !stopping() && !retry_asked()) {
		if (GetTickCount64() >= check) {
			if (!interface_present(pid, serial))
				return;
			check = GetTickCount64() + 1000;
		}
		Sleep(50);
	}
}

/* How long a device in its usual configuration is left to Apple's service before it is switched (freshly plugged in,
 * or given back): the service greets it first. Only a device that trusts this computer sends its screen, and in the
 * capture configuration Apple's service cannot reach it: "Trust This Computer" can neither be asked nor answered */
#define SETTLE_MS 5000
/* "Trust" not answered: tried again this many times on its own (each a little later), then only when asked
 * ("Yeniden Dene") or when the device is plugged in anew; every try makes it leave and come back twice. The same,
 * after more tries, for sessions that fail */
#define LOCKED_TRIES 4
#define FAILED_TRIES 6

/* after a failure: the next try (or none on its own any more) */
static ULONGLONG after_failure(unsigned failures, enum usbqt_state *shown)
{
	if (failures >= FAILED_TRIES) {
		*shown = USBQT_FAILED;
		set_state(USBQT_FAILED);
		ulog("failed %u times: waiting for \"Yeniden Dene\" or for the device to be plugged in anew", failures);
		return ~0ull;
	}
	*shown = USBQT_ERROR;
	set_state(USBQT_ERROR);
	return GetTickCount64() + (failures > 3 ? 15000 : SETTLE_MS);
}

static DWORD WINAPI usb_main(LPVOID unused)
{
	(void)unused;
	wait_for_helpers();
	unsigned failures = 0, locked = 0;
	char seen[80] = "";                       /* the device on USB (its serial): new ones are described in the log */
	ULONGLONG not_before = 0;                 /* no switch before then */
	enum usbqt_state shown = USBQT_STARTING;  /* shown while waiting for that */
	enum usbqt_state waiting_logged = USBQT_OFF;
	while (!stopping()) {
		struct found f;
		if (!load_libusb() || !find_apple(NULL, 0, &f)) {
			seen[0] = '\0'; /* (unplugged: everything starts anew when it is back) */
			locked = failures = 0;
			unsigned problem = 0;
			enum usbqt_state st = waiting_state(&problem);
			if (st != waiting_logged) {
				if (st == USBQT_DRIVER_FAILED)
					ulog("the USB driver is installed, but Windows reports problem %u with the device", problem);
				else
					ulog("%s", st == USBQT_NO_DEVICE ? "no iPad / iPhone on USB" :
						   st == USBQT_NO_APPLE ? "an iPad / iPhone is on USB, but Apple's own support "
									  "(Apple Devices / iTunes) is missing or does not run it" :
						   st == USBQT_APPLE_STOPPED ? "an iPad / iPhone is on USB, but Apple Mobile Device "
									       "Service is stopped" :
						   st == USBQT_NO_DRIVER ? "an iPad / iPhone is on USB without the USB driver" :
									   "the USB driver is installed, not in use yet: unplug and plug the device");
				waiting_logged = st;
			}
			set_state(st);
			pause_ms(2000);
			take_retry();
			continue;
		}
		waiting_logged = USBQT_OFF;
		ULONGLONG now = GetTickCount64();
		bool fresh = strcmp(seen, f.serial) != 0;
		if (fresh) {
			snprintf(seen, sizeof(seen), "%s", f.serial);
			locked = failures = 0;
			not_before = now + SETTLE_MS;
			shown = USBQT_STARTING;
		}

		/* its configuration now (a standard request) and what that configuration offers */
		int active = -1;
		lu_handle *h = L.open(f.dev);
		if (h) {
			active = active_config(h);
			L.close(h);
		}
		struct capture_iface q;
		memset(&q, 0, sizeof(q));
		bool capturing = active > 0 && capture_in_descriptors(f.dev, active, &q);
		bool known = active > 0 && config_listed(f.dev, active);
		bool mux = known && config_has(f.dev, active, MUX_SUBCLASS);
		enum usbqt_apple apple = usbqt_apple_service();
		if (fresh)
			describe(&f, active, capturing, mux, apple);

		/* Apple's own support: its driver makes Windows use the capture configuration after the switch, its
		 * service pairs with the device. Without them a switch can only fail, and the device would leave and come
		 * back again and again; so it is not touched */
		if (!capturing && (apple != USBQT_APPLE_OK || (known && !mux))) {
			enum usbqt_state st = apple == USBQT_APPLE_NOT_RUNNING ? USBQT_APPLE_STOPPED : USBQT_NO_APPLE;
			if (U.state != st)
				ulog("%s", apple == USBQT_APPLE_MISSING ?
						   "Apple's service (Apple Devices / iTunes) is not installed: not switched" :
					   apple == USBQT_APPLE_NOT_RUNNING ?
						   "Apple Mobile Device Service is stopped: not switched" :
						   "Apple's driver does not run the device (Windows uses its photo-only configuration): "
						   "not switched");
			set_state(st);
			/* (looked at again in 10 s, or when the device comes back anew: installing Apple's driver does that) */
			wait_quietly(10000, f.pid, f.serial);
			take_retry();
			continue;
		}

		/* a wait planned (settling, trying again later), without touching the device; "try again now" ends it */
		if (!capturing && now < not_before) {
			if (!take_retry()) {
				set_state(shown);
				wait_quietly(not_before - now, f.pid, f.serial);
				continue;
			}
			ulog("trying again now (asked)");
		}
		take_retry(); /* (asked meanwhile: this is the try) */
		/* (while "Trust" is awaited, that stays on screen through the tries) */
		set_state(shown == USBQT_LOCKED ? USBQT_LOCKED : USBQT_STARTING);

		bool newly = false;
		if (!capturing) {
			ulog("switching the device to screen capture");
			int code = run_switch("activate", f.serial, f.pid, 25000);
			ulog("switch: %d", code);
			if (code != 0 && code != 20 && code != 32 && code != 33) {
				not_before = after_failure(++failures, &shown);
				continue;
			}
			newly = code != 20;
		}
		if (stopping()) {
			if (newly || capturing)
				give_back(f.serial, f.pid);
			break;
		}
		unsigned pid = f.pid; /* (as it shows itself now) */
		lu_handle *stream_h = open_capture(f.serial, &pid, newly, &q);
		if (!stream_h) {
			give_back(f.serial, pid);
			not_before = after_failure(++failures, &shown);
			continue;
		}
		ULONGLONG began = GetTickCount64();
		enum end_reason why = stream(stream_h, &q, newly, f.serial, pid);
		bool short_session = GetTickCount64() - began < 30000;
		if (why != END_GONE)
			give_back(f.serial, pid);
		if (stopping())
			break;
		now = GetTickCount64();
		if (why == END_LOCKED) {
			/* No answer: it does not trust this computer (yet), or it is locked. Given back at once (above): in its
			 * usual configuration Apple's service reaches it again, "Trust This Computer" can be asked and
			 * answered. Tried again later, each time a little later; then only when asked */
			locked++;
			shown = USBQT_LOCKED;
			set_state(USBQT_LOCKED);
			if (locked >= LOCKED_TRIES) {
				not_before = ~0ull;
				ulog("no answer %u times: waiting for \"Yeniden Dene\" or for the device to be plugged in anew",
				     locked);
			} else {
				DWORD wait = locked == 1 ? 15000 : locked == 2 ? 30000 : 60000;
				not_before = now + wait;
				ulog("tried again in %lu s (at once with \"Yeniden Dene\")", (unsigned long)(wait / 1000));
			}
			continue;
		}
		locked = 0;
		if (why == END_ERROR || (why == END_DEVICE_STOPPED && short_session)) {
			/* (a session the device ends at once counts as a failure too: it is not switched again and again) */
			not_before = after_failure(++failures, &shown);
		} else {
			failures = 0;
			shown = USBQT_STARTING;
			not_before = now + SETTLE_MS;
		}
	}
	set_state(USBQT_OFF);
	return 0;
}

/* ------------------------------------------------------------------ start / stop */

bool usbqt_start(const struct usbqt_callbacks *cb)
{
	if (U.thread) {
		if (WaitForSingleObject(U.thread, 0) == WAIT_TIMEOUT)
			return !stopping(); /* running; or still giving the device back: try again a little later */
		CloseHandle(U.thread);
		U.thread = NULL;
	}
	U.cb = *cb;
	U.state = USBQT_OFF;
	InterlockedExchange(&U.stop, 0);
	InterlockedExchange(&U.quick, 0);
	InterlockedExchange(&U.streaming, 0);
	InterlockedExchange(&U.detached, 0);
	InterlockedExchange(&U.retry, 0);
	U.thread = CreateThread(NULL, 0, usb_main, NULL, 0, NULL);
	return U.thread != NULL;
}

void usbqt_retry(void)
{
	InterlockedExchange(&U.retry, 1);
}

void usbqt_stop(bool wait)
{
	if (!U.thread)
		return;
	InterlockedExchange(&U.stop, 1);
	if (wait) {
		InterlockedExchange(&U.quick, 1);
		if (WaitForSingleObject(U.thread, 15000) == WAIT_OBJECT_0) {
			CloseHandle(U.thread);
			U.thread = NULL;
		} else {
			/* (still waiting for a helper: the program goes on ending, so nothing may be called back any more) */
			InterlockedExchange(&U.detached, 1);
		}
	}
}

bool usbqt_running(void)
{
	return U.thread && WaitForSingleObject(U.thread, 0) == WAIT_TIMEOUT;
}

bool usbqt_streaming(void)
{
	return InterlockedCompareExchange(&U.streaming, 0, 0) != 0;
}

#else /* not Windows: wired mirroring needs the Windows driver */

bool usbqt_start(const struct usbqt_callbacks *cb)
{
	(void)cb;
	return false;
}

void usbqt_stop(bool wait)
{
	(void)wait;
}

bool usbqt_running(void)
{
	return false;
}

bool usbqt_streaming(void)
{
	return false;
}

void usbqt_retry(void)
{
}

bool usbqt_find_device(struct usbqt_device *out)
{
	memset(out, 0, sizeof(*out));
	return false;
}

enum usbqt_apple usbqt_apple_service(void)
{
	return USBQT_APPLE_MISSING;
}

bool usbqt_apple_driver(const struct usbqt_device *d)
{
	(void)d;
	return false;
}

bool usbqt_is_helper(unsigned long pid)
{
	(void)pid;
	return false;
}

int usbqt_switch_main(int argc, char **argv)
{
	(void)argc;
	(void)argv;
	return 1;
}

#endif

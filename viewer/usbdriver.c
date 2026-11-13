/*
 * Relios Mirror: the installer of the USB driver for wired mirroring (relios-usb-kurucu.exe).
 *
 *   relios-usb-kurucu.exe install|remove <device instance>      (run by the program with administrator rights)
 *
 * It is a program of its own, kept small on purpose: it runs with administrator rights from the program's folder,
 * so it loads nothing from that folder. It is linked statically and imports only Windows' own KnownDLLs (kernel32,
 * advapi32, setupapi, shell32) and the C runtime of Windows; bcrypt.dll is loaded from System32 by its full path.
 * The build checks that list (paketle.sh).
 *
 * install: the two files of libusb-win32 1.2.6.0 next to it (usb\libusb0.sys, usb\libusb0.dll) are read once and
 *          checked against their SHA-256 fingerprints, and what was checked is written to System32 (only if the
 *          files are not there yet: another libusb0 that is already installed is never replaced, and then nothing
 *          is done); the libusb0 kernel service is made (or an existing one checked); "libusb0" is added to the
 *          iPad's upper filters and the iPad is restarted. If Windows does not start it again without a problem
 *          (e.g. Memory Integrity refusing the old driver), everything this run did is undone.
 * remove:  "libusb0" is taken out of the iPad's upper filters and the iPad restarted (the service and the files
 *          stay: without a device that names it the driver is never loaded, and other programs may use them).
 *
 * As libusb-win32's own install-filter does (add the filter, remove the device's SurpriseRemovalOK value, restart
 * the device) and as iPhoneMirror (GPL-3.0-only) does around it (checked files, a health check, undoing).
 */
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
#include <windows.h>
#include <bcrypt.h>
#include <setupapi.h>
#include <cfgmgr32.h>
#include <shellapi.h>

#include "usbids.h"
#include "usbqt.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* libusb-win32 1.2.6.0, amd64 (https://sourceforge.net/projects/libusb-win32/files/libusb-win32-releases/1.2.6.0/).
 * The build checks the same fingerprints when it downloads them */
static const char sys_sha256[] = "8058f2afe6ef96a7d2ded432997fd8655970c9ea75a938ee4557d6a2cb4cc989";
static const char dll_sha256[] = "4f18b5d2c28aa66b648c8683c6d09b52b92cbbee85984bbefad5f38a64bc2a14";

/* ------------------------------------------------------------------ SHA-256 (bcrypt.dll from System32) */

typedef NTSTATUS(WINAPI *open_alg_fn)(BCRYPT_ALG_HANDLE *, LPCWSTR, LPCWSTR, ULONG);
typedef NTSTATUS(WINAPI *create_hash_fn)(BCRYPT_ALG_HANDLE, BCRYPT_HASH_HANDLE *, PUCHAR, ULONG, PUCHAR, ULONG, ULONG);
typedef NTSTATUS(WINAPI *hash_data_fn)(BCRYPT_HASH_HANDLE, PUCHAR, ULONG, ULONG);
typedef NTSTATUS(WINAPI *finish_hash_fn)(BCRYPT_HASH_HANDLE, PUCHAR, ULONG, ULONG);
typedef NTSTATUS(WINAPI *destroy_hash_fn)(BCRYPT_HASH_HANDLE);
typedef NTSTATUS(WINAPI *close_alg_fn)(BCRYPT_ALG_HANDLE, ULONG);

static bool sha256_hex(const uint8_t *data, size_t n, char out[65])
{
	static HMODULE dll;
	static open_alg_fn open_alg;
	static create_hash_fn create_hash;
	static hash_data_fn hash_data;
	static finish_hash_fn finish_hash;
	static destroy_hash_fn destroy_hash;
	static close_alg_fn close_alg;
	out[0] = '\0';
	if (!dll) {
		dll = LoadLibraryExW(L"bcrypt.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
		if (!dll)
			return false;
#define GET(var, type, name) (var = (type)(void (*)(void))GetProcAddress(dll, name))
		if (!GET(open_alg, open_alg_fn, "BCryptOpenAlgorithmProvider") ||
		    !GET(create_hash, create_hash_fn, "BCryptCreateHash") ||
		    !GET(hash_data, hash_data_fn, "BCryptHashData") || !GET(finish_hash, finish_hash_fn, "BCryptFinishHash") ||
		    !GET(destroy_hash, destroy_hash_fn, "BCryptDestroyHash") ||
		    !GET(close_alg, close_alg_fn, "BCryptCloseAlgorithmProvider")) {
			FreeLibrary(dll);
			dll = NULL;
			return false;
		}
#undef GET
	}
	BCRYPT_ALG_HANDLE alg = NULL;
	BCRYPT_HASH_HANDLE hash = NULL;
	UCHAR digest[32];
	bool ok = false;
	if (n > 0x7FFFFFFF || (LONG)open_alg(&alg, BCRYPT_SHA256_ALGORITHM, NULL, 0) < 0)
		return false;
	if ((LONG)create_hash(alg, &hash, NULL, 0, NULL, 0, 0) >= 0) {
		ok = (LONG)hash_data(hash, (PUCHAR)data, (ULONG)n, 0) >= 0 &&
		     (LONG)finish_hash(hash, digest, (ULONG)sizeof(digest), 0) >= 0;
		destroy_hash(hash);
	}
	close_alg(alg, 0);
	if (ok) {
		for (int i = 0; i < 32; i++)
			snprintf(out + i * 2, 3, "%02x", digest[i]);
	}
	return ok;
}

/* ------------------------------------------------------------------ files */

/* a whole file (at most max bytes) */
static uint8_t *read_whole(const wchar_t *path, size_t max, size_t *n)
{
	*n = 0;
	HANDLE f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
	if (f == INVALID_HANDLE_VALUE)
		return NULL;
	uint8_t *buf = NULL;
	LARGE_INTEGER size;
	if (GetFileSizeEx(f, &size) && size.QuadPart > 0 && (unsigned long long)size.QuadPart <= max) {
		DWORD want = (DWORD)size.QuadPart, got = 0;
		buf = malloc(want);
		if (buf && ReadFile(f, buf, want, &got, NULL) && got == want) {
			*n = want;
		} else {
			free(buf);
			buf = NULL;
		}
	}
	CloseHandle(f);
	return buf;
}

enum { FILE_MISSING, FILE_SAME, FILE_OTHER };

static int file_state(const wchar_t *path, const char *sha)
{
	if (GetFileAttributesW(path) == INVALID_FILE_ATTRIBUTES) {
		DWORD e = GetLastError();
		return e == ERROR_FILE_NOT_FOUND || e == ERROR_PATH_NOT_FOUND ? FILE_MISSING : FILE_OTHER;
	}
	size_t n = 0;
	uint8_t *data = read_whole(path, 16u << 20, &n);
	char hex[65];
	bool same = data && sha256_hex(data, n, hex) && !strcmp(hex, sha);
	free(data);
	return same ? FILE_SAME : FILE_OTHER;
}

/* a file that is not there yet, written from memory (what was checked is what is written), then checked again */
static bool write_new(const wchar_t *path, const uint8_t *data, size_t n, const char *sha)
{
	wchar_t tmp[MAX_PATH + 64];
	if (wcslen(path) + 16 >= sizeof(tmp) / sizeof(tmp[0]))
		return false;
	wcscpy(tmp, path);
	wcscat(tmp, L".relios-tmp");
	DeleteFileW(tmp);
	HANDLE f = CreateFileW(tmp, GENERIC_WRITE, 0, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
	if (f == INVALID_HANDLE_VALUE)
		return false;
	DWORD put = 0;
	bool ok = WriteFile(f, data, (DWORD)n, &put, NULL) && put == (DWORD)n && FlushFileBuffers(f);
	CloseHandle(f);
	if (!ok || !MoveFileExW(tmp, path, MOVEFILE_WRITE_THROUGH)) {
		DeleteFileW(tmp);
		return false;
	}
	return file_state(path, sha) == FILE_SAME;
}

static void delete_now_or_later(const wchar_t *path)
{
	if (!DeleteFileW(path))
		MoveFileExW(path, NULL, MOVEFILE_DELAY_UNTIL_REBOOT);
}

/* ------------------------------------------------------------------ the service */

/* whether a service's binary path is <System32>\drivers\libusb0.sys (as Windows reads it: relative to the Windows
 * folder, \SystemRoot\..., \??\C:\... or C:\...) */
static bool is_system_driver_path(const wchar_t *path)
{
	wchar_t win[MAX_PATH], sys[MAX_PATH], full[MAX_PATH * 3], want[MAX_PATH + 40];
	UINT wn = GetSystemWindowsDirectoryW(win, MAX_PATH), sn = GetSystemDirectoryW(sys, MAX_PATH);
	if (!wn || wn >= MAX_PATH || !sn || sn >= MAX_PATH || wcslen(path) >= MAX_PATH * 2)
		return false;
	_snwprintf(want, MAX_PATH + 39, L"%ls\\drivers\\libusb0.sys", sys);
	want[MAX_PATH + 39] = L'\0';
	if (!wcsncmp(path, L"\\??\\", 4))
		path += 4;
	if (!_wcsnicmp(path, L"\\SystemRoot\\", 12))
		_snwprintf(full, MAX_PATH * 3 - 1, L"%ls%ls", win, path + 11);
	else if (!_wcsnicmp(path, L"System32\\", 9))
		_snwprintf(full, MAX_PATH * 3 - 1, L"%ls\\%ls", win, path);
	else if (path[0] && path[1] == L':')
		_snwprintf(full, MAX_PATH * 3 - 1, L"%ls", path);
	else
		return false;
	full[MAX_PATH * 3 - 1] = L'\0';
	return !_wcsicmp(full, want);
}

/* the libusb0 kernel service: an existing one must be a kernel driver that is not disabled, loaded from
 * <System32>\drivers\libusb0.sys; otherwise it is made (made: whether this call made it). 0 when ready */
static int ensure_service(bool *made)
{
	*made = false;
	SC_HANDLE scm = OpenSCManagerW(NULL, NULL, SC_MANAGER_CONNECT | SC_MANAGER_CREATE_SERVICE);
	if (!scm)
		return -1;
	int result = -1;
	SC_HANDLE svc = OpenServiceW(scm, L"libusb0", SERVICE_QUERY_CONFIG);
	if (svc) {
		DWORD need = 0;
		QueryServiceConfigW(svc, NULL, 0, &need);
		QUERY_SERVICE_CONFIGW *cfg = need > 0 && need < 65536 ? malloc(need) : NULL;
		if (cfg && QueryServiceConfigW(svc, cfg, need, &need) && cfg->dwServiceType == SERVICE_KERNEL_DRIVER &&
		    cfg->dwStartType != SERVICE_DISABLED && cfg->lpBinaryPathName && is_system_driver_path(cfg->lpBinaryPathName))
			result = 0;
		free(cfg);
		CloseServiceHandle(svc);
	} else if (GetLastError() == ERROR_SERVICE_DOES_NOT_EXIST) {
		svc = CreateServiceW(scm, L"libusb0", L"libusb-win32 - Kernel Driver, Version 1.2.6.0", SERVICE_QUERY_CONFIG,
				     SERVICE_KERNEL_DRIVER, SERVICE_DEMAND_START, SERVICE_ERROR_NORMAL,
				     L"System32\\drivers\\libusb0.sys", NULL, NULL, NULL, NULL, NULL);
		if (svc) {
			*made = true;
			result = 0;
			CloseServiceHandle(svc);
		}
	}
	CloseServiceHandle(scm);
	return result;
}

static void delete_service(void)
{
	SC_HANDLE scm = OpenSCManagerW(NULL, NULL, SC_MANAGER_CONNECT);
	SC_HANDLE svc = scm ? OpenServiceW(scm, L"libusb0", DELETE) : NULL;
	if (svc) {
		DeleteService(svc);
		CloseServiceHandle(svc);
	}
	if (scm)
		CloseServiceHandle(scm);
}

/* ------------------------------------------------------------------ the device */

/* the device's upper filters (REG_MULTI_SZ) */
struct filters {
	wchar_t list[1024];
	bool existed;
	DWORD bytes;
};

static bool read_filters(HDEVINFO set, SP_DEVINFO_DATA *info, struct filters *f)
{
	DWORD type = 0, size = 0;
	memset(f, 0, sizeof(*f));
	if (SetupDiGetDeviceRegistryPropertyW(set, info, SPDRP_UPPERFILTERS, &type, (PBYTE)f->list, sizeof(f->list) - 4,
					      &size)) {
		if (type != REG_MULTI_SZ || size > sizeof(f->list) - 4)
			return false;
		f->existed = true;
		f->bytes = size;
		return true;
	}
	return GetLastError() == ERROR_INVALID_DATA; /* (the device has none) */
}

static bool has_libusb0(const struct filters *f)
{
	for (const wchar_t *p = f->list; *p && p < f->list + 1020; p += wcslen(p) + 1) {
		if (!_wcsicmp(p, L"libusb0"))
			return true;
	}
	return false;
}

/* the list without libusb0, and with it at the end when add; an empty list removes the value */
static bool write_filters(HDEVINFO set, SP_DEVINFO_DATA *info, const struct filters *old, bool add)
{
	wchar_t list[1100];
	size_t o = 0;
	for (const wchar_t *p = old->list; *p && p < old->list + 1020; p += wcslen(p) + 1) {
		size_t len = wcslen(p);
		if (!_wcsicmp(p, L"libusb0"))
			continue;
		if (o + len + 12 >= sizeof(list) / sizeof(list[0]))
			return false;
		memcpy(list + o, p, (len + 1) * sizeof(wchar_t));
		o += len + 1;
	}
	if (add) {
		memcpy(list + o, L"libusb0", 8 * sizeof(wchar_t));
		o += 8;
	}
	list[o] = L'\0';
	if (o == 0)
		return SetupDiSetDeviceRegistryPropertyW(set, info, SPDRP_UPPERFILTERS, NULL, 0) != FALSE;
	return SetupDiSetDeviceRegistryPropertyW(set, info, SPDRP_UPPERFILTERS, (const BYTE *)list,
						 (DWORD)((o + 1) * sizeof(wchar_t))) != FALSE;
}

static bool restore_filters(HDEVINFO set, SP_DEVINFO_DATA *info, const struct filters *old)
{
	if (!old->existed)
		return SetupDiSetDeviceRegistryPropertyW(set, info, SPDRP_UPPERFILTERS, NULL, 0) != FALSE;
	return SetupDiSetDeviceRegistryPropertyW(set, info, SPDRP_UPPERFILTERS, (const BYTE *)old->list, old->bytes) !=
	       FALSE;
}

/* the device is stopped and started again (its drivers loaded anew), as libusb-win32's installer does */
static void restart_device(HDEVINFO set, SP_DEVINFO_DATA *info)
{
	SP_PROPCHANGE_PARAMS p;
	memset(&p, 0, sizeof(p));
	p.ClassInstallHeader.cbSize = sizeof(SP_CLASSINSTALL_HEADER);
	p.ClassInstallHeader.InstallFunction = DIF_PROPERTYCHANGE;
	p.StateChange = DICS_PROPCHANGE;
	p.Scope = DICS_FLAG_CONFIGSPECIFIC;
	p.HwProfile = 0;
	if (SetupDiSetClassInstallParamsW(set, info, &p.ClassInstallHeader, sizeof(p)))
		SetupDiCallClassInstaller(DIF_PROPERTYCHANGE, set, info);
}

/* present, started and without a problem (waits at most timeout_ms for it) */
static bool device_healthy(const wchar_t *instance, DWORD timeout_ms)
{
	ULONGLONG until = GetTickCount64() + timeout_ms;
	for (;;) {
		DEVINST node = 0;
		ULONG status = 0, problem = 0;
		if (CM_Locate_DevNodeW(&node, (DEVINSTID_W)instance, CM_LOCATE_DEVNODE_NORMAL) == CR_SUCCESS &&
		    CM_Get_DevNode_Status(&status, &problem, node, 0) == CR_SUCCESS && !(status & DN_HAS_PROBLEM) &&
		    (status & DN_STARTED))
			return true;
		if (GetTickCount64() >= until)
			return false;
		Sleep(250);
	}
}

/* (libusb-win32's installer removes this value of the device when it adds the filter) */
static void remove_surprise_value(HDEVINFO set, SP_DEVINFO_DATA *info)
{
	HKEY key = SetupDiOpenDevRegKey(set, info, DICS_FLAG_GLOBAL, 0, DIREG_DEV, KEY_SET_VALUE);
	if (key && key != (HKEY)INVALID_HANDLE_VALUE) {
		RegDeleteValueW(key, L"SurpriseRemovalOK");
		RegCloseKey(key);
	}
}

/* ------------------------------------------------------------------ install / remove */

static int install_filter(HDEVINFO set, SP_DEVINFO_DATA *info, const wchar_t *instance)
{
	if (!device_healthy(instance, 0))
		return USBQT_DRIVER_DEVICE_PROBLEM;

	/* the files: read once, checked, and written from what was checked */
	wchar_t folder[MAX_PATH * 2], src_sys[MAX_PATH * 2 + 40], src_dll[MAX_PATH * 2 + 40];
	DWORD n = GetModuleFileNameW(NULL, folder, MAX_PATH * 2);
	wchar_t *slash = n && n < MAX_PATH * 2 ? wcsrchr(folder, L'\\') : NULL;
	if (!slash)
		return USBQT_DRIVER_NO_FILES;
	slash[1] = L'\0';
	_snwprintf(src_sys, MAX_PATH * 2 + 39, L"%lsusb\\libusb0.sys", folder);
	_snwprintf(src_dll, MAX_PATH * 2 + 39, L"%lsusb\\libusb0.dll", folder);
	src_sys[MAX_PATH * 2 + 39] = src_dll[MAX_PATH * 2 + 39] = L'\0';
	size_t sys_n = 0, dll_n = 0;
	uint8_t *sys = read_whole(src_sys, 4u << 20, &sys_n), *dll = read_whole(src_dll, 4u << 20, &dll_n);
	char hex[65];
	int result = USBQT_DRIVER_DONE;
	if (!sys || !dll)
		result = USBQT_DRIVER_NO_FILES;
	else if (!sha256_hex(sys, sys_n, hex) || strcmp(hex, sys_sha256) || !sha256_hex(dll, dll_n, hex) ||
		 strcmp(hex, dll_sha256))
		result = USBQT_DRIVER_BAD_FILES;

	wchar_t sysdir[MAX_PATH], dst_sys[MAX_PATH + 40], dst_dll[MAX_PATH + 40];
	UINT len = GetSystemDirectoryW(sysdir, MAX_PATH);
	if (len == 0 || len >= MAX_PATH) {
		if (result == USBQT_DRIVER_DONE)
			result = USBQT_DRIVER_COPY;
		sysdir[0] = L'\0';
	}
	_snwprintf(dst_sys, MAX_PATH + 39, L"%ls\\drivers\\libusb0.sys", sysdir);
	_snwprintf(dst_dll, MAX_PATH + 39, L"%ls\\libusb0.dll", sysdir);
	dst_sys[MAX_PATH + 39] = dst_dll[MAX_PATH + 39] = L'\0';
	bool made_sys = false, made_dll = false, made_service = false;
	if (result == USBQT_DRIVER_DONE) {
		/* a libusb0 already installed by another program is left as it is */
		int sys_state = file_state(dst_sys, sys_sha256), dll_state = file_state(dst_dll, dll_sha256);
		if (sys_state == FILE_OTHER || dll_state == FILE_OTHER)
			result = USBQT_DRIVER_OTHER_VERSION;
		if (result == USBQT_DRIVER_DONE && sys_state == FILE_MISSING) {
			made_sys = write_new(dst_sys, sys, sys_n, sys_sha256);
			if (!made_sys)
				result = USBQT_DRIVER_COPY;
		}
		if (result == USBQT_DRIVER_DONE && dll_state == FILE_MISSING) {
			made_dll = write_new(dst_dll, dll, dll_n, dll_sha256);
			if (!made_dll)
				result = USBQT_DRIVER_COPY;
		}
	}
	free(sys);
	free(dll);
	if (result == USBQT_DRIVER_DONE && ensure_service(&made_service) != 0)
		result = USBQT_DRIVER_SERVICE;

	struct filters before;
	if (result == USBQT_DRIVER_DONE && !read_filters(set, info, &before))
		result = USBQT_DRIVER_REGISTRY;
	if (result == USBQT_DRIVER_DONE && !has_libusb0(&before)) {
		if (!write_filters(set, info, &before, true)) {
			result = USBQT_DRIVER_REGISTRY;
		} else {
			remove_surprise_value(set, info);
			restart_device(set, info);
			if (!device_healthy(instance, 20000)) {
				/* Windows did not start the device with the filter: back as it was */
				restore_filters(set, info, &before);
				restart_device(set, info);
				device_healthy(instance, 20000);
				result = USBQT_DRIVER_NOT_LOADED;
			}
		}
	}
	if (result != USBQT_DRIVER_DONE) {
		/* (what this run added is taken away again) */
		if (made_service)
			delete_service();
		if (made_sys)
			delete_now_or_later(dst_sys);
		if (made_dll)
			delete_now_or_later(dst_dll);
	}
	return result;
}

static int remove_filter(HDEVINFO set, SP_DEVINFO_DATA *info, const wchar_t *instance)
{
	struct filters before;
	if (!read_filters(set, info, &before))
		return USBQT_DRIVER_REGISTRY;
	if (!has_libusb0(&before))
		return USBQT_DRIVER_DONE;
	if (!write_filters(set, info, &before, false))
		return USBQT_DRIVER_REGISTRY;
	DEVINST node = 0;
	if (CM_Locate_DevNodeW(&node, (DEVINSTID_W)instance, CM_LOCATE_DEVNODE_NORMAL) == CR_SUCCESS) {
		restart_device(set, info);
		device_healthy(instance, 20000);
	}
	return USBQT_DRIVER_DONE;
}

static int run(int argc, wchar_t **argv)
{
	if (argc != 3)
		return USBQT_DRIVER_ARGUMENTS;
	bool install = !wcscmp(argv[1], L"install");
	if (!install && wcscmp(argv[1], L"remove"))
		return USBQT_DRIVER_ARGUMENTS;
	/* the device instance: plain ASCII, as the program found it */
	char instance[200];
	wchar_t winstance[200];
	size_t i = 0;
	for (; argv[2][i]; i++) {
		if (i + 1 >= sizeof(instance) || argv[2][i] > 0x7f)
			return USBQT_DRIVER_ARGUMENTS;
		instance[i] = (char)toupper((unsigned char)argv[2][i]);
		winstance[i] = (wchar_t)(unsigned char)instance[i];
	}
	instance[i] = '\0';
	winstance[i] = L'\0';
	if (!valid_instance(instance))
		return USBQT_DRIVER_ARGUMENTS;

	HDEVINFO set = SetupDiCreateDeviceInfoList(NULL, NULL);
	if (set == INVALID_HANDLE_VALUE)
		return USBQT_DRIVER_NO_DEVICE;
	SP_DEVINFO_DATA info;
	memset(&info, 0, sizeof(info));
	info.cbSize = sizeof(info);
	int result = USBQT_DRIVER_NO_DEVICE;
	if (SetupDiOpenDeviceInfoW(set, winstance, NULL, 0, &info))
		result = install ? install_filter(set, &info, winstance) : remove_filter(set, &info, winstance);
	SetupDiDestroyDeviceInfoList(set);
	return result;
}

int WINAPI WinMain(HINSTANCE instance, HINSTANCE previous, LPSTR args, int show)
{
	(void)instance;
	(void)previous;
	(void)args;
	(void)show;
	/* (anything loaded later comes from System32 only) */
	SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_SYSTEM32);
	int argc = 0;
	wchar_t **argv = CommandLineToArgvW(GetCommandLineW(), &argc);
	int result = argv ? run(argc, argv) : USBQT_DRIVER_ARGUMENTS;
	if (argv)
		LocalFree(argv);
	return result;
}

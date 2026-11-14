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
 * winusb-install <interface device>: Microsoft's WinUSB for the iPad's screen-capture interface alone (see below).
 * remove:  "libusb0" is taken out of the iPad's upper filters and the iPad restarted. Then, if this program
 *          installed the driver (marked in the service's key) and no device or device class names libusb0 any
 *          more, the service and the files it put into System32 are deleted too (a file still in use: at the next
 *          restart of Windows), so that nothing of it is left (it would also keep Memory Integrity off).
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
#include <shlobj.h>

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

/* a file this program put there, if it is still that file: deleted (true: only at the next restart of Windows) */
static bool delete_ours(const wchar_t *path, const char *sha)
{
	if (file_state(path, sha) != FILE_SAME || DeleteFileW(path))
		return false;
	MoveFileExW(path, NULL, MOVEFILE_DELAY_UNTIL_REBOOT);
	return true;
}

/* whether Windows is to delete this file at its next restart (a removal still waiting for it) */
static bool delete_pending(const wchar_t *path)
{
	static const wchar_t key[] = L"SYSTEM\\CurrentControlSet\\Control\\Session Manager";
	DWORD size = 0;
	if (RegGetValueW(HKEY_LOCAL_MACHINE, key, L"PendingFileRenameOperations", RRF_RT_REG_MULTI_SZ, NULL, NULL,
			 &size) != ERROR_SUCCESS ||
	    size == 0 || size > (4u << 20))
		return false;
	wchar_t *buf = calloc(size / sizeof(wchar_t) + 4, sizeof(wchar_t));
	if (!buf)
		return false;
	bool found = false;
	if (RegGetValueW(HKEY_LOCAL_MACHINE, key, L"PendingFileRenameOperations", RRF_RT_REG_MULTI_SZ, NULL, buf, &size) ==
	    ERROR_SUCCESS) {
		/* pairs of strings: the file (\??\C:\...), then where it goes ("" when it is to be deleted) */
		const wchar_t *end = buf + size / sizeof(wchar_t);
		for (const wchar_t *p = buf; p < end && *p;) {
			const wchar_t *file = p;
			p += wcslen(p) + 1;
			if (p >= end)
				break;
			const wchar_t *to = p;
			p += wcslen(p) + 1;
			if (!wcsncmp(file, L"\\??\\", 4))
				file += 4;
			found = found || (!*to && !_wcsicmp(file, path));
		}
	}
	free(buf);
	return found;
}

/* ------------------------------------------------------------------ the service */

/* what this program made for the driver, noted in the service's own key (so the note goes with it):
 * 1 the service, 2 System32\drivers\libusb0.sys, 4 System32\libusb0.dll */
#define SERVICE_KEY L"SYSTEM\\CurrentControlSet\\Services\\libusb0"

static DWORD read_marks(void)
{
	DWORD v = 0, size = sizeof(v);
	if (RegGetValueW(HKEY_LOCAL_MACHINE, SERVICE_KEY, L"ReliosMirror", RRF_RT_REG_DWORD, NULL, &v, &size) !=
	    ERROR_SUCCESS)
		return 0;
	return v & 7;
}

static void write_marks(DWORD v)
{
	RegSetKeyValueW(HKEY_LOCAL_MACHINE, SERVICE_KEY, L"ReliosMirror", REG_DWORD, &v, sizeof(v));
}

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
 * <System32>\drivers\libusb0.sys; otherwise it is made (made: whether this call made it). 0 when ready, -2 when
 * it is still waiting to be deleted (a removal waiting for a restart of Windows) */
static int ensure_service(bool *made)
{
	*made = false;
	SC_HANDLE scm = OpenSCManagerW(NULL, NULL, SC_MANAGER_CONNECT | SC_MANAGER_CREATE_SERVICE);
	if (!scm)
		return -1;
	int result = -1;
	SC_HANDLE svc = OpenServiceW(scm, L"libusb0", SERVICE_QUERY_CONFIG | SERVICE_CHANGE_CONFIG);
	if (svc) {
		DWORD need = 0;
		QueryServiceConfigW(svc, NULL, 0, &need);
		QUERY_SERVICE_CONFIGW *cfg = need > 0 && need < 65536 ? malloc(need) : NULL;
		if (cfg && QueryServiceConfigW(svc, cfg, need, &need) && cfg->dwServiceType == SERVICE_KERNEL_DRIVER &&
		    cfg->dwStartType != SERVICE_DISABLED && cfg->lpBinaryPathName && is_system_driver_path(cfg->lpBinaryPathName))
			result = 0;
		free(cfg);
		/* (a change that changes nothing: refused only for a service marked for deletion) */
		if (result == 0 &&
		    !ChangeServiceConfigW(svc, SERVICE_NO_CHANGE, SERVICE_NO_CHANGE, SERVICE_NO_CHANGE, NULL, NULL, NULL, NULL,
					  NULL, NULL, NULL) &&
		    GetLastError() == ERROR_SERVICE_MARKED_FOR_DELETE)
			result = -2;
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

/* whether Apple's driver runs the device: Windows' composite driver (under Apple's filters). Otherwise Windows runs
 * it as a photo camera alone, and the filter would be of no use there */
static bool apple_runs_it(HDEVINFO set, SP_DEVINFO_DATA *info)
{
	wchar_t service[64];
	DWORD type = 0, got = 0;
	memset(service, 0, sizeof(service));
	if (!SetupDiGetDeviceRegistryPropertyW(set, info, SPDRP_SERVICE, &type, (PBYTE)service, sizeof(service) - 4,
					       &got) ||
	    type != REG_SZ)
		return true; /* (not readable: not held against it; the program checked it as well) */
	return !_wcsicmp(service, L"usbccgp");
}

static int install_filter(HDEVINFO set, SP_DEVINFO_DATA *info, const wchar_t *instance)
{
	if (!device_healthy(instance, 0))
		return USBQT_DRIVER_DEVICE_PROBLEM;
	if (!apple_runs_it(set, info))
		return USBQT_DRIVER_NO_APPLE;

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
	/* (an earlier removal whose files go only at the next restart: installing now would lose them then) */
	if (result == USBQT_DRIVER_DONE && (delete_pending(dst_sys) || delete_pending(dst_dll)))
		result = USBQT_DRIVER_RESTART_FIRST;
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
	if (result == USBQT_DRIVER_DONE) {
		int service = ensure_service(&made_service);
		if (service == -2)
			result = USBQT_DRIVER_RESTART_FIRST;
		else if (service != 0)
			result = USBQT_DRIVER_SERVICE;
	}

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
	} else {
		/* (noted for a removal: only what this program made is ever deleted) */
		DWORD marks = read_marks();
		if (made_service || (marks & 1))
			write_marks(marks | (made_service ? 1u : 0u) | (made_sys ? 2u : 0u) | (made_dll ? 4u : 0u));
	}
	return result;
}

/* whether any device (present or not) or any device class still names libusb0, as a filter or as its driver */
static bool libusb0_in_use(void)
{
	bool used = false;
	HDEVINFO set = SetupDiGetClassDevsW(NULL, NULL, NULL, DIGCF_ALLCLASSES);
	if (set == INVALID_HANDLE_VALUE)
		return true; /* (not known: kept) */
	SP_DEVINFO_DATA info;
	info.cbSize = sizeof(info);
	static const DWORD props[] = {SPDRP_UPPERFILTERS, SPDRP_LOWERFILTERS, SPDRP_SERVICE};
	for (DWORD i = 0; !used && SetupDiEnumDeviceInfo(set, i, &info); i++) {
		for (int k = 0; k < 3 && !used; k++) {
			wchar_t buf[1024];
			DWORD type = 0, size = 0;
			memset(buf, 0, sizeof(buf));
			if (!SetupDiGetDeviceRegistryPropertyW(set, &info, props[k], &type, (PBYTE)buf, sizeof(buf) - 4, &size))
				continue;
			for (const wchar_t *p = buf; *p && p < buf + 1020; p += wcslen(p) + 1)
				used = used || !_wcsicmp(p, L"libusb0");
		}
	}
	SetupDiDestroyDeviceInfoList(set);
	if (used)
		return true;
	/* (libusb-win32 can also be installed as a filter of a whole device class) */
	HKEY classes;
	if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Control\\Class", 0, KEY_READ, &classes) !=
	    ERROR_SUCCESS)
		return true;
	for (DWORD i = 0; !used; i++) {
		wchar_t name[80];
		DWORD len = 80;
		LONG r = RegEnumKeyExW(classes, i, name, &len, NULL, NULL, NULL, NULL);
		if (r == ERROR_NO_MORE_ITEMS)
			break;
		if (r != ERROR_SUCCESS)
			continue;
		static const wchar_t *const values[] = {L"UpperFilters", L"LowerFilters"};
		for (int k = 0; k < 2 && !used; k++) {
			wchar_t buf[1024];
			DWORD size = sizeof(buf) - 4;
			memset(buf, 0, sizeof(buf));
			if (RegGetValueW(classes, name, values[k], RRF_RT_REG_MULTI_SZ, NULL, buf, &size) != ERROR_SUCCESS)
				continue;
			for (const wchar_t *p = buf; *p && p < buf + 1020; p += wcslen(p) + 1)
				used = used || !_wcsicmp(p, L"libusb0");
		}
	}
	RegCloseKey(classes);
	return used;
}

/* the driver itself: deleted if this program installed it and nothing names it any more */
static int remove_driver(void)
{
	DWORD marks = read_marks();
	if (!(marks & 1) || libusb0_in_use())
		return USBQT_DRIVER_KEPT;
	wchar_t sysdir[MAX_PATH], path[MAX_PATH + 40];
	UINT len = GetSystemDirectoryW(sysdir, MAX_PATH);
	if (!len || len >= MAX_PATH)
		return USBQT_DRIVER_KEPT;
	bool later = false;
	if (marks & 2) {
		_snwprintf(path, MAX_PATH + 39, L"%ls\\drivers\\libusb0.sys", sysdir);
		path[MAX_PATH + 39] = L'\0';
		later = delete_ours(path, sys_sha256) || later;
	}
	if (marks & 4) {
		_snwprintf(path, MAX_PATH + 39, L"%ls\\libusb0.dll", sysdir);
		path[MAX_PATH + 39] = L'\0';
		later = delete_ours(path, dll_sha256) || later;
	}
	/* (marked for deletion: gone at once, or when the driver is unloaded) */
	delete_service();
	return later ? USBQT_DRIVER_REBOOT : USBQT_DRIVER_DONE;
}

static int remove_filter(HDEVINFO set, SP_DEVINFO_DATA *info, const wchar_t *instance)
{
	struct filters before;
	if (!read_filters(set, info, &before))
		return USBQT_DRIVER_REGISTRY;
	if (has_libusb0(&before)) {
		if (!write_filters(set, info, &before, false))
			return USBQT_DRIVER_REGISTRY;
		DEVINST node = 0;
		if (CM_Locate_DevNodeW(&node, (DEVINSTID_W)instance, CM_LOCATE_DEVNODE_NORMAL) == CR_SUCCESS) {
			restart_device(set, info);
			device_healthy(instance, 20000);
		}
	}
	return remove_driver();
}

/* ------------------------------------------------------------------ WinUSB for the screen-capture interface */

/* Under Apple's driver, Windows' composite driver (usbccgp) opens a device of its own for each interface of the
 * iPad's capture configuration. The capture interface's device (class FF/2A) gets whatever driver Windows had for
 * that interface number (an iPad's network adapter, which cannot run it: problem 10). Here Microsoft's WinUSB, from
 * Windows' own winusb.inf, is made its driver, as Device Manager does when "WinUsb Device" is picked from the list
 * by hand: no driver package of ours, no certificate. Apple's driver keeps the iPad itself (pairing, "Trust"), and
 * the configuration stays Windows' to select: nothing selects one through libusb0 (that crashed Windows, 1.3.4). */

static const GUID usbdevice_class = {0x88bae032, 0x5a81, 0x49f0, {0xbc, 0x3d, 0xa4, 0xff, 0x13, 0x82, 0x16, 0xd6}};

/* whether one of the device's compatible IDs is the screen-capture interface's (USB\Class_ff&SubClass_2a...) */
static bool is_capture_interface(HDEVINFO set, SP_DEVINFO_DATA *info)
{
	wchar_t ids[1024];
	DWORD type = 0;
	memset(ids, 0, sizeof(ids));
	if (!SetupDiGetDeviceRegistryPropertyW(set, info, SPDRP_COMPATIBLEIDS, &type, (PBYTE)ids, sizeof(ids) - 4,
					       NULL) ||
	    type != REG_MULTI_SZ)
		return false;
	for (const wchar_t *p = ids; p < ids + 1020 && *p; p += wcslen(p) + 1) {
		if (!_wcsnicmp(p, L"USB\\Class_ff&SubClass_2a", 24))
			return true;
	}
	return false;
}

/* whether this program marked the device (its interface GUID in the device's DeviceInterfaceGUIDs) */
static bool marked_ours(HDEVINFO set, SP_DEVINFO_DATA *info)
{
	HKEY key = SetupDiOpenDevRegKey(set, info, DICS_FLAG_GLOBAL, 0, DIREG_DEV, KEY_READ);
	if (key == INVALID_HANDLE_VALUE)
		return false;
	wchar_t v[256];
	DWORD type = 0, size = sizeof(v) - 4;
	memset(v, 0, sizeof(v));
	bool ours = RegQueryValueExW(key, L"DeviceInterfaceGUIDs", NULL, &type, (BYTE *)v, &size) == ERROR_SUCCESS &&
		    type == REG_MULTI_SZ && !_wcsicmp(v, USBQT_CAPTURE_GUID_W);
	RegCloseKey(key);
	return ours;
}

static bool started_without_problem(const wchar_t *instance, DWORD timeout_ms)
{
	ULONGLONG until = GetTickCount64() + timeout_ms;
	for (;;) {
		DEVINST node = 0;
		ULONG status = 0, problem = 0;
		if (CM_Locate_DevNodeW(&node, (DEVINSTID_W)instance, CM_LOCATE_DEVNODE_NORMAL) == CR_SUCCESS &&
		    CM_Get_DevNode_Status(&status, &problem, node, 0) == CR_SUCCESS && (status & DN_STARTED) &&
		    !(status & DN_HAS_PROBLEM))
			return true;
		if (GetTickCount64() >= until)
			return false;
		Sleep(250);
	}
}

static int winusb_install(HDEVINFO set, SP_DEVINFO_DATA *info, const wchar_t *instance)
{
	/* only the screen-capture interface of an Apple device that is here now */
	DEVINST node = 0;
	if (CM_Locate_DevNodeW(&node, (DEVINSTID_W)instance, CM_LOCATE_DEVNODE_NORMAL) != CR_SUCCESS)
		return USBQT_DRIVER_NO_DEVICE;
	if (!is_capture_interface(set, info))
		return USBQT_DRIVER_NOT_CAPTURE;
	wchar_t service[64];
	DWORD type = 0;
	memset(service, 0, sizeof(service));
	if (SetupDiGetDeviceRegistryPropertyW(set, info, SPDRP_SERVICE, &type, (PBYTE)service, sizeof(service) - 4,
					      NULL) &&
	    !_wcsicmp(service, L"WinUSB") && marked_ours(set, info))
		return USBQT_DRIVER_DONE; /* (already) */

	/* the interface GUID WinUSB registers it under (how the program finds it), in the device's own key */
	HKEY key = SetupDiCreateDevRegKeyW(set, info, DICS_FLAG_GLOBAL, 0, DIREG_DEV, NULL, NULL);
	if (key == INVALID_HANDLE_VALUE)
		return USBQT_DRIVER_REGISTRY;
	static const wchar_t guids[] = USBQT_CAPTURE_GUID_W L"\0";
	LONG w = RegSetValueExW(key, L"DeviceInterfaceGUIDs", 0, REG_MULTI_SZ, (const BYTE *)guids, sizeof(guids));
	RegCloseKey(key);
	if (w != ERROR_SUCCESS)
		return USBQT_DRIVER_REGISTRY;

	/* WinUSB's list: Windows' own winusb.inf alone, of its class ("Universal Serial Bus devices"), also for a
	 * device whose IDs it does not name (Device Manager's "pick from a list") */
	wchar_t old_class[64];
	memset(old_class, 0, sizeof(old_class));
	SetupDiGetDeviceRegistryPropertyW(set, info, SPDRP_CLASSGUID, &type, (PBYTE)old_class, sizeof(old_class) - 4,
					  NULL);
	/* (written out by hand: ole32's StringFromGUID2 is not among this program's DLLs) */
	wchar_t cls[64];
	const GUID *g = &usbdevice_class;
	_snwprintf(cls, 63, L"{%08lX-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X}", (unsigned long)g->Data1, g->Data2,
		   g->Data3, g->Data4[0], g->Data4[1], g->Data4[2], g->Data4[3], g->Data4[4], g->Data4[5], g->Data4[6],
		   g->Data4[7]);
	cls[63] = L'\0';
	int result = USBQT_DRIVER_WINUSB_STEP + 1;
	SP_DEVINSTALL_PARAMS_W params;
	memset(&params, 0, sizeof(params));
	params.cbSize = sizeof(params);
	wchar_t windir[MAX_PATH];
	UINT wl = GetSystemWindowsDirectoryW(windir, MAX_PATH);
	if (!wl || wl > MAX_PATH - 30)
		return USBQT_DRIVER_WINUSB;
	if (!SetupDiSetDeviceRegistryPropertyW(set, info, SPDRP_CLASSGUID, (const BYTE *)cls,
					       (DWORD)((wcslen(cls) + 1) * sizeof(wchar_t))))
		return USBQT_DRIVER_WINUSB_STEP + 1;
	/* (which step failed, for the log: USBQT_DRIVER_WINUSB_STEP + 1..5) */
	result = USBQT_DRIVER_WINUSB_STEP + 2;
	if (SetupDiGetDeviceInstallParamsW(set, info, &params)) {
		_snwprintf(params.DriverPath, MAX_PATH, L"%ls\\INF\\winusb.inf", windir);
		params.DriverPath[MAX_PATH - 1] = L'\0';
		params.Flags |= DI_ENUMSINGLEINF;
		params.FlagsEx |= DI_FLAGSEX_ALLOWEXCLUDEDDRVS;
		if (SetupDiSetDeviceInstallParamsW(set, info, &params) &&
		    SetupDiBuildDriverInfoList(set, info, SPDIT_CLASSDRIVER)) {
			SP_DRVINFO_DATA_W drv;
			memset(&drv, 0, sizeof(drv));
			drv.cbSize = sizeof(drv);
			bool found = false;
			for (DWORD i = 0; !found && SetupDiEnumDriverInfoW(set, info, SPDIT_CLASSDRIVER, i, &drv); i++)
				found = drv.DriverType == SPDIT_CLASSDRIVER;
			result = found ? USBQT_DRIVER_WINUSB_STEP + 4 : USBQT_DRIVER_WINUSB_STEP + 3;
			/* newdev.dll (Device Manager's own installer), from System32 */
			typedef BOOL(WINAPI * di_install_fn)(HWND, HDEVINFO, PSP_DEVINFO_DATA, PSP_DRVINFO_DATA_W, DWORD, PBOOL);
			HMODULE newdev = found ? LoadLibraryExW(L"newdev.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32) : NULL;
			di_install_fn di_install =
				newdev ? (di_install_fn)(void (*)(void))GetProcAddress(newdev, "DiInstallDevice") : NULL;
			BOOL reboot = FALSE;
			if (di_install && di_install(NULL, set, info, &drv, 0, &reboot)) {
				/* (the mark again, should the installation have rewritten the device's key; then a restart
				 * so that WinUSB reads it) */
				if (!marked_ours(set, info)) {
					HKEY again = SetupDiCreateDevRegKeyW(set, info, DICS_FLAG_GLOBAL, 0, DIREG_DEV, NULL, NULL);
					if (again != INVALID_HANDLE_VALUE) {
						RegSetValueExW(again, L"DeviceInterfaceGUIDs", 0, REG_MULTI_SZ, (const BYTE *)guids,
							       sizeof(guids));
						RegCloseKey(again);
					}
					restart_device(set, info);
				}
				result = marked_ours(set, info) && started_without_problem(instance, 15000) ?
						 USBQT_DRIVER_DONE :
						 USBQT_DRIVER_WINUSB_STEP + 5;
			}
			if (newdev)
				FreeLibrary(newdev);
			SetupDiDestroyDriverInfoList(set, info, SPDIT_CLASSDRIVER);
		}
	}
	if (result != USBQT_DRIVER_DONE) {
		/* put back: its old class (Windows then gives it its old driver again when it next appears) and
		 * the device removed, so that nothing half-made stays */
		if (old_class[0])
			SetupDiSetDeviceRegistryPropertyW(set, info, SPDRP_CLASSGUID, (const BYTE *)old_class,
							  (DWORD)((wcslen(old_class) + 1) * sizeof(wchar_t)));
		SetupDiRemoveDevice(set, info);
	}
	return result;
}

/* every device this program gave WinUSB (marked; present or not) taken away: Windows makes them anew, with their
 * old driver, when the iPad next shows them. Apple's own WinUSB devices (its pairing channel) are not marked */
static void winusb_remove_all(void)
{
	HDEVINFO set = SetupDiGetClassDevsW(NULL, L"USB", NULL, DIGCF_ALLCLASSES);
	if (set == INVALID_HANDLE_VALUE)
		return;
	SP_DEVINFO_DATA info;
	info.cbSize = sizeof(info);
	for (DWORD i = 0; SetupDiEnumDeviceInfo(set, i, &info); i++) {
		wchar_t id[300];
		char a[300];
		if (!SetupDiGetDeviceInstanceIdW(set, &info, id, 300, NULL))
			continue;
		size_t k = 0;
		bool plain = true;
		for (; id[k] && k + 1 < sizeof(a); k++) {
			plain = plain && id[k] < 0x80;
			a[k] = (char)toupper((unsigned char)(id[k] & 0x7f));
		}
		a[k] = '\0';
		if (plain && valid_interface_instance(a) && marked_ours(set, &info))
			SetupDiRemoveDevice(set, &info);
	}
	SetupDiDestroyDeviceInfoList(set);
}

/* ------------------------------------------------------------------ the virtual camera */

/* The camera's DLL (softcam.dll, built by the workflow) is accepted only with the SHA-256 fingerprint the build
 * computed and wrote into this program (paketle.sh, CMakeLists.txt): a file swapped in the program's folder, which
 * the user can write to, is never registered with administrator rights. What was checked is what is written, to a
 * folder only administrators can change, and only from there is it made known to Windows. */
#ifdef SOFTCAM_SHA256
static const char cam_sha256[] = SOFTCAM_SHA256;
#else
static const char cam_sha256[] = ""; /* (a build without the camera: nothing can be installed) */
#endif

/* "<Program Files>\Relios Mirror" and the DLL there (from Windows' own record of the folder, not from the
 * environment, which the user can change) */
static bool camera_paths(wchar_t *dir, wchar_t *dll)
{
	wchar_t pf[MAX_PATH];
	if (SHGetFolderPathW(NULL, CSIDL_PROGRAM_FILES, NULL, SHGFP_TYPE_CURRENT, pf) != S_OK || !pf[0] ||
	    wcslen(pf) > MAX_PATH - 50)
		return false;
	_snwprintf(dir, MAX_PATH, L"%ls\\Relios Mirror", pf);
	_snwprintf(dll, MAX_PATH, L"%ls\\Relios Mirror\\ReliosMirrorCamera.dll", pf);
	dir[MAX_PATH - 1] = dll[MAX_PATH - 1] = L'\0';
	return true;
}

/* System32's regsvr32 on the DLL (silent; "/u": taken out of Windows); its exit code */
static DWORD regsvr32(const wchar_t *dll, bool remove)
{
	wchar_t exe[MAX_PATH + 20], cmd[MAX_PATH * 2 + 64];
	UINT len = GetSystemDirectoryW(exe, MAX_PATH);
	if (!len || len >= MAX_PATH)
		return 1;
	wcscat(exe, L"\\regsvr32.exe");
	_snwprintf(cmd, MAX_PATH * 2 + 63, L"\"%ls\" /s %ls\"%ls\"", exe, remove ? L"/u " : L"", dll);
	cmd[MAX_PATH * 2 + 63] = L'\0';
	STARTUPINFOW si;
	PROCESS_INFORMATION pi;
	memset(&si, 0, sizeof(si));
	memset(&pi, 0, sizeof(pi));
	si.cb = sizeof(si);
	wchar_t sysdir[MAX_PATH];
	GetSystemDirectoryW(sysdir, MAX_PATH);
	if (!CreateProcessW(exe, cmd, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, sysdir, &si, &pi))
		return 1;
	DWORD code = 1;
	if (WaitForSingleObject(pi.hProcess, 60000) == WAIT_OBJECT_0)
		GetExitCodeProcess(pi.hProcess, &code);
	CloseHandle(pi.hThread);
	CloseHandle(pi.hProcess);
	return code;
}

static int camera_install(void)
{
	if (strlen(cam_sha256) != 64)
		return USBQT_DRIVER_NO_FILES;
	wchar_t folder[MAX_PATH * 2], src[MAX_PATH * 2 + 40], dir[MAX_PATH], dll[MAX_PATH];
	DWORD n = GetModuleFileNameW(NULL, folder, MAX_PATH * 2);
	wchar_t *slash = n && n < MAX_PATH * 2 ? wcsrchr(folder, L'\\') : NULL;
	if (!slash || !camera_paths(dir, dll))
		return USBQT_DRIVER_NO_FILES;
	slash[1] = L'\0';
	_snwprintf(src, MAX_PATH * 2 + 39, L"%lssoftcam.dll", folder);
	src[MAX_PATH * 2 + 39] = L'\0';
	size_t size = 0;
	uint8_t *data = read_whole(src, 16u << 20, &size);
	char hex[65];
	if (!data)
		return USBQT_DRIVER_NO_FILES;
	if (!sha256_hex(data, size, hex) || strcmp(hex, cam_sha256)) {
		free(data);
		return USBQT_DRIVER_BAD_FILES;
	}
	/* the folder: made here if missing; one that is a link to somewhere else is not used */
	CreateDirectoryW(dir, NULL);
	DWORD attr = GetFileAttributesW(dir);
	int result = USBQT_DRIVER_DONE;
	if (attr == INVALID_FILE_ATTRIBUTES || !(attr & FILE_ATTRIBUTE_DIRECTORY) || (attr & FILE_ATTRIBUTE_REPARSE_POINT))
		result = USBQT_DRIVER_COPY;
	else if (file_state(dll, cam_sha256) != FILE_SAME) {
		/* (another file of that name, an older camera say: replaced; in use, it cannot be) */
		if (GetFileAttributesW(dll) != INVALID_FILE_ATTRIBUTES && !DeleteFileW(dll))
			result = USBQT_DRIVER_COPY;
		else if (!write_new(dll, data, size, cam_sha256))
			result = USBQT_DRIVER_COPY;
	}
	free(data);
	if (result == USBQT_DRIVER_DONE && regsvr32(dll, false) != 0)
		result = USBQT_DRIVER_REGISTER;
	return result;
}

static int camera_remove(void)
{
	wchar_t dir[MAX_PATH], dll[MAX_PATH];
	if (!camera_paths(dir, dll))
		return USBQT_DRIVER_NO_FILES;
	if (GetFileAttributesW(dll) != INVALID_FILE_ATTRIBUTES)
		regsvr32(dll, true);
	bool later = false;
	if (GetFileAttributesW(dll) != INVALID_FILE_ATTRIBUTES && !DeleteFileW(dll)) {
		/* (in use by a program that has the camera open: gone at the next restart of Windows) */
		MoveFileExW(dll, NULL, MOVEFILE_DELAY_UNTIL_REBOOT);
		later = true;
	}
	RemoveDirectoryW(dir); /* (only if empty) */
	return later ? USBQT_DRIVER_REBOOT : USBQT_DRIVER_DONE;
}

static int run(int argc, wchar_t **argv)
{
	if (argc == 2 && !wcscmp(argv[1], L"camera-install"))
		return camera_install();
	if (argc == 2 && !wcscmp(argv[1], L"camera-remove"))
		return camera_remove();
	if (argc != 3)
		return USBQT_DRIVER_ARGUMENTS;
	bool winusb = !wcscmp(argv[1], L"winusb-install");
	bool install = !wcscmp(argv[1], L"install");
	if (!install && !winusb && wcscmp(argv[1], L"remove"))
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
	if (winusb ? !valid_interface_instance(instance) : !valid_instance(instance))
		return USBQT_DRIVER_ARGUMENTS;

	HDEVINFO set = SetupDiCreateDeviceInfoList(NULL, NULL);
	if (set == INVALID_HANDLE_VALUE)
		return USBQT_DRIVER_NO_DEVICE;
	SP_DEVINFO_DATA info;
	memset(&info, 0, sizeof(info));
	info.cbSize = sizeof(info);
	int result = USBQT_DRIVER_NO_DEVICE;
	if (SetupDiOpenDeviceInfoW(set, winstance, NULL, 0, &info))
		result = winusb ? winusb_install(set, &info, winstance) :
			 install ? install_filter(set, &info, winstance) : remove_filter(set, &info, winstance);
	if (!install && !winusb)
		winusb_remove_all();
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

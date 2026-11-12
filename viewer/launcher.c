/*
 * Relios Mirror as one .exe: this small program carries the whole program folder (a .tgz appended to this file,
 * then "RLSM" + its size as 16 hex digits + a 14-digit build stamp). At the first start it unpacks the folder to
 * %LOCALAPPDATA%\ReliosMirror\app-<stamp> (with Windows' own tar.exe) and then starts ReliosMirror.exe from there.
 */
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FOOTER 34
#define PATH_MAX_W 2048

static void fail(const wchar_t *text)
{
	MessageBoxW(NULL, text, L"Relios Mirror", MB_ICONERROR);
	ExitProcess(1);
}

static BOOL run(wchar_t *cmd, const wchar_t *cwd, BOOL wait)
{
	STARTUPINFOW si;
	PROCESS_INFORMATION pi;
	memset(&si, 0, sizeof(si));
	memset(&pi, 0, sizeof(pi));
	si.cb = sizeof(si);
	if (!CreateProcessW(NULL, cmd, NULL, NULL, FALSE, wait ? CREATE_NO_WINDOW : 0, NULL, cwd, &si, &pi))
		return FALSE;
	if (wait)
		WaitForSingleObject(pi.hProcess, 300000);
	CloseHandle(pi.hThread);
	CloseHandle(pi.hProcess);
	return TRUE;
}

int WINAPI WinMain(HINSTANCE instance, HINSTANCE previous, LPSTR args, int show)
{
	(void)instance;
	(void)previous;
	(void)show;
	static wchar_t self[PATH_MAX_W], base[PATH_MAX_W], dir[PATH_MAX_W], exe[PATH_MAX_W], tgz[PATH_MAX_W],
		sys[PATH_MAX_W], cmd[PATH_MAX_W * 3];
	if (!GetModuleFileNameW(NULL, self, PATH_MAX_W - 1))
		fail(L"Program dosyası bulunamadı.");
	HANDLE in = CreateFileW(self, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
	LARGE_INTEGER size, pos;
	char foot[FOOTER + 1], hex[17], stamp[15];
	DWORD got = 0;
	if (in == INVALID_HANDLE_VALUE || !GetFileSizeEx(in, &size) || size.QuadPart < FOOTER)
		fail(L"Program dosyası okunamadı.");
	pos.QuadPart = size.QuadPart - FOOTER;
	if (!SetFilePointerEx(in, pos, NULL, FILE_BEGIN) || !ReadFile(in, foot, FOOTER, &got, NULL) || got != FOOTER ||
	    memcmp(foot, "RLSM", 4))
		fail(L"Program dosyası eksik ya da bozuk. Yeniden indirin.");
	memcpy(hex, foot + 4, 16);
	hex[16] = 0;
	memcpy(stamp, foot + 20, 14);
	stamp[14] = 0;
	for (int i = 0; i < 14; i++) {
		if (stamp[i] < '0' || stamp[i] > '9')
			fail(L"Program dosyası eksik ya da bozuk. Yeniden indirin.");
	}
	unsigned long long payload = _strtoui64(hex, NULL, 16);
	if (payload == 0 || payload > (unsigned long long)(size.QuadPart - FOOTER))
		fail(L"Program dosyası eksik ya da bozuk. Yeniden indirin.");

	if (!GetEnvironmentVariableW(L"LOCALAPPDATA", base, 1000))
		fail(L"Kullanıcı klasörü bulunamadı.");
	_snwprintf(dir, PATH_MAX_W - 1, L"%ls\\ReliosMirror", base);
	CreateDirectoryW(dir, NULL);
	_snwprintf(dir, PATH_MAX_W - 1, L"%ls\\ReliosMirror\\app-%hs", base, stamp);
	_snwprintf(exe, PATH_MAX_W - 1, L"%ls\\ReliosMirror.exe", dir);

	if (GetFileAttributesW(exe) == INVALID_FILE_ATTRIBUTES) {
		CreateDirectoryW(dir, NULL);
		_snwprintf(tgz, PATH_MAX_W - 1, L"%ls\\payload.tgz", dir);
		HANDLE out = CreateFileW(tgz, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
		if (out == INVALID_HANDLE_VALUE)
			fail(L"Program klasörü oluşturulamadı.");
		pos.QuadPart = size.QuadPart - FOOTER - (LONGLONG)payload;
		SetFilePointerEx(in, pos, NULL, FILE_BEGIN);
		DWORD chunk = 1024 * 1024, put = 0;
		char *buf = malloc(chunk);
		unsigned long long left = payload;
		while (buf && left > 0) {
			DWORD want = left > chunk ? chunk : (DWORD)left;
			if (!ReadFile(in, buf, want, &got, NULL) || got == 0 || !WriteFile(out, buf, got, &put, NULL) ||
			    put != got)
				break;
			left -= got;
		}
		free(buf);
		CloseHandle(out);
		if (left > 0)
			fail(L"Program dosyaları çıkarılamadı (disk dolu olabilir).");
		GetSystemDirectoryW(sys, 1000);
		_snwprintf(cmd, PATH_MAX_W * 3 - 1, L"\"%ls\\tar.exe\" -xf \"%ls\" -C \"%ls\"", sys, tgz, dir);
		if (!run(cmd, dir, TRUE))
			fail(L"Program dosyaları çıkarılamadı (Windows'un tar.exe'si çalışmadı; Windows 10 1803 ya da yenisi gerekir).");
		DeleteFileW(tgz);
		if (GetFileAttributesW(exe) == INVALID_FILE_ATTRIBUTES)
			fail(L"Program dosyaları çıkarılamadı.");
	}
	CloseHandle(in);
	SetEnvironmentVariableW(L"RELIOS_LAUNCHER", self); /* (for "start with Windows") */
	_snwprintf(cmd, PATH_MAX_W * 3 - 1, L"\"%ls\" %hs", exe, args ? args : "");
	if (!run(cmd, dir, FALSE))
		fail(L"Relios Mirror başlatılamadı.");
	return 0;
}

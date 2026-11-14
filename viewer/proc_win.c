/*
 * Relios Mirror: starting the receiver program on Windows (see proc.h).
 * Taken from ReliosOBS's plugins/relios-airplay/receiver-process.c, without OBS's helpers.
 *
 * Always: a job that ends it with this program, lets it start no other program and keeps it off the clipboard
 * and other windows. Then either
 *  - low integrity (PROC_BOX_LOW, as in ReliosOBS): it cannot change the user's files or other programs, but it
 *    can read most files, or
 *  - "Sıkı Koruma" (PROC_BOX_STRICT): an AppContainer ("ReliosMirror.Receiver", the box Windows keeps Store apps
 *    in). It reaches only what is given to it: read access to its own program file and its key, passing through
 *    the settings folder (its current folder), and the network capability for home and work (private) networks
 *    only: no internet, whatever the firewall rules say. Nothing of the user's (documents, pictures, browser
 *    data, other programs' folders) is readable from it. What it was given is noted (siki-koruma.txt in the
 *    settings folder) before it is given, and proc_strict_remove takes all of it back.
 */
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
#include <windows.h>
#include <aclapi.h>
#include <sddl.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "proc.h"

struct proc {
	HANDLE process;
	HANDLE job;
	HANDLE in_write;
	HANDLE out_read;
	HANDLE err_read;
};

static void log_error(const char *what)
{
	app_log("%s failed (error %lu)", what, (unsigned long)GetLastError());
}

static wchar_t *command_line(const char *exe_utf8, const char *args_utf8)
{
	size_t size = strlen(exe_utf8) + (args_utf8 ? strlen(args_utf8) : 0) + 8;
	char *cmd = malloc(size);
	if (!cmd)
		return NULL;
	snprintf(cmd, size, "\"%s\" %s", exe_utf8, args_utf8 ? args_utf8 : "");
	int n = MultiByteToWideChar(CP_UTF8, 0, cmd, -1, NULL, 0);
	wchar_t *w = n > 0 ? calloc((size_t)n + 1, sizeof(wchar_t)) : NULL;
	if (w)
		MultiByteToWideChar(CP_UTF8, 0, cmd, -1, w, n);
	free(cmd);
	return w;
}

/* a restricted copy of this process's token, at low integrity */
static HANDLE low_integrity_token(void)
{
	HANDLE own = NULL, restricted = NULL;
	if (!OpenProcessToken(GetCurrentProcess(),
			      TOKEN_DUPLICATE | TOKEN_QUERY | TOKEN_ADJUST_DEFAULT | TOKEN_ASSIGN_PRIMARY, &own)) {
		log_error("OpenProcessToken");
		return NULL;
	}
	if (!CreateRestrictedToken(own, 0, 0, NULL, 0, NULL, 0, NULL, &restricted)) {
		log_error("CreateRestrictedToken");
		CloseHandle(own);
		return NULL;
	}
	CloseHandle(own);

	PSID sid = NULL;
	if (!ConvertStringSidToSidW(L"S-1-16-4096", &sid)) { /* the low mandatory level */
		log_error("ConvertStringSidToSid");
		CloseHandle(restricted);
		return NULL;
	}
	TOKEN_MANDATORY_LABEL label;
	memset(&label, 0, sizeof(label));
	label.Label.Attributes = SE_GROUP_INTEGRITY;
	label.Label.Sid = sid;
	BOOL ok = SetTokenInformation(restricted, TokenIntegrityLevel, &label,
				      (DWORD)(sizeof(label) + GetLengthSid(sid)));
	LocalFree(sid);
	if (!ok) {
		log_error("SetTokenInformation(TokenIntegrityLevel)");
		CloseHandle(restricted);
		return NULL;
	}
	return restricted;
}

static HANDLE make_job(void)
{
	HANDLE job = CreateJobObjectW(NULL, NULL);
	if (!job) {
		log_error("CreateJobObject");
		return NULL;
	}
	JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits;
	memset(&limits, 0, sizeof(limits));
	limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE |
						  JOB_OBJECT_LIMIT_DIE_ON_UNHANDLED_EXCEPTION |
						  JOB_OBJECT_LIMIT_ACTIVE_PROCESS | JOB_OBJECT_LIMIT_PROCESS_MEMORY;
	limits.BasicLimitInformation.ActiveProcessLimit = 1; /* it may not start other programs */
	/* nor take the computer's memory: it needs a few tens of MB (a fault, or someone on the network, could make it
	 * ask for more and more: it then fails at 1 GB, and this program starts a new one) */
	limits.ProcessMemoryLimit = (SIZE_T)1024 * 1024 * 1024;
	if (!SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof(limits))) {
		log_error("SetInformationJobObject(limits)");
		CloseHandle(job);
		return NULL;
	}
	JOBOBJECT_BASIC_UI_RESTRICTIONS ui;
	memset(&ui, 0, sizeof(ui));
	ui.UIRestrictionsClass = JOB_OBJECT_UILIMIT_DESKTOP | JOB_OBJECT_UILIMIT_DISPLAYSETTINGS |
				 JOB_OBJECT_UILIMIT_EXITWINDOWS | JOB_OBJECT_UILIMIT_GLOBALATOMS |
				 JOB_OBJECT_UILIMIT_HANDLES | JOB_OBJECT_UILIMIT_READCLIPBOARD |
				 JOB_OBJECT_UILIMIT_SYSTEMPARAMETERS | JOB_OBJECT_UILIMIT_WRITECLIPBOARD;
	if (!SetInformationJobObject(job, JobObjectBasicUIRestrictions, &ui, sizeof(ui)))
		log_error("SetInformationJobObject(UI restrictions)");
	return job;
}

static bool make_pipe(HANDLE *read_end, HANDLE *write_end, bool child_reads)
{
	SECURITY_ATTRIBUTES sa;
	memset(&sa, 0, sizeof(sa));
	sa.nLength = sizeof(sa);
	sa.bInheritHandle = TRUE;
	if (!CreatePipe(read_end, write_end, &sa, 1024 * 1024)) {
		log_error("CreatePipe");
		return false;
	}
	/* this program's end is not inherited */
	SetHandleInformation(child_reads ? *write_end : *read_end, HANDLE_FLAG_INHERIT, 0);
	return true;
}

/* ------------------------------------------------------------------ "Sıkı Koruma": the AppContainer */

#define BOX_NAME L"ReliosMirror.Receiver"
#define BOX_NAME_UTF8 "ReliosMirror.Receiver"
#define BOX_NOTE "siki-koruma.txt" /* (in the current folder: the settings folder) */
#define PRIVATE_NETWORK_SID L"S-1-15-3-3" /* the capability privateNetworkClientServer: home and work networks */

#ifndef PROC_THREAD_ATTRIBUTE_SECURITY_CAPABILITIES
#define PROC_THREAD_ATTRIBUTE_SECURITY_CAPABILITIES 0x00020009
#endif
#ifndef PROC_THREAD_ATTRIBUTE_MITIGATION_POLICY
#define PROC_THREAD_ATTRIBUTE_MITIGATION_POLICY 0x00020007
#endif

/* In the box, Windows' exploit mitigations for the receiver as well (as browsers use for their sandboxed parts): a
 * damaged heap ends it at once; no legacy extension points (AppInit DLLs, Winsock LSPs, global hooks) are loaded into
 * it; it loads no DLL from a network share or with a low integrity label (one a sandboxed program could have made) */
#define BOX_MITIGATIONS                                                                                                \
	((1ULL << 12) /* HEAP_TERMINATE_ALWAYS_ON */ | (1ULL << 32) /* EXTENSION_POINT_DISABLE_ALWAYS_ON */ |       \
	 (1ULL << 52) /* IMAGE_LOAD_NO_REMOTE_ALWAYS_ON */ | (1ULL << 56) /* IMAGE_LOAD_NO_LOW_LABEL_ALWAYS_ON */)

/* SECURITY_CAPABILITIES, as Windows reads it */
struct box_capabilities {
	PSID AppContainerSid;
	PSID_AND_ATTRIBUTES Capabilities;
	DWORD CapabilityCount;
	DWORD Reserved;
};

typedef HRESULT(WINAPI *create_profile_fn)(PCWSTR, PCWSTR, PCWSTR, PSID_AND_ATTRIBUTES, DWORD, PSID *);
typedef HRESULT(WINAPI *derive_sid_fn)(PCWSTR, PSID *);
typedef HRESULT(WINAPI *delete_profile_fn)(PCWSTR);

/* (used from the window thread only) */
static struct {
	bool tried;
	create_profile_fn create;
	derive_sid_fn derive;
	delete_profile_fn remove;
} UE;

static wchar_t *wide(const char *utf8)
{
	int n = MultiByteToWideChar(CP_UTF8, 0, utf8, -1, NULL, 0);
	wchar_t *w = n > 0 ? calloc((size_t)n, sizeof(wchar_t)) : NULL;
	if (w && MultiByteToWideChar(CP_UTF8, 0, utf8, -1, w, n) <= 0) {
		free(w);
		w = NULL;
	}
	return w;
}

static char *narrow(const wchar_t *w)
{
	int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, NULL, 0, NULL, NULL);
	char *s = n > 0 ? calloc((size_t)n, 1) : NULL;
	if (s && WideCharToMultiByte(CP_UTF8, 0, w, -1, s, n, NULL, NULL) <= 0) {
		free(s);
		s = NULL;
	}
	return s;
}

/* Windows' AppContainer functions (userenv.dll, taken from System32 only); false if this Windows has none */
static bool box_functions(void)
{
	if (!UE.tried) {
		UE.tried = true;
		HMODULE m = LoadLibraryExW(L"userenv.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
		if (m) {
			UE.create = (create_profile_fn)(void *)GetProcAddress(m, "CreateAppContainerProfile");
			UE.derive = (derive_sid_fn)(void *)GetProcAddress(m, "DeriveAppContainerSidFromAppContainerName");
			UE.remove = (delete_profile_fn)(void *)GetProcAddress(m, "DeleteAppContainerProfile");
		}
		if (!UE.create || !UE.derive || !UE.remove)
			app_log("strict protection: this Windows has no AppContainer functions (userenv.dll)");
	}
	return UE.create && UE.derive && UE.remove;
}

/* the box's SID (to FreeSid); with `make` its profile is made first if it is not there yet. NULL on failure. */
static PSID box_sid(bool make)
{
	PSID sid = NULL;
	HRESULT hr;
	if (!box_functions())
		return NULL;
	if (make) {
		PSID network = NULL;
		if (!ConvertStringSidToSidW(PRIVATE_NETWORK_SID, &network)) {
			log_error("ConvertStringSidToSid(capability)");
			return NULL;
		}
		SID_AND_ATTRIBUTES capability;
		capability.Sid = network;
		capability.Attributes = SE_GROUP_ENABLED;
		hr = UE.create(BOX_NAME, L"Relios Mirror (alıcı)", L"Relios Mirror: Wi-Fi alıcısı, Sıkı Koruma", &capability,
			       1, &sid);
		LocalFree(network);
		if (SUCCEEDED(hr) && sid) {
			app_log("strict protection: AppContainer profile made");
			return sid;
		}
		if (hr != HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS)) {
			app_log("strict protection: CreateAppContainerProfile failed (0x%08lx)", (unsigned long)hr);
			return NULL;
		}
		sid = NULL;
	}
	hr = UE.derive(BOX_NAME, &sid);
	if (FAILED(hr) || !sid) {
		app_log("strict protection: DeriveAppContainerSidFromAppContainerName failed (0x%08lx)", (unsigned long)hr);
		return NULL;
	}
	return sid;
}

/* an entry of the object's own that lets `sid` do at least `mask`? */
static bool acl_gives(PACL dacl, PSID sid, DWORD mask)
{
	for (DWORD i = 0; i < dacl->AceCount; i++) {
		ACE_HEADER *h = NULL;
		if (!GetAce(dacl, i, (LPVOID *)&h) || h->AceType != ACCESS_ALLOWED_ACE_TYPE || (h->AceFlags & INHERIT_ONLY_ACE))
			continue;
		ACCESS_ALLOWED_ACE *a = (ACCESS_ALLOWED_ACE *)h;
		if (EqualSid((PSID)&a->SidStart, sid) && (a->Mask & mask) == mask)
			return true;
	}
	return false;
}

/* lets the box do `mask` on this one file or folder (nothing inside a folder: no inheritance) */
static bool acl_grant(const wchar_t *path, const char *what, PSID sid, DWORD mask)
{
	PACL dacl = NULL, updated = NULL;
	PSECURITY_DESCRIPTOR sd = NULL;
	DWORD r = GetNamedSecurityInfoW(path, SE_FILE_OBJECT, DACL_SECURITY_INFORMATION, NULL, NULL, &dacl, NULL, &sd);
	if (r != ERROR_SUCCESS) {
		app_log("strict protection: the permissions of %s could not be read (error %lu)", what, (unsigned long)r);
		return false;
	}
	bool ok;
	if (!dacl) {
		/* no permissions at all (a drive that keeps none, as FAT): anything may read it already. (A list with
		 * only the box in it would shut everyone else out: nothing is changed.) */
		ok = true;
	} else if (acl_gives(dacl, sid, mask)) {
		ok = true;
	} else {
		EXPLICIT_ACCESS_W ea;
		memset(&ea, 0, sizeof(ea));
		ea.grfAccessPermissions = mask;
		ea.grfAccessMode = GRANT_ACCESS;
		ea.grfInheritance = NO_INHERITANCE;
		ea.Trustee.TrusteeForm = TRUSTEE_IS_SID;
		ea.Trustee.TrusteeType = TRUSTEE_IS_UNKNOWN;
		ea.Trustee.ptstrName = (LPWSTR)sid;
		r = SetEntriesInAclW(1, &ea, dacl, &updated);
		if (r == ERROR_SUCCESS)
			r = SetNamedSecurityInfoW((LPWSTR)path, SE_FILE_OBJECT, DACL_SECURITY_INFORMATION, NULL, NULL, updated,
						  NULL);
		ok = r == ERROR_SUCCESS;
		if (!ok)
			app_log("strict protection: %s could not be given to the box (error %lu)", what, (unsigned long)r);
	}
	if (updated)
		LocalFree(updated);
	LocalFree(sd);
	return ok;
}

/* takes every entry for the box out of this file's or folder's own entries; true also if it is gone */
static bool acl_remove(const wchar_t *path, PSID sid)
{
	PACL dacl = NULL;
	PSECURITY_DESCRIPTOR sd = NULL;
	DWORD r = GetNamedSecurityInfoW(path, SE_FILE_OBJECT, DACL_SECURITY_INFORMATION, NULL, NULL, &dacl, NULL, &sd);
	if (r == ERROR_FILE_NOT_FOUND || r == ERROR_PATH_NOT_FOUND)
		return true;
	if (r != ERROR_SUCCESS) {
		app_log("strict protection: permissions could not be read to take them back (error %lu)", (unsigned long)r);
		return false;
	}
	bool found = false, ok = true;
	for (DWORD i = dacl ? dacl->AceCount : 0; i-- > 0;) {
		ACE_HEADER *h = NULL;
		if (!GetAce(dacl, i, (LPVOID *)&h) || (h->AceFlags & INHERITED_ACE) ||
		    (h->AceType != ACCESS_ALLOWED_ACE_TYPE && h->AceType != ACCESS_DENIED_ACE_TYPE))
			continue;
		/* (an "allow" and a "deny" entry are laid out alike) */
		if (EqualSid((PSID)&((ACCESS_ALLOWED_ACE *)h)->SidStart, sid) && DeleteAce(dacl, i))
			found = true;
	}
	if (found) {
		r = SetNamedSecurityInfoW((LPWSTR)path, SE_FILE_OBJECT, DACL_SECURITY_INFORMATION, NULL, NULL, dacl, NULL);
		ok = r == ERROR_SUCCESS;
		if (!ok)
			app_log("strict protection: a permission could not be taken back (error %lu)", (unsigned long)r);
	}
	LocalFree(sd);
	return ok;
}

/* the note of what the box was given, whole (NULL: there is none) */
static char *note_read(void)
{
	FILE *f = fopen(BOX_NOTE, "rb");
	if (!f)
		return NULL;
	size_t cap = 4096, len = 0, n;
	char *text = malloc(cap);
	while (text && (n = fread(text + len, 1, cap - 1 - len, f)) > 0) {
		len += n;
		if (len == cap - 1) {
			char *bigger = cap < (1u << 20) ? realloc(text, cap * 2) : NULL;
			if (!bigger)
				break; /* (kept as far as it was read) */
			text = bigger;
			cap *= 2;
		}
	}
	fclose(f);
	if (text)
		text[len] = '\0';
	return text;
}

/* the next line of a note (its end in *next); its length without the line break */
static size_t note_line(const char *p, const char **next)
{
	const char *end = strchr(p, '\n');
	size_t n = end ? (size_t)(end - p) : strlen(p);
	*next = end ? end + 1 : p + n;
	if (n && p[n - 1] == '\r')
		n--;
	return n;
}

/* notes one line (once); done before what it names is made or changed */
static bool note_add(const char *line)
{
	char *text = note_read();
	bool fresh = !text, have = false;
	size_t len = strlen(line);
	for (const char *p = text, *next; p && *p && !have; p = next)
		have = note_line(p, &next) == len && !memcmp(p, line, len);
	free(text);
	if (have)
		return true;
	FILE *f = fopen(BOX_NOTE, "ab");
	if (!f) {
		app_log("strict protection: the note " BOX_NOTE " cannot be written");
		return false;
	}
	bool ok = true;
	if (fresh)
		ok = fputs("Relios Mirror: Sıkı Koruma'nın Windows'ta verdiği izinler. Sıkı Koruma kapatılınca hepsi geri "
			   "alınır ve bu dosya silinir.\n",
			   f) >= 0;
	ok = fprintf(f, "%s\n", line) > 0 && ok;
	ok = fclose(f) == 0 && ok;
	if (!ok)
		app_log("strict protection: the note " BOX_NOTE " cannot be written");
	return ok;
}

bool proc_strict_prepare(const char *exe_utf8, const char *key_utf8)
{
	wchar_t folder[1024], key[1200];
	DWORD n = GetCurrentDirectoryW(1024, folder);
	if (!n || n >= 1024) {
		log_error("GetCurrentDirectory");
		return false;
	}
	if (!box_functions())
		return false;
	wchar_t *exe = wide(exe_utf8), *key_name = wide(key_utf8);
	bool ok = exe && key_name && wcslen(folder) + 1 + wcslen(key_name) < 1200;
	if (ok)
		_snwprintf(key, 1200, L"%ls\\%ls", folder, key_name);
	key[1199] = L'\0';

	/* first noted, then made: whatever happens in between, proc_strict_remove finds it */
	PSID sid = ok && note_add("profile " BOX_NAME_UTF8) ? box_sid(true) : NULL;
	const struct {
		const wchar_t *path;
		const char *what;
		DWORD mask;
	} items[3] = {
		/* the receiver itself (it loads nothing else from its folder: only Windows' own DLLs) */
		{exe, "the receiver's program file", FILE_GENERIC_READ | FILE_GENERIC_EXECUTE},
		/* the settings folder: only to pass through it, as its current folder (no listing, no reading) */
		{folder, "the settings folder", FILE_TRAVERSE | FILE_READ_ATTRIBUTES | SYNCHRONIZE},
		/* its key (read; it is made beforehand by this program) */
		{key, "the receiver's key", FILE_GENERIC_READ},
	};
	ok = sid != NULL;
	for (int i = 0; ok && i < 3; i++) {
		char *path = narrow(items[i].path);
		size_t size = path ? strlen(path) + 8 : 0;
		char *line = size ? malloc(size) : NULL;
		if (line)
			snprintf(line, size, "path %s", path);
		ok = line && note_add(line) && acl_grant(items[i].path, items[i].what, sid, items[i].mask);
		free(line);
		free(path);
	}
	if (sid)
		FreeSid(sid);
	free(exe);
	free(key_name);
	if (ok)
		app_log("strict protection ready: of the user's files the receiver may read only its program file and its key");
	return ok;
}

bool proc_strict_remove(void)
{
	char *text = note_read();
	if (!text)
		return true; /* never given anything, or all of it taken back already */
	if (!box_functions()) {
		/* (without these functions nothing could have been made) */
		free(text);
		remove(BOX_NOTE);
		return true;
	}
	PSID sid = box_sid(false);
	bool ok = sid != NULL;
	for (const char *p = text, *next; sid && *p; p = next) {
		size_t n = note_line(p, &next);
		if (n <= 5 || strncmp(p, "path ", 5))
			continue;
		char *path = malloc(n - 4);
		if (!path) {
			ok = false;
			continue;
		}
		memcpy(path, p + 5, n - 5);
		path[n - 5] = '\0';
		wchar_t *w = wide(path);
		ok = w && acl_remove(w, sid) && ok;
		free(w);
		free(path);
	}
	if (sid)
		FreeSid(sid);
	/* the profile: its folder (AppData\Local\Packages\ReliosMirror.Receiver) and its registry entries */
	HRESULT hr = UE.remove(BOX_NAME);
	bool gone = SUCCEEDED(hr) || hr == HRESULT_FROM_WIN32(ERROR_NOT_FOUND) || hr == HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND) ||
		    hr == HRESULT_FROM_WIN32(ERROR_PATH_NOT_FOUND);
	if (!gone)
		app_log("strict protection: DeleteAppContainerProfile failed (0x%08lx)", (unsigned long)hr);
	ok = ok && gone;
	free(text);
	if (ok) {
		remove(BOX_NOTE);
		app_log("strict protection taken away: its profile and every permission it was given are gone");
	} else {
		app_log("strict protection: not all of it could be taken back (tried again at the next start)");
	}
	return ok;
}

/* ------------------------------------------------------------------ starting it */

struct proc *proc_start(const char *exe_utf8, const char *args_utf8, enum proc_box box)
{
	struct proc *p = calloc(1, sizeof(*p));
	HANDLE in_read = NULL, out_write = NULL, err_write = NULL, token = NULL;
	LPPROC_THREAD_ATTRIBUTE_LIST attrs = NULL;
	bool attrs_ready = false, strict = box == PROC_BOX_STRICT;
	wchar_t *cmd = NULL;
	PROCESS_INFORMATION pi;
	STARTUPINFOEXW si;
	HANDLE inherit[3];
	SIZE_T attr_size = 0;
	DWORD flags, attr_count = strict ? 3 : 1;
	DWORD64 mitigations = BOX_MITIGATIONS;
	BOOL created;
	bool ok = false;
	PSID box_id = NULL, network = NULL;
	SID_AND_ATTRIBUTES capability;
	struct box_capabilities caps;

	memset(&pi, 0, sizeof(pi));
	memset(&si, 0, sizeof(si));
	memset(&capability, 0, sizeof(capability));
	memset(&caps, 0, sizeof(caps));
	if (!p)
		return NULL;

	if (strict) {
		/* the box (made by proc_strict_prepare) and the one thing it may do besides: use private networks */
		box_id = box_sid(false);
		if (!box_id)
			goto done;
		if (!ConvertStringSidToSidW(PRIVATE_NETWORK_SID, &network)) {
			log_error("ConvertStringSidToSid(capability)");
			goto done;
		}
		capability.Sid = network;
		capability.Attributes = SE_GROUP_ENABLED;
		caps.AppContainerSid = box_id;
		caps.Capabilities = &capability;
		caps.CapabilityCount = 1;
	}

	if (!make_pipe(&in_read, &p->in_write, true) || !make_pipe(&p->out_read, &out_write, false) ||
	    !make_pipe(&p->err_read, &err_write, false))
		goto done;

	InitializeProcThreadAttributeList(NULL, attr_count, 0, &attr_size);
	attrs = calloc(1, attr_size ? attr_size : 1);
	if (!attrs || !InitializeProcThreadAttributeList(attrs, attr_count, 0, &attr_size)) {
		log_error("InitializeProcThreadAttributeList");
		goto done;
	}
	attrs_ready = true;
	inherit[0] = in_read;
	inherit[1] = out_write;
	inherit[2] = err_write;
	if (!UpdateProcThreadAttribute(attrs, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherit, sizeof(inherit), NULL,
				       NULL)) {
		log_error("UpdateProcThreadAttribute(handle list)");
		goto done;
	}
	if (strict && !UpdateProcThreadAttribute(attrs, 0, PROC_THREAD_ATTRIBUTE_SECURITY_CAPABILITIES, &caps,
						 sizeof(caps), NULL, NULL)) {
		log_error("UpdateProcThreadAttribute(security capabilities)");
		goto done;
	}
	if (strict && !UpdateProcThreadAttribute(attrs, 0, PROC_THREAD_ATTRIBUTE_MITIGATION_POLICY, &mitigations,
						 sizeof(mitigations), NULL, NULL)) {
		log_error("UpdateProcThreadAttribute(mitigation policy)");
		goto done;
	}

	si.StartupInfo.cb = sizeof(si);
	si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
	si.StartupInfo.hStdInput = in_read;
	si.StartupInfo.hStdOutput = out_write;
	si.StartupInfo.hStdError = err_write;
	si.lpAttributeList = attrs;

	p->job = make_job();
	if (!p->job)
		goto done;

	cmd = command_line(exe_utf8, args_utf8);
	if (!cmd)
		goto done;
	if (strict) {
		/* Windows makes the AppContainer's token from this program's own. No console (no console host in the
		 * box): its standard handles are the pipes. A failure while Windows loads it ends it with a code (seen
		 * by proc_alive) instead of showing a message box. */
		flags = CREATE_SUSPENDED | DETACHED_PROCESS | EXTENDED_STARTUPINFO_PRESENT | CREATE_UNICODE_ENVIRONMENT;
		UINT mode = GetErrorMode();
		SetErrorMode(mode | SEM_FAILCRITICALERRORS);
		created = CreateProcessW(NULL, cmd, NULL, NULL, TRUE, flags, NULL, NULL, &si.StartupInfo, &pi);
		DWORD error = GetLastError();
		SetErrorMode(mode);
		if (!created)
			app_log("CreateProcess (strict protection) failed (error %lu)", (unsigned long)error);
	} else {
		flags = CREATE_SUSPENDED | CREATE_NO_WINDOW | EXTENDED_STARTUPINFO_PRESENT | CREATE_UNICODE_ENVIRONMENT;
		token = low_integrity_token();
		if (!token)
			goto done;
		created = CreateProcessAsUserW(token, NULL, cmd, NULL, NULL, TRUE, flags, NULL, NULL, &si.StartupInfo,
					       &pi);
		if (!created)
			log_error("CreateProcessAsUser");
	}
	if (!created)
		goto done;

	if (!AssignProcessToJobObject(p->job, pi.hProcess)) {
		log_error("AssignProcessToJobObject");
		TerminateProcess(pi.hProcess, 1);
		CloseHandle(pi.hThread);
		CloseHandle(pi.hProcess);
		goto done;
	}
	ResumeThread(pi.hThread);
	CloseHandle(pi.hThread);
	p->process = pi.hProcess;
	ok = true;

done:
	if (attrs_ready)
		DeleteProcThreadAttributeList(attrs);
	free(attrs);
	free(cmd);
	if (token)
		CloseHandle(token);
	if (box_id)
		FreeSid(box_id);
	if (network)
		LocalFree(network);
	/* the child's ends: only the child keeps them */
	if (in_read)
		CloseHandle(in_read);
	if (out_write)
		CloseHandle(out_write);
	if (err_write)
		CloseHandle(err_write);
	if (!ok) {
		proc_stop(p, 0);
		proc_destroy(p);
		return NULL;
	}
	return p;
}

bool proc_alive(struct proc *p, unsigned wait_ms, unsigned long *code)
{
	DWORD exit_code = 0;
	if (!p || !p->process)
		return false;
	if (WaitForSingleObject(p->process, wait_ms) != WAIT_OBJECT_0)
		return true;
	GetExitCodeProcess(p->process, &exit_code);
	if (code)
		*code = exit_code;
	return false;
}

int proc_run(const char *exe_utf8, const char *args_utf8, unsigned timeout_ms)
{
	wchar_t *cmd = command_line(exe_utf8, args_utf8);
	STARTUPINFOW si;
	PROCESS_INFORMATION pi;
	int code = -1;
	memset(&si, 0, sizeof(si));
	memset(&pi, 0, sizeof(pi));
	si.cb = sizeof(si);
	if (!cmd)
		return -1;
	if (CreateProcessW(NULL, cmd, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
		if (WaitForSingleObject(pi.hProcess, timeout_ms) == WAIT_OBJECT_0) {
			DWORD exit_code = 1;
			GetExitCodeProcess(pi.hProcess, &exit_code);
			code = (int)exit_code;
		} else {
			TerminateProcess(pi.hProcess, 1);
		}
		CloseHandle(pi.hThread);
		CloseHandle(pi.hProcess);
	} else {
		log_error("CreateProcess");
	}
	free(cmd);
	return code;
}

static size_t read_handle(HANDLE h, void *buf, size_t size)
{
	DWORD got = 0;
	if (!h)
		return 0;
	if (!ReadFile(h, buf, (DWORD)(size > 0x7fffffff ? 0x7fffffff : size), &got, NULL))
		return 0;
	return got;
}

size_t proc_read_out(struct proc *p, void *buf, size_t size)
{
	return read_handle(p->out_read, buf, size);
}

size_t proc_read_err(struct proc *p, void *buf, size_t size)
{
	return read_handle(p->err_read, buf, size);
}

bool proc_write(struct proc *p, const char *text)
{
	DWORD len = (DWORD)strlen(text), done = 0;
	if (!p || !p->in_write)
		return false;
	return WriteFile(p->in_write, text, len, &done, NULL) && done == len;
}

void proc_stop(struct proc *p, unsigned wait_ms)
{
	if (!p)
		return;
	if (p->in_write) {
		CloseHandle(p->in_write);
		p->in_write = NULL;
	}
	if (p->process && WaitForSingleObject(p->process, wait_ms) != WAIT_OBJECT_0) {
		app_log("the receiver did not end by itself; ending it");
		if (p->job)
			TerminateJobObject(p->job, 1);
		WaitForSingleObject(p->process, 2000);
	}
	/* (with the process gone, its ends of the pipes are closed and the reading threads finish) */
}

void proc_destroy(struct proc *p)
{
	if (!p)
		return;
	if (p->in_write)
		CloseHandle(p->in_write);
	if (p->out_read)
		CloseHandle(p->out_read);
	if (p->err_read)
		CloseHandle(p->err_read);
	if (p->process)
		CloseHandle(p->process);
	if (p->job)
		CloseHandle(p->job); /* KILL_ON_JOB_CLOSE: nothing of it stays */
	free(p);
}

/*
 * Relios Mirror: starting the receiver program on Windows (see proc.h).
 * Taken from ReliosOBS's plugins/relios-airplay/receiver-process.c, without OBS's helpers.
 */
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
#include <windows.h>
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
						  JOB_OBJECT_LIMIT_ACTIVE_PROCESS;
	limits.BasicLimitInformation.ActiveProcessLimit = 1; /* it may not start other programs */
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

struct proc *proc_start(const char *exe_utf8, const char *args_utf8, bool low_integrity)
{
	struct proc *p = calloc(1, sizeof(*p));
	HANDLE in_read = NULL, out_write = NULL, err_write = NULL, token = NULL;
	LPPROC_THREAD_ATTRIBUTE_LIST attrs = NULL;
	bool attrs_ready = false;
	wchar_t *cmd = NULL;
	PROCESS_INFORMATION pi;
	STARTUPINFOEXW si;
	HANDLE inherit[3];
	SIZE_T attr_size = 0;
	DWORD flags;
	BOOL created;
	bool ok = false;

	memset(&pi, 0, sizeof(pi));
	memset(&si, 0, sizeof(si));
	if (!p)
		return NULL;

	if (!make_pipe(&in_read, &p->in_write, true) || !make_pipe(&p->out_read, &out_write, false) ||
	    !make_pipe(&p->err_read, &err_write, false))
		goto done;

	InitializeProcThreadAttributeList(NULL, 1, 0, &attr_size);
	attrs = calloc(1, attr_size ? attr_size : 1);
	if (!attrs || !InitializeProcThreadAttributeList(attrs, 1, 0, &attr_size)) {
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
	flags = CREATE_SUSPENDED | CREATE_NO_WINDOW | EXTENDED_STARTUPINFO_PRESENT | CREATE_UNICODE_ENVIRONMENT;
	if (low_integrity) {
		token = low_integrity_token();
		if (!token)
			goto done;
		created = CreateProcessAsUserW(token, NULL, cmd, NULL, NULL, TRUE, flags, NULL, NULL, &si.StartupInfo,
					       &pi);
		if (!created)
			log_error("CreateProcessAsUser");
	} else {
		created = CreateProcessW(NULL, cmd, NULL, NULL, TRUE, flags, NULL, NULL, &si.StartupInfo, &pi);
		if (!created)
			log_error("CreateProcess");
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

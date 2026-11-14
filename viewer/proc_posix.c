/*
 * Relios Mirror: starting the receiver program on Linux (for checks; see proc.h). No sandbox here.
 */
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "proc.h"

struct proc {
	pid_t pid;
	int in_write, out_read, err_read;
};

#define MAX_ARGS 16

static void split_args(const char *exe, const char *args, char *copy, char **argv)
{
	int n = 0;
	argv[n++] = (char *)exe;
	for (char *t = strtok(copy, " "); t && n < MAX_ARGS - 1; t = strtok(NULL, " "))
		argv[n++] = t;
	argv[n] = NULL;
	(void)args;
}

bool proc_strict_prepare(const char *exe, const char *key)
{
	(void)exe;
	(void)key;
	return false; /* (Windows only) */
}

bool proc_strict_remove(void)
{
	return true;
}

struct proc *proc_start(const char *exe, const char *args, enum proc_box box)
{
	(void)box;
	int in[2], out[2], err[2];
	if (pipe(in) || pipe(out) || pipe(err))
		return NULL;
	char *copy = strdup(args ? args : "");
	char *argv[MAX_ARGS];
	split_args(exe, args, copy, argv);
	pid_t pid = fork();
	if (pid == 0) {
		dup2(in[0], 0);
		dup2(out[1], 1);
		dup2(err[1], 2);
		close(in[1]);
		close(out[0]);
		close(err[0]);
		execv(exe, argv);
		_exit(127);
	}
	free(copy);
	close(in[0]);
	close(out[1]);
	close(err[1]);
	if (pid < 0) {
		close(in[1]);
		close(out[0]);
		close(err[0]);
		return NULL;
	}
	signal(SIGPIPE, SIG_IGN);
	struct proc *p = calloc(1, sizeof(*p));
	p->pid = pid;
	p->in_write = in[1];
	p->out_read = out[0];
	p->err_read = err[0];
	return p;
}

int proc_run(const char *exe, const char *args, unsigned timeout_ms)
{
	(void)timeout_ms;
	char *copy = strdup(args ? args : "");
	char *argv[MAX_ARGS];
	split_args(exe, args, copy, argv);
	pid_t pid = fork();
	if (pid == 0) {
		execv(exe, argv);
		_exit(127);
	}
	free(copy);
	if (pid < 0)
		return -1;
	int status = 0;
	if (waitpid(pid, &status, 0) < 0)
		return -1;
	return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

bool proc_alive(struct proc *p, unsigned wait_ms, unsigned long *code)
{
	int status = 0;
	unsigned waited = 0;
	for (;;) {
		pid_t r = waitpid(p->pid, &status, WNOHANG);
		if (r == p->pid) {
			if (code)
				*code = WIFEXITED(status) ? (unsigned long)WEXITSTATUS(status) : 1;
			p->pid = 0; /* (reaped: proc_stop has nothing left to wait for) */
			return false;
		}
		if (r < 0 || waited >= wait_ms)
			return true;
		struct timespec ts = {0, 20 * 1000000};
		nanosleep(&ts, NULL);
		waited += 20;
	}
}

size_t proc_read_out(struct proc *p, void *buf, size_t size)
{
	ssize_t n = read(p->out_read, buf, size);
	return n > 0 ? (size_t)n : 0;
}

size_t proc_read_err(struct proc *p, void *buf, size_t size)
{
	ssize_t n = read(p->err_read, buf, size);
	return n > 0 ? (size_t)n : 0;
}

bool proc_write(struct proc *p, const char *text)
{
	size_t len = strlen(text);
	if (!p || p->in_write < 0)
		return false;
	return write(p->in_write, text, len) == (ssize_t)len;
}

void proc_stop(struct proc *p, unsigned wait_ms)
{
	if (!p)
		return;
	if (p->in_write >= 0) {
		close(p->in_write);
		p->in_write = -1;
	}
	if (p->pid > 0) {
		int status;
		unsigned waited = 0;
		while (waitpid(p->pid, &status, WNOHANG) == 0) {
			if (waited >= wait_ms) {
				kill(p->pid, SIGKILL);
				waitpid(p->pid, &status, 0);
				break;
			}
			struct timespec ts = {0, 20 * 1000000};
			nanosleep(&ts, NULL);
			waited += 20;
		}
		p->pid = 0;
	}
}

void proc_destroy(struct proc *p)
{
	if (!p)
		return;
	if (p->in_write >= 0)
		close(p->in_write);
	close(p->out_read);
	close(p->err_read);
	free(p);
}

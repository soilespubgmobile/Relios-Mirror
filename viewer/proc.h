/*
 * Relios Mirror: starting the receiver program (relios-airplay-receiver) and talking to it through pipes.
 * On Windows the receiver runs at low integrity, in a job that ends with this program (as in ReliosOBS); with
 * "Sıkı Koruma" (strict protection) also in an AppContainer (see proc_win.c). All text is UTF-8.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>

struct proc;

/* defined in main.c: one line to the log file */
void app_log(const char *fmt, ...);

/* How the receiver is kept apart from the rest of the computer (Windows; elsewhere both start it plainly) */
enum proc_box {
	PROC_BOX_LOW,    /* low integrity, in a job: it cannot change the user's files, but it can read them */
	PROC_BOX_STRICT, /* also in an AppContainer: it sees none of the user's files and uses only private networks */
};

/* Starts "<exe> <args>" with pipes for its standard input, output and error. NULL on failure. PROC_BOX_STRICT
 * needs proc_strict_prepare first. */
struct proc *proc_start(const char *exe_utf8, const char *args_utf8, enum proc_box box);

/* Strict box (Windows): makes its AppContainer profile and lets it read exactly what the receiver needs: its
 * program file, the current folder (passing through only, as its current folder) and the key file named
 * (relative to the current folder). What was given is noted in a file in the current folder first, so that
 * proc_strict_remove can take all of it back. false: something failed (logged); start with PROC_BOX_LOW then. */
bool proc_strict_prepare(const char *exe_utf8, const char *key_utf8);

/* Takes back everything proc_strict_prepare gave (every file and folder in the note) and deletes the profile
 * (its folder and registry entries). Only while no receiver runs. true: nothing of it is left. */
bool proc_strict_remove(void);

/* Waits up to wait_ms for the program to end: true if it still runs; false if it ended (its exit code in *code) */
bool proc_alive(struct proc *p, unsigned wait_ms, unsigned long *code);

/* Runs "<exe> <args>" normally and waits up to timeout_ms; its exit code, or -1. */
int proc_run(const char *exe_utf8, const char *args_utf8, unsigned timeout_ms);

/* Blocking reads of its standard output / error; 0 at the end. */
size_t proc_read_out(struct proc *p, void *buf, size_t size);
size_t proc_read_err(struct proc *p, void *buf, size_t size);

/* Writes to its standard input; false if it has gone. */
bool proc_write(struct proc *p, const char *text);

/* Closes its standard input (the receiver then ends), waits up to wait_ms, then ends it if still running. */
void proc_stop(struct proc *p, unsigned wait_ms);

/* After proc_stop and after the reading threads have finished. */
void proc_destroy(struct proc *p);

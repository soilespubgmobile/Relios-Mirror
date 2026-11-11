/*
 * Relios Mirror: starting the receiver program (relios-airplay-receiver) and talking to it through pipes.
 * On Windows the receiver runs at low integrity, in a job that ends with this program (as in ReliosOBS).
 * All text is UTF-8.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>

struct proc;

/* defined in main.c: one line to the log file */
void app_log(const char *fmt, ...);

/* Starts "<exe> <args>" with pipes for its standard input, output and error. NULL on failure. */
struct proc *proc_start(const char *exe_utf8, const char *args_utf8, bool low_integrity);

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

/*
 * What winrun needs from the system it runs on: fluxOS (os_flux.c), or a
 * Linux host for the tests (os_linux.c). Everything else is plain C and
 * POSIX calls both have.
 */
#ifndef WIN32_OS_H
#define WIN32_OS_H

#include <stdint.h>
#include <time.h>

/* Before anything else: the guest window and the segment selectors (gate_*). */
void os_init(void);
/* Where the loader's own memory ends (linear), so the guest gets nothing below. */
uint32_t os_loader_end(void);
/* [linear, linear + size) becomes zeroed memory the program can read, write
 * and run (fixed address; whatever was there is gone). 0 on success. */
int os_map(uint32_t linear, uint32_t size);
int os_unmap(uint32_t linear, uint32_t size);

uint32_t os_ticks_ms(void);             /* since boot */
uint64_t os_time_us(void);              /* since 1970 */
int os_tz_minutes(void);                /* local time, east of UTC */
void os_localtime(time_t t, struct tm *out);

/* The terminal: raw (no echo, bytes as typed) or cooked. Returns the old
 * setting, or -1 if fd is not a terminal. */
int os_console_raw(int fd, int raw);
int os_readable(int fd, int timeout_ms);    /* 1 if a read would not block */
int os_nonblock(int fd, int on);

/* Faults in guest code arrive at seh_fault (win32.h). */
void os_install_fault_handlers(void);

/* Run a program (host path, argv), with these standard descriptors (or -1
 * to inherit), in dir (or NULL). Returns the pid, or -1. */
int os_spawn(const char *path, char *const argv[], char *const envp[], const char *dir, const int fds[3]);
int os_wait(int pid, int *exit_code, int block);     /* 1 if it ended */
const char *os_self(void);              /* how to run another Windows program: winrun's own path */
int os_kill(int pid);

uint32_t os_mem_total_kb(void);
uint32_t os_mem_free_kb(void);
int os_ncpus(void);
void os_beep(void);
const char *os_hostname(void);

#endif

#ifndef _UNISTD_H
#define _UNISTD_H

#include <sys/types.h>
#include <flux/abi.h>

/* What fluxOS claims: POSIX.1-2001 as far as one user, processes without
 * threads and a FAT root allow. */
#define _POSIX_VERSION  200112L
#define _POSIX2_VERSION 200112L
#define _XOPEN_VERSION  600
#define _POSIX_JOB_CONTROL 1
#define _POSIX_SAVED_IDS 1
#define _POSIX_NO_TRUNC 1
#define _POSIX_VDISABLE '\0'
#define _POSIX_MAPPED_FILES 200112L
#define _POSIX_MONOTONIC_CLOCK 200112L
#define _POSIX_TIMERS 200112L

#define STDIN_FILENO  0
#define STDOUT_FILENO 1
#define STDERR_FILENO 2

#define F_OK 0
#define X_OK 1
#define W_OK 2
#define R_OK 4

/* sysconf(3) */
#define _SC_ARG_MAX           0
#define _SC_CHILD_MAX         1
#define _SC_CLK_TCK           2
#define _SC_NGROUPS_MAX       3
#define _SC_OPEN_MAX          4
#define _SC_STREAM_MAX        5
#define _SC_TZNAME_MAX        6
#define _SC_JOB_CONTROL       7
#define _SC_SAVED_IDS         8
#define _SC_VERSION           29
#define _SC_PAGESIZE          30
#define _SC_PAGE_SIZE         _SC_PAGESIZE
#define _SC_LINE_MAX          43
#define _SC_HOST_NAME_MAX     180
#define _SC_LOGIN_NAME_MAX    71
#define _SC_TTY_NAME_MAX      72
#define _SC_SYMLOOP_MAX       173
#define _SC_NPROCESSORS_CONF  83
#define _SC_NPROCESSORS_ONLN  84
#define _SC_PHYS_PAGES        85
#define _SC_AVPHYS_PAGES      86
#define _SC_GETPW_R_SIZE_MAX  70
#define _SC_GETGR_R_SIZE_MAX  69
#define _SC_MONOTONIC_CLOCK   149

/* pathconf(3) */
#define _PC_LINK_MAX          0
#define _PC_MAX_CANON         1
#define _PC_MAX_INPUT         2
#define _PC_NAME_MAX          3
#define _PC_PATH_MAX          4
#define _PC_PIPE_BUF          5
#define _PC_CHOWN_RESTRICTED  6
#define _PC_NO_TRUNC          7
#define _PC_VDISABLE          8

extern char **environ;

pid_t fork(void);
pid_t vfork(void);
int execve(const char *path, char *const argv[], char *const envp[]);
int execv(const char *path, char *const argv[]);
int execvp(const char *file, char *const argv[]);
int execvpe(const char *file, char *const argv[], char *const envp[]);
int execl(const char *path, const char *arg, ...);
int execlp(const char *file, const char *arg, ...);
int execle(const char *path, const char *arg, ...);
void _exit(int status) __attribute__((noreturn));

ssize_t read(int fd, void *buf, size_t n);
ssize_t write(int fd, const void *buf, size_t n);
ssize_t pread(int fd, void *buf, size_t n, off_t off);
ssize_t pwrite(int fd, const void *buf, size_t n, off_t off);
int close(int fd);
void sync(void);
int fsync(int fd);
int fdatasync(int fd);
int ftruncate(int fd, off_t size);
int truncate(const char *path, off_t size);
int dup(int fd);
int dup2(int oldfd, int newfd);
int dup3(int oldfd, int newfd, int flags);
int pipe(int fds[2]);
int pipe2(int fds[2], int flags);
off_t lseek(int fd, off_t off, int whence);
off64_t lseek64(int fd, off64_t off, int whence);
int unlink(const char *path);
int rmdir(const char *path);
int link(const char *from, const char *to);
int symlink(const char *target, const char *path);
ssize_t readlink(const char *path, char *buf, size_t size);
int chdir(const char *path);
int fchdir(int fd);
char *getcwd(char *buf, size_t size);
int access(const char *path, int mode);
int isatty(int fd);
char *ttyname(int fd);
int ttyname_r(int fd, char *buf, size_t size);

pid_t getpid(void);
pid_t gettid(void);                     /* the calling thread's id */
pid_t getppid(void);
uid_t getuid(void);
uid_t geteuid(void);
gid_t getgid(void);
gid_t getegid(void);
int setuid(uid_t uid);
int setgid(gid_t gid);
int seteuid(uid_t uid);
int setegid(gid_t gid);
int setreuid(uid_t ruid, uid_t euid);
int setregid(gid_t rgid, gid_t egid);
int getgroups(int size, gid_t list[]);
char *getlogin(void);
int getlogin_r(char *buf, size_t size);
int chown(const char *path, uid_t owner, gid_t group);
int fchown(int fd, uid_t owner, gid_t group);
int lchown(const char *path, uid_t owner, gid_t group);
int setpgid(pid_t pid, pid_t pgid);
pid_t getpgid(pid_t pid);
pid_t getpgrp(void);
pid_t setsid(void);
pid_t getsid(pid_t pid);
pid_t tcgetpgrp(int fd);
int tcsetpgrp(int fd, pid_t pgid);

long sysconf(int name);
long pathconf(const char *path, int name);
long fpathconf(int fd, int name);
int getpagesize(void);
int gethostname(char *name, size_t size);
int sethostname(const char *name, size_t size);
int nice(int inc);

void *sbrk(int incr);
int brk(void *addr);
unsigned int sleep(unsigned int seconds);
int usleep(unsigned int usec);
unsigned int alarm(unsigned int seconds);
int pause(void);

/* getopt(3): POSIX puts it here; <getopt.h> adds getopt_long. */
extern char *optarg;
extern int optind, opterr, optopt;
int getopt(int argc, char *const argv[], const char *optstring);

#endif

#ifndef _UNISTD_H
#define _UNISTD_H

#include <sys/types.h>
#include <flux/abi.h>

#define STDIN_FILENO  0
#define STDOUT_FILENO 1
#define STDERR_FILENO 2

#define F_OK 0
#define X_OK 1
#define W_OK 2
#define R_OK 4

extern char **environ;

pid_t fork(void);
int execve(const char *path, char *const argv[], char *const envp[]);
int execv(const char *path, char *const argv[]);
int execvp(const char *file, char *const argv[]);
void _exit(int status) __attribute__((noreturn));

ssize_t read(int fd, void *buf, size_t n);
ssize_t write(int fd, const void *buf, size_t n);
int close(int fd);
void sync(void);
int fsync(int fd);
int ftruncate(int fd, off_t size);
int dup(int fd);
int dup2(int oldfd, int newfd);
int pipe(int fds[2]);
off_t lseek(int fd, off_t off, int whence);
off64_t lseek64(int fd, off64_t off, int whence);
int unlink(const char *path);
int rmdir(const char *path);
int chdir(const char *path);
char *getcwd(char *buf, size_t size);
int access(const char *path, int mode);
int isatty(int fd);

pid_t getpid(void);
pid_t getppid(void);
int setpgid(pid_t pid, pid_t pgid);
pid_t getpgid(pid_t pid);
pid_t getpgrp(void);
pid_t tcgetpgrp(int fd);
int tcsetpgrp(int fd, pid_t pgid);

void *sbrk(int incr);
unsigned int sleep(unsigned int seconds);
int usleep(unsigned int usec);

#endif

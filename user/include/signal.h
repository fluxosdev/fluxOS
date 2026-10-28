#ifndef _SIGNAL_H
#define _SIGNAL_H

#include <sys/types.h>
#include <flux/abi.h>

typedef int sig_atomic_t;
typedef uint32_t sigset_t;
typedef void (*sighandler_t)(int);

typedef struct {
    int32_t  si_signo;
    int32_t  si_errno;
    int32_t  si_code;
    int32_t  si_pid;
    uint32_t si_uid;
    int32_t  si_status;
    uint32_t si_addr;
} siginfo_t;

#define SIG_DFL ((sighandler_t)0)
#define SIG_IGN ((sighandler_t)1)
#define SIG_ERR ((sighandler_t)-1)

struct sigaction {
    union {
        sighandler_t sa_handler;
        void (*sa_sigaction)(int, siginfo_t *, void *);
    };
    uint32_t sa_flags;
    sigset_t sa_mask;
    void (*sa_restorer)(void);
};

sighandler_t signal(int sig, sighandler_t handler);
int sigaction(int sig, const struct sigaction *act, struct sigaction *oldact);
int sigprocmask(int how, const sigset_t *set, sigset_t *oldset);
int sigemptyset(sigset_t *set);
int sigfillset(sigset_t *set);
int sigaddset(sigset_t *set, int sig);
int sigdelset(sigset_t *set, int sig);
int sigismember(const sigset_t *set, int sig);
#define SIGRTMIN 32
#define SIGRTMAX 31                     /* none */
#define NSIG_MAX NSIG
#define SA_ONESHOT SA_RESETHAND
#define SA_NOMASK  SA_NODEFER
#define SA_INTERRUPT 0

typedef struct {
    void *ss_sp;
    int ss_flags;
    size_t ss_size;
} stack_t;
#define SS_ONSTACK 1
#define SS_DISABLE 2
#define MINSIGSTKSZ 2048
#define SIGSTKSZ 8192

extern const char *const sys_siglist[NSIG];

int kill(pid_t pid, int sig);
int killpg(pid_t pgrp, int sig);
int raise(int sig);
const char *strsignal(int sig);
void psignal(int sig, const char *msg);
int sigsuspend(const sigset_t *mask);
int sigpending(sigset_t *set);
int sigaltstack(const stack_t *ss, stack_t *old);
int siginterrupt(int sig, int flag);
sighandler_t bsd_signal(int sig, sighandler_t handler);

#endif

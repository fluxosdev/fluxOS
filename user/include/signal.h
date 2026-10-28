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
int kill(pid_t pid, int sig);
int raise(int sig);
const char *strsignal(int sig);

#endif

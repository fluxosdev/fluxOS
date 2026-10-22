#ifndef _SIGNAL_H
#define _SIGNAL_H

#include <sys/types.h>
#include <flux/abi.h>

typedef void (*sighandler_t)(int);

/* Only the default action and "ignore" are supported. */
#define SIG_DFL ((sighandler_t)0)
#define SIG_IGN ((sighandler_t)1)
#define SIG_ERR ((sighandler_t)-1)

sighandler_t signal(int sig, sighandler_t handler);
int kill(pid_t pid, int sig);
int raise(int sig);
const char *strsignal(int sig);

#endif

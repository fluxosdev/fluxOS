/* Native POSIX declarations plus the desktop ABI's independent poll record. */
#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#undef NSIG
#undef WIFEXITED
#undef WEXITSTATUS
#undef WIFSIGNALED
#undef WTERMSIG
#undef POLLIN
#undef POLLOUT
#undef POLLERR
#undef POLLHUP
#undef POLLNVAL
#define pollfd flux_pollfd
#include "desktop.h"
#undef pollfd

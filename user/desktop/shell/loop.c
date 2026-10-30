/*
 * The descriptors a desktop process's loop waits on besides its own (an
 * installer's pipe, a page's requests, the window socket), the same in
 * the desktop (main.c) and in its programs (client.c).
 */
#include <flux.h>
#include <poll.h>
#include "desktop.h"

#define MAX_WATCHES 64

static struct watch {
    int fd, events;
    void (*ready)(void *ctx, int revents);
    void *ctx;
} watches[MAX_WATCHES], snapshot[MAX_WATCHES];
static int nwatches, nsnapshot;

unsigned int now_ms(void)
{
    return uptime_ms();
}

int loop_watch(int fd, void (*ready)(void *ctx, int revents), void *ctx)
{
    if (nwatches == MAX_WATCHES)
        return -1;
    watches[nwatches].fd = fd;
    watches[nwatches].events = POLLIN;
    watches[nwatches].ready = ready;
    watches[nwatches].ctx = ctx;
    nwatches++;
    return 0;
}

void loop_unwatch(int fd)
{
    for (int i = 0; i < nwatches; i++) {
        if (watches[i].fd == fd) {
            watches[i] = watches[--nwatches];
            return;
        }
    }
}

void loop_watch_events(int fd, int events)
{
    for (int i = 0; i < nwatches; i++)
        if (watches[i].fd == fd)
            watches[i].events = events;
}

int loop_poll_fds(struct pollfd *fds, int max)
{
    nsnapshot = nwatches < max ? nwatches : max;
    for (int i = 0; i < nsnapshot; i++) {
        snapshot[i] = watches[i];
        fds[i].fd = watches[i].fd;
        fds[i].events = (short)watches[i].events;
        fds[i].revents = 0;
    }
    return nsnapshot;
}

static int watching(const struct watch *w)
{
    for (int i = 0; i < nwatches; i++)
        if (watches[i].fd == w->fd && watches[i].ready == w->ready && watches[i].ctx == w->ctx)
            return 1;
    return 0;
}

void loop_dispatch(const struct pollfd *fds)
{
    /* A callback may drop its own watch or another; check each is still wanted. */
    for (int i = 0; i < nsnapshot; i++)
        if (fds[i].revents && watching(&snapshot[i]))
            snapshot[i].ready(snapshot[i].ctx, fds[i].revents);
}

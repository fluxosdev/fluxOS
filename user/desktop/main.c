/*
 * desktop: the fluxOS graphical desktop. init starts it at boot when
 * the boot loader set up a framebuffer; it can also be started from a
 * console shell. Exiting it (Shut Down... > exit to the console) gives
 * the screen back to the text console.
 *
 * One loop polls the input device and every terminal's pty, handles
 * what is ready, runs the timers and repaints what changed.
 */
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <sys/wait.h>
#include <unistd.h>
#include "desktop.h"

#define MAX_TERMS   32
#define MAX_WATCHES 16
#define BLINK_MS    500

/* Other descriptors the loop waits on, such as the installer's pipe. */
static struct watch {
    int fd;
    void (*ready)(void *ctx, int revents);
    void *ctx;
} watches[MAX_WATCHES];
static int nwatches;

int loop_watch(int fd, void (*ready)(void *ctx, int revents), void *ctx)
{
    if (nwatches == MAX_WATCHES)
        return -1;
    watches[nwatches].fd = fd;
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

static int watching(const struct watch *w)
{
    for (int i = 0; i < nwatches; i++)
        if (watches[i].fd == w->fd && watches[i].ready == w->ready && watches[i].ctx == w->ctx)
            return 1;
    return 0;
}

int main(void)
{
    signal(SIGINT, SIG_IGN);
    signal(SIGQUIT, SIG_IGN);

    int input = open("/dev/input", O_RDONLY);
    if (input < 0) {
        perror("desktop: /dev/input");
        return 1;
    }
    wm_init();
    terminal_open("Terminal", NULL, 0, 1);
    wm_flush();

    unsigned int last_blink = now_ms(), last_tick = last_blink;
    for (;;) {
        struct pollfd fds[1 + MAX_TERMS + MAX_WATCHES];
        struct window *owners[1 + MAX_TERMS];
        struct watch w[MAX_WATCHES];
        fds[0].fd = input;
        fds[0].events = POLLIN;
        fds[0].revents = 0;
        int n = 1 + terminal_poll_fds(fds + 1, owners + 1, MAX_TERMS);
        int nw = nwatches;
        for (int i = 0; i < nw; i++) {
            w[i] = watches[i];
            fds[n + i].fd = w[i].fd;
            fds[n + i].events = POLLIN;
            fds[n + i].revents = 0;
        }

        unsigned int since = now_ms() - last_blink;
        int timeout = since >= BLINK_MS ? 0 : (int)(BLINK_MS - since);
        int frame = gears_timeout();            /* animation wants an earlier wakeup */
        if (frame >= 0 && frame < timeout)
            timeout = frame;
        poll(fds, (unsigned int)(n + nw), timeout);

        /* Terminals first: input handling may close their windows. */
        for (int i = 1; i < n; i++)
            if (fds[i].revents)
                terminal_ready(owners[i], fds[i].revents);

        /* A callback may drop its own watch or another; check each is still wanted. */
        for (int i = 0; i < nw; i++)
            if (fds[n + i].revents && watching(&w[i]))
                w[i].ready(w[i].ctx, fds[n + i].revents);

        if (fds[0].revents & POLLIN) {
            struct input_event ev[64];
            int r = (int)read(input, ev, sizeof(ev));
            for (int i = 0; i < r / (int)sizeof(ev[0]); i++)
                wm_input(&ev[i]);
        }

        int pid, status;
        while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
            terminal_child_exited(pid);
            setup_child_exited(pid, status);
            browser_child_exited(pid, status);
        }

        gears_frame();
        unsigned int now = now_ms();
        if (now - last_blink >= BLINK_MS) {
            last_blink = now;
            terminal_blink();
            editor_blink();
            browser_blink();
        }
        if (now - last_tick >= 1000) {
            last_tick = now;
            wm_tick();
            about_tick();
            display_tick();
            browser_tick();
        }
        wm_flush();
    }
}

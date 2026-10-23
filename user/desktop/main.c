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

#define MAX_TERMS 32
#define BLINK_MS  500

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
        struct pollfd fds[1 + MAX_TERMS];
        struct window *owners[1 + MAX_TERMS];
        fds[0].fd = input;
        fds[0].events = POLLIN;
        fds[0].revents = 0;
        int n = 1 + terminal_poll_fds(fds + 1, owners + 1, MAX_TERMS);

        unsigned int since = now_ms() - last_blink;
        poll(fds, (unsigned int)n, since >= BLINK_MS ? 0 : (int)(BLINK_MS - since));

        /* Terminals first: input handling may close their windows. */
        for (int i = 1; i < n; i++)
            if (fds[i].revents)
                terminal_ready(owners[i], fds[i].revents);

        if (fds[0].revents & POLLIN) {
            struct input_event ev[64];
            int r = (int)read(input, ev, sizeof(ev));
            for (int i = 0; i < r / (int)sizeof(ev[0]); i++)
                wm_input(&ev[i]);
        }

        int pid, status;
        while ((pid = waitpid(-1, &status, WNOHANG)) > 0)
            terminal_child_exited(pid);

        unsigned int now = now_ms();
        if (now - last_blink >= BLINK_MS) {
            last_blink = now;
            terminal_blink();
        }
        if (now - last_tick >= 1000) {
            last_tick = now;
            wm_tick();
            about_tick();
        }
        wm_flush();
    }
}

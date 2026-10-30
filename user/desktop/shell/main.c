/*
 * desktop: the fluxOS graphical desktop. init starts it at boot when
 * the boot loader set up a framebuffer; it can also be started from a
 * console shell. Exiting it (Shut Down... > exit to the console) gives
 * the screen back to the text console.
 *
 * It is the window manager, the taskbar, its folder on the wallpaper and
 * its own dialogs; every program is a process of its own that connects
 * to it for its windows (server.c), the terminal it opens at the start
 * too.
 *
 * "desktop --setup" is what the install CD starts instead: fluxOS Setup
 * on a screen of its own, with no taskbar or desktop folder.
 *
 * One loop polls the input device, the programs' sockets and whatever
 * else is watched, handles what is ready, runs the timers and repaints
 * what changed.
 */
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>
#include "desktop.h"
#include "imagejobs.h"

#define MAX_WATCHES 64
#define TICK_MS     1000

int main(int argc, char **argv)
{
    signal(SIGINT, SIG_IGN);
    signal(SIGQUIT, SIG_IGN);
    signal(SIGPIPE, SIG_IGN);
    image_workers_init();              /* fork before windows and page heaps exist */
    int setup = argc > 1 && !strcmp(argv[1], "--setup");

    int input = open("/dev/input", O_RDONLY);
    if (input < 0) {
        perror("desktop: /dev/input");
        return 1;
    }
    wm_init(setup);
    mouse_init();
    datetime_init();
    sound_init();
    power_init();
    wireless_init();
    if (server_init() < 0)
        fprintf(stderr, "desktop: programs cannot open windows\n");
    if (setup) {
        setup_open();
        wm_flush();
    } else {
        saver_init();
        files_open_desktop();           /* the desktop's folder, under every window */
        launch1("terminal", "-m");
        wm_flush();
        sound_startup();                /* the desktop has replaced the boot screen */
        wallpaper_init();               /* after the first frame: decoding takes a moment */
        wm_flush();
    }

    unsigned int last_tick = now_ms();
    for (;;) {
        struct pollfd fds[1 + MAX_WATCHES];
        fds[0].fd = input;
        fds[0].events = POLLIN;
        fds[0].revents = 0;
        int nw = loop_poll_fds(fds + 1, MAX_WATCHES);

        unsigned int since = now_ms() - last_tick;
        int timeout = since >= TICK_MS ? 0 : (int)(TICK_MS - since);
        int images = image_jobs_timeout();
        if (images >= 0 && images < timeout)
            timeout = images;
        int sv = saver_timeout();               /* the screen saver starting, or moving */
        if (sv >= 0 && sv < timeout)
            timeout = sv;
        poll(fds, (unsigned int)(1 + nw), timeout);

        /* Input first, before background I/O callbacks (the programs' messages among them). */
        if (fds[0].revents & POLLIN) {
            struct input_event ev[64];
            int r = (int)read(input, ev, sizeof(ev));
            for (int i = 0; i < r / (int)sizeof(ev[0]); i++)
                wm_input(&ev[i]);
        }
        loop_dispatch(fds + 1);

        int pid, status;
        while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
            image_worker_child_exited(pid);
            setup_child_exited(pid, status);
        }

        saver_frame();
        image_jobs_poll();
        unsigned int now = now_ms();
        if (now - last_tick >= TICK_MS) {
            last_tick = now;
            wm_tick();
            clock_tick();
            about_tick();
            sound_tick();
            power_tick();
            wireless_tray_tick();
            files_tick();
            setup_tick();
        }
        server_sync();
        wm_flush();
    }
}

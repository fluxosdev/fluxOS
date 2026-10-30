/* Exercise the desktop's real pointer conversion without a display or VM. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#undef WIFEXITED
#undef WEXITSTATUS
#undef WIFSIGNALED
#undef WTERMSIG
#undef WIFSTOPPED
#undef WSTOPSIG
#undef WIFCONTINUED
#include "../user/desktop/shell/mousecfg.c"
#include "../user/desktop/apps/mouse.c"

void mouse_config_changed(void) {}     /* the desktop's to hear: there is none here */

static void move(int absolute, int px, int py, int *x, int *y)
{
    struct input_event ev = {
        .type = IEV_MOUSE, .flags = absolute ? IEV_ABS : 0,
        .x = px, .y = py,
    };
    mouse_move_pointer(&ev, 1024, 1024, x, y);
}

int main(void)
{
    int x = 0, y = 0;
    struct mouse_config cfg = mouse_saved_cfg;
    move(1, 16000, 16000, &x, &y);
    assert(x == 500 && y == 500);

    cfg.speed = 8;
    sync_preview(&cfg);
    move(1, 16320, 15680, &x, &y);
    assert(x == 530 && y == 470);  /* absolute motion now uses 3x speed */
    move(1, 16320, 15680, &x, &y);
    assert(x == 530 && y == 470);  /* button/wheel-only packets do not drift */

    cfg.speed = 0;
    sync_preview(&cfg);
    for (int i = 1; i <= 4; i++)
        move(1, 16320 + i * 32, 15680 - i * 32, &x, &y);
    assert(x == 531 && y == 469);  /* retain fractional motion at 0.25x */

    mouse_active_cfg = mouse_saved_cfg; /* Cancel restores host alignment */
    move(1, 16000, 16000, &x, &y);
    assert(x == 500 && y == 500);
    move(0, 10, -10, &x, &y);
    assert(x == 510 && y == 490);
    cfg.speed = 8;
    sync_preview(&cfg);
    move(0, 10, -10, &x, &y);
    assert(x == 540 && y == 460);
    move(1, 16000, 16000, &x, &y);
    assert(x == 500 && y == 500);  /* re-anchor after a device change */

    move(1, 32767, 0, &x, &y);
    assert(x == 1023 && y == 0);
    move(1, 32735, 32, &x, &y);
    assert(x == 1020 && y == 3);  /* immediately move away from an edge */

    struct input_event ev = { .type = IEV_MOUSE, .flags = IEV_ABS,
                              .x = 16000, .y = 16000 };
    mouse_move_pointer(&ev, 800, 600, &x, &y);
    assert(x == 390 && y == 292); /* resolution changes re-anchor */

    cfg.speed = 0;
    sync_preview(&cfg);
    x = y = 100;
    for (int i = 0; i < 4; i++)
        move(0, -1, 1, &x, &y);
    assert(x == 99 && y == 101);
    puts("DESKTOP MOUSE REGRESSIONS PASSED");
    return 0;
}

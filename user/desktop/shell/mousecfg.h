/* The mouse's settings (mousecfg.c), as Mouse Properties (mouse.c) works on them. */
#ifndef MOUSECFG_H
#define MOUSECFG_H

#define SPEED_STEPS    9
#define DBLCLICK_STEPS 9
#define WHEEL_MAX      9

struct mouse_config {
    int speed;              /* 0 .. SPEED_STEPS - 1 */
    int dblclick;           /* 0 .. DBLCLICK_STEPS - 1 */
    int left_handed;        /* 0: right-handed, 1: left-handed */
    int wheel_lines;        /* 1 .. WHEEL_MAX */
    int wheel_reverse;      /* 0: normal, 1: natural / reverse */
};

extern const int speed_mult[SPEED_STEPS];
extern const unsigned int dblclick_times[DBLCLICK_STEPS];
extern struct mouse_config mouse_saved_cfg, mouse_active_cfg;   /* kept, and in use (previewed) */

int mouse_clamp(int v, int lo, int hi);
void mouse_save(const struct mouse_config *c);     /* for the next start */

#endif

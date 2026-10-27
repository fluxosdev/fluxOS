/*
 * Touchpads on the PS/2 auxiliary port (mouse.c hands over the bytes).
 *
 * A laptop's touchpad pretends to be a PS/2 mouse until told otherwise,
 * doing its own taps, but nothing more. A Synaptics pad (most laptops of
 * the 2000s) is switched to absolute mode instead: it reports where each
 * finger is, how hard it presses and how wide it is (W), 80 times a
 * second, and the gestures are made here:
 *
 *   one finger moves the pointer, faster the faster it moves;
 *   a tap clicks: one finger the left button, two the right, three the
 *     middle; tap and touch again to drag;
 *   two fingers scroll, or, on pads that cannot count fingers, one along
 *     the right edge;
 *   a clickpad's (the pad is the button) bottom right corner is the right
 *     button;
 *   no taps just after a key (a palm brushing the pad while typing), and
 *     nothing from a touch as wide as a palm.
 *
 * A TrackPoint behind a Synaptics pad (ThinkPads) speaks through it: its
 * packets come wrapped in the pad's (W = 3). Its middle button scrolls
 * while held, and clicks when only pressed.
 *
 * ALPS and Elantech pads are recognised, for the log, and left as the
 * mice they emulate (tapping still works; scrolling does not).
 *
 * "touchpad=off" on the command line skips all this (the pad stays a
 * mouse), "touchpad.tap=0" turns tapping off.
 */
#include "kernel.h"
#include "drivers.h"
#include "ps2.h"

/* Synaptics queries (sent as four "set resolution" commands, then read
 * with "status request") */
#define SYN_IDENTIFY   0x00
#define SYN_CAPS       0x02
#define SYN_MODEL      0x03
#define SYN_RESOLUTION 0x08
#define SYN_EXT_CAPS   0x0C
#define SYN_MAX_COORDS 0x0D
#define SYN_MIN_COORDS 0x0F

#define MODE_ABSOLUTE 0x80
#define MODE_RATE80   0x40
#define MODE_NO_GEST  0x04
#define MODE_W        0x01

#define CAP_EXTENDED(c)     ((c) & (1u << 23))      /* W mode, among others */
#define CAP_MIDDLE(c)       ((c) & (1u << 18))
#define CAP_EXT_QUERIES(c)  (((c) >> 20) & 7)
#define CAP_PASSTHROUGH(c)  ((c) & (1u << 7))
#define CAP_FOUR_BUTTON(c)  ((c) & (1u << 3))
#define CAP_MULTIFINGER(c)  ((c) & (1u << 1))
#define CAP_PALMDETECT(c)   ((c) & (1u << 0))
#define EXT_CLICKPAD(e)     ((e) & (1u << 20) || (e) & (1u << 8))
#define EXT_MAX_COORDS(e)   ((e) & (1u << 17))
#define EXT_MIN_COORDS(e)   ((e) & (1u << 13))
#define MODEL_NEWABS(m)     ((m) & (1u << 17))

/* The gestures, in the pad's units: about 55 to the millimetre on most. */
#define Z_TOUCH      30                 /* pressure that is a finger */
#define Z_LIFT       25
#define W_PALM       13                 /* a touch at least this wide is a palm */
#define TAP_TICKS    (HZ / 5)           /* a tap is shorter than this */
#define TAP_MOVE     150                /* and moves less than this */
#define DRAG_TICKS   (HZ / 5)           /* touch again this soon after a tap to drag */
#define TYPING_TICKS (3 * HZ / 10)      /* no taps this soon after a key */
#define SCROLL_STEP  160                /* finger travel per wheel step */
#define STICK_STEP   6                  /* TrackPoint travel per wheel step */

static struct {
    char name[64];
    int synaptics;                      /* driven in absolute mode */
    uint32_t caps, ext, model;
    int xmin, xmax, ymin, ymax;
    int xres;                           /* units per millimetre */
    int clickpad, passthrough;
    int notap;

    uint8_t pkt[6];
    int npkt;

    /* the gesture */
    int touching, fingers, max_fingers;
    uint32_t touch_tick;
    int start_x, start_y, last_x, last_y;
    int moved, skip, palm;
    int edge_scroll;
    int rem_x, rem_y, scroll;           /* what has not made a whole count yet */
    int pad_buttons, buttons_seen;      /* the pad's own buttons, and any during this touch */
    int tap_button;                     /* the button a tap left held, for a drag */
    uint32_t tap_tick;
    int dragging;
    int last_up_down;                   /* four-button pads: their up and down buttons */

    /* the TrackPoint */
    uint8_t stick_buttons;
    int stick_scroll, stick_scrolled;
} tp;

static inline int iabs(int v)
{
    return v < 0 ? -v : v;
}

static int buttons(void)
{
    return tp.pad_buttons | (tp.stick_buttons & (MOUSE_LEFT | MOUSE_RIGHT)) | tp.tap_button;
}

static void report(int dx, int dy, int wheel)
{
    struct input_event ev = { .type = IEV_MOUSE };
    ev.mods = (uint8_t)buttons();
    ev.x = (int16_t)dx;
    ev.y = (int16_t)dy;
    ev.code = (uint16_t)(int16_t)wheel;
    input_push(&ev);
}

/* A click: down, then up. */
static void click(int button)
{
    tp.tap_button = button;
    report(0, 0, 0);
    tp.tap_button = 0;
    report(0, 0, 0);
}

/* ---- the Synaptics protocol ---- */

/* A byte as four "set resolution" arguments, two bits each, after a
 * "set scaling 1:1" that starts the sequence. */
static int special(uint8_t arg)
{
    if (ps2_aux_cmd(0xE6) < 0)
        return -1;
    for (int shift = 6; shift >= 0; shift -= 2)
        if (ps2_aux_cmd(0xE8) < 0 || ps2_aux_cmd((uint8_t)((arg >> shift) & 3)) < 0)
            return -1;
    return 0;
}

static int syn_query(uint8_t q, uint8_t r[3])
{
    return special(q) < 0 ? -1 : ps2_aux_query(0xE9, r, 3);
}

/* The mode byte goes as a "set sample rate 20" after the special sequence. */
static int syn_mode(uint8_t mode)
{
    if (special(mode) < 0 || ps2_aux_cmd(0xF3) < 0 || ps2_aux_cmd(0x14) < 0)
        return -1;
    return 0;
}

/* A command for the device behind the pad: "set sample rate 40". */
static void guest_cmd(uint8_t cmd)
{
    if (special(cmd) == 0) {
        ps2_aux_cmd(0xF3);
        ps2_aux_cmd(0x28);
    }
}

static uint32_t u24(const uint8_t r[3])
{
    return (uint32_t)r[0] << 16 | (uint32_t)r[1] << 8 | r[2];
}

static int syn_setup(void)
{
    uint8_t r[3];
    if (ps2_aux_cmd(0xFF) < 0 || ps2_aux_read(&r[0], 800) < 0 || ps2_aux_read(&r[1], 50) < 0)
        return -1;                      /* reset: AAh, 00h once it has tested itself */
    uint8_t mode = MODE_ABSOLUTE | MODE_RATE80 | MODE_NO_GEST | (CAP_EXTENDED(tp.caps) ? MODE_W : 0);
    if (syn_mode(mode) < 0)
        return -1;
    if (tp.passthrough)
        guest_cmd(0xF4);                /* the TrackPoint streams */
    return ps2_aux_cmd(0xF4);
}

static int synaptics_probe(void)
{
    uint8_t r[3];
    if (syn_query(SYN_IDENTIFY, r) < 0 || r[1] != 0x47)
        return 0;
    int major = r[2] & 0x0F, minor = r[0];
    if (syn_query(SYN_CAPS, r) == 0 && r[1] == 0x47)      /* (0x47 marks it valid) */
        tp.caps = u24(r);
    if (syn_query(SYN_MODEL, r) == 0)
        tp.model = u24(r);
    if (CAP_EXT_QUERIES(tp.caps) >= 4 && syn_query(SYN_EXT_CAPS, r) == 0)
        tp.ext = u24(r);
    tp.xmin = 1472;
    tp.xmax = 5472;
    tp.ymin = 1408;
    tp.ymax = 4448;
    tp.xres = 55;
    if (syn_query(SYN_RESOLUTION, r) == 0 && (r[1] & 0x80) && r[0] >= 20 && r[0] <= 200)
        tp.xres = r[0];
    if (CAP_EXT_QUERIES(tp.caps) >= 5 && EXT_MAX_COORDS(tp.ext) && syn_query(SYN_MAX_COORDS, r) == 0) {
        int x = r[0] << 5 | (r[1] & 0x0F) << 1, y = r[2] << 5 | (r[1] & 0xF0) >> 3;
        if (x > 2000 && y > 2000) {
            tp.xmax = x;
            tp.ymax = y;
        }
    }
    if (CAP_EXT_QUERIES(tp.caps) >= 7 && EXT_MIN_COORDS(tp.ext) && syn_query(SYN_MIN_COORDS, r) == 0) {
        int x = r[0] << 5 | (r[1] & 0x0F) << 1, y = r[2] << 5 | (r[1] & 0xF0) >> 3;
        if (x < tp.xmax / 2 && y < tp.ymax / 2) {
            tp.xmin = x;
            tp.ymin = y;
        }
    }
    tp.clickpad = EXT_CLICKPAD(tp.ext) != 0;
    tp.passthrough = CAP_PASSTHROUGH(tp.caps) != 0;
    snprintf(tp.name, sizeof(tp.name), "Synaptics %d.%d%s%s%s", major, minor,
             CAP_MULTIFINGER(tp.caps) ? ", two-finger scrolling" : ", edge scrolling",
             tp.clickpad ? ", clickpad" : "", tp.passthrough ? ", TrackPoint" : "");
    if (major < 4 || !MODEL_NEWABS(tp.model)) {
        strlcat(tp.name, " (too old: as a mouse)", sizeof(tp.name));
        return 0;
    }
    if (syn_setup() < 0) {
        strlcat(tp.name, " (would not change mode: as a mouse)", sizeof(tp.name));
        ps2_aux_cmd(0xFF);
        uint8_t b;
        ps2_aux_read(&b, 800);
        ps2_aux_read(&b, 50);
        return 0;
    }
    return 1;
}

/* ---- the gestures ---- */

static int fingers_of(int z, int w)
{
    if (z < (tp.touching ? Z_LIFT : Z_TOUCH))
        return 0;
    if (!CAP_EXTENDED(tp.caps) || !CAP_MULTIFINGER(tp.caps))
        return 1;
    return w == 0 ? 2 : w == 1 ? 3 : 1;
}

/* Pad units to screen counts: slow movements finely, fast ones far. */
static int scale(int d, int speed, int *rem)
{
    int lo = 8, hi = 64;                /* units per packet */
    int gain = speed <= lo ? 24 : speed >= hi ? 100 : 24 + (speed - lo) * (100 - 24) / (hi - lo);
    int v = d * gain * 55 / tp.xres + *rem;     /* 1/256ths of a count */
    *rem = v % 256;
    return v / 256;
}

static void touch_ended(void)
{
    uint32_t now = ticks;
    if (tp.dragging) {
        tp.dragging = 0;
        tp.tap_button = 0;
        report(0, 0, 0);
    } else if (!tp.notap && !tp.moved && !tp.buttons_seen && !tp.palm && !tp.edge_scroll &&
               (int32_t)(now - tp.touch_tick) < TAP_TICKS &&
               (int32_t)(now - keyboard_last_tick) >= TYPING_TICKS) {
        int b = tp.max_fingers >= 3 ? MOUSE_MIDDLE : tp.max_fingers == 2 ? MOUSE_RIGHT : MOUSE_LEFT;
        click(b);
        if (b == MOUSE_LEFT)
            tp.tap_tick = now;
    }
    tp.touching = 0;
    tp.fingers = 0;
}

static void touch_started(int x, int y, int fingers)
{
    tp.touching = 1;
    tp.fingers = tp.max_fingers = fingers;
    tp.touch_tick = ticks;
    tp.start_x = tp.last_x = x;
    tp.start_y = tp.last_y = y;
    tp.moved = tp.palm = 0;
    tp.buttons_seen = tp.pad_buttons;
    tp.rem_x = tp.rem_y = tp.scroll = 0;
    /* Without counting fingers, the right edge is the scroll bar. */
    tp.edge_scroll = !CAP_MULTIFINGER(tp.caps) && x > tp.xmax - (tp.xmax - tp.xmin) * 7 / 100;
    /* Touching again just after a tap: the tap's button stays down. */
    if (tp.tap_tick && (int32_t)(ticks - tp.tap_tick) < DRAG_TICKS) {
        tp.dragging = 1;
        tp.tap_button = MOUSE_LEFT;
        report(0, 0, 0);
    }
    tp.tap_tick = 0;
}

static void pad_frame(int x, int y, int z, int w, int pad_buttons)
{
    /* A clickpad's click in its bottom right corner is the right button. */
    if (tp.clickpad && (pad_buttons & MOUSE_LEFT) && !(tp.pad_buttons & MOUSE_LEFT) && z >= Z_TOUCH &&
        x > (tp.xmin + tp.xmax) / 2 && y < tp.ymin + (tp.ymax - tp.ymin) * 18 / 100)
        pad_buttons = (pad_buttons & ~MOUSE_LEFT) | MOUSE_RIGHT;
    else if (tp.clickpad && (pad_buttons & MOUSE_LEFT) && (tp.pad_buttons & MOUSE_RIGHT))
        pad_buttons = (pad_buttons & ~MOUSE_LEFT) | MOUSE_RIGHT;   /* held: still the right one */
    int changed = pad_buttons != tp.pad_buttons;
    tp.pad_buttons = pad_buttons;
    if (pad_buttons)
        tp.buttons_seen = 1;

    int fingers = fingers_of(z, w);
    if (!fingers) {
        if (tp.touching)
            touch_ended();
        if (changed)
            report(0, 0, 0);
        return;
    }
    if (!tp.touching) {
        touch_started(x, y, fingers);
        if (changed)
            report(0, 0, 0);
        return;
    }
    if (fingers > tp.max_fingers)
        tp.max_fingers = fingers;
    if (fingers == 1 && CAP_EXTENDED(tp.caps) && w >= W_PALM)
        tp.palm = 1;
    if (iabs(x - tp.start_x) + iabs(y - tp.start_y) > TAP_MOVE)
        tp.moved = 1;

    int dx = x - tp.last_x, dy = y - tp.last_y;
    tp.last_x = x;
    tp.last_y = y;
    /* Fingers coming or going move the reported point: not a movement.
     * Nor is a press on a clickpad, which shifts the finger. */
    if (fingers != tp.fingers || (tp.clickpad && changed)) {
        tp.fingers = fingers;
        tp.scroll = 0;
        if (changed)
            report(0, 0, 0);
        return;
    }
    if (tp.palm) {
        if (changed)
            report(0, 0, 0);
        return;
    }

    if (fingers >= 2 || tp.edge_scroll) {
        /* Scrolling: fingers down the pad scroll down (the pad's y grows upwards). */
        tp.scroll += dy;
        int steps = tp.scroll / SCROLL_STEP;
        tp.scroll -= steps * SCROLL_STEP;
        if (steps || changed)
            report(0, 0, steps);
        return;
    }
    int speed = MAX(iabs(dx), iabs(dy));
    int cx = scale(dx, speed, &tp.rem_x), cy = -scale(dy, speed, &tp.rem_y);
    if (cx || cy || changed)
        report(cx, cy, 0);
}

/* A packet from the TrackPoint, inside the pad's: a PS/2 mouse packet. */
static void stick_packet(uint8_t flags, uint8_t bx, uint8_t by)
{
    if (flags & 0xC0)
        return;                         /* overflow */
    int dx = bx - ((flags << 4) & 0x100);
    int dy = by - ((flags << 3) & 0x100);
    uint8_t was = tp.stick_buttons;
    tp.stick_buttons = flags & 7;
    if (tp.stick_buttons & MOUSE_MIDDLE) {
        /* Middle held: the stick scrolls. */
        tp.stick_scroll += dy;
        int steps = tp.stick_scroll / STICK_STEP;
        tp.stick_scroll -= steps * STICK_STEP;
        if (steps) {
            tp.stick_scrolled = 1;
            report(0, 0, steps);
        } else if ((was ^ tp.stick_buttons) & (MOUSE_LEFT | MOUSE_RIGHT)) {
            report(0, 0, 0);
        }
        return;
    }
    if (was & MOUSE_MIDDLE) {           /* let go: a click unless it scrolled */
        if (!tp.stick_scrolled)
            click(MOUSE_MIDDLE);
        tp.stick_scrolled = 0;
        tp.stick_scroll = 0;
    }
    if (dx || dy || ((was ^ tp.stick_buttons) & (MOUSE_LEFT | MOUSE_RIGHT)))
        report(dx, -dy, 0);
}

static void syn_packet(const uint8_t *p)
{
    int w = (p[0] & 0x30) >> 2 | (p[0] & 0x04) >> 1 | (p[3] & 0x04) >> 2;
    if (w == 3 && tp.passthrough && CAP_EXTENDED(tp.caps)) {
        stick_packet(p[1], p[4], p[5]);
        return;
    }
    if (w == 2 && CAP_EXTENDED(tp.caps))
        return;                         /* extended W data: not asked for */
    int x = (p[3] & 0x10) << 8 | (p[1] & 0x0F) << 8 | p[4];
    int y = (p[3] & 0x20) << 7 | (p[1] & 0xF0) << 4 | p[5];
    int z = p[2];
    int b = p[0] & 3;                   /* left, right */
    int extra = (p[0] ^ p[3]) & 3;
    if (tp.clickpad) {
        if (extra & 1)
            b |= MOUSE_LEFT;            /* a clickpad's click */
    } else if (CAP_MIDDLE(tp.caps)) {
        if (extra & 1)
            b |= MOUSE_MIDDLE;
    } else if (CAP_FOUR_BUTTON(tp.caps)) {
        /* Up and down buttons: a wheel step as each goes down. */
        int pressed = extra & ~tp.last_up_down;
        tp.last_up_down = extra;
        if (pressed)
            report(0, 0, (pressed & 1) ? 1 : -1);
    }
    pad_frame(x, y, z, w, b);
}

void touchpad_byte(uint8_t b)
{
    if (!tp.synaptics)
        return;
    /* Bytes 1 and 4 have fixed bits: out of step, wait for a first byte. */
    if (tp.npkt == 0 && (b & 0xC8) != 0x80)
        return;
    if (tp.npkt == 3 && (b & 0xC8) != 0xC0) {
        tp.npkt = 0;
        return;
    }
    tp.pkt[tp.npkt++] = b;
    if (tp.npkt == 6) {
        tp.npkt = 0;
        syn_packet(tp.pkt);
    }
}

/* ---- the others, for the log ---- */

static void identify_other(void)
{
    uint8_t r[3];
    if (ps2_aux_cmd(0xE6) < 0 || ps2_aux_cmd(0xE6) < 0 || ps2_aux_cmd(0xE6) < 0 ||
        ps2_aux_query(0xE9, r, 3) < 0)
        return;
    if (r[0] == 0x3C && r[1] == 0x03 && (r[2] == 0xC8 || r[2] == 0x00)) {
        strlcpy(tp.name, "Elantech touchpad (as a mouse)", sizeof(tp.name));
        return;
    }
    if ((r[0] & 0xF8) || r[1] || (r[2] != 10 && r[2] != 100))
        return;
    uint8_t e7[3];
    if (ps2_aux_cmd(0xE7) == 0 && ps2_aux_cmd(0xE7) == 0 && ps2_aux_cmd(0xE7) == 0 &&
        ps2_aux_query(0xE9, e7, 3) == 0 && e7[0] == 0x73)
        snprintf(tp.name, sizeof(tp.name), "ALPS touchpad %02x %02x %02x (as a mouse)", e7[0], e7[1],
                 e7[2]);
}

/* ---- mouse.c's side ---- */

int touchpad_probe(void)
{
    if (ps2_aux_cmd(0xF5) < 0)          /* stop streaming; no answer: nothing there */
        return 0;
    tp.notap = cmdline_has("touchpad.tap=0");
    tp.synaptics = synaptics_probe();
    if (!tp.synaptics && !tp.name[0])
        identify_other();
    if (!tp.synaptics)
        ps2_aux_cmd(0xF6);              /* back to the defaults for mouse.c */
    klog("touchpad: %s\n", tp.name[0] ? tp.name : "none (a plain mouse)");
    return tp.synaptics;
}

/* After suspend to RAM, the pad is a mouse again: absolute mode once more. */
int touchpad_resume(void)
{
    tp.npkt = 0;
    tp.touching = 0;
    tp.pad_buttons = tp.stick_buttons = tp.tap_button = 0;
    tp.dragging = 0;
    if (syn_setup() < 0) {
        klog("touchpad: did not come back after suspend; driving it as a mouse\n");
        tp.synaptics = 0;
        return 0;
    }
    return 1;
}

const char *touchpad_name(void)
{
    return tp.name;
}

/*
 * PS/2 keyboard driver (scancode set 1, US layout). Keys are translated
 * to bytes and VT100/xterm escape sequences and fed into the console tty, or,
 * while the desktop has /dev/input open, reported there as key events
 * that carry the same bytes.
 *
 * A laptop's Fn keys are the keyboard controller's business: volume,
 * mute and the like arrive as the multimedia keys (E0-prefixed codes, to
 * the desktop as KEY_VOLUMEUP and so on), brightness and suspend mostly
 * through ACPI (power.c). The power and sleep keys, with no desktop to
 * hear them, go to ACPI's handling of its buttons. Caps Lock's light
 * follows Caps Lock.
 */
#include "kernel.h"
#include "arch.h"
#include "drivers.h"
#include "tty.h"
#include "x86.h"

#define KEY_BACKSPACE 0x7F

static const char keymap[128] = {
    0, 0x1B, '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-', '=', KEY_BACKSPACE, '\t',
    'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', '[', ']', '\n', 0,
    'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', ';', '\'', '`', 0, '\\',
    'z', 'x', 'c', 'v', 'b', 'n', 'm', ',', '.', '/', 0, '*', 0, ' ', 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0,           /* F1-F10 */
    0, 0,                                   /* num lock, scroll lock */
    '7', '8', '9', '-', '4', '5', '6', '+', '1', '2', '3', '0', '.',
};

static const char keymap_shift[128] = {
    0, 0x1B, '!', '@', '#', '$', '%', '^', '&', '*', '(', ')', '_', '+', KEY_BACKSPACE, '\t',
    'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I', 'O', 'P', '{', '}', '\n', 0,
    'A', 'S', 'D', 'F', 'G', 'H', 'J', 'K', 'L', ':', '"', '~', 0, '|',
    'Z', 'X', 'C', 'V', 'B', 'N', 'M', '<', '>', '?', 0, '*', 0, ' ', 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0,
    '7', '8', '9', '-', '4', '5', '6', '+', '1', '2', '3', '0', '.',
};

#define SC_LCTRL  0x1D
#define SC_LSHIFT 0x2A
#define SC_RSHIFT 0x36
#define SC_LALT   0x38
#define SC_CAPS   0x3A
#define SC_DEL    0x53
#define SC_POWER  0x5E                  /* E0-prefixed */
#define SC_SLEEP  0x5F

#define KBD_ACK    0xFA
#define KBD_RESEND 0xFE
#define KBD_SET_LEDS 0xED
#define LED_CAPS   0x04

static int shift, ctrl, alt, caps, extended;
static int e1_skip;                     /* Pause's E1 sequence: bytes still to come */
volatile uint32_t keyboard_last_tick;

/* Setting the lights is a command and a byte, each acknowledged; the
 * acknowledgements arrive as interrupts, so it goes a step at a time. */
static int led_step;                    /* 0 idle, 1 command sent, 2 value sent */
static uint32_t led_tick;
static uint8_t leds;

static void kbd_write(uint8_t b)
{
    for (int i = 0; i < 100000 && (inb(0x64) & 0x02); i++)
        ;
    outb(0x60, b);
}

static void set_leds(void)
{
    leds = caps ? LED_CAPS : 0;
    if (led_step && (int32_t)(ticks - led_tick) < HZ)
        return;                         /* under way: the newest value goes with it */
    led_step = 1;
    led_tick = ticks;
    kbd_write(KBD_SET_LEDS);
}

/* An acknowledgement or a resend request: 1 if it was for the lights. */
static int led_answer(uint8_t b)
{
    if (!led_step)
        return 0;
    if (b == KBD_RESEND) {
        kbd_write(led_step == 1 ? KBD_SET_LEDS : leds);
    } else if (led_step == 1) {
        led_step = 2;
        kbd_write(leds);
    } else {
        led_step = 0;
    }
    return 1;
}

static int mods(void)
{
    return (shift ? KMOD_SHIFT : 0) | (ctrl ? KMOD_CTRL : 0) | (alt ? KMOD_ALT : 0);
}

static void key_event(uint16_t code, int pressed, const char *text, int len)
{
    if (!input_grabbed()) {
        for (int i = 0; i < len; i++)
            tty_input(text[i]);
        return;
    }
    struct input_event ev = { .type = IEV_KEY, .code = code };
    ev.flags = pressed ? IEV_PRESSED : 0;
    ev.mods = (uint8_t)mods();
    ev.len = (uint8_t)len;
    memcpy(ev.text, text, (uint32_t)len);
    input_push(&ev);
}

static const char *extended_text(uint8_t sc)
{
    switch (sc) {
    case 0x48: return "\033[A";        /* up */
    case 0x50: return "\033[B";        /* down */
    case 0x4D: return "\033[C";        /* right */
    case 0x4B: return "\033[D";        /* left */
    case 0x47: return "\033[H";        /* home */
    case 0x4F: return "\033[F";        /* end */
    case 0x53: return "\033[3~";       /* delete */
    case 0x49: return "\033[5~";       /* page up */
    case 0x51: return "\033[6~";       /* page down */
    case 0x52: return "\033[2~";       /* insert */
    case 0x1C: return "\n";            /* keypad enter */
    case 0x35: return "/";              /* keypad slash */
    }
    return "";
}

/* Function keys send xterm's sequences: ESC O P-S for F1-F4, ESC [ n ~ above. */
static const char *function_text(uint8_t sc)
{
    switch (sc) {
    case 0x3B: return "\033OP";
    case 0x3C: return "\033OQ";
    case 0x3D: return "\033OR";
    case 0x3E: return "\033OS";
    case 0x3F: return "\033[15~";
    case 0x40: return "\033[17~";
    case 0x41: return "\033[18~";
    case 0x42: return "\033[19~";
    case 0x43: return "\033[20~";
    case 0x44: return "\033[21~";
    case 0x57: return "\033[23~";
    case 0x58: return "\033[24~";
    }
    return "";
}

static void key(uint8_t sc, int ext, int released);

/*
 * One key going down or up. Both keyboards come here: the PS/2 interrupt
 * with the codes it reads, and USB keyboards with their keys translated
 * to the same codes, perhaps on another processor at the same time: the
 * modifier keys' state has a lock.
 */
void keyboard_key(uint8_t sc, int ext, int released)
{
    static spinlock_t kbd_lock = SPINLOCK_INIT("keyboard");
    spin_lock(&kbd_lock);
    key(sc, ext, released);
    spin_unlock(&kbd_lock);
}

static void key(uint8_t sc, int ext, int released)
{
    if (!released && sc != SC_LSHIFT && sc != SC_RSHIFT && sc != SC_LCTRL && sc != SC_LALT)
        keyboard_last_tick = ticks;
    if (ext) {
        if (sc == SC_LSHIFT || sc == SC_RSHIFT)
            return;                     /* fake shifts around nav keys */
        if (!released && (sc == SC_POWER || sc == SC_SLEEP) && !input_grabbed()) {
            acpi_button(sc == SC_SLEEP);
            return;
        }
        if (sc == SC_LCTRL)
            ctrl = !released;
        else if (sc == SC_LALT)
            alt = !released;
        else if (!released && ctrl && alt && sc == SC_DEL)
            machine_reboot();
        const char *text = released ? "" : extended_text(sc);
        key_event(0x80 | sc, !released, text, (int)strlen(text));
        return;
    }

    switch (sc) {
    case SC_LSHIFT:
    case SC_RSHIFT:
        shift = !released;
        key_event(sc, !released, "", 0);
        return;
    case SC_LCTRL:
        ctrl = !released;
        key_event(sc, !released, "", 0);
        return;
    case SC_LALT:
        alt = !released;
        key_event(sc, !released, "", 0);
        return;
    case SC_CAPS:
        if (!released) {
            caps = !caps;
            set_leds();
        }
        return;
    }

    if (released) {
        key_event(sc, 0, "", 0);
        return;
    }
    if (ctrl && alt && sc == SC_DEL)
        machine_reboot();

    char c = shift ? keymap_shift[sc] : keymap[sc];
    if (!c) {
        const char *text = function_text(sc);
        key_event(sc, 1, text, (int)strlen(text));
        return;
    }
    if (caps && c >= 'a' && c <= 'z' && !shift)
        c = (char)(c - 'a' + 'A');
    else if (caps && c >= 'A' && c <= 'Z' && shift)
        c = (char)(c - 'A' + 'a');

    if (ctrl) {
        if (c >= 'a' && c <= 'z')
            c = (char)(c - 'a' + 1);
        else if (c >= 'A' && c <= 'Z')
            c = (char)(c - 'A' + 1);
        else if (c == '[')
            c = 0x1B;
        else if (c == '\\')
            c = 0x1C;
        else if (c == ' ')
            c = 0;
    }
    char text[2];
    int len = 0;
    if (alt)
        text[len++] = 0x1B;             /* meta sends an ESC prefix */
    text[len++] = c;
    key_event(sc, 1, text, len);
}

static void keyboard_irq(struct trapframe *tf)
{
    (void)tf;
    uint8_t st = inb(0x64);
    if (!(st & 0x01) || (st & 0x20))
        return;                         /* nothing, or mouse data (IRQ12) */
    uint8_t sc = inb(0x60);

    if (sc == KBD_ACK || sc == KBD_RESEND) {
        led_answer(sc);                 /* not keys, whatever they answer */
        return;
    }
    if (e1_skip) {
        e1_skip--;
        return;
    }
    if (sc == 0xE1) {
        e1_skip = 5;                    /* Pause: E1 1D 45 E1 9D C5, no release */
        return;
    }
    if (sc == 0xE0) {
        extended = 1;
        return;
    }
    int ext = extended;
    extended = 0;
    keyboard_key(sc & 0x7F, ext, sc & 0x80);
}

void keyboard_init(void)
{
    /* Drain anything the firmware left in the output buffer. */
    for (int i = 0; i < 64 && (inb(0x64) & 0x01); i++)
        (void)inb(0x60);
    irq_register(IRQ_KBD, keyboard_irq);
}

/* After suspend to RAM, from i8042_resume() with interrupts off: the
 * keyboard scanning again, with its lights. */
void keyboard_resume(void)
{
    extended = 0;
    e1_skip = 0;
    led_step = 0;
    kbd_write(0xF4);                    /* enable scanning */
    for (int i = 0; i < 1000 && !(inb(0x64) & 0x01); i++)
        udelay(100);
    if ((inb(0x64) & 0x21) == 0x01)
        (void)inb(0x60);                /* its acknowledgement */
    if (caps) {
        kbd_write(KBD_SET_LEDS);
        udelay(5000);
        kbd_write(LED_CAPS);
        udelay(5000);
        for (int i = 0; i < 4 && (inb(0x64) & 0x01); i++)
            (void)inb(0x60);
    }
}

/*
 * PS/2 keyboard driver (scancode set 1, US layout). Keys are translated
 * to bytes and VT100 escape sequences and fed into the tty.
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

static int shift, ctrl, alt, caps, extended;

static void send(const char *s)
{
    while (*s)
        tty_input(*s++);
}

static void extended_key(uint8_t sc)
{
    switch (sc) {
    case 0x48: send("\033[A"); break;   /* up */
    case 0x50: send("\033[B"); break;   /* down */
    case 0x4D: send("\033[C"); break;   /* right */
    case 0x4B: send("\033[D"); break;   /* left */
    case 0x47: send("\033[H"); break;   /* home */
    case 0x4F: send("\033[F"); break;   /* end */
    case 0x53: send("\033[3~"); break;  /* delete */
    case 0x49: send("\033[5~"); break;  /* page up */
    case 0x51: send("\033[6~"); break;  /* page down */
    case 0x52: send("\033[2~"); break;  /* insert */
    case 0x1C: tty_input('\n'); break;  /* keypad enter */
    case 0x35: tty_input('/'); break;   /* keypad slash */
    }
}

static void keyboard_irq(struct trapframe *tf)
{
    (void)tf;
    uint8_t sc = inb(0x60);

    if (sc == 0xE0) {
        extended = 1;
        return;
    }

    int released = sc & 0x80;
    sc &= 0x7F;

    if (extended) {
        extended = 0;
        if (sc == SC_LCTRL)
            ctrl = !released;
        else if (sc == SC_LALT)
            alt = !released;
        else if (sc == SC_LSHIFT || sc == SC_RSHIFT)
            ;                           /* fake shifts around nav keys */
        else if (!released) {
            if (ctrl && alt && sc == SC_DEL)
                machine_reboot();
            extended_key(sc);
        }
        return;
    }

    switch (sc) {
    case SC_LSHIFT:
    case SC_RSHIFT:
        shift = !released;
        return;
    case SC_LCTRL:
        ctrl = !released;
        return;
    case SC_LALT:
        alt = !released;
        return;
    case SC_CAPS:
        if (!released)
            caps = !caps;
        return;
    }

    if (released)
        return;
    if (ctrl && alt && sc == SC_DEL)
        machine_reboot();

    char c = shift ? keymap_shift[sc] : keymap[sc];
    if (!c)
        return;
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
    if (alt)
        tty_input(0x1B);                /* meta sends an ESC prefix */
    tty_input(c);
}

void keyboard_init(void)
{
    /* Drain anything the firmware left in the output buffer. */
    for (int i = 0; i < 64 && (inb(0x64) & 0x01); i++)
        (void)inb(0x60);
    irq_register(IRQ_KBD, keyboard_irq);
}

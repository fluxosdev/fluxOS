/*
 * The keyboard. The terminal is put in raw mode, and what it sends
 * (characters and xterm escape sequences for arrows, function keys and
 * Alt combinations) becomes PC keystrokes: a scan code and a character.
 *
 * Keystrokes reach the program the way a real keyboard's do: each makes
 * port 60h hold a make code and raises IRQ 1, then a break code, so
 * programs with their own INT 9 handler see them. The default INT 9
 * handler puts the keystroke in the BIOS buffer in the data area
 * (40:1E-40:3D), where INT 16h and DOS find it.
 *
 * Ctrl+\ ends the emulator, whatever the program is doing.
 */
#include <flux.h>
#include <poll.h>
#include <string.h>
#include <unistd.h>
#include "dos.h"

#define QUEUE 64
#define INPUT 256
#define ESC_WAIT_MS 30                  /* a lone ESC: no sequence follows */

#define MOD_SHIFT 0x02                  /* BIOS shift-state bits (40:17) */
#define MOD_CTRL  0x04
#define MOD_ALT   0x08

static int in_fd = -1;
static int old_mode = -1;

static uint32_t queue[QUEUE];           /* mods << 16 | scan << 8 | char */
static int q_head, q_tail;
static uint8_t input[INPUT];
static int in_len;

/* Delivery of the key at the head of the queue through port 60h:
 * 0 idle, 1 make code raised, 2 break code raised. */
static int phase;
static uint32_t current;
static uint8_t port60;
static int port_read;                   /* port 60h read since the last IRQ */
static int empty_polls;

/* Scan codes of the ASCII characters on a US keyboard. */
static uint8_t scan_of[128];

static void build_scan_table(void)
{
    static const struct { const char *keys; uint8_t first; } rows[] = {
        { "1234567890-=", 0x02 }, { "!@#$%^&*()_+", 0x02 },
        { "qwertyuiop[]", 0x10 }, { "QWERTYUIOP{}", 0x10 },
        { "asdfghjkl;'`", 0x1E }, { "ASDFGHJKL:\"~", 0x1E },
        { "\\zxcvbnm,./", 0x2B }, { "|ZXCVBNM<>?", 0x2B },
    };
    for (unsigned r = 0; r < sizeof(rows) / sizeof(rows[0]); r++)
        for (int i = 0; rows[r].keys[i]; i++)
            scan_of[(unsigned char)rows[r].keys[i]] = (uint8_t)(rows[r].first + i);
    scan_of[' '] = 0x39;
    scan_of['\t'] = 0x0F;
    scan_of['\r'] = 0x1C;
    scan_of['\b'] = 0x0E;
    scan_of[0x1B] = 0x01;
    for (int c = 1; c <= 26; c++)       /* Ctrl+letter */
        if (c != '\t' && c != '\r' && c != '\b')
            scan_of[c] = scan_of['a' + c - 1];
}

void kbd_init(void)
{
    if (!scan_of['a'])
        build_scan_table();
    if (isatty(0)) {
        in_fd = 0;
        old_mode = ttymode(0);          /* no echo, no line editing, no signals */
    }
}

void kbd_restore(void)
{
    if (old_mode >= 0)
        ttymode(old_mode);
    old_mode = -1;
}

static void enqueue(uint16_t key, int mods)
{
    int next = (q_tail + 1) % QUEUE;
    if (next == q_head)
        return;                         /* full: the PC would beep */
    queue[q_tail] = (uint32_t)mods << 16 | key;
    q_tail = next;
}

static void key_char(uint8_t c)
{
    if (c == '\n')
        c = '\r';                       /* Enter */
    else if (c == 0x7F)
        c = '\b';                       /* Backspace */
    if (c >= 0x80) {
        enqueue(c, 0);
        return;
    }
    int mods = 0;
    if (c >= 1 && c <= 26 && c != '\t' && c != '\r' && c != '\b')
        mods = MOD_CTRL;
    else if ((c >= 'A' && c <= 'Z') || (c > 32 && c < 127 && strchr("!@#$%^&*()_+{}:\"~|<>?", c)))
        mods = MOD_SHIFT;
    if (c == 0)
        enqueue(0x0300, MOD_CTRL);      /* Ctrl+2 / Ctrl+Space */
    else
        enqueue((uint16_t)(scan_of[c] << 8 | c), mods);
}

/* Alt+key: the scan code with no character. */
static void key_alt(uint8_t c)
{
    if (c >= '1' && c <= '9')
        enqueue((uint16_t)((0x78 + c - '1') << 8), MOD_ALT);
    else if (c == '0')
        enqueue(0x8100, MOD_ALT);
    else if (c == '-')
        enqueue(0x8200, MOD_ALT);
    else if (c == '=')
        enqueue(0x8300, MOD_ALT);
    else if (c == 0x7F || c == '\b')
        enqueue(0x0E00, MOD_ALT);
    else if (c == '\n' || c == '\r')
        enqueue(0x1C00, MOD_ALT);
    else if (c < 128 && scan_of[c])
        enqueue((uint16_t)(scan_of[c] << 8), MOD_ALT);
}

/* Function key n (1-12) with xterm modifier code m (1 none, 2 shift,
 * 3 alt, 5 ctrl). */
static void key_function(int n, int m)
{
    static const uint8_t base[12] = {
        0x3B, 0x3C, 0x3D, 0x3E, 0x3F, 0x40, 0x41, 0x42, 0x43, 0x44, 0x85, 0x86,
    };
    int shift = m == 2, alt = m == 3 || m == 4, ctrl = m >= 5;
    uint8_t s = base[n - 1];
    if (n <= 10) {
        if (shift)
            s = (uint8_t)(0x54 + n - 1);
        else if (ctrl)
            s = (uint8_t)(0x5E + n - 1);
        else if (alt)
            s = (uint8_t)(0x68 + n - 1);
    } else {
        if (shift)
            s = (uint8_t)(0x87 + n - 11);
        else if (ctrl)
            s = (uint8_t)(0x89 + n - 11);
        else if (alt)
            s = (uint8_t)(0x8B + n - 11);
    }
    enqueue((uint16_t)(s << 8), shift ? MOD_SHIFT : ctrl ? MOD_CTRL : alt ? MOD_ALT : 0);
}

/* Cursor keypad keys; Ctrl changes some of them. */
static void key_nav(uint8_t scan, int m)
{
    if (m >= 5) {
        switch (scan) {
        case 0x4B: scan = 0x73; break;  /* Ctrl+Left */
        case 0x4D: scan = 0x74; break;
        case 0x47: scan = 0x77; break;  /* Ctrl+Home */
        case 0x4F: scan = 0x75; break;
        case 0x49: scan = 0x84; break;  /* Ctrl+PgUp */
        case 0x51: scan = 0x76; break;
        case 0x48: scan = 0x8D; break;  /* Ctrl+Up */
        case 0x50: scan = 0x91; break;
        }
    }
    enqueue((uint16_t)(scan << 8), m == 2 ? MOD_SHIFT : m >= 5 ? MOD_CTRL : 0);
}

/* Parse one escape sequence at input[i] (after the ESC). Returns the
 * bytes it used, or 0 if it is incomplete. */
static int parse_escape(int i)
{
    int start = i;
    if (i >= in_len)
        return 0;
    uint8_t intro = input[i++];
    if (intro != '[' && intro != 'O') {
        key_alt(intro);
        return i - start;
    }
    if (intro == '[' && i < in_len && input[i] == '[') {    /* Linux console F1-F5 */
        if (i + 1 >= in_len)
            return 0;
        uint8_t f = input[i + 1];
        if (f >= 'A' && f <= 'E')
            key_function(f - 'A' + 1, 1);
        return i + 2 - start;
    }
    int p[2] = { 0, 0 }, np = 0;
    while (i < in_len && ((input[i] >= '0' && input[i] <= '9') || input[i] == ';')) {
        if (input[i] == ';') {
            if (np < 1)
                np++;
        } else {
            p[np] = p[np] * 10 + (input[i] - '0');
        }
        i++;
    }
    if (i >= in_len)
        return 0;
    uint8_t final = input[i++];
    int m = p[1] ? p[1] : 1;
    switch (final) {
    case 'A': key_nav(0x48, m); break;
    case 'B': key_nav(0x50, m); break;
    case 'C': key_nav(0x4D, m); break;
    case 'D': key_nav(0x4B, m); break;
    case 'H': key_nav(0x47, m); break;
    case 'F': key_nav(0x4F, m); break;
    case 'E': key_nav(0x4C, m); break;
    case 'P': key_function(1, m); break;
    case 'Q': key_function(2, m); break;
    case 'R': key_function(3, m); break;
    case 'S': key_function(4, m); break;
    case 'Z': enqueue(0x0F00, MOD_SHIFT); break;    /* Shift+Tab */
    case '~':
        switch (p[0]) {
        case 1: case 7: key_nav(0x47, m); break;
        case 4: case 8: key_nav(0x4F, m); break;
        case 2: key_nav(0x52, m); break;
        case 3: key_nav(0x53, m); break;
        case 5: key_nav(0x49, m); break;
        case 6: key_nav(0x51, m); break;
        case 11: case 12: case 13: case 14: case 15:
            key_function(p[0] - 10, m);
            break;
        case 17: case 18: case 19: case 20: case 21:
            key_function(p[0] - 11, m);
            break;
        case 23: case 24:
            key_function(p[0] - 12, m);
            break;
        }
        break;
    }
    return i - start;
}

static void stop_requested(void)
{
    fatal("stopped (Ctrl+\\)");
}

/* Turn the bytes read so far into keystrokes. A trailing incomplete
 * escape sequence is kept for the next read. */
static void parse_input(int flush_esc)
{
    int i = 0;
    while (i < in_len) {
        uint8_t c = input[i];
        if (c == 0x1C)
            stop_requested();
        if (c != 0x1B) {
            key_char(c);
            i++;
            continue;
        }
        if (i + 1 >= in_len) {
            if (!flush_esc)
                break;
            key_char(0x1B);             /* the Esc key itself */
            i++;
            continue;
        }
        if (input[i + 1] == 0x1B) {
            key_char(0x1B);
            i++;
            continue;
        }
        int n = parse_escape(i + 1);
        if (!n) {
            if (!flush_esc)
                break;
            key_char(0x1B);
            i++;
            continue;
        }
        i += 1 + n;
    }
    memmove(input, input + i, (size_t)(in_len - i));
    in_len -= i;
}

int kbd_poll(int timeout_ms)
{
    int before = (q_tail - q_head + QUEUE) % QUEUE;
    if (in_fd < 0) {
        if (timeout_ms > 0)
            msleep((unsigned)timeout_ms);
        return 0;
    }
    struct pollfd pfd = { in_fd, POLLIN, 0 };
    if (poll(&pfd, 1, timeout_ms) > 0 && (pfd.revents & (POLLIN | POLLHUP))) {
        int n = (int)read(in_fd, input + in_len, (size_t)(INPUT - in_len));
        if (n > 0)
            in_len += n;
        else if (n == 0 && (pfd.revents & POLLHUP))
            fatal("the terminal went away");
    }
    parse_input(0);
    if (in_len && input[0] == 0x1B) {
        /* Wait briefly for the rest of an escape sequence. */
        pfd.revents = 0;
        if (poll(&pfd, 1, ESC_WAIT_MS) > 0) {
            int n = (int)read(in_fd, input + in_len, (size_t)(INPUT - in_len));
            if (n > 0)
                in_len += n;
        }
        parse_input(1);
    }
    kbd_pump();
    return (q_tail - q_head + QUEUE) % QUEUE - before;
}

/* Present the next make or break code on port 60h and raise IRQ 1. With
 * IRQ 1 masked (a program polling the port), a read of the port moves on. */
void kbd_pump(void)
{
    if (irq_busy(1) && !irq_masked(1))
        return;
    if (irq_masked(1) && phase && !port_read)
        return;
    switch (phase) {
    case 0:
        if (q_head == q_tail)
            return;
        current = queue[q_head];
        q_head = (q_head + 1) % QUEUE;
        port60 = (uint8_t)(current >> 8);
        mem[0x417] = (uint8_t)((mem[0x417] & ~(MOD_SHIFT | MOD_CTRL | MOD_ALT)) | (current >> 16));
        phase = 1;
        break;
    case 1:
        port60 = (uint8_t)((current >> 8) | 0x80);
        phase = 2;
        break;
    default:
        mem[0x417] &= (uint8_t)~(MOD_SHIFT | MOD_CTRL | MOD_ALT);
        phase = 0;
        return;
    }
    port_read = 0;
    pic_raise(1);
}

uint8_t kbd_port60(void)
{
    port_read = 1;
    return port60;
}

uint8_t kbd_port64(void)
{
    return (uint8_t)(0x1C | (phase && !port_read));
}

/* The BIOS's INT 9: store the keystroke of a make code. */
void kbd_irq_ack(void)
{
    port_read = 1;
    if (phase == 1 && !(port60 & 0x80))
        kbd_push((uint16_t)current);
}

/* ---- the BIOS keyboard buffer ---- */

int kbd_available(void)
{
    return rw(0x41A) != rw(0x41C);
}

uint16_t kbd_peek(void)
{
    return kbd_available() ? rw(0x400 + rw(0x41A)) : 0;
}

uint16_t kbd_read(void)
{
    if (!kbd_available())
        return 0;
    uint16_t head = rw(0x41A);
    uint16_t key = rw(0x400 + head);
    head = (uint16_t)(head + 2);
    if (head >= rw(0x482))
        head = rw(0x480);
    ww(0x41A, head);
    empty_polls = 0;
    return key;
}

void kbd_push(uint16_t key)
{
    uint16_t tail = rw(0x41C), next = (uint16_t)(tail + 2);
    if (next >= rw(0x482))
        next = rw(0x480);
    if (next == rw(0x41A))
        return;                         /* full */
    ww(0x400 + tail, key);
    ww(0x41C, next);
}

/* A program that keeps asking for a key and gets none is waiting for
 * one: let the host sleep now and then instead of spinning. */
void kbd_idle_hint(void)
{
    if (++empty_polls >= 50) {
        empty_polls = 0;
        request_idle(5);
    }
}

void int16(void)
{
    switch (AH) {
    case 0x00:                          /* wait for a key */
    case 0x10:
        if (!kbd_available()) {
            blocked();
            return;
        }
        AX = kbd_read();
        if (AH == 0x00 && AL == 0xE0)
            AL = 0;
        return;
    case 0x01:                          /* is a key ready? */
    case 0x11:
        if (kbd_available()) {
            AX = kbd_peek();
            set_zf(0);
        } else {
            set_zf(1);
            kbd_idle_hint();
        }
        return;
    case 0x02:                          /* shift state */
        AL = mem[0x417];
        return;
    case 0x12:
        AL = mem[0x417];
        AH = mem[0x418];
        return;
    case 0x05:                          /* store a keystroke */
        if (rw(0x41C) + 2 == rw(0x41A) || (rw(0x41C) + 2 == rw(0x482) && rw(0x41A) == rw(0x480))) {
            AL = 1;
        } else {
            kbd_push(CX);
            AL = 0;
        }
        return;
    default:
        return;
    }
}

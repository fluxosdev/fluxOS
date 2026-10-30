/*
 * The PC around the CPU: the ROM with the interrupt stubs, the BIOS
 * data area, the interrupt controller, the timer, I/O ports, and the
 * BIOS services other than video and keyboard.
 *
 * Every interrupt vector starts out pointing to an 8-byte stub in the
 * ROM at F000:1000 + 8n, "0F FF n 00 CF": the emulator callback for n,
 * then IRET. A callback returns its carry and zero flags to the caller
 * by patching the flags image the IRET pops. A callback that has to
 * wait (for a key, say) rewinds to its 0F FF and enables interrupts, so
 * the timer and keyboard interrupts keep running while it waits, as
 * they do in a real BIOS's wait loop.
 */
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <flux/font.h>
#include "dos.h"

static uint8_t conventional[MEM_SIZE + 4];
uint8_t *mem = conventional;
uint32_t mem_mask = MEM_SIZE - 1;
int video_written;

/* ---- interrupt controller (one 8259; IRQs 8-15 never fire) ---- */

uint8_t pic_irr;
static uint8_t pic_imr, pic_isr;

void pic_raise(int irq)
{
    pic_irr |= (uint8_t)(1 << irq);
}

/* The highest-priority request that no in-service interrupt blocks. */
int pic_pending(void)
{
    uint8_t req = pic_irr & ~pic_imr;
    for (int i = 0; i < 8 && req; i++) {
        if (pic_isr & (1 << i))
            return -1;
        if (req & (1 << i))
            return i;
    }
    return -1;
}

void pic_ack(int irq)
{
    pic_irr &= (uint8_t)~(1 << irq);
    pic_isr |= (uint8_t)(1 << irq);
}

static void pic_eoi(void)
{
    pic_isr &= (uint8_t)(pic_isr - 1);  /* clear the highest-priority bit */
}

static void pic_eoi_irq(int irq)
{
    pic_isr &= (uint8_t)~(1 << irq);
}

int irq_busy(int irq)
{
    return ((pic_irr | pic_isr) >> irq) & 1;
}

int irq_masked(int irq)
{
    return (pic_imr >> irq) & 1;
}

int irq_hooked(int irq)
{
    int vector = irq < 8 ? 8 + irq : 0x70 + irq - 8;
    return rw((uint32_t)vector * 4) != CALLBACK_OFF(vector) ||
           rw((uint32_t)vector * 4 + 2) != BIOS_SEG || dpmi_hooked(vector);
}

/* ---- timer (8253 PIT) ---- */

#define PIT_HZ 1193182u

struct pit_channel {
    uint32_t reload;                    /* 1-65536 */
    uint8_t access;                     /* 1 low byte, 2 high byte, 3 both */
    uint8_t write_hi, read_hi;          /* next byte of a two-byte access */
    uint8_t mode;                       /* counter mode 0-5 */
    uint8_t latched;
    uint16_t latch;
};

static struct pit_channel pit[3];
static uint32_t timer_period_us = 54925;
static uint32_t timer_last_ms, timer_acc_us;
static int timer_backlog;
static uint8_t port61;
static int retrace;
static uint8_t crtc_index, crtc_regs[32];

/* The counter runs at instruction speed (about one count per eight
 * instructions): delay loops calibrated against it then behave. */
static uint16_t pit_count(int ch)
{
    uint32_t elapsed = cpu.steps / 8;
    return (uint16_t)(pit[ch].reload - elapsed % pit[ch].reload);
}

static void pit_write(int ch, uint8_t v)
{
    struct pit_channel *p = &pit[ch];
    uint32_t r = p->reload == 65536 ? 0 : p->reload;
    if (p->access == 1) {
        r = v;
    } else if (p->access == 2) {
        r = (uint32_t)v << 8;
    } else if (!p->write_hi) {
        r = (r & 0xFF00) | v;
        p->write_hi = 1;
        p->reload = r ? r : 65536;
        return;
    } else {
        r = (r & 0x00FF) | (uint32_t)v << 8;
        p->write_hi = 0;
    }
    p->reload = r ? r : 65536;
    if (ch == 2)
        speaker_update(port61, p->reload, p->mode);
    if (ch == 0) {
        timer_period_us = p->reload * 838 / 1000;   /* 1 / 1.193182 MHz */
        if (timer_period_us < 200)
            timer_period_us = 200;
    }
}

static uint8_t pit_read(int ch)
{
    struct pit_channel *p = &pit[ch];
    uint16_t v = p->latched ? p->latch : pit_count(ch);
    uint8_t b;
    if (p->access == 1) {
        b = (uint8_t)v;
        p->latched = 0;
    } else if (p->access == 2) {
        b = (uint8_t)(v >> 8);
        p->latched = 0;
    } else if (!p->read_hi) {
        b = (uint8_t)v;
        p->read_hi = 1;
    } else {
        b = (uint8_t)(v >> 8);
        p->read_hi = 0;
        p->latched = 0;
    }
    return b;
}

static void pit_control(uint8_t v)
{
    int ch = v >> 6;
    if (ch == 3)
        return;                         /* read-back: not on an 8253 */
    struct pit_channel *p = &pit[ch];
    int access = (v >> 4) & 3;
    if (!access) {
        p->latch = pit_count(ch);
        p->latched = 1;
        return;
    }
    p->access = (uint8_t)access;
    p->mode = (uint8_t)((v >> 1) & 7);
    p->write_hi = p->read_hi = 0;
}

/* Raise IRQ 0 for the timer ticks that are due. */
void timer_poll(void)
{
    uint32_t now = now_ms();
    uint32_t elapsed = now - timer_last_ms;
    timer_last_ms = now;
    if (elapsed > 1000)
        elapsed = 1000;                 /* after a long wait, do not flood */
    timer_acc_us += elapsed * 1000;
    while (timer_acc_us >= timer_period_us) {
        timer_acc_us -= timer_period_us;
        if (timer_backlog < 8)
            timer_backlog++;
    }
    if (timer_backlog && !irq_busy(0)) {
        timer_backlog--;
        pic_raise(0);
    }
}

uint32_t bios_ticks(void)
{
    return rd(0x46C);
}

/* INT 8: the BIOS's timer tick, counting time of day; the stub then
 * calls INT 1Ch. */
static void timer_tick(void)
{
    uint32_t t = bios_ticks() + 1;
    if (t >= 0x1800B0) {
        t = 0;
        mem[0x470] = 1;
    }
    wd(0x46C, t);
    pic_eoi_irq(0);
}

/* ---- I/O ports ---- */

uint32_t io_in(uint16_t port, int size)
{
    if (size > 8)
        return io_in(port, 8) | io_in((uint16_t)(port + 1), size - 8) << 8;
    uint8_t sv;
    if (sound_io_in(port, &sv) || vga_io_in(port, &sv))
        return sv;
    switch (port) {
    case 0x20:
        return 0;
    case 0x21:
        return pic_imr;
    case 0x40:
    case 0x41:
    case 0x42:
        return pit_read(port - 0x40);
    case 0x60:
        return kbd_port60();
    case 0x61:
        port61 ^= 0x30;                 /* refresh toggle and timer 2 output */
        return port61;
    case 0x64:
        return kbd_port64();
    case 0x201:
        return 0xF0;                    /* no joystick: buttons up, axes settled */
    case 0x3BA:
    case 0x3DA:
        retrace = !retrace;             /* alternate between retrace and display */
        return retrace ? 0x09 : 0x00;
    case 0x3B5:
    case 0x3D5:
        if (crtc_index == 0x0E || crtc_index == 0x0F) {
            uint32_t pos = rb(0x451) * (uint32_t)rw(0x44A) + rb(0x450);
            return crtc_index == 0x0E ? (pos >> 8) & 0xFF : pos & 0xFF;
        }
        return crtc_regs[crtc_index & 31];
    case 0x3B4:
    case 0x3D4:
        return crtc_index;
    }
    return 0xFF;
}

void io_out(uint16_t port, uint32_t v, int size)
{
    if (size > 8) {
        io_out(port, v & 0xFF, 8);
        io_out((uint16_t)(port + 1), v >> 8, size - 8);
        return;
    }
    uint8_t b = (uint8_t)v;
    if (sound_io_out(port, b) || vga_io_out(port, b))
        return;
    switch (port) {
    case 0x20:
        if (b == 0x20)
            pic_eoi();
        else if ((b & 0xF8) == 0x60)
            pic_eoi_irq(b & 7);
        return;
    case 0x21:
        pic_imr = b;
        return;
    case 0x40:
    case 0x41:
    case 0x42:
        pit_write(port - 0x40, b);
        return;
    case 0x43:
        pit_control(b);
        return;
    case 0x61:
        port61 = b;
        speaker_update(port61, pit[2].reload, pit[2].mode);
        return;
    case 0x3B4:
    case 0x3D4:
        crtc_index = b;
        return;
    case 0x3B5:
    case 0x3D5:
        crtc_regs[crtc_index & 31] = b;
        video_crtc_write(crtc_index, b);
        return;
    }
}

/* ---- callbacks ---- */

/* The current callback cannot finish yet: run it again once interrupts
 * had a chance (the CPU gets back to it after the next IRQ or idle). */
void blocked(void)
{
    cpu.ip = (cpu.ip - 4) & 0xFFFF;
    cpu.flags |= F_IF;
    request_idle(55);
}

static uint8_t bcd(int v)
{
    return (uint8_t)((v / 10) << 4 | v % 10);
}

static void int15(void)
{
    static int waiting;
    static uint32_t wait_until;
    switch (AH) {
    case 0x4F:                          /* keyboard intercept: keep the key */
        set_cf(1);
        return;
    case 0x86: {                        /* wait CX:DX microseconds */
        uint32_t us = (uint32_t)CX << 16 | DX;
        if (!waiting) {
            waiting = 1;
            wait_until = now_ms() + us / 1000;
        }
        if ((int32_t)(now_ms() - wait_until) < 0) {
            blocked();
            return;
        }
        waiting = 0;
        set_cf(0);
        AH = 0;
        return;
    }
    case 0x88:                          /* no extended memory */
        AX = 0;
        set_cf(0);
        return;
    case 0x90:                          /* device busy / interrupt complete */
    case 0x91:
        AH = 0;
        set_cf(0);
        return;
    case 0xC0:                          /* system configuration table */
        ES = BIOS_SEG;
        BX = 0xE6F5;
        AH = 0;
        set_cf(0);
        return;
    default:
        AH = 0x86;                      /* not supported */
        set_cf(1);
        return;
    }
}

static void int1a(void)
{
    time_t now = time(NULL);
    struct tm tm;
    gmtime_r(&now, &tm);
    switch (AH) {
    case 0x00:
        CX = (uint16_t)(bios_ticks() >> 16);
        DX = (uint16_t)bios_ticks();
        AL = mem[0x470];
        mem[0x470] = 0;
        return;
    case 0x01:
        wd(0x46C, (uint32_t)CX << 16 | DX);
        mem[0x470] = 0;
        return;
    case 0x02:
        CH = bcd(tm.tm_hour);
        CL = bcd(tm.tm_min);
        DH = bcd(tm.tm_sec);
        DL = 0;
        set_cf(0);
        return;
    case 0x04:
        CH = bcd(19 + tm.tm_year / 100);
        CL = bcd(tm.tm_year % 100);
        DH = bcd(tm.tm_mon + 1);
        DL = bcd(tm.tm_mday);
        set_cf(0);
        return;
    case 0x03:
    case 0x05:                          /* setting the clock: accepted, ignored */
        set_cf(0);
        return;
    default:
        set_cf(1);
        return;
    }
}

static void unhandled(int id)
{
    static uint8_t reported[256];
    if (id < 256 && !reported[id]) {
        reported[id] = 1;
        dlog("unhandled INT %02Xh AX=%04X\n", id, AX);
    }
}

void bios_callback(int id)
{
    /* The interrupted code's IP, CS and flags are at SS:SP. */
    uint16_t ss = SS, sp = SP, cs = CS;
    uint32_t ip = cpu.ip;

    switch (id) {
    case 0x00:
        con_puts("\r\nDivide overflow\r\n");
        dos_terminate(0, 0);
        break;
    case 0x06:
        fatal("illegal instruction at %04X:%04X", rw(ADDR(ss, sp + 2)), rw(ADDR(ss, sp)));
    case 0x08:
        timer_tick();
        break;
    case 0x09:
        kbd_irq_ack();
        pic_eoi_irq(1);
        break;
    case 0x0D:                          /* IRQ 5 (the Sound Blaster) and 7 with */
    case 0x0F:                          /* no handler: just end the interrupt */
        pic_eoi_irq(id - 8);
        break;
    case 0x10:
        int10();
        break;
    case 0x11:
        AX = rw(0x410);
        break;
    case 0x12:
        AX = rw(0x413);
        break;
    case 0x13:                          /* no BIOS disks */
        if (AH == 0x00 || AH == 0x0D) {
            AH = 0;
            set_cf(0);
        } else if (AH == 0x15) {
            AH = 0;                     /* no such drive */
            set_cf(0);
        } else {
            AH = 0x01;
            set_cf(1);
        }
        break;
    case 0x14:                          /* serial ports: time out */
        AH = 0x80;
        break;
    case 0x15:
        int15();
        break;
    case 0x16:
        int16();
        break;
    case 0x17:                          /* printer: not selected */
        AH = 0x00;
        break;
    case 0x19:
        fatal("the program restarted the computer");
    case 0x1A:
        int1a();
        break;
    case 0x20:
        int20();
        break;
    case 0x21:
        int21();
        break;
    case 0x23:                          /* Ctrl-C: terminate */
        dos_terminate(0, 1);
        break;
    case 0x24:                          /* critical error: fail the call */
        AL = 3;
        break;
    case 0x25:                          /* absolute disk read/write: return */
    case 0x26:                          /* with RETF, the flags left pushed */
        cpu.ip = cpu_pop16();
        CS = cpu_pop16();
        AX = 0x0802;                    /* sector not found */
        set_cf(1);
        return;
    case 0x27:                          /* terminate and stay resident */
        dos_keep((uint16_t)((DX + 15) >> 4), 0);
        break;
    case 0x28:                          /* DOS idle */
        break;
    case 0x29:                          /* fast console output */
        con_putc(AL);
        break;
    case 0x2A:                          /* no network */
        AH = 0;
        break;
    case 0x2F:
        int2f();
        break;
    case 0x33:                          /* no mouse driver */
        if (AX == 0x0000 || AX == 0x0021)
            AX = BX = 0;
        break;
    case 0x67:                          /* no EMS */
        AH = 0x84;
        break;
    case CB_CPM:
        dos_cpm_call();
        return;
    case CB_CASEMAP:
        AL = cp437_upper(AL);
        return;
    default:
        if (id < 8 || (id >= 0x0A && id <= 0x0F) || (id >= 0x70 && id <= 0x77) ||
            id == 0x1B || id == 0x1C || id == 0x22 || id == 0x05 || id == 0x34 ||
            (id >= 0x35 && id <= 0x3F))
            break;                      /* nothing to do: IRET */
        unhandled(id);
        break;
    }

    if (SS == ss && SP == sp && CS == cs && cpu.ip == ip) {
        uint32_t frame = ADDR(ss, sp + 4);
        uint16_t f = rw(frame);
        ww(frame, (uint16_t)((f & ~(F_CF | F_ZF)) | (cpu.flags & (F_CF | F_ZF))));
    }
}

/* ---- ROM and BIOS data area ---- */

static void stub(int id, const uint8_t *code, int n)
{
    memcpy(mem + 0xF0000 + CALLBACK_OFF(id), code, (size_t)n);
}

void bios_init(void)
{
    memset(mem, 0, MEM_SIZE + 4);
    memset(mem + 0xF0000, 0xCF, 0x10000);   /* stray jumps into the ROM return */

    for (int n = 0; n < 256; n++) {
        uint8_t code[8] = { 0x0F, 0xFF, (uint8_t)n, 0x00, 0xCF, 0x90, 0x90, 0x90 };
        if (n == 0x08) {                /* the timer tick then calls INT 1Ch */
            code[4] = 0xCD;
            code[5] = 0x1C;
            code[6] = 0xCF;
        }
        stub(n, code, 8);
        /* As on a PC, the vectors no one has claimed (60h-66h for user
         * programs, and 78h up) are zero: DOS extenders look for free
         * vectors as ones that are equal. */
        if ((n >= 0x60 && n <= 0x66) || n >= 0x78)
            continue;
        ww((uint32_t)n * 4, (uint16_t)CALLBACK_OFF(n));
        ww((uint32_t)n * 4 + 2, BIOS_SEG);
    }
    static const uint8_t cpm[] = { 0x0F, 0xFF, CB_CPM & 0xFF, CB_CPM >> 8 };
    static const uint8_t iret[] = { 0xCF };
    static const uint8_t casemap[] = { 0x0F, 0xFF, CB_CASEMAP & 0xFF, CB_CASEMAP >> 8, 0xCB };
    stub(CB_CPM, cpm, sizeof(cpm));
    stub(CB_IRET, iret, sizeof(iret));
    stub(CB_CASEMAP, casemap, sizeof(casemap));

    /* Reset vector (into INT 19h: restarting ends the emulation), BIOS
     * date and the AT model byte. */
    static const uint8_t reset[] = { 0xEA, 0x00, 0x00, 0x00, 0xF0 };
    memcpy(mem + 0xFFFF0, reset, sizeof(reset));
    mem[0xFFFF1] = (uint8_t)CALLBACK_OFF(0x19);
    mem[0xFFFF2] = (uint8_t)(CALLBACK_OFF(0x19) >> 8);
    memcpy(mem + 0xFFFF5, "01/01/92", 8);
    mem[0xFFFFE] = 0xFC;
    /* INT 15h C0h configuration table: AT, second PIC, RTC, INT 15h/4Fh. */
    static const uint8_t config[] = { 0x08, 0x00, 0xFC, 0x01, 0x00, 0x70, 0x00, 0x00, 0x00, 0x00 };
    memcpy(mem + 0xFE6F5, config, sizeof(config));
    /* The 8x16 font, for INT 10h AX=1130h. */
    memset(mem + 0xF0000 + FONT_OFF, 0, 256 * 16);
    for (int c = FONT_FIRST; c < FONT_FIRST + FONT_GLYPHS; c++)
        memcpy(mem + 0xF0000 + FONT_OFF + c * 16, font8x16_glyph((unsigned char)c), 16);

    /* BIOS data area. */
    ww(0x410, 0x0021);                  /* floppy drive, 80x25 colour, no FPU */
    ww(0x413, 640);
    mem[0x417] = 0x20;                  /* Num Lock on */
    ww(0x41A, 0x1E);
    ww(0x41C, 0x1E);
    ww(0x480, 0x1E);
    ww(0x482, 0x3E);
    mem[0x475] = 1;                     /* hard disks */
    mem[0x496] = 0x10;                  /* 101-key keyboard */

    time_t now = time(NULL);
    uint32_t secs = (uint32_t)(now % 86400);
    wd(0x46C, secs * 18 + secs * 13532 / 65536);    /* 18.2065 ticks a second */

    for (int i = 0; i < 3; i++) {
        pit[i].reload = 65536;
        pit[i].access = 3;
    }
    timer_last_ms = now_ms();
    dpmi_rom_init();
}

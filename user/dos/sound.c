/*
 * Sound for DOS programs: the PC speaker, an AdLib and a Sound Blaster 16,
 * each passed on to the real thing when fluxOS has it.
 *
 *  - PC speaker: when port 61h connects PIT channel 2 to the speaker and
 *    the channel makes a square wave, /dev/speaker plays its frequency.
 *  - AdLib (388h-38Bh, and the SB16's own FM ports): register writes go to
 *    /dev/opl. The status port is emulated, with the two timers programs
 *    use to detect the chip.
 *  - Sound Blaster 16 at 220h, IRQ 5, DMA 1 and 5 (BLASTER=A220 I5 D1 H5
 *    T6): the DSP's commands for 8-bit and 16-bit DMA output (single-cycle
 *    and auto-init), the time constant and rate commands, pause and
 *    resume, and its identification; the mixer, and the 8237 DMA
 *    controllers the program points at its samples. The transfer moves
 *    at the real sample rate, a little ahead of what is heard: samples
 *    are fetched from emulated memory as their time comes, written to
 *    /dev/dsp, and the DMA counters move as they would on a PC, with
 *    IRQ 5 at the end of each block. Recording gives no data, and
 *    neither ADPCM nor the direct DAC (command 10h) make a sound.
 *
 * The devices are opened when a program first uses them, so a program
 * that never makes a sound leaves them free for others.
 */
#include <fcntl.h>
#include <flux.h>
#include <string.h>
#include <sys/soundcard.h>
#include <sys/stat.h>
#include <unistd.h>
#include "dos.h"

#define SB_BASE   0x220
#define SB_IRQ    5
#define SB_DMA8   1
#define SB_DMA16  5
#define LEAD_MS   60                    /* how far the transfer runs ahead of the clock */
#define STEPS_PER_US 10                 /* instructions in a microsecond, as the PIT assumes */

/* ---- PC speaker ---- */

static int spkr_fd = -1;
static uint32_t spkr_freq;

void speaker_update(int port61, uint32_t reload, int mode)
{
    uint32_t freq = 0;
    if ((port61 & 3) == 3 && (mode == 3 || mode == 7 || mode == 2 || mode == 6) && reload > 1)
        freq = 1193182u / reload;
    if (freq == spkr_freq)
        return;
    spkr_freq = freq;
    if (spkr_fd < 0)
        spkr_fd = open("/dev/speaker", O_WRONLY);
    if (spkr_fd >= 0)
        ioctl(spkr_fd, SPKR_SET, freq);
}

/* ---- the 8237 DMA controllers ---- */

struct dma_channel {
    uint16_t base_addr, base_count;     /* what was programmed */
    uint16_t addr, count;               /* where the transfer is */
    uint8_t page, mode;
    int masked;
};

static struct dma_channel dma[8];
static int flipflop[2];
static uint8_t dma_tc;                  /* terminal count flags, channels 0-3 */
static uint8_t dma_tc16;

/* Page registers 81h-8Fh: which channel each belongs to, or -1. */
static int page_channel(uint16_t port)
{
    switch (port) {
    case 0x87: return 0;
    case 0x83: return 1;
    case 0x81: return 2;
    case 0x82: return 3;
    case 0x8F: return 4;
    case 0x8B: return 5;
    case 0x89: return 6;
    case 0x8A: return 7;
    }
    return -1;
}

static int dma_in(uint16_t port, uint8_t *v)
{
    int ch = -1, hi, wide = 0, reg;
    if (port < 0x10) {
        reg = port;
    } else if (port >= 0xC0 && port < 0xE0) {
        reg = (port - 0xC0) >> 1;
        wide = 1;
    } else if (port >= 0x81 && port <= 0x8F) {
        int c = page_channel(port);
        *v = c >= 0 ? dma[c].page : 0;
        return 1;
    } else {
        return 0;
    }
    if (reg < 8) {
        ch = (reg >> 1) + (wide ? 4 : 0);
        hi = flipflop[wide];
        flipflop[wide] ^= 1;
        uint16_t val = (reg & 1) ? dma[ch].count : dma[ch].addr;
        *v = (uint8_t)(hi ? val >> 8 : val);
        return 1;
    }
    if (reg == 8) {                     /* status: terminal counts, reading clears them */
        *v = wide ? dma_tc16 : dma_tc;
        if (wide)
            dma_tc16 = 0;
        else
            dma_tc = 0;
        return 1;
    }
    *v = 0xFF;
    return 1;
}

static int dma_out(uint16_t port, uint8_t v)
{
    int wide = 0, reg;
    if (port < 0x10) {
        reg = port;
    } else if (port >= 0xC0 && port < 0xE0) {
        reg = (port - 0xC0) >> 1;
        wide = 1;
    } else if (port >= 0x81 && port <= 0x8F) {
        int c = page_channel(port);
        if (c >= 0)
            dma[c].page = v;
        return 1;
    } else {
        return 0;
    }
    int first = wide ? 4 : 0;
    if (reg < 8) {
        struct dma_channel *d = &dma[(reg >> 1) + first];
        int hi = flipflop[wide];
        flipflop[wide] ^= 1;
        uint16_t *base = (reg & 1) ? &d->base_count : &d->base_addr;
        *base = hi ? (uint16_t)((*base & 0x00FF) | v << 8) : (uint16_t)((*base & 0xFF00) | v);
        if (reg & 1)
            d->count = d->base_count;
        else
            d->addr = d->base_addr;
        return 1;
    }
    switch (reg) {
    case 0x0A:                          /* single mask */
        dma[first + (v & 3)].masked = (v >> 2) & 1;
        break;
    case 0x0B:                          /* mode */
        dma[first + (v & 3)].mode = v;
        break;
    case 0x0C:                          /* clear the flip-flop */
        flipflop[wide] = 0;
        break;
    case 0x0D:                          /* master clear */
        flipflop[wide] = 0;
        for (int i = 0; i < 4; i++)
            dma[first + i].masked = 1;
        break;
    case 0x0E:                          /* clear all masks */
        for (int i = 0; i < 4; i++)
            dma[first + i].masked = 0;
        break;
    case 0x0F:                          /* write all masks */
        for (int i = 0; i < 4; i++)
            dma[first + i].masked = (v >> i) & 1;
        break;
    }
    return 1;
}

/* Move up to n bytes from memory through channel ch, as the card reads
 * them. A 16-bit channel moves words. Stops at a single transfer's end. */
static int dma_fetch(int ch, uint8_t *buf, int n)
{
    struct dma_channel *d = &dma[ch];
    int wide = ch >= 4, got = 0;
    while (got + (wide ? 2 : 1) <= n && !d->masked) {
        if (wide) {
            uint32_t a = ((uint32_t)(d->page & 0xFE) << 16) | (uint32_t)d->addr << 1;
            if (buf) {
                buf[got] = rb(a);
                buf[got + 1] = rb(a + 1);
            }
            got += 2;
        } else {
            uint32_t a = (uint32_t)d->page << 16 | d->addr;
            if (buf)
                buf[got] = rb(a);
            got++;
        }
        d->addr = (d->mode & 0x20) ? (uint16_t)(d->addr - 1) : (uint16_t)(d->addr + 1);
        if (d->count-- == 0) {          /* terminal count */
            if (wide)
                dma_tc16 |= (uint8_t)(1 << (ch & 3));
            else
                dma_tc |= (uint8_t)(1 << ch);
            if (d->mode & 0x10) {       /* auto-init */
                d->addr = d->base_addr;
                d->count = d->base_count;
            } else {
                d->masked = 1;
            }
        }
    }
    return got;
}

/* ---- AdLib ---- */

static struct {
    int avail, opl3;
    int fd;
    uint8_t index[2];
    uint8_t t1, t2, ctrl, flags;
    uint32_t t1_steps, t2_steps, t1_ms, t2_ms;     /* when each timer started */
    struct opl_write q[256];
    int nq;
} opl = { .fd = -1 };

static void opl_flush(void)
{
    if (opl.nq && opl.fd >= 0)
        write(opl.fd, opl.q, (unsigned int)opl.nq * sizeof(opl.q[0]));
    opl.nq = 0;
}

static int timer_done(uint32_t steps0, uint32_t ms0, uint32_t us)
{
    return cpu.steps - steps0 >= us * STEPS_PER_US || (now_ms() - ms0) * 1000 >= us;
}

static uint8_t opl_status(void)
{
    if (!opl.avail)
        return 0xFF;
    if ((opl.ctrl & 1) && !(opl.ctrl & 0x40) && timer_done(opl.t1_steps, opl.t1_ms, (256u - opl.t1) * 80))
        opl.flags |= 0x40;
    if ((opl.ctrl & 2) && !(opl.ctrl & 0x20) && timer_done(opl.t2_steps, opl.t2_ms, (256u - opl.t2) * 320))
        opl.flags |= 0x20;
    uint8_t s = opl.flags;
    if (s & 0x60)
        s |= 0x80;
    return (uint8_t)(s | (opl.opl3 ? 0 : 0x06));
}

static void opl_data(int bank, uint8_t v)
{
    if (!opl.avail)
        return;
    uint8_t reg = opl.index[bank];
    if (bank == 0 && reg >= 2 && reg <= 4) {    /* the timers stay here */
        if (reg == 2) {
            opl.t1 = v;
        } else if (reg == 3) {
            opl.t2 = v;
        } else if (v & 0x80) {
            opl.flags = 0;              /* reset the IRQ flags */
        } else {
            if ((v & 1) && !(opl.ctrl & 1)) {
                opl.t1_steps = cpu.steps;
                opl.t1_ms = now_ms();
            }
            if ((v & 2) && !(opl.ctrl & 2)) {
                opl.t2_steps = cpu.steps;
                opl.t2_ms = now_ms();
            }
            opl.ctrl = v;
        }
        return;
    }
    if (opl.fd < 0) {
        opl.fd = open("/dev/opl", O_WRONLY);
        if (opl.fd < 0) {
            dlog("sound: cannot open /dev/opl, the AdLib is silent\n");
            opl.avail = 0;
            return;
        }
    }
    if (opl.nq == (int)(sizeof(opl.q) / sizeof(opl.q[0])))
        opl_flush();
    opl.q[opl.nq].reg = (uint16_t)(reg | bank << 8);
    opl.q[opl.nq].val = v;
    opl.q[opl.nq].reserved = 0;
    opl.nq++;
}

/* ---- Sound Blaster 16 ---- */

static struct {
    int avail;
    int fd;
    int reset_high;                     /* 1 written to the reset port */
    uint8_t out[64];                    /* bytes for the program to read */
    int nout, outpos;
    uint8_t last_read;
    uint8_t cmd;
    int need, have;
    uint8_t args[4];
    uint8_t test_reg;
    int speaker;
    int tc_rate, rate;                  /* time constant rate (0 none), command 41h rate */
    uint32_t block;                     /* command 48h: bytes per block, less one */
    uint8_t mixer_index, mixer[256];
    uint8_t irq_status;                 /* bit 0: 8-bit IRQ pending, bit 1: 16-bit */
    int irq_line;                       /* set through mixer register 80h; 0: not set */

    /* the transfer */
    int active, paused, autoinit, bits16, stereo, is_signed, silence, input;
    int exit_after_block;
    int xrate;                          /* per channel */
    uint32_t block_bytes, block_left;
    uint32_t last_ms, acc;              /* pacing: bytes owed, in thousandths */
    int out_rate, out_channels, out_fmt;
} sb = { .fd = -1 };

static void sb_queue(uint8_t v)
{
    if (sb.nout < (int)sizeof(sb.out))
        sb.out[sb.nout++] = v;
}

static void mixer_reset(void)
{
    memset(sb.mixer, 0, sizeof(sb.mixer));
    sb.mixer[0x22] = sb.mixer[0x04] = sb.mixer[0x26] = 0xCC;     /* SB Pro volumes */
    sb.mixer[0x30] = sb.mixer[0x31] = sb.mixer[0x32] = sb.mixer[0x33] = 0xC0;
    sb.mixer[0x34] = sb.mixer[0x35] = 0xC0;
    sb.mixer[0x41] = sb.mixer[0x42] = 0;
    sb.mixer[0x44] = sb.mixer[0x45] = sb.mixer[0x46] = sb.mixer[0x47] = 0x80;
}

static void dsp_reset(void)
{
    sb.active = 0;
    sb.nout = sb.outpos = 0;
    sb.need = sb.have = 0;
    sb.cmd = 0;
    sb.irq_status = 0;
    sb.tc_rate = 0;
    sb.rate = 22050;
    sb.block = 0x7FF;
    sb.speaker = 0;
    sb_queue(0xAA);
    if (sb.fd >= 0)
        ioctl(sb.fd, SNDCTL_DSP_RESET);
}

/* Bytes of arguments each command takes. */
static int arg_count(uint8_t c)
{
    if ((c & 0xF0) == 0xB0 || (c & 0xF0) == 0xC0)
        return 3;
    switch (c) {
    case 0x10: case 0x38: case 0x40: case 0xE0: case 0xE2: case 0xE4:
        return 1;
    case 0x14: case 0x16: case 0x17: case 0x24: case 0x41: case 0x42: case 0x48: case 0x74:
    case 0x75: case 0x76: case 0x77: case 0x80:
        return 2;
    }
    return 0;
}

static int bytes_per_second(void)
{
    return sb.xrate * (sb.stereo ? 2 : 1) * (sb.bits16 ? 2 : 1);
}

/* Set /dev/dsp to the transfer's format. */
static int open_dsp(void)
{
    if (sb.fd < 0) {
        sb.fd = open("/dev/dsp", O_WRONLY);
        if (sb.fd < 0) {
            dlog("sound: cannot open /dev/dsp, the Sound Blaster is silent\n");
            return -1;
        }
        sb.out_rate = sb.out_channels = sb.out_fmt = 0;
    }
    int fmt = sb.bits16 ? AFMT_S16_LE : AFMT_U8, ch = sb.stereo ? 2 : 1, rate = sb.xrate;
    if (fmt != sb.out_fmt || ch != sb.out_channels || rate != sb.out_rate) {
        ioctl(sb.fd, SNDCTL_DSP_SETFMT, &fmt);
        ioctl(sb.fd, SNDCTL_DSP_CHANNELS, &ch);
        ioctl(sb.fd, SNDCTL_DSP_SPEED, &rate);
        sb.out_fmt = sb.bits16 ? AFMT_S16_LE : AFMT_U8;
        sb.out_channels = sb.stereo ? 2 : 1;
        sb.out_rate = sb.xrate;
    }
    return 0;
}

static void start_transfer(int bits16, int autoinit, uint32_t bytes, int stereo, int is_signed)
{
    sb.bits16 = bits16;
    sb.autoinit = autoinit;
    sb.stereo = stereo;
    sb.is_signed = is_signed;
    sb.silence = sb.input = 0;
    sb.exit_after_block = 0;
    sb.paused = 0;
    sb.xrate = sb.rate;
    if (sb.tc_rate && !bits16 && sb.cmd < 0xB0) {
        /* Commands before the SB16 use the time constant, which for an SB Pro in
         * stereo (mixer register 0Eh, bit 1) counts both channels. */
        sb.stereo = (sb.mixer[0x0E] & 2) != 0;
        sb.xrate = sb.stereo ? sb.tc_rate / 2 : sb.tc_rate;
    }
    if (sb.xrate < 4000)
        sb.xrate = 4000;
    if (sb.xrate > 48000)
        sb.xrate = 48000;
    sb.block_bytes = sb.block_left = bytes ? bytes : 1;
    sb.active = 1;
    sb.last_ms = now_ms();
    sb.acc = (uint32_t)bytes_per_second() * LEAD_MS;   /* start a little ahead */
    open_dsp();
}

/* The IRQ line: the one chosen in mixer register 80h, or else the one the
 * program hooked (its setup may say 7 while the card is at 5). */
static int irq_line(void)
{
    if (sb.irq_line)
        return sb.irq_line;
    if (!irq_hooked(SB_IRQ)) {
        if (irq_hooked(7))
            return 7;
        if (irq_hooked(2))
            return 2;
    }
    return SB_IRQ;
}

static void raise_irq(int bits16)
{
    sb.irq_status |= bits16 ? 2 : 1;
    pic_raise(irq_line());
}

static void dsp_command(void)
{
    uint8_t c = sb.cmd, *a = sb.args;
    uint32_t len = (uint32_t)(a[0] | a[1] << 8) + 1;
    if ((c & 0xF0) == 0xB0 || (c & 0xF0) == 0xC0) {
        /* SB16 DMA: bit 3 input, bit 2 auto-init; mode: bit 4 signed, bit 5 stereo */
        int wide = (c & 0xF0) == 0xB0;
        uint32_t samples = (uint32_t)(a[1] | a[2] << 8) + 1;
        start_transfer(wide, (c & 4) != 0, samples * (wide ? 2 : 1), (a[0] & 0x20) != 0, (a[0] & 0x10) != 0);
        sb.input = (c & 8) != 0;
        return;
    }
    switch (c) {
    case 0x10:                          /* direct DAC: one sample, not played */
        break;
    case 0x14: case 0x91:               /* 8-bit single-cycle output */
    case 0x24: case 0x99:               /* ... and input */
        start_transfer(0, 0, c == 0x91 || c == 0x99 ? sb.block + 1 : len, 0, 0);
        sb.input = c == 0x24 || c == 0x99;
        break;
    case 0x1C: case 0x90:               /* 8-bit auto-init output */
    case 0x2C: case 0x98:               /* ... and input */
        start_transfer(0, 1, sb.block + 1, 0, 0);
        sb.input = c == 0x2C || c == 0x98;
        break;
    case 0x20:                          /* direct ADC: silence */
        sb_queue(0x80);
        break;
    case 0x40:                          /* time constant */
        sb.tc_rate = 1000000 / (256 - a[0]);
        break;
    case 0x41: case 0x42:               /* output and input rate */
        sb.rate = a[0] << 8 | a[1];
        sb.tc_rate = 0;
        break;
    case 0x48:
        sb.block = (uint32_t)(a[0] | a[1] << 8);
        break;
    case 0x80:                          /* silence for len samples, then the IRQ */
        start_transfer(0, 0, len, 0, 0);
        sb.silence = 1;
        break;
    case 0xD0: case 0xD5:               /* pause */
        sb.paused = 1;
        break;
    case 0xD4: case 0xD6: case 0x45: case 0x47:     /* continue */
        sb.paused = 0;
        sb.last_ms = now_ms();
        break;
    case 0xD1:
        sb.speaker = 1;
        break;
    case 0xD3:
        sb.speaker = 0;
        break;
    case 0xD8:
        sb_queue(sb.speaker ? 0xFF : 0x00);
        break;
    case 0xD9: case 0xDA:               /* leave auto-init after this block */
        sb.exit_after_block = 1;
        break;
    case 0xE0:
        sb_queue((uint8_t)~a[0]);
        break;
    case 0xE1:
        sb_queue(4);                    /* DSP 4.05: a Sound Blaster 16 */
        sb_queue(5);
        break;
    case 0xE3: {
        static const char copyright[] = "COPYRIGHT (C) CREATIVE TECHNOLOGY LTD, 1992.";
        for (const char *p = copyright; *p; p++)
            sb_queue((uint8_t)*p);
        sb_queue(0);
        break;
    }
    case 0xE4:
        sb.test_reg = a[0];
        break;
    case 0xE8:
        sb_queue(sb.test_reg);
        break;
    case 0xF2:
        raise_irq(0);
        break;
    case 0xF3:
        raise_irq(1);
        break;
    case 0xF8:
        sb_queue(0);
        break;
    default:
        dlog("sound: unsupported DSP command %02Xh\n", c);
        break;
    }
}

static void dsp_write(uint8_t v)
{
    if (sb.need > sb.have) {
        sb.args[sb.have++] = v;
        if (sb.have == sb.need) {
            sb.need = sb.have = 0;
            dsp_command();
        }
        return;
    }
    sb.cmd = v;
    sb.need = arg_count(v);
    sb.have = 0;
    if (!sb.need)
        dsp_command();
}

/* Write fetched samples to /dev/dsp, as much as fits without waiting. */
static void play_bytes(uint8_t *buf, int n)
{
    if (sb.input || sb.silence || n <= 0 || sb.fd < 0)
        return;
    if (sb.bits16 && !sb.is_signed)
        for (int i = 1; i < n; i += 2)
            buf[i] ^= 0x80;             /* unsigned 16-bit to signed */
    if (!sb.bits16 && sb.is_signed)
        for (int i = 0; i < n; i++)
            buf[i] ^= 0x80;             /* signed 8-bit to unsigned */
    struct audio_buf_info bi;
    if (ioctl(sb.fd, SNDCTL_DSP_GETOSPACE, &bi) == 0 && bi.bytes < n)
        n = bi.bytes & ~3;              /* the card fell behind: drop the excess */
    if (n > 0)
        write(sb.fd, buf, (unsigned int)n);
}

/* Advance the transfer to the present. */
static void sb_poll(void)
{
    if (!sb.active || sb.paused)
        return;
    uint32_t now = now_ms(), elapsed = now - sb.last_ms;
    sb.last_ms = now;
    if (elapsed > 200)
        elapsed = 200;
    sb.acc += elapsed * (uint32_t)bytes_per_second();
    uint32_t owed = sb.acc / 1000;
    if (sb.bits16)
        owed &= ~1u;
    sb.acc -= owed * 1000;
    static uint8_t buf[8192];
    while (owed && sb.active) {
        uint32_t n = owed < sb.block_left ? owed : sb.block_left;
        if (n > sizeof(buf))
            n = sizeof(buf);
        int got;
        if (sb.silence) {
            got = (int)n;
        } else {
            got = dma_fetch(sb.bits16 ? SB_DMA16 : SB_DMA8, sb.input ? NULL : buf, (int)n);
            if (got <= 0)
                break;                  /* the program has not unmasked the channel */
        }
        play_bytes(buf, got);
        owed -= (uint32_t)got;
        sb.block_left -= (uint32_t)got;
        if (!sb.block_left) {
            raise_irq(sb.bits16);
            if (sb.autoinit && !sb.exit_after_block) {
                sb.block_left = sb.block_bytes;
            } else {
                sb.active = 0;
                if (sb.fd >= 0)
                    ioctl(sb.fd, SNDCTL_DSP_POST);  /* play what is left */
            }
        }
    }
}

/* ---- ports ---- */

int sound_io_in(uint16_t port, uint8_t *v)
{
    if (dma_in(port, v))
        return 1;
    if (port >= 0x388 && port <= 0x38B) {
        *v = (port & 1) ? 0xFF : opl_status();
        return 1;
    }
    if (!sb.avail || port < SB_BASE || port >= SB_BASE + 0x10)
        return 0;
    switch (port - SB_BASE) {
    case 0x0: case 0x2: case 0x8:       /* FM status */
        *v = opl_status();
        return 1;
    case 0x5:
        if (sb.mixer_index == 0x80) {
            int irq = irq_line();
            *v = irq == 2 ? 0x01 : irq == 7 ? 0x04 : 0x02;
        }
        else if (sb.mixer_index == 0x81)
            *v = (uint8_t)(1 << SB_DMA8 | 1 << SB_DMA16);
        else if (sb.mixer_index == 0x82)
            *v = sb.irq_status;
        else
            *v = sb.mixer[sb.mixer_index];
        return 1;
    case 0x4:
        *v = sb.mixer_index;
        return 1;
    case 0xA:
        if (sb.outpos < sb.nout) {
            sb.last_read = sb.out[sb.outpos++];
            if (sb.outpos == sb.nout)
                sb.nout = sb.outpos = 0;
        }
        *v = sb.last_read;
        return 1;
    case 0xC:
        *v = 0x7F;                      /* always ready for a command */
        return 1;
    case 0xE:
        sb.irq_status &= ~1;            /* acknowledges an 8-bit IRQ */
        *v = (uint8_t)(sb.outpos < sb.nout ? 0xFF : 0x7F);
        return 1;
    case 0xF:
        sb.irq_status &= ~2;            /* acknowledges a 16-bit IRQ */
        *v = 0xFF;
        return 1;
    }
    *v = 0xFF;
    return 1;
}

int sound_io_out(uint16_t port, uint8_t v)
{
    if (dma_out(port, v))
        return 1;
    if (port >= 0x388 && port <= 0x38B) {
        int bank = (port >> 1) & 1;
        if (port & 1)
            opl_data(bank, v);
        else
            opl.index[bank] = v;
        return 1;
    }
    if (!sb.avail || port < SB_BASE || port >= SB_BASE + 0x10)
        return 0;
    switch (port - SB_BASE) {
    case 0x0: case 0x8:
        opl.index[0] = v;
        return 1;
    case 0x1: case 0x9:
        opl_data(0, v);
        return 1;
    case 0x2:
        opl.index[1] = v;
        return 1;
    case 0x3:
        opl_data(1, v);
        return 1;
    case 0x4:
        sb.mixer_index = v;
        return 1;
    case 0x5:
        if (sb.mixer_index == 0x00)
            mixer_reset();
        else if (sb.mixer_index < 0x80)
            sb.mixer[sb.mixer_index] = v;
        else if (sb.mixer_index == 0x80)    /* IRQ select: 2, 5 or 7 (10 is not wired) */
            sb.irq_line = v & 0x01 ? 2 : v & 0x04 ? 7 : v & 0x02 ? 5 : 0;
        return 1;
    case 0x6:
        if (v & 1)
            sb.reset_high = 1;
        else if (sb.reset_high) {
            sb.reset_high = 0;
            dsp_reset();
        }
        return 1;
    case 0xC:
        dsp_write(v);
        return 1;
    }
    return 1;
}

/* ---- the emulator's side ---- */

void sound_init(void)
{
    struct stat st;
    sb.avail = stat("/dev/dsp", &st) == 0;
    opl.avail = stat("/dev/opl", &st) == 0;
    if (opl.avail) {
        int fd = open("/dev/opl", O_WRONLY);
        struct sound_info si;
        memset(&si, 0, sizeof(si));
        if (fd >= 0) {
            ioctl(fd, SOUND_INFO, &si);
            close(fd);
        }
        opl.opl3 = (si.flags & SOUNDF_OPL3) != 0;
    }
    for (int i = 0; i < 8; i++)
        dma[i].masked = 1;
    mixer_reset();
    sb.rate = 22050;
    sb.block = 0x7FF;
    sb.nout = 0;
}

void sound_poll(void)
{
    sb_poll();
    opl_flush();
}

int sound_active(void)
{
    return sb.active && !sb.paused;
}

const char *sound_blaster_env(void)
{
    return sb.avail ? "BLASTER=A220 I5 D1 H5 T6" : NULL;
}

void sound_shutdown(void)
{
    opl_flush();
    if (opl.fd >= 0)
        close(opl.fd);                  /* the driver silences the voices */
    opl.fd = -1;
    if (sb.fd >= 0)
        close(sb.fd);
    sb.fd = -1;
    if (spkr_fd >= 0) {
        ioctl(spkr_fd, SPKR_SET, 0);
        close(spkr_fd);
    }
    spkr_fd = -1;
    spkr_freq = 0;
}

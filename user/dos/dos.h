/*
 * dos: runs DOS programs (.COM files and MZ .EXE files) on fluxOS.
 *
 * The program runs on an emulated PC: an x86 CPU (the 8086 instruction
 * set with the 186, 286 and 386 additions, and no FPU), one MiB of
 * memory, a BIOS (video, keyboard, timer, equipment) and DOS 5 (files,
 * memory, processes, console). A DPMI host (dpmi.c) adds protected mode
 * and extended memory for DOS extenders such as DOS/4GW. Each
 * interrupt vector initially points to a stub in the emulated ROM that
 * calls back into C, so programs can hook and chain interrupts as on a
 * real PC.
 *
 * Files: drive C: is the fluxOS root, A: and B: the floppy mount points
 * and D: the CD-ROM. Text the program prints goes to the terminal as it
 * would from a fluxOS program; once it draws on the screen (writing
 * video memory or moving the cursor), the terminal shows its 80x25
 * text screen instead. In the desktop's terminal, VGA graphics (mode 13h
 * and its unchained "mode X" form) are shown too.
 */
#ifndef DOS_H
#define DOS_H

#include <stdint.h>

/* ---- memory ---- */

/*
 * mem is the first MiB, until a protected-mode program asks for more:
 * then it is reallocated with extended memory after it (dpmi.c), and
 * mem_mask grows from the 1 MiB wrap of real mode (A20 off) to cover it.
 * A0000-AFFFF is VGA memory in graphics modes (video.c), and F0000-FFFFF
 * is ROM.
 */
#define MEM_SIZE 0x100000
#define ADDR(seg, off) ((((uint32_t)(seg) << 4) + (uint32_t)(off)) & 0xFFFFF)

extern uint8_t *mem;
extern uint32_t mem_mask;
extern int video_written;               /* the program wrote to text video memory */
extern int vga_graphics;                /* A0000-AFFFF is the VGA's planes */

uint8_t vga_read(uint32_t a);
void vga_write(uint32_t a, uint8_t v);

static inline uint8_t rb(uint32_t a)
{
    a &= mem_mask;
    if (a - 0xA0000 < 0x10000 && vga_graphics)
        return vga_read(a);
    return mem[a];
}

static inline void wb(uint32_t a, uint8_t v)
{
    a &= mem_mask;
    if (a - 0xA0000 < 0x60000) {
        if (a >= 0xF0000)
            return;                     /* ROM */
        if (a < 0xB0000) {
            if (vga_graphics) {
                vga_write(a, v);
                return;
            }
        } else if (a < 0xC0000) {
            video_written = 1;
        }
    }
    mem[a] = v;
}

/* Words and doublewords: in one go away from the edges and the video and
 * ROM area, byte by byte there. */
static inline int plain(uint32_t a, uint32_t n)
{
    a &= mem_mask;
    return a <= mem_mask - n && (a + n <= 0xA0000 || a >= 0x100000);
}

static inline uint16_t rw(uint32_t a)
{
    if (plain(a, 2)) {
        uint16_t v;
        __builtin_memcpy(&v, mem + (a & mem_mask), 2);
        return v;
    }
    return (uint16_t)(rb(a) | rb(a + 1) << 8);
}

static inline uint32_t rd(uint32_t a)
{
    if (plain(a, 4)) {
        uint32_t v;
        __builtin_memcpy(&v, mem + (a & mem_mask), 4);
        return v;
    }
    return rw(a) | (uint32_t)rw(a + 2) << 16;
}

static inline void ww(uint32_t a, uint16_t v)
{
    if (plain(a, 2)) {
        __builtin_memcpy(mem + (a & mem_mask), &v, 2);
        return;
    }
    wb(a, (uint8_t)v);
    wb(a + 1, (uint8_t)(v >> 8));
}

static inline void wd(uint32_t a, uint32_t v)
{
    if (plain(a, 4)) {
        __builtin_memcpy(mem + (a & mem_mask), &v, 4);
        return;
    }
    ww(a, (uint16_t)v);
    ww(a + 2, (uint16_t)(v >> 16));
}

/* ---- CPU ---- */

enum { R_AX, R_CX, R_DX, R_BX, R_SP, R_BP, R_SI, R_DI };
enum { S_ES, S_CS, S_SS, S_DS, S_FS, S_GS };

#define F_CF 0x0001
#define F_PF 0x0004
#define F_AF 0x0010
#define F_ZF 0x0040
#define F_SF 0x0080
#define F_TF 0x0100
#define F_IF 0x0200
#define F_DF 0x0400
#define F_OF 0x0800

struct cpu {
    uint32_t r[8];
    uint16_t seg[6];                    /* selectors in protected mode */
    uint32_t base[6];                   /* protected mode: the descriptors' bases */
    uint32_t ip;
    uint32_t flags;
    int pm;                             /* protected mode (a DPMI client runs) */
    int code32;                         /* CS is a 32-bit segment */
    int stack32;                        /* SS is a 32-bit segment: ESP, not SP */
    uint32_t ip_mask;                   /* 0xFFFF, or all ones with code32 */
    uint32_t ip_start;                  /* the instruction being executed */
    int inhibit;                        /* no interrupt before the next instruction */
    int stop;                           /* leave cpu_run() */
    int halted;                         /* HLT: wait for an interrupt */
    uint32_t steps;                     /* instructions executed */
};

extern struct cpu cpu;

#define REG16(i) (*(uint16_t *)&cpu.r[i])
#define REG8L(i) (*(uint8_t *)&cpu.r[i])
#define REG8H(i) (((uint8_t *)&cpu.r[i])[1])

#define AX REG16(R_AX)
#define BX REG16(R_BX)
#define CX REG16(R_CX)
#define DX REG16(R_DX)
#define SP REG16(R_SP)
#define BP REG16(R_BP)
#define SI REG16(R_SI)
#define DI REG16(R_DI)
#define AL REG8L(R_AX)
#define AH REG8H(R_AX)
#define BL REG8L(R_BX)
#define BH REG8H(R_BX)
#define CL REG8L(R_CX)
#define CH REG8H(R_CX)
#define DL REG8L(R_DX)
#define DH REG8H(R_DX)
#define ES cpu.seg[S_ES]
#define CS cpu.seg[S_CS]
#define SS cpu.seg[S_SS]
#define DS cpu.seg[S_DS]

void cpu_reset(void);
void cpu_run(uint32_t count);
void cpu_interrupt(int n);              /* enter interrupt n as INT n does */
void cpu_push16(uint16_t v);
uint16_t cpu_pop16(void);
void cpu_push32(uint32_t v);
uint32_t cpu_pop32(void);
uint32_t cpu_sp(void);                  /* SP, or ESP on a 32-bit stack */
void cpu_set_sp(uint32_t v);
/* Load segment register s: a paragraph in real mode, a selector in
 * protected mode (from the DPMI host's descriptors). */
void cpu_load_seg(int s, uint16_t sel);
void cpu_real_mode(void);               /* leave protected mode */
static inline uint32_t seg_base(int s)
{
    return cpu.pm ? cpu.base[s] : (uint32_t)cpu.seg[s] << 4;
}

static inline void set_cf(int on)
{
    cpu.flags = on ? cpu.flags | F_CF : cpu.flags & ~F_CF;
}

static inline void set_zf(int on)
{
    cpu.flags = on ? cpu.flags | F_ZF : cpu.flags & ~F_ZF;
}

/* ---- the emulated ROM ---- */

#define BIOS_SEG   0xF000
#define STUB_BASE  0x1000               /* vector n's stub: F000:STUB_BASE + 8n */
#define STUB_SIZE  8
#define CB_CPM     0x100                /* callbacks beyond the vectors */
#define CB_IRET    0x101                /* (just an IRET) */
#define CB_CASEMAP 0x102                /* country info case map routine */
#define CB_DPMI    0x110                /* 0x110-0x1FF: the DPMI host's real-mode entries */
#define CB_DPMI_END 0x200
#define CALLBACK_OFF(id) (STUB_BASE + (id) * STUB_SIZE)
#define FONT_OFF   0xA000               /* the 8x16 font at F000:A000 */

void bios_init(void);
void bios_callback(int id);             /* the 0F FF opcode */
void blocked(void);                     /* re-run the current callback later */
uint32_t io_in(uint16_t port, int size);
void io_out(uint16_t port, uint32_t v, int size);
extern uint8_t pic_irr;                 /* IRQs waiting to be delivered */
void pic_raise(int irq);
int pic_pending(void);                  /* an IRQ to deliver now, or -1 */
void pic_ack(int irq);
int irq_busy(int irq);                  /* requested or in service */
int irq_masked(int irq);
int irq_hooked(int irq);                /* the program has its own handler for the IRQ */
void timer_poll(void);
uint32_t bios_ticks(void);

/* ---- DPMI (dpmi.c) ---- */

/* A protected-mode code segment for the DPMI host's own stubs lies in the
 * ROM at F000:DPMI_STUBS: its callback ids are DPMI_CB_PM and up. */
#define DPMI_STUBS  0xC000
#define DPMI_CB_PM  0x400

void dpmi_callback(int id);             /* the 0F FF opcode, for DPMI ids */
int dpmi_active(void);
void dpmi_interrupt(int n, int hardware);   /* INT n (or an IRQ) in protected mode */
int dpmi_irq_in_real_mode(int vector);  /* pass an IRQ up to a protected-mode handler */
int dpmi_hooked(int vector);            /* the client set a protected-mode handler */
void dpmi_exception(int n);             /* a fault in protected mode */
int dpmi_descriptor(uint16_t sel, uint32_t *base, uint32_t *limit, uint8_t *access, int *big);
void dpmi_terminate(uint16_t psp);      /* the program ended: drop its protected mode */
void dpmi_install_check(void);          /* INT 2Fh AX=1687h */
void dpmi_rom_init(void);               /* the host's stubs in the ROM */

/* ---- sound (sound.c) ---- */

void sound_init(void);
void sound_poll(void);                  /* move a Sound Blaster transfer on; flush FM */
int sound_active(void);                 /* a transfer is running: do not sleep long */
void sound_shutdown(void);
int sound_io_in(uint16_t port, uint8_t *v);     /* 1 if the port is a sound port */
int sound_io_out(uint16_t port, uint8_t v);
void speaker_update(int port61, uint32_t reload, int mode);     /* PIT channel 2 */
const char *sound_blaster_env(void);    /* "BLASTER=...", or NULL without a card */

/* ---- keyboard ---- */

void kbd_init(void);
void kbd_restore(void);
int kbd_poll(int timeout_ms);           /* read the terminal; returns keys queued */
int kbd_available(void);                /* keys in the BIOS buffer */
uint16_t kbd_read(void);                /* scan code << 8 | character, or 0 */
uint16_t kbd_peek(void);
void kbd_push(uint16_t key);
void kbd_irq_ack(void);                 /* INT 9 default handler */
void kbd_pump(void);                    /* deliver queued keys as IRQ 1 */
uint8_t kbd_port60(void);
uint8_t kbd_port64(void);
void kbd_idle_hint(void);               /* the program polled and found no key */
void kbd_raw(int on);                   /* key presses and releases from the terminal */
void int16(void);

/* ---- video ---- */

void video_init(void);
void video_refresh(int force);
void video_leave(void);
void video_suspend(void);               /* before running a fluxOS program */
void video_resume(void);
void video_flush(void);
void con_putc(uint8_t c);               /* console output: teletype with ANSI.SYS */
void con_puts(const char *s);
void video_crtc_write(int index, uint8_t v);
int vga_io_in(uint16_t port, uint8_t *v);       /* 1 if the port is a VGA register */
int vga_io_out(uint16_t port, uint8_t v);
int video_graphics_ok(void);            /* the terminal can show graphics */
void int10(void);
extern int screen_mode;

/* ---- DOS ---- */

/* DOS's own data, below the first memory block. */
#define DOS_SEG         0x0060
#define DOS_INDOS       0x0001          /* InDOS flag; the critical error flag precedes it */
#define DOS_LOL         0x0010          /* list of lists; the first MCB segment is at -2 */
#define DOS_CON_HDR     0x0060          /* CON device header */
#define DOS_COUNTRY     0x0080          /* country information (34 bytes) */
#define DOS_SFT         0x0100          /* an empty system file table header */
#define DOS_UPCASE      0x0110          /* word 128, then upper case of 80h-FFh */
#define DOS_TERMINATORS 0x0192          /* filename terminator table */
#define DOS_COLLATE     0x01C0          /* word 256, then the collating sequence */
#define DOS_DBCS        0x02D0          /* empty DBCS lead byte table */
#define DOS_MEDIA       0x02E0          /* media ID byte for INT 21h 1Bh/1Ch */
#define FIRST_MCB       0x0100
#define MEM_TOP         0xA000          /* 640 KiB of conventional memory */

enum { DEV_FILE, DEV_CON, DEV_NUL };

extern uint16_t dos_psp;                /* the running program */
extern int dos_last_error;
uint32_t dos_dta(void);
void dos_fail(int code);                /* CF set, AX = DOS error code */
void dos_ok(void);
int con_read(uint32_t addr, int count, int raw);    /* -1: waiting, -2: terminated */

void dos_init(char **envp);
int dos_start(const char *host_path, int argc, char **argv);
void int20(void);
void int21(void);
void int2f(void);
void dos_cpm_call(void);
void dos_terminate(int code, int type);
void dos_keep(uint16_t paras, int code);        /* terminate and stay resident */
int dos_ctrl_c(void);                   /* 1 if it ended the program */
/* Conventional memory for the DPMI host (INT 31h 0100h-0102h); DOS error codes. */
int dos_mem_alloc(uint16_t paras, uint16_t *seg, uint16_t *largest);
int dos_mem_free(uint16_t seg);
int dos_mem_resize(uint16_t seg, uint16_t paras, uint16_t *max);
uint8_t cp437_upper(uint8_t c);
extern int dos_exited;
extern int dos_exit_code;

/* ---- files and paths ---- */

#define DOS_PATH 128

int path_to_host(const char *dos, char *host, int size, int *drive);
int host_to_dos(const char *host, char *dos, int size);
int drive_valid(int d);
int fs_errno(int err);                  /* fluxOS errno -> DOS error code */
void files_init(void);
int files_int21(void);                  /* the file functions; 0 if not one */
void files_setup_root(uint16_t psp);    /* standard handles 0-4 */
void files_inherit(uint16_t parent, uint16_t child);
void files_close_all(uint16_t psp);
int fcb_parse(uint32_t str, uint32_t fcb, int flags, uint32_t *end);
int stdout_write(const uint8_t *buf, int n);        /* -2: terminated by Ctrl-C */
int stdin_device(void);                 /* DEV_* of handle 0, or -1 */
int stdin_read_byte(void);              /* from a redirected standard input; -1 at EOF */
int current_drive(void);

/* ---- misc ---- */

void dlog(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void fatal(const char *fmt, ...) __attribute__((noreturn, format(printf, 1, 2)));
uint32_t now_ms(void);
void request_idle(int ms);              /* nothing to do: sleep up to ms */

/* Copy between emulated memory and C strings. */
void mem_read(uint32_t addr, void *buf, uint32_t n);
void mem_write(uint32_t addr, const void *buf, uint32_t n);
int mem_strz(uint32_t addr, char *buf, int size);

#endif

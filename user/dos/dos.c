/*
 * DOS: memory, processes, the console, and the INT 21h functions that
 * are not about files (those are in files.c).
 *
 * Memory is DOS's chain of memory control blocks from 0100:0000 up to
 * 640 KiB. The first block holds a stand-in for COMMAND.COM: a PSP that
 * owns the standard handles and the master environment, and is the
 * parent of the program started from the command line. When that
 * program ends, the emulator exits with its return code.
 *
 * EXEC loads and runs DOS programs inside the emulator, saving the
 * parent's stack in its PSP as DOS does; a fluxOS program (ELF or
 * script) runs as a real child process, and COMMAND.COM runs the
 * fluxOS shell, with "/C command" if given.
 *
 * Console input waits by rewinding the callback (see blocked()), so a
 * program's timer and keyboard interrupts keep running while DOS waits
 * for a key, as on a real PC.
 */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include "dos.h"

uint16_t dos_psp;
int dos_last_error;
int dos_exited;
int dos_exit_code;

static uint16_t root_psp;
static uint16_t dta_seg, dta_off;
static uint16_t ret_code;               /* for 4Dh: type << 8 | code */
static int break_on, verify_on, strategy;
static int pending_scan = -1;           /* the scan code half of an extended key */
static unsigned switches;               /* process changes, to notice them */

static uint8_t upcase_hi[128];          /* code page 437 upper case of 80h-FFh */

uint32_t dos_dta(void)
{
    return ADDR(dta_seg, dta_off);
}

void dos_fail(int code)
{
    set_cf(1);
    AX = (uint16_t)code;
    dos_last_error = code;
}

void dos_ok(void)
{
    set_cf(0);
}

uint8_t cp437_upper(uint8_t c)
{
    if (c >= 'a' && c <= 'z')
        return (uint8_t)(c - 32);
    return c >= 0x80 ? upcase_hi[c - 0x80] : c;
}

static uint16_t le16(const uint8_t *p)
{
    return (uint16_t)(p[0] | p[1] << 8);
}

/* ---- memory control blocks ---- */

static uint8_t mcb_type(uint16_t m) { return rb(ADDR(m, 0)); }
static uint16_t mcb_owner(uint16_t m) { return rw(ADDR(m, 1)); }
static uint16_t mcb_size(uint16_t m) { return rw(ADDR(m, 3)); }

static int mcb_valid(uint16_t m)
{
    uint8_t t = mcb_type(m);
    return t == 'M' || t == 'Z';
}

static void mcb_set(uint16_t m, uint8_t type, uint16_t owner, uint16_t size)
{
    wb(ADDR(m, 0), type);
    ww(ADDR(m, 1), owner);
    ww(ADDR(m, 3), size);
}

static void mcb_name(uint16_t m, const char *path)
{
    const char *base = path;
    for (const char *p = path; *p; p++)
        if (*p == '\\' || *p == '/' || *p == ':')
            base = p + 1;
    for (int i = 0; i < 8; i++) {
        char c = base[i] && base[i] != '.' ? base[i] : 0;
        wb(ADDR(m, 8 + i), cp437_upper((uint8_t)c));
        if (!c)
            base = "";
    }
}

/* Join the free blocks that follow the free block m into it. */
static void mcb_merge(uint16_t m)
{
    while (mcb_type(m) == 'M') {
        uint16_t n = (uint16_t)(m + 1 + mcb_size(m));
        if (!mcb_valid(n) || mcb_owner(n) != 0)
            break;
        mcb_set(m, mcb_type(n), 0, (uint16_t)(mcb_size(m) + 1 + mcb_size(n)));
    }
}

static int mem_alloc(uint16_t paras, uint16_t owner, uint16_t *seg, uint16_t *largest)
{
    uint16_t found = 0, m = FIRST_MCB;
    int fit = strategy & 3;             /* 0 first, 1 best, 2 last */
    *largest = 0;
    for (;;) {
        if (!mcb_valid(m))
            return 7;                   /* the chain is damaged */
        if (mcb_owner(m) == 0) {
            mcb_merge(m);
            uint16_t size = mcb_size(m);
            if (size > *largest)
                *largest = size;
            if (size >= paras && (fit == 2 || !found || (fit == 1 && size < mcb_size(found))))
                found = m;
        }
        if (mcb_type(m) == 'Z')
            break;
        m = (uint16_t)(m + 1 + mcb_size(m));
    }
    if (!found)
        return 8;
    uint16_t size = mcb_size(found);
    uint8_t type = mcb_type(found);
    if (fit == 2 && size > paras) {     /* last fit: the top of the block */
        uint16_t head = (uint16_t)(size - paras - 1);
        uint16_t blk = (uint16_t)(found + 1 + head);
        mcb_set(blk, type, owner, paras);
        mcb_set(found, 'M', 0, head);
        *seg = (uint16_t)(blk + 1);
        return 0;
    }
    if (size > paras) {
        mcb_set((uint16_t)(found + 1 + paras), type, 0, (uint16_t)(size - paras - 1));
        type = 'M';
    }
    mcb_set(found, type, owner, size > paras ? paras : size);
    *seg = (uint16_t)(found + 1);
    return 0;
}

static int mem_free(uint16_t seg)
{
    uint16_t m = (uint16_t)(seg - 1);
    if (!mcb_valid(m))
        return 9;
    ww(ADDR(m, 1), 0);
    return 0;
}

static int mem_resize(uint16_t seg, uint16_t paras, uint16_t *max)
{
    uint16_t m = (uint16_t)(seg - 1);
    if (!mcb_valid(m))
        return 9;
    uint16_t owner = mcb_owner(m), size = mcb_size(m);
    uint8_t type = mcb_type(m);
    if (paras > size) {
        /* Take in the free blocks that follow. */
        uint32_t total = size;
        uint8_t t = type;
        uint16_t n = (uint16_t)(m + 1 + size);
        while (t == 'M' && mcb_valid(n) && mcb_owner(n) == 0) {
            total += 1u + mcb_size(n);
            t = mcb_type(n);
            n = (uint16_t)(m + 1 + total);
        }
        if (paras > total) {
            *max = (uint16_t)total;
            if (total > size) {         /* leave the free space as one block */
                mcb_set((uint16_t)(m + 1 + size), t, 0, (uint16_t)(total - size - 1));
                mcb_set(m, 'M', owner, size);
            }
            return 8;
        }
        size = (uint16_t)total;
        type = t;
    }
    if (paras < size) {
        uint16_t rest = (uint16_t)(m + 1 + paras);
        mcb_set(rest, type, 0, (uint16_t)(size - paras - 1));
        mcb_set(m, 'M', owner, paras);
        mcb_merge(rest);
    } else {
        mcb_set(m, type, owner, size);
    }
    return 0;
}

int dos_mem_alloc(uint16_t paras, uint16_t *seg, uint16_t *largest)
{
    return mem_alloc(paras, dos_psp, seg, largest);
}

int dos_mem_free(uint16_t seg)
{
    return mem_free(seg);
}

int dos_mem_resize(uint16_t seg, uint16_t paras, uint16_t *max)
{
    return mem_resize(seg, paras, max);
}

static void mem_free_owned(uint16_t owner)
{
    for (uint16_t m = FIRST_MCB; mcb_valid(m); m = (uint16_t)(m + 1 + mcb_size(m))) {
        if (mcb_owner(m) == owner)
            ww(ADDR(m, 1), 0);
        if (mcb_type(m) == 'Z')
            break;
    }
}

/* ---- environment and PSP ---- */

/* Copy environment src (its strings, not the program name after them)
 * into a new block, followed by the name of the program it is for. */
static int make_env(uint16_t src, const char *progname, uint16_t owner, uint16_t *seg)
{
    uint32_t len = 0;
    if (src && rb(ADDR(src, 0))) {
        while (len < 32766 && (rb(ADDR(src, len)) || rb(ADDR(src, len + 1))))
            len++;
        len += 2;
    } else {
        len = src ? 1 : 0;
    }
    uint32_t total = (len ? len : 1) + 2 + (uint32_t)strlen(progname) + 1;
    uint16_t largest;
    int err = mem_alloc((uint16_t)((total + 15) / 16), owner, seg, &largest);
    if (err)
        return err;
    uint32_t e = ADDR(*seg, 0);
    for (uint32_t i = 0; i < len; i++)
        wb(e + i, rb(ADDR(src, i)));
    if (!len)
        wb(e + len++, 0);
    ww(e + len, 1);
    mem_write(e + len + 2, progname, (uint32_t)strlen(progname) + 1);
    return 0;
}

static void make_psp(uint16_t psp, uint16_t top, uint16_t parent, uint16_t env)
{
    uint32_t p = ADDR(psp, 0);
    for (int i = 0; i < 256; i++)
        wb(p + (uint32_t)i, 0);
    wb(p, 0xCD);                        /* INT 20h */
    wb(p + 1, 0x20);
    ww(p + 2, top);
    /* CALL FAR to the CP/M-style entry, placed so that the word at 06h
     * says how much memory a CP/M program may use. */
    wb(p + 5, 0x9A);
    ww(p + 6, 0xFEF0);
    ww(p + 8, (uint16_t)((0xF0000u + CALLBACK_OFF(CB_CPM) - 0xFEF0u) >> 4));
    wd(p + 0x0A, rd(0x22 * 4));
    wd(p + 0x0E, rd(0x23 * 4));
    wd(p + 0x12, rd(0x24 * 4));
    ww(p + 0x16, parent);
    for (int h = 0; h < 20; h++)
        wb(p + 0x18 + (uint32_t)h, 0xFF);
    ww(p + 0x2C, env);
    ww(p + 0x32, 20);
    ww(p + 0x34, 0x18);
    ww(p + 0x36, psp);
    wd(p + 0x38, 0xFFFFFFFF);
    ww(p + 0x40, 0x0005);               /* DOS version 5.0 */
    wb(p + 0x50, 0xCD);                 /* INT 21h, RETF */
    wb(p + 0x51, 0x21);
    wb(p + 0x52, 0xCB);
    for (int i = 1; i < 12; i++) {
        wb(p + 0x5C + (uint32_t)i, ' ');
        wb(p + 0x6C + (uint32_t)i, ' ');
    }
    wb(p + 0x81, 0x0D);
}

/* ---- loading programs ---- */

struct start {
    uint16_t psp, cs, ip, ss, sp;
};

struct exe {
    uint32_t header, image, relocs, reloc_off;
    uint16_t minalloc, maxalloc, ss, sp, cs, ip;
};

static int read_file(const char *host, uint8_t **data, uint32_t *size)
{
    int fd = open(host, O_RDONLY);
    if (fd < 0)
        return fs_errno(errno);
    struct stat st;
    if (fstat(fd, &st) < 0 || S_ISDIR(st.st_mode)) {
        close(fd);
        return 5;
    }
    uint32_t n = st.st_size < MEM_SIZE ? st.st_size : MEM_SIZE;
    uint8_t *d = malloc(n + 1);
    if (!d) {
        close(fd);
        return 8;
    }
    uint32_t got = 0;
    while (got < n) {
        int r = (int)read(fd, d + got, n - got);
        if (r <= 0)
            break;
        got += (uint32_t)r;
    }
    close(fd);
    *data = d;
    *size = got;
    return 0;
}

static int is_exe(const uint8_t *d, uint32_t n)
{
    return n >= 0x1C && ((d[0] == 'M' && d[1] == 'Z') || (d[0] == 'Z' && d[1] == 'M'));
}

static int parse_exe(const uint8_t *d, uint32_t n, struct exe *e)
{
    uint32_t last = le16(d + 2), pages = le16(d + 4);
    e->relocs = le16(d + 6);
    e->header = le16(d + 8) * 16u;
    e->minalloc = le16(d + 0x0A);
    e->maxalloc = le16(d + 0x0C);
    e->ss = le16(d + 0x0E);
    e->sp = le16(d + 0x10);
    e->ip = le16(d + 0x14);
    e->cs = le16(d + 0x16);
    e->reloc_off = le16(d + 0x18);
    uint32_t total = pages * 512 - (last ? 512 - last : 0);
    if (!pages || total > n)
        total = n;
    if (e->header > total || e->reloc_off + e->relocs * 4 > n)
        return 11;                      /* invalid format */
    e->image = total - e->header;
    return 0;
}

static void relocate(const uint8_t *d, const struct exe *e, uint16_t load_seg, uint16_t delta)
{
    for (uint32_t i = 0; i < e->relocs; i++) {
        const uint8_t *r = d + e->reloc_off + i * 4;
        uint32_t a = ADDR((uint16_t)(load_seg + le16(r + 2)), le16(r));
        ww(a, (uint16_t)(rw(a) + delta));
    }
}

static uint8_t fcb_drive_flag(uint32_t fcb)
{
    uint8_t d = rb(fcb);
    return d && !drive_valid(d - 1) ? 0xFF : 0;
}

/*
 * Load a DOS program as a child of the current one. tail is the
 * command tail (length, text, CR). FCBs come from fcb1/fcb2 (far
 * pointers) or, if parse is set, are parsed from the tail.
 */
static int load_program(const char *host, const char *dosname, uint16_t env_src,
                        const uint8_t *tail, uint32_t fcb1, uint32_t fcb2, int parse,
                        struct start *st)
{
    uint8_t *d;
    uint32_t n;
    int err = read_file(host, &d, &n);
    if (err)
        return err;
    struct exe e;
    int exe = is_exe(d, n);
    if (exe && (err = parse_exe(d, n, &e))) {
        free(d);
        return err;
    }
    if (!exe && n > 0xFE00) {
        free(d);
        return 11;
    }

    uint16_t env, psp, largest, dummy;
    if ((err = make_env(env_src, dosname, dos_psp, &env))) {
        free(d);
        return err;
    }
    mem_alloc(0xFFFF, 0, &dummy, &largest);
    uint32_t load = exe ? (e.image + 15) / 16 : 0, need, want;
    int high = exe && !e.minalloc && !e.maxalloc;
    if (exe) {
        need = 0x10 + load + e.minalloc;
        want = high ? 0xFFFF : 0x10 + load + e.maxalloc;
    } else {
        need = (0x100 + n + 0x100 + 15) / 16;   /* PSP, image, a small stack */
        want = 0xFFFF;
    }
    if (want > 0xFFFF)
        want = 0xFFFF;
    if (largest < need) {
        mem_free(env);
        free(d);
        return 8;
    }
    uint16_t size = (uint16_t)(want < largest ? want : largest);
    if ((err = mem_alloc(size, dos_psp, &psp, &dummy))) {
        mem_free(env);
        free(d);
        return err;
    }
    ww(ADDR(env - 1, 1), psp);
    ww(ADDR(psp - 1, 1), psp);
    mcb_name((uint16_t)(psp - 1), dosname);
    mcb_name((uint16_t)(env - 1), dosname);

    make_psp(psp, (uint16_t)(psp + size), dos_psp, env);
    files_inherit(dos_psp, psp);
    mem_write(ADDR(psp, 0x80), tail, 128);
    if (parse) {
        uint32_t end;
        fcb_parse(ADDR(psp, 0x81), ADDR(psp, 0x5C), 1, &end);
        fcb_parse(end, ADDR(psp, 0x6C), 1, NULL);
    } else {
        for (int i = 0; i < 16; i++) {
            if (fcb1 && fcb1 != 0xFFFFFFFF)
                wb(ADDR(psp, 0x5C + i), rb(ADDR(fcb1 >> 16, (fcb1 & 0xFFFF) + (uint32_t)i)));
            if (fcb2 && fcb2 != 0xFFFFFFFF)
                wb(ADDR(psp, 0x6C + i), rb(ADDR(fcb2 >> 16, (fcb2 & 0xFFFF) + (uint32_t)i)));
        }
    }

    if (exe) {
        uint16_t load_seg = (uint16_t)(high ? psp + size - load : psp + 0x10u);
        mem_write(ADDR(load_seg, 0), d + e.header, e.image);
        relocate(d, &e, load_seg, load_seg);
        st->cs = (uint16_t)(load_seg + e.cs);
        st->ip = e.ip;
        st->ss = (uint16_t)(load_seg + e.ss);
        st->sp = e.sp;
    } else {
        mem_write(ADDR(psp, 0x100), d, n);
        uint32_t top = (uint32_t)size * 16;
        st->cs = st->ss = psp;
        st->ip = 0x100;
        st->sp = (uint16_t)(top >= 0x10000 ? 0xFFFE : top - 2);
        ww(ADDR(psp, st->sp), 0);       /* RET goes to the INT 20h at PSP:0 */
    }
    st->psp = psp;
    free(d);
    return 0;
}

static void start_program(const struct start *st)
{
    uint16_t psp = st->psp;
    dos_psp = psp;
    dta_seg = psp;
    dta_off = 0x80;
    for (int i = 0; i < 8; i++)
        cpu.r[i] = 0;
    AL = fcb_drive_flag(ADDR(psp, 0x5C));
    AH = fcb_drive_flag(ADDR(psp, 0x6C));
    CX = 0x00FF;
    DX = psp;
    SI = st->ip;
    DI = st->sp;
    BP = 0x091C;
    SP = st->sp;
    DS = ES = psp;
    SS = st->ss;
    CS = st->cs;
    cpu.ip = st->ip;
    cpu.flags = 0x0202;
    switches++;
}

/* The command tail for arguments: " ARG ARG...", at most 126 bytes. */
static void build_tail(uint8_t tail[128], int argc, char **argv)
{
    int len = 0;
    memset(tail, 0, 128);
    for (int i = 0; i < argc; i++) {
        if (len < 126)
            tail[1 + len++] = ' ';
        for (const char *p = argv[i]; *p && len < 126; p++)
            tail[1 + len++] = (uint8_t)*p;
    }
    tail[0] = (uint8_t)len;
    tail[1 + len] = 0x0D;
}

int dos_start(const char *host, int argc, char **argv)
{
    char dosname[DOS_PATH];
    uint8_t tail[128];
    host_to_dos(host, dosname, sizeof(dosname));
    build_tail(tail, argc, argv);
    struct start st;
    int err = load_program(host, dosname, rw(ADDR(root_psp, 0x2C)), tail, 0, 0, 1, &st);
    if (err)
        return err;
    start_program(&st);
    return 0;
}

/* ---- termination ---- */

void dos_terminate(int code, int type)
{
    uint16_t psp = dos_psp, parent = rw(ADDR(psp, 0x16));
    video_flush();
    dpmi_terminate(psp);
    if (type != 3) {
        files_close_all(psp);
        mem_free_owned(psp);
    }
    wd(0x22 * 4, rd(ADDR(psp, 0x0A)));
    wd(0x23 * 4, rd(ADDR(psp, 0x0E)));
    wd(0x24 * 4, rd(ADDR(psp, 0x12)));
    ret_code = (uint16_t)(type << 8 | (code & 0xFF));
    pending_scan = -1;
    switches++;
    if (parent == root_psp || parent == psp) {
        dos_exited = 1;
        dos_exit_code = code & 0xFF;
        cpu.stop = 1;
        return;
    }
    /* Back to the parent: on its stack, as it was in the INT 21h that
     * started the child, then out of that INT 21h with carry clear. */
    dos_psp = parent;
    dta_seg = parent;
    dta_off = 0x80;
    SS = rw(ADDR(parent, 0x30));
    SP = rw(ADDR(parent, 0x2E));
    uint32_t frame = ADDR(SS, SP + 4);
    ww(frame, (uint16_t)(rw(frame) & ~F_CF));
    CS = BIOS_SEG;
    cpu.ip = CALLBACK_OFF(CB_IRET);
}

void dos_keep(uint16_t paras, int code)
{
    uint16_t max;
    if (paras < 6)
        paras = 6;
    mem_resize(dos_psp, paras, &max);
    dos_terminate(code, 3);
}

/* Ctrl-C typed while DOS read or wrote the console: terminate, unless
 * the program has an INT 23h handler, which here means it is ignored.
 * Returns 1 if the program was terminated. */
int dos_ctrl_c(void)
{
    con_puts("^C\r\n");
    if (rw(0x23 * 4) != CALLBACK_OFF(0x23) || rw(0x23 * 4 + 2) != BIOS_SEG)
        return 0;
    dos_terminate(0, 1);
    return 1;
}

void int20(void)
{
    dos_terminate(0, 0);
}

/* ---- running other programs ---- */

static int is_native(const char *host)
{
    unsigned char b[4];
    int fd = open(host, O_RDONLY);
    if (fd < 0)
        return 0;
    int n = (int)read(fd, b, 4);
    close(fd);
    return (n == 4 && !memcmp(b, "\x7f" "ELF", 4)) || (n >= 2 && b[0] == '#' && b[1] == '!');
}

static int run_native(char **argv)
{
    video_suspend();
    kbd_restore();
    int status = 0, pid = fork();
    if (pid == 0) {
        execve(argv[0], argv, environ);
        dprintf(2, "dos: cannot run %s: %s\n", argv[0], strerror(errno));
        _exit(127);
    }
    if (pid > 0)
        waitpid(pid, &status, 0);
    kbd_init();
    video_resume();
    timer_poll();
    if (pid < 0)
        return -1;
    return WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
}

/* Split a command tail into arguments, after argv[0]. */
static int split_tail(char *text, char **argv, int max, int first)
{
    int n = first;
    for (char *p = text; *p && n < max - 1;) {
        while (*p == ' ' || *p == '\t')
            p++;
        if (!*p)
            break;
        argv[n++] = p;
        while (*p && *p != ' ' && *p != '\t')
            p++;
        if (*p)
            *p++ = 0;
    }
    argv[n] = NULL;
    return n;
}

static int base_is(const char *path, const char *name)
{
    const char *base = path;
    for (const char *p = path; *p; p++)
        if (*p == '\\' || *p == '/' || *p == ':')
            base = p + 1;
    return !strcasecmp(base, name);
}

/* COMMAND.COM: the fluxOS shell, running "/C command" if there is one. */
static int run_shell(char *text)
{
    char *argv[4];
    while (*text == ' ' || *text == '\t')
        text++;
    if (text[0] == '/' && (text[1] == 'c' || text[1] == 'C')) {
        argv[0] = "/bin/sh";
        argv[1] = "-c";
        argv[2] = text + 2;
        argv[3] = NULL;
        return run_native(argv);
    }
    argv[0] = "/bin/sh";
    argv[1] = NULL;
    video_suspend();
    dprintf(1, "Type 'exit' to return to the DOS program.\r\n");
    return run_native(argv);
}

static void dos_exec(void)
{
    char path[DOS_PATH], host[PATH_MAX], text[128];
    mem_strz(ADDR(DS, DX), path, sizeof(path));
    uint32_t pb = ADDR(ES, BX);
    int mode = AL;
    if (mode != 0 && mode != 1 && mode != 3) {
        dos_fail(1);
        return;
    }

    if (mode != 3) {
        uint32_t t = rd(pb + 2);
        uint32_t ta = ADDR(t >> 16, t & 0xFFFF);
        int len = rb(ta);
        if (len > 126)
            len = 126;
        for (int i = 0; i < len; i++)
            text[i] = (char)rb(ta + 1 + (uint32_t)i);
        text[len] = 0;
        char *cr = strchr(text, '\r');
        if (cr)
            *cr = 0;
    }

    if (mode == 0 && base_is(path, "COMMAND.COM")) {
        int r = run_shell(text);
        ret_code = (uint16_t)(r < 0 ? 0 : r & 0xFF);
        dos_ok();
        return;
    }
    int err = path_to_host(path, host, sizeof(host), NULL);
    if (err || access(host, F_OK) != 0) {
        dos_fail(err ? err : 2);
        return;
    }
    if (mode == 0 && is_native(host)) {
        char *argv[32];
        argv[0] = host;
        split_tail(text, argv, 32, 1);
        int r = run_native(argv);
        if (r < 0) {
            dos_fail(8);
            return;
        }
        ret_code = (uint16_t)(r & 0xFF);
        dos_ok();
        return;
    }

    if (mode == 3) {                    /* load an overlay */
        uint16_t seg = rw(pb), delta = rw(pb + 2);
        uint8_t *d;
        uint32_t n;
        if ((err = read_file(host, &d, &n))) {
            dos_fail(err);
            return;
        }
        struct exe e;
        if (is_exe(d, n)) {
            if ((err = parse_exe(d, n, &e))) {
                free(d);
                dos_fail(err);
                return;
            }
            mem_write(ADDR(seg, 0), d + e.header, e.image);
            relocate(d, &e, seg, delta);
        } else {
            mem_write(ADDR(seg, 0), d, n);
        }
        free(d);
        dos_ok();
        return;
    }

    /* A DOS program: remember where the parent is, start the child. */
    uint8_t tail[128];
    uint32_t t = rd(pb + 2);
    mem_read(ADDR(t >> 16, t & 0xFFFF), tail, 128);
    if (tail[0] > 126) {
        tail[0] = 126;
        tail[127] = 0x0D;
    }
    char dosname[DOS_PATH];
    host_to_dos(host, dosname, sizeof(dosname));
    uint16_t env = rw(pb);
    if (!env)
        env = rw(ADDR(dos_psp, 0x2C));
    uint16_t parent = dos_psp;
    ww(ADDR(parent, 0x2E), SP);
    ww(ADDR(parent, 0x30), SS);
    uint32_t ret = rd(ADDR(SS, SP));    /* the caller's IP and CS */

    struct start st;
    err = load_program(host, dosname, env, tail, rd(pb + 6), rd(pb + 10), 0, &st);
    if (err) {
        dos_fail(err);
        return;
    }
    wd(ADDR(st.psp, 0x0A), ret);        /* terminate address */
    wd(0x22 * 4, ret);
    if (mode == 1) {                    /* load, do not run: tell the caller where */
        st.sp = (uint16_t)(st.sp - 2);
        ww(ADDR(st.ss, st.sp), (uint16_t)(fcb_drive_flag(ADDR(st.psp, 0x5C)) |
                                          fcb_drive_flag(ADDR(st.psp, 0x6C)) << 8));
        ww(pb + 0x0E, st.sp);
        ww(pb + 0x10, st.ss);
        ww(pb + 0x12, st.ip);
        ww(pb + 0x14, st.cs);
        dos_psp = st.psp;
        dta_seg = st.psp;
        dta_off = 0x80;
        dos_ok();
        return;
    }
    start_program(&st);
}

/* ---- the console ---- */

/* The next key typed, as DOS returns it: an extended key comes as 0,
 * then its scan code. -1 if no key is waiting. */
static int con_key(void)
{
    if (pending_scan >= 0) {
        int c = pending_scan;
        pending_scan = -1;
        return c;
    }
    if (!kbd_available())
        return -1;
    uint16_t k = kbd_read();
    uint8_t c = (uint8_t)k;
    if (c == 0 || c == 0xE0) {
        pending_scan = k >> 8;
        return 0;
    }
    return c;
}

static int con_ready(void)
{
    return pending_scan >= 0 || kbd_available();
}

/* A character of standard input: the keyboard, or a redirected file
 * (^Z at its end). -1 while no key is waiting. */
static int stdin_char(void)
{
    int dev = stdin_device();
    if (dev == DEV_CON)
        return con_key();
    if (dev != DEV_FILE)
        return 0x1A;
    int c = stdin_read_byte();
    return c < 0 ? 0x1A : c;
}

static int out_char(uint8_t c)
{
    return stdout_write(&c, 1);
}

static struct {
    int active, len;
    char buf[256];
} line;

/* Line input with DOS editing (Backspace, Esc). Returns 1 when Enter
 * ends the line, 0 while waiting, -1 if Ctrl-C ended the program. */
static int line_input(int max, int from_console, int echo_lf)
{
    int con = from_console || stdin_device() == DEV_CON;
    if (!line.active) {
        line.active = 1;
        line.len = 0;
    }
    for (;;) {
        int c = from_console ? con_key() : stdin_char();
        if (c < 0) {
            blocked();
            return 0;
        }
        if (!con && c == '\n')
            c = '\r';
        if (c == 0) {
            if (con)
                con_key();              /* an extended key: no use here */
            continue;
        }
        if (c == 3 && con) {
            line.active = 0;
            if (dos_ctrl_c())
                return -1;
            line.active = 1;
            line.len = 0;
            continue;
        }
        if (c == '\r' || (!con && c == 0x1A && !line.len)) {
            line.active = 0;
            if (con)
                con_puts(echo_lf ? "\r\n" : "\r");
            if (c == 0x1A)
                line.buf[line.len++] = 0x1A;
            return 1;
        }
        if (c == 8 || c == 0x7F) {
            if (line.len) {
                line.len--;
                if (con)
                    con_puts("\b \b");
            }
            continue;
        }
        if (c == 27 && con) {
            line.len = 0;
            con_puts("\\\r\n");
            continue;
        }
        if (line.len < max - 1 && line.len < (int)sizeof(line.buf) - 2) {
            line.buf[line.len++] = (char)c;
            if (con)
                con_putc((uint8_t)c);
        }
    }
}

static char con_line[260];
static int con_line_len, con_line_pos;

int con_read(uint32_t addr, int count, int raw)
{
    if (count <= 0)
        return 0;
    if (raw) {
        int n = 0;
        for (int c; n < count && (c = con_key()) >= 0; n++)
            wb(addr + (uint32_t)n, (uint8_t)c);
        if (!n) {
            blocked();
            return -1;
        }
        return n;
    }
    if (con_line_pos >= con_line_len) {
        int r = line_input(128, 1, 1);
        if (r == 0)
            return -1;
        if (r < 0)
            return -2;
        char *eof = memchr(line.buf, 0x1A, (size_t)line.len);
        con_line_len = eof ? (int)(eof - line.buf) : line.len;
        memcpy(con_line, line.buf, (size_t)con_line_len);
        if (!eof) {
            con_line[con_line_len++] = '\r';
            con_line[con_line_len++] = '\n';
        }
        con_line_pos = 0;
    }
    int n = con_line_len - con_line_pos;
    if (n > count)
        n = count;
    mem_write(addr, con_line + con_line_pos, (uint32_t)n);
    con_line_pos += n;
    return n;
}

/* Read a character for functions 01h, 07h and 08h. Returns 0 when the
 * caller should return at once (waiting, or terminated). */
static int read_char(int echo, int check_break)
{
    int c = stdin_char();
    if (c < 0) {
        blocked();
        return 0;
    }
    if (c == 3 && check_break && stdin_device() == DEV_CON && dos_ctrl_c())
        return 0;
    if (echo && c && out_char((uint8_t)c) == -2)
        return 0;
    AL = (uint8_t)c;
    return 1;
}

static int break_check(void)
{
    if ((kbd_peek() & 0xFF) == 3 && stdin_device() == DEV_CON) {
        kbd_read();
        return dos_ctrl_c();
    }
    return 0;
}

static void console_function(int fn)
{
    switch (fn) {
    case 0x01:
        read_char(1, 1);
        return;
    case 0x06:
        if (DL != 0xFF) {
            out_char(DL);
            AL = DL;
            return;
        }
        if (stdin_device() == DEV_CON && !con_ready()) {
            AL = 0;
            set_zf(1);
            kbd_idle_hint();
            return;
        }
        AL = (uint8_t)stdin_char();
        set_zf(0);
        return;
    case 0x07:
        read_char(0, 0);
        return;
    case 0x08:
        read_char(0, 1);
        return;
    case 0x0A: {
        uint32_t b = ADDR(DS, DX);
        int max = rb(b);
        if (!max)
            return;
        if (line_input(max, 0, 0) <= 0)
            return;
        int len = line.len;
        if (len && line.buf[len - 1] == 0x1A)
            len--;
        mem_write(b + 2, line.buf, (uint32_t)len);
        wb(b + 2 + (uint32_t)len, '\r');
        wb(b + 1, (uint8_t)len);
        return;
    }
    }
}

/* ---- country information ---- */

static void country_info(uint32_t a)
{
    for (int i = 0; i < 34; i++)
        wb(a + (uint32_t)i, 0);
    wb(a + 2, '$');
    wb(a + 7, ',');
    wb(a + 9, '.');
    wb(a + 11, '-');
    wb(a + 13, ':');
    wb(a + 16, 2);                      /* digits after the decimal point */
    ww(a + 0x12, CALLBACK_OFF(CB_CASEMAP));
    ww(a + 0x14, BIOS_SEG);
    wb(a + 0x16, ',');
}

static void ext_country(void)
{
    uint32_t out = ADDR(ES, DI);
    uint32_t table;
    switch (AL) {
    case 0x01: {
        int n = CX < 41 ? CX : 41;
        uint8_t buf[41];
        buf[0] = 1;
        buf[1] = 38;
        buf[2] = 0;
        buf[3] = 1;                     /* country 1 */
        buf[4] = 0;
        buf[5] = 437 & 0xFF;            /* code page 437 */
        buf[6] = 437 >> 8;
        mem_read(ADDR(DOS_SEG, DOS_COUNTRY), buf + 7, 34);
        mem_write(out, buf, (uint32_t)n);
        CX = (uint16_t)n;
        dos_ok();
        return;
    }
    case 0x02: table = DOS_UPCASE; break;
    case 0x04: table = DOS_UPCASE; break;
    case 0x05: table = DOS_TERMINATORS; break;
    case 0x06: table = DOS_COLLATE; break;
    case 0x07: table = DOS_DBCS; break;
    case 0x20:
        DL = cp437_upper(DL);
        dos_ok();
        return;
    case 0x21:
        for (uint32_t i = 0, a = ADDR(DS, DX); i < CX; i++)
            wb(a + i, cp437_upper(rb(a + i)));
        dos_ok();
        return;
    case 0x22:
        for (uint32_t a = ADDR(DS, DX); rb(a); a++)
            wb(a, cp437_upper(rb(a)));
        dos_ok();
        return;
    case 0x23: {
        uint8_t c = cp437_upper(DL);
        AX = c == 'Y' ? 1 : c == 'N' ? 0 : 2;
        dos_ok();
        return;
    }
    default:
        dos_fail(1);
        return;
    }
    wb(out, AL);
    ww(out + 1, (uint16_t)table);
    ww(out + 3, DOS_SEG);
    CX = 5;
    dos_ok();
}

/* ---- INT 21h ---- */

static void unknown_function(void)
{
    static uint8_t reported[256];
    if (!reported[AH]) {
        reported[AH] = 1;
        dlog("unsupported INT 21h function %02Xh (AX=%04X)\n", AH, AX);
    }
    dos_fail(1);
}

static void int21_body(void)
{
    if (files_int21())
        return;

    uint16_t seg, largest;
    int err;
    time_t now;
    struct tm tm;

    switch (AH) {
    case 0x00:
        dos_terminate(0, 0);
        return;
    case 0x01:
    case 0x06:
    case 0x07:
    case 0x08:
    case 0x0A:
        console_function(AH);
        return;
    case 0x02:
        if (out_char(DL) != -2)
            AL = DL;
        return;
    case 0x03:                          /* auxiliary input: nothing */
        AL = 0x1A;
        return;
    case 0x04:                          /* auxiliary and printer output: dropped */
    case 0x05:
        return;
    case 0x09: {
        uint8_t buf[256];
        uint32_t a = ADDR(DS, DX);
        if (break_check())
            return;
        for (uint32_t total = 0; total < 0x10000;) {
            int n = 0;
            while (n < (int)sizeof(buf) && rb(a + total) != '$') {
                buf[n++] = rb(a + total);
                total++;
            }
            if (n && stdout_write(buf, n) == -2)
                return;
            if (n < (int)sizeof(buf))
                break;
        }
        AL = '$';
        return;
    }
    case 0x0B:
        if (break_check())
            return;
        if (stdin_device() == DEV_CON) {
            AL = con_ready() ? 0xFF : 0x00;
            if (!AL)
                kbd_idle_hint();
        } else {
            AL = 0xFF;
        }
        return;
    case 0x0C: {                        /* flush input, then read */
        static int flushed;
        if (!flushed) {
            while (kbd_available())
                kbd_read();
            pending_scan = -1;
            flushed = 1;
        }
        uint32_t ip = cpu.ip;
        if (AL == 0x01 || AL == 0x06 || AL == 0x07 || AL == 0x08 || AL == 0x0A)
            console_function(AL);
        if (cpu.ip == ip)
            flushed = 0;                /* done, not waiting */
        return;
    }
    case 0x0D:                          /* disk reset */
        sync();
        return;
    case 0x18:
    case 0x1D:
    case 0x1E:
    case 0x20:                          /* CP/M leftovers */
        AL = 0;
        return;
    case 0x1A:
        dta_seg = DS;
        dta_off = DX;
        return;
    case 0x1F:                          /* drive parameter blocks: none */
    case 0x32:
        AL = 0xFF;
        return;
    case 0x25:
        ww((uint32_t)AL * 4, DX);
        ww((uint32_t)AL * 4 + 2, DS);
        return;
    case 0x26:                          /* create a PSP: a copy of the current one */
        for (uint32_t i = 0; i < 256; i++)
            wb(ADDR(DX, i), rb(ADDR(dos_psp, i)));
        ww(ADDR(DX, 0x36), DX);
        return;
    case 0x2A:
        now = time(NULL);
        gmtime_r(&now, &tm);
        CX = (uint16_t)(1900 + tm.tm_year);
        DH = (uint8_t)(tm.tm_mon + 1);
        DL = (uint8_t)tm.tm_mday;
        AL = (uint8_t)tm.tm_wday;
        return;
    case 0x2C:
        now = time(NULL);
        gmtime_r(&now, &tm);
        CH = (uint8_t)tm.tm_hour;
        CL = (uint8_t)tm.tm_min;
        DH = (uint8_t)tm.tm_sec;
        DL = (uint8_t)(now_ms() % 1000 / 10);
        return;
    case 0x2B:                          /* setting the date or time: accepted, ignored */
    case 0x2D:
        AL = 0;
        return;
    case 0x2E:
        verify_on = AL & 1;
        return;
    case 0x2F:
        ES = dta_seg;
        BX = dta_off;
        return;
    case 0x30:
        if (AL == 1)
            BH = 0x00;                  /* not in the HMA */
        else
            BH = 0xFF;                  /* MS-DOS */
        AL = 5;
        AH = 0;
        BL = 0;
        CX = 0;
        return;
    case 0x31:
        dos_keep(DX, AL);
        return;
    case 0x33:
        switch (AL) {
        case 0: DL = (uint8_t)break_on; break;
        case 1: break_on = DL & 1; break;
        case 2: { int old = break_on; break_on = DL & 1; DL = (uint8_t)old; break; }
        case 5: DL = 3; break;          /* booted from C: */
        case 6: BX = 0x0005; DX = 0; break;
        default: AL = 0xFF; break;
        }
        return;
    case 0x34:
        ES = DOS_SEG;
        BX = DOS_INDOS;
        return;
    case 0x35:
        BX = rw((uint32_t)AL * 4);
        ES = rw((uint32_t)AL * 4 + 2);
        return;
    case 0x37:                          /* switch character */
        if (AL == 0) {
            DL = '/';
            AL = 0;
        } else if (AL == 1) {
            AL = 0;
        } else {
            DL = 0xFF;
            AL = 0;
        }
        return;
    case 0x38:
        if (DX != 0xFFFF)
            country_info(ADDR(DS, DX));
        BX = 1;
        dos_ok();
        return;
    case 0x48:
        if ((err = mem_alloc(BX, dos_psp, &seg, &largest))) {
            dos_fail(err);
            BX = largest;
            return;
        }
        AX = seg;
        dos_ok();
        return;
    case 0x49:
        if ((err = mem_free(ES)))
            dos_fail(err);
        else
            dos_ok();
        return;
    case 0x4A:
        if ((err = mem_resize(ES, BX, &largest))) {
            dos_fail(err);
            if (err == 8)
                BX = largest;
            return;
        }
        dos_ok();
        return;
    case 0x4B:
        dos_exec();
        return;
    case 0x4C:
        dos_terminate(AL, 0);
        return;
    case 0x4D:
        AX = ret_code;
        ret_code = 0;
        return;
    case 0x50:
        dos_psp = BX;
        return;
    case 0x51:
    case 0x62:
        BX = dos_psp;
        return;
    case 0x52:
        ES = DOS_SEG;
        BX = DOS_LOL;
        return;
    case 0x54:
        AL = (uint8_t)verify_on;
        return;
    case 0x55:                          /* create a child PSP */
        make_psp(DX, SI, dos_psp, rw(ADDR(dos_psp, 0x2C)));
        files_inherit(dos_psp, DX);
        dos_psp = DX;
        return;
    case 0x58:
        if (AL == 0) {
            AX = (uint16_t)strategy;
            dos_ok();
        } else if (AL == 1) {
            strategy = BX & 0x43;
            dos_ok();
        } else if (AL == 2) {
            AL = 0;                     /* no upper memory */
            dos_ok();
        } else if (AL == 3) {
            if (BX)
                dos_fail(1);
            else
                dos_ok();
        } else {
            dos_fail(1);
        }
        return;
    case 0x59:                          /* extended error */
        AX = (uint16_t)dos_last_error;
        BH = dos_last_error == 2 || dos_last_error == 3 || dos_last_error == 0x12 ? 8
           : dos_last_error == 5 ? 3 : dos_last_error == 4 || dos_last_error == 8 ? 1 : 7;
        BL = 4;
        CH = dos_last_error >= 2 && dos_last_error <= 6 ? 2 : 1;
        return;
    case 0x5F:                          /* no network redirections */
        dos_fail(AL == 2 ? 0x12 : 1);
        return;
    case 0x63:                          /* no double-byte characters */
        DS = DOS_SEG;
        SI = DOS_DBCS + 2;
        AL = 0;
        return;
    case 0x65:
        ext_country();
        return;
    case 0x66:
        if (AL == 1) {
            BX = 437;
            DX = 437;
        }
        dos_ok();
        return;
    case 0x67: {                        /* set handle count */
        uint16_t psp = dos_psp;
        int n = rw(ADDR(psp, 0x32));
        if (BX <= n) {
            dos_ok();
            return;
        }
        if ((err = mem_alloc((uint16_t)((BX + 15) / 16), psp, &seg, &largest))) {
            dos_fail(err);
            return;
        }
        uint32_t old = ADDR(rw(ADDR(psp, 0x36)), rw(ADDR(psp, 0x34)));
        for (int h = 0; h < BX; h++)
            wb(ADDR(seg, h), h < n ? rb(old + (uint32_t)h) : 0xFF);
        ww(ADDR(psp, 0x32), BX);
        ww(ADDR(psp, 0x34), 0);
        ww(ADDR(psp, 0x36), seg);
        dos_ok();
        return;
    }
    case 0x69:                          /* disk serial number */
        if (AL == 0) {
            uint32_t a = ADDR(DS, DX);
            ww(a, 0);
            wd(a + 2, 0x464C5558);
            mem_write(a + 6, "NO NAME    FAT     ", 19);
        }
        dos_ok();
        return;
    case 0x71:                          /* no long file name API */
        AX = 0x7100;
        set_cf(1);
        return;
    default:
        unknown_function();
        return;
    }
}

void int21(void)
{
    int21_body();
}

/* CALL 5 in the PSP, the CP/M way into DOS: function in CL. */
void dos_cpm_call(void)
{
    uint32_t ip_after = cpu.ip;
    unsigned before = switches;
    uint16_t far_ip = cpu_pop16(), far_cs = cpu_pop16(), near_ip = cpu_pop16();
    if (CL <= 0x24) {
        AH = CL;
        int21_body();
    } else {
        AL = 0;
    }
    if (switches != before)
        return;                         /* the program ended */
    if (cpu.ip != ip_after) {           /* waiting: run the call again later */
        cpu_push16(near_ip);
        cpu_push16(far_cs);
        cpu_push16(far_ip);
        return;
    }
    CS = far_cs;
    cpu.ip = near_ip;
}

void int2f(void)
{
    switch (AX) {
    case 0x1600:                        /* no Windows */
    case 0x4300:                        /* no XMS */
        AL = 0;
        return;
    case 0x1687:                        /* the DPMI host */
        dpmi_install_check();
        return;
    case 0x1680:                        /* release the time slice */
        request_idle(2);
        AL = 0;
        return;
    case 0x1A00:                        /* ANSI.SYS is there */
        AL = 0xFF;
        return;
    case 0x1500:                        /* no MSCDEX */
        BX = 0;
        return;
    case 0x4A01:                        /* no free HMA */
    case 0x4A02:
        BX = 0;
        ES = 0xFFFF;
        DI = 0xFFFF;
        return;
    }
    /* Anything else: not installed (AL stays 0 for installation checks). */
}

/* ---- setup ---- */

static void dos_data(void)
{
    uint32_t base = ADDR(DOS_SEG, 0);
    for (uint32_t i = 0; i < 0x400; i++)
        wb(base + i, 0);
    uint32_t lol = base + DOS_LOL;
    ww(lol - 2, FIRST_MCB);
    wd(lol + 0x00, 0xFFFFFFFF);         /* no drive parameter blocks */
    ww(lol + 0x04, DOS_SFT);
    ww(lol + 0x06, DOS_SEG);
    ww(lol + 0x08, DOS_LOL + 0x22);     /* CLOCK$: NUL will do */
    ww(lol + 0x0A, DOS_SEG);
    ww(lol + 0x0C, DOS_CON_HDR);
    ww(lol + 0x0E, DOS_SEG);
    ww(lol + 0x10, 512);
    wd(lol + 0x12, 0xFFFFFFFF);
    wd(lol + 0x16, 0xFFFFFFFF);
    wd(lol + 0x1A, 0xFFFFFFFF);
    wb(lol + 0x20, 4);
    wb(lol + 0x21, 26);                 /* LASTDRIVE=Z */
    /* The device chain: NUL, then CON. */
    uint32_t nul = lol + 0x22, con = base + DOS_CON_HDR;
    ww(nul, DOS_CON_HDR);
    ww(nul + 2, DOS_SEG);
    ww(nul + 4, 0x8004);
    mem_write(nul + 10, "NUL     ", 8);
    wd(con, 0xFFFFFFFF);
    ww(con + 4, 0x8013);
    mem_write(con + 10, "CON     ", 8);
    country_info(base + DOS_COUNTRY);
    wd(base + DOS_SFT, 0xFFFFFFFF);

    /* Upper case for 80h-FFh, as in code page 437's country data. */
    for (int i = 0; i < 128; i++)
        upcase_hi[i] = (uint8_t)(0x80 + i);
    static const uint8_t pairs[][2] = {
        { 0x81, 0x9A }, { 0x82, 0x90 }, { 0x83, 'A' }, { 0x84, 0x8E }, { 0x85, 'A' },
        { 0x86, 0x8F }, { 0x87, 0x80 }, { 0x88, 'E' }, { 0x89, 'E' }, { 0x8A, 'E' },
        { 0x8B, 'I' }, { 0x8C, 'I' }, { 0x8D, 'I' }, { 0x91, 0x92 }, { 0x93, 'O' },
        { 0x94, 0x99 }, { 0x95, 'O' }, { 0x96, 'U' }, { 0x97, 'U' }, { 0x98, 'Y' },
        { 0xA0, 'A' }, { 0xA1, 'I' }, { 0xA2, 'O' }, { 0xA3, 'U' }, { 0xA4, 0xA5 },
    };
    for (unsigned i = 0; i < sizeof(pairs) / sizeof(pairs[0]); i++)
        upcase_hi[pairs[i][0] - 0x80] = pairs[i][1];
    ww(base + DOS_UPCASE, 128);
    mem_write(base + DOS_UPCASE + 2, upcase_hi, 128);

    static const uint8_t terminators[] = {
        0x16, 0x00, 0x01, 0x00, 0xFF, 0x00, 0x00, 0x20, 0x02, 0x0E,
        '.', '"', '/', '\\', '[', ']', ':', '|', '<', '>', '+', '=', ';', ',',
    };
    mem_write(base + DOS_TERMINATORS, terminators, sizeof(terminators));
    ww(base + DOS_COLLATE, 256);
    for (int i = 0; i < 256; i++)
        wb(base + DOS_COLLATE + 2 + (uint32_t)i, (uint8_t)i);
    wb(base + DOS_MEDIA, 0xF8);
}

void dos_init(char **envp)
{
    (void)envp;
    dos_data();
    mcb_set(FIRST_MCB, 'Z', 0, (uint16_t)(MEM_TOP - FIRST_MCB - 1));

    /* The COMMAND.COM stand-in: its PSP, and the master environment. */
    uint16_t largest, env;
    mem_alloc(0x10, 8, &root_psp, &largest);       /* owner 8: DOS, until set */
    const char *vars[] = {
        "COMSPEC=C:\\COMMAND.COM", "PATH=C:\\", "PROMPT=$P$G", "TEMP=C:\\TMP", "TMP=C:\\TMP",
        sound_blaster_env(),            /* last: NULL without a Sound Blaster */
    };
    unsigned nvars = sizeof(vars) / sizeof(vars[0]) - (vars[5] ? 0 : 1);
    uint32_t len = 0;
    for (unsigned i = 0; i < nvars; i++)
        len += (uint32_t)strlen(vars[i]) + 1;
    mem_alloc((uint16_t)((len + 1 + 15) / 16 + 16), root_psp, &env, &largest);
    uint32_t e = ADDR(env, 0);
    for (unsigned i = 0; i < nvars; i++) {
        mem_write(e, vars[i], (uint32_t)strlen(vars[i]) + 1);
        e += (uint32_t)strlen(vars[i]) + 1;
    }
    wb(e, 0);
    ww(ADDR(root_psp - 1, 1), root_psp);
    mcb_name((uint16_t)(root_psp - 1), "COMMAND");
    mcb_name((uint16_t)(env - 1), "COMMAND");
    make_psp(root_psp, MEM_TOP, root_psp, env);
    files_setup_root(root_psp);
    dos_psp = root_psp;
    dta_seg = root_psp;
    dta_off = 0x80;
}

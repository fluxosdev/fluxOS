/*
 * The registry of the DLLs winrun provides, the thunks the program calls
 * them through, and the dispatcher those thunks reach (gate.S).
 *
 * A builtin DLL is also given a small image in the program's memory: a
 * PE header and an export directory whose entries are the thunks, so
 * GetProcAddress, and programs that read export tables themselves, work
 * on it as on a real DLL.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "win32.h"

uint32_t api_edx, api_ecx, api_ebx;
int api_ebx_valid;
double api_fret;
int api_fret_valid;
double api_farg[2];
int api_trace, api_strict;
struct call_frame *api_frame;

#define MAX_DLLS 48
static struct api_dll *dlls[MAX_DLLS];
static int ndlls;

/* Thunks of unresolved imports, by name. */
struct missing { char *name; gp_t thunk; int warned; };
static struct missing *missing;
static int nmissing, missing_cap;

/* The gate code in guest memory, and the thunk pages. */
static gp_t gate_guest;                 /* lcall flat_cs:api_gate_entry; ret 4 */
static gp_t thunk_pool;
static uint32_t thunk_left;

extern struct api_dll kernel32_dll, ntdll_dll, msvcrt_dll, user32_dll, gdi32_dll, advapi32_dll,
                      shell32_dll, ole32_dll, oleaut32_dll, winmm_dll, comctl32_dll, comdlg32_dll,
                      version_dll, winspool_dll, ws2_32_dll, imm32_dll, shlwapi_dll, rpcrt4_dll, uxtheme_dll,
                      dwmapi_dll, psapi_dll, crypt32_dll, secur32_dll, wininet_dll, mpr_dll, setupapi_dll;

static void api_register(struct api_dll *d)
{
    if (ndlls < MAX_DLLS)
        dlls[ndlls++] = d;
}

void api_register_all(void)
{
    api_register(&kernel32_dll);
    api_register(&ntdll_dll);
    api_register(&msvcrt_dll);
    api_register(&user32_dll);
    api_register(&gdi32_dll);
    api_register(&advapi32_dll);
    api_register(&shell32_dll);
    api_register(&ole32_dll);
    api_register(&oleaut32_dll);
    api_register(&winmm_dll);
    api_register(&comctl32_dll);
    api_register(&comdlg32_dll);
    api_register(&version_dll);
    api_register(&winspool_dll);
    api_register(&ws2_32_dll);
    api_register(&imm32_dll);
    api_register(&shlwapi_dll);
    api_register(&rpcrt4_dll);
    api_register(&uxtheme_dll);
    api_register(&dwmapi_dll);
    api_register(&psapi_dll);
    api_register(&crypt32_dll);
    api_register(&secur32_dll);
    api_register(&wininet_dll);
    api_register(&mpr_dll);
    api_register(&setupapi_dll);
}

/* "msvcr71.dll", "api-ms-win-crt-stdio-l1-1-0.dll" and the like: which of
 * ours answers for it. */
static const char *canonical(const char *name, char *buf, size_t size)
{
    size_t n = strlen(name);
    if (n >= size)
        n = size - 1;
    memcpy(buf, name, n);
    buf[n] = 0;
    str_upper(buf);
    char *dot = strrchr(buf, '.');
    if (dot && (!strcmp(dot, ".DLL") || !strcmp(dot, ".EXE") || !strcmp(dot, ".DRV")))
        *dot = 0;
    const char *slash = path_basename(buf);
    if (slash != buf)
        memmove(buf, slash, strlen(slash) + 1);
    if (!strncmp(buf, "API-MS-WIN-CRT", 14) || !strncmp(buf, "MSVCR", 5) || !strcmp(buf, "UCRTBASE") ||
        !strncmp(buf, "VCRUNTIME", 9) || !strcmp(buf, "MSVCRTD") || !strcmp(buf, "CRTDLL") ||
        !strncmp(buf, "MSVCP", 5))
        return "MSVCRT";
    if (!strncmp(buf, "API-MS-WIN-CORE", 15) || !strncmp(buf, "API-MS-WIN-", 11) || !strcmp(buf, "KERNELBASE") ||
        !strncmp(buf, "EXT-MS-WIN-", 11))
        return "KERNEL32";
    return buf;
}

struct api_dll *api_find_dll(const char *name)
{
    char buf[128];
    const char *c = canonical(name, buf, sizeof(buf));
    for (int i = 0; i < ndlls; i++) {
        char dbuf[64];
        if (!strcmp(canonical(dlls[i]->name, dbuf, sizeof(dbuf)), c))
            return dlls[i];
    }
    return NULL;
}

static int cmp_entry(const void *a, const void *b)
{
    const struct api_entry *const *x = a, *const *y = b;
    return strcmp((*x)->name, (*y)->name);
}

/* The i-th entry of all the DLL's tables together, and the reverse. */
static const struct api_entry *entry_at(struct api_dll *d, int i)
{
    if (i < d->count)
        return &d->entries[i];
    i -= d->count;
    for (int t = 0; t < API_TABLES; t++) {
        if (i < d->counts[t])
            return &d->tables[t][i];
        i -= d->counts[t];
    }
    return NULL;
}

static int entry_index(struct api_dll *d, const struct api_entry *e)
{
    if (e >= d->entries && e < d->entries + d->count)
        return (int)(e - d->entries);
    int base = d->count;
    for (int t = 0; t < API_TABLES; t++) {
        if (d->tables[t] && e >= d->tables[t] && e < d->tables[t] + d->counts[t])
            return base + (int)(e - d->tables[t]);
        base += d->counts[t];
    }
    return -1;
}

static void prepare(struct api_dll *d)
{
    if (d->sorted)
        return;
    d->total = d->count;
    for (int t = 0; t < API_TABLES; t++)
        d->total += d->tables[t] ? d->counts[t] : 0;
    d->sorted = calloc((size_t)d->total, sizeof(*d->sorted));
    if (!d->sorted)
        wdie("out of memory");
    for (int i = 0; i < d->total; i++)
        d->sorted[i] = entry_at(d, i);
    qsort(d->sorted, (size_t)d->total, sizeof(*d->sorted), cmp_entry);
    if (d->init)
        d->init();
}

static const struct api_entry *find_sorted(struct api_dll *d, const char *name)
{
    int lo = 0, hi = d->total;
    while (lo < hi) {
        int mid = (lo + hi) / 2;
        int c = strcmp(name, d->sorted[mid]->name);
        if (!c)
            return d->sorted[mid];
        if (c < 0)
            hi = mid;
        else
            lo = mid + 1;
    }
    return NULL;
}

const struct api_entry *api_find(struct api_dll *d, const char *name)
{
    prepare(d);
    const struct api_entry *e = find_sorted(d, name);
    for (int depth = 0; e && (e->flags & API_ALIAS) && depth < 4; depth++)
        e = find_sorted(d, (const char *)e->fn);
    return e;
}

const struct api_entry *api_find_ordinal(struct api_dll *d, int ordinal)
{
    prepare(d);
    for (int i = 0; i < d->total; i++)
        if (d->sorted[i]->ordinal == ordinal)
            return api_find(d, d->sorted[i]->name);
    return NULL;
}

/* ---- thunks ---- */

static uint8_t *thunk_space(uint32_t n, gp_t *at)
{
    n = (n + 15) & ~15u;
    if (n > thunk_left) {
        thunk_pool = vm_alloc(0, 0x10000, PAGE_EXECUTE_READWRITE, MEM_PRIVATE | MEM_TOP_DOWN, 0);
        if (!thunk_pool)
            wdie("out of guest memory for thunks");
        thunk_left = 0x10000;
    }
    *at = thunk_pool;
    thunk_pool += n;
    thunk_left -= n;
    return GP(*at);
}

static void emit32(uint8_t **p, uint32_t v)
{
    memcpy(*p, &v, 4);
    *p += 4;
}

static void ensure_gate(void)
{
    if (gate_guest)
        return;
    gp_t at;
    uint8_t *p = thunk_space(32, &at);
    gate_guest = at;
    *p++ = 0x9A;                                /* lcall flat_cs:api_gate_entry */
    emit32(&p, (uint32_t)(uintptr_t)api_gate_entry);
    *p++ = (uint8_t)gate_flat_cs;
    *p++ = (uint8_t)(gate_flat_cs >> 8);
    *p++ = 0xC2;                                /* ret 4 */
    *p++ = 0x04;
    *p++ = 0x00;
    /* the return trampoline for callbacks: ljmp flat_cs:call_guest_back */
    gate_return_guest = at + 16;
    p = GP(gate_return_guest);
    *p++ = 0xEA;
    emit32(&p, (uint32_t)(uintptr_t)call_guest_back);
    *p++ = (uint8_t)gate_flat_cs;
    *p++ = (uint8_t)(gate_flat_cs >> 8);
}

/* push id; call gate; ret n — with the x87 arguments moved to the stack first. */
static gp_t make_thunk(uint32_t id, uint16_t nargs, uint16_t flags)
{
    ensure_gate();
    gp_t at;
    uint8_t *p = thunk_space(32, &at), *start = p;
    int fargs = (flags & API_FARG2) ? 2 : (flags & API_FARG1) ? 1 : 0;
    if (fargs) {
        *p++ = 0x83; *p++ = 0xEC; *p++ = (uint8_t)(8 * fargs);     /* sub esp, 8*n */
        if (fargs == 2) {
            *p++ = 0xDD; *p++ = 0x5C; *p++ = 0x24; *p++ = 0x08;     /* fstp qword [esp+8] (st0: the second) */
        }
        *p++ = 0xDD; *p++ = 0x1C; *p++ = 0x24;                      /* fstp qword [esp] */
    }
    *p++ = 0x68;                                /* push id */
    emit32(&p, id);
    *p++ = 0xE8;                                /* call gate */
    gp_t next = at + (gp_t)(p - start) + 4;
    emit32(&p, gate_guest - next);
    if (fargs) {
        *p++ = 0x83; *p++ = 0xC4; *p++ = (uint8_t)(8 * fargs);     /* add esp, 8*n */
    }
    if (nargs && !(flags & API_CDECL)) {
        *p++ = 0xC2;                            /* ret n*4 */
        *p++ = (uint8_t)(nargs * 4);
        *p++ = (uint8_t)((nargs * 4) >> 8);
    } else {
        *p++ = 0xC3;
    }
    return at;
}

static int dll_index(struct api_dll *d)
{
    for (int i = 0; i < ndlls; i++)
        if (dlls[i] == d)
            return i;
    return -1;
}

gp_t api_thunk(struct api_dll *d, const struct api_entry *e)
{
    prepare(d);
    if (e->flags & API_DATA)
        return e->data ? *e->data : 0;
    int ei = entry_index(d, e);
    static gp_t *thunks[MAX_DLLS];
    int di = dll_index(d);
    if (di < 0 || ei < 0)
        return 0;
    if (!thunks[di]) {
        thunks[di] = calloc((size_t)d->total, sizeof(gp_t));
        if (!thunks[di])
            wdie("out of memory");
    }
    if (!thunks[di][ei]) {
        if (e->flags & API_RAW) {
            gp_t at;
            uint8_t *p = thunk_space(e->nargs, &at);
            memcpy(p, (const void *)e->fn, e->nargs);
            thunks[di][ei] = at;
        } else {
            thunks[di][ei] = make_thunk((uint32_t)di << 16 | (uint32_t)ei, e->nargs, e->flags);
        }
    }
    return thunks[di][ei];
}

gp_t api_missing_thunk(const char *dll, const char *name)
{
    char full[300];
    char dbuf[64];
    snprintf(full, sizeof(full), "%s!%s", canonical(dll, dbuf, sizeof(dbuf)), name);
    for (int i = 0; i < nmissing; i++)
        if (!strcmp(missing[i].name, full))
            return missing[i].thunk;
    if (nmissing == missing_cap) {
        missing_cap = missing_cap ? missing_cap * 2 : 64;
        missing = realloc(missing, (size_t)missing_cap * sizeof(*missing));
        if (!missing)
            wdie("out of memory");
    }
    missing[nmissing].name = strdup(full);
    missing[nmissing].warned = 0;
    missing[nmissing].thunk = make_thunk(0xFFFF0000u | (uint32_t)nmissing, 0, API_CDECL);
    return missing[nmissing++].thunk;
}

void api_unimplemented(const char *what)
{
    if (api_strict)
        wdie("%s is not implemented", what);
    wlog("%s is not implemented", what);
}

void api_set_fret(double d)
{
    api_fret = d;
    api_fret_valid = 1;
}

uint32_t api_dispatch(uint32_t id, uint32_t *args, uint32_t *regs)
{
    struct call_frame *saved = api_frame;
    api_frame = (struct call_frame *)regs;
    api_edx = 0;
    api_fret_valid = 0;
    api_ebx_valid = 0;
    uint32_t r = 0;
    if ((id & 0xFFFF0000u) == 0xFFFF0000u) {
        struct missing *m = &missing[id & 0xFFFF];
        if (!m->warned || api_trace) {
            m->warned = 1;
            if (api_strict)
                wdie("%s is not implemented (called from %08x)", m->name, api_frame->ret_addr);
            wlog("%s is not implemented (called from %08x)", m->name, api_frame->ret_addr);
        }
    } else {
        struct api_dll *d = dlls[id >> 16];
        const struct api_entry *e = entry_at(d, (int)(id & 0xFFFF));
        if (api_trace)
            fprintf(stderr, "[%s!%s(%08x, %08x, %08x, %08x) from %08x]\n", d->name, e->name,
                    args[0], args[1], args[2], args[3], api_frame->ret_addr);
        if (e->flags & (API_FARG1 | API_FARG2)) {
            memcpy(&api_farg[0], args, 8);
            if (e->flags & API_FARG2)
                memcpy(&api_farg[1], args + 2, 8);
        }
        r = e->fn(args);
        if (api_trace && !api_fret_valid)
            fprintf(stderr, " = %08x\n", r);
    }
    api_frame = saved;
    return r;
}

/* ---- builtin DLL images ---- */

static const uint8_t dos_stub[64] = {
    'M', 'Z', 0x90, 0, 3, 0, 0, 0, 4, 0, 0, 0, 0xFF, 0xFF, 0, 0, 0xB8, 0, 0, 0, 0, 0, 0, 0, 0x40, 0,
    [0x3C] = 0x40, 0, 0, 0,
};

/* Build the image for d: returns its base and size. */
gp_t api_build_image(struct api_dll *d, uint32_t *size_out, uint32_t *export_rva, uint32_t *export_size)
{
    prepare(d);
    /* sizes: headers 0x400; export directory 40; names/ordinals/functions; strings; */
    uint32_t nstrings = 0, maxord = 0;
    for (int i = 0; i < d->total; i++) {
        nstrings += (uint32_t)strlen(d->sorted[i]->name) + 1;
        if (d->sorted[i]->ordinal > (int)maxord)
            maxord = (uint32_t)d->sorted[i]->ordinal;
    }
    uint32_t nfuncs = (uint32_t)d->total + maxord + 1;
    uint32_t dll_name_len = (uint32_t)strlen(d->name) + 1;
    uint32_t exp_off = 0x400;
    uint32_t funcs_off = exp_off + 40;
    uint32_t names_off = funcs_off + nfuncs * 4;
    uint32_t ords_off = names_off + (uint32_t)d->total * 4;
    uint32_t strings_off = ords_off + (uint32_t)d->total * 2;
    uint32_t end = strings_off + nstrings + dll_name_len;
    uint32_t size = (end + 0xFFF) & ~0xFFFu;
    gp_t base = vm_alloc(0, size, PAGE_EXECUTE_READ, MEM_IMAGE | MEM_TOP_DOWN, 0);
    if (!base)
        wdie("out of guest memory for %s", d->name);
    uint8_t *img = GP(base);
    memset(img, 0, size);
    memcpy(img, dos_stub, sizeof(dos_stub));
    uint8_t *nt = img + 0x40;
    memcpy(nt, "PE\0\0", 4);
    uint16_t machine = 0x14C, nsections = 1, opt_size = 224, chars = 0x210E;   /* DLL, executable, 32-bit, no relocs */
    memcpy(nt + 4, &machine, 2);
    memcpy(nt + 6, &nsections, 2);
    memcpy(nt + 20, &opt_size, 2);
    memcpy(nt + 22, &chars, 2);
    uint8_t *oh = nt + 24;
    uint16_t magic = 0x10B;
    memcpy(oh, &magic, 2);
    uint32_t v;
    v = base; memcpy(oh + 28, &v, 4);           /* ImageBase */
    v = 0x1000; memcpy(oh + 32, &v, 4);         /* SectionAlignment */
    v = 0x200; memcpy(oh + 36, &v, 4);          /* FileAlignment */
    uint16_t ver = 5; memcpy(oh + 40, &ver, 2); memcpy(oh + 48, &ver, 2);
    ver = 1; memcpy(oh + 42, &ver, 2); memcpy(oh + 50, &ver, 2);
    v = size; memcpy(oh + 56, &v, 4);           /* SizeOfImage */
    v = 0x400; memcpy(oh + 60, &v, 4);          /* SizeOfHeaders */
    uint16_t subsys = 3; memcpy(oh + 68, &subsys, 2);
    v = 16; memcpy(oh + 92, &v, 4);             /* NumberOfRvaAndSizes */
    v = exp_off; memcpy(oh + 96, &v, 4);        /* export directory */
    v = end - exp_off; memcpy(oh + 100, &v, 4);
    /* one section: .edata */
    uint8_t *sh = oh + 224;
    memcpy(sh, ".edata\0\0", 8);
    v = end - 0x400; memcpy(sh + 8, &v, 4);
    v = 0x400; memcpy(sh + 12, &v, 4);
    memcpy(sh + 16, &v, 4);
    memcpy(sh + 20, &v, 4);
    v = 0x40000040; memcpy(sh + 36, &v, 4);     /* initialized data, readable */

    /* the export directory */
    uint8_t *ed = img + exp_off;
    v = strings_off + nstrings; memcpy(ed + 12, &v, 4);     /* Name */
    v = 1; memcpy(ed + 16, &v, 4);                          /* Base */
    v = nfuncs; memcpy(ed + 20, &v, 4);
    v = (uint32_t)d->total; memcpy(ed + 24, &v, 4);
    v = funcs_off; memcpy(ed + 28, &v, 4);
    v = names_off; memcpy(ed + 32, &v, 4);
    v = ords_off; memcpy(ed + 36, &v, 4);
    memcpy(img + strings_off + nstrings, d->name, dll_name_len);
    /* functions: named ones take the slots after the explicit ordinals */
    uint32_t next_slot = maxord;                /* ordinal = slot + 1 */
    uint32_t soff = strings_off;
    for (int i = 0; i < d->total; i++) {
        const struct api_entry *e = d->sorted[i];
        const struct api_entry *target = api_find(d, e->name);
        gp_t addr = target ? api_thunk(d, target) : 0;
        uint32_t slot = e->ordinal ? (uint32_t)e->ordinal - 1 : next_slot++;
        uint32_t rva = addr ? addr - base : 0;
        /* an address outside the image is fine for a function pointer: it is not a forwarder */
        memcpy(img + funcs_off + slot * 4, &rva, 4);
        memcpy(img + names_off + (uint32_t)i * 4, &soff, 4);
        uint16_t o = (uint16_t)slot;
        memcpy(img + ords_off + (uint32_t)i * 2, &o, 2);
        size_t len = strlen(e->name) + 1;
        memcpy(img + soff, e->name, len);
        soff += (uint32_t)len;
    }
    *size_out = size;
    *export_rva = exp_off;
    *export_size = end - exp_off;
    return base;
}

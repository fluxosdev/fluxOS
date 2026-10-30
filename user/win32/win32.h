/*
 * winrun: runs Windows programs (32-bit Portable Executables, as Windows
 * 9x and NT run them) on fluxOS.
 *
 * The program runs natively. It keeps the addresses it was linked for:
 * its code and data segments are based at the start of user space
 * (SEG_WIN_CODE and SEG_WIN_DATA in <flux/abi.h>), so its address 0x400000
 * is linear 0x40400000, and the 2 GiB a Windows program expects lie over
 * fluxOS's user half. The loader (this program) lives at the bottom of the
 * same address space in the flat segments, and the two sides call each
 * other through gate.S: an imported function is a small thunk in guest
 * memory that far-calls into the loader, and the loader calls window
 * procedures and other callbacks by switching to the guest's segments.
 *
 * Guest pointers are plain 32-bit numbers (gp_t); GP() turns one into a
 * pointer the loader can use and GA() goes back. Everything the program
 * may look at (its heaps, the thread block, strings an API hands back)
 * lives in guest memory, which vmem.c manages, so that the same code also
 * runs on a Linux host for the tests (os_linux.c), where the loader's own
 * memory is outside the window.
 */
#ifndef WIN32_H
#define WIN32_H

#include <stdint.h>
#include <stddef.h>
#include <limits.h>
#include <flux/abi.h>

typedef uint32_t gp_t;                  /* a guest (program) address */

#define WIN_BASE WIN_SEG_BASE
#define GP(a)  ((void *)(uintptr_t)((gp_t)(a) + WIN_BASE))
#define GA(p)  ((gp_t)((uintptr_t)(p) - WIN_BASE))
/* A guest pointer that may be NULL. */
static inline void *gp(gp_t a) { return a ? GP(a) : NULL; }
static inline char *gstr(gp_t a) { return a ? (char *)GP(a) : NULL; }
static inline uint16_t *gwstr(gp_t a) { return a ? (uint16_t *)GP(a) : NULL; }

/* The guest's view of memory: where its addresses may be. */
#define GUEST_LOW   0x00010000u         /* nothing below (NULL page and more) */
#define GUEST_IMAGE 0x00400000u         /* where the loader's own memory ends */
#define GUEST_TEB   (WIN_TEB_LINEAR - WIN_BASE)
#define GUEST_HIGH  GUEST_TEB           /* the thread block tops the usable range */
#define GUEST_TOP   0x80000000u

/* ---- Windows types, as the program sees them ---- */
typedef uint32_t DWORD, HANDLE, HMODULE, HWND, HDC, HGDIOBJ, HINSTANCE, HMENU, HICON, HCURSOR, HBRUSH, HFONT, HBITMAP, HPEN, HRGN, HKEY, LPARAM, WPARAM, UINT, ULONG, BOOL, COLORREF, LRESULT, SIZE_T, HRESULT, HGLOBAL, HLOCAL;
typedef int32_t LONG, INT;
typedef uint16_t WORD, WCHAR, ATOM;
typedef uint8_t BYTE;
#define TRUE 1
#define FALSE 0
#define INVALID_HANDLE_VALUE 0xFFFFFFFFu
#define MAX_PATH 260

typedef struct { int32_t left, top, right, bottom; } RECT;
typedef struct { int32_t x, y; } POINT;
typedef struct { int32_t cx, cy; } SIZE;
typedef struct { uint32_t lo, hi; } FILETIME;
typedef struct { uint16_t year, month, dow, day, hour, minute, second, ms; } SYSTEMTIME;

/* The thread block (TEB), at fs:0. Offsets as on NT; what matters is what
 * compiled code reads directly: the exception chain at 0, the stack limits,
 * the self pointer at 0x18, the last error at 0x34, TLS at 0x2C and 0xE10,
 * the PEB at 0x30. */
struct teb {
    gp_t except_list;           /* 0x00: SEH chain (-1 ends it) */
    gp_t stack_base;            /* 0x04: highest stack address */
    gp_t stack_limit;           /* 0x08: lowest */
    gp_t subsystem_tib;         /* 0x0C */
    gp_t fiber_data;            /* 0x10 */
    gp_t arbitrary;             /* 0x14 */
    gp_t self;                  /* 0x18 */
    gp_t environment_pointer;   /* 0x1C */
    uint32_t process_id;        /* 0x20 */
    uint32_t thread_id;         /* 0x24 */
    gp_t rpc_handle;            /* 0x28 */
    gp_t tls_pointer;           /* 0x2C: ThreadLocalStoragePointer */
    gp_t peb;                   /* 0x30 */
    uint32_t last_error;        /* 0x34 */
    uint32_t critical_sections; /* 0x38 */
    gp_t csr_client;            /* 0x3C */
    gp_t win32_thread_info;     /* 0x40 */
    uint32_t user32_reserved[26];   /* 0x44 */
    uint32_t user_reserved[5];  /* 0xAC */
    gp_t wow32_reserved;        /* 0xC0 */
    uint32_t locale;            /* 0xC4 */
    uint32_t fp_software_status;    /* 0xC8 */
    uint32_t reserved1[54];     /* 0xCC */
    uint32_t exception_code;    /* 0x1A4 */
    uint8_t  reserved2[0xE10 - 0x1A8];
    gp_t tls_slots[64];         /* 0xE10 */
    uint8_t  reserved3[0xF94 - 0xF10];
    uint32_t last_status;       /* 0xF94? (LastStatusValue is at 0xBF4 on NT; keep it simple) */
    uint8_t  reserved4[0x1000 - 0xF98];
};

/* The process block (PEB), at the TEB's peb. Compiled code reads a few
 * fields: BeingDebugged at 2, ImageBaseAddress at 8, Ldr at 0xC,
 * ProcessParameters at 0x10, ProcessHeap at 0x18, OSMajorVersion at 0xA4. */
struct peb {
    uint8_t  inherited_address_space, read_image_file_exec_options, being_debugged, spare;
    gp_t     mutant;            /* 0x04 */
    gp_t     image_base;        /* 0x08 */
    gp_t     ldr;               /* 0x0C */
    gp_t     process_parameters;    /* 0x10 */
    gp_t     subsystem_data;    /* 0x14 */
    gp_t     process_heap;      /* 0x18 */
    gp_t     fast_peb_lock;     /* 0x1C */
    uint32_t reserved1[6];      /* 0x20 */
    uint32_t tls_bitmap_bits_dummy[1]; /* 0x38 */
    uint32_t reserved2[2];      /* 0x3C */
    gp_t     ansi_code_page_data;   /* 0x44? */
    uint32_t reserved3[0x64 / 4 - 0x48 / 4];    /* up to 0x64 */
    uint32_t number_of_processors;  /* 0x64 */
    uint32_t nt_global_flag;    /* 0x68 */
    uint32_t reserved4[13];     /* 0x6C */
    uint32_t os_major;          /* 0xA4 */
    uint32_t os_minor;          /* 0xA8 */
    uint16_t os_build, os_csd;  /* 0xAC */
    uint32_t os_platform_id;    /* 0xB0 */
    uint32_t image_subsystem;   /* 0xB4 */
    uint32_t image_subsystem_major; /* 0xB8 */
    uint32_t image_subsystem_minor; /* 0xBC */
    uint8_t  reserved5[0x1000 - 0xC0];
};

/* RTL_USER_PROCESS_PARAMETERS, enough of it. */
struct unicode_string { uint16_t len, max; gp_t buf; };
struct process_params {
    uint32_t max_length, length, flags, debug_flags;
    gp_t console_handle;
    uint32_t console_flags;
    gp_t std_in, std_out, std_err;
    struct unicode_string cur_dir;      /* 0x24: DosPath */
    gp_t cur_dir_handle;
    struct unicode_string dll_path;     /* 0x30 */
    struct unicode_string image_path;   /* 0x38 */
    struct unicode_string command_line; /* 0x40 */
    gp_t environment;                   /* 0x48 */
    uint32_t starting_x, starting_y, count_x, count_y, count_chars_x, count_chars_y, fill_attribute;
    uint32_t window_flags, show_window;
    struct unicode_string window_title, desktop, shell_info, runtime_data;
};

/* ---- the API registry (api.c) ---- */
typedef uint32_t (*api_fn)(uint32_t *a);

#define API_CDECL  0x01                 /* the caller pops the arguments */
#define API_FLOAT  0x02                 /* returns a double in ST(0) (set api_fret) */
#define API_DATA   0x04                 /* a variable: the import gets its address */
#define API_FARG1  0x08                 /* takes ST(0) as its argument (api_farg[0]) */
#define API_FARG2  0x10                 /* takes ST(1), ST(0) (api_farg[0], [1]) */
#define API_ALIAS  0x20                 /* name is "Other" in the same table */
#define API_RAW    0x40                 /* fn points at nargs bytes of x86 code to copy into guest memory */

struct api_entry {
    const char *name;
    api_fn fn;
    uint16_t nargs;                     /* dwords the callee pops (stdcall) */
    uint16_t flags;
    gp_t *data;                         /* API_DATA: the guest address, set at init */
    int ordinal;                        /* 0: none assigned */
};

#define API_TABLES 3
struct api_dll {
    const char *name;                   /* "KERNEL32.DLL", in upper case */
    const struct api_entry *entries;    /* the first table (more in tables[]) */
    int count;
    void (*init)(void);                 /* before the first use: sets up API_DATA variables */
    const struct api_entry *tables[API_TABLES];     /* further tables, from other files */
    int counts[API_TABLES];
    /* filled in by the loader */
    struct module *module;
    const struct api_entry **sorted;
    int total;
};

#define DEF(nm) static uint32_t api_##nm(uint32_t *a)
#define API(nm, n)      { #nm, api_##nm, (n), 0, NULL, 0 }
#define APIC(nm)        { #nm, api_##nm, 0, API_CDECL, NULL, 0 }
#define APIF(nm)        { #nm, api_##nm, 0, API_CDECL | API_FLOAT, NULL, 0 }
#define APIF1(nm)       { #nm, api_##nm, 0, API_CDECL | API_FLOAT | API_FARG1, NULL, 0 }
#define APIF2(nm)       { #nm, api_##nm, 0, API_CDECL | API_FLOAT | API_FARG2, NULL, 0 }
#define DATA(nm, var)   { #nm, NULL, 0, API_DATA, &(var), 0 }
#define ALIAS(nm, of)   { #nm, (api_fn)#of, 0, API_ALIAS, NULL, 0 }
#define ORD(n, nm, args) { #nm, api_##nm, (args), 0, NULL, (n) }
#define RAW(nm, bytes)  { nm, (api_fn)(bytes), sizeof(bytes), API_RAW, NULL, 0 }
#define XDEF(nm) uint32_t api_##nm(uint32_t *a)     /* a handler another file's table lists */

extern struct api_dll kernel32_dll, ntdll_dll, msvcrt_dll, user32_dll, gdi32_dll;

extern uint32_t api_edx;                /* the second half of a 64-bit result */
extern uint32_t api_ecx, api_ebx;       /* for the few functions that return more (set api_ebx_valid) */
extern int api_ebx_valid;
extern double api_fret;                 /* a floating-point result */
extern int api_fret_valid;
extern double api_farg[2];              /* floating-point arguments taken off the x87 stack */
extern int api_trace;                   /* WINRUN_DEBUG: log every call */
extern int api_strict;                  /* WINRUN_STRICT: stop at the first unimplemented call */

uint32_t api_dispatch(uint32_t id, uint32_t *args, uint32_t *regs);
void api_register_all(void);
struct api_dll *api_find_dll(const char *name);
const struct api_entry *api_find(struct api_dll *d, const char *name);
const struct api_entry *api_find_ordinal(struct api_dll *d, int ordinal);
/* The thunk for an entry of a DLL (made on first use), or for a missing name. */
gp_t api_thunk(struct api_dll *d, const struct api_entry *e);
gp_t api_missing_thunk(const char *dll, const char *name);
void api_unimplemented(const char *what);
void api_set_fret(double d);

/* ---- gate.S ---- */
extern uint16_t gate_win_cs, gate_win_ds, gate_win_fs, gate_flat_cs, gate_flat_ds;
extern gp_t gate_return_guest;          /* the guest address of the return trampoline */
void api_gate_entry(void);
void call_guest_back(void);
uint32_t call_guest_on(gp_t fn, int nargs, const uint32_t *args, uint32_t new_stack_linear);
static inline uint32_t call_guest(gp_t fn, int nargs, const uint32_t *args)
{
    return call_guest_on(fn, nargs, args, 0);
}
uint32_t call_guest1(gp_t fn, uint32_t a);
uint32_t call_guest2(gp_t fn, uint32_t a, uint32_t b);
uint32_t call_guest3(gp_t fn, uint32_t a, uint32_t b, uint32_t c);
uint32_t call_guest4(gp_t fn, uint32_t a, uint32_t b, uint32_t c, uint32_t d);
/* Continue the guest with these registers (longjmp, exception handlers): never returns. */
struct guest_regs { uint32_t edi, esi, ebp, esp, ebx, edx, ecx, eax, eip, eflags; };
void guest_resume(const struct guest_regs *r) __attribute__((noreturn));
/* The registers the current API call was made with: ebx, esi, edi, ebp as the
 * gate saved them, and where the return address lies. */
struct call_frame { uint32_t ebp, edi, esi, ebx; gp_t ret_addr; gp_t args; uint32_t edx, ecx; };
extern struct call_frame *api_frame;

/* ---- vmem.c: the guest's address space ---- */
#define MEM_COMMIT  0x1000
#define MEM_RESERVE 0x2000
#define MEM_DECOMMIT 0x4000
#define MEM_RELEASE 0x8000
#define MEM_FREE    0x10000
#define MEM_PRIVATE 0x20000
#define MEM_MAPPED  0x40000
#define MEM_TOP_DOWN 0x100000
#define MEM_IMAGE   0x1000000
#define PAGE_NOACCESS 0x01
#define PAGE_READONLY 0x02
#define PAGE_READWRITE 0x04
#define PAGE_WRITECOPY 0x08
#define PAGE_EXECUTE 0x10
#define PAGE_EXECUTE_READ 0x20
#define PAGE_EXECUTE_READWRITE 0x40
#define PAGE_GUARD 0x100

void vm_init(gp_t loader_end);
/* Reserve (and commit) size bytes at addr, or anywhere if addr is 0 (or
 * taken, unless fixed). Returns the address, or 0. type: MEM_PRIVATE/IMAGE/MAPPED. */
gp_t vm_alloc(gp_t addr, uint32_t size, uint32_t protect, uint32_t type, int fixed);
int vm_free(gp_t addr, uint32_t size);          /* release a whole allocation */
int vm_commit(gp_t addr, uint32_t size, uint32_t protect);  /* within a reservation: marks it */
int vm_decommit(gp_t addr, uint32_t size);
int vm_protect(gp_t addr, uint32_t size, uint32_t protect, uint32_t *old);
struct vm_info { gp_t base, alloc_base; uint32_t alloc_protect, size, state, protect, type; };
int vm_query(gp_t addr, struct vm_info *out);
int vm_readable(gp_t addr, uint32_t size);
int vm_writable(gp_t addr, uint32_t size);
/* Guest memory for the loader's own use (strings handed to the program...). */
gp_t vm_alloc_loader(uint32_t size);

/* ---- heap.c: Windows heaps in guest memory ---- */
gp_t heap_create(uint32_t initial, uint32_t max, uint32_t flags);
int heap_destroy(gp_t heap);
gp_t heap_alloc(gp_t heap, uint32_t size, int zero);
int heap_free(gp_t heap, gp_t p);
gp_t heap_realloc(gp_t heap, gp_t p, uint32_t size, int zero, int in_place);
uint32_t heap_size(gp_t heap, gp_t p);
int heap_validate(gp_t heap, gp_t p);
extern gp_t process_heap;
/* Strings in the process heap (freed with heap_free(process_heap, ...)). */
gp_t gstrdup(const char *s);
gp_t gwstrdup(const uint16_t *s);
gp_t galloc(uint32_t size);             /* zeroed, from the process heap */
void gfree(gp_t p);

/* ---- pe.c and loader.c: modules ---- */
struct module {
    char name[64];                      /* "HELLO.EXE", upper case, no path */
    char path[PATH_MAX];                /* host path, or "" for a builtin */
    char winpath[MAX_PATH];             /* "C:\...\hello.exe" */
    gp_t base;
    uint32_t size;                      /* SizeOfImage */
    gp_t entry;
    int builtin;
    struct api_dll *dll;                /* builtin: its table */
    int refs;
    int attached;                       /* DllMain(DLL_PROCESS_ATTACH) ran */
    int is_exe;
    uint16_t subsystem;
    uint32_t stack_reserve, stack_commit, heap_reserve, heap_commit;
    uint32_t checksum;
    /* data directories (RVA, size) */
    uint32_t dir[16][2];
    int tls_index;                      /* -1: none */
    struct module *next;
};
#define DIR_EXPORT 0
#define DIR_IMPORT 1
#define DIR_RESOURCE 2
#define DIR_EXCEPTION 3
#define DIR_SECURITY 4
#define DIR_BASERELOC 5
#define DIR_DEBUG 6
#define DIR_TLS 9
#define DIR_LOADCONFIG 10
#define DIR_BOUND_IMPORT 11
#define DIR_IAT 12
#define DIR_DELAY_IMPORT 13

extern struct module *modules;          /* the EXE first */
extern struct module *main_module;

/* Map the file into guest memory at its base (or relocated); 0 on success,
 * else a negative errno-ish value, with a message in err. */
int pe_map(const char *path, struct module *m, char *err, size_t errsize);
int pe_resolve_imports(struct module *m, char *err, size_t errsize);
gp_t pe_export(struct module *m, const char *name, int ordinal);  /* 0 if none */
void pe_run_tls_callbacks(struct module *m, uint32_t reason);
int pe_setup_tls(struct module *m);
/* Is the file a PE? (its subsystem in *subsystem, if asked) */
int pe_file_info(const char *path, uint16_t *subsystem, int *is_dll);
/* Resources: the data of a resource (type and name each an integer id, or a
 * name), and its size. The first language is taken. */
gp_t pe_find_resource(struct module *m, uint32_t type_id, const uint16_t *type_name,
                      uint32_t name_id, const uint16_t *name_name, uint32_t *size);
/* The address of a DLL's export: a builtin's thunk, or a loaded DLL's
 * function. Loads the DLL if need be; 0 if there is no such thing. */
gp_t loader_resolve(const char *dll, const char *name, int ordinal, const char *from_dir);

struct module *module_find(const char *name);           /* by name, with or without ".dll" */
struct module *module_by_base(gp_t base);
struct module *module_load(const char *name, const char *from_dir, char *err, size_t errsize);
int module_unload(struct module *m);
struct module *module_builtin(struct api_dll *d);
void module_attach_all(void);           /* DllMain(DLL_PROCESS_ATTACH) in load order */
void module_detach_all(void);
/* Windows names of the system folders and the program's own folder. */
extern char exe_dir_host[PATH_MAX];

/* ---- paths.c ---- */
/* "C:\foo\bar" -> "/foo/bar" (into out, size PATH_MAX): 0, or -1 for a drive we have not got. */
int path_to_host(const char *win, char *out);
/* "/foo/bar" -> "C:\foo\bar" */
void path_to_win(const char *host, char *out, size_t size);
/* Case-insensitive lookup: the real spelling of a path, if the exact one does not exist. */
int path_fix_case(char *path);
const char *path_basename(const char *p);

/* ---- strings ---- */
int wstrlen(const uint16_t *s);
void wstrcpy(uint16_t *d, const uint16_t *s);
int wstrcmp(const uint16_t *a, const uint16_t *b);
int wstrcasecmp(const uint16_t *a, const uint16_t *b);
/* ANSI (bytes, Latin-1) <-> UTF-16, with the terminating zero. */
int ansi_to_wide(const char *s, int n, uint16_t *out, int outlen);   /* n < 0: to the NUL */
int wide_to_ansi(const uint16_t *s, int n, char *out, int outlen);
int utf8_to_wide(const char *s, int n, uint16_t *out, int outlen);
int wide_to_utf8(const uint16_t *s, int n, char *out, int outlen);
/* A temporary ANSI copy of a wide guest string (rotating buffers). */
const char *wtmp(gp_t wstr);
char *wtmp_n(const uint16_t *w, int n);
void str_upper(char *s);
void str_lower(char *s);

/* ---- process state (loader.c, kernel32.c) ---- */
extern struct teb *teb;
extern struct peb *peb;
extern gp_t teb_gp, peb_gp;
extern gp_t cmdline_gp, cmdline_w_gp;   /* GetCommandLineA/W */
extern char cmdline[4096];
extern int show_cmd;                    /* STARTUPINFO wShowWindow */
extern uint32_t last_error;             /* mirrored in the TEB */
void set_last_error(uint32_t e);
uint32_t get_last_error(void);
extern gp_t guest_stack_base, guest_stack_limit;
extern uint32_t exit_code;
void process_exit(uint32_t code) __attribute__((noreturn));
void env_init(char **envp);
gp_t env_block_ansi(void);              /* "A=B\0C=D\0\0" in guest memory (fresh) */
gp_t env_block_wide(void);
const char *env_get(const char *name);
int env_set(const char *name, const char *value);   /* NULL removes */
extern int env_generation;             /* bumped on every change */
int env_count(void);
const char *env_nth(int i);             /* "NAME=value" */

/* ---- handles.c ---- */
enum htype { H_NONE, H_FILE, H_FIND, H_EVENT, H_MUTEX, H_SEMAPHORE, H_PROCESS, H_THREAD, H_PIPE_R, H_PIPE_W,
             H_MAPPING, H_CONSOLE_IN, H_CONSOLE_OUT, H_TIMER, H_KEY, H_CHANGE, H_HEAP, H_SNAPSHOT };
struct hobj {
    enum htype type;
    int refs;
    int fd;                             /* files, pipes, consoles */
    char *path;                         /* files: host path */
    uint32_t access, share, flags;      /* files: as opened */
    int append;
    int console;                        /* the handle is a terminal */
    int signaled, manual;               /* events */
    int count, max;                     /* semaphores, mutex recursion */
    int pid, exited;                    /* processes */
    uint32_t exit_code;
    uint32_t thread_id;
    void *priv;                         /* finds, mappings... */
    uint64_t pos_hint;
};
struct mapping { int fd; uint32_t protect; uint32_t size; gp_t anon; };     /* H_MAPPING's priv */
HANDLE handle_new(enum htype type);
struct hobj *handle_get(HANDLE h);                      /* NULL if bad */
struct hobj *handle_get_type(HANDLE h, enum htype type);
int handle_close(HANDLE h);
HANDLE handle_dup(HANDLE h);
HANDLE handle_for_fd(int fd, enum htype type, int console);
extern HANDLE std_handles[3];
#define PSEUDO_PROCESS 0xFFFFFFFFu
#define PSEUDO_THREAD  0xFFFFFFFEu

/* ---- console (kernel32.c) ---- */
int console_write(int fd, const char *s, uint32_t n);
int console_read(int fd, char *buf, uint32_t n);
int console_raw(int on);                /* raw (no echo, no line editing) input mode */
int console_kbhit(void);
int console_getch(void);

/* ---- SEH (seh.c) ---- */
#define EXCEPTION_ACCESS_VIOLATION 0xC0000005u
#define EXCEPTION_BREAKPOINT 0x80000003u
#define EXCEPTION_SINGLE_STEP 0x80000004u
#define EXCEPTION_ILLEGAL_INSTRUCTION 0xC000001Du
#define EXCEPTION_INT_DIVIDE_BY_ZERO 0xC0000094u
#define EXCEPTION_FLT_DIVIDE_BY_ZERO 0xC000008Eu
#define EXCEPTION_FLT_INVALID_OPERATION 0xC0000090u
#define EXCEPTION_STACK_OVERFLOW 0xC00000FDu
#define EXCEPTION_NONCONTINUABLE 0x1
#define EXCEPTION_UNWINDING 0x2
#define EXCEPTION_EXIT_UNWIND 0x4
#define EXCEPTION_NONCONTINUABLE_EXCEPTION 0xC0000025u
#define STATUS_INVALID_DISPOSITION 0xC0000026u
#define EXCEPTION_MAXIMUM_PARAMETERS 15
struct exception_record {
    uint32_t code, flags;
    gp_t record;                        /* nested */
    gp_t address;
    uint32_t nparams;
    uint32_t params[EXCEPTION_MAXIMUM_PARAMETERS];
};
/* CONTEXT (x86), 716 bytes */
struct context {
    uint32_t flags;
    uint32_t dr0, dr1, dr2, dr3, dr6, dr7;
    uint8_t  float_save[112];
    uint32_t gs, fs, es, ds;
    uint32_t edi, esi, ebx, edx, ecx, eax;
    uint32_t ebp, eip, cs, eflags, esp, ss;
    uint8_t  ext[512];
};
#define CONTEXT_FULL 0x10007
/* Dispatch a software exception raised by the program (RaiseException): calls
 * the handlers on the fs:0 chain; returns only if one wants execution to go
 * on (the context then describes where). */
void seh_raise(struct exception_record *rec, struct context *ctx);
/* A fault in guest code: the registers are those at the fault. Never returns. */
void seh_fault(uint32_t code, gp_t fault_addr, int write, const struct guest_regs *regs, uint32_t fs) __attribute__((noreturn));
void seh_unwind(gp_t target_frame, gp_t target_ip, struct exception_record *rec, uint32_t retval, struct context *ctx);
extern gp_t unhandled_filter;           /* SetUnhandledExceptionFilter */
extern gp_t vectored_handlers[8];
extern int nvectored;
/* The FS value the guest runs with (the based selector), for CONTEXT records. */
uint32_t guest_fs(void);

/* ---- the C runtime (msvcrt.c) ---- */
void crt_flush_all(void);               /* at exit: buffered output goes out */
void crt_run_atexit(void);              /* the program's atexit/_onexit functions */

/* ---- api.c ---- */
gp_t api_build_image(struct api_dll *d, uint32_t *size_out, uint32_t *export_rva, uint32_t *export_size);
void process_init(const char *exe_host_path, int argc, char **argv, char **envp);
void process_run(void) __attribute__((noreturn));

/* ---- logging ---- */
void wlog(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void wdie(const char *fmt, ...) __attribute__((format(printf, 1, 2), noreturn));
void wtrace(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/* ---- Win32 structures shared by more than one file ---- */
#define GENERIC_READ  0x80000000u
#define GENERIC_WRITE 0x40000000u
#define GENERIC_EXECUTE 0x20000000u
#define GENERIC_ALL 0x10000000u
#define FILE_SHARE_READ 1
#define FILE_SHARE_WRITE 2
#define CREATE_NEW 1
#define CREATE_ALWAYS 2
#define OPEN_EXISTING 3
#define OPEN_ALWAYS 4
#define TRUNCATE_EXISTING 5
#define FILE_ATTRIBUTE_READONLY 0x1
#define FILE_ATTRIBUTE_HIDDEN 0x2
#define FILE_ATTRIBUTE_SYSTEM 0x4
#define FILE_ATTRIBUTE_DIRECTORY 0x10
#define FILE_ATTRIBUTE_ARCHIVE 0x20
#define FILE_ATTRIBUTE_DEVICE 0x40
#define FILE_ATTRIBUTE_NORMAL 0x80
#define FILE_ATTRIBUTE_TEMPORARY 0x100
#define FILE_FLAG_OVERLAPPED 0x40000000u
#define INVALID_FILE_ATTRIBUTES 0xFFFFFFFFu

#define ERROR_SUCCESS 0
#define ERROR_INVALID_FUNCTION 1
#define ERROR_FILE_NOT_FOUND 2
#define ERROR_PATH_NOT_FOUND 3
#define ERROR_TOO_MANY_OPEN_FILES 4
#define ERROR_ACCESS_DENIED 5
#define ERROR_INVALID_HANDLE 6
#define ERROR_NOT_ENOUGH_MEMORY 8
#define ERROR_BAD_FORMAT 11
#define ERROR_INVALID_ACCESS 12
#define ERROR_INVALID_DATA 13
#define ERROR_OUTOFMEMORY 14
#define ERROR_INVALID_DRIVE 15
#define ERROR_CURRENT_DIRECTORY 16
#define ERROR_NOT_SAME_DEVICE 17
#define ERROR_NO_MORE_FILES 18
#define ERROR_WRITE_PROTECT 19
#define ERROR_NOT_READY 21
#define ERROR_BAD_COMMAND 22
#define ERROR_CRC 23
#define ERROR_SEEK 25
#define ERROR_WRITE_FAULT 29
#define ERROR_READ_FAULT 30
#define ERROR_GEN_FAILURE 31
#define ERROR_SHARING_VIOLATION 32
#define ERROR_LOCK_VIOLATION 33
#define ERROR_HANDLE_EOF 38
#define ERROR_HANDLE_DISK_FULL 39
#define ERROR_NOT_SUPPORTED 50
#define ERROR_FILE_EXISTS 80
#define ERROR_CANNOT_MAKE 82
#define ERROR_INVALID_PARAMETER 87
#define ERROR_BROKEN_PIPE 109
#define ERROR_OPEN_FAILED 110
#define ERROR_BUFFER_OVERFLOW 111
#define ERROR_DISK_FULL 112
#define ERROR_CALL_NOT_IMPLEMENTED 120
#define ERROR_SEM_TIMEOUT 121
#define ERROR_INSUFFICIENT_BUFFER 122
#define ERROR_INVALID_NAME 123
#define ERROR_MOD_NOT_FOUND 126
#define ERROR_PROC_NOT_FOUND 127
#define ERROR_NEGATIVE_SEEK 131
#define ERROR_DIR_NOT_EMPTY 145
#define ERROR_BAD_PATHNAME 161
#define ERROR_BUSY 170
#define ERROR_ALREADY_EXISTS 183
#define ERROR_BAD_EXE_FORMAT 193
#define ERROR_ENVVAR_NOT_FOUND 203
#define ERROR_FILENAME_EXCED_RANGE 206
#define ERROR_MORE_DATA 234
#define ERROR_NO_MORE_ITEMS 259
#define ERROR_DIRECTORY 267
#define ERROR_INVALID_ADDRESS 487
#define ERROR_IO_PENDING 997
#define ERROR_NOACCESS 998
#define ERROR_INVALID_WINDOW_HANDLE 1400
#define ERROR_CLASS_ALREADY_EXISTS 1410
#define ERROR_CLASS_DOES_NOT_EXIST 1411
#define ERROR_CANNOT_FIND_WND_CLASS 1407
#define ERROR_RESOURCE_TYPE_NOT_FOUND 1813
#define ERROR_RESOURCE_NAME_NOT_FOUND 1814
#define ERROR_NO_UNICODE_TRANSLATION 1113
#define ERROR_NOT_OWNER 288
#define ERROR_INVALID_FLAGS 1004
#define ERROR_INVALID_THREAD_ID 1444
#define ERROR_FILE_INVALID 1006
#define WAIT_OBJECT_0 0
#define WAIT_TIMEOUT 0x102
#define WAIT_FAILED 0xFFFFFFFFu
#define INFINITE 0xFFFFFFFFu
#define STILL_ACTIVE 259

/* The error code for errno (os.c) */
uint32_t errno_to_win(int e);

/* ---- user32.c / gdi32.c (GUI) ---- */
int gui_available(void);                /* the desktop's window server answered */
int gui_pump(int block);                /* handle server messages; 1 if any came */
void gui_exit(void);                    /* the program is ending: close its windows */
int gui_wait_ms(int ms);                /* wait for input or a timer, at most ms */
void gui_timers_due(void);
int gui_next_timer_ms(void);
extern int gui_connected;

#endif

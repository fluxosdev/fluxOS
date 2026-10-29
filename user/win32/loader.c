/*
 * Modules: the program, the DLLs it brings with it (loaded from files, as
 * pe.c does), and the DLLs winrun provides (api.c). Also the process:
 * its thread and process blocks, command line, environment and stack,
 * and running the program's entry point.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "win32.h"
#include "os.h"

#define DLL_PROCESS_ATTACH 1
#define DLL_PROCESS_DETACH 0

struct module *modules, *main_module;
char exe_dir_host[PATH_MAX];
struct teb *teb;
struct peb *peb;
gp_t teb_gp, peb_gp;
gp_t cmdline_gp, cmdline_w_gp;
char cmdline[4096];
int show_cmd = 1;
uint32_t last_error;
gp_t guest_stack_base, guest_stack_limit;
uint32_t exit_code;
static gp_t guest_stack_alloc;
static uint32_t guest_stack_size;
static int process_running;

/* ---- names ---- */

static void module_name_of(const char *path, char *out, size_t size)
{
    snprintf(out, size, "%s", path_basename(path));
    str_upper(out);
}

static int same_module_name(const char *a, const char *b)
{
    /* with or without the extension */
    char x[80], y[80];
    snprintf(x, sizeof(x), "%s", a);
    snprintf(y, sizeof(y), "%s", b);
    str_upper(x);
    str_upper(y);
    char *dx = strrchr(x, '.'), *dy = strrchr(y, '.');
    if (dx && !strcmp(dx, ".DLL")) *dx = 0;
    if (dy && !strcmp(dy, ".DLL")) *dy = 0;
    return !strcmp(x, y);
}

struct module *module_find(const char *name)
{
    char base[80];
    module_name_of(name, base, sizeof(base));
    for (struct module *m = modules; m; m = m->next)
        if (same_module_name(m->name, base))
            return m;
    struct api_dll *d = api_find_dll(name);
    if (d && d->module)
        return d->module;
    return NULL;
}

struct module *module_by_base(gp_t base)
{
    for (struct module *m = modules; m; m = m->next)
        if (m->base == base)
            return m;
    return NULL;
}

static void module_add(struct module *m)
{
    struct module **pp = &modules;
    while (*pp)
        pp = &(*pp)->next;
    m->next = NULL;
    *pp = m;
}

struct module *module_builtin(struct api_dll *d)
{
    if (d->module)
        return d->module;
    struct module *m = calloc(1, sizeof(*m));
    if (!m)
        wdie("out of memory");
    snprintf(m->name, sizeof(m->name), "%s", d->name);
    snprintf(m->winpath, sizeof(m->winpath), "C:\\WINDOWS\\SYSTEM32\\%s", d->name);
    uint32_t exp_rva, exp_size;
    m->base = api_build_image(d, &m->size, &exp_rva, &exp_size);
    m->dir[DIR_EXPORT][0] = exp_rva;
    m->dir[DIR_EXPORT][1] = exp_size;
    m->builtin = 1;
    m->dll = d;
    m->refs = 1;
    m->attached = 1;
    m->tls_index = -1;
    d->module = m;
    module_add(m);
    wtrace("builtin %s at %08x", d->name, m->base);
    return m;
}

/* Where a DLL file may be: beside the program, in the current folder, or
 * in C:\WINDOWS\SYSTEM32 (/windows/system32). */
static int find_dll_file(const char *name, const char *from_dir, char *out)
{
    char host[PATH_MAX];
    if (strchr(name, '\\') || strchr(name, ':') || strchr(name, '/')) {
        if (path_to_host(name, host) < 0)
            return -1;
        snprintf(out, PATH_MAX, "%s", host);
        return path_fix_case(out);
    }
    const char *dirs[5];
    int nd = 0;
    if (from_dir)
        dirs[nd++] = from_dir;
    if (exe_dir_host[0])
        dirs[nd++] = exe_dir_host;
    dirs[nd++] = ".";
    dirs[nd++] = "/windows/system32";
    dirs[nd++] = "/windows";
    for (int i = 0; i < nd; i++) {
        snprintf(out, PATH_MAX, "%s/%s", dirs[i], name);
        if (path_fix_case(out) == 0) {
            struct stat st;
            if (stat(out, &st) == 0 && S_ISREG(st.st_mode))
                return 0;
        }
    }
    return -1;
}

static void module_attach(struct module *m)
{
    if (m->attached || m->builtin)
        return;
    m->attached = 1;
    pe_run_tls_callbacks(m, DLL_PROCESS_ATTACH);
    if (!m->is_exe && m->entry) {
        wtrace("DllMain %s attach", m->name);
        uint32_t ok = call_guest3(m->entry, m->base, DLL_PROCESS_ATTACH, 0);
        if (!ok)
            wlog("%s: DllMain refused to initialize", m->name);
    }
}

struct module *module_load(const char *name, const char *from_dir, char *err, size_t errsize)
{
    struct module *m = module_find(name);
    if (m) {
        m->refs++;
        return m;
    }
    struct api_dll *d = api_find_dll(name);
    char path[PATH_MAX];
    /* a DLL of the program's own, beside it, comes before ours of the same name */
    int have_file = find_dll_file(name, from_dir, path) == 0;
    if (d && (!have_file || d == &kernel32_dll || d == &ntdll_dll || d == &user32_dll || d == &gdi32_dll))
        return module_builtin(d);
    if (!have_file) {
        snprintf(err, errsize, "cannot find %s", name);
        return NULL;
    }
    m = calloc(1, sizeof(*m));
    if (!m)
        wdie("out of memory");
    if (pe_map(path, m, err, errsize) < 0) {
        free(m);
        return NULL;
    }
    module_name_of(path, m->name, sizeof(m->name));
    path_to_win(path, m->winpath, sizeof(m->winpath));
    m->refs = 1;
    module_add(m);
    wtrace("loaded %s at %08x", m->name, m->base);
    if (pe_resolve_imports(m, err, errsize) < 0)
        return NULL;
    if (teb)
        pe_setup_tls(m);
    if (process_running)
        module_attach(m);
    return m;
}

int module_unload(struct module *m)
{
    if (!m || m->builtin || m == main_module)
        return 0;
    if (--m->refs > 0)
        return 0;
    if (m->attached && !m->is_exe && m->entry)
        call_guest3(m->entry, m->base, DLL_PROCESS_DETACH, 0);
    for (struct module **pp = &modules; *pp; pp = &(*pp)->next)
        if (*pp == m) {
            *pp = m->next;
            break;
        }
    vm_free(m->base, 0);
    free(m);
    return 0;
}

gp_t loader_resolve(const char *dll, const char *name, int ordinal, const char *from_dir)
{
    char err[256];
    struct module *m = module_find(dll);
    if (!m)
        m = module_load(dll, from_dir, err, sizeof(err));
    if (!m)
        return 0;
    if (m->builtin) {
        const struct api_entry *e = name ? api_find(m->dll, name) : api_find_ordinal(m->dll, ordinal);
        return e ? api_thunk(m->dll, e) : 0;
    }
    return pe_export(m, name, ordinal);
}

void module_attach_all(void)
{
    for (struct module *m = modules; m; m = m->next)
        if (!m->is_exe)
            module_attach(m);
}

void module_detach_all(void)
{
    for (struct module *m = modules; m; m = m->next)
        if (!m->builtin && !m->is_exe && m->attached && m->entry) {
            m->attached = 0;
            call_guest3(m->entry, m->base, DLL_PROCESS_DETACH, 0);
        }
}

/* ---- the environment ---- */

static char **env;
static int nenv, env_cap;

static int env_index(const char *name)
{
    size_t n = strlen(name);
    for (int i = 0; i < nenv; i++)
        if (!strncasecmp(env[i], name, n) && env[i][n] == '=')
            return i;
    return -1;
}

int env_generation;

int env_set(const char *name, const char *value)
{
    int i = env_index(name);
    env_generation++;
    if (!value) {
        if (i >= 0) {
            free(env[i]);
            env[i] = env[--nenv];
        }
        return 0;
    }
    char *e = malloc(strlen(name) + strlen(value) + 2);
    if (!e)
        return -1;
    sprintf(e, "%s=%s", name, value);
    if (i >= 0) {
        free(env[i]);
        env[i] = e;
        return 0;
    }
    if (nenv == env_cap) {
        env_cap = env_cap ? env_cap * 2 : 64;
        env = realloc(env, (size_t)env_cap * sizeof(*env));
        if (!env)
            wdie("out of memory");
    }
    env[nenv++] = e;
    return 0;
}

const char *env_get(const char *name)
{
    int i = env_index(name);
    return i < 0 ? NULL : strchr(env[i], '=') + 1;
}

int env_count(void)
{
    return nenv;
}

const char *env_nth(int i)
{
    return i >= 0 && i < nenv ? env[i] : NULL;
}

static int cmp_env(const void *a, const void *b)
{
    return strcasecmp(*(char *const *)a, *(char *const *)b);
}

void env_init(char **envp)
{
    for (int i = 0; envp && envp[i]; i++) {
        const char *eq = strchr(envp[i], '=');
        if (!eq || eq == envp[i])
            continue;
        char name[256];
        snprintf(name, sizeof(name), "%.*s", (int)(eq - envp[i]), envp[i]);
        if (!strcmp(name, "PATH")) {
            env_set("PATH", "C:\\WINDOWS\\SYSTEM32;C:\\WINDOWS;C:\\bin");
            continue;
        }
        env_set(name, eq + 1);
    }
    const char *home = env_get("HOME");
    char win[MAX_PATH];
    path_to_win(home ? home : "/", win, sizeof(win));
    static const char *const defaults[][2] = {
        { "SystemRoot", "C:\\WINDOWS" }, { "windir", "C:\\WINDOWS" }, { "SystemDrive", "C:" },
        { "TEMP", "C:\\tmp" }, { "TMP", "C:\\tmp" }, { "OS", "Windows_NT" },
        { "USERNAME", "flux" }, { "HOMEDRIVE", "C:" }, { "PROCESSOR_ARCHITECTURE", "x86" },
        { "PROCESSOR_IDENTIFIER", "x86 Family 15 Model 2 Stepping 4, GenuineIntel" },
        { "PROCESSOR_LEVEL", "15" }, { "PROCESSOR_REVISION", "0204" }, { "NUMBER_OF_PROCESSORS", "1" },
        { "ComSpec", "C:\\WINDOWS\\SYSTEM32\\CMD.EXE" }, { "PATHEXT", ".COM;.EXE;.BAT;.CMD" },
        { "PROMPT", "$P$G" }, { "ProgramFiles", "C:\\Program Files" },
        { "CommonProgramFiles", "C:\\Program Files\\Common Files" },
        { "ALLUSERSPROFILE", "C:\\home" }, { "APPDATA", NULL }, { "LOCALAPPDATA", NULL },
        { "USERPROFILE", NULL }, { "HOMEPATH", NULL }, { "COMPUTERNAME", NULL }, { "PATH", "C:\\WINDOWS\\SYSTEM32;C:\\WINDOWS;C:\\bin" },
    };
    char ncpu[8];
    snprintf(ncpu, sizeof(ncpu), "%d", os_ncpus());
    for (unsigned i = 0; i < sizeof(defaults) / sizeof(defaults[0]); i++) {
        const char *name = defaults[i][0], *value = defaults[i][1];
        if (env_get(name))
            continue;
        if (!strcmp(name, "USERPROFILE") || !strcmp(name, "APPDATA") || !strcmp(name, "LOCALAPPDATA"))
            value = win;
        else if (!strcmp(name, "HOMEPATH"))
            value = win + 2;
        else if (!strcmp(name, "COMPUTERNAME")) {
            static char host[64];
            snprintf(host, sizeof(host), "%s", os_hostname());
            str_upper(host);
            value = host;
        } else if (!strcmp(name, "NUMBER_OF_PROCESSORS"))
            value = ncpu;
        env_set(name, value);
    }
    qsort(env, (size_t)nenv, sizeof(*env), cmp_env);
}

gp_t env_block_ansi(void)
{
    uint32_t total = 1;
    for (int i = 0; i < nenv; i++)
        total += (uint32_t)strlen(env[i]) + 1;
    gp_t block = galloc(total + 1);
    char *p = GP(block);
    for (int i = 0; i < nenv; i++) {
        size_t n = strlen(env[i]) + 1;
        memcpy(p, env[i], n);
        p += n;
    }
    *p = 0;
    return block;
}

gp_t env_block_wide(void)
{
    uint32_t total = 1;
    for (int i = 0; i < nenv; i++)
        total += (uint32_t)strlen(env[i]) + 1;
    gp_t block = galloc((total + 1) * 2);
    uint16_t *p = GP(block);
    for (int i = 0; i < nenv; i++) {
        int n = ansi_to_wide(env[i], -1, p, (int)total);
        p += n;
    }
    *p = 0;
    return block;
}

/* ---- the process ---- */

void set_last_error(uint32_t e)
{
    last_error = e;
    if (teb)
        teb->last_error = e;
}

uint32_t get_last_error(void)
{
    return teb ? teb->last_error : last_error;
}

/* An argument as it goes on a Windows command line. */
static void quote_arg(const char *arg, char *out, size_t size)
{
    int needs = !*arg || strpbrk(arg, " \t\"") != NULL;
    size_t n = 0;
    if (needs && n < size - 1)
        out[n++] = '"';
    for (const char *p = arg; *p && n < size - 2; p++) {
        if (*p == '"') {
            if (n < size - 3)
                out[n++] = '\\';
            out[n++] = '"';
        } else if (*p == '\\') {
            /* backslashes before a quote (or the end, when quoted) are doubled */
            const char *q = p;
            while (*q == '\\')
                q++;
            int before_quote = *q == '"' || (needs && !*q);
            for (const char *s = p; s < q && n < size - 2; s++) {
                out[n++] = '\\';
                if (before_quote)
                    out[n++] = '\\';
            }
            p = q - 1;
        } else {
            out[n++] = *p;
        }
    }
    if (needs && n < size - 1)
        out[n++] = '"';
    out[n] = 0;
}

static void write_ustr(struct unicode_string *u, const char *s)
{
    int n = (int)strlen(s);
    gp_t buf = vm_alloc_loader((uint32_t)(n + 1) * 2);
    ansi_to_wide(s, -1, GP(buf), n + 1);
    u->len = (uint16_t)(n * 2);
    u->max = (uint16_t)((n + 1) * 2);
    u->buf = buf;
}

void process_init(const char *exe_host_path, int argc, char **argv, char **envp)
{
    /* the thread and process blocks, at the fixed place FS points at */
    if (vm_alloc(GUEST_TEB, WIN_TEB_SIZE, PAGE_READWRITE, MEM_PRIVATE, 1) != GUEST_TEB)
        wdie("cannot map the thread block at %08x", GUEST_TEB);
    teb_gp = GUEST_TEB;
    peb_gp = GUEST_TEB + 0x1000;
    teb = GP(teb_gp);
    peb = GP(peb_gp);
    memset(teb, 0, sizeof(*teb));
    memset(peb, 0, sizeof(*peb));
    teb->except_list = 0xFFFFFFFFu;
    teb->self = teb_gp;
    teb->peb = peb_gp;
    teb->process_id = (uint32_t)getpid();
    teb->thread_id = (uint32_t)getpid() * 4 + 4;
    teb->tls_pointer = vm_alloc_loader(64 * 4);
    teb->locale = 0x409;
    teb->last_error = 0;

    peb->image_base = main_module->base;
    peb->number_of_processors = (uint32_t)os_ncpus();
    peb->os_major = 5;
    peb->os_minor = 1;
    peb->os_build = 2600;
    peb->os_platform_id = 2;
    peb->image_subsystem = main_module->subsystem;
    peb->image_subsystem_major = 4;
    peb->process_parameters = GUEST_TEB + 0x2000;

    process_heap = heap_create(0, 0, 0);
    if (!process_heap)
        wdie("cannot create the process heap");
    peb->process_heap = process_heap;

    /* the command line: the program as Windows names it, then the arguments */
    char win[MAX_PATH];
    path_to_win(exe_host_path, win, sizeof(win));
    snprintf(main_module->winpath, sizeof(main_module->winpath), "%s", win);
    size_t n = 0;
    char q[1024];
    quote_arg(win, q, sizeof(q));
    n += (size_t)snprintf(cmdline + n, sizeof(cmdline) - n, "%s", q);
    for (int i = 1; i < argc && n < sizeof(cmdline) - 2; i++) {
        quote_arg(argv[i], q, sizeof(q));
        n += (size_t)snprintf(cmdline + n, sizeof(cmdline) - n, " %s", q);
    }
    cmdline_gp = vm_alloc_loader((uint32_t)strlen(cmdline) + 1);
    strcpy(GP(cmdline_gp), cmdline);
    cmdline_w_gp = vm_alloc_loader(((uint32_t)strlen(cmdline) + 1) * 2);
    ansi_to_wide(cmdline, -1, GP(cmdline_w_gp), (int)strlen(cmdline) + 1);

    env_init(envp);

    struct process_params *pp = GP(peb->process_parameters);
    memset(pp, 0, sizeof(*pp));
    pp->max_length = pp->length = sizeof(*pp);
    pp->flags = 1;                      /* normalized */
    write_ustr(&pp->command_line, cmdline);
    write_ustr(&pp->image_path, win);
    char cwd[PATH_MAX], wcwd[MAX_PATH];
    if (!getcwd(cwd, sizeof(cwd)))
        strcpy(cwd, "/");
    path_to_win(cwd, wcwd, sizeof(wcwd));
    write_ustr(&pp->cur_dir, wcwd);
    write_ustr(&pp->dll_path, "C:\\WINDOWS\\SYSTEM32;C:\\WINDOWS");
    write_ustr(&pp->window_title, win);
    write_ustr(&pp->desktop, "WinSta0\\Default");
    pp->environment = env_block_wide();
    pp->show_window = (uint32_t)show_cmd;
    pp->window_flags = 1;               /* STARTF_USESHOWWINDOW */

    /* the stack: what the program asked for, 1 MiB at least */
    guest_stack_size = main_module->stack_reserve;
    if (guest_stack_size < 0x100000)
        guest_stack_size = 0x100000;
    if (guest_stack_size > 0x4000000)
        guest_stack_size = 0x4000000;
    guest_stack_size = (guest_stack_size + 0xFFFF) & ~0xFFFFu;
    guest_stack_alloc = vm_alloc(0, guest_stack_size, PAGE_READWRITE, MEM_PRIVATE | MEM_TOP_DOWN, 0);
    if (!guest_stack_alloc)
        wdie("cannot allocate the stack");
    guest_stack_limit = guest_stack_alloc;
    guest_stack_base = guest_stack_alloc + guest_stack_size;
    teb->stack_base = guest_stack_base;
    teb->stack_limit = guest_stack_limit;

    /* standard handles */
    std_handles[0] = handle_for_fd(0, isatty(0) ? H_CONSOLE_IN : H_FILE, isatty(0));
    std_handles[1] = handle_for_fd(1, isatty(1) ? H_CONSOLE_OUT : H_FILE, isatty(1));
    std_handles[2] = handle_for_fd(2, isatty(2) ? H_CONSOLE_OUT : H_FILE, isatty(2));
    pp->std_in = std_handles[0];
    pp->std_out = std_handles[1];
    pp->std_err = std_handles[2];
}

void process_run(void)
{
    for (struct module *m = modules; m; m = m->next)
        if (!m->builtin)
            pe_setup_tls(m);
    process_running = 1;
    module_attach_all();
    pe_run_tls_callbacks(main_module, DLL_PROCESS_ATTACH);
    if (!main_module->entry)
        wdie("the program has no entry point");
    wtrace("entry %08x, stack %08x", main_module->entry, guest_stack_base);
    uint32_t r = call_guest_on(main_module->entry, 0, NULL, guest_stack_base - 64 + WIN_BASE);
    process_exit(r);
}

void process_exit(uint32_t code)
{
    static int exiting;
    if (!exiting) {
        exiting = 1;
        exit_code = code;
        crt_flush_all();
        gui_exit();
        module_detach_all();
    }
    fflush(stdout);
    fflush(stderr);
    _exit((int)(code & 0xFF));
}

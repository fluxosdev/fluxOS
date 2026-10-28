/*
 * winrun - run a Windows program.
 *
 *   winrun [-d] [-s] PROGRAM.EXE [ARGUMENTS...]
 *
 * The kernel runs Windows programs through this, so "./app.exe" and a
 * double-click in the File Manager work too. -d (or WINRUN_DEBUG=1 in the
 * environment) logs every call the program makes into the Windows API on
 * standard error; -s (WINRUN_STRICT=1) stops at the first call of a
 * function winrun does not provide, instead of returning zero from it.
 * Drive C: is the fluxOS root; see paths in util.c.
 */
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "win32.h"
#include "os.h"

static void usage(void)
{
    fprintf(stderr, "usage: winrun [-d] [-s] PROGRAM.EXE [ARGUMENTS...]\n");
    exit(2);
}

/* The program file: as given, in any case, or with .EXE added. */
static int find_program(const char *arg, char *out)
{
    char host[PATH_MAX];
    if (strchr(arg, '\\') || (arg[0] && arg[1] == ':'))
        path_to_host(arg, host);
    else
        snprintf(host, sizeof(host), "%s", arg);
    struct stat st;
    snprintf(out, PATH_MAX, "%s", host);
    if (path_fix_case(out) == 0 && stat(out, &st) == 0 && S_ISREG(st.st_mode))
        return 0;
    snprintf(out, PATH_MAX, "%s.exe", host);
    if (path_fix_case(out) == 0 && stat(out, &st) == 0 && S_ISREG(st.st_mode))
        return 0;
    return -1;
}

int main(int argc, char **argv, char **envp)
{
    int i = 1;
    if (getenv("WINRUN_DEBUG") && atoi(getenv("WINRUN_DEBUG")))
        api_trace = 1;
    if (getenv("WINRUN_STRICT") && atoi(getenv("WINRUN_STRICT")))
        api_strict = 1;
    for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
        if (!strcmp(argv[i], "-d"))
            api_trace = 1;
        else if (!strcmp(argv[i], "-s"))
            api_strict = 1;
        else if (!strcmp(argv[i], "--"))
            { i++; break; }
        else
            usage();
    }
    if (i >= argc)
        usage();
    char path[PATH_MAX];
    if (find_program(argv[i], path) < 0) {
        fprintf(stderr, "winrun: %s: %s\n", argv[i], strerror(errno == 0 ? ENOENT : errno));
        return 127;
    }
    uint16_t subsystem;
    int is_dll;
    if (pe_file_info(path, &subsystem, &is_dll) < 0) {
        fprintf(stderr, "winrun: %s: not a Windows program\n", path);
        return 126;
    }
    if (is_dll) {
        fprintf(stderr, "winrun: %s: a DLL, not a program\n", path);
        return 126;
    }

    os_init();
    api_register_all();
    uint32_t lend = os_loader_end();
    vm_init(lend > WIN_BASE ? lend - WIN_BASE : 0);

    struct module *m = calloc(1, sizeof(*m));
    if (!m)
        wdie("out of memory");
    char err[256];
    if (pe_map(path, m, err, sizeof(err)) < 0) {
        fprintf(stderr, "winrun: %s: %s\n", path, err);
        return 126;
    }
    snprintf(m->name, sizeof(m->name), "%s", path_basename(path));
    str_upper(m->name);
    m->refs = 1;
    m->is_exe = 1;
    modules = main_module = m;
    snprintf(exe_dir_host, sizeof(exe_dir_host), "%s", path);
    char *slash = strrchr(exe_dir_host, '/');
    if (slash)
        *slash = 0;
    else
        strcpy(exe_dir_host, ".");
    if (exe_dir_host[0] != '/') {
        char cwd[PATH_MAX], abs[PATH_MAX];
        if (getcwd(cwd, sizeof(cwd))) {
            snprintf(abs, sizeof(abs), "%s/%s", cwd, exe_dir_host);
            snprintf(exe_dir_host, sizeof(exe_dir_host), "%s", abs);
        }
    }
    char abspath[PATH_MAX];
    snprintf(abspath, sizeof(abspath), "%s/%s", exe_dir_host, path_basename(path));

    process_init(abspath, argc - i, argv + i, envp);
    if (pe_resolve_imports(m, err, sizeof(err)) < 0) {
        fprintf(stderr, "winrun: %s: %s\n", path, err);
        return 126;
    }
    os_install_fault_handlers();
    process_run();
}

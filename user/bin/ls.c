/* ls - list directory contents */
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

struct entry {
    char name[NAME_MAX + 1];
    struct stat st;
};

static int opt_all, opt_almost, opt_long, opt_one, opt_classify, opt_color;
static int opt_dir, opt_reverse, opt_size_sort, opt_time_sort, opt_human, opt_recursive;
static int status;

static int compare(const void *a, const void *b)
{
    const struct entry *x = a, *y = b;
    int r;
    if (opt_size_sort)
        r = (int)(y->st.st_size - x->st.st_size);
    else if (opt_time_sort)
        r = (int)(y->st.st_mtime - x->st.st_mtime);
    else
        r = 0;
    if (!r)
        r = strcmp(x->name, y->name);
    return opt_reverse ? -r : r;
}

static const char *color_of(const struct stat *st)
{
    if (!opt_color)
        return NULL;
    if (S_ISDIR(st->st_mode))
        return "\033[1;34m";
    if (S_ISCHR(st->st_mode) || S_ISBLK(st->st_mode))
        return "\033[1;33m";
    if (S_ISFIFO(st->st_mode))
        return "\033[33m";
    if (S_ISREG(st->st_mode) && (st->st_mode & 0111))
        return "\033[1;32m";
    return NULL;
}

static char type_suffix(const struct stat *st)
{
    if (!opt_classify)
        return 0;
    if (S_ISDIR(st->st_mode)) return '/';
    if (S_ISFIFO(st->st_mode)) return '|';
    if (S_ISREG(st->st_mode) && (st->st_mode & 0111)) return '*';
    return 0;
}

static int print_name(const struct entry *e)
{
    const char *c = color_of(&e->st);
    char suf = type_suffix(&e->st);
    if (c)
        printf("%s%s\033[0m", c, e->name);
    else
        fputs(e->name, stdout);
    if (suf)
        putchar(suf);
    return (int)strlen(e->name) + (suf ? 1 : 0);
}

static void mode_string(uint32_t m, char *s)
{
    s[0] = S_ISDIR(m) ? 'd' : S_ISCHR(m) ? 'c' : S_ISBLK(m) ? 'b' : S_ISFIFO(m) ? 'p' : '-';
    const char *rwx = "rwxrwxrwx";
    for (int i = 0; i < 9; i++)
        s[i + 1] = (m & (1u << (8 - i))) ? rwx[i] : '-';
    if (m & 01000)
        s[9] = (m & 1) ? 't' : 'T';
    s[10] = 0;
}

static void human(uint32_t n, char *buf, size_t size)
{
    if (n < 1024)
        snprintf(buf, size, "%u", n);
    else if (n < 1024 * 1024)
        snprintf(buf, size, "%u.%uK", n / 1024, (n % 1024) * 10 / 1024);
    else
        snprintf(buf, size, "%u.%uM", n / (1024 * 1024), (n % (1024 * 1024)) * 10 / (1024 * 1024));
}

static void print_long(const struct entry *e)
{
    char mode[12], when[32], size[24];
    mode_string(e->st.st_mode, mode);
    time_t t = (time_t)e->st.st_mtime;
    struct tm tm;
    gmtime_r(&t, &tm);
    strftime(when, sizeof(when), "%b %e %H:%M", &tm);
    if (S_ISCHR(e->st.st_mode) || S_ISBLK(e->st.st_mode))
        snprintf(size, sizeof(size), "%u, %u", MAJOR(e->st.st_rdev), MINOR(e->st.st_rdev));
    else if (opt_human)
        human(e->st.st_size, size, sizeof(size));
    else
        snprintf(size, sizeof(size), "%u", e->st.st_size);
    printf("%s %2u flux flux %7s %s ", mode, e->st.st_nlink, size, when);
    print_name(e);
    putchar('\n');
}

static void print_entries(struct entry *v, int n)
{
    if (opt_long) {
        for (int i = 0; i < n; i++)
            print_long(&v[i]);
        return;
    }
    if (opt_one) {
        for (int i = 0; i < n; i++) {
            print_name(&v[i]);
            putchar('\n');
        }
        return;
    }
    int maxw = 1;
    for (int i = 0; i < n; i++) {
        int w = (int)strlen(v[i].name) + (opt_classify ? 1 : 0);
        if (w > maxw)
            maxw = w;
    }
    int colw = maxw + 2;
    const char *c = getenv("COLUMNS");
    struct winsize ws;
    int width = c ? atoi(c) : ioctl(1, TIOCGWINSZ, &ws) == 0 ? ws.ws_col : 80;
    if (width < 20)
        width = 80;
    int cols = width / colw;
    if (cols < 1)
        cols = 1;
    int rows = (n + cols - 1) / cols;
    for (int r = 0; r < rows; r++) {
        for (int k = 0; k < cols; k++) {
            int i = k * rows + r;
            if (i >= n)
                break;
            int w = print_name(&v[i]);
            if ((k + 1) * rows + r < n)
                for (; w < colw; w++)
                    putchar(' ');
        }
        putchar('\n');
    }
}

static void list_dir(const char *path)
{
    DIR *d = opendir(path);
    if (!d) {
        fprintf(stderr, "ls: cannot access '%s': %s\n", path, strerror(errno));
        status = 2;
        return;
    }
    int cap = 32, n = 0;
    struct entry *v = malloc(sizeof(struct entry) * (size_t)cap);
    struct dirent *de;
    while ((de = readdir(d))) {
        if (de->d_name[0] == '.') {
            int dots = !strcmp(de->d_name, ".") || !strcmp(de->d_name, "..");
            if (!opt_all && !(opt_almost && !dots))
                continue;
        }
        if (n == cap) {
            cap *= 2;
            v = realloc(v, sizeof(struct entry) * (size_t)cap);
        }
        strlcpy(v[n].name, de->d_name, sizeof(v[n].name));
        char full[PATH_MAX];
        snprintf(full, sizeof(full), "%s/%s", path, de->d_name);
        if (stat(full, &v[n].st) < 0)
            memset(&v[n].st, 0, sizeof(v[n].st));
        n++;
    }
    closedir(d);
    qsort(v, (size_t)n, sizeof(struct entry), compare);
    if (opt_long) {
        uint32_t blocks = 0;
        for (int i = 0; i < n; i++)
            blocks += (v[i].st.st_size + 1023) / 1024;
        printf("total %u\n", blocks);
    }
    print_entries(v, n);
    if (opt_recursive) {
        for (int i = 0; i < n; i++) {
            if (!S_ISDIR(v[i].st.st_mode) || !strcmp(v[i].name, ".") || !strcmp(v[i].name, ".."))
                continue;
            char full[PATH_MAX];
            snprintf(full, sizeof(full), "%s/%s", path, v[i].name);
            printf("\n%s:\n", full);
            list_dir(full);
        }
    }
    free(v);
}

int main(int argc, char **argv)
{
    int i = 1;
    opt_color = isatty(1);
    opt_one = !isatty(1);
    for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
        if (!strcmp(argv[i], "--")) {
            i++;
            break;
        }
        if (!strcmp(argv[i], "--color") || !strcmp(argv[i], "--color=always")) {
            opt_color = 1;
            continue;
        }
        if (!strcmp(argv[i], "--color=never")) {
            opt_color = 0;
            continue;
        }
        for (const char *f = argv[i] + 1; *f; f++) {
            switch (*f) {
            case 'a': opt_all = 1; break;
            case 'A': opt_almost = 1; break;
            case 'l': opt_long = 1; break;
            case '1': opt_one = 1; break;
            case 'C': opt_one = 0; break;
            case 'F': opt_classify = 1; break;
            case 'd': opt_dir = 1; break;
            case 'r': opt_reverse = 1; break;
            case 'S': opt_size_sort = 1; break;
            case 't': opt_time_sort = 1; break;
            case 'h': opt_human = 1; break;
            case 'R': opt_recursive = 1; break;
            default:
                fprintf(stderr, "ls: invalid option -- '%c'\n", *f);
                return 2;
            }
        }
    }

    static char *dot[] = { ".", NULL };
    if (i == argc) {
        argv = dot;
        argc = 1;
        i = 0;
    }
    int nops = argc - i;

    struct entry *files = malloc(sizeof(struct entry) * (size_t)nops);
    int nfiles = 0;
    char **dirs = malloc(sizeof(char *) * (size_t)nops);
    int ndirs = 0;
    for (int k = i; k < argc; k++) {
        struct stat st;
        if (stat(argv[k], &st) < 0) {
            fprintf(stderr, "ls: cannot access '%s': %s\n", argv[k], strerror(errno));
            status = 2;
            continue;
        }
        if (S_ISDIR(st.st_mode) && !opt_dir) {
            dirs[ndirs++] = argv[k];
        } else {
            strlcpy(files[nfiles].name, argv[k], sizeof(files[nfiles].name));
            files[nfiles].st = st;
            nfiles++;
        }
    }
    if (nfiles) {
        qsort(files, (size_t)nfiles, sizeof(struct entry), compare);
        print_entries(files, nfiles);
    }
    for (int k = 0; k < ndirs; k++) {
        if (nfiles || ndirs > 1 || opt_recursive)
            printf("%s%s:\n", (nfiles || k) ? "\n" : "", dirs[k]);
        list_dir(dirs[k]);
    }
    return status;
}

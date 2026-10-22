/* stat - display file status */
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

static const char *type_name(uint32_t m)
{
    if (S_ISDIR(m)) return "directory";
    if (S_ISCHR(m)) return "character special file";
    if (S_ISFIFO(m)) return "fifo";
    return "regular file";
}

int main(int argc, char **argv)
{
    int status = 0;
    if (argc < 2) {
        fprintf(stderr, "stat: missing operand\n");
        return 1;
    }
    for (int i = 1; i < argc; i++) {
        struct stat st;
        if (stat(argv[i], &st) < 0) {
            fprintf(stderr, "stat: cannot stat '%s': %s\n", argv[i], strerror(errno));
            status = 1;
            continue;
        }
        char when[64];
        time_t t = (time_t)st.st_mtime;
        struct tm tm;
        gmtime_r(&t, &tm);
        strftime(when, sizeof(when), "%Y-%m-%d %H:%M:%S UTC", &tm);
        printf("  File: %s\n", argv[i]);
        printf("  Size: %-10u Type: %s\n", st.st_size, type_name(st.st_mode));
        printf(" Inode: %-10u Links: %u", st.st_ino, st.st_nlink);
        if (S_ISCHR(st.st_mode))
            printf("   Device: %u,%u", MAJOR(st.st_rdev), MINOR(st.st_rdev));
        printf("\nAccess: (%04o)\nModify: %s\n", st.st_mode & 07777, when);
    }
    return status;
}

/* lsblk - list block devices */
#include <flux.h>
#include <stdio.h>
#include <string.h>

static void size_str(unsigned sectors, unsigned ssize, char *out, size_t n)
{
    unsigned kb = ssize >= 1024 ? sectors * (ssize / 1024) : sectors / (1024 / ssize);
    if (kb < 1024)
        snprintf(out, n, "%uK", kb);
    else if (kb < 1024 * 1024)
        snprintf(out, n, "%u.%uM", kb / 1024, kb % 1024 * 10 / 1024);
    else
        snprintf(out, n, "%u.%uG", kb / (1024 * 1024), kb / 1024 % 1024 * 10 / 1024);
}

int main(void)
{
    static const char *types[] = { "disk", "rom", "part" };
    printf("%-8s %-5s %8s %-8s %-14s %s\n", "NAME", "TYPE", "SIZE", "FSTYPE", "MOUNTPOINT", "MODEL");
    struct blkinfo bi;
    for (int i = 0; blkinfo(i, &bi) == 0; i++) {
        char name[24], size[16];
        snprintf(name, sizeof(name), "%s%s", bi.type == BLK_PART ? "`-" : "", bi.name);
        if (bi.sectors)
            size_str(bi.sectors, bi.sector_size, size, sizeof(size));
        else
            strcpy(size, "-");
        printf("%-8s %-5s %8s %-8s %-14s %s\n", name, bi.type < 3 ? types[bi.type] : "?",
               size, bi.fstype, bi.mountpoint, bi.type == BLK_PART ? "" : bi.model);
    }
    return 0;
}

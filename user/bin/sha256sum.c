/*
 * sha256sum - print or check SHA-256 checksums
 *
 *   sha256sum [FILE...]
 *   sha256sum -c LIST
 *
 * Prints "HASH  NAME" for each file (standard input when none or "-"),
 * the format GNU sha256sum reads and writes; -c reads such lines and
 * says OK or FAILED for each file.
 */
#include <errno.h>
#include <fcntl.h>
#include <flux/sha256.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int hash_file(const char *name, char hex[SHA256_SIZE * 2 + 1])
{
    int fd = strcmp(name, "-") ? open(name, O_RDONLY) : 0;
    if (fd < 0) {
        fprintf(stderr, "sha256sum: %s: %s\n", name, strerror(errno));
        return -1;
    }
    static char buf[65536];
    struct sha256 s;
    sha256_init(&s);
    int n;
    while ((n = (int)read(fd, buf, sizeof(buf))) > 0)
        sha256_update(&s, buf, (size_t)n);
    if (n < 0)
        fprintf(stderr, "sha256sum: %s: %s\n", name, strerror(errno));
    if (fd)
        close(fd);
    if (n < 0)
        return -1;
    uint8_t out[SHA256_SIZE];
    sha256_final(&s, out);
    for (int i = 0; i < SHA256_SIZE; i++)
        sprintf(hex + i * 2, "%02x", out[i]);
    return 0;
}

static int check(const char *list)
{
    FILE *f = strcmp(list, "-") ? fopen(list, "r") : stdin;
    if (!f) {
        fprintf(stderr, "sha256sum: %s: %s\n", list, strerror(errno));
        return 1;
    }
    char line[4200], hex[SHA256_SIZE * 2 + 1];
    int failed = 0, bad_lines = 0;
    while (fgets(line, sizeof(line), f)) {
        line[strcspn(line, "\r\n")] = 0;
        if (!line[0])
            continue;
        /* "HASH  NAME" or "HASH *NAME" (binary mode, the same here) */
        if (strlen(line) < SHA256_SIZE * 2 + 2 || line[SHA256_SIZE * 2] != ' ') {
            bad_lines++;
            continue;
        }
        const char *name = line + SHA256_SIZE * 2 + 1;
        if (*name == ' ' || *name == '*')
            name++;
        if (hash_file(name, hex) < 0) {
            printf("%s: FAILED open or read\n", name);
            failed++;
        } else if (strncasecmp(hex, line, SHA256_SIZE * 2)) {
            printf("%s: FAILED\n", name);
            failed++;
        } else {
            printf("%s: OK\n", name);
        }
    }
    if (f != stdin)
        fclose(f);
    if (bad_lines)
        fprintf(stderr, "sha256sum: %d improperly formatted line%s\n", bad_lines, bad_lines == 1 ? "" : "s");
    if (failed)
        fprintf(stderr, "sha256sum: %d computed checksum%s did NOT match\n", failed, failed == 1 ? "" : "s");
    return failed || bad_lines ? 1 : 0;
}

int main(int argc, char **argv)
{
    if (argc > 1 && !strcmp(argv[1], "-c")) {
        if (argc != 3) {
            fprintf(stderr, "usage: sha256sum [FILE...] | sha256sum -c LIST\n");
            return 2;
        }
        return check(argv[2]);
    }
    char hex[SHA256_SIZE * 2 + 1];
    int status = 0;
    if (argc < 2) {
        if (hash_file("-", hex) < 0)
            return 1;
        printf("%s  -\n", hex);
        return 0;
    }
    for (int i = 1; i < argc; i++) {
        if (hash_file(argv[i], hex) < 0) {
            status = 1;
            continue;
        }
        printf("%s  %s\n", hex, argv[i]);
    }
    return status;
}

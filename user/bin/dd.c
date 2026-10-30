/*
 * dd - copy with block sizes and offsets
 *
 *   dd [if=FILE] [of=FILE] [bs=N] [ibs=N] [obs=N] [count=N] [skip=N] [seek=N]
 *      [conv=notrunc,sync,fsync,noerror] [status=none|noxfer]
 *
 * Sizes take c, w, b, k, K, M, G suffixes (and a product like 2x512).
 * Reads are of ibs bytes, writes of obs (bs sets both); skip and seek
 * are in blocks; count is in input blocks.
 */
#define _LARGEFILE64_SOURCE             /* lseek64 on a host's C library too */
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static long long parse_size(const char *s)
{
    long long total = 1;
    while (*s) {
        char *end;
        long long v = strtoll(s, &end, 10);
        if (end == s)
            return -1;
        switch (*end) {
        case 'c': v *= 1; end++; break;
        case 'w': v *= 2; end++; break;
        case 'b': v *= 512; end++; break;
        case 'k': case 'K': v *= 1024; end++; break;
        case 'M': v *= 1024 * 1024; end++; break;
        case 'G': v *= 1024LL * 1024 * 1024; end++; break;
        }
        if (*end == 'B')
            end++;
        if (v < 0 || (v && total > LLONG_MAX / v))
            return -1;                  /* negative, or too big to be a size */
        total *= v;
        if (*end == 'x')
            end++;
        else if (*end)
            return -1;
        s = end;
    }
    return total;
}

int main(int argc, char **argv)
{
    const char *in_name = NULL, *out_name = NULL;
    /* count -1 copies everything: only leaving count out may ask for that,
     * never a count that fails to parse (count=1O must not wipe a disk) */
    long long ibs = 512, obs = 512, count = -1, skip = 0, seek = 0;
    int have_count = 0;
    int notrunc = 0, sync_pad = 0, noerror = 0, do_fsync = 0, quiet = 0;
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i], *eq = strchr(a, '=');
        if (!eq) {
            fprintf(stderr, "dd: unrecognized operand '%s'\n", a);
            return 1;
        }
        size_t kl = (size_t)(eq - a);
        const char *v = eq + 1;
        if (!strncmp(a, "if", kl) && kl == 2) in_name = v;
        else if (!strncmp(a, "of", kl) && kl == 2) out_name = v;
        else if (!strncmp(a, "bs", kl) && kl == 2) ibs = obs = parse_size(v);
        else if (!strncmp(a, "ibs", kl) && kl == 3) ibs = parse_size(v);
        else if (!strncmp(a, "obs", kl) && kl == 3) obs = parse_size(v);
        else if (!strncmp(a, "count", kl) && kl == 5) { count = parse_size(v); have_count = 1; }
        else if (!strncmp(a, "skip", kl) && kl == 4) skip = parse_size(v);
        else if (!strncmp(a, "seek", kl) && kl == 4) seek = parse_size(v);
        else if (!strncmp(a, "status", kl) && kl == 6) quiet = !strcmp(v, "none") || !strcmp(v, "noxfer");
        else if (!strncmp(a, "conv", kl) && kl == 4) {
            char tmp[128];
            snprintf(tmp, sizeof(tmp), "%s", v);
            for (char *t = strtok(tmp, ","); t; t = strtok(NULL, ",")) {
                if (!strcmp(t, "notrunc")) notrunc = 1;
                else if (!strcmp(t, "sync")) sync_pad = 1;
                else if (!strcmp(t, "noerror")) noerror = 1;
                else if (!strcmp(t, "fsync") || !strcmp(t, "fdatasync")) do_fsync = 1;
                else {
                    fprintf(stderr, "dd: invalid conversion: '%s'\n", t);
                    return 1;
                }
            }
        } else {
            fprintf(stderr, "dd: unrecognized operand '%s'\n", a);
            return 1;
        }
        if (ibs <= 0 || obs <= 0 || ibs > INT_MAX || obs > INT_MAX || skip < 0 || seek < 0 ||
            (have_count && count < 0) || skip > LLONG_MAX / ibs || seek > LLONG_MAX / obs) {
            fprintf(stderr, "dd: invalid number: '%s'\n", v);
            return 1;
        }
    }
    int in = 0, out = 1;
    if (in_name && (in = open(in_name, O_RDONLY)) < 0) {
        fprintf(stderr, "dd: failed to open '%s': %s\n", in_name, strerror(errno));
        return 1;
    }
    if (out_name && (out = open(out_name, O_WRONLY | O_CREAT | (notrunc || seek ? 0 : O_TRUNC), 0644)) < 0) {
        fprintf(stderr, "dd: failed to open '%s': %s\n", out_name, strerror(errno));
        return 1;
    }
    if (skip) {
        if (lseek64(in, (off64_t)(skip * ibs), SEEK_SET) < 0) {
            char *junk = malloc((size_t)ibs);
            for (long long k = 0; k < skip; k++)
                if (read(in, junk, (size_t)ibs) <= 0)
                    break;
            free(junk);
        }
    }
    if (seek) {
        /* 64-bit: off_t is 32 bits here, and seek=4608 bs=1M must not
         * wrap to 512 MiB on a disk */
        if (lseek64(out, (off64_t)(seek * obs), SEEK_SET) < 0) {
            fprintf(stderr, "dd: cannot seek '%s': %s\n", out_name ? out_name : "stdout", strerror(errno));
            return 1;
        }
        /* ftruncate takes an off_t: past its range, leave the file as it is
         * rather than cut it at a wrapped size */
        if (!notrunc && out_name && (off64_t)(seek * obs) == (off64_t)(off_t)(seek * obs))
            (void)!ftruncate(out, (off_t)(seek * obs));
    }
    char *ibuf = malloc((size_t)ibs), *obuf = malloc((size_t)obs);
    if (!ibuf || !obuf) {
        fprintf(stderr, "dd: out of memory\n");
        return 1;
    }
    long long full_in = 0, part_in = 0, full_out = 0, part_out = 0, total = 0;
    long long opos = 0;
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    int status = 0;
    while (count < 0 || full_in + part_in < count) {
        int n = (int)read(in, ibuf, (size_t)ibs);
        if (n < 0) {
            fprintf(stderr, "dd: error reading '%s': %s\n", in_name ? in_name : "stdin", strerror(errno));
            if (!noerror) {
                status = 1;
                break;
            }
            n = 0;
        }
        if (n == 0)
            break;
        if (n == ibs)
            full_in++;
        else {
            part_in++;
            if (sync_pad) {
                memset(ibuf + n, 0, (size_t)(ibs - n));
                n = (int)ibs;
            }
        }
        /* through the output buffer in obs-sized pieces */
        int off = 0;
        while (off < n) {
            int take = (int)(obs - opos) < n - off ? (int)(obs - opos) : n - off;
            memcpy(obuf + opos, ibuf + off, (size_t)take);
            opos += take;
            off += take;
            if (opos == obs) {
                if (write(out, obuf, (size_t)obs) != (int)obs) {
                    fprintf(stderr, "dd: error writing '%s': %s\n", out_name ? out_name : "stdout", strerror(errno));
                    return 1;
                }
                full_out++;
                total += obs;
                opos = 0;
            }
        }
    }
    if (opos) {
        if (write(out, obuf, (size_t)opos) != (int)opos) {
            fprintf(stderr, "dd: error writing '%s': %s\n", out_name ? out_name : "stdout", strerror(errno));
            return 1;
        }
        part_out++;
        total += opos;
    }
    if (do_fsync)
        fsync(out);
    if (out != 1)
        close(out);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    if (!quiet) {
        double secs = (double)(t1.tv_sec - t0.tv_sec) + (double)(t1.tv_nsec - t0.tv_nsec) / 1e9;
        fprintf(stderr, "%lld+%lld records in\n%lld+%lld records out\n%lld bytes copied, %.4g s, %.3g kB/s\n",
                full_in, part_in, full_out, part_out, total, secs, secs > 0 ? (double)total / secs / 1000 : 0.0);
    }
    return status;
}

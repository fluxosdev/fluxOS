/*
 * getopt, getopt_long and getopt_long_only, as glibc does them: options
 * may come after operands (they are moved forward, unless the option
 * string starts with '+' or POSIXLY_CORRECT is set), "--" ends them,
 * and a leading ':' in the option string asks for ':' rather than '?'
 * when an argument is missing.
 */
#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

char *optarg;
int optind = 1, opterr = 1, optopt = '?';

static int nextchar;                    /* position inside a group like -abc */
static int first_nonopt, last_nonopt;   /* the operands passed over so far */

/* Move the operands in [first_nonopt, last_nonopt) after the options that
 * followed them, up to optind. */
static void exchange(char **argv)
{
    int bottom = first_nonopt, middle = last_nonopt, top = optind;
    while (top > middle && middle > bottom) {
        if (top - middle > middle - bottom) {
            int len = middle - bottom;
            for (int i = 0; i < len; i++) {
                char *t = argv[bottom + i];
                argv[bottom + i] = argv[top - (middle - bottom) + i];
                argv[top - (middle - bottom) + i] = t;
            }
            top -= len;
        } else {
            int len = top - middle;
            for (int i = 0; i < len; i++) {
                char *t = argv[bottom + i];
                argv[bottom + i] = argv[middle + i];
                argv[middle + i] = t;
            }
            bottom += len;
        }
    }
    first_nonopt += optind - last_nonopt;
    last_nonopt = optind;
}

static int getopt_internal(int argc, char *const argv_c[], const char *optstring,
                           const struct option *longopts, int *longindex, int long_only)
{
    char **argv = (char **)argv_c;
    if (optind == 0) {                  /* a reset */
        optind = 1;
        nextchar = 0;
        first_nonopt = last_nonopt = 0;
    }
    int permute = 1, return_in_order = 0;
    if (*optstring == '+') {
        permute = 0;
        optstring++;
    } else if (*optstring == '-') {
        return_in_order = 1;
        optstring++;
    } else if (getenv("POSIXLY_CORRECT")) {
        permute = 0;
    }
    int colon = *optstring == ':';
    optarg = NULL;

    if (!nextchar) {
        if (last_nonopt > optind)
            last_nonopt = optind;
        if (first_nonopt > optind)
            first_nonopt = optind;
        if (permute) {
            if (first_nonopt != last_nonopt && last_nonopt != optind)
                exchange(argv);
            else if (last_nonopt != optind)
                first_nonopt = optind;
            while (optind < argc && (argv[optind][0] != '-' || !argv[optind][1]))
                optind++;
            last_nonopt = optind;
        }
        if (optind != argc && !strcmp(argv[optind], "--")) {
            optind++;
            if (first_nonopt != last_nonopt && last_nonopt != optind)
                exchange(argv);
            else if (first_nonopt == last_nonopt)
                first_nonopt = optind;
            last_nonopt = argc;
            optind = argc;
        }
        if (optind == argc) {
            if (first_nonopt != last_nonopt)
                optind = first_nonopt;
            return -1;
        }
        if (argv[optind][0] != '-' || !argv[optind][1]) {
            if (!permute && !return_in_order)
                return -1;
            optarg = argv[optind++];
            return 1;
        }
        /* a long option */
        if (longopts && (argv[optind][1] == '-' ||
                         (long_only && (argv[optind][2] || !strchr(optstring, argv[optind][1]))))) {
            char *name = argv[optind] + (argv[optind][1] == '-' ? 2 : 1);
            char *eq = strchr(name, '=');
            size_t len = eq ? (size_t)(eq - name) : strlen(name);
            const struct option *found = NULL;
            int idx = -1, ambiguous = 0;
            for (int i = 0; longopts[i].name; i++) {
                if (strncmp(longopts[i].name, name, len))
                    continue;
                if (strlen(longopts[i].name) == len) {
                    found = &longopts[i];
                    idx = i;
                    ambiguous = 0;
                    break;
                }
                if (!found) {
                    found = &longopts[i];
                    idx = i;
                } else if (found->has_arg != longopts[i].has_arg || found->flag != longopts[i].flag ||
                           found->val != longopts[i].val) {
                    ambiguous = 1;
                }
            }
            if (ambiguous) {
                if (opterr)
                    fprintf(stderr, "%s: option '%s' is ambiguous\n", argv[0], argv[optind]);
                optind++;
                optopt = 0;
                return '?';
            }
            if (found) {
                optind++;
                if (eq) {
                    if (found->has_arg == no_argument) {
                        if (opterr)
                            fprintf(stderr, "%s: option '--%s' doesn't allow an argument\n", argv[0], found->name);
                        optopt = found->val;
                        return '?';
                    }
                    optarg = eq + 1;
                } else if (found->has_arg == required_argument) {
                    if (optind < argc) {
                        optarg = argv[optind++];
                    } else {
                        if (opterr)
                            fprintf(stderr, "%s: option '--%s' requires an argument\n", argv[0], found->name);
                        optopt = found->val;
                        return colon ? ':' : '?';
                    }
                }
                if (longindex)
                    *longindex = idx;
                if (found->flag) {
                    *found->flag = found->val;
                    return 0;
                }
                return found->val;
            }
            if (!long_only || argv[optind][1] == '-' || !strchr(optstring, argv[optind][1])) {
                if (opterr)
                    fprintf(stderr, "%s: unrecognized option '%s'\n", argv[0], argv[optind]);
                optind++;
                optopt = 0;
                return '?';
            }
        }
        nextchar = 1;
    }

    /* a short option, in a group */
    char c = argv[optind][nextchar++];
    const char *spec = strchr(optstring, c);
    if (!argv[optind][nextchar]) {
        optind++;
        nextchar = 0;
    }
    if (!spec || c == ':' || c == ';') {
        if (opterr && !colon)
            fprintf(stderr, "%s: invalid option -- '%c'\n", argv[0], c);
        optopt = c;
        return '?';
    }
    if (spec[1] == ':') {
        if (spec[2] == ':') {           /* optional: only when attached */
            if (nextchar) {
                optarg = argv[optind++] + nextchar;
                nextchar = 0;
            }
        } else if (nextchar) {
            optarg = argv[optind++] + nextchar;
            nextchar = 0;
        } else if (optind < argc) {
            optarg = argv[optind++];
        } else {
            if (opterr && !colon)
                fprintf(stderr, "%s: option requires an argument -- '%c'\n", argv[0], c);
            optopt = c;
            return colon ? ':' : '?';
        }
    }
    return c;
}

int getopt(int argc, char *const argv[], const char *optstring)
{
    return getopt_internal(argc, argv, optstring, NULL, NULL, 0);
}

int getopt_long(int argc, char *const argv[], const char *optstring, const struct option *longopts, int *longindex)
{
    return getopt_internal(argc, argv, optstring, longopts, longindex, 0);
}

int getopt_long_only(int argc, char *const argv[], const char *optstring, const struct option *longopts,
                     int *longindex)
{
    return getopt_internal(argc, argv, optstring, longopts, longindex, 1);
}

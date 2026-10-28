#ifndef _REGEX_H
#define _REGEX_H

#include <stddef.h>
#include <sys/types.h>

typedef int regoff_t;

typedef struct {
    size_t re_nsub;                     /* parenthesized subexpressions */
    void *re_prog;                      /* the compiled program (regex.c) */
    int re_cflags;
} regex_t;

typedef struct {
    regoff_t rm_so;
    regoff_t rm_eo;
} regmatch_t;

#define REG_EXTENDED 1
#define REG_ICASE    2
#define REG_NOSUB    4
#define REG_NEWLINE  8

#define REG_NOTBOL   1
#define REG_NOTEOL   2

#define REG_OK       0
#define REG_NOMATCH  1
#define REG_BADPAT   2
#define REG_ECOLLATE 3
#define REG_ECTYPE   4
#define REG_EESCAPE  5
#define REG_ESUBREG  6
#define REG_EBRACK   7
#define REG_EPAREN   8
#define REG_EBRACE   9
#define REG_BADBR    10
#define REG_ERANGE   11
#define REG_ESPACE   12
#define REG_BADRPT   13

int regcomp(regex_t *re, const char *pattern, int cflags);
int regexec(const regex_t *re, const char *s, size_t nmatch, regmatch_t *match, int eflags);
size_t regerror(int err, const regex_t *re, char *buf, size_t size);
void regfree(regex_t *re);

#endif

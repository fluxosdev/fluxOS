#ifndef _STDIO_H
#define _STDIO_H

#include <stdarg.h>
#include <stddef.h>
#include <sys/types.h>

#define EOF (-1)
#define BUFSIZ 4096
#define _IOFBF 0
#define _IOLBF 1
#define _IONBF 2

#ifndef SEEK_SET
#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2
#endif

#define FILENAME_MAX 512
#define FOPEN_MAX    256
#define L_tmpnam     32
#define L_ctermid    16
#define TMP_MAX      238328
#define P_tmpdir     "/tmp"

typedef struct FILE FILE;
typedef struct { off64_t pos; } fpos_t;

extern FILE *stdin;
extern FILE *stdout;
extern FILE *stderr;
#define stdin  stdin
#define stdout stdout
#define stderr stderr

FILE *fopen(const char *path, const char *mode);
FILE *fdopen(int fd, const char *mode);
FILE *freopen(const char *path, const char *mode, FILE *f);
FILE *tmpfile(void);
char *tmpnam(char *buf);
char *tempnam(const char *dir, const char *prefix);
FILE *popen(const char *command, const char *mode);
int pclose(FILE *f);
FILE *fmemopen(void *buf, size_t size, const char *mode);
FILE *open_memstream(char **buf, size_t *size);
int fclose(FILE *f);
int fflush(FILE *f);
int fileno(FILE *f);
int feof(FILE *f);
int ferror(FILE *f);
void clearerr(FILE *f);
int setvbuf(FILE *f, char *buf, int mode, size_t size);
void setbuf(FILE *f, char *buf);
void setbuffer(FILE *f, char *buf, size_t size);
void setlinebuf(FILE *f);

int fseek(FILE *f, long off, int whence);
long ftell(FILE *f);
int fseeko(FILE *f, off_t off, int whence);
off_t ftello(FILE *f);
void rewind(FILE *f);
int fgetpos(FILE *f, fpos_t *pos);
int fsetpos(FILE *f, const fpos_t *pos);

int fgetc(FILE *f);
int getc(FILE *f);
int getchar(void);
int ungetc(int c, FILE *f);
char *fgets(char *buf, int size, FILE *f);
char *gets(char *buf) __attribute__((deprecated));
ssize_t getline(char **line, size_t *cap, FILE *f);
ssize_t getdelim(char **line, size_t *cap, int delim, FILE *f);
size_t fread(void *buf, size_t size, size_t n, FILE *f);
int fputc(int c, FILE *f);
int putc(int c, FILE *f);
int putchar(int c);
int fputs(const char *s, FILE *f);
int puts(const char *s);
size_t fwrite(const void *buf, size_t size, size_t n, FILE *f);

/* No threads: the unlocked forms are the same functions. */
int getc_unlocked(FILE *f);
int getchar_unlocked(void);
int putc_unlocked(int c, FILE *f);
int putchar_unlocked(int c);
int fgetc_unlocked(FILE *f);
int fputc_unlocked(int c, FILE *f);
int fputs_unlocked(const char *s, FILE *f);
size_t fread_unlocked(void *buf, size_t size, size_t n, FILE *f);
size_t fwrite_unlocked(const void *buf, size_t size, size_t n, FILE *f);
int fflush_unlocked(FILE *f);
int feof_unlocked(FILE *f);
int ferror_unlocked(FILE *f);
void clearerr_unlocked(FILE *f);
int fileno_unlocked(FILE *f);
void flockfile(FILE *f);
void funlockfile(FILE *f);
int ftrylockfile(FILE *f);

int printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
int fprintf(FILE *f, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
int dprintf(int fd, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
int sprintf(char *buf, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
int snprintf(char *buf, size_t size, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
int asprintf(char **out, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
int vprintf(const char *fmt, va_list ap);
int vfprintf(FILE *f, const char *fmt, va_list ap);
int vdprintf(int fd, const char *fmt, va_list ap);
int vsprintf(char *buf, const char *fmt, va_list ap);
int vsnprintf(char *buf, size_t size, const char *fmt, va_list ap);
int vasprintf(char **out, const char *fmt, va_list ap);

int scanf(const char *fmt, ...) __attribute__((format(scanf, 1, 2)));
int fscanf(FILE *f, const char *fmt, ...) __attribute__((format(scanf, 2, 3)));
int sscanf(const char *s, const char *fmt, ...) __attribute__((format(scanf, 2, 3)));
int vscanf(const char *fmt, va_list ap);
int vfscanf(FILE *f, const char *fmt, va_list ap);
int vsscanf(const char *s, const char *fmt, va_list ap);

void perror(const char *msg);
int rename(const char *from, const char *to);
int remove(const char *path);
char *ctermid(char *buf);

#endif

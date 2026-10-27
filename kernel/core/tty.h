/* Terminals: the console and pseudo-terminals share one line discipline. */
#ifndef TTY_H
#define TTY_H

#include <stdint.h>

struct tty;
struct vnode;
struct file;
struct proc;

extern struct tty console_tty;

void tty_init(void);
void tty_input(char c);                 /* console input, from IRQ context */
int tty_read(struct tty *t, char *buf, uint32_t n);
int tty_write(struct tty *t, const char *buf, uint32_t n);
int tty_set_mode(struct tty *t, int mode);      /* returns previous mode; <0 queries */
int tty_get_fg(struct tty *t);
void tty_set_fg(struct tty *t, int pgid);

/*
 * Controlling terminals. A process inherits its parent's, and exec
 * switches it to the terminal on standard input, if that is one.
 */
struct tty *tty_hold(struct tty *t);
void tty_drop(struct tty *t);
void tty_exec_attach(struct proc *p);
struct tty *tty_current(void);

/* Device node callbacks; a NULL dev_priv means the controlling terminal. */
int tty_dev_read(struct vnode *vn, void *buf, uint64_t off, uint32_t n);
int tty_dev_write(struct vnode *vn, const void *buf, uint64_t off, uint32_t n);
int tty_dev_poll(struct vnode *vn);

int pty_create(struct file **master, struct file **slave);

#endif

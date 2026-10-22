/* Console terminal line discipline. */
#ifndef TTY_H
#define TTY_H

#include <stdint.h>

void tty_init(void);
void tty_input(char c);                 /* called from IRQ context */
int tty_read(char *buf, uint32_t n);
int tty_write(const char *buf, uint32_t n);
int tty_set_mode(int mode);             /* returns previous mode; <0 queries */

int tty_get_fg(void);
void tty_set_fg(int pgid);

#endif

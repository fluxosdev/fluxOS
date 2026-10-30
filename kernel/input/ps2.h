/*
 * The i8042's auxiliary port (mouse.c), for the touchpad drivers
 * (touchpad.c). Commands are polled, so only with the mouse's interrupt
 * kept away: at boot, or with interrupts off.
 */
#ifndef PS2_H
#define PS2_H

#include <stdint.h>

int ps2_aux_cmd(uint8_t b);             /* send a byte: 0 if acknowledged */
int ps2_aux_read(uint8_t *b, uint32_t ms);      /* a byte from the device: 0, or -1 */
int ps2_aux_query(uint8_t cmd, uint8_t *resp, int n);  /* a command and n bytes back */

/* touchpad.c: take the device on the auxiliary port if it is a touchpad
 * we can drive in absolute mode (1), and then its bytes. */
int touchpad_probe(void);
void touchpad_byte(uint8_t b);
int touchpad_resume(void);              /* its mode again: 1, or 0 if it is gone */
const char *touchpad_name(void);

#endif

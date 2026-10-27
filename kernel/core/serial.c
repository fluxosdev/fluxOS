/*
 * COM1 (16550 UART) driver. Output mirrors the VGA console; input is
 * fed into the tty, so fluxOS can be driven entirely over a serial line.
 */
#include "kernel.h"
#include "arch.h"
#include "drivers.h"
#include "tty.h"
#include "x86.h"

#define COM1 0x3F8

static int serial_ok;

void serial_init(void)
{
    outb(COM1 + 1, 0x00);       /* disable interrupts */
    outb(COM1 + 3, 0x80);       /* DLAB on */
    outb(COM1 + 0, 0x01);       /* divisor 1 = 115200 baud */
    outb(COM1 + 1, 0x00);
    outb(COM1 + 3, 0x03);       /* 8N1 */
    outb(COM1 + 2, 0xC7);       /* FIFO on, cleared, 14-byte threshold */
    outb(COM1 + 4, 0x0B);       /* DTR, RTS, OUT2 (IRQ gate) */

    /* Loopback self-test: a missing UART reads back 0xFF. */
    outb(COM1 + 4, 0x1E);
    outb(COM1 + 0, 0xAE);
    serial_ok = inb(COM1 + 0) == 0xAE;
    outb(COM1 + 4, 0x0B);
}

static void serial_irq(struct trapframe *tf)
{
    (void)tf;
    while (inb(COM1 + 5) & 0x01)
        tty_input((char)inb(COM1));
}

void serial_enable_irq(void)
{
    if (!serial_ok)
        return;
    irq_register(IRQ_COM1, serial_irq);
    outb(COM1 + 1, 0x01);       /* interrupt on received data */
}

static void serial_putc(char c)
{
    for (int spin = 0; spin < 100000 && !(inb(COM1 + 5) & 0x20); spin++)
        ;
    outb(COM1, (uint8_t)c);
}

void serial_write(const char *buf, uint32_t len)
{
    if (!serial_ok)
        return;
    for (uint32_t i = 0; i < len; i++) {
        if (buf[i] == '\n')
            serial_putc('\r');
        serial_putc(buf[i]);
    }
}

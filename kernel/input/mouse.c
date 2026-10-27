/*
 * PS/2 mouse on the i8042 auxiliary port (IRQ12), reported as relative
 * motion, with a scroll wheel when the mouse speaks the IntelliMouse
 * protocol. Under QEMU, VMware and VirtualBox the VMware "vmmouse"
 * backdoor gives absolute positions (and the wheel) instead, so the
 * pointer follows the host's without grabbing it; the PS/2 interrupt
 * still signals that data is waiting.
 */
#include "kernel.h"
#include "arch.h"
#include "drivers.h"
#include "x86.h"

#define PS2_DATA   0x60
#define PS2_STATUS 0x64
#define PS2_CMD    0x64
#define ST_OUTPUT  0x01
#define ST_INPUT   0x02
#define ST_AUX     0x20
#define IRQ_MOUSE  12

#define VMW_MAGIC        0x564D5868
#define VMW_PORT         0x5658
#define VMW_GETVERSION   10
#define VMW_ABS_DATA     39
#define VMW_ABS_STATUS   40
#define VMW_ABS_COMMAND  41
#define VMMOUSE_ENABLE   0x45414552
#define VMMOUSE_ABSOLUTE 0x53424152
#define VMMOUSE_ID       0x3442554A
#define VMMOUSE_ERROR    0xFFFF0000
#define VMMOUSE_RELATIVE 0x00010000

static uint8_t packet[4];
static int npacket;
static int packet_size = 3;             /* 4 with an IntelliMouse wheel */
static int vmmouse;

static void wait_write(void)
{
    for (int i = 0; i < 100000 && (inb(PS2_STATUS) & ST_INPUT); i++)
        ;
}

static int wait_read(void)
{
    for (int i = 0; i < 100000; i++)
        if (inb(PS2_STATUS) & ST_OUTPUT)
            return 1;
    return 0;
}

static void ctrl_cmd(uint8_t cmd)
{
    wait_write();
    outb(PS2_CMD, cmd);
}

/* Send a byte to the mouse and swallow its acknowledgement. */
static void mouse_cmd(uint8_t b)
{
    ctrl_cmd(0xD4);
    wait_write();
    outb(PS2_DATA, b);
    if (wait_read())
        (void)inb(PS2_DATA);
}

struct vmw_regs {
    uint32_t a, b, c, d;
};

static void vmw_call(uint32_t cmd, uint32_t arg, struct vmw_regs *r)
{
    uint32_t a = VMW_MAGIC, b = arg, c = cmd, d = VMW_PORT, si = 0, di = 0;
    __asm__ volatile("inl %%dx, %%eax"
                     : "+a"(a), "+b"(b), "+c"(c), "+d"(d), "+S"(si), "+D"(di)
                     : : "memory");
    r->a = a;
    r->b = b;
    r->c = c;
    r->d = d;
}

static int vmmouse_enable(void)
{
    struct vmw_regs r;
    vmw_call(VMW_GETVERSION, 0, &r);
    if (r.b != VMW_MAGIC)
        return 0;                       /* not a VMware-compatible hypervisor */
    vmw_call(VMW_ABS_COMMAND, VMMOUSE_ENABLE, &r);
    vmw_call(VMW_ABS_STATUS, 0, &r);
    if ((r.a & VMMOUSE_ERROR) == VMMOUSE_ERROR || (r.a & 0xFFFF) == 0)
        return 0;
    vmw_call(VMW_ABS_DATA, 1, &r);
    if (r.a != VMMOUSE_ID)
        return 0;
    vmw_call(VMW_ABS_COMMAND, VMMOUSE_ABSOLUTE, &r);
    return 1;
}

static void vmmouse_poll(void)
{
    struct vmw_regs r;
    for (;;) {
        vmw_call(VMW_ABS_STATUS, 0, &r);
        if ((r.a & VMMOUSE_ERROR) == VMMOUSE_ERROR) {
            klog("mouse: vmmouse error, using relative PS/2 motion\n");
            vmmouse = 0;
            return;
        }
        if ((r.a & 0xFFFF) < 4)
            return;
        vmw_call(VMW_ABS_DATA, 4, &r);
        struct input_event ev = { .type = IEV_MOUSE };
        if (r.a & 0x20)
            ev.mods |= MOUSE_LEFT;
        if (r.a & 0x10)
            ev.mods |= MOUSE_RIGHT;
        if (r.a & 0x08)
            ev.mods |= MOUSE_MIDDLE;
        ev.code = (uint16_t)(int16_t)-(int8_t)r.d;     /* wheel: positive is away from the user */
        if (r.a & VMMOUSE_RELATIVE) {
            ev.x = (int16_t)(int32_t)r.b;
            ev.y = (int16_t)-(int32_t)r.c;
        } else {
            ev.flags = IEV_ABS;
            ev.x = (int16_t)(r.b >> 1);
            ev.y = (int16_t)(r.c >> 1);
        }
        input_push(&ev);
    }
}

static void ps2_packet(void)
{
    if (packet[0] & 0xC0)
        return;                         /* overflow: the motion is garbage */
    struct input_event ev = { .type = IEV_MOUSE };
    ev.mods = packet[0] & 0x07;         /* left, right, middle */
    ev.x = (int16_t)(packet[1] - ((packet[0] << 4) & 0x100));
    ev.y = (int16_t)-(packet[2] - ((packet[0] << 3) & 0x100));
    if (packet_size == 4)
        ev.code = (uint16_t)(int16_t)-(int8_t)packet[3];
    input_push(&ev);
}

static void mouse_irq(struct trapframe *tf)
{
    (void)tf;
    uint8_t st;
    while ((st = inb(PS2_STATUS)) & ST_OUTPUT) {
        if (!(st & ST_AUX))
            break;                      /* keyboard data: IRQ1 takes it */
        uint8_t b = inb(PS2_DATA);
        if (vmmouse)
            continue;
        if (npacket == 0 && !(b & 0x08))
            continue;                   /* out of sync: wait for a first byte */
        packet[npacket++] = b;
        if (npacket == packet_size) {
            npacket = 0;
            ps2_packet();
        }
    }
    if (vmmouse)
        vmmouse_poll();
}

void mouse_init(void)
{
    ctrl_cmd(0xA8);                     /* enable the auxiliary port */
    ctrl_cmd(0x20);                     /* read the controller config */
    if (!wait_read())
        return;
    uint8_t cfg = inb(PS2_DATA);
    cfg |= 0x02;                        /* IRQ12 on */
    cfg &= (uint8_t)~0x20;              /* aux clock on */
    ctrl_cmd(0x60);
    wait_write();
    outb(PS2_DATA, cfg);

    mouse_cmd(0xF6);                    /* defaults */
    /* The IntelliMouse knock: sample rates 200, 100, 80 turn on the wheel. */
    static const uint8_t knock[] = { 200, 100, 80 };
    for (int i = 0; i < 3; i++) {
        mouse_cmd(0xF3);
        mouse_cmd(knock[i]);
    }
    mouse_cmd(0xF2);                    /* identify */
    if (wait_read() && inb(PS2_DATA) == 3)
        packet_size = 4;
    mouse_cmd(0xF3);
    mouse_cmd(100);                     /* back to the default rate */
    mouse_cmd(0xF4);                    /* start streaming */
    vmmouse = vmmouse_enable();
    irq_register(IRQ_MOUSE, mouse_irq);
    boot_ok("PS/2 mouse ready%s%s", packet_size == 4 ? " with a wheel" : "",
            vmmouse ? " (absolute positioning via vmmouse)" : "");
}

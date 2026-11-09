/*
 * PS/2 mouse on the i8042 auxiliary port (IRQ12), reported as relative
 * motion, with a scroll wheel when the mouse speaks the IntelliMouse
 * protocol. A laptop's touchpad on the same port goes to touchpad.c
 * when it is one that driver knows; otherwise it is driven as a mouse,
 * as its firmware emulates one. Under QEMU, VMware and VirtualBox the
 * VMware "vmmouse" backdoor gives absolute positions (and the wheel)
 * instead, so the pointer follows the host's without grabbing it; the
 * PS/2 interrupt still signals that data is waiting.
 */
#include "kernel.h"
#include "arch.h"
#include "drivers.h"
#include "ps2.h"
#include "x86.h"

#define PS2_DATA   0x60
#define PS2_STATUS 0x64
#define PS2_CMD    0x64
#define ST_OUTPUT  0x01
#define ST_INPUT   0x02
#define ST_AUX     0x20
#define IRQ_MOUSE  12

#define CFG_IRQ12    0x02
#define CFG_AUX_OFF  0x20

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
static int touchpad;                    /* touchpad.c has the device */
static int present;                     /* something answered on the port */
static uint8_t boot_cfg;                /* the controller's configuration, as we set it at boot */

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

/* A byte from the auxiliary device within ms; keyboard bytes are dropped. */
int ps2_aux_read(uint8_t *b, uint32_t ms)
{
    for (uint32_t i = 0; i < ms * 20; i++) {
        uint8_t st = inb(PS2_STATUS);
        if (st & ST_OUTPUT) {
            uint8_t v = inb(PS2_DATA);
            if (st & ST_AUX) {
                *b = v;
                return 0;
            }
            continue;
        }
        udelay(50);
    }
    return -1;
}

/* Send a byte to the mouse and wait for its acknowledgement (FAh); a
 * resend request (FEh) gets it once more. */
int ps2_aux_cmd(uint8_t b)
{
    for (int tries = 0; tries < 2; tries++) {
        ctrl_cmd(0xD4);
        wait_write();
        outb(PS2_DATA, b);
        uint8_t r;
        if (ps2_aux_read(&r, 50) < 0)
            return -1;
        if (r == 0xFA)
            return 0;
        if (r != 0xFE)
            return -1;
    }
    return -1;
}

int ps2_aux_query(uint8_t cmd, uint8_t *resp, int n)
{
    if (ps2_aux_cmd(cmd) < 0)
        return -1;
    for (int i = 0; i < n; i++)
        if (ps2_aux_read(&resp[i], 50) < 0)
            return -1;
    return 0;
}

/* Send a byte to the mouse and swallow its acknowledgement. */
static void mouse_cmd(uint8_t b)
{
    ps2_aux_cmd(b);
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

static int vmware(void)
{
    struct vmw_regs r;
    vmw_call(VMW_GETVERSION, 0, &r);
    return r.b == VMW_MAGIC;
}

static int vmmouse_enable(void)
{
    struct vmw_regs r;
    if (!vmware())
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
        if (touchpad) {
            touchpad_byte(b);
            continue;
        }
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

/* The controller's configuration: the firmware's, with the auxiliary
 * port's clock and interrupt on; after suspend, that again. */
static void ctrl_setup(int resume)
{
    if (resume)
        ctrl_cmd(0xAE);                 /* the keyboard port on */
    ctrl_cmd(0xA8);                     /* the auxiliary port on */
    uint8_t cfg = boot_cfg;
    if (!resume) {
        ctrl_cmd(0x20);                 /* read the controller config */
        if (!wait_read())
            return;
        cfg = inb(PS2_DATA);
        cfg |= CFG_IRQ12;
        cfg &= (uint8_t)~CFG_AUX_OFF;
        boot_cfg = cfg;
    } else if (!cfg) {
        return;
    }
    ctrl_cmd(0x60);
    wait_write();
    outb(PS2_DATA, cfg);
}

/* A plain mouse, with the wheel if it has one. */
static void mouse_setup(void)
{
    packet_size = 3;
    npacket = 0;
    mouse_cmd(0xF6);                    /* defaults */
    /* The IntelliMouse knock: sample rates 200, 100, 80 turn on the wheel. */
    static const uint8_t knock[] = { 200, 100, 80 };
    for (int i = 0; i < 3; i++) {
        mouse_cmd(0xF3);
        mouse_cmd(knock[i]);
    }
    uint8_t id;
    if (ps2_aux_query(0xF2, &id, 1) == 0 && id == 3)        /* identify */
        packet_size = 4;
    mouse_cmd(0xF3);
    mouse_cmd(100);                     /* back to the default rate */
    mouse_cmd(0xF4);                    /* start streaming */
}

void mouse_init(void)
{
    ctrl_setup(0);
    for (int i = 0; i < 64 && (inb(PS2_STATUS) & ST_OUTPUT); i++)
        (void)inb(PS2_DATA);
    present = 1;
    if (!cmdline_has("touchpad=off") && !vmware())
        touchpad = touchpad_probe();
    if (!touchpad)
        mouse_setup();
    vmmouse = vmmouse_enable();
    irq_register(IRQ_MOUSE, mouse_irq);
    if (touchpad)
        boot_ok("PS/2 touchpad ready: %s", touchpad_name());
    else
        boot_ok("PS/2 mouse ready%s%s%s%s", packet_size == 4 ? " with a wheel" : "",
                vmmouse ? " (absolute positioning via vmmouse)" : "",
                touchpad_name()[0] ? ": " : "", touchpad_name());
}

/*
 * After suspend to RAM. The firmware may have set the controller up for
 * itself and the devices lost their settings: both ports again, the
 * keyboard (keyboard.c), then the mouse or touchpad. Polled, with
 * interrupts off, so the handlers do not take the answers.
 */
void i8042_resume(void)
{
    uint32_t flags = irq_save();
    for (int i = 0; i < 64 && (inb(PS2_STATUS) & ST_OUTPUT); i++)
        (void)inb(PS2_DATA);
    ctrl_setup(1);
    keyboard_resume();
    if (present && !vmmouse) {
        if (touchpad)
            touchpad = touchpad_resume();
        if (!touchpad)
            mouse_setup();
    }
    npacket = 0;
    irq_restore(flags);
}

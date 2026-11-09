/*
 * The ACPI embedded controller (PNP0C09): the small microcontroller on a
 * laptop's motherboard that knows the battery, the lid, the fans and the
 * Fn keys. AML reads and writes its 256 bytes of registers through an
 * EmbeddedControl operation region, which this driver serves; and when the
 * EC has news it raises its GPE with SCI_EVT set, and a query command
 * tells which _Qxx method of the EC's device to run (they mostly Notify()
 * the battery, AC adapter or lid, which power.c hears about).
 *
 * Two ports: data, and command/status. A transaction writes a command,
 * then bytes, each once the EC has taken the last (IBF clear), and reads
 * each answer once it is there (OBF set). It is polled: a byte takes the
 * EC tens of microseconds, sometimes a few milliseconds, and a process
 * waiting longer sleeps a tick at a time. One transaction at a time, with
 * the ACPI global lock too if the firmware's _GLK asks for it.
 *
 * The ECDT, where there is one, describes the EC before the namespace
 * loads, so AML run while it loads can use it already; otherwise the
 * PNP0C09 device's _CRS gives the ports once it has.
 */
#include <uacpi/acpi.h>
#include <uacpi/event.h>
#include <uacpi/namespace.h>
#include <uacpi/opregion.h>
#include <uacpi/resources.h>
#include <uacpi/tables.h>
#include <uacpi/uacpi.h>
#include <uacpi/utilities.h>
#include "kernel.h"
#include "acpi.h"
#include "drivers.h"
#include "proc.h"
#include "x86.h"

#define EC_OBF     0x01                 /* output buffer full: a byte to read */
#define EC_IBF     0x02                 /* input buffer full: the EC has not taken ours */
#define EC_SCI_EVT 0x20                 /* an event waits for a query */

#define EC_READ  0x80
#define EC_WRITE 0x81
#define EC_QUERY 0x84

static struct {
    int found;
    uint16_t data, cmd;
    uacpi_namespace_node *node;         /* the device: its _Qxx methods */
    int handler_on_root;                /* installed from the ECDT */
    int gpe;                            /* -1: none */
    int glk;                            /* take the global lock */
    int timeouts;
} ec = { .gpe = -1 };

static spinlock_t ec_lk = SPINLOCK_INIT("ec");
static int ec_busy;                     /* a transaction is under way */
static volatile int query_queued;

/* ---- one transaction at a time ---- */

static void ec_lock(void)
{
    int can_sleep = osl_can_sleep();
    spin_lock(&ec_lk);
    while (ec_busy) {
        if (can_sleep) {
            sleep_on_lock(&ec_busy, &ec_lk);
        } else {
            spin_unlock(&ec_lk);
            cpu_relax();
            spin_lock(&ec_lk);
        }
    }
    ec_busy = 1;
    spin_unlock(&ec_lk);
}

static void ec_unlock(void)
{
    spin_lock(&ec_lk);
    ec_busy = 0;
    wakeup(&ec_busy);
    spin_unlock(&ec_lk);
}

/* Wait for (status & mask) == want: 0, or -1 after a second. */
static int ec_wait(uint8_t mask, uint8_t want)
{
    for (int i = 0; i < 200; i++) {     /* the first millisecond, closely */
        if ((inb(ec.cmd) & mask) == want)
            return 0;
        udelay(5);
    }
    int can_sleep = osl_can_sleep();
    for (int i = 0; i < 1000; i += can_sleep ? 1000 / HZ : 1) {
        if ((inb(ec.cmd) & mask) == want)
            return 0;
        if (can_sleep)
            sleep_ticks(1);
        else
            udelay(1000);
    }
    return (inb(ec.cmd) & mask) == want ? 0 : -1;
}

static int ec_send(uint16_t port, uint8_t v)
{
    if (ec_wait(EC_IBF, 0) < 0)
        return -1;
    outb(port, v);
    return 0;
}

static int ec_recv(uint8_t *v)
{
    if (ec_wait(EC_OBF, EC_OBF) < 0)
        return -1;
    *v = inb(ec.data);
    return 0;
}

static void timed_out(const char *what, uint8_t addr)
{
    if (ec.timeouts++ < 8)
        klog("ec: %s %02x timed out (status %02x)\n", what, addr, inb(ec.cmd));
}

static int ec_read(uint8_t addr, uint8_t *v)
{
    if (ec_send(ec.cmd, EC_READ) < 0 || ec_send(ec.data, addr) < 0 || ec_recv(v) < 0) {
        timed_out("read", addr);
        return -1;
    }
    return 0;
}

static int ec_write(uint8_t addr, uint8_t v)
{
    if (ec_send(ec.cmd, EC_WRITE) < 0 || ec_send(ec.data, addr) < 0 || ec_send(ec.data, v) < 0 ||
        ec_wait(EC_IBF, 0) < 0) {
        timed_out("write", addr);
        return -1;
    }
    return 0;
}

static uint32_t glk_take(void)
{
    uacpi_u32 seq = 0;
    if (ec.glk)
        uacpi_acquire_global_lock(0xFFFF, &seq);
    return seq;
}

static void glk_give(uint32_t seq)
{
    if (ec.glk)
        uacpi_release_global_lock(seq);
}

/* ---- the EmbeddedControl operation region ---- */

static uacpi_status region_handler(uacpi_region_op op, uacpi_handle data)
{
    if (op == UACPI_REGION_OP_ATTACH || op == UACPI_REGION_OP_DETACH)
        return UACPI_STATUS_OK;
    if (op != UACPI_REGION_OP_READ && op != UACPI_REGION_OP_WRITE)
        return UACPI_STATUS_INVALID_ARGUMENT;
    uacpi_region_rw_data *rw = data;
    if (rw->offset + rw->byte_width > 256)
        return UACPI_STATUS_INVALID_ARGUMENT;
    uint8_t addr = (uint8_t)rw->offset;
    int r = 0;
    uint32_t seq = glk_take();
    ec_lock();
    if (op == UACPI_REGION_OP_READ) {
        rw->value = 0;
        for (int i = 0; i < rw->byte_width && !r; i++) {
            uint8_t b = 0;
            r = ec_read((uint8_t)(addr + i), &b);
            rw->value |= (uint64_t)b << (8 * i);
        }
    } else {
        for (int i = 0; i < rw->byte_width && !r; i++)
            r = ec_write((uint8_t)(addr + i), (uint8_t)(rw->value >> (8 * i)));
    }
    ec_unlock();
    glk_give(seq);
    return r ? UACPI_STATUS_HARDWARE_TIMEOUT : UACPI_STATUS_OK;
}

/* ---- events ---- */

static void run_query(uint8_t q)
{
    if (!ec.node)
        return;
    static const char hex[] = "0123456789ABCDEF";
    char name[5] = { '_', 'Q', hex[q >> 4], hex[q & 15], 0 };
    uacpi_status st = uacpi_eval(ec.node, name, NULL, NULL);
    if (st != UACPI_STATUS_OK && st != UACPI_STATUS_NOT_FOUND)
        klog("ec: %s: %s\n", name, uacpi_status_to_string(st));
}

/* In the GPE worker: ask what happened until the EC has no more news. */
static void query_work(void *arg)
{
    (void)arg;
    __atomic_store_n(&query_queued, 0, __ATOMIC_RELEASE);
    for (int n = 0; n < 32; n++) {
        if (!(inb(ec.cmd) & EC_SCI_EVT))
            break;
        uint8_t q = 0;
        uint32_t seq = glk_take();
        ec_lock();
        int r = ec_send(ec.cmd, EC_QUERY) < 0 || ec_recv(&q) < 0;
        ec_unlock();
        glk_give(seq);
        if (r) {
            timed_out("query", 0);
            break;
        }
        if (!q)
            break;
        run_query(q);
    }
}

static void queue_query(void)
{
    if (!__atomic_exchange_n(&query_queued, 1, __ATOMIC_ACQ_REL) && osl_defer(query_work, NULL) < 0)
        __atomic_store_n(&query_queued, 0, __ATOMIC_RELEASE);
}

static uacpi_interrupt_ret gpe_handler(uacpi_handle ctx, uacpi_namespace_node *dev, uacpi_u16 idx)
{
    (void)ctx;
    (void)dev;
    (void)idx;
    if (inb(ec.cmd) & EC_SCI_EVT)
        queue_query();
    return UACPI_INTERRUPT_HANDLED | UACPI_GPE_REENABLE;
}

/* From the power thread now and then: an event whose GPE was missed. */
void ec_poll(void)
{
    if (ec.found && ec.gpe >= 0 && (inb(ec.cmd) & EC_SCI_EVT))
        queue_query();
}

/* ---- finding it ---- */

/* The ECDT: the EC's ports, GPE and device path, before the namespace loads. */
void ec_early(void)
{
    uacpi_table t;
    if (cmdline_has("ec=off") || uacpi_table_find_by_signature(ACPI_ECDT_SIGNATURE, &t) != UACPI_STATUS_OK)
        return;
    struct acpi_ecdt *e = t.ptr;
    if (e->hdr.length >= sizeof(*e) && e->ec_control.address_space_id == UACPI_ADDRESS_SPACE_SYSTEM_IO &&
        e->ec_data.address_space_id == UACPI_ADDRESS_SPACE_SYSTEM_IO && e->ec_control.address &&
        e->ec_data.address && e->ec_control.address < 0x10000 && e->ec_data.address < 0x10000) {
        ec.cmd = (uint16_t)e->ec_control.address;
        ec.data = (uint16_t)e->ec_data.address;
        ec.gpe = e->gpe_bit;
        if (uacpi_install_address_space_handler(uacpi_namespace_root(),
                                                UACPI_ADDRESS_SPACE_EMBEDDED_CONTROLLER,
                                                region_handler, NULL) == UACPI_STATUS_OK) {
            ec.found = 1;
            ec.handler_on_root = 1;
            klog("ec: from the ECDT: ports %04x/%04x, GPE %d, %s\n", ec.data, ec.cmd, ec.gpe, e->ec_id);
        }
    }
    uacpi_table_unref(&t);
}

struct ports {
    int n;
    uint16_t port[2];
};

static uacpi_iteration_decision crs_port(void *user, uacpi_resource *r)
{
    struct ports *p = user;
    if (p->n < 2 && r->type == UACPI_RESOURCE_TYPE_IO)
        p->port[p->n++] = r->io.minimum;
    else if (p->n < 2 && r->type == UACPI_RESOURCE_TYPE_FIXED_IO)
        p->port[p->n++] = r->fixed_io.address;
    return UACPI_ITERATION_DECISION_CONTINUE;
}

static uacpi_iteration_decision found_ec(void *user, uacpi_namespace_node *node, uacpi_u32 depth)
{
    (void)user;
    (void)depth;
    if (ec.node && !ec.handler_on_root)
        return UACPI_ITERATION_DECISION_BREAK;          /* one is enough */
    struct ports p = { 0 };
    if (!ec.handler_on_root) {
        if (uacpi_for_each_device_resource(node, "_CRS", crs_port, &p) != UACPI_STATUS_OK || p.n < 2)
            return UACPI_ITERATION_DECISION_CONTINUE;
        ec.data = p.port[0];
        ec.cmd = p.port[1];
    }
    ec.node = node;
    uint64_t v;
    if (acpi_eval_int(node, "_GPE", &v) == 0 && v < 256)
        ec.gpe = (int)v;
    ec.glk = acpi_eval_int(node, "_GLK", &v) == 0 && v;
    return UACPI_ITERATION_DECISION_BREAK;
}

/* The PNP0C09 device, once the namespace has loaded: its ports (unless the
 * ECDT gave them), GPE, and _Qxx methods. */
void ec_init(void)
{
    if (cmdline_has("ec=off"))
        return;
    uacpi_find_devices("PNP0C09", found_ec, NULL);
    if (!ec.node && !ec.found)
        return;
    if (!ec.handler_on_root) {
        if (uacpi_install_address_space_handler(ec.node, UACPI_ADDRESS_SPACE_EMBEDDED_CONTROLLER,
                                                region_handler, NULL) != UACPI_STATUS_OK)
            return;
        ec.found = 1;
    }
    boot_ok("ACPI embedded controller at %04x/%04x, GPE %d%s", ec.data, ec.cmd, ec.gpe,
            ec.glk ? ", global lock" : "");
}

void ec_start(void)
{
    if (!ec.found || ec.gpe < 0)
        return;
    if (uacpi_install_gpe_handler(NULL, (uint16_t)ec.gpe, UACPI_GPE_TRIGGERING_EDGE, gpe_handler, NULL) !=
        UACPI_STATUS_OK)
        klog("ec: cannot take GPE %d\n", ec.gpe);
}

/* After S3: events may have come while it slept. */
void ec_resume(void)
{
    ec_poll();
}

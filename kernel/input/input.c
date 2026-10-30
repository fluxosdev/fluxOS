/*
 * /dev/input: keyboard and mouse events for the desktop, as struct
 * input_event records. While the device is open the keyboard reports
 * here instead of typing on the console; mouse events are only queued
 * while it is open.
 */
#include "kernel.h"
#include "drivers.h"
#include "proc.h"
#include "vfs.h"

#define QSIZE 256

static struct input_event queue[QSIZE];
static uint32_t qhead, qtail;           /* free-running */
static struct vnode *input_vn;
static spinlock_t input_lock = SPINLOCK_INIT("input");

int input_grabbed(void)
{
    return input_vn && input_vn->refs > 0;
}

static int same_motion(const struct input_event *a, const struct input_event *b)
{
    return a->type == IEV_MOUSE && b->type == IEV_MOUSE &&
           a->flags == b->flags && a->mods == b->mods && !a->code && !b->code;
}

/* Called from IRQ handlers. */
void input_push(const struct input_event *ev)
{
    if (!input_grabbed())
        return;
    spin_lock(&input_lock);
    /* Merge motion with the newest unread event when no button changed. */
    if (qhead != qtail && same_motion(&queue[(qhead - 1) % QSIZE], ev)) {
        struct input_event *last = &queue[(qhead - 1) % QSIZE];
        if (ev->flags & IEV_ABS) {
            last->x = ev->x;
            last->y = ev->y;
        } else {
            last->x = (int16_t)(last->x + ev->x);
            last->y = (int16_t)(last->y + ev->y);
        }
    } else {
        if (qhead - qtail == QSIZE)
            qtail++;                    /* drop the oldest */
        queue[qhead++ % QSIZE] = *ev;
    }
    wakeup(queue);
    poll_wakeup();
    spin_unlock(&input_lock);
}

/* buf is the reader's, checked beforehand: it is filled holding the lock. */
static int input_read(struct vnode *vn, void *buf, uint64_t off, uint32_t n)
{
    (void)vn; (void)off;
    if (n < sizeof(struct input_event))
        return -EINVAL;
    spin_lock(&input_lock);
    while (qhead == qtail) {
        if (signal_pending(current)) {
            spin_unlock(&input_lock);
            return -EINTR;
        }
        sleep_on_lock_intr(queue, &input_lock);
    }
    struct input_event *out = buf;
    uint32_t i = 0;
    while (qtail != qhead && (i + 1) * sizeof(*out) <= n)
        out[i++] = queue[qtail++ % QSIZE];
    spin_unlock(&input_lock);
    return (int)(i * sizeof(*out));
}

static int input_poll(struct vnode *vn)
{
    (void)vn;
    return __atomic_load_n(&qhead, __ATOMIC_RELAXED) != __atomic_load_n(&qtail, __ATOMIC_RELAXED)
           ? POLLIN : 0;
}

void input_register(struct vnode *devdir)
{
    input_vn = vfs_mknod(devdir, "input", MKDEV(INPUT_MAJOR, 0), input_read, NULL);
    if (input_vn) {
        input_vn->mode = 0640;
        input_vn->dev_poll = input_poll;
    }
}

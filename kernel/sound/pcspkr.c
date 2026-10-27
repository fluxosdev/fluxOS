/*
 * The PC speaker: PIT channel 2 makes a square wave, and bits 0 (gate)
 * and 1 (speaker data) of port 61h connect it to the speaker.
 *
 * /dev/speaker plays a queue of tones in the background, timed by the
 * scheduler tick (10 ms steps): a write queues struct spkr_tone records,
 * blocking only while the queue is full. SPKR_SET instead holds one
 * frequency until it is changed, for programs that drive the speaker
 * directly (the DOS emulator); the process that set it falls silent
 * when it closes the device. The console's bell (^G) beeps here too.
 */
#include "kernel.h"
#include "drivers.h"
#include "proc.h"
#include "syscall.h"
#include "vfs.h"
#include "x86.h"

#define PIT_HZ 1193182u
#define QLEN   128
#define MS_PER_TICK (1000 / HZ)

static struct spkr_tone queue[QLEN];
static uint32_t qhead, qtail;           /* free-running */
static int playing;                     /* a queued tone is sounding */
static int32_t left_ms;                 /* of the current tone */
static uint32_t held_freq;              /* SPKR_SET's tone, between queued ones */
static int held_pid;

static void sound(uint32_t freq)
{
    if (freq < 19 || freq > 20000) {
        outb(0x61, inb(0x61) & 0xFC);
        return;
    }
    uint32_t div = PIT_HZ / freq;
    outb(0x43, 0xB6);                   /* channel 2, lo/hi byte, square wave */
    outb(0x42, (uint8_t)div);
    outb(0x42, (uint8_t)(div >> 8));
    outb(0x61, inb(0x61) | 0x03);
}

/* Start the next queued tone, or go back to the held one. */
static void next_tone(void)
{
    if (qtail == qhead) {
        playing = 0;
        sound(held_freq);
        return;
    }
    struct spkr_tone *t = &queue[qtail++ % QLEN];
    playing = 1;
    left_ms = (int32_t)t->ms;
    sound(t->freq);
    wakeup(queue);
}

/* The timer interrupt, HZ times a second. */
void spkr_tick(void)
{
    if (!playing)
        return;
    left_ms -= MS_PER_TICK;
    if (left_ms <= 0)
        next_tone();
}

static int queue_tone(uint32_t freq, uint32_t ms)
{
    if (qhead - qtail == QLEN)
        return -1;
    if (ms > 60000)
        ms = 60000;
    queue[qhead].freq = freq;
    queue[qhead].ms = ms;
    qhead++;
    if (!playing)
        next_tone();
    return 0;
}

/* A short beep, unless the queue is full (the console bell). */
void spkr_beep(uint32_t freq, uint32_t ms)
{
    queue_tone(freq, ms);
}

static int spkr_write(struct vnode *vn, const void *buf, uint32_t off, uint32_t n)
{
    (void)vn; (void)off;
    const struct spkr_tone *t = buf;
    uint32_t count = n / sizeof(*t), done = 0;
    while (done < count) {
        if (queue_tone(t[done].freq, t[done].ms) == 0) {
            done++;
            continue;
        }
        if (signal_pending(current))
            return done ? (int)(done * sizeof(*t)) : -EINTR;
        sleep_on(queue);
    }
    return (int)(done * sizeof(*t));
}

static int spkr_ioctl(struct vnode *vn, uint32_t req, uint32_t arg)
{
    (void)vn;
    switch (req) {
    case SPKR_TONE: {
        if (!user_access_ok((void *)arg, sizeof(struct spkr_tone)))
            return -EFAULT;
        const struct spkr_tone *t = (const struct spkr_tone *)arg;
        while (queue_tone(t->freq, t->ms) < 0) {
            if (signal_pending(current))
                return -EINTR;
            sleep_on(queue);
        }
        return 0;
    }
    case SPKR_SET:
        qtail = qhead;
        playing = 0;
        held_freq = arg;
        held_pid = arg ? current->pid : 0;
        sound(arg);
        wakeup(queue);
        return 0;
    case SPKR_STOP:
        qtail = qhead;
        playing = 0;
        held_freq = 0;
        held_pid = 0;
        sound(0);
        wakeup(queue);
        return 0;
    case SOUND_INFO:
        if (!user_access_ok((void *)arg, sizeof(struct sound_info)))
            return -EFAULT;
        sound_get_info((struct sound_info *)arg);
        return 0;
    }
    return -EINVAL;
}

static void spkr_close(struct vnode *vn, int flags)
{
    (void)vn; (void)flags;
    if (held_pid && held_pid == current->pid) {
        held_freq = 0;
        held_pid = 0;
        if (!playing)
            sound(0);
    }
}

void spkr_init(struct vnode *devdir)
{
    sound(0);
    struct vnode *vn = vfs_mknod(devdir, "speaker", MKDEV(SOUND_MAJOR, SOUND_MINOR_SPEAKER),
                                 NULL, spkr_write);
    if (vn) {
        vn->dev_ioctl = spkr_ioctl;
        vn->dev_close = spkr_close;
    }
}

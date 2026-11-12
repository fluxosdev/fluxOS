/*
 * Shared memory segments, System V style: shmget makes one (or finds it
 * by key), shmat maps it into the caller, shmctl's IPC_RMID takes its id
 * away. A segment holds a reference on each of its frames and every
 * mapping holds one more, so the memory lasts as long as anything maps
 * it, whatever became of the id; fork shares the mappings. A segment
 * still there when the process that made it exits is removed with it.
 *
 * The desktop shares windows' pixels with the programs drawing them this
 * way (user/desktop/shell/server.c).
 */
#include "kernel.h"
#include "lock.h"
#include "mm.h"
#include "proc.h"
#include "syscall.h"

#define SHM_SEGS    1024
#define SHM_MAXSIZE (64u << 20)
/*
 * Segments made and not yet removed are limited per process, so that a few
 * programs cannot take them all (Linux has SHMMNI for the system). The limit
 * is the web engine's own quota of shared buffers. The process that has the
 * screen, the desktop, makes one for each window of every program it serves:
 * it is not limited, and the others together leave it SHM_RESERVE.
 */
#define SHM_PER_PROC 128
#define SHM_RESERVE  256

struct shm_seg {
    int used;
    int key;                    /* IPC_PRIVATE: none */
    int creator;                /* its process's pid */
    int display;                /* made by the process that had the screen: in no limit */
    uint32_t size, npages;
    uint32_t *frames;
    uint32_t seq;               /* ids are slot + SHM_SEGS * seq: an old id names no new segment */
};

static struct shm_seg segs[SHM_SEGS];
static spinlock_t shm_lock = SPINLOCK_INIT("shm");

static int seg_id(int slot)
{
    return (int)(((segs[slot].seq & (0x7FFFFFFFu / SHM_SEGS)) * SHM_SEGS) + (uint32_t)slot);
}

/* With shm_lock: the slot id names, or -1. */
static int seg_slot(int id)
{
    if (id < 0)
        return -1;
    int slot = id % SHM_SEGS;
    return segs[slot].used && seg_id(slot) == id ? slot : -1;
}

static void free_frames(uint32_t *frames, uint32_t n)
{
    for (uint32_t i = 0; i < n; i++)
        pmm_free(frames[i]);
    kfree(frames);
}

/* With shm_lock: empty the slot, handing back its frames to free once it is let go. */
static uint32_t *seg_remove(int slot, uint32_t *n)
{
    uint32_t *frames = segs[slot].frames;
    *n = segs[slot].npages;
    segs[slot].used = 0;
    segs[slot].frames = NULL;
    segs[slot].seq++;
    return frames;
}

/* With shm_lock: may process pid (display: it has the screen) make another segment? */
static int may_create(int pid, int display)
{
    if (display)
        return 1;
    int mine = 0, others = 0;
    for (int i = 0; i < SHM_SEGS; i++) {
        if (!segs[i].used)
            continue;
        if (segs[i].creator == pid)
            mine++;
        if (!segs[i].display)
            others++;
    }
    return mine < SHM_PER_PROC && others < SHM_SEGS - SHM_RESERVE;
}

static int shm_create(int key, uint32_t size)
{
    if (!size || size > SHM_MAXSIZE)
        return -EINVAL;
    int pid = current->leader->pid;
    int display = vmm_mmio_mapped(current->pd, USER_MMIO_BASE);    /* the screen, by fbmap */
    spin_lock(&shm_lock);
    int ok = may_create(pid, display);
    spin_unlock(&shm_lock);
    if (!ok)
        return -ENOSPC;                 /* (looked at again once the memory is there) */
    uint32_t n = ALIGN_UP(size, PAGE_SIZE) / PAGE_SIZE;
    uint32_t *frames = kzalloc(n * sizeof(*frames));
    if (!frames)
        return -ENOMEM;
    for (uint32_t i = 0; i < n; i++) {
        frames[i] = pmm_alloc_user_low();    /* (below 4 GiB: its frames are kept in 32 bits, and the GPU may be lent them) */
        if (!frames[i]) {
            free_frames(frames, i);
            return -ENOMEM;
        }
        vmm_zero_frame(frames[i]);
    }
    spin_lock(&shm_lock);
    int slot = -1;
    for (int i = 0; i < SHM_SEGS && slot < 0; i++)
        if (!segs[i].used)
            slot = i;
    if (slot < 0 || !may_create(pid, display)) {
        spin_unlock(&shm_lock);
        free_frames(frames, n);
        return -ENOSPC;
    }
    segs[slot].used = 1;
    segs[slot].key = key;
    segs[slot].creator = pid;
    segs[slot].display = display;
    segs[slot].size = size;
    segs[slot].npages = n;
    segs[slot].frames = frames;
    int id = seg_id(slot);
    spin_unlock(&shm_lock);
    return id;
}

int sys_shmget(uint32_t key, uint32_t size, uint32_t flags, uint32_t d, uint32_t e)
{
    (void)d; (void)e;
    if ((int)key != IPC_PRIVATE) {
        spin_lock(&shm_lock);
        for (int i = 0; i < SHM_SEGS; i++) {
            if (segs[i].used && segs[i].key == (int)key) {
                int id = seg_id(i);
                int too_small = size > segs[i].size;
                spin_unlock(&shm_lock);
                if ((flags & (IPC_CREAT | IPC_EXCL)) == (IPC_CREAT | IPC_EXCL))
                    return -EEXIST;
                return too_small ? -EINVAL : id;
            }
        }
        spin_unlock(&shm_lock);
        if (!(flags & IPC_CREAT))
            return -ENOENT;
    }
    return shm_create((int)key, size);
}

long sys_shmat(uint32_t id, uint64_t usize, uint32_t c, uint32_t d, uint32_t e)
{
    (void)c; (void)d; (void)e;
    /* The frames are copied out with a reference each for the mapping, so removing the
     * segment meanwhile cannot free them under it. */
    spin_lock(&shm_lock);
    int slot = seg_slot((int)id);
    uint32_t n = slot < 0 ? 0 : segs[slot].npages, size = slot < 0 ? 0 : segs[slot].size;
    spin_unlock(&shm_lock);
    if (slot < 0)
        return -EINVAL;
    uint32_t *frames = kmalloc(n * sizeof(*frames));
    if (!frames)
        return -ENOMEM;
    spin_lock(&shm_lock);
    if (seg_slot((int)id) != slot) {
        spin_unlock(&shm_lock);
        kfree(frames);
        return -EINVAL;                 /* removed meanwhile */
    }
    uint32_t got = 0;
    for (; got < n; got++) {
        frames[got] = segs[slot].frames[got];
        if (!pmm_ref(frames[got]))
            break;                      /* shared too widely */
    }
    spin_unlock(&shm_lock);
    uint64_t addr = 0;
    int r = got < n ? -ENOMEM : proc_map_frames(current, frames, n, &addr);
    if (r < 0) {
        for (uint32_t i = 0; i < got; i++)
            pmm_free(frames[i]);
        kfree(frames);
        return r;
    }
    kfree(frames);
    if (usize && copy_to_user(UPTR(usize), &size, sizeof(size)) < 0) {
        proc_munmap(current, addr, (uint64_t)n * PAGE_SIZE);
        return -EFAULT;
    }
    return (long)addr;
}

int sys_shmctl(uint32_t id, uint32_t cmd, uint64_t ubuf, uint32_t d, uint32_t e)
{
    (void)d; (void)e;
    spin_lock(&shm_lock);
    int slot = seg_slot((int)id);
    if (slot < 0) {
        spin_unlock(&shm_lock);
        return -EINVAL;
    }
    if (cmd == IPC_STAT) {
        struct flux_shmid_ds ds = { segs[slot].size, segs[slot].creator, 0 };
        spin_unlock(&shm_lock);
        return copy_to_user(UPTR(ubuf), &ds, sizeof(ds)) < 0 ? -EFAULT : 0;
    }
    if (cmd != IPC_RMID) {
        spin_unlock(&shm_lock);
        return -EINVAL;
    }
    uint32_t n;
    uint32_t *frames = seg_remove(slot, &n);
    spin_unlock(&shm_lock);
    free_frames(frames, n);
    return 0;
}

/* The process pid is exiting: the segments it made go (their memory stays while mapped). */
void shm_proc_exit(int pid)
{
    for (int i = 0; i < SHM_SEGS; i++) {
        spin_lock(&shm_lock);
        uint32_t n = 0, *frames = NULL;
        if (segs[i].used && segs[i].creator == pid)
            frames = seg_remove(i, &n);
        spin_unlock(&shm_lock);
        if (frames)
            free_frames(frames, n);
    }
}

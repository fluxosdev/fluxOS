/*
 * System call entry points. Arguments arrive in rbx, rcx, rdx, rsi and rdi,
 * the result goes back in rax; every user pointer is validated (and the
 * stack demand-grown) before the kernel touches it.
 *
 * A 64-bit program passes 64-bit values, its addresses among them, and gets
 * one back. A 32-bit program's registers are the low halves, ebx to edi
 * and eax: what it passes arrives zero-extended. A handler declares each
 * argument as wide as it takes it: an address or the size of a piece of
 * the address space as a uint64_t (UPTR() makes a pointer of an address),
 * the size of a file or a position in one likewise, and everything else as
 * the 32 bits it always was, the low half of the register whatever the
 * program put in the rest.
 *
 * What is read from a program's memory has the program's layout. The
 * structures of <flux/abi.h> are the same in both kinds of program except
 * for the few that hold an address or such a size (a file's among them:
 * struct stat), which are as wide there as the program's pointers: the
 * kernel has the 64-bit layout under the structure's name and a 32-bit
 * program's in compat.h.
 */
#include "kernel.h"
#include "blkdev.h"
#include "compat.h"
#include "drivers.h"
#include "fs.h"
#include "mm.h"
#include "net.h"
#include "proc.h"
#include "syscall.h"
#include "tty.h"
#include "vfs.h"
#include <flux/syscall.h>

typedef long (*syscall_fn)(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);

/* ---- user memory access ---- */

static int user_check(const void *ptr, uint64_t len)
{
    uint64_t a = (uintptr_t)ptr, top = user_top(current);
    if (!len)
        return 0;
    if (a < USER_BASE || a >= top || len > top - a)
        return -EFAULT;
    for (uint64_t pg = ALIGN_DOWN(a, PAGE_SIZE); pg < a + len; pg += PAGE_SIZE)
        if (!vmm_translate(current->pd, pg) && !proc_grow_stack(pg))
            return -EFAULT;
    return 0;
}

static int user_check_w(void *ptr, uint64_t len)
{
    uint64_t a = (uintptr_t)ptr, top = user_top(current);
    if (!len)
        return 0;
    if (a < USER_BASE || a >= top || len > top - a)
        return -EFAULT;
    for (uint64_t pg = ALIGN_DOWN(a, PAGE_SIZE); pg < a + len; pg += PAGE_SIZE)
        if (!vmm_write_ok(current->pd, pg) && !proc_page_fault(pg, 1))
            return -EFAULT;
    return 0;
}

/*
 * Copies to and from a process's memory. The range is checked first,
 * which also pages it in; should a page still be missing when the copy
 * runs, the copy itself gives up with -EFAULT (entry.S, idt.c) instead
 * of the kernel faulting on its own stack.
 */
int copy_from_user(void *dst, const void *usrc, uint32_t n)
{
    int r = user_check(usrc, n);
    return r < 0 ? r : uaccess_copy(dst, usrc, n);
}

int copy_to_user(void *udst, const void *src, uint32_t n)
{
    int r = user_check_w(udst, n);
    return r < 0 ? r : uaccess_copy(udst, src, n);
}

/* Copy a NUL-terminated user string; returns its length. It is copied in
 * short pieces that stay within a page (and max), so dst may be written a
 * little past the NUL. */
static int copy_str(char *dst, const char *src, uint32_t max)
{
    uint64_t a = (uintptr_t)src;
    uint32_t i = 0;
    while (i < max) {
        uint32_t n = MIN(MIN(max - i, PAGE_SIZE - (a & 0xFFF)), 256u);
        if (copy_from_user(dst + i, UPTR(a), n) < 0)
            return -EFAULT;
        for (uint32_t end = i + n; i < end; i++)
            if (!dst[i])
                return (int)i;
        a += n;
    }
    return -ENAMETOOLONG;
}

/* A user path, copied into one of the thread's path buffers (allocated the first time and kept:
 * PATH_MAX is too much for the kernel stack, and a system call wants up to three). */
static int path_buf(char **out)
{
    struct proc *p = current;
    if (p->npaths >= 3)
        return -ENOMEM;
    char *b = p->paths[p->npaths];
    if (!b && !(b = p->paths[p->npaths] = kmalloc(PATH_MAX)))
        return -ENOMEM;
    p->npaths++;
    *out = b;
    return 0;
}

static int user_path(char **out, uint64_t uptr)
{
    char *b;
    int r = path_buf(&b);
    if (r < 0)
        return r;
    r = copy_str(b, (const char *)UPTR(uptr), PATH_MAX);
    if (r < 0)
        return r;
    *out = b;
    return 0;
}

#define COPY_PATH(var, uptr)                            \
    char *var;                                          \
    do {                                                \
        int _r = user_path(&var, (uptr));               \
        if (_r < 0)                                     \
            return _r;                                  \
    } while (0)

/*
 * The file behind descriptor fd, held until the system call returns: the
 * descriptor table is shared by the process's threads, and another one may
 * close fd (dropping the table's reference) while this call still uses it.
 */
static struct file *get_file(uint32_t fd)
{
    if (fd >= MAX_FDS)
        return NULL;
    struct proc *g = current->leader;
    spin_lock(&g->fd_lock);
    struct file *f = file_dup(g->fds[fd]);
    spin_unlock(&g->fd_lock);
    if (!f)
        return NULL;
    if (current->nheld >= (int)(sizeof(current->held) / sizeof(current->held[0])))
        panic("get_file: more than %d descriptors in one system call", current->nheld);
    current->held[current->nheld++] = f;
    return f;
}

/* The end of a system call: the references get_file took. */
static void put_held_files(void)
{
    while (current->nheld > 0) {
        struct file *f = current->held[--current->nheld];
        current->held[current->nheld] = NULL;
        file_close(f);
    }
}

/* Clear slot fd if it still holds f (an error path giving back a descriptor
 * it just made); 0 if another thread closed or replaced it meanwhile. */
static int fd_take_back(int fd, struct file *f)
{
    struct proc *g = current->leader;
    int took = 0;
    spin_lock(&g->fd_lock);
    if (fd >= 0 && fd < MAX_FDS && g->fds[fd] == f) {
        g->fds[fd] = NULL;
        fd_cloexec_set(g, fd, 0);
        took = 1;
    }
    spin_unlock(&g->fd_lock);
    return took;
}

/* (The descriptor table is the process's: its threads take and free slots under fd_lock.) */
static int alloc_fd_from(struct file *f, int min_fd, int cloexec)
{
    if (min_fd < 0 || min_fd >= MAX_FDS)
        return -EINVAL;
    struct proc *g = current->leader;
    spin_lock(&g->fd_lock);
    for (int i = min_fd; i < MAX_FDS; i++) {
        if (!g->fds[i]) {
            g->fds[i] = f;
            fd_cloexec_set(g, i, cloexec);
            spin_unlock(&g->fd_lock);
            return i;
        }
    }
    spin_unlock(&g->fd_lock);
    return -EMFILE;
}

static int alloc_fd(struct file *f)
{
    return alloc_fd_from(f, 0, 0);
}

/* ---- processes ---- */

static int sys_exit(uint32_t code, uint32_t b, uint32_t c, uint32_t d, uint32_t e)
{
    (void)b; (void)c; (void)d; (void)e;
    do_exit((int)((code & 0xFF) << 8));
}

static int sys_fork(uint32_t a, uint32_t b, uint32_t c, uint32_t d, uint32_t e)
{
    (void)a; (void)b; (void)c; (void)d; (void)e;
    return do_fork();
}

/* Copy a NULL-terminated user vector of strings into kernel memory. */
static int copy_vector(uint64_t uvec, char **kvec, int max, char **pool, uint32_t *pool_left)
{
    int n = 0;
    if (!uvec) {
        kvec[0] = NULL;
        return 0;
    }
    /* A pointer of the process's: 4 bytes in a 32-bit program, 8 in a 64-bit one. */
    uint32_t word = current->user64 ? 8 : 4;
    for (;; n++) {
        uint64_t slot = uvec + (uint64_t)n * word;
        uint64_t s = 0;
        if (copy_from_user(&s, UPTR(slot), word) < 0)
            return -EFAULT;
        if (!s)
            break;
        if (n >= max)
            return -E2BIG;
        int len = copy_str(*pool, UPTR(s), *pool_left);
        if (len < 0)
            return len == -ENAMETOOLONG ? -E2BIG : len;
        kvec[n] = *pool;
        *pool += len + 1;
        *pool_left -= (uint32_t)len + 1;
    }
    kvec[n] = NULL;
    return n;
}

#define EXEC_MAX_ARGS 4096
#define EXEC_POOL     (256 * 1024)

static int sys_execve(uint64_t upath, uint64_t uargv, uint64_t uenvp, uint32_t d, uint32_t e)
{
    (void)d; (void)e;
    COPY_PATH(path, upath);

    char **argv = kmalloc(sizeof(char *) * (EXEC_MAX_ARGS + 1) * 2);
    char *pool = kmalloc(EXEC_POOL);
    if (!argv || !pool) {
        kfree(argv);
        kfree(pool);
        return -ENOMEM;
    }
    char **envp = argv + EXEC_MAX_ARGS + 1;
    char *cursor = pool;
    uint32_t left = EXEC_POOL;

    int r = copy_vector(uargv, argv, EXEC_MAX_ARGS, &cursor, &left);
    if (r >= 0)
        r = copy_vector(uenvp, envp, EXEC_MAX_ARGS, &cursor, &left);
    if (r >= 0)
        r = proc_exec(current, path, argv, envp);
    if (r >= 0)
        tty_exec_attach(current);
    kfree(argv);
    kfree(pool);
    return r < 0 ? r : 0;
}

static int sys_waitpid(uint32_t pid, uint64_t ustatus, uint32_t options, uint32_t d, uint32_t e)
{
    (void)d; (void)e;
    if (ustatus && user_check_w(UPTR(ustatus), sizeof(int)) < 0)
        return -EFAULT;
    int status = 0;
    int r = do_waitpid((int)pid, &status, (int)options);
    if (r > 0 && ustatus && copy_to_user(UPTR(ustatus), &status, sizeof(status)) < 0)
        return -EFAULT;
    return r;
}

static int sys_getpid(uint32_t a, uint32_t b, uint32_t c, uint32_t d, uint32_t e)
{
    (void)a; (void)b; (void)c; (void)d; (void)e;
    return current->leader->pid;        /* the process's; a thread's own is gettid()'s */
}

static int sys_getppid(uint32_t a, uint32_t b, uint32_t c, uint32_t d, uint32_t e)
{
    (void)a; (void)b; (void)c; (void)d; (void)e;
    return current->leader->ppid;
}

/* ---- threads ---- */

static int sys_thread_create(uint64_t entry, uint64_t stack, uint64_t tls, uint64_t clear_tid, uint32_t e)
{
    (void)e;
    return thread_create(entry, stack, tls, clear_tid);
}

static int sys_thread_exit(uint64_t addr, uint64_t len, uint32_t c, uint32_t d, uint32_t e)
{
    (void)c; (void)d; (void)e;
    thread_exit(addr, len);
}

static int sys_futex(uint64_t addr, uint32_t op, uint32_t val, uint32_t timeout_ms, uint32_t e)
{
    (void)e;
    if (op == FUTEX_WAIT)
        return futex_wait(addr, val, (int)timeout_ms);
    if (op == FUTEX_WAKE)
        return futex_wake(addr, (int)val);
    return -EINVAL;
}

static int sys_set_tls(uint64_t base, uint32_t b, uint32_t c, uint32_t d, uint32_t e)
{
    (void)b; (void)c; (void)d; (void)e;
    if (base && (base < USER_BASE || base >= user_top(current)))
        return -EINVAL;
    uint32_t flags = irq_save();        /* the processor whose segment this is stays this one */
    current->tls = base;
    proc_load_tls(current);
    irq_restore(flags);
    /* The selector to load, into GS; a 64-bit program has its block through FS as it is. */
    return current->user64 ? 0 : SEG_USER_TLS;
}

static int sys_set_win_teb(uint32_t base, uint32_t b, uint32_t c, uint32_t d, uint32_t e)
{
    (void)b;(void)c;(void)d;(void)e;
    if(base && (base<USER_BASE || base>USER_TOP-WIN_TEB_SIZE)) return -EINVAL;
    uint32_t flags=irq_save();
    current->win_teb=base;gdt_set_win_teb(base);
    irq_restore(flags);return SEG_WIN_FS;
}

static int sys_gettid(uint32_t a, uint32_t b, uint32_t c, uint32_t d, uint32_t e)
{
    (void)a; (void)b; (void)c; (void)d; (void)e;
    return current->pid;
}

static int sys_tkill(uint32_t tid, uint32_t sig, uint32_t c, uint32_t d, uint32_t e)
{
    (void)c; (void)d; (void)e;
    return do_tkill((int)tid, (int)sig);
}

/* ---- interval timers, signal stacks, sessions ---- */

static uint32_t ms_to_ticks(uint32_t ms)
{
    uint32_t t = ((uint64_t)ms * HZ + 999) / 1000;
    return ms && !t ? 1 : t;
}

static int sys_setitimer(uint32_t which, uint64_t uvalue, uint64_t uold, uint32_t d, uint32_t e)
{
    (void)d; (void)e;
    struct flux_itimer v = { 0, 0 };
    if (uvalue && copy_from_user(&v, UPTR(uvalue), sizeof(v)) < 0)
        return -EFAULT;
    if (uold && user_check_w(UPTR(uold), sizeof(v)) < 0)
        return -EFAULT;
    uint32_t ov, oi;
    int r = proc_setitimer((int)which, ms_to_ticks(v.value_ms), ms_to_ticks(v.interval_ms), &ov, &oi);
    if (r < 0)
        return r;
    if (uold) {
        struct flux_itimer o = { ov * (1000 / HZ), oi * (1000 / HZ) };
        if (copy_to_user(UPTR(uold), &o, sizeof(o)) < 0)
            return -EFAULT;
    }
    return 0;
}

static int sys_getitimer(uint32_t which, uint64_t uout, uint32_t c, uint32_t d, uint32_t e)
{
    (void)c; (void)d; (void)e;
    if (user_check_w(UPTR(uout), sizeof(struct flux_itimer)) < 0)
        return -EFAULT;
    struct proc *g = current->leader;
    struct flux_itimer o;
    spin_lock(&sched_lock);
    switch (which) {
    case ITIMER_REAL:
        o.value_ms = g->it_real_at ? ((int32_t)(g->it_real_at - ticks) > 0 ? g->it_real_at - ticks : 1) : 0;
        o.interval_ms = g->it_real_interval;
        break;
    case ITIMER_VIRTUAL: o.value_ms = g->it_virt_left, o.interval_ms = g->it_virt_interval; break;
    case ITIMER_PROF: o.value_ms = g->it_prof_left, o.interval_ms = g->it_prof_interval; break;
    default:
        spin_unlock(&sched_lock);
        return -EINVAL;
    }
    spin_unlock(&sched_lock);
    o.value_ms *= 1000 / HZ;
    o.interval_ms *= 1000 / HZ;
    return copy_to_user(UPTR(uout), &o, sizeof(o)) < 0 ? -EFAULT : 0;
}

static int sys_sigpending(uint64_t uset, uint32_t b, uint32_t c, uint32_t d, uint32_t e)
{
    (void)b; (void)c; (void)d; (void)e;
    uint32_t set = __atomic_load_n(&current->sig_pending, __ATOMIC_RELAXED);
    return copy_to_user(UPTR(uset), &set, sizeof(set)) < 0 ? -EFAULT : 0;
}

static int sys_sigaltstack(uint64_t uss, uint64_t uold, uint32_t c, uint32_t d, uint32_t e)
{
    (void)c; (void)d; (void)e;
    struct proc *p = current;
    struct flux_stack ss, old;
    uint64_t sp = p->tf->rsp;
    int on = p->alt_size && sp > p->alt_sp && sp <= p->alt_sp + p->alt_size;
    memset(&old, 0, sizeof(old));
    old.sp = p->alt_sp;
    old.size = p->alt_size;
    old.flags = !p->alt_size ? SS_DISABLE : on ? SS_ONSTACK : 0;
    if (uss) {
        if (p->user64) {
            if (copy_from_user(&ss, UPTR(uss), sizeof(ss)) < 0)
                return -EFAULT;
        } else {
            struct flux_stack32 ss32;
            if (copy_from_user(&ss32, UPTR(uss), sizeof(ss32)) < 0)
                return -EFAULT;
            ss = (struct flux_stack){ .sp = ss32.sp, .flags = ss32.flags, .size = ss32.size };
        }
        if (on)
            return -EPERM;              /* not while running on it */
        if (ss.flags & SS_DISABLE) {
            p->alt_size = 0;
        } else {
            if (ss.flags & ~(uint32_t)SS_ONSTACK)
                return -EINVAL;
            if (ss.size < 2048)
                return -ENOMEM;
            if (ss.sp < USER_BASE || ss.sp >= user_top(p) || ss.size > user_top(p) - ss.sp)
                return -EINVAL;
            p->alt_sp = ss.sp;
            p->alt_size = ss.size;
        }
    }
    if (uold && p->user64) {
        if (copy_to_user(UPTR(uold), &old, sizeof(old)) < 0)
            return -EFAULT;
    } else if (uold) {
        struct flux_stack32 old32 = { (uint32_t)old.sp, old.flags, (uint32_t)old.size };
        if (copy_to_user(UPTR(uold), &old32, sizeof(old32)) < 0)
            return -EFAULT;
    }
    return 0;
}

static int sys_setsid(uint32_t a, uint32_t b, uint32_t c, uint32_t d, uint32_t e)
{
    (void)a; (void)b; (void)c; (void)d; (void)e;
    return do_setsid();
}

static int sys_getsid(uint32_t pid, uint32_t b, uint32_t c, uint32_t d, uint32_t e)
{
    (void)b; (void)c; (void)d; (void)e;
    return do_getsid((int)pid);
}

static int sys_kill(uint32_t pid, uint32_t sig, uint32_t c, uint32_t d, uint32_t e)
{
    (void)c; (void)d; (void)e;
    return do_kill((int)pid, (int)sig);
}

static long sbrk_locked(struct proc *p, int64_t delta);

static long sys_sbrk(uint64_t incr, uint32_t b, uint32_t c, uint32_t d, uint32_t e)
{
    (void)b; (void)c; (void)d; (void)e;
    struct proc *p = current;
    mutex_lock(&p->leader->mm_lock);
    /* (A 32-bit program's increment is a 32-bit number, negative to shrink.) */
    long r = sbrk_locked(p, p->user64 ? (int64_t)incr : (int32_t)incr);
    mutex_unlock(&p->leader->mm_lock);
    return r;
}

/* The heap's end moved by delta bytes: the old end, which a 64-bit program
 * gets whole and a 32-bit one as the 32 bits it is. */
static long sbrk_locked(struct proc *p, int64_t delta)
{
    uint64_t old = p->leader->brk;
    uint64_t nbrk = old + (uint64_t)delta;

    if (delta > 0) {
        if (nbrk < old || nbrk > proc_vma_lowest(p))
            return -ENOMEM;
        for (uint64_t va = ALIGN_UP(old, PAGE_SIZE); va < nbrk; va += PAGE_SIZE) {
            if (vmm_alloc(p->pd, va) < 0) {
                vmm_free_range(p->pd, ALIGN_UP(old, PAGE_SIZE), va);
                return -ENOMEM;
            }
        }
    } else if (delta < 0) {
        if (nbrk < p->leader->heap_start || nbrk > old)
            return -EINVAL;
        /* Pages lent to the GPU stay until they are taken back (DISP_UNMAPBUF). */
        if (fb_pinned(p->leader->pid, ALIGN_UP(nbrk, PAGE_SIZE), ALIGN_UP(old, PAGE_SIZE)))
            return -EBUSY;
        vmm_free_range(p->pd, ALIGN_UP(nbrk, PAGE_SIZE), ALIGN_UP(old, PAGE_SIZE));
    }
    p->leader->brk = nbrk;
    return (long)old;
}

static int sys_sleep(uint32_t ms, uint32_t b, uint32_t c, uint32_t d, uint32_t e)
{
    (void)b; (void)c; (void)d; (void)e;
    uint32_t t = (ms + (1000 / HZ) - 1) / (1000 / HZ);
    return sleep_ticks_intr(t ? t : 1);
}

static int sys_yield(uint32_t a, uint32_t b, uint32_t c, uint32_t d, uint32_t e)
{
    (void)a; (void)b; (void)c; (void)d; (void)e;
    set_resched();
    return 0;
}

static long sys_signal(uint32_t sig, uint64_t disp, uint32_t c, uint32_t d, uint32_t e)
{
    (void)c; (void)d; (void)e;
    if (sig == 0 || sig >= NSIG || sig == SIGKILL || sig == SIGSTOP)
        return -EINVAL;
    uint32_t bit = 1u << sig;
    struct proc *p = current;
    /* (An address the processor would not take must never become a program counter.) */
    if (disp != SIGDISP_DFL && disp != SIGDISP_IGN && (disp < USER_BASE || disp >= user_top(p)))
        return -EINVAL;
    /* Serialize the disposition and pending-bit update with signal_locked(). */
    spin_lock(&sched_lock);
    long old = (long)p->leader->sig_actions[sig].fsa_handler;
    if (p->leader->sig_ignored & bit)
        old = SIGDISP_IGN;
    p->leader->sig_actions[sig].fsa_handler = disp;
    p->leader->sig_actions[sig].fsa_flags = 0;
    p->leader->sig_actions[sig].fsa_mask = 0;
    if (disp == SIGDISP_IGN) {
        __atomic_or_fetch(&p->leader->sig_ignored, bit, __ATOMIC_RELAXED);
        __atomic_and_fetch(&p->sig_pending, ~bit, __ATOMIC_RELAXED);
    } else {
        __atomic_and_fetch(&p->leader->sig_ignored, ~bit, __ATOMIC_RELAXED);
        if (disp == SIGDISP_DFL &&
            (sig == SIGCHLD || sig == SIGURG || sig == SIGWINCH || sig == SIGCONT))
            __atomic_and_fetch(&p->sig_pending, ~bit, __ATOMIC_RELAXED);
    }
    spin_unlock(&sched_lock);
    return old;
}

static int sys_sigaction(uint32_t sig, uint64_t uact, uint64_t uoact, uint32_t d, uint32_t e)
{
    (void)d; (void)e;
    if (sig == 0 || sig >= NSIG)
        return -EINVAL;
    if (uact && (sig == SIGKILL || sig == SIGSTOP))
        return -EINVAL;
    struct proc *p = current;
    struct flux_sigaction act;
    if (uact) {
        if (p->user64) {
            if (copy_from_user(&act, UPTR(uact), sizeof(act)) < 0)
                return -EFAULT;
        } else {
            struct flux_sigaction32 act32;
            if (copy_from_user(&act32, UPTR(uact), sizeof(act32)) < 0)
                return -EFAULT;
            act = (struct flux_sigaction){ act32.fsa_handler, act32.fsa_mask, act32.fsa_flags, act32.fsa_restorer };
        }
        if (act.fsa_handler != SIGDISP_DFL && act.fsa_handler != SIGDISP_IGN &&
            (act.fsa_handler < USER_BASE || act.fsa_handler >= user_top(p)))
            return -EINVAL;
    }
    uint32_t osize = p->user64 ? sizeof(struct flux_sigaction) : sizeof(struct flux_sigaction32);
    if (uoact && user_check_w(UPTR(uoact), osize) < 0)
        return -EFAULT;

    uint32_t bit = 1u << sig;
    spin_lock(&sched_lock);
    struct flux_sigaction old = p->leader->sig_actions[sig];
    if (p->leader->sig_ignored & bit)
        old.fsa_handler = SIGDISP_IGN;
    if (uact) {
        act.fsa_mask &= ~((1u << SIGKILL) | (1u << SIGSTOP));
        p->leader->sig_actions[sig] = act;
        if (act.fsa_handler == SIGDISP_IGN) {
            __atomic_or_fetch(&p->leader->sig_ignored, bit, __ATOMIC_RELAXED);
            __atomic_and_fetch(&p->sig_pending, ~bit, __ATOMIC_RELAXED);
        } else {
            __atomic_and_fetch(&p->leader->sig_ignored, ~bit, __ATOMIC_RELAXED);
            if (act.fsa_handler == SIGDISP_DFL &&
                (sig == SIGCHLD || sig == SIGURG || sig == SIGWINCH || sig == SIGCONT))
                __atomic_and_fetch(&p->sig_pending, ~bit, __ATOMIC_RELAXED);
        }
    }
    spin_unlock(&sched_lock);
    if (uoact && p->user64) {
        if (copy_to_user(UPTR(uoact), &old, sizeof(old)) < 0)
            return -EFAULT;
    } else if (uoact) {
        struct flux_sigaction32 old32 = { (uint32_t)old.fsa_handler, old.fsa_mask, old.fsa_flags,
                                          (uint32_t)old.fsa_restorer };
        if (copy_to_user(UPTR(uoact), &old32, sizeof(old32)) < 0)
            return -EFAULT;
    }
    return 0;
}

static int sys_sigprocmask(uint32_t how, uint64_t uset, uint64_t uoset, uint32_t d, uint32_t e)
{
    (void)d; (void)e;
    uint32_t set = 0;
    if (uset && copy_from_user(&set, UPTR(uset), sizeof(set)) < 0)
        return -EFAULT;
    if (uoset && user_check_w(UPTR(uoset), sizeof(uint32_t)) < 0)
        return -EFAULT;

    struct proc *p = current;
    spin_lock(&sched_lock);
    uint32_t old = p->sig_blocked;
    if (uset) {
        switch (how) {
        case SIG_BLOCK:   p->sig_blocked |= set; break;
        case SIG_UNBLOCK: p->sig_blocked &= ~set; break;
        case SIG_SETMASK: p->sig_blocked = set; break;
        default:
            spin_unlock(&sched_lock);
            return -EINVAL;
        }
        p->sig_blocked &= ~((1u << SIGKILL) | (1u << SIGSTOP) | 1u);
    }
    spin_unlock(&sched_lock);
    if (uoset && copy_to_user(UPTR(uoset), &old, sizeof(old)) < 0)
        return -EFAULT;
    return 0;
}

/* A selector a process may hold in DS, ES, FS or GS (or none). */
static int data_seg_ok(uint32_t sel)
{
    return sel == SEG_UDATA || sel == SEG_WDATA || sel == SEG_WFS || sel == SEG_USER_TLS || sel == 0;
}

/* A 64-bit program's: all its registers, as struct flux_sigcontext64 has them. */
static long sigreturn64(uint64_t uctx)
{
    struct flux_sigcontext64 sc;
    if (copy_from_user(&sc, UPTR(uctx), sizeof(sc)) < 0)
        return -EFAULT;
    /* Its own segments, and addresses the processor will take: one that
     * is not an address at all would fault in the kernel on the way out. */
    if (sc.cs != SEG_UCODE64 || sc.ss != SEG_UDATA)
        return -EFAULT;
    if (!data_seg_ok(sc.ds) || !data_seg_ok(sc.es) || !data_seg_ok(sc.fs) || !data_seg_ok(sc.gs))
        return -EFAULT;
    struct proc *p = current;
    if (sc.rip < USER_BASE || sc.rip >= user_top(p) || sc.rsp < USER_BASE || sc.rsp > user_top(p))
        return -EFAULT;

    struct trapframe *tf = p->tf;
    tf->ds = sc.ds;
    tf->es = sc.es;
    tf->fs = sc.fs;
    tf->gs = sc.gs;
    tf->r15 = sc.r15; tf->r14 = sc.r14; tf->r13 = sc.r13; tf->r12 = sc.r12;
    tf->r11 = sc.r11; tf->r10 = sc.r10; tf->r9 = sc.r9; tf->r8 = sc.r8;
    tf->rdi = sc.rdi; tf->rsi = sc.rsi; tf->rbp = sc.rbp; tf->rbx = sc.rbx;
    tf->rdx = sc.rdx; tf->rcx = sc.rcx; tf->rax = sc.rax;
    tf->rip = sc.rip;
    tf->rflags = (sc.rflags & 0x000CD5u) | (tf->rflags & ~0x000CD5ull) | 0x200u;
    tf->rsp = sc.rsp;

    spin_lock(&sched_lock);
    p->sig_blocked = sc.oldmask & ~((1u << SIGKILL) | (1u << SIGSTOP) | 1u);
    spin_unlock(&sched_lock);

    fpu_restore_from(p, sc.fpu_used, sc.fpu);
    p->in_syscall = 0;
    /* (The dispatcher stores what this returns in RAX: the register's own value.) */
    return (long)tf->rax;
}

static long sys_sigreturn(uint64_t uctx, uint32_t b, uint32_t c, uint32_t d, uint32_t e)
{
    (void)b; (void)c; (void)d; (void)e;
    if (current->user64)
        return sigreturn64(uctx);
    struct flux_sigcontext sc;
    if (copy_from_user(&sc, UPTR(uctx), sizeof(sc)) < 0)
        return -EFAULT;
    /* The segments come back too: the flat ones, or a Win32 program's
     * based ones (gdt.c), whose offsets stay within the 2 GiB. Anything
     * else would fault in the kernel on the way out. */
    int cs_based = sc.cs == SEG_WCODE, ss_based = sc.ss == SEG_WDATA;
    if (!cs_based && sc.cs != SEG_UCODE)
        return -EFAULT;
    if (!ss_based && sc.ss != SEG_UDATA)
        return -EFAULT;
    if (!data_seg_ok(sc.ds) || !data_seg_ok(sc.es) || !data_seg_ok(sc.fs) || !data_seg_ok(sc.gs))
        return -EFAULT;
    if (cs_based ? sc.eip >= WIN_SEG_LIMIT : (sc.eip < USER_BASE || sc.eip >= USER_TOP))
        return -EFAULT;
    if (ss_based ? sc.useresp > WIN_SEG_LIMIT : (sc.useresp < USER_BASE || sc.useresp > USER_TOP))
        return -EFAULT;

    struct proc *p = current;
    struct trapframe *tf = p->tf;
    tf->cs = sc.cs;
    tf->ss = sc.ss;
    tf->ds = sc.ds;
    tf->es = sc.es;
    tf->fs = sc.fs;
    tf->gs = sc.gs;
    tf->edi = sc.edi;
    tf->esi = sc.esi;
    tf->ebp = sc.ebp;
    tf->ebx = sc.ebx;
    tf->edx = sc.edx;
    tf->ecx = sc.ecx;
    tf->eax = sc.eax;
    tf->eip = sc.eip;
    tf->eflags = (sc.eflags & 0x000CD5u) | (tf->eflags & ~0x000CD5u) | 0x200u;
    tf->useresp = sc.useresp;

    spin_lock(&sched_lock);
    p->sig_blocked = sc.oldmask & ~((1u << SIGKILL) | (1u << SIGSTOP) | 1u);
    spin_unlock(&sched_lock);

    fpu_restore_from(p, sc.fpu_used, sc.fpu);
    p->in_syscall = 0;
    return (int)tf->eax;
}

static long sys_mmap(uint64_t uargs, uint32_t b, uint32_t c, uint32_t d, uint32_t e)
{
    (void)b; (void)c; (void)d; (void)e;
    struct flux_mmap_args a;
    if (current->user64) {
        if (copy_from_user(&a, UPTR(uargs), sizeof(a)) < 0)
            return -EFAULT;
    } else {
        struct flux_mmap_args32 a32;
        if (copy_from_user(&a32, UPTR(uargs), sizeof(a32)) < 0)
            return -EFAULT;
        a = (struct flux_mmap_args){ a32.addr, a32.len, a32.prot, a32.flags, a32.fd, a32.offset };
    }
    if (!a.len || (a.flags & MAP_KFRAMES))
        return -EINVAL;

    struct file *f = NULL;
    if (!(a.flags & MAP_ANONYMOUS)) {
        f = get_file((uint32_t)a.fd);
        if (!f || f->type != FT_VNODE || f->vn->type != VN_FILE)
            return -EBADF;
        if ((f->flags & O_ACCMODE) == O_WRONLY)
            return -EACCES;
        if (a.flags & MAP_SHARED)
            return -ENOTSUP;
        if (a.offset & (PAGE_SIZE - 1))
            return -EINVAL;
        /* The file positions read below, offset + up to len rounded up,
         * are positions: not past what an off_t holds. */
        if (a.len > USER_LIMIT || a.offset > (uint64_t)INT64_MAX - ALIGN_UP(a.len, PAGE_SIZE))
            return -EOVERFLOW;
    }

    uint64_t base = 0;
    int r = proc_mmap(current, &a, &base);
    if (r < 0)
        return r;
    if (!f)
        return (long)base;

    uint64_t len = ALIGN_UP(a.len, PAGE_SIZE);
    /* Read through a kernel page: another thread may unmap the new pages
     * meanwhile, which only makes the copy fail. */
    uint8_t *page = kmalloc(PAGE_SIZE);
    if (!page) {
        proc_munmap(current, base, len);
        return -ENOMEM;
    }
    vfs_lock();
    for (uint64_t off = 0; off < len; off += PAGE_SIZE) {
        uint64_t foff = a.offset + off;
        if (foff < f->vn->size) {
            uint32_t n = (uint32_t)MIN((uint64_t)PAGE_SIZE, f->vn->size - foff);
            int nr = vfs_read(f->vn, page, foff, n);
            if (nr > 0 && copy_to_user(UPTR(base + off), page, (uint32_t)nr) < 0)
                nr = -EFAULT;
            if (nr < 0) {
                vfs_unlock();
                kfree(page);
                proc_munmap(current, base, len);
                return nr;
            }
        }
        if (!(a.prot & PROT_WRITE))
            vmm_protect_page(current->pd, base + off, a.prot);
    }
    vfs_unlock();
    kfree(page);
    return (long)base;
}

static int sys_munmap(uint64_t addr, uint64_t len, uint32_t c, uint32_t d, uint32_t e)
{
    (void)c; (void)d; (void)e;
    return proc_munmap(current, addr, len);
}

static int sys_mprotect(uint64_t addr, uint64_t len, uint32_t prot, uint32_t d, uint32_t e)
{
    (void)d; (void)e;
    return proc_mprotect(current, addr, len, prot);
}

static int sys_setpgid(uint32_t pid, uint32_t pgid, uint32_t c, uint32_t d, uint32_t e)
{
    (void)c; (void)d; (void)e;
    return do_setpgid((int)pid, (int)pgid);
}

static int sys_getpgid(uint32_t pid, uint32_t b, uint32_t c, uint32_t d, uint32_t e)
{
    (void)b; (void)c; (void)d; (void)e;
    return do_getpgid((int)pid);
}

static int sys_tcsetpgrp(uint32_t pgid, uint32_t b, uint32_t c, uint32_t d, uint32_t e)
{
    (void)b; (void)c; (void)d; (void)e;
    tty_set_fg(tty_current(), (int)pgid);
    return 0;
}

static int sys_tcgetpgrp(uint32_t a, uint32_t b, uint32_t c, uint32_t d, uint32_t e)
{
    (void)a; (void)b; (void)c; (void)d; (void)e;
    return tty_get_fg(tty_current());
}

static int sys_ttymode(uint32_t mode, uint32_t b, uint32_t c, uint32_t d, uint32_t e)
{
    (void)b; (void)c; (void)d; (void)e;
    return tty_set_mode(tty_current(), (int)mode);
}

/* ---- files ---- */

/*
 * File I/O goes through a kernel buffer, a chunk at a time: filesystems,
 * drivers, terminals, pipes and sockets never see the caller's memory,
 * which another thread may unmap while they sleep, or which they would
 * otherwise copy holding a spinlock. The caller's side is copied with
 * nothing held. I/O that may reach a disk runs under the VFS lock, a
 * chunk at a time so other processes get their turn during a long
 * transfer. Terminals and pipes block for unbounded time and never take
 * the lock.
 *
 * A 64-bit program's count is 64 bits and so is what comes back: one call
 * moves as much of a file as there is memory to hold. A 32-bit program's
 * result is an int, so no more than IO_MAX32 goes at once there.
 */
#define IO_MAX32 0x7FFFF000u
#define IO_CHUNK (64 * 1024)

static int needs_vfs_lock(struct file *f)
{
    return f->type == FT_VNODE && f->vn->type != VN_CHR;
}

/* The kernel buffer for a transfer of n bytes (a chunk of it at a time). */
static uint8_t *bounce_alloc(uint64_t n)
{
    return kmalloc(n ? MIN(n, (uint64_t)IO_CHUNK) : 1);
}

static long file_io(struct file *f, uint8_t *ubuf, uint64_t n, int write)
{
    if (!current->user64)
        n = MIN(n, (uint64_t)IO_MAX32);
    uint8_t *kbuf = bounce_alloc(n);
    if (!kbuf)
        return -ENOMEM;
    int locked = needs_vfs_lock(f);
    uint64_t done = 0;
    int r;
    do {                                /* once at least: a 0-byte transfer still checks f */
        uint32_t chunk = (uint32_t)MIN(n - done, (uint64_t)IO_CHUNK);
        if (write && copy_from_user(kbuf, ubuf + done, chunk) < 0) {
            r = -EFAULT;
            break;
        }
        if (locked)
            vfs_lock();
        /* A terminal's device calls do not see the file's flags: O_NONBLOCK goes its own way. */
        r = -ENOTTY;
        if ((f->flags & O_NONBLOCK) && f->type == FT_VNODE && f->vn->type == VN_CHR &&
            (f->flags & O_ACCMODE) != (write ? O_RDONLY : O_WRONLY))
            r = tty_io_nonblock(f->vn, (char *)kbuf, chunk, write);
        if (r == -ENOTTY)
            r = write ? file_write(f, kbuf, chunk) : file_read(f, kbuf, chunk);
        if (locked)
            vfs_unlock();
        if (r < 0)
            break;
        if (!write && r > 0 && copy_to_user(ubuf + done, kbuf, (uint32_t)r) < 0) {
            r = -EFAULT;                /* (what was read is lost, as it would be on Linux) */
            break;
        }
        done += (uint32_t)r;
        if ((uint32_t)r < chunk)
            break;
        /* A terminal, pipe, socket or device hands over what it has:
         * another chunk could wait for more, with this one's in hand. */
        if (!write && !locked)
            break;
    } while (done < n);
    kfree(kbuf);
    return done ? (long)done : r;
}

static long sys_read(uint32_t fd, uint64_t buf, uint64_t n, uint32_t d, uint32_t e)
{
    (void)d; (void)e;
    struct file *f = get_file(fd);
    if (!f)
        return -EBADF;
    if (user_check_w(UPTR(buf), n) < 0)
        return -EFAULT;
    return file_io(f, (uint8_t *)UPTR(buf), n, 0);
}

static long sys_write(uint32_t fd, uint64_t buf, uint64_t n, uint32_t d, uint32_t e)
{
    (void)d; (void)e;
    struct file *f = get_file(fd);
    if (!f)
        return -EBADF;
    if (user_check(UPTR(buf), n) < 0)
        return -EFAULT;
    return file_io(f, (uint8_t *)UPTR(buf), n, 1);
}

static int sys_open(uint64_t upath, uint32_t flags, uint32_t mode, uint32_t d, uint32_t e)
{
    (void)d; (void)e;
    COPY_PATH(path, upath);
    struct vnode *vn;
    int r = vfs_resolve(path, current->leader->cwd, &vn);

    if (r == -ENOENT && (flags & O_CREAT)) {
        /* (through a dangling symbolic link: made where it points) */
        r = vfs_create_file(path, current->leader->cwd, (flags & O_EXCL) != 0, &vn);
        if (r == 0 && vn) {
            uint32_t m = mode ? (mode & 07777) : 0666;
            vn->mode = (m & ~(current->leader->umask & 0777)) | (m & 07000);
            vfs_setattr(vn);            /* kept where the filesystem keeps modes (ext2) */
        }
    } else if (r == 0 && (flags & O_CREAT) && (flags & O_EXCL)) {
        return -EEXIST;
    }
    if (r < 0)
        return r;

    int writable = (flags & O_ACCMODE) != O_RDONLY;
    if ((flags & O_DIRECTORY) && vn->type != VN_DIR)
        return -ENOTDIR;
    if (vn->type == VN_DIR && writable)
        return -EISDIR;
    if ((flags & O_TRUNC) && writable && vn->type == VN_FILE) {
        r = vfs_truncate(vn, 0);
        if (r < 0)
            return r;
    }

    int cloned = vn->type == VN_CHR && vn->dev_clone;
    if (cloned && (r = vn->dev_clone(vn, (int)flags, &vn)) < 0)
        return r;
    if (vn->type == VN_CHR && vn->dev_open && (r = vn->dev_open(vn, (int)flags)) < 0) {
        if (cloned)
            vfs_unref(vn);              /* the clone goes too */
        return r;
    }
    struct file *f = file_open(vn, (int)(flags & ~O_CLOEXEC));
    if (!f) {
        if (vn->type == VN_CHR && vn->dev_close)
            vn->dev_close(vn, (int)flags);
        if (cloned)
            vfs_unref(vn);              /* nothing else refers to it: it goes */
        return -ENOMEM;
    }
    int fd = alloc_fd_from(f, 0, (flags & O_CLOEXEC) != 0);
    if (fd < 0)
        file_close(f);
    return fd;
}

static int sys_close(uint32_t fd, uint32_t b, uint32_t c, uint32_t d, uint32_t e)
{
    (void)b; (void)c; (void)d; (void)e;
    struct proc *g = current->leader;
    if (fd >= MAX_FDS)
        return -EBADF;
    spin_lock(&g->fd_lock);
    struct file *f = g->fds[fd];
    g->fds[fd] = NULL;
    if (f)
        fd_cloexec_set(g, (int)fd, 0);
    spin_unlock(&g->fd_lock);
    if (!f)
        return -EBADF;
    file_close(f);
    return 0;
}

static int sys_dup(uint32_t fd, uint32_t b, uint32_t c, uint32_t d, uint32_t e)
{
    (void)b; (void)c; (void)d; (void)e;
    struct file *f = get_file(fd);
    if (!f)
        return -EBADF;
    file_dup(f);                        /* the new slot's, before another thread can close it */
    int nfd = alloc_fd(f);
    if (nfd < 0)
        file_close(f);
    return nfd;
}

static int sys_dup2(uint32_t oldfd, uint32_t newfd, uint32_t c, uint32_t d, uint32_t e)
{
    (void)c; (void)d; (void)e;
    struct file *f = get_file(oldfd);
    if (!f || newfd >= MAX_FDS)
        return -EBADF;
    if (oldfd == newfd)
        return (int)newfd;
    struct proc *g = current->leader;
    struct file *nf = file_dup(f);
    spin_lock(&g->fd_lock);
    struct file *prev = g->fds[newfd];
    g->fds[newfd] = nf;
    fd_cloexec_set(g, (int)newfd, 0);
    spin_unlock(&g->fd_lock);
    if (prev)
        file_close(prev);
    return (int)newfd;
}

static int sys_fcntl(uint32_t fd, uint32_t cmd, uint64_t arg, uint32_t d, uint32_t e)
{
    (void)d; (void)e;
    struct file *f = get_file(fd);
    if (!f)
        return -EBADF;
    switch (cmd) {
    case F_DUPFD:
    case F_DUPFD_CLOEXEC: {
        file_dup(f);                    /* the new slot's, before another thread can close it */
        int nfd = alloc_fd_from(f, (int)arg, cmd == F_DUPFD_CLOEXEC);
        if (nfd < 0)
            file_close(f);
        return nfd;
    }
    case F_GETFD:
        return fd_cloexec_get(current, (int)fd) ? FD_CLOEXEC : 0;
    case F_SETFD: {
        struct proc *g = current->leader;
        spin_lock(&g->fd_lock);         /* the bits share words with other descriptors' */
        fd_cloexec_set(g, (int)fd, (arg & FD_CLOEXEC) != 0);
        spin_unlock(&g->fd_lock);
        return 0;
    }
    case F_GETFL:
        return f->flags & ~O_CLOEXEC;
    case F_SETFL:
        f->flags = (f->flags & ~(O_APPEND | O_NONBLOCK)) | ((int)arg & (O_APPEND | O_NONBLOCK));
        return 0;
    default:
        return -EINVAL;
    }
}

static int sys_pipe(uint64_t ufds, uint32_t b, uint32_t c, uint32_t d, uint32_t e)
{
    (void)b; (void)c; (void)d; (void)e;
    if (user_check_w(UPTR(ufds), 2 * sizeof(int)) < 0)
        return -EFAULT;
    struct file *rd, *wr;
    int r = pipe_create(&rd, &wr);
    if (r < 0)
        return r;
    int fd0 = alloc_fd(rd);
    int fd1 = fd0 >= 0 ? alloc_fd(wr) : -EMFILE;
    int fds[2] = { fd0, fd1 };
    int r2 = fd1 < 0 ? -EMFILE : copy_to_user(UPTR(ufds), fds, sizeof(fds));
    if (r2 < 0) {
        /* A slot another thread already closed gave its file back itself. */
        if (fd0 < 0 || fd_take_back(fd0, rd))
            file_close(rd);
        if (fd1 < 0 || fd_take_back(fd1, wr))
            file_close(wr);
        return r2;
    }
    return 0;
}

static int sys_openpty(uint64_t ufds, uint32_t b, uint32_t c, uint32_t d, uint32_t e)
{
    (void)b; (void)c; (void)d; (void)e;
    if (user_check_w(UPTR(ufds), 2 * sizeof(int)) < 0)
        return -EFAULT;
    struct file *master, *slave;
    int r = pty_create(&master, &slave);
    if (r < 0)
        return r;
    int fd0 = alloc_fd(master);
    int fd1 = fd0 >= 0 ? alloc_fd(slave) : -EMFILE;
    int fds[2] = { fd0, fd1 };
    int r2 = fd1 < 0 ? -EMFILE : copy_to_user(UPTR(ufds), fds, sizeof(fds));
    if (r2 < 0) {
        /* A slot another thread already closed gave its file back itself. */
        if (fd0 < 0 || fd_take_back(fd0, master))
            file_close(master);
        if (fd1 < 0 || fd_take_back(fd1, slave))
            file_close(slave);
        return r2;
    }
    return 0;
}

#define POLL_MAX 64

static int sys_poll(uint64_t ufds, uint32_t nfds, uint32_t timeout, uint32_t d, uint32_t e)
{
    (void)d; (void)e;
    if (nfds > POLL_MAX)
        return -EINVAL;
    /* The kernel's copy of the table: the caller's memory is checked up
     * front, so that the results can be written back after the wait. */
    struct pollfd fds[POLL_MAX];
    uint32_t bytes = nfds * sizeof(fds[0]);
    if (user_check_w(UPTR(ufds), bytes) < 0 ||
        (bytes && copy_from_user(fds, UPTR(ufds), bytes) < 0))
        return -EFAULT;
    int r = do_poll(fds, nfds, (int)timeout);
    if (bytes && copy_to_user(UPTR(ufds), fds, bytes) < 0)
        return -EFAULT;
    return r;
}

/*
 * The new position of f, moved by the signed offset off. Positions are
 * 64-bit: a file's, and a disk's device node's.
 */
static int seek(struct file *f, int64_t off, uint32_t whence, uint64_t *out)
{
    if (f->type == FT_PIPE || f->type == FT_SOCKET || f->type == FT_UNIX)
        return -ESPIPE;                 /* (an AF_UNIX file has no vnode) */
    if (f->vn->type == VN_CHR) {
        *out = 0;
        return 1;                       /* terminals and the like: nowhere to go */
    }
    int64_t base;
    switch (whence) {
    case SEEK_SET: base = 0; break;
    case SEEK_CUR: base = (int64_t)f->off; break;
    case SEEK_END: {
        struct blockdev *bd = blk_by_vnode(f->vn);
        base = bd ? (int64_t)blk_bytes(bd) : (int64_t)f->vn->size;
        break;
    }
    default: return -EINVAL;
    }
    if ((off > 0 && base > INT64_MAX - off) || base + off < 0)
        return -EINVAL;
    *out = (uint64_t)(base + off);
    return 0;
}

/* A 64-bit program's offset and result are whole. A 32-bit program's are
 * 32 bits: a position past 2 GiB cannot be its result (SYS_llseek's can). */
static long sys_lseek(uint32_t fd, uint64_t off, uint32_t whence, uint32_t d, uint32_t e)
{
    (void)d; (void)e;
    struct file *f = get_file(fd);
    if (!f)
        return -EBADF;
    int wide = current->user64;
    uint64_t pos;
    int r = seek(f, wide ? (int64_t)off : (int32_t)off, whence, &pos);
    if (r < 0)
        return r;
    if (r == 0) {
        if (!wide && pos > INT32_MAX)
            return -EOVERFLOW;
        f->off = pos;
    }
    return (long)pos;
}

/* lseek with a 64-bit offset (high, low) and result (written to *uresult):
 * what a 32-bit program has for it. */
static int sys_llseek(uint32_t fd, uint32_t hi, uint32_t lo, uint64_t uresult, uint32_t whence)
{
    struct file *f = get_file(fd);
    if (!f)
        return -EBADF;
    if (user_check_w(UPTR(uresult), sizeof(uint64_t)) < 0)
        return -EFAULT;
    uint64_t pos;
    int r = seek(f, (int64_t)((uint64_t)hi << 32 | lo), whence, &pos);
    if (r < 0)
        return r;
    if (r == 0)
        f->off = pos;
    return copy_to_user(UPTR(uresult), &pos, sizeof(pos));
}

/* struct stat as the calling program has it. */
static uint32_t stat_bytes(void)
{
    return current->user64 ? sizeof(struct stat) : sizeof(struct stat32);
}

/* A 32-bit program's size is 32 bits, signed: a file of 2 GiB or more
 * cannot be told of (EOVERFLOW, as POSIX has it), and of a disk, whose size
 * such a program asks for another way, as much as fits is. */
static int put_stat(uint64_t ust, const struct stat *st)
{
    if (current->user64)
        return copy_to_user(UPTR(ust), st, sizeof(*st));
    uint64_t size = (uint64_t)st->st_size;
    if (size > INT32_MAX) {
        if (!S_ISBLK(st->st_mode))
            return -EOVERFLOW;
        size = INT32_MAX;
    }
    struct stat32 s = {
        .st_dev = st->st_dev, .st_ino = st->st_ino, .st_mode = st->st_mode, .st_nlink = st->st_nlink,
        .st_uid = st->st_uid, .st_gid = st->st_gid, .st_rdev = st->st_rdev, .st_size = (int32_t)size,
        .st_blksize = st->st_blksize, .st_blocks = (uint32_t)MIN(st->st_blocks, (flux_blkcnt_t)UINT32_MAX),
        .st_atime = st->st_atime, .st_mtime = st->st_mtime, .st_ctime = st->st_ctime,
    };
    return copy_to_user(UPTR(ust), &s, sizeof(s));
}

static int stat_path(uint64_t upath, uint64_t ust, int follow)
{
    COPY_PATH(path, upath);
    if (user_check_w(UPTR(ust), stat_bytes()) < 0)
        return -EFAULT;
    struct vnode *vn;
    int r = vfs_lookup(path, current->leader->cwd, follow, &vn);
    if (r < 0)
        return r;
    struct stat st;
    memset(&st, 0, sizeof(st));
    vfs_stat(vn, &st);
    return put_stat(ust, &st);
}

static int sys_stat(uint64_t upath, uint64_t ust, uint32_t c, uint32_t d, uint32_t e)
{
    (void)c; (void)d; (void)e;
    return stat_path(upath, ust, 1);
}

static int sys_lstat(uint64_t upath, uint64_t ust, uint32_t c, uint32_t d, uint32_t e)
{
    (void)c; (void)d; (void)e;
    return stat_path(upath, ust, 0);
}

static int sys_readlink(uint64_t upath, uint64_t ubuf, uint32_t size, uint32_t d, uint32_t e)
{
    (void)d; (void)e;
    COPY_PATH(path, upath);
    if (!size)
        return -EINVAL;
    size = MIN(size, (uint32_t)PATH_MAX);
    if (user_check_w(UPTR(ubuf), size) < 0)
        return -EFAULT;
    char *tmp = kmalloc(size);
    if (!tmp)
        return -ENOMEM;
    int r = vfs_readlink(path, current->leader->cwd, tmp, size);
    if (r > 0 && copy_to_user(UPTR(ubuf), tmp, (uint32_t)r) < 0)
        r = -EFAULT;
    kfree(tmp);
    return r;
}

static int sys_symlink(uint64_t utarget, uint64_t upath, uint32_t c, uint32_t d, uint32_t e)
{
    (void)c; (void)d; (void)e;
    COPY_PATH(target, utarget);
    COPY_PATH(path, upath);
    return vfs_symlink(target, path, current->leader->cwd);
}

static int sys_link(uint64_t ufrom, uint64_t uto, uint32_t c, uint32_t d, uint32_t e)
{
    (void)c; (void)d; (void)e;
    COPY_PATH(from, ufrom);
    COPY_PATH(to, uto);
    return vfs_link(from, to, current->leader->cwd);
}

/* An owner of -1 is kept. One user, so anyone may give a file away. */
static int set_owner(struct vnode *vn, uint32_t uid, uint32_t gid)
{
    if (vn->fs && vn->fs->readonly)
        return -EROFS;
    if (uid != UINT32_MAX)
        vn->uid = uid;
    if (gid != UINT32_MAX)
        vn->gid = gid;
    return vfs_setattr(vn);
}

static int sys_chown(uint64_t upath, uint32_t uid, uint32_t gid, uint32_t follow, uint32_t e)
{
    (void)e;
    COPY_PATH(path, upath);
    struct vnode *vn;
    int r = vfs_lookup(path, current->leader->cwd, follow != 0, &vn);
    return r < 0 ? r : set_owner(vn, uid, gid);
}

static int sys_fchown(uint32_t fd, uint32_t uid, uint32_t gid, uint32_t d, uint32_t e)
{
    (void)d; (void)e;
    struct file *f = get_file(fd);
    if (!f)
        return -EBADF;
    if (!f->vn)
        return -EINVAL;
    return set_owner(f->vn, uid, gid);
}

static int sys_fstat(uint32_t fd, uint64_t ust, uint32_t c, uint32_t d, uint32_t e)
{
    (void)c; (void)d; (void)e;
    struct file *f = get_file(fd);
    if (!f)
        return -EBADF;
    if (user_check_w(UPTR(ust), stat_bytes()) < 0)
        return -EFAULT;
    struct stat st;
    memset(&st, 0, sizeof(st));
    if (f->type == FT_SOCKET || f->type == FT_UNIX) {    /* (an AF_UNIX file has no vnode) */
        st.st_mode = S_IFSOCK | 0777;
        st.st_nlink = 1;
    } else if (f->type == FT_PIPE) {
        st.st_mode = S_IFIFO | 0600;
        st.st_size = f->pipe->wpos - f->pipe->rpos;
        st.st_nlink = 1;
    } else {
        vfs_stat(f->vn, &st);
    }
    return put_stat(ust, &st);
}

static int sys_chdir(uint64_t upath, uint32_t b, uint32_t c, uint32_t d, uint32_t e)
{
    (void)b; (void)c; (void)d; (void)e;
    COPY_PATH(path, upath);
    struct vnode *vn;
    int r = vfs_resolve(path, current->leader->cwd, &vn);
    if (r < 0)
        return r;
    if (vn->type != VN_DIR)
        return -ENOTDIR;
    vfs_ref(vn);
    vfs_unref(current->leader->cwd);
    current->leader->cwd = vn;
    return 0;
}

static int sys_fchdir(uint32_t fd, uint32_t b, uint32_t c, uint32_t d, uint32_t e)
{
    (void)b; (void)c; (void)d; (void)e;
    struct file *f = get_file(fd);
    if (!f)
        return -EBADF;
    struct vnode *vn = f->vn;
    if (!vn || vn->type != VN_DIR)
        return -ENOTDIR;
    vfs_ref(vn);
    vfs_unref(current->leader->cwd);
    current->leader->cwd = vn;
    return 0;
}

static int sys_getcwd(uint64_t ubuf, uint32_t size, uint32_t c, uint32_t d, uint32_t e)
{
    (void)c; (void)d; (void)e;
    char *path;
    int r = path_buf(&path);                    /* (a buffer of the thread's, for the answer) */
    if (r < 0)
        return r;
    r = vfs_getpath(current->leader->cwd, path, PATH_MAX);
    if (r < 0)
        return r;
    uint32_t len = strlen(path) + 1;
    if (len > size)
        return -ERANGE;
    if (copy_to_user(UPTR(ubuf), path, len) < 0)
        return -EFAULT;
    return (int)len;
}

static int sys_getdents(uint32_t fd, uint64_t ubuf, uint32_t count, uint32_t d, uint32_t e)
{
    (void)d; (void)e;
    struct file *f = get_file(fd);
    if (!f)
        return -EBADF;
    if (f->type != FT_VNODE || f->vn->type != VN_DIR)
        return -ENOTDIR;
    if (user_check_w(UPTR(ubuf), count) < 0)
        return -EFAULT;
    uint32_t used = 0;
    struct dirent de;
    while (count - used >= sizeof(de) && f->off < UINT32_MAX) {
        /* Go on after the name returned last, unless lseek moved the offset since. */
        const char *after = f->dir_after && f->dir_off == f->off ? f->dir_after : NULL;
        int r = vfs_dirent(f->vn, (uint32_t)f->off, after, &de);
        if (r <= 0)
            return used ? (int)used : r;
        if (copy_to_user((uint8_t *)UPTR(ubuf) + used, &de, sizeof(de)) < 0)
            return used ? (int)used : -EFAULT;
        used += sizeof(de);
        if (f->off++ >= 2 && (f->dir_after || (f->dir_after = kmalloc(NAME_MAX + 1)))) {
            strlcpy(f->dir_after, de.d_name, NAME_MAX + 1);
            f->dir_off = f->off;
        }
    }
    return (int)used;
}

static int sys_mkdir(uint64_t upath, uint32_t mode, uint32_t c, uint32_t d, uint32_t e)
{
    (void)c; (void)d; (void)e;
    COPY_PATH(path, upath);
    int r = vfs_mkdir(path, current->leader->cwd);
    if (r == 0) {
        struct vnode *vn;
        if (vfs_resolve(path, current->leader->cwd, &vn) == 0 && vn) {
            uint32_t m = mode ? (mode & 07777) : 0777;
            vn->mode = (m & ~(current->leader->umask & 0777)) | (m & 07000);
            vfs_setattr(vn);            /* kept where the filesystem keeps modes (ext2) */
        }
    }
    return r;
}

static int sys_rmdir(uint64_t upath, uint32_t b, uint32_t c, uint32_t d, uint32_t e)
{
    (void)b; (void)c; (void)d; (void)e;
    COPY_PATH(path, upath);
    return vfs_rmdir(path, current->leader->cwd);
}

static int sys_unlink(uint64_t upath, uint32_t b, uint32_t c, uint32_t d, uint32_t e)
{
    (void)b; (void)c; (void)d; (void)e;
    COPY_PATH(path, upath);
    return vfs_unlink(path, current->leader->cwd);
}

static int sys_rename(uint64_t ufrom, uint64_t uto, uint32_t c, uint32_t d, uint32_t e)
{
    (void)c; (void)d; (void)e;
    COPY_PATH(from, ufrom);
    COPY_PATH(to, uto);
    return vfs_rename(from, to, current->leader->cwd);
}

static int sys_chmod(uint64_t upath, uint32_t mode, uint32_t c, uint32_t d, uint32_t e)
{
    (void)c; (void)d; (void)e;
    COPY_PATH(path, upath);
    struct vnode *vn;
    int r = vfs_resolve(path, current->leader->cwd, &vn);
    if (r < 0)
        return r;
    if (vn->fs && vn->fs->readonly)
        return -EROFS;
    vn->mode = mode & 07777;
    return vfs_setattr(vn);
}

static int sys_fchmod(uint32_t fd, uint32_t mode, uint32_t c, uint32_t d, uint32_t e)
{
    (void)c; (void)d; (void)e;
    struct file *f = get_file(fd);
    if (!f)
        return -EBADF;
    struct vnode *vn = f->vn;
    if (!vn)
        return -EINVAL;                 /* pipes and sockets have no mode */
    if (vn->fs && vn->fs->readonly)
        return -EROFS;
    vn->mode = mode & 07777;
    return vfs_setattr(vn);
}

static int sys_umask(uint32_t mask, uint32_t b, uint32_t c, uint32_t d, uint32_t e)
{
    (void)b; (void)c; (void)d; (void)e;
    uint32_t old = current->leader->umask;
    current->leader->umask = mask & 0777;
    return (int)old;
}

static int sys_utime(uint64_t upath, uint64_t utimes, uint32_t c, uint32_t d, uint32_t e)
{
    (void)c; (void)d; (void)e;
    COPY_PATH(path, upath);
    uint32_t mtime = time_now();
    if (utimes) {
        struct flux_utimbuf tb;
        if (copy_from_user(&tb, UPTR(utimes), sizeof(tb)) < 0)
            return -EFAULT;
        mtime = tb.modtime;
    }
    return vfs_utime(path, current->leader->cwd, mtime);
}

static int sys_times(uint64_t ubuf, uint32_t b, uint32_t c, uint32_t d, uint32_t e)
{
    (void)b; (void)c; (void)d; (void)e;
    if (ubuf) {
        struct flux_tms tms = {
            .tms_utime = proc_cpu_ticks(current),       /* the process's, every thread's */
            .tms_stime = 0,
            .tms_cutime = current->leader->child_ticks, /* waitpid() keeps it on the leader */
            .tms_cstime = 0,
        };
        if (copy_to_user(UPTR(ubuf), &tms, sizeof(tms)) < 0)
            return -EFAULT;
    }
    return (int)ticks;
}

/* ---- sockets ---- */

static int get_socket_file(uint32_t fd, struct socket **out, struct file **fout)
{
    struct file *f = get_file(fd);
    if (!f)
        return -EBADF;
    if (f->type != FT_SOCKET)
        return -ENOTSOCK;
    *out = f->sock;
    if (fout)
        *fout = f;
    return 0;
}

static int get_socket(uint32_t fd, struct socket **out)
{
    return get_socket_file(fd, out, NULL);
}

/* Socket addresses are always a struct sockaddr_in; len is only checked. */
static int copy_sockaddr(uint64_t uaddr, uint32_t len, struct sockaddr_in *out)
{
    if (len < sizeof(*out))
        return -EINVAL;
    return copy_from_user(out, UPTR(uaddr), sizeof(*out)) < 0 ? -EFAULT : 0;
}

static int install_socket(struct socket *s)
{
    struct file *f = file_socket(s);
    if (!f) {
        sock_close(s);
        return -ENOMEM;
    }
    int fd = alloc_fd(f);
    if (fd < 0)
        file_close(f);
    return fd;
}

/* An AF_UNIX descriptor's file, or NULL (an AF_INET socket or not a socket at all). */
static struct file *get_unix(uint32_t fd)
{
    struct file *f = get_file(fd);
    return f && f->type == FT_UNIX ? f : NULL;
}

/* A struct sockaddr_un's path, NUL-terminated (Linux allows sun_path to fill the field). */
static int copy_unix_path(uint64_t uaddr, uint32_t len, char *path)
{
    if (len <= 2)
        return -EINVAL;
    uint32_t n = MIN(len - 2, (uint32_t)UNIX_PATH_MAX - 1);
    uint16_t family;
    if (copy_from_user(&family, UPTR(uaddr), 2) < 0 ||
        copy_from_user(path, (const char *)UPTR(uaddr) + 2, n) < 0)
        return -EFAULT;
    if (family != AF_UNIX)
        return -EAFNOSUPPORT;
    path[n] = 0;
    return 0;
}

static int sys_socket(uint32_t domain, uint32_t type, uint32_t proto, uint32_t d, uint32_t e)
{
    (void)d; (void)e;
    if (domain == AF_UNIX) {
        if (type != SOCK_STREAM)
            return -EPROTOTYPE;
        if (proto)
            return -EPROTONOSUPPORT;
        struct file *f = unix_create();
        if (!f)
            return -ENOMEM;
        int fd = alloc_fd(f);
        if (fd < 0)
            file_close(f);
        return fd;
    }
    struct socket *s;
    int r = sock_create((int)domain, (int)type, (int)proto, &s);
    return r < 0 ? r : install_socket(s);
}

static int sys_bind(uint32_t fd, uint64_t uaddr, uint32_t len, uint32_t d, uint32_t e)
{
    (void)d; (void)e;
    struct file *uf = get_unix(fd);
    if (uf) {
        char path[UNIX_PATH_MAX];
        int r = copy_unix_path(uaddr, len, path);
        return r < 0 ? r : unix_bind(uf, path);
    }
    struct socket *s;
    struct sockaddr_in sin;
    int r = get_socket(fd, &s);
    if (r == 0)
        r = copy_sockaddr(uaddr, len, &sin);
    return r < 0 ? r : sock_bind(s, &sin);
}

static int sys_connect(uint32_t fd, uint64_t uaddr, uint32_t len, uint32_t d, uint32_t e)
{
    (void)d; (void)e;
    struct file *uf = get_unix(fd);
    if (uf) {
        char path[UNIX_PATH_MAX];
        int r = copy_unix_path(uaddr, len, path);
        return r < 0 ? r : unix_connect(uf, path);
    }
    struct socket *s;
    struct file *f;
    struct sockaddr_in sin;
    int r = get_socket_file(fd, &s, &f);
    if (r == 0)
        r = copy_sockaddr(uaddr, len, &sin);
    return r < 0 ? r : sock_connect(s, &sin, (f->flags & O_NONBLOCK) != 0);
}

static int sys_listen(uint32_t fd, uint32_t backlog, uint32_t c, uint32_t d, uint32_t e)
{
    (void)c; (void)d; (void)e;
    struct file *uf = get_unix(fd);
    if (uf)
        return unix_listen(uf, (int)backlog);
    struct socket *s;
    int r = get_socket(fd, &s);
    return r < 0 ? r : sock_listen(s, (int)backlog);
}

static int sys_accept(uint32_t fd, uint64_t uaddr, uint32_t c, uint32_t d, uint32_t e)
{
    (void)c; (void)d; (void)e;
    struct file *uf = get_unix(fd);
    if (uf) {
        /* The peer has no name to give: the caller's address is left as it is. */
        struct file *conn;
        int r = unix_accept(uf, &conn);
        if (r < 0)
            return r;
        r = alloc_fd(conn);
        if (r < 0)
            file_close(conn);
        return r;
    }
    struct socket *s, *conn;
    struct file *f;
    struct sockaddr_in peer;
    int r = get_socket_file(fd, &s, &f);
    if (r < 0)
        return r;
    if (uaddr && user_check_w(UPTR(uaddr), sizeof(peer)) < 0)
        return -EFAULT;
    r = sock_accept(s, &conn, &peer, (f->flags & O_NONBLOCK) != 0);
    if (r < 0)
        return r;
    r = install_socket(conn);
    if (r >= 0 && uaddr)
        copy_to_user(UPTR(uaddr), &peer, sizeof(peer));  /* checked above; the connection is the caller's now */
    return r;
}

static long sys_write(uint32_t fd, uint64_t buf, uint64_t n, uint32_t d, uint32_t e);
static long sys_read(uint32_t fd, uint64_t buf, uint64_t n, uint32_t d, uint32_t e);

/*
 * send and recv on an AF_UNIX descriptor, which is connected: no address.
 * MSG_DONTWAIT makes this one transfer non-blocking through a copy of the
 * file with O_NONBLOCK, not the descriptor's shared flags; the copy takes
 * no reference, as get_unix() holds the file (and so the connection) for
 * the call. MSG_PEEK copies what a read would give and leaves it there:
 * at most a pipe's worth, which is all a pipe can hold.
 */
static int unix_io(struct file *f, uint64_t buf, uint32_t n, uint32_t flags, int write)
{
    if ((write ? user_check(UPTR(buf), n) : user_check_w(UPTR(buf), n)) < 0)
        return -EFAULT;
    if (!write && (flags & MSG_PEEK)) {
        uint32_t len = MIN(n, (uint32_t)PIPE_SIZE);
        uint8_t *kbuf = bounce_alloc(len);
        if (!kbuf)
            return -ENOMEM;
        int nonblock = (flags & MSG_DONTWAIT) || (f->flags & O_NONBLOCK);
        int r = file_peek(f, kbuf, len, nonblock);
        if (r > 0 && copy_to_user(UPTR(buf), kbuf, (uint32_t)r) < 0)
            r = -EFAULT;
        kfree(kbuf);
        return r;
    }
    if (!(flags & MSG_DONTWAIT))
        return (int)file_io(f, (uint8_t *)UPTR(buf), n, write);    /* (n is 32 bits here, and a pipe's worth moves) */
    struct file once = *f;
    once.flags |= O_NONBLOCK;
    return (int)file_io(&once, (uint8_t *)UPTR(buf), n, write);
}

static int sys_sendto(uint32_t fd, uint64_t buf, uint32_t n, uint32_t flags, uint64_t uaddr)
{
    struct file *uf = get_unix(fd);
    if (uf)
        return unix_io(uf, buf, n, flags, 1);
    struct socket *s;
    struct file *f;
    struct sockaddr_in sin;
    int r = get_socket_file(fd, &s, &f);
    if (r < 0)
        return r;
    if (user_check(UPTR(buf), n) < 0)
        return -EFAULT;
    if (uaddr && (r = copy_sockaddr(uaddr, sizeof(sin), &sin)) < 0)
        return r;
    int sflags = (int)flags | ((f->flags & O_NONBLOCK) ? MSG_DONTWAIT : 0);
    /* Through a kernel buffer, as file_io(): the stack copies it holding net_lock.
     * (A datagram is far smaller than a chunk: one that is not fails whole.) */
    uint8_t *kbuf = bounce_alloc(n);
    if (!kbuf)
        return -ENOMEM;
    uint32_t done = 0;
    do {
        uint32_t chunk = MIN(n - done, (uint32_t)IO_CHUNK);
        if (copy_from_user(kbuf, (const uint8_t *)UPTR(buf) + done, chunk) < 0) {
            r = -EFAULT;
            break;
        }
        r = sock_sendto(s, kbuf, chunk, sflags, uaddr ? &sin : NULL);
        if (r < 0)
            break;
        done += (uint32_t)r;
        if ((uint32_t)r < chunk)
            break;
    } while (done < n);
    kfree(kbuf);
    return done ? (int)done : r;
}

static int sys_recvfrom(uint32_t fd, uint64_t buf, uint32_t n, uint32_t flags, uint64_t uaddr)
{
    struct file *uf = get_unix(fd);
    if (uf)
        return unix_io(uf, buf, n, flags, 0);
    struct socket *s;
    struct file *f;
    struct sockaddr_in sin;
    int r = get_socket_file(fd, &s, &f);
    if (r < 0)
        return r;
    if (user_check_w(UPTR(buf), n) < 0 || (uaddr && user_check_w(UPTR(uaddr), sizeof(sin)) < 0))
        return -EFAULT;
    int sflags = (int)flags | ((f->flags & O_NONBLOCK) ? MSG_DONTWAIT : 0);
    /* Up to a chunk, received into a kernel buffer as by file_io(). */
    uint8_t *kbuf = bounce_alloc(n);
    if (!kbuf)
        return -ENOMEM;
    r = sock_recvfrom(s, kbuf, MIN(n, (uint32_t)IO_CHUNK), sflags, uaddr ? &sin : NULL);
    if (r > 0 && copy_to_user(UPTR(buf), kbuf, (uint32_t)r) < 0)
        r = -EFAULT;                    /* (the data is taken, and lost) */
    kfree(kbuf);
    if (r >= 0 && uaddr)
        copy_to_user(UPTR(uaddr), &sin, sizeof(sin));    /* checked above; the data is already taken */
    return r;
}

static int sys_shutdown(uint32_t fd, uint32_t how, uint32_t c, uint32_t d, uint32_t e)
{
    (void)c; (void)d; (void)e;
    struct socket *s;
    int r = get_socket(fd, &s);
    return r < 0 ? r : sock_shutdown(s, (int)how);
}

/* getsockname (peer 0) and getpeername (peer 1) */
static int sys_sockname(uint32_t fd, uint64_t uaddr, uint32_t peer, uint32_t d, uint32_t e)
{
    (void)d; (void)e;
    struct socket *s;
    struct sockaddr_in sin;
    int r = get_socket(fd, &s);
    if (r < 0)
        return r;
    if (user_check_w(UPTR(uaddr), sizeof(sin)) < 0)
        return -EFAULT;
    r = sock_name(s, (int)peer, &sin);
    if (r == 0)
        r = copy_to_user(UPTR(uaddr), &sin, sizeof(sin));
    return r;
}

static int sys_netinfo(uint64_t ubuf, uint32_t b, uint32_t c, uint32_t d, uint32_t e)
{
    (void)b; (void)c; (void)d; (void)e;
    struct netinfo ni;
    net_get_info(&ni);
    return copy_to_user(UPTR(ubuf), &ni, sizeof(ni));
}

static int sys_netconf(uint64_t ubuf, uint32_t b, uint32_t c, uint32_t d, uint32_t e)
{
    (void)b; (void)c; (void)d; (void)e;
    struct netinfo ni;
    if (copy_from_user(&ni, UPTR(ubuf), sizeof(ni)) < 0)
        return -EFAULT;
    return net_configure(&ni);
}

/* ---- filesystems ---- */

static int sys_mount(uint64_t usrc, uint64_t utarget, uint64_t utype, uint32_t d, uint32_t e)
{
    (void)d; (void)e;
    COPY_PATH(src, usrc);
    COPY_PATH(target, utarget);
    char type[16] = "auto";
    if (utype && copy_str(type, (const char *)UPTR(utype), sizeof(type)) < 0)
        return -EINVAL;
    return vfs_mount(src, target, type, current->leader->cwd);
}

static int sys_umount(uint64_t utarget, uint32_t b, uint32_t c, uint32_t d, uint32_t e)
{
    (void)b; (void)c; (void)d; (void)e;
    COPY_PATH(target, utarget);
    return vfs_umount(target, current->leader->cwd);
}

static int sys_statfs(uint64_t upath, uint64_t ubuf, uint32_t c, uint32_t d, uint32_t e)
{
    (void)c; (void)d; (void)e;
    COPY_PATH(path, upath);
    if (user_check_w(UPTR(ubuf), sizeof(struct statfs)) < 0)
        return -EFAULT;
    struct vnode *vn;
    int r = vfs_resolve(path, current->leader->cwd, &vn);
    if (r < 0)
        return r;
    struct statfs st;
    r = vfs_statfs(vn, &st);
    return r < 0 ? r : copy_to_user(UPTR(ubuf), &st, sizeof(st));
}

static int sys_getmounts(uint64_t ubuf, uint32_t size, uint32_t c, uint32_t d, uint32_t e)
{
    (void)c; (void)d; (void)e;
    size = MIN(size, 8192u);
    if (user_check_w(UPTR(ubuf), size) < 0)
        return -EFAULT;
    char *tmp = kmalloc(size + 1);
    if (!tmp)
        return -ENOMEM;
    int n = vfs_mounts_text(tmp, size + 1);
    n = MIN(n, (int)size);
    if (copy_to_user(UPTR(ubuf), tmp, (uint32_t)n) < 0)
        n = -EFAULT;
    kfree(tmp);
    return n;
}

static int sys_blkinfo(uint32_t idx, uint64_t ubuf, uint32_t c, uint32_t d, uint32_t e)
{
    (void)c; (void)d; (void)e;
    if (user_check_w(UPTR(ubuf), sizeof(struct blkinfo)) < 0)
        return -EFAULT;
    struct blockdev *bd = blk_get((int)idx);
    if (!bd)
        return -ENODEV;
    struct blkinfo bi;
    memset(&bi, 0, sizeof(bi));
    if (bd->refresh)
        bd->refresh(bd);
    strlcpy(bi.name, bd->name, sizeof(bi.name));
    strlcpy(bi.model, bd->model, sizeof(bi.model));
    bi.type = (uint32_t)bd->type;
    bi.sector_size = bd->sector_size;
    bi.sectors = bd->nsectors;
    bi.start = bd->start;
    if (bd->fs) {
        strlcpy(bi.fstype, bd->fs->name, sizeof(bi.fstype));
        vfs_mountpoint_of(bd, bi.mountpoint, sizeof(bi.mountpoint));
    }
    return copy_to_user(UPTR(ubuf), &bi, sizeof(bi));
}

static int sys_usbinfo(uint32_t idx, uint64_t ubuf, uint32_t c, uint32_t d, uint32_t e)
{
    (void)c; (void)d; (void)e;
    if (user_check_w(UPTR(ubuf), sizeof(struct usbinfo)) < 0)
        return -EFAULT;
    struct usbinfo ui;
    int r = usb_info((int)idx, &ui);
    return r < 0 ? r : copy_to_user(UPTR(ubuf), &ui, sizeof(ui));
}

static int sys_wifi(uint32_t op, uint64_t arg, uint32_t c, uint32_t d, uint32_t e)
{
    (void)c; (void)d; (void)e;
    return wifi_syscall(op, arg);
}

static int sys_power(uint32_t op, uint64_t arg, uint32_t c, uint32_t d, uint32_t e)
{
    (void)c; (void)d; (void)e;
    if (op == POWER_INFO && user_check_w(UPTR(arg), sizeof(struct powerinfo)) < 0)
        return -EFAULT;
    return power_syscall(op, arg);
}

/* ---- system information ---- */

static int sys_time(uint32_t a, uint32_t b, uint32_t c, uint32_t d, uint32_t e)
{
    (void)a; (void)b; (void)c; (void)d; (void)e;
    return (int)time_now();
}

static int sys_stime(uint32_t now, uint32_t b, uint32_t c, uint32_t d, uint32_t e)
{
    (void)b; (void)c; (void)d; (void)e;
    time_set(now);
    return 0;
}

static int sys_uptime(uint32_t a, uint32_t b, uint32_t c, uint32_t d, uint32_t e)
{
    (void)a; (void)b; (void)c; (void)d; (void)e;
    return (int)(ticks * (1000 / HZ));
}

static int sys_uname(uint64_t ubuf, uint32_t b, uint32_t c, uint32_t d, uint32_t e)
{
    (void)b; (void)c; (void)d; (void)e;
    struct utsname u;
    memset(&u, 0, sizeof(u));
    strcpy(u.sysname, FLUXOS_NAME);
    strcpy(u.nodename, "fluxOS");
    strcpy(u.release, FLUXOS_RELEASE);
    strcpy(u.version, "#1 PREEMPT " __DATE__ " " __TIME__);
    /* What the program asking is (and what programs like it are built as). */
    strcpy(u.machine, current->user64 ? "x86_64" : "i686");
    return copy_to_user(UPTR(ubuf), &u, sizeof(u));
}

static int sys_procinfo(uint32_t slot, uint64_t ubuf, uint32_t c, uint32_t d, uint32_t e)
{
    (void)c; (void)d; (void)e;
    if (user_check_w(UPTR(ubuf), sizeof(struct procinfo)) < 0)
        return -EFAULT;
    struct procinfo pi;
    int r = proc_info((int)slot, &pi);
    if (r == 1 && copy_to_user(UPTR(ubuf), &pi, sizeof(pi)) < 0)
        return -EFAULT;
    return r;
}

static int sys_meminfo(uint64_t ubuf, uint32_t b, uint32_t c, uint32_t d, uint32_t e)
{
    (void)b; (void)c; (void)d; (void)e;
    struct meminfo mi;
    memset(&mi, 0, sizeof(mi));
    uint32_t htotal, hused;
    heap_stats(&htotal, &hused);
    mi.total_kb = pmm_total_kb();
    mi.free_kb = pmm_free_kb();
    mi.kernel_kb = (uint32_t)(kernel_end - kernel_start) / 1024;
    mi.heap_total_kb = htotal / 1024;
    mi.heap_used_kb = hused / 1024;
    mi.ncpu = (uint32_t)ncpus;
    for (int i = 0; i < 16; i++)
        mi.cpu_idle[i] = i < ncpus ? cpus[i].idle_ticks : 0;
    return copy_to_user(UPTR(ubuf), &mi, sizeof(mi));
}

/* Re-read the partition table of a whole disk (like Linux's BLKRRPART). */
static int sys_blkrescan(uint64_t upath, uint32_t b, uint32_t c, uint32_t d, uint32_t e)
{
    (void)b; (void)c; (void)d; (void)e;
    COPY_PATH(path, upath);
    struct vnode *vn;
    int r = vfs_resolve(path, current->leader->cwd, &vn);
    if (r < 0)
        return r;
    struct blockdev *bd = blk_by_vnode(vn);
    return bd ? blk_rescan(bd) : -ENOTBLK;
}

/* Trim the free space of the filesystem holding path (like Linux's FITRIM). */
static int sys_fstrim(uint64_t upath, uint64_t utrimmed, uint32_t c, uint32_t d, uint32_t e)
{
    (void)c; (void)d; (void)e;
    COPY_PATH(path, upath);
    if (utrimmed && user_check_w(UPTR(utrimmed), sizeof(uint64_t)) < 0)
        return -EFAULT;
    struct vnode *vn;
    int r = vfs_resolve(path, current->leader->cwd, &vn);
    if (r < 0)
        return r;
    uint64_t trimmed;
    r = vfs_trim(vn, &trimmed);
    if (utrimmed && copy_to_user(UPTR(utrimmed), &trimmed, sizeof(trimmed)) < 0)
        return -EFAULT;
    return r;
}

/* ---- resource limits ----
 *
 * The one the kernel keeps is the stack's (the others are the C library's
 * to answer for: what they would limit has a fixed size here, or none).
 */
static int sys_getrlimit(uint32_t resource, uint64_t ulim, uint32_t c, uint32_t d, uint32_t e)
{
    (void)c; (void)d; (void)e;
    if (resource != FLUX_RLIMIT_STACK)
        return -EINVAL;
    struct flux_rlimit l;
    proc_stack_rlimit(current, &l);
    return copy_to_user(UPTR(ulim), &l, sizeof(l));
}

static int sys_setrlimit(uint32_t resource, uint64_t ulim, uint32_t c, uint32_t d, uint32_t e)
{
    (void)c; (void)d; (void)e;
    if (resource != FLUX_RLIMIT_STACK)
        return -EINVAL;
    struct flux_rlimit l;
    if (copy_from_user(&l, UPTR(ulim), sizeof(l)) < 0)
        return -EFAULT;
    return proc_set_stack_rlimit(current, &l);
}

static int sys_sync(uint32_t a, uint32_t b, uint32_t c, uint32_t d, uint32_t e)
{
    (void)a; (void)b; (void)c; (void)d; (void)e;
    return blk_sync_all();
}

/* Sync the device holding the file (or the block device itself). */
static int sys_fsync(uint32_t fd, uint32_t b, uint32_t c, uint32_t d, uint32_t e)
{
    (void)b; (void)c; (void)d; (void)e;
    struct file *f = get_file(fd);
    if (!f)
        return -EBADF;
    if (f->type != FT_VNODE)
        return -EINVAL;
    struct blockdev *bd = blk_by_vnode(f->vn);
    if (!bd && f->vn->fs)
        bd = f->vn->fs->bd;
    return bd ? blk_sync(bd) : 0;
}

/* (The size is an off_t: 64 bits of it from a 64-bit program, 32 from a 32-bit one.) */
static int sys_ftruncate(uint32_t fd, uint64_t size, uint32_t c, uint32_t d, uint32_t e)
{
    (void)c; (void)d; (void)e;
    if (current->user64 ? (int64_t)size < 0 : (int32_t)size < 0)
        return -EINVAL;
    struct file *f = get_file(fd);
    if (!f)
        return -EBADF;
    if (f->type != FT_VNODE || f->vn->type != VN_FILE)
        return -EINVAL;
    if ((f->flags & O_ACCMODE) == O_RDONLY)
        return -EBADF;
    return vfs_truncate(f->vn, size);
}

static int sys_fbinfo(uint64_t ubuf, uint32_t b, uint32_t c, uint32_t d, uint32_t e)
{
    (void)b; (void)c; (void)d; (void)e;
    if (user_check_w(UPTR(ubuf), sizeof(struct fbinfo)) < 0)
        return -EFAULT;
    struct fbinfo fi;
    int r = fb_get_info(&fi);
    return r < 0 ? r : copy_to_user(UPTR(ubuf), &fi, sizeof(fi));
}

static long sys_fbmap(uint32_t a, uint32_t b, uint32_t c, uint32_t d, uint32_t e)
{
    (void)a; (void)b; (void)c; (void)d; (void)e;
    /* The screen's address, which is above 2 GiB: not a negative number. */
    int r = fb_map_user();
    return r < 0 && r >= -4095 ? r : (long)(uint32_t)r;
}

int user_access_ok(const void *ptr, uint32_t len)
{
    return user_check_w((void *)ptr, len) == 0;
}

/* Device control: the device checks the request and its argument. */
static int sys_ioctl(uint32_t fd, uint32_t req, uint64_t arg, uint32_t d, uint32_t e)
{
    (void)d; (void)e;
    struct file *f = get_file(fd);
    if (!f)
        return -EBADF;
    if (f->type != FT_VNODE || f->vn->type != VN_CHR || !f->vn->dev_ioctl)
        return -ENOTTY;
    return f->vn->dev_ioctl(f->vn, req, arg);
}

static int sys_display(uint32_t op, uint64_t uarg, uint32_t c, uint32_t d, uint32_t e)
{
    (void)c; (void)d; (void)e;
    uint32_t size;
    int writes = 0;
    switch (op) {
    case DISP_INFO: size = sizeof(struct dispinfo); writes = 1; break;
    case DISP_MODES: size = DISP_MAX_MODES * sizeof(struct dispmode); writes = 1; break;
    case DISP_SETMODE: size = sizeof(struct dispmode); break;
    case DISP_MAP3D: size = sizeof(struct disp3d); writes = 1; break;
    case DISP_FLIP: size = sizeof(struct dispflip); break;
    case DISP_CURSOR: size = sizeof(struct dispcursor); break;
    case DISP_CURSORPOS: size = sizeof(struct disppos); break;
    case DISP_COPYRECTS: size = sizeof(struct dispcopy); break;
    case DISP_MAPBUF: size = sizeof(struct dispbuf); writes = 1; break;     /* (the caller's layout: below) */
    case DISP_UNMAPBUF: size = sizeof(struct dispbuf); break;
    case DISP_BLIT: size = sizeof(struct dispblit); break;
    case DISP_SUBMIT: size = sizeof(struct dispsubmit); break;
    default: return -EINVAL;
    }
    /* A 32-bit program's struct dispbuf is laid out its own way (compat.h). */
    int buf32 = !current->user64 && (op == DISP_MAPBUF || op == DISP_UNMAPBUF);
    uint32_t usize = buf32 ? sizeof(struct dispbuf32) : size;
    if ((writes ? user_check_w(UPTR(uarg), usize) : user_check(UPTR(uarg), usize)) < 0)
        return -EFAULT;
    /* The drivers work on a private copy: they check fields and use them
     * again later, and another thread could change the caller's memory in
     * between, or unmap it while the driver sleeps. */
    void *karg = kmalloc(size);
    if (!karg)
        return -ENOMEM;
    struct dispbuf32 b32;
    if (copy_from_user(buf32 ? (void *)&b32 : karg, UPTR(uarg), usize) < 0) {
        kfree(karg);
        return -EFAULT;
    }
    if (buf32)
        *(struct dispbuf *)karg = (struct dispbuf){ b32.addr, b32.size, b32.handle, b32.gpu_lo, b32.gpu_hi };
    /* DISP_MAPBUF breaks copy-on-write in the caller's pages, which only the
     * holder of mm_lock may do (another thread's fault, a fork). The lock
     * comes first, as for munmap and fork, which ask the display whether
     * pages are lent (fb_pinned). */
    struct mutex *mm = op == DISP_MAPBUF ? &current->leader->mm_lock : NULL;
    if (mm)
        mutex_lock(mm);
    int r = fb_display((int)op, karg);
    if (mm)
        mutex_unlock(mm);
    if (buf32) {
        const struct dispbuf *b = karg;
        b32 = (struct dispbuf32){ (uint32_t)b->addr, b->size, b->handle, b->gpu_lo, b->gpu_hi };
    }
    if (writes && copy_to_user(UPTR(uarg), buf32 ? (void *)&b32 : karg, usize) < 0)
        r = -EFAULT;
    kfree(karg);
    return r;
}

static int sys_reboot(uint32_t cmd, uint32_t b, uint32_t c, uint32_t d, uint32_t e)
{
    (void)b; (void)c; (void)d; (void)e;
    if (cmd == REBOOT_RESTART || cmd == REBOOT_POWEROFF || cmd == REBOOT_HALT) {
        vfs_lock();
        vfs_sync_filesystems();                 /* volumes left clean (ext2) */
        blk_sync_all();                         /* nothing cached may be lost */
        vfs_unlock();
    }
    switch (cmd) {
    case REBOOT_RESTART:
        kprintf("\nfluxOS: restarting system...\n");
        machine_reboot();
    case REBOOT_POWEROFF:
        kprintf("\nfluxOS: powering off...\n");
        machine_poweroff();
        return -EIO;                    /* still on: the caller says it is safe to switch off */
    case REBOOT_HALT:
        machine_halt();
    }
    return -EINVAL;
}

static int sys_dmesg(uint64_t ubuf, uint32_t size, uint32_t c, uint32_t d, uint32_t e)
{
    (void)c; (void)d; (void)e;
    size = MIN(size, (uint32_t)KLOG_SIZE);
    if (user_check_w(UPTR(ubuf), size) < 0)
        return -EFAULT;
    char *tmp = kmalloc(size ? size : 1);   /* the log is read holding its lock */
    if (!tmp)
        return -ENOMEM;
    int n = klog_read(tmp, size);
    if (copy_to_user(UPTR(ubuf), tmp, (uint32_t)n) < 0)
        n = -EFAULT;
    kfree(tmp);
    return n;
}

/*
 * The table's entries: each hands the registers to its handler, which takes
 * of each what its argument's type holds (the whole register for an
 * address, its low half for the rest) and whose result, an int or a long,
 * is the long the program gets.
 */
#define ENTRY(fn) \
    static long fn##_entry(uint64_t a, uint64_t b, uint64_t c, uint64_t d, uint64_t e) \
    { \
        return fn(a, b, c, d, e); \
    }
ENTRY(sys_exit)
ENTRY(sys_fork)
ENTRY(sys_read)
ENTRY(sys_write)
ENTRY(sys_open)
ENTRY(sys_close)
ENTRY(sys_waitpid)
ENTRY(sys_execve)
ENTRY(sys_chdir)
ENTRY(sys_time)
ENTRY(sys_getpid)
ENTRY(sys_thread_create)
ENTRY(sys_thread_exit)
ENTRY(sys_futex)
ENTRY(sys_set_tls)
ENTRY(sys_set_win_teb)
ENTRY(sys_gettid)
ENTRY(sys_tkill)
ENTRY(sys_setitimer)
ENTRY(sys_getitimer)
ENTRY(sys_sigpending)
ENTRY(sys_sigaltstack)
ENTRY(sys_setsid)
ENTRY(sys_getsid)
ENTRY(sys_getppid)
ENTRY(sys_kill)
ENTRY(sys_mkdir)
ENTRY(sys_rmdir)
ENTRY(sys_unlink)
ENTRY(sys_dup)
ENTRY(sys_dup2)
ENTRY(sys_pipe)
ENTRY(sys_sbrk)
ENTRY(sys_sleep)
ENTRY(sys_lseek)
ENTRY(sys_stat)
ENTRY(sys_fstat)
ENTRY(sys_getcwd)
ENTRY(sys_getdents)
ENTRY(sys_uname)
ENTRY(sys_yield)
ENTRY(sys_signal)
ENTRY(sys_setpgid)
ENTRY(sys_getpgid)
ENTRY(sys_tcsetpgrp)
ENTRY(sys_tcgetpgrp)
ENTRY(sys_ttymode)
ENTRY(sys_procinfo)
ENTRY(sys_meminfo)
ENTRY(sys_uptime)
ENTRY(sys_reboot)
ENTRY(sys_rename)
ENTRY(sys_dmesg)
ENTRY(sys_chmod)
ENTRY(sys_mount)
ENTRY(sys_umount)
ENTRY(sys_statfs)
ENTRY(sys_getmounts)
ENTRY(sys_blkinfo)
ENTRY(sys_sync)
ENTRY(sys_fsync)
ENTRY(sys_blkrescan)
ENTRY(sys_fstrim)
ENTRY(sys_getrlimit)
ENTRY(sys_setrlimit)
ENTRY(sys_fbinfo)
ENTRY(sys_fbmap)
ENTRY(sys_openpty)
ENTRY(sys_poll)
ENTRY(sys_socket)
ENTRY(sys_bind)
ENTRY(sys_connect)
ENTRY(sys_listen)
ENTRY(sys_accept)
ENTRY(sys_sendto)
ENTRY(sys_recvfrom)
ENTRY(sys_shutdown)
ENTRY(sys_sockname)
ENTRY(sys_netinfo)
ENTRY(sys_netconf)
ENTRY(sys_ftruncate)
ENTRY(sys_display)
ENTRY(sys_ioctl)
ENTRY(sys_usbinfo)
ENTRY(sys_power)
ENTRY(sys_wifi)
ENTRY(sys_stime)
ENTRY(sys_llseek)
ENTRY(sys_sigaction)
ENTRY(sys_sigprocmask)
ENTRY(sys_sigreturn)
ENTRY(sys_mmap)
ENTRY(sys_munmap)
ENTRY(sys_mprotect)
ENTRY(sys_fcntl)
ENTRY(sys_umask)
ENTRY(sys_utime)
ENTRY(sys_lstat)
ENTRY(sys_readlink)
ENTRY(sys_symlink)
ENTRY(sys_link)
ENTRY(sys_chown)
ENTRY(sys_fchown)
ENTRY(sys_times)
ENTRY(sys_fchdir)
ENTRY(sys_fchmod)
ENTRY(sys_shmget)
ENTRY(sys_shmat)
ENTRY(sys_shmctl)

static const syscall_fn syscalls[NSYSCALLS] = {
    [SYS_exit] = sys_exit_entry,
    [SYS_fork] = sys_fork_entry,
    [SYS_read] = sys_read_entry,
    [SYS_write] = sys_write_entry,
    [SYS_open] = sys_open_entry,
    [SYS_close] = sys_close_entry,
    [SYS_waitpid] = sys_waitpid_entry,
    [SYS_execve] = sys_execve_entry,
    [SYS_chdir] = sys_chdir_entry,
    [SYS_time] = sys_time_entry,
    [SYS_getpid] = sys_getpid_entry,
    [SYS_thread_create] = sys_thread_create_entry,
    [SYS_thread_exit] = sys_thread_exit_entry,
    [SYS_futex] = sys_futex_entry,
    [SYS_set_tls] = sys_set_tls_entry,
    [SYS_set_win_teb] = sys_set_win_teb_entry,
    [SYS_gettid] = sys_gettid_entry,
    [SYS_tkill] = sys_tkill_entry,
    [SYS_setitimer] = sys_setitimer_entry,
    [SYS_getitimer] = sys_getitimer_entry,
    [SYS_sigpending] = sys_sigpending_entry,
    [SYS_sigaltstack] = sys_sigaltstack_entry,
    [SYS_setsid] = sys_setsid_entry,
    [SYS_getsid] = sys_getsid_entry,
    [SYS_getppid] = sys_getppid_entry,
    [SYS_kill] = sys_kill_entry,
    [SYS_mkdir] = sys_mkdir_entry,
    [SYS_rmdir] = sys_rmdir_entry,
    [SYS_unlink] = sys_unlink_entry,
    [SYS_dup] = sys_dup_entry,
    [SYS_dup2] = sys_dup2_entry,
    [SYS_pipe] = sys_pipe_entry,
    [SYS_sbrk] = sys_sbrk_entry,
    [SYS_sleep] = sys_sleep_entry,
    [SYS_lseek] = sys_lseek_entry,
    [SYS_stat] = sys_stat_entry,
    [SYS_fstat] = sys_fstat_entry,
    [SYS_getcwd] = sys_getcwd_entry,
    [SYS_getdents] = sys_getdents_entry,
    [SYS_uname] = sys_uname_entry,
    [SYS_yield] = sys_yield_entry,
    [SYS_signal] = sys_signal_entry,
    [SYS_setpgid] = sys_setpgid_entry,
    [SYS_getpgid] = sys_getpgid_entry,
    [SYS_tcsetpgrp] = sys_tcsetpgrp_entry,
    [SYS_tcgetpgrp] = sys_tcgetpgrp_entry,
    [SYS_ttymode] = sys_ttymode_entry,
    [SYS_procinfo] = sys_procinfo_entry,
    [SYS_meminfo] = sys_meminfo_entry,
    [SYS_uptime] = sys_uptime_entry,
    [SYS_reboot] = sys_reboot_entry,
    [SYS_rename] = sys_rename_entry,
    [SYS_dmesg] = sys_dmesg_entry,
    [SYS_chmod] = sys_chmod_entry,
    [SYS_mount] = sys_mount_entry,
    [SYS_umount] = sys_umount_entry,
    [SYS_statfs] = sys_statfs_entry,
    [SYS_getmounts] = sys_getmounts_entry,
    [SYS_blkinfo] = sys_blkinfo_entry,
    [SYS_sync] = sys_sync_entry,
    [SYS_fsync] = sys_fsync_entry,
    [SYS_blkrescan] = sys_blkrescan_entry,
    [SYS_fstrim] = sys_fstrim_entry,
    [SYS_getrlimit] = sys_getrlimit_entry,
    [SYS_setrlimit] = sys_setrlimit_entry,
    [SYS_fbinfo] = sys_fbinfo_entry,
    [SYS_fbmap] = sys_fbmap_entry,
    [SYS_openpty] = sys_openpty_entry,
    [SYS_poll] = sys_poll_entry,
    [SYS_socket] = sys_socket_entry,
    [SYS_bind] = sys_bind_entry,
    [SYS_connect] = sys_connect_entry,
    [SYS_listen] = sys_listen_entry,
    [SYS_accept] = sys_accept_entry,
    [SYS_sendto] = sys_sendto_entry,
    [SYS_recvfrom] = sys_recvfrom_entry,
    [SYS_shutdown] = sys_shutdown_entry,
    [SYS_sockname] = sys_sockname_entry,
    [SYS_netinfo] = sys_netinfo_entry,
    [SYS_netconf] = sys_netconf_entry,
    [SYS_ftruncate] = sys_ftruncate_entry,
    [SYS_display] = sys_display_entry,
    [SYS_ioctl] = sys_ioctl_entry,
    [SYS_usbinfo] = sys_usbinfo_entry,
    [SYS_power] = sys_power_entry,
    [SYS_wifi] = sys_wifi_entry,
    [SYS_stime] = sys_stime_entry,
    [SYS_llseek] = sys_llseek_entry,
    [SYS_sigaction] = sys_sigaction_entry,
    [SYS_sigprocmask] = sys_sigprocmask_entry,
    [SYS_sigreturn] = sys_sigreturn_entry,
    [SYS_mmap] = sys_mmap_entry,
    [SYS_munmap] = sys_munmap_entry,
    [SYS_mprotect] = sys_mprotect_entry,
    [SYS_fcntl] = sys_fcntl_entry,
    [SYS_umask] = sys_umask_entry,
    [SYS_utime] = sys_utime_entry,
    [SYS_lstat] = sys_lstat_entry,
    [SYS_readlink] = sys_readlink_entry,
    [SYS_symlink] = sys_symlink_entry,
    [SYS_link] = sys_link_entry,
    [SYS_chown] = sys_chown_entry,
    [SYS_fchown] = sys_fchown_entry,
    [SYS_times] = sys_times_entry,
    [SYS_fchdir] = sys_fchdir_entry,
    [SYS_fchmod] = sys_fchmod_entry,
    [SYS_shmget] = sys_shmget_entry,
    [SYS_shmat] = sys_shmat_entry,
    [SYS_shmctl] = sys_shmctl_entry,
};

/* VFS operations and shared file offsets; read and write lock per chunk. */
static const uint8_t takes_vfs_lock[NSYSCALLS] = {
    [SYS_open] = 1, [SYS_execve] = 1, [SYS_chdir] = 1, [SYS_getcwd] = 1,
    [SYS_getdents] = 1, [SYS_mkdir] = 1, [SYS_rmdir] = 1, [SYS_unlink] = 1,
    [SYS_rename] = 1, [SYS_chmod] = 1, [SYS_stat] = 1, [SYS_mount] = 1,
    [SYS_umount] = 1, [SYS_statfs] = 1, [SYS_getmounts] = 1, [SYS_blkinfo] = 1,
    [SYS_sync] = 1, [SYS_fsync] = 1, [SYS_blkrescan] = 1, [SYS_ftruncate] = 1,
    [SYS_lseek] = 1, [SYS_llseek] = 1, [SYS_utime] = 1, [SYS_fchdir] = 1, [SYS_fchmod] = 1,
    [SYS_lstat] = 1, [SYS_readlink] = 1, [SYS_symlink] = 1, [SYS_link] = 1, [SYS_chown] = 1,
    [SYS_fchown] = 1, [SYS_fstrim] = 1,
};

void syscall_dispatch(struct trapframe *tf)
{
    uint32_t n = tf->eax;
    int wide = current->user64;         /* (as the call found it: exec may make it another kind) */
    if (n >= NSYSCALLS || !syscalls[n]) {
        tf->rax = wide ? (uint64_t)-ENOSYS : (uint32_t)-ENOSYS;
        return;
    }
    current->in_syscall = (int)n;
    current->syscall_orig_eax = n;
    current->npaths = 0;                /* its path buffers are free again */
    if (takes_vfs_lock[n])
        vfs_lock();
    long r = wide ? syscalls[n](tf->rbx, tf->rcx, tf->rdx, tf->rsi, tf->rdi)
                  : syscalls[n](tf->ebx, tf->ecx, tf->edx, tf->esi, tf->edi);
    /* (exec and sigreturn return the new registers' own rax, of whichever kind of program.) */
    tf->rax = current->user64 ? (uint64_t)r : (uint32_t)r;
    if (takes_vfs_lock[n])
        vfs_unlock();
    put_held_files();                   /* after the lock: a last close may take it */
}

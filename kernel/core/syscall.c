/*
 * System call entry points. Arguments arrive in ebx, ecx, edx, esi, edi;
 * every user pointer is validated (and the stack demand-grown) before
 * the kernel touches it.
 */
#include "kernel.h"
#include "blkdev.h"
#include "drivers.h"
#include "fs.h"
#include "mm.h"
#include "net.h"
#include "proc.h"
#include "syscall.h"
#include "tty.h"
#include "vfs.h"
#include <flux/syscall.h>

typedef int (*syscall_fn)(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t);

/* ---- user memory access ---- */

static int user_check(const void *ptr, uint32_t len)
{
    uint32_t a = (uint32_t)ptr;
    if (!len)
        return 0;
    if (a < USER_BASE || a >= USER_TOP || len > USER_TOP - a)
        return -EFAULT;
    for (uint32_t pg = ALIGN_DOWN(a, PAGE_SIZE); pg < a + len; pg += PAGE_SIZE)
        if (!vmm_translate(current->pd, pg) && !proc_grow_stack(pg))
            return -EFAULT;
    return 0;
}

static int user_check_w(void *ptr, uint32_t len)
{
    uint32_t a = (uint32_t)ptr;
    if (!len)
        return 0;
    if (a < USER_BASE || a >= USER_TOP || len > USER_TOP - a)
        return -EFAULT;
    for (uint32_t pg = ALIGN_DOWN(a, PAGE_SIZE); pg < a + len; pg += PAGE_SIZE)
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

/* Copy a NUL-terminated user string; returns its length. */
static int copy_str(char *dst, const char *src, uint32_t max)
{
    uint32_t a = (uint32_t)src;
    for (uint32_t i = 0; i < max; i++, a++) {
        if ((i == 0 || (a & 0xFFF) == 0) && user_check((const void *)a, 1) < 0)
            return -EFAULT;
        dst[i] = *(const char *)a;
        if (!dst[i])
            return (int)i;
    }
    return -ENAMETOOLONG;
}

#define COPY_PATH(var, uptr)                            \
    char var[PATH_MAX];                                 \
    do {                                                \
        int _r = copy_str(var, (const char *)(uptr), PATH_MAX); \
        if (_r < 0)                                     \
            return _r;                                  \
    } while (0)

static struct file *get_file(uint32_t fd)
{
    return fd < MAX_FDS ? current->fds[fd] : NULL;
}

static int alloc_fd_from(struct file *f, int min_fd, int cloexec)
{
    if (min_fd < 0 || min_fd >= MAX_FDS)
        return -EINVAL;
    for (int i = min_fd; i < MAX_FDS; i++) {
        if (!current->fds[i]) {
            current->fds[i] = f;
            fd_cloexec_set(current, i, cloexec);
            return i;
        }
    }
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
static int copy_vector(uint32_t uvec, char **kvec, int max, char **pool, uint32_t *pool_left)
{
    int n = 0;
    if (!uvec) {
        kvec[0] = NULL;
        return 0;
    }
    for (;; n++) {
        uint32_t slot = uvec + (uint32_t)n * 4;
        if (user_check((const void *)slot, 4) < 0)
            return -EFAULT;
        const char *s = *(const char **)slot;
        if (!s)
            break;
        if (n >= max)
            return -E2BIG;
        int len = copy_str(*pool, s, *pool_left);
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

static int sys_execve(uint32_t upath, uint32_t uargv, uint32_t uenvp, uint32_t d, uint32_t e)
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

static int sys_waitpid(uint32_t pid, uint32_t ustatus, uint32_t options, uint32_t d, uint32_t e)
{
    (void)d; (void)e;
    if (ustatus && user_check_w((void *)ustatus, sizeof(int)) < 0)
        return -EFAULT;
    int status = 0;
    int r = do_waitpid((int)pid, &status, (int)options);
    if (r > 0 && ustatus && copy_to_user((void *)ustatus, &status, sizeof(status)) < 0)
        return -EFAULT;
    return r;
}

static int sys_getpid(uint32_t a, uint32_t b, uint32_t c, uint32_t d, uint32_t e)
{
    (void)a; (void)b; (void)c; (void)d; (void)e;
    return current->pid;
}

static int sys_getppid(uint32_t a, uint32_t b, uint32_t c, uint32_t d, uint32_t e)
{
    (void)a; (void)b; (void)c; (void)d; (void)e;
    return current->ppid;
}

static int sys_kill(uint32_t pid, uint32_t sig, uint32_t c, uint32_t d, uint32_t e)
{
    (void)c; (void)d; (void)e;
    return do_kill((int)pid, (int)sig);
}

static int sys_sbrk(uint32_t incr, uint32_t b, uint32_t c, uint32_t d, uint32_t e)
{
    (void)b; (void)c; (void)d; (void)e;
    struct proc *p = current;
    uint32_t old = p->brk;
    int32_t delta = (int32_t)incr;
    uint32_t nbrk = old + (uint32_t)delta;

    if (delta > 0) {
        if (nbrk < old || nbrk > proc_vma_lowest(p))
            return -ENOMEM;
        for (uint32_t va = ALIGN_UP(old, PAGE_SIZE); va < nbrk; va += PAGE_SIZE) {
            if (vmm_alloc(p->pd, va) < 0) {
                for (uint32_t u = ALIGN_UP(old, PAGE_SIZE); u < va; u += PAGE_SIZE)
                    vmm_free_page(p->pd, u);
                return -ENOMEM;
            }
        }
    } else if (delta < 0) {
        if (nbrk < p->heap_start || nbrk > old)
            return -EINVAL;
        /* Pages lent to the GPU stay until they are taken back (DISP_UNMAPBUF). */
        if (fb_pinned(p->pid, ALIGN_UP(nbrk, PAGE_SIZE), ALIGN_UP(old, PAGE_SIZE)))
            return -EBUSY;
        for (uint32_t va = ALIGN_UP(nbrk, PAGE_SIZE); va < ALIGN_UP(old, PAGE_SIZE); va += PAGE_SIZE)
            vmm_free_page(p->pd, va);
    }
    p->brk = nbrk;
    return (int)old;
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

static int sys_signal(uint32_t sig, uint32_t disp, uint32_t c, uint32_t d, uint32_t e)
{
    (void)c; (void)d; (void)e;
    if (sig == 0 || sig >= NSIG || sig == SIGKILL || sig == SIGSTOP)
        return -EINVAL;
    uint32_t bit = 1u << sig;
    struct proc *p = current;
    /* Serialize the disposition and pending-bit update with signal_locked(). */
    spin_lock(&sched_lock);
    int old = (int)p->sig_actions[sig].fsa_handler;
    if (p->sig_ignored & bit)
        old = SIGDISP_IGN;
    p->sig_actions[sig].fsa_handler = disp;
    p->sig_actions[sig].fsa_flags = 0;
    p->sig_actions[sig].fsa_mask = 0;
    if (disp == SIGDISP_IGN) {
        __atomic_or_fetch(&p->sig_ignored, bit, __ATOMIC_RELAXED);
        __atomic_and_fetch(&p->sig_pending, ~bit, __ATOMIC_RELAXED);
    } else {
        __atomic_and_fetch(&p->sig_ignored, ~bit, __ATOMIC_RELAXED);
        if (disp == SIGDISP_DFL &&
            (sig == SIGCHLD || sig == SIGURG || sig == SIGWINCH || sig == SIGCONT))
            __atomic_and_fetch(&p->sig_pending, ~bit, __ATOMIC_RELAXED);
    }
    spin_unlock(&sched_lock);
    return old;
}

static int sys_sigaction(uint32_t sig, uint32_t uact, uint32_t uoact, uint32_t d, uint32_t e)
{
    (void)d; (void)e;
    if (sig == 0 || sig >= NSIG)
        return -EINVAL;
    if (uact && (sig == SIGKILL || sig == SIGSTOP))
        return -EINVAL;
    struct flux_sigaction act;
    if (uact) {
        if (copy_from_user(&act, (const void *)uact, sizeof(act)) < 0)
            return -EFAULT;
        if (act.fsa_handler != SIGDISP_DFL && act.fsa_handler != SIGDISP_IGN &&
            (act.fsa_handler < USER_BASE || act.fsa_handler >= USER_TOP))
            return -EINVAL;
    }
    if (uoact && user_check_w((void *)uoact, sizeof(struct flux_sigaction)) < 0)
        return -EFAULT;

    struct proc *p = current;
    uint32_t bit = 1u << sig;
    spin_lock(&sched_lock);
    struct flux_sigaction old = p->sig_actions[sig];
    if (p->sig_ignored & bit)
        old.fsa_handler = SIGDISP_IGN;
    if (uact) {
        act.fsa_mask &= ~((1u << SIGKILL) | (1u << SIGSTOP));
        p->sig_actions[sig] = act;
        if (act.fsa_handler == SIGDISP_IGN) {
            __atomic_or_fetch(&p->sig_ignored, bit, __ATOMIC_RELAXED);
            __atomic_and_fetch(&p->sig_pending, ~bit, __ATOMIC_RELAXED);
        } else {
            __atomic_and_fetch(&p->sig_ignored, ~bit, __ATOMIC_RELAXED);
            if (act.fsa_handler == SIGDISP_DFL &&
                (sig == SIGCHLD || sig == SIGURG || sig == SIGWINCH || sig == SIGCONT))
                __atomic_and_fetch(&p->sig_pending, ~bit, __ATOMIC_RELAXED);
        }
    }
    spin_unlock(&sched_lock);
    if (uoact && copy_to_user((void *)uoact, &old, sizeof(old)) < 0)
        return -EFAULT;
    return 0;
}

static int sys_sigprocmask(uint32_t how, uint32_t uset, uint32_t uoset, uint32_t d, uint32_t e)
{
    (void)d; (void)e;
    uint32_t set = 0;
    if (uset && copy_from_user(&set, (const void *)uset, sizeof(set)) < 0)
        return -EFAULT;
    if (uoset && user_check_w((void *)uoset, sizeof(uint32_t)) < 0)
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
    if (uoset && copy_to_user((void *)uoset, &old, sizeof(old)) < 0)
        return -EFAULT;
    return 0;
}

/* A selector a process may hold in DS, ES, FS or GS (or none). */
static int data_seg_ok(uint32_t sel)
{
    return sel == SEG_UDATA || sel == SEG_WDATA || sel == SEG_WFS || sel == 0;
}

static int sys_sigreturn(uint32_t uctx, uint32_t b, uint32_t c, uint32_t d, uint32_t e)
{
    (void)b; (void)c; (void)d; (void)e;
    struct flux_sigcontext sc;
    if (copy_from_user(&sc, (const void *)uctx, sizeof(sc)) < 0)
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

static int sys_mmap(uint32_t uargs, uint32_t b, uint32_t c, uint32_t d, uint32_t e)
{
    (void)b; (void)c; (void)d; (void)e;
    struct flux_mmap_args a;
    if (copy_from_user(&a, (const void *)uargs, sizeof(a)) < 0)
        return -EFAULT;
    if (!a.len)
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
         * must not wrap round to the start of the file. */
        if (a.len > 0xFFFFFFFFu - PAGE_SIZE || a.offset > 0xFFFFFFFFu - ALIGN_UP(a.len, PAGE_SIZE))
            return -EOVERFLOW;
    }

    uint32_t base = 0;
    int r = proc_mmap(current, &a, &base);
    if (r < 0)
        return r;
    if (!f)
        return (int)base;

    uint32_t len = ALIGN_UP(a.len, PAGE_SIZE);
    vfs_lock();
    for (uint32_t off = 0; off < len; off += PAGE_SIZE) {
        uint32_t foff = a.offset + off;
        if (foff < f->vn->size) {
            uint32_t n = MIN((uint32_t)PAGE_SIZE, f->vn->size - foff);
            int nr = vfs_read(f->vn, (void *)(base + off), foff, n);
            if (nr < 0) {
                vfs_unlock();
                proc_munmap(current, base, len);
                return nr;
            }
        }
        if (!(a.prot & PROT_WRITE))
            vmm_protect_page(current->pd, base + off, a.prot);
    }
    vfs_unlock();
    return (int)base;
}

static int sys_munmap(uint32_t addr, uint32_t len, uint32_t c, uint32_t d, uint32_t e)
{
    (void)c; (void)d; (void)e;
    return proc_munmap(current, addr, len);
}

static int sys_mprotect(uint32_t addr, uint32_t len, uint32_t prot, uint32_t d, uint32_t e)
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
 * File I/O that may reach a disk runs under the VFS lock, in chunks so
 * other processes get their turn during a long transfer. Terminals and
 * pipes block for unbounded time and never take the lock.
 */
#define IO_CHUNK (64 * 1024)

static int needs_vfs_lock(struct file *f)
{
    return f->type == FT_VNODE && f->vn->type != VN_CHR;
}

static int file_io(struct file *f, uint8_t *buf, uint32_t n, int write)
{
    if (!needs_vfs_lock(f))
        return write ? file_write(f, buf, n) : file_read(f, buf, n);
    uint32_t done = 0;
    while (done < n) {
        uint32_t chunk = MIN(n - done, (uint32_t)IO_CHUNK);
        vfs_lock();
        int r = write ? file_write(f, buf + done, chunk) : file_read(f, buf + done, chunk);
        vfs_unlock();
        if (r < 0)
            return done ? (int)done : r;
        done += (uint32_t)r;
        if ((uint32_t)r < chunk)
            break;
    }
    return (int)done;
}

static int sys_read(uint32_t fd, uint32_t buf, uint32_t n, uint32_t d, uint32_t e)
{
    (void)d; (void)e;
    struct file *f = get_file(fd);
    if (!f)
        return -EBADF;
    if (user_check_w((void *)buf, n) < 0)
        return -EFAULT;
    return file_io(f, (uint8_t *)buf, n, 0);
}

static int sys_write(uint32_t fd, uint32_t buf, uint32_t n, uint32_t d, uint32_t e)
{
    (void)d; (void)e;
    struct file *f = get_file(fd);
    if (!f)
        return -EBADF;
    if (user_check((void *)buf, n) < 0)
        return -EFAULT;
    return file_io(f, (uint8_t *)buf, n, 1);
}

static int sys_open(uint32_t upath, uint32_t flags, uint32_t mode, uint32_t d, uint32_t e)
{
    (void)d; (void)e;
    COPY_PATH(path, upath);
    struct vnode *vn;
    int r = vfs_resolve(path, current->cwd, &vn);

    if (r == -ENOENT && (flags & O_CREAT)) {
        struct vnode *dir;
        char name[NAME_MAX + 1];
        r = vfs_resolve_parent(path, current->cwd, &dir, name);
        if (r == 0) {
            r = vfs_create(dir, name, VN_FILE, &vn);
            if (r == 0 && vn) {
                uint32_t m = mode ? (mode & 07777) : 0666;
                vn->mode = (m & ~(current->umask & 0777)) | (m & 07000);
            }
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

    if (vn->type == VN_CHR && vn->dev_open && (r = vn->dev_open(vn, (int)flags)) < 0)
        return r;
    struct file *f = file_open(vn, (int)(flags & ~O_CLOEXEC));
    if (!f) {
        if (vn->type == VN_CHR && vn->dev_close)
            vn->dev_close(vn, (int)flags);
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
    struct file *f = get_file(fd);
    if (!f)
        return -EBADF;
    current->fds[fd] = NULL;
    fd_cloexec_set(current, (int)fd, 0);
    file_close(f);
    return 0;
}

static int sys_dup(uint32_t fd, uint32_t b, uint32_t c, uint32_t d, uint32_t e)
{
    (void)b; (void)c; (void)d; (void)e;
    struct file *f = get_file(fd);
    if (!f)
        return -EBADF;
    int nfd = alloc_fd(f);
    if (nfd >= 0)
        file_dup(f);
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
    struct file *prev = current->fds[newfd];
    current->fds[newfd] = file_dup(f);
    fd_cloexec_set(current, (int)newfd, 0);
    if (prev)
        file_close(prev);
    return (int)newfd;
}

static int sys_fcntl(uint32_t fd, uint32_t cmd, uint32_t arg, uint32_t d, uint32_t e)
{
    (void)d; (void)e;
    struct file *f = get_file(fd);
    if (!f)
        return -EBADF;
    switch (cmd) {
    case F_DUPFD:
    case F_DUPFD_CLOEXEC: {
        int nfd = alloc_fd_from(f, (int)arg, cmd == F_DUPFD_CLOEXEC);
        if (nfd >= 0)
            file_dup(f);
        return nfd;
    }
    case F_GETFD:
        return fd_cloexec_get(current, (int)fd) ? FD_CLOEXEC : 0;
    case F_SETFD:
        fd_cloexec_set(current, (int)fd, (arg & FD_CLOEXEC) != 0);
        return 0;
    case F_GETFL:
        return f->flags & ~O_CLOEXEC;
    case F_SETFL:
        f->flags = (f->flags & ~(O_APPEND | O_NONBLOCK)) | ((int)arg & (O_APPEND | O_NONBLOCK));
        return 0;
    default:
        return -EINVAL;
    }
}

static int sys_pipe(uint32_t ufds, uint32_t b, uint32_t c, uint32_t d, uint32_t e)
{
    (void)b; (void)c; (void)d; (void)e;
    if (user_check_w((void *)ufds, 2 * sizeof(int)) < 0)
        return -EFAULT;
    struct file *rd, *wr;
    int r = pipe_create(&rd, &wr);
    if (r < 0)
        return r;
    int fd0 = alloc_fd(rd);
    int fd1 = fd0 >= 0 ? alloc_fd(wr) : -EMFILE;
    int fds[2] = { fd0, fd1 };
    int r2 = fd1 < 0 ? -EMFILE : copy_to_user((void *)ufds, fds, sizeof(fds));
    if (r2 < 0) {
        if (fd0 >= 0)
            current->fds[fd0] = NULL;
        if (fd1 >= 0)
            current->fds[fd1] = NULL;
        file_close(rd);
        file_close(wr);
        return r2;
    }
    return 0;
}

static int sys_openpty(uint32_t ufds, uint32_t b, uint32_t c, uint32_t d, uint32_t e)
{
    (void)b; (void)c; (void)d; (void)e;
    if (user_check_w((void *)ufds, 2 * sizeof(int)) < 0)
        return -EFAULT;
    struct file *master, *slave;
    int r = pty_create(&master, &slave);
    if (r < 0)
        return r;
    int fd0 = alloc_fd(master);
    int fd1 = fd0 >= 0 ? alloc_fd(slave) : -EMFILE;
    int fds[2] = { fd0, fd1 };
    int r2 = fd1 < 0 ? -EMFILE : copy_to_user((void *)ufds, fds, sizeof(fds));
    if (r2 < 0) {
        if (fd0 >= 0)
            current->fds[fd0] = NULL;
        if (fd1 >= 0)
            current->fds[fd1] = NULL;
        file_close(master);
        file_close(slave);
        return r2;
    }
    return 0;
}

#define POLL_MAX 64

static int sys_poll(uint32_t ufds, uint32_t nfds, uint32_t timeout, uint32_t d, uint32_t e)
{
    (void)d; (void)e;
    if (nfds > POLL_MAX)
        return -EINVAL;
    /* The kernel's copy of the table: the caller's memory is checked up
     * front, so that the results can be written back after the wait. */
    struct pollfd fds[POLL_MAX];
    uint32_t bytes = nfds * sizeof(fds[0]);
    if (user_check_w((void *)ufds, bytes) < 0 ||
        (bytes && copy_from_user(fds, (const void *)ufds, bytes) < 0))
        return -EFAULT;
    int r = do_poll(fds, nfds, (int)timeout);
    if (bytes && copy_to_user((void *)ufds, fds, bytes) < 0)
        return -EFAULT;
    return r;
}

/*
 * The new position of f, moved by the signed offset off. Positions are
 * 64-bit: a disk's device node goes past 4 GiB, and a file up to 4 GiB.
 */
static int seek(struct file *f, int64_t off, uint32_t whence, uint64_t *out)
{
    if (f->type == FT_PIPE || f->type == FT_SOCKET)
        return -ESPIPE;
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

/* The 32-bit lseek: a position past 2 GiB cannot be its result. */
static int sys_lseek(uint32_t fd, uint32_t off, uint32_t whence, uint32_t d, uint32_t e)
{
    (void)d; (void)e;
    struct file *f = get_file(fd);
    if (!f)
        return -EBADF;
    uint64_t pos;
    int r = seek(f, (int32_t)off, whence, &pos);
    if (r < 0)
        return r;
    if (r == 0) {
        if (pos > INT32_MAX)
            return -EOVERFLOW;
        f->off = pos;
    }
    return (int)pos;
}

/* lseek with a 64-bit offset (high, low) and result (written to *uresult). */
static int sys_llseek(uint32_t fd, uint32_t hi, uint32_t lo, uint32_t uresult, uint32_t whence)
{
    struct file *f = get_file(fd);
    if (!f)
        return -EBADF;
    if (user_check_w((void *)uresult, sizeof(uint64_t)) < 0)
        return -EFAULT;
    uint64_t pos;
    int r = seek(f, (int64_t)((uint64_t)hi << 32 | lo), whence, &pos);
    if (r < 0)
        return r;
    if (r == 0)
        f->off = pos;
    return copy_to_user((void *)uresult, &pos, sizeof(pos));
}

static int sys_stat(uint32_t upath, uint32_t ust, uint32_t c, uint32_t d, uint32_t e)
{
    (void)c; (void)d; (void)e;
    COPY_PATH(path, upath);
    if (user_check_w((void *)ust, sizeof(struct stat)) < 0)
        return -EFAULT;
    struct vnode *vn;
    int r = vfs_resolve(path, current->cwd, &vn);
    if (r < 0)
        return r;
    struct stat st;
    memset(&st, 0, sizeof(st));
    vfs_stat(vn, &st);
    return copy_to_user((void *)ust, &st, sizeof(st));
}

static int sys_fstat(uint32_t fd, uint32_t ust, uint32_t c, uint32_t d, uint32_t e)
{
    (void)c; (void)d; (void)e;
    struct file *f = get_file(fd);
    if (!f)
        return -EBADF;
    if (user_check_w((void *)ust, sizeof(struct stat)) < 0)
        return -EFAULT;
    struct stat st;
    memset(&st, 0, sizeof(st));
    if (f->type == FT_SOCKET) {
        st.st_mode = S_IFSOCK | 0777;
        st.st_nlink = 1;
    } else if (f->type == FT_PIPE) {
        st.st_mode = S_IFIFO | 0600;
        st.st_size = f->pipe->wpos - f->pipe->rpos;
        st.st_nlink = 1;
    } else {
        vfs_stat(f->vn, &st);
    }
    return copy_to_user((void *)ust, &st, sizeof(st));
}

static int sys_chdir(uint32_t upath, uint32_t b, uint32_t c, uint32_t d, uint32_t e)
{
    (void)b; (void)c; (void)d; (void)e;
    COPY_PATH(path, upath);
    struct vnode *vn;
    int r = vfs_resolve(path, current->cwd, &vn);
    if (r < 0)
        return r;
    if (vn->type != VN_DIR)
        return -ENOTDIR;
    vfs_ref(vn);
    vfs_unref(current->cwd);
    current->cwd = vn;
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
    vfs_unref(current->cwd);
    current->cwd = vn;
    return 0;
}

static int sys_getcwd(uint32_t ubuf, uint32_t size, uint32_t c, uint32_t d, uint32_t e)
{
    (void)c; (void)d; (void)e;
    char path[PATH_MAX];
    int r = vfs_getpath(current->cwd, path, sizeof(path));
    if (r < 0)
        return r;
    uint32_t len = strlen(path) + 1;
    if (len > size)
        return -ERANGE;
    if (copy_to_user((void *)ubuf, path, len) < 0)
        return -EFAULT;
    return (int)len;
}

static int sys_getdents(uint32_t fd, uint32_t ubuf, uint32_t count, uint32_t d, uint32_t e)
{
    (void)d; (void)e;
    struct file *f = get_file(fd);
    if (!f)
        return -EBADF;
    if (f->type != FT_VNODE || f->vn->type != VN_DIR)
        return -ENOTDIR;
    if (user_check_w((void *)ubuf, count) < 0)
        return -EFAULT;
    uint32_t used = 0;
    struct dirent de;
    while (count - used >= sizeof(de) && f->off < UINT32_MAX && vfs_dirent(f->vn, (uint32_t)f->off, &de)) {
        if (copy_to_user((uint8_t *)ubuf + used, &de, sizeof(de)) < 0)
            return used ? (int)used : -EFAULT;
        used += sizeof(de);
        f->off++;
    }
    return (int)used;
}

static int sys_mkdir(uint32_t upath, uint32_t mode, uint32_t c, uint32_t d, uint32_t e)
{
    (void)c; (void)d; (void)e;
    COPY_PATH(path, upath);
    int r = vfs_mkdir(path, current->cwd);
    if (r == 0) {
        struct vnode *vn;
        if (vfs_resolve(path, current->cwd, &vn) == 0 && vn) {
            uint32_t m = mode ? (mode & 07777) : 0777;
            vn->mode = (m & ~(current->umask & 0777)) | (m & 07000);
        }
    }
    return r;
}

static int sys_rmdir(uint32_t upath, uint32_t b, uint32_t c, uint32_t d, uint32_t e)
{
    (void)b; (void)c; (void)d; (void)e;
    COPY_PATH(path, upath);
    return vfs_rmdir(path, current->cwd);
}

static int sys_unlink(uint32_t upath, uint32_t b, uint32_t c, uint32_t d, uint32_t e)
{
    (void)b; (void)c; (void)d; (void)e;
    COPY_PATH(path, upath);
    return vfs_unlink(path, current->cwd);
}

static int sys_rename(uint32_t ufrom, uint32_t uto, uint32_t c, uint32_t d, uint32_t e)
{
    (void)c; (void)d; (void)e;
    COPY_PATH(from, ufrom);
    COPY_PATH(to, uto);
    return vfs_rename(from, to, current->cwd);
}

static int sys_chmod(uint32_t upath, uint32_t mode, uint32_t c, uint32_t d, uint32_t e)
{
    (void)c; (void)d; (void)e;
    COPY_PATH(path, upath);
    struct vnode *vn;
    int r = vfs_resolve(path, current->cwd, &vn);
    if (r < 0)
        return r;
    if (vn->fs && vn->fs->readonly)
        return -EROFS;
    vn->mode = mode & 07777;
    return 0;
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
    return 0;
}

static int sys_umask(uint32_t mask, uint32_t b, uint32_t c, uint32_t d, uint32_t e)
{
    (void)b; (void)c; (void)d; (void)e;
    uint32_t old = current->umask;
    current->umask = mask & 0777;
    return (int)old;
}

static int sys_utime(uint32_t upath, uint32_t utimes, uint32_t c, uint32_t d, uint32_t e)
{
    (void)c; (void)d; (void)e;
    COPY_PATH(path, upath);
    uint32_t mtime = time_now();
    if (utimes) {
        struct flux_utimbuf tb;
        if (copy_from_user(&tb, (const void *)utimes, sizeof(tb)) < 0)
            return -EFAULT;
        mtime = tb.modtime;
    }
    return vfs_utime(path, current->cwd, mtime);
}

static int sys_times(uint32_t ubuf, uint32_t b, uint32_t c, uint32_t d, uint32_t e)
{
    (void)b; (void)c; (void)d; (void)e;
    if (ubuf) {
        struct flux_tms tms = {
            .tms_utime = current->cpu_ticks,
            .tms_stime = 0,
            .tms_cutime = current->child_ticks,
            .tms_cstime = 0,
        };
        if (copy_to_user((void *)ubuf, &tms, sizeof(tms)) < 0)
            return -EFAULT;
    }
    return (int)ticks;
}

/* ---- sockets ---- */

static int get_socket(uint32_t fd, struct socket **out)
{
    struct file *f = get_file(fd);
    if (!f)
        return -EBADF;
    if (f->type != FT_SOCKET)
        return -ENOTSOCK;
    *out = f->sock;
    return 0;
}

/* Socket addresses are always a struct sockaddr_in; len is only checked. */
static int copy_sockaddr(uint32_t uaddr, uint32_t len, struct sockaddr_in *out)
{
    if (len < sizeof(*out))
        return -EINVAL;
    return copy_from_user(out, (const void *)uaddr, sizeof(*out)) < 0 ? -EFAULT : 0;
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

static int sys_socket(uint32_t domain, uint32_t type, uint32_t proto, uint32_t d, uint32_t e)
{
    (void)d; (void)e;
    struct socket *s;
    int r = sock_create((int)domain, (int)type, (int)proto, &s);
    return r < 0 ? r : install_socket(s);
}

static int sys_bind(uint32_t fd, uint32_t uaddr, uint32_t len, uint32_t d, uint32_t e)
{
    (void)d; (void)e;
    struct socket *s;
    struct sockaddr_in sin;
    int r = get_socket(fd, &s);
    if (r == 0)
        r = copy_sockaddr(uaddr, len, &sin);
    return r < 0 ? r : sock_bind(s, &sin);
}

static int sys_connect(uint32_t fd, uint32_t uaddr, uint32_t len, uint32_t d, uint32_t e)
{
    (void)d; (void)e;
    struct socket *s;
    struct sockaddr_in sin;
    int r = get_socket(fd, &s);
    if (r == 0)
        r = copy_sockaddr(uaddr, len, &sin);
    return r < 0 ? r : sock_connect(s, &sin);
}

static int sys_listen(uint32_t fd, uint32_t backlog, uint32_t c, uint32_t d, uint32_t e)
{
    (void)c; (void)d; (void)e;
    struct socket *s;
    int r = get_socket(fd, &s);
    return r < 0 ? r : sock_listen(s, (int)backlog);
}

static int sys_accept(uint32_t fd, uint32_t uaddr, uint32_t c, uint32_t d, uint32_t e)
{
    (void)c; (void)d; (void)e;
    struct socket *s, *conn;
    struct sockaddr_in peer;
    int r = get_socket(fd, &s);
    if (r < 0)
        return r;
    if (uaddr && user_check_w((void *)uaddr, sizeof(peer)) < 0)
        return -EFAULT;
    r = sock_accept(s, &conn, &peer);
    if (r < 0)
        return r;
    r = install_socket(conn);
    if (r >= 0 && uaddr)
        copy_to_user((void *)uaddr, &peer, sizeof(peer));  /* checked above; the connection is the caller's now */
    return r;
}

static int sys_sendto(uint32_t fd, uint32_t buf, uint32_t n, uint32_t flags, uint32_t uaddr)
{
    struct socket *s;
    struct sockaddr_in sin;
    int r = get_socket(fd, &s);
    if (r < 0)
        return r;
    if (user_check((void *)buf, n) < 0)
        return -EFAULT;
    if (uaddr && (r = copy_sockaddr(uaddr, sizeof(sin), &sin)) < 0)
        return r;
    int sflags = (int)flags | ((get_file(fd)->flags & O_NONBLOCK) ? MSG_DONTWAIT : 0);
    return sock_sendto(s, (const void *)buf, n, sflags, uaddr ? &sin : NULL);
}

static int sys_recvfrom(uint32_t fd, uint32_t buf, uint32_t n, uint32_t flags, uint32_t uaddr)
{
    struct socket *s;
    struct sockaddr_in sin;
    int r = get_socket(fd, &s);
    if (r < 0)
        return r;
    if (user_check_w((void *)buf, n) < 0 || (uaddr && user_check_w((void *)uaddr, sizeof(sin)) < 0))
        return -EFAULT;
    int sflags = (int)flags | ((get_file(fd)->flags & O_NONBLOCK) ? MSG_DONTWAIT : 0);
    r = sock_recvfrom(s, (void *)buf, n, sflags, uaddr ? &sin : NULL);
    if (r >= 0 && uaddr)
        copy_to_user((void *)uaddr, &sin, sizeof(sin));    /* checked above; the data is already taken */
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
static int sys_sockname(uint32_t fd, uint32_t uaddr, uint32_t peer, uint32_t d, uint32_t e)
{
    (void)d; (void)e;
    struct socket *s;
    struct sockaddr_in sin;
    int r = get_socket(fd, &s);
    if (r < 0)
        return r;
    if (user_check_w((void *)uaddr, sizeof(sin)) < 0)
        return -EFAULT;
    r = sock_name(s, (int)peer, &sin);
    if (r == 0)
        r = copy_to_user((void *)uaddr, &sin, sizeof(sin));
    return r;
}

static int sys_netinfo(uint32_t ubuf, uint32_t b, uint32_t c, uint32_t d, uint32_t e)
{
    (void)b; (void)c; (void)d; (void)e;
    struct netinfo ni;
    net_get_info(&ni);
    return copy_to_user((void *)ubuf, &ni, sizeof(ni));
}

static int sys_netconf(uint32_t ubuf, uint32_t b, uint32_t c, uint32_t d, uint32_t e)
{
    (void)b; (void)c; (void)d; (void)e;
    struct netinfo ni;
    if (copy_from_user(&ni, (const void *)ubuf, sizeof(ni)) < 0)
        return -EFAULT;
    return net_configure(&ni);
}

/* ---- filesystems ---- */

static int sys_mount(uint32_t usrc, uint32_t utarget, uint32_t utype, uint32_t d, uint32_t e)
{
    (void)d; (void)e;
    COPY_PATH(src, usrc);
    COPY_PATH(target, utarget);
    char type[16] = "auto";
    if (utype && copy_str(type, (const char *)utype, sizeof(type)) < 0)
        return -EINVAL;
    return vfs_mount(src, target, type, current->cwd);
}

static int sys_umount(uint32_t utarget, uint32_t b, uint32_t c, uint32_t d, uint32_t e)
{
    (void)b; (void)c; (void)d; (void)e;
    COPY_PATH(target, utarget);
    return vfs_umount(target, current->cwd);
}

static int sys_statfs(uint32_t upath, uint32_t ubuf, uint32_t c, uint32_t d, uint32_t e)
{
    (void)c; (void)d; (void)e;
    COPY_PATH(path, upath);
    if (user_check_w((void *)ubuf, sizeof(struct statfs)) < 0)
        return -EFAULT;
    struct vnode *vn;
    int r = vfs_resolve(path, current->cwd, &vn);
    if (r < 0)
        return r;
    struct statfs st;
    r = vfs_statfs(vn, &st);
    return r < 0 ? r : copy_to_user((void *)ubuf, &st, sizeof(st));
}

static int sys_getmounts(uint32_t ubuf, uint32_t size, uint32_t c, uint32_t d, uint32_t e)
{
    (void)c; (void)d; (void)e;
    size = MIN(size, 8192u);
    if (user_check_w((void *)ubuf, size) < 0)
        return -EFAULT;
    char *tmp = kmalloc(size + 1);
    if (!tmp)
        return -ENOMEM;
    int n = vfs_mounts_text(tmp, size + 1);
    n = MIN(n, (int)size);
    if (copy_to_user((void *)ubuf, tmp, (uint32_t)n) < 0)
        n = -EFAULT;
    kfree(tmp);
    return n;
}

static int sys_blkinfo(uint32_t idx, uint32_t ubuf, uint32_t c, uint32_t d, uint32_t e)
{
    (void)c; (void)d; (void)e;
    if (user_check_w((void *)ubuf, sizeof(struct blkinfo)) < 0)
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
    return copy_to_user((void *)ubuf, &bi, sizeof(bi));
}

static int sys_usbinfo(uint32_t idx, uint32_t ubuf, uint32_t c, uint32_t d, uint32_t e)
{
    (void)c; (void)d; (void)e;
    if (user_check_w((void *)ubuf, sizeof(struct usbinfo)) < 0)
        return -EFAULT;
    struct usbinfo ui;
    int r = usb_info((int)idx, &ui);
    return r < 0 ? r : copy_to_user((void *)ubuf, &ui, sizeof(ui));
}

static int sys_power(uint32_t op, uint32_t arg, uint32_t c, uint32_t d, uint32_t e)
{
    (void)c; (void)d; (void)e;
    if (op == POWER_INFO && user_check_w((void *)arg, sizeof(struct powerinfo)) < 0)
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

static int sys_uname(uint32_t ubuf, uint32_t b, uint32_t c, uint32_t d, uint32_t e)
{
    (void)b; (void)c; (void)d; (void)e;
    struct utsname u;
    memset(&u, 0, sizeof(u));
    strcpy(u.sysname, FLUXOS_NAME);
    strcpy(u.nodename, "fluxOS");
    strcpy(u.release, FLUXOS_RELEASE);
    strcpy(u.version, "#1 PREEMPT " __DATE__ " " __TIME__);
    strcpy(u.machine, "i686");
    return copy_to_user((void *)ubuf, &u, sizeof(u));
}

static int sys_procinfo(uint32_t slot, uint32_t ubuf, uint32_t c, uint32_t d, uint32_t e)
{
    (void)c; (void)d; (void)e;
    if (user_check_w((void *)ubuf, sizeof(struct procinfo)) < 0)
        return -EFAULT;
    struct procinfo pi;
    int r = proc_info((int)slot, &pi);
    if (r == 1 && copy_to_user((void *)ubuf, &pi, sizeof(pi)) < 0)
        return -EFAULT;
    return r;
}

static int sys_meminfo(uint32_t ubuf, uint32_t b, uint32_t c, uint32_t d, uint32_t e)
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
    return copy_to_user((void *)ubuf, &mi, sizeof(mi));
}

/* Re-read the partition table of a whole disk (like Linux's BLKRRPART). */
static int sys_blkrescan(uint32_t upath, uint32_t b, uint32_t c, uint32_t d, uint32_t e)
{
    (void)b; (void)c; (void)d; (void)e;
    COPY_PATH(path, upath);
    struct vnode *vn;
    int r = vfs_resolve(path, current->cwd, &vn);
    if (r < 0)
        return r;
    struct blockdev *bd = blk_by_vnode(vn);
    return bd ? blk_rescan(bd) : -ENOTBLK;
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

static int sys_ftruncate(uint32_t fd, uint32_t size, uint32_t c, uint32_t d, uint32_t e)
{
    (void)c; (void)d; (void)e;
    if ((int32_t)size < 0)
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

static int sys_fbinfo(uint32_t ubuf, uint32_t b, uint32_t c, uint32_t d, uint32_t e)
{
    (void)b; (void)c; (void)d; (void)e;
    if (user_check_w((void *)ubuf, sizeof(struct fbinfo)) < 0)
        return -EFAULT;
    struct fbinfo fi;
    int r = fb_get_info(&fi);
    return r < 0 ? r : copy_to_user((void *)ubuf, &fi, sizeof(fi));
}

static int sys_fbmap(uint32_t a, uint32_t b, uint32_t c, uint32_t d, uint32_t e)
{
    (void)a; (void)b; (void)c; (void)d; (void)e;
    return fb_map_user();
}

int user_access_ok(const void *ptr, uint32_t len)
{
    return user_check_w((void *)ptr, len) == 0;
}

/* Device control: the device checks the request and its argument. */
static int sys_ioctl(uint32_t fd, uint32_t req, uint32_t arg, uint32_t d, uint32_t e)
{
    (void)d; (void)e;
    struct file *f = get_file(fd);
    if (!f)
        return -EBADF;
    if (f->type != FT_VNODE || f->vn->type != VN_CHR || !f->vn->dev_ioctl)
        return -ENOTTY;
    return f->vn->dev_ioctl(f->vn, req, arg);
}

static int sys_display(uint32_t op, uint32_t uarg, uint32_t c, uint32_t d, uint32_t e)
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
    case DISP_MAPBUF: size = sizeof(struct dispbuf); writes = 1; break;
    case DISP_UNMAPBUF: size = sizeof(struct dispbuf); break;
    case DISP_BLIT: size = sizeof(struct dispblit); break;
    case DISP_SUBMIT: size = sizeof(struct dispsubmit); break;
    default: return -EINVAL;
    }
    if ((writes ? user_check_w((void *)uarg, size) : user_check((const void *)uarg, size)) < 0)
        return -EFAULT;
    return fb_display((int)op, (void *)uarg);
}

static int sys_reboot(uint32_t cmd, uint32_t b, uint32_t c, uint32_t d, uint32_t e)
{
    (void)b; (void)c; (void)d; (void)e;
    if (cmd == REBOOT_RESTART || cmd == REBOOT_POWEROFF || cmd == REBOOT_HALT) {
        vfs_lock();
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

static int sys_dmesg(uint32_t ubuf, uint32_t size, uint32_t c, uint32_t d, uint32_t e)
{
    (void)c; (void)d; (void)e;
    size = MIN(size, (uint32_t)KLOG_SIZE);
    if (user_check_w((void *)ubuf, size) < 0)
        return -EFAULT;
    char *tmp = kmalloc(size ? size : 1);   /* the log is read holding its lock */
    if (!tmp)
        return -ENOMEM;
    int n = klog_read(tmp, size);
    if (copy_to_user((void *)ubuf, tmp, (uint32_t)n) < 0)
        n = -EFAULT;
    kfree(tmp);
    return n;
}

static const syscall_fn syscalls[NSYSCALLS] = {
    [SYS_exit] = sys_exit,
    [SYS_fork] = sys_fork,
    [SYS_read] = sys_read,
    [SYS_write] = sys_write,
    [SYS_open] = sys_open,
    [SYS_close] = sys_close,
    [SYS_waitpid] = sys_waitpid,
    [SYS_execve] = sys_execve,
    [SYS_chdir] = sys_chdir,
    [SYS_time] = sys_time,
    [SYS_getpid] = sys_getpid,
    [SYS_getppid] = sys_getppid,
    [SYS_kill] = sys_kill,
    [SYS_mkdir] = sys_mkdir,
    [SYS_rmdir] = sys_rmdir,
    [SYS_unlink] = sys_unlink,
    [SYS_dup] = sys_dup,
    [SYS_dup2] = sys_dup2,
    [SYS_pipe] = sys_pipe,
    [SYS_sbrk] = sys_sbrk,
    [SYS_sleep] = sys_sleep,
    [SYS_lseek] = sys_lseek,
    [SYS_stat] = sys_stat,
    [SYS_fstat] = sys_fstat,
    [SYS_getcwd] = sys_getcwd,
    [SYS_getdents] = sys_getdents,
    [SYS_uname] = sys_uname,
    [SYS_yield] = sys_yield,
    [SYS_signal] = sys_signal,
    [SYS_setpgid] = sys_setpgid,
    [SYS_getpgid] = sys_getpgid,
    [SYS_tcsetpgrp] = sys_tcsetpgrp,
    [SYS_tcgetpgrp] = sys_tcgetpgrp,
    [SYS_ttymode] = sys_ttymode,
    [SYS_procinfo] = sys_procinfo,
    [SYS_meminfo] = sys_meminfo,
    [SYS_uptime] = sys_uptime,
    [SYS_reboot] = sys_reboot,
    [SYS_rename] = sys_rename,
    [SYS_dmesg] = sys_dmesg,
    [SYS_chmod] = sys_chmod,
    [SYS_mount] = sys_mount,
    [SYS_umount] = sys_umount,
    [SYS_statfs] = sys_statfs,
    [SYS_getmounts] = sys_getmounts,
    [SYS_blkinfo] = sys_blkinfo,
    [SYS_sync] = sys_sync,
    [SYS_fsync] = sys_fsync,
    [SYS_blkrescan] = sys_blkrescan,
    [SYS_fbinfo] = sys_fbinfo,
    [SYS_fbmap] = sys_fbmap,
    [SYS_openpty] = sys_openpty,
    [SYS_poll] = sys_poll,
    [SYS_socket] = sys_socket,
    [SYS_bind] = sys_bind,
    [SYS_connect] = sys_connect,
    [SYS_listen] = sys_listen,
    [SYS_accept] = sys_accept,
    [SYS_sendto] = sys_sendto,
    [SYS_recvfrom] = sys_recvfrom,
    [SYS_shutdown] = sys_shutdown,
    [SYS_sockname] = sys_sockname,
    [SYS_netinfo] = sys_netinfo,
    [SYS_netconf] = sys_netconf,
    [SYS_ftruncate] = sys_ftruncate,
    [SYS_display] = sys_display,
    [SYS_ioctl] = sys_ioctl,
    [SYS_usbinfo] = sys_usbinfo,
    [SYS_power] = sys_power,
    [SYS_stime] = sys_stime,
    [SYS_llseek] = sys_llseek,
    [SYS_sigaction] = sys_sigaction,
    [SYS_sigprocmask] = sys_sigprocmask,
    [SYS_sigreturn] = sys_sigreturn,
    [SYS_mmap] = sys_mmap,
    [SYS_munmap] = sys_munmap,
    [SYS_mprotect] = sys_mprotect,
    [SYS_fcntl] = sys_fcntl,
    [SYS_umask] = sys_umask,
    [SYS_utime] = sys_utime,
    [SYS_times] = sys_times,
    [SYS_fchdir] = sys_fchdir,
    [SYS_fchmod] = sys_fchmod,
};

/* VFS operations and shared file offsets; read and write lock per chunk. */
static const uint8_t takes_vfs_lock[NSYSCALLS] = {
    [SYS_open] = 1, [SYS_execve] = 1, [SYS_chdir] = 1, [SYS_getcwd] = 1,
    [SYS_getdents] = 1, [SYS_mkdir] = 1, [SYS_rmdir] = 1, [SYS_unlink] = 1,
    [SYS_rename] = 1, [SYS_chmod] = 1, [SYS_stat] = 1, [SYS_mount] = 1,
    [SYS_umount] = 1, [SYS_statfs] = 1, [SYS_getmounts] = 1, [SYS_blkinfo] = 1,
    [SYS_sync] = 1, [SYS_fsync] = 1, [SYS_blkrescan] = 1, [SYS_ftruncate] = 1,
    [SYS_lseek] = 1, [SYS_llseek] = 1, [SYS_utime] = 1, [SYS_fchdir] = 1, [SYS_fchmod] = 1,
};

void syscall_dispatch(struct trapframe *tf)
{
    uint32_t n = tf->eax;
    if (n >= NSYSCALLS || !syscalls[n]) {
        tf->eax = (uint32_t)-ENOSYS;
        return;
    }
    current->in_syscall = (int)n;
    current->syscall_orig_eax = n;
    if (takes_vfs_lock[n])
        vfs_lock();
    tf->eax = (uint32_t)syscalls[n](tf->ebx, tf->ecx, tf->edx, tf->esi, tf->edi);
    if (takes_vfs_lock[n])
        vfs_unlock();
}

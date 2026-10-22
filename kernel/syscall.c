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

static int alloc_fd(struct file *f)
{
    for (int i = 0; i < MAX_FDS; i++) {
        if (!current->fds[i]) {
            current->fds[i] = f;
            return i;
        }
    }
    return -EMFILE;
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

#define EXEC_MAX_ARGS 256
#define EXEC_POOL     (64 * 1024)

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
    kfree(argv);
    kfree(pool);
    return r < 0 ? r : 0;
}

static int sys_waitpid(uint32_t pid, uint32_t ustatus, uint32_t options, uint32_t d, uint32_t e)
{
    (void)d; (void)e;
    if (ustatus && user_check((void *)ustatus, sizeof(int)) < 0)
        return -EFAULT;
    int status = 0;
    int r = do_waitpid((int)pid, &status, (int)options);
    if (r > 0 && ustatus)
        *(int *)ustatus = status;
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
        if (nbrk < old || nbrk > USER_STACK_LIMIT - PAGE_SIZE)
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
    return sleep_ticks(t ? t : 1);
}

static int sys_yield(uint32_t a, uint32_t b, uint32_t c, uint32_t d, uint32_t e)
{
    (void)a; (void)b; (void)c; (void)d; (void)e;
    need_resched = 1;
    return 0;
}

static int sys_signal(uint32_t sig, uint32_t disp, uint32_t c, uint32_t d, uint32_t e)
{
    (void)c; (void)d; (void)e;
    if (sig == 0 || sig >= NSIG || sig == SIGKILL)
        return -EINVAL;
    uint32_t bit = 1u << sig;
    int old = (current->sig_ignored & bit) ? SIGDISP_IGN : SIGDISP_DFL;
    if (disp == SIGDISP_IGN) {
        current->sig_ignored |= bit;
        current->sig_pending &= ~bit;
    } else {
        current->sig_ignored &= ~bit;
    }
    return old;
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
    tty_set_fg((int)pgid);
    return 0;
}

static int sys_tcgetpgrp(uint32_t a, uint32_t b, uint32_t c, uint32_t d, uint32_t e)
{
    (void)a; (void)b; (void)c; (void)d; (void)e;
    return tty_get_fg();
}

static int sys_ttymode(uint32_t mode, uint32_t b, uint32_t c, uint32_t d, uint32_t e)
{
    (void)b; (void)c; (void)d; (void)e;
    return tty_set_mode((int)mode);
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
    if (user_check((void *)buf, n) < 0)
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

static int sys_open(uint32_t upath, uint32_t flags, uint32_t c, uint32_t d, uint32_t e)
{
    (void)c; (void)d; (void)e;
    COPY_PATH(path, upath);
    struct vnode *vn;
    int r = vfs_resolve(path, current->cwd, &vn);

    if (r == -ENOENT && (flags & O_CREAT)) {
        struct vnode *dir;
        char name[NAME_MAX + 1];
        r = vfs_resolve_parent(path, current->cwd, &dir, name);
        if (r == 0)
            r = vfs_create(dir, name, VN_FILE, &vn);
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
    if ((flags & O_TRUNC) && writable && vn->type == VN_FILE)
        vfs_truncate(vn, 0);

    struct file *f = file_open(vn, (int)flags);
    if (!f)
        return -ENOMEM;
    int fd = alloc_fd(f);
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
    if (current->fds[newfd])
        file_close(current->fds[newfd]);
    current->fds[newfd] = file_dup(f);
    return (int)newfd;
}

static int sys_pipe(uint32_t ufds, uint32_t b, uint32_t c, uint32_t d, uint32_t e)
{
    (void)b; (void)c; (void)d; (void)e;
    if (user_check((void *)ufds, 2 * sizeof(int)) < 0)
        return -EFAULT;
    struct file *rd, *wr;
    int r = pipe_create(&rd, &wr);
    if (r < 0)
        return r;
    int fd0 = alloc_fd(rd);
    int fd1 = fd0 >= 0 ? alloc_fd(wr) : -EMFILE;
    if (fd1 < 0) {
        if (fd0 >= 0)
            current->fds[fd0] = NULL;
        file_close(rd);
        file_close(wr);
        return -EMFILE;
    }
    ((int *)ufds)[0] = fd0;
    ((int *)ufds)[1] = fd1;
    return 0;
}

static int sys_lseek(uint32_t fd, uint32_t off, uint32_t whence, uint32_t d, uint32_t e)
{
    (void)d; (void)e;
    struct file *f = get_file(fd);
    if (!f)
        return -EBADF;
    if (f->type == FT_PIPE)
        return -ESPIPE;
    if (f->vn->type == VN_CHR)
        return 0;

    int32_t base;
    switch (whence) {
    case SEEK_SET: base = 0; break;
    case SEEK_CUR: base = (int32_t)f->off; break;
    case SEEK_END: base = (int32_t)f->vn->size; break;
    default: return -EINVAL;
    }
    int32_t pos = base + (int32_t)off;
    if (pos < 0)
        return -EINVAL;
    f->off = (uint32_t)pos;
    return pos;
}

static int sys_stat(uint32_t upath, uint32_t ust, uint32_t c, uint32_t d, uint32_t e)
{
    (void)c; (void)d; (void)e;
    COPY_PATH(path, upath);
    if (user_check((void *)ust, sizeof(struct stat)) < 0)
        return -EFAULT;
    struct vnode *vn;
    int r = vfs_resolve(path, current->cwd, &vn);
    if (r < 0)
        return r;
    vfs_stat(vn, (struct stat *)ust);
    return 0;
}

static int sys_fstat(uint32_t fd, uint32_t ust, uint32_t c, uint32_t d, uint32_t e)
{
    (void)c; (void)d; (void)e;
    struct file *f = get_file(fd);
    if (!f)
        return -EBADF;
    if (user_check((void *)ust, sizeof(struct stat)) < 0)
        return -EFAULT;
    struct stat *st = (struct stat *)ust;
    if (f->type == FT_PIPE) {
        memset(st, 0, sizeof(*st));
        st->st_mode = S_IFIFO | 0600;
        st->st_size = f->pipe->wpos - f->pipe->rpos;
        st->st_nlink = 1;
        return 0;
    }
    vfs_stat(f->vn, st);
    return 0;
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
    if (user_check((void *)ubuf, len) < 0)
        return -EFAULT;
    memcpy((void *)ubuf, path, len);
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
    if (user_check((void *)ubuf, count) < 0)
        return -EFAULT;
    uint32_t used = 0;
    struct dirent de;
    while (count - used >= sizeof(de) && vfs_dirent(f->vn, f->off, &de)) {
        memcpy((uint8_t *)ubuf + used, &de, sizeof(de));
        used += sizeof(de);
        f->off++;
    }
    return (int)used;
}

static int sys_mkdir(uint32_t upath, uint32_t b, uint32_t c, uint32_t d, uint32_t e)
{
    (void)b; (void)c; (void)d; (void)e;
    COPY_PATH(path, upath);
    return vfs_mkdir(path, current->cwd);
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
    if (user_check((void *)ubuf, sizeof(struct statfs)) < 0)
        return -EFAULT;
    struct vnode *vn;
    int r = vfs_resolve(path, current->cwd, &vn);
    if (r < 0)
        return r;
    return vfs_statfs(vn, (struct statfs *)ubuf);
}

static int sys_getmounts(uint32_t ubuf, uint32_t size, uint32_t c, uint32_t d, uint32_t e)
{
    (void)c; (void)d; (void)e;
    size = MIN(size, 8192u);
    if (user_check((void *)ubuf, size) < 0)
        return -EFAULT;
    char *tmp = kmalloc(size + 1);
    if (!tmp)
        return -ENOMEM;
    int n = vfs_mounts_text(tmp, size + 1);
    n = MIN(n, (int)size);
    memcpy((void *)ubuf, tmp, (uint32_t)n);
    kfree(tmp);
    return n;
}

static int sys_blkinfo(uint32_t idx, uint32_t ubuf, uint32_t c, uint32_t d, uint32_t e)
{
    (void)c; (void)d; (void)e;
    if (user_check((void *)ubuf, sizeof(struct blkinfo)) < 0)
        return -EFAULT;
    struct blockdev *bd = blk_get((int)idx);
    if (!bd)
        return -ENODEV;
    struct blkinfo *bi = (struct blkinfo *)ubuf;
    memset(bi, 0, sizeof(*bi));
    if (bd->refresh)
        bd->refresh(bd);
    strlcpy(bi->name, bd->name, sizeof(bi->name));
    strlcpy(bi->model, bd->model, sizeof(bi->model));
    bi->type = (uint32_t)bd->type;
    bi->sector_size = bd->sector_size;
    bi->sectors = bd->nsectors;
    if (bd->fs) {
        strlcpy(bi->fstype, bd->fs->ops->name, sizeof(bi->fstype));
        vfs_mountpoint_of(bd, bi->mountpoint, sizeof(bi->mountpoint));
    }
    return 0;
}

/* ---- system information ---- */

static int sys_time(uint32_t a, uint32_t b, uint32_t c, uint32_t d, uint32_t e)
{
    (void)a; (void)b; (void)c; (void)d; (void)e;
    return (int)time_now();
}

static int sys_uptime(uint32_t a, uint32_t b, uint32_t c, uint32_t d, uint32_t e)
{
    (void)a; (void)b; (void)c; (void)d; (void)e;
    return (int)(ticks * (1000 / HZ));
}

static int sys_uname(uint32_t ubuf, uint32_t b, uint32_t c, uint32_t d, uint32_t e)
{
    (void)b; (void)c; (void)d; (void)e;
    if (user_check((void *)ubuf, sizeof(struct utsname)) < 0)
        return -EFAULT;
    struct utsname *u = (struct utsname *)ubuf;
    memset(u, 0, sizeof(*u));
    strcpy(u->sysname, FLUXOS_NAME);
    strcpy(u->nodename, "fluxOS");
    strcpy(u->release, FLUXOS_RELEASE);
    strcpy(u->version, "#1 PREEMPT " __DATE__ " " __TIME__);
    strcpy(u->machine, "i686");
    return 0;
}

static int sys_procinfo(uint32_t slot, uint32_t ubuf, uint32_t c, uint32_t d, uint32_t e)
{
    (void)c; (void)d; (void)e;
    if (user_check((void *)ubuf, sizeof(struct procinfo)) < 0)
        return -EFAULT;
    return proc_info((int)slot, (struct procinfo *)ubuf);
}

static int sys_meminfo(uint32_t ubuf, uint32_t b, uint32_t c, uint32_t d, uint32_t e)
{
    (void)b; (void)c; (void)d; (void)e;
    if (user_check((void *)ubuf, sizeof(struct meminfo)) < 0)
        return -EFAULT;
    struct meminfo *mi = (struct meminfo *)ubuf;
    uint32_t htotal, hused;
    heap_stats(&htotal, &hused);
    mi->total_kb = pmm_total_kb();
    mi->free_kb = pmm_free_kb();
    mi->kernel_kb = (uint32_t)(kernel_end - kernel_start) / 1024;
    mi->heap_total_kb = htotal / 1024;
    mi->heap_used_kb = hused / 1024;
    return 0;
}

static int sys_reboot(uint32_t cmd, uint32_t b, uint32_t c, uint32_t d, uint32_t e)
{
    (void)b; (void)c; (void)d; (void)e;
    switch (cmd) {
    case REBOOT_RESTART:
        kprintf("\nfluxOS: restarting system...\n");
        machine_reboot();
    case REBOOT_POWEROFF:
        kprintf("\nfluxOS: powering off...\n");
        machine_poweroff();
    case REBOOT_HALT:
        machine_halt();
    }
    return -EINVAL;
}

static int sys_dmesg(uint32_t ubuf, uint32_t size, uint32_t c, uint32_t d, uint32_t e)
{
    (void)c; (void)d; (void)e;
    if (user_check((void *)ubuf, size) < 0)
        return -EFAULT;
    return klog_read((char *)ubuf, size);
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
};

/* Calls that walk or change the VFS tree; read and write lock per chunk. */
static const uint8_t takes_vfs_lock[NSYSCALLS] = {
    [SYS_open] = 1, [SYS_execve] = 1, [SYS_chdir] = 1, [SYS_getcwd] = 1,
    [SYS_getdents] = 1, [SYS_mkdir] = 1, [SYS_rmdir] = 1, [SYS_unlink] = 1,
    [SYS_rename] = 1, [SYS_chmod] = 1, [SYS_stat] = 1, [SYS_mount] = 1,
    [SYS_umount] = 1, [SYS_statfs] = 1, [SYS_getmounts] = 1, [SYS_blkinfo] = 1,
};

void syscall_dispatch(struct trapframe *tf)
{
    uint32_t n = tf->eax;
    if (n >= NSYSCALLS || !syscalls[n]) {
        tf->eax = (uint32_t)-ENOSYS;
        return;
    }
    if (takes_vfs_lock[n])
        vfs_lock();
    tf->eax = (uint32_t)syscalls[n](tf->ebx, tf->ecx, tf->edx, tf->esi, tf->edi);
    if (takes_vfs_lock[n])
        vfs_unlock();
}

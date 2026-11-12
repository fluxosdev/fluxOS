#include "kernel-test.h"
#include <sys/mman.h>
#include "../kernel/core/syscall.c"
/* (shmat hands proc_map_frames a malloc'd array, which the host compiler takes for unset) */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmaybe-uninitialized"
#include "../kernel/core/shm.c"
#pragma GCC diagnostic pop
struct proc *current;
void *kmalloc(size_t n) { return malloc(n); }
void *kzalloc(size_t n) { return calloc(1, n); }
void kfree(void *p) { free(p); }
/* Shared memory segments: frames are only numbers here, and the screen is mapped when the test says. */
static uint32_t next_frame = 0x1000, screen_mapped;
uint32_t pmm_alloc_user_low(void) { return next_frame += PAGE_SIZE; }
void pmm_free(uint64_t phys) { (void)phys; }
void vmm_zero_frame(uint64_t phys) { (void)phys; }
int vmm_mmio_mapped(pte_t *pd, uint64_t va) { (void)pd; return va == USER_MMIO_BASE && screen_mapped; }
static int calls, truncate_error, opens;
static struct vnode open_vnode = { .type = VN_FILE };
static struct vnode *resolves_to = &open_vnode;
static struct file open_file;
int vfs_truncate(struct vnode *vn, uint64_t size)
{ (void)vn; (void)size; calls++; return truncate_error; }
/* User memory: the host's own, but for a page made to be missing (another
 * thread's munmap), which the copies must find without touching it. */
static uint32_t hole = 1;
static int missing(uint64_t va) { return (va & ~0xFFFu) == hole; }
uint64_t vmm_translate(pte_t *pd, uint64_t va) { (void)pd; return missing(va) ? 0 : va; }
int vmm_write_ok(pte_t *pd, uint64_t va) { (void)pd; return !missing(va); }
int proc_grow_stack(uint64_t va) { (void)va; return 0; }
int proc_page_fault(uint64_t va, int write) { (void)va; (void)write; return 0; }
int uaccess_copy(void *dst, const void *src, uint32_t n)
{
    for (uint32_t i = 0; i < n; i++)
        if (missing((uint32_t)(uintptr_t)dst + i) || missing((uint32_t)(uintptr_t)src + i))
            return -EFAULT;
    memcpy(dst, src, n);
    return 0;
}
int vfs_resolve(const char *path, struct vnode *cwd, struct vnode **out)
{ (void)path; (void)cwd; *out = resolves_to; return 0; }
static int unrefs;
static struct vnode *unref_last;
void vfs_unref(struct vnode *vn) { unrefs++; unref_last = vn; }
static int vfs_locks;
void vfs_lock(void) { vfs_locks++; }
void vfs_unlock(void) {}

/* What the read/write/send/recv paths hand down: always kernel memory, a
 * chunk at most, never the caller's buffer. */
static int ios, io_result[8], io_fill;
static uint32_t io_len[8], io_unmaps;  /* a page another thread unmaps while the call sleeps */
static int io_step(const void *buf, uint32_t n)
{
    if (io_unmaps)
        hole = io_unmaps;
    CHECK((uintptr_t)buf < USER_BASE || (uintptr_t)buf >= USER_TOP);
    CHECK(n <= IO_CHUNK && ios < 8);
    io_len[ios] = n;
    int r = io_result[ios++];
    return r == 1 ? (int)n : r;         /* 1: all of it */
}
static int io_flags;                    /* the flags of the file the latest transfer was given */
int file_read(struct file *f, void *buf, uint32_t n)
{
    io_flags = f->flags;
    int r = io_step(buf, n);
    if (r > 0)
        memset(buf, io_fill, (uint32_t)r);
    return r;
}
int file_write(struct file *f, const void *buf, uint32_t n) { io_flags = f->flags; return io_step(buf, n); }
static int peeks, peek_nonblock;
int file_peek(struct file *f, void *buf, uint32_t n, int nonblock)
{
    (void)f;
    peeks++;
    peek_nonblock = nonblock;
    int r = io_step(buf, n);
    if (r > 0)
        memset(buf, io_fill, (uint32_t)r);
    return r;
}
/* A terminal's node: O_NONBLOCK transfers go to tty.c, which has no input here. */
static struct vnode tty_vn = { .type = VN_CHR };
static int tty_calls;
int tty_io_nonblock(struct vnode *vn, char *buf, uint32_t n, int write)
{ (void)buf; (void)n; (void)write; tty_calls++; return vn == &tty_vn ? -EAGAIN : -ENOTTY; }
int sock_sendto(struct socket *s, const void *buf, uint32_t n, int flags, const struct sockaddr_in *to)
{ (void)s; (void)flags; (void)to; return io_step(buf, n); }
int sock_recvfrom(struct socket *s, void *buf, uint32_t n, int flags, struct sockaddr_in *from)
{
    (void)s; (void)flags; (void)from;
    int r = io_step(buf, n);
    if (r > 0)
        memset(buf, io_fill, (uint32_t)r);
    return r;
}
static void io_expect(int a, int b, int c)
{
    ios = 0;
    io_result[0] = a; io_result[1] = b; io_result[2] = c; io_result[3] = 1;
}

/* A /dev/dsp-like node: every open clones a device of its own. */
static struct vnode clone_vn = { .type = VN_CHR };
static int clone_open_error;
static int clone_open(struct vnode *vn, int flags) { (void)vn; (void)flags; return clone_open_error; }
static int dsp_clone(struct vnode *vn, int flags, struct vnode **out)
{ (void)vn; (void)flags; *out = &clone_vn; return 0; }
int vfs_resolve_parent(const char *path, struct vnode *cwd, struct vnode **out, char *name)
{ (void)path; (void)cwd; (void)out; (void)name; abort(); }
int vfs_create(struct vnode *dir, const char *name, int type, struct vnode **out)
{ (void)dir; (void)name; (void)type; (void)out; abort(); }
int vfs_create_file(const char *path, struct vnode *cwd, int excl, struct vnode **out)
{ (void)path; (void)cwd; (void)excl; (void)out; abort(); }
struct file *file_open(struct vnode *vn, int flags)
{ (void)vn; (void)flags; opens++; return &open_file; }
/* Descriptor lookups hold the file until the system call returns
 * (put_held_files); the opened file is never closed. */
static int closes;
struct file *file_dup(struct file *f) { if (f) f->refs++; return f; }
void file_close(struct file *f) { if (f == &open_file) abort(); f->refs--; closes++; }
void panic(const char *fmt, ...) { fprintf(stderr, "panic: %s\n", fmt); abort(); }
/* the end of each system call, as syscall_dispatch does it */
static int ended(int r) { put_held_files(); current->npaths = 0; return r; }
int main(void)
{
    struct vnode vn = { .type = VN_FILE };
    struct file f = { .type = FT_VNODE, .flags = O_RDWR, .vn = &vn };
    struct proc p = {0}; p.leader = &p; p.fds[0] = &f; current = &p;
    CHECK(ended(sys_ftruncate(0, UINT32_MAX, 0, 0, 0)) == -EINVAL);
    CHECK(ended(sys_ftruncate(0, 0x80000000u, 0, 0, 0)) == -EINVAL);
    CHECK(calls == 0);
    CHECK(ended(sys_ftruncate(0, 0, 0, 0, 0)) == 0 && calls == 1);
    CHECK(ended(sys_ftruncate(0, 128, 0, 0, 0)) == 0 && calls == 2);
    /* each lookup's reference was given back when its call ended */
    CHECK(f.refs == 0 && closes == 2 && p.nheld == 0);    /* the two bad lengths fail before the lookup */
    /* Exercise sys_open through its real 32-bit user pointer validation. */
    char *path = mmap((void *)0x50000000u, PAGE_SIZE, PROT_READ | PROT_WRITE,
                      MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    CHECK(path != MAP_FAILED && (uintptr_t)path >= USER_BASE && (uintptr_t)path < USER_TOP);
    strcpy(path, "/file");
    int before = calls;
    truncate_error = -EIO;
    CHECK(ended(sys_open((uint32_t)(uintptr_t)path, O_WRONLY | O_TRUNC, 0, 0, 0)) == -EIO);
    truncate_error = -EROFS;
    CHECK(ended(sys_open((uint32_t)(uintptr_t)path, O_WRONLY | O_TRUNC, 0, 0, 0)) == -EROFS);
    CHECK(calls == before + 2 && opens == 0 && p.fds[1] == NULL);
    truncate_error = 0;
    CHECK(ended(sys_open((uint32_t)(uintptr_t)path, O_WRONLY | O_TRUNC, 0, 0, 0)) == 1);
    CHECK(calls == before + 3 && opens == 1 && p.fds[1] == &open_file);
    CHECK(munmap(path, PAGE_SIZE) == 0);

    /* A clone whose open hook fails goes with it (it was referenced by nothing else). */
    char *mem = mmap((void *)0x50000000u, 4 * IO_CHUNK, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    CHECK(mem != MAP_FAILED && (uintptr_t)mem == 0x50000000u);
    uint32_t umem = (uint32_t)(uintptr_t)mem;
    strcpy(mem, "/dev/dsp");
    struct vnode dsp = { .type = VN_CHR, .dev_clone = dsp_clone };
    clone_vn.dev_open = clone_open;
    clone_open_error = -EIO;
    resolves_to = &dsp;
    CHECK(ended(sys_open(umem, O_WRONLY, 0, 0, 0)) == -EIO);
    CHECK(unrefs == 1 && unref_last == &clone_vn && opens == 1);
    resolves_to = &open_vnode;

    /* A path read up to a missing page is -EFAULT; one across two present pages is copied. */
    memset(mem, 'a', PAGE_SIZE);
    hole = umem + PAGE_SIZE;
    CHECK(ended(sys_open(umem + PAGE_SIZE - 3, O_RDONLY, 0, 0, 0)) == -EFAULT);
    hole = 1;
    strcpy(mem + PAGE_SIZE - 3, "/file");
    CHECK(ended(sys_open(umem + PAGE_SIZE - 3, O_RDONLY, 0, 0, 0)) == 2 && opens == 2);

    struct file pf = { .type = FT_PIPE, .flags = O_RDWR };
    struct file ff = { .type = FT_VNODE, .flags = O_RDWR, .vn = &vn };
    struct file sf = { .type = FT_SOCKET, .flags = O_RDWR };
    p.fds[3] = &pf; p.fds[4] = &ff; p.fds[5] = &sf;

    /* A pipe, terminal or socket read is one chunk: a second could block with data in hand. */
    io_fill = 'p';
    io_expect(40, 0, 0);
    memset(mem, 0, 4 * IO_CHUNK);
    CHECK(ended(sys_read(3, umem, 100, 0, 0)) == 40 && ios == 1 && io_len[0] == 100);
    CHECK(mem[0] == 'p' && mem[39] == 'p' && mem[40] == 0);
    io_expect(1, 1, 1);
    CHECK(ended(sys_read(3, umem, 3 * IO_CHUNK, 0, 0)) == IO_CHUNK && ios == 1);
    io_expect(-EAGAIN, 0, 0);
    CHECK(ended(sys_read(3, umem, 100, 0, 0)) == -EAGAIN);
    io_expect(-EBADF, 0, 0);
    CHECK(ended(sys_read(3, umem, 0, 0, 0)) == -EBADF && ios == 1 && io_len[0] == 0);

    /* A disk file is read whole, a chunk at a time under the VFS lock, up to an error or a short chunk. */
    int locks = vfs_locks;
    io_fill = 'f';
    io_expect(1, 1, 1);
    CHECK(ended(sys_read(4, umem, 3 * IO_CHUNK + 5, 0, 0)) == 3 * IO_CHUNK + 5);
    CHECK(ios == 4 && io_len[3] == 5 && vfs_locks == locks + 4 && mem[3 * IO_CHUNK + 4] == 'f');
    io_expect(1, -EIO, 0);
    CHECK(ended(sys_read(4, umem, 2 * IO_CHUNK, 0, 0)) == IO_CHUNK && ios == 2);
    io_expect(1, 7, 0);
    CHECK(ended(sys_read(4, umem, 3 * IO_CHUNK, 0, 0)) == IO_CHUNK + 7 && ios == 2);

    /* The caller's buffer unmapped while the driver slept: -EFAULT, not a kernel fault. */
    io_unmaps = umem;
    io_expect(1, 0, 0);
    CHECK(ended(sys_read(4, umem + 8, 16, 0, 0)) == -EFAULT && ios == 1);
    hole = 1;
    io_unmaps = umem + 2 * IO_CHUNK - PAGE_SIZE;
    io_expect(1, 1, 0);                 /* the second chunk is not there to send */
    CHECK(ended(sys_write(3, umem + IO_CHUNK - 8, IO_CHUNK + 16, 0, 0)) == IO_CHUNK && ios == 1);
    hole = 1;
    io_unmaps = 0;

    /* Writes go on chunk by chunk; a short or failed chunk ends them with what was written. */
    io_expect(1, 1, 1);
    CHECK(ended(sys_write(3, umem, 2 * IO_CHUNK + 1, 0, 0)) == 2 * IO_CHUNK + 1 && ios == 3);
    io_expect(1, -EAGAIN, 0);
    CHECK(ended(sys_write(3, umem, 2 * IO_CHUNK, 0, 0)) == IO_CHUNK && ios == 2);
    io_expect(-EAGAIN, 0, 0);
    CHECK(ended(sys_write(3, umem, 2 * IO_CHUNK, 0, 0)) == -EAGAIN && ios == 1);
    io_expect(1, 100, 0);
    CHECK(ended(sys_write(4, umem, 2 * IO_CHUNK, 0, 0)) == IO_CHUNK + 100 && ios == 2);
    io_expect(-EPIPE, 0, 0);
    CHECK(ended(sys_write(3, umem, 0, 0, 0)) == -EPIPE && ios == 1);

    /* send and receive the same way. */
    io_expect(1, 1, 0);
    CHECK(ended(sys_sendto(5, umem, IO_CHUNK + 9, 0, 0)) == IO_CHUNK + 9 && ios == 2 && io_len[1] == 9);
    io_expect(1, -EAGAIN, 0);
    CHECK(ended(sys_sendto(5, umem, 2 * IO_CHUNK, MSG_DONTWAIT, 0)) == IO_CHUNK && ios == 2);
    io_fill = 's';
    io_expect(1, 0, 0);
    CHECK(ended(sys_recvfrom(5, umem, 2 * IO_CHUNK, 0, 0)) == IO_CHUNK && ios == 1);
    CHECK(mem[IO_CHUNK - 1] == 's');
    io_expect(-EAGAIN, 0, 0);
    CHECK(ended(sys_recvfrom(5, umem, 10, MSG_DONTWAIT, 0)) == -EAGAIN);
    io_unmaps = umem;
    io_expect(1, 0, 0);
    CHECK(ended(sys_recvfrom(5, umem + 8, 10, 0, 0)) == -EFAULT && ios == 1);
    hole = 1;
    io_unmaps = 0;

    /* AF_UNIX: MSG_DONTWAIT is this call's alone (the shared file keeps its flags); MSG_PEEK peeks. */
    struct file uf = { .type = FT_UNIX, .flags = O_RDWR };
    p.fds[6] = &uf;
    io_expect(-EAGAIN, 0, 0);
    CHECK(ended(sys_recvfrom(6, umem, 10, MSG_DONTWAIT, 0)) == -EAGAIN && (io_flags & O_NONBLOCK));
    CHECK(!(uf.flags & O_NONBLOCK));
    io_expect(-EAGAIN, 0, 0);
    CHECK(ended(sys_sendto(6, umem, 10, MSG_DONTWAIT, 0)) == -EAGAIN && (io_flags & O_NONBLOCK));
    io_expect(1, 0, 0);
    CHECK(ended(sys_sendto(6, umem, 10, 0, 0)) == 10 && !(io_flags & O_NONBLOCK));
    io_fill = 'k';
    io_expect(1, 0, 0);
    memset(mem, 0, 10);
    CHECK(ended(sys_recvfrom(6, umem, 10, MSG_PEEK, 0)) == 10 && peeks == 1 && !peek_nonblock &&
          ios == 1 && mem[9] == 'k');
    io_expect(-EAGAIN, 0, 0);
    CHECK(ended(sys_recvfrom(6, umem, 10, MSG_PEEK | MSG_DONTWAIT, 0)) == -EAGAIN && peeks == 2 &&
          peek_nonblock && !(uf.flags & O_NONBLOCK));
    hole = umem;                        /* an unmapped buffer: refused before peeking */
    io_expect(1, 0, 0);
    CHECK(ended(sys_recvfrom(6, umem + 8, 10, MSG_PEEK, 0)) == -EFAULT && peeks == 2);
    hole = 1;

    /* A terminal opened O_NONBLOCK does not wait: its device calls cannot see the flag. */
    struct file tf = { .type = FT_VNODE, .flags = O_RDWR | O_NONBLOCK, .vn = &tty_vn };
    p.fds[7] = &tf;
    ios = 0;
    CHECK(ended(sys_read(7, umem, 10, 0, 0)) == -EAGAIN && ios == 0 && tty_calls == 1);
    tf.flags = O_RDWR;
    io_expect(1, 0, 0);
    CHECK(ended(sys_read(7, umem, 10, 0, 0)) == 10 && ios == 1 && tty_calls == 1);
    CHECK(munmap(mem, 4 * IO_CHUNK) == 0);

    /* SysV segments: SHM_PER_PROC a process; the one with the screen (the desktop) has no limit,
     * and the others together leave it SHM_RESERVE. Removing one, or the creator's exit, gives it back. */
    p.pid = 100;
    int first = sys_shmget(IPC_PRIVATE, 1, 0, 0, 0);
    CHECK(first >= 0);
    for (int i = 1; i < SHM_PER_PROC; i++)
        CHECK(sys_shmget(IPC_PRIVATE, 1, 0, 0, 0) >= 0);
    CHECK(sys_shmget(IPC_PRIVATE, 1, 0, 0, 0) == -ENOSPC);
    CHECK(sys_shmctl((uint32_t)first, IPC_RMID, 0, 0, 0) == 0);
    CHECK(sys_shmget(IPC_PRIVATE, 1, 0, 0, 0) >= 0);
    int made = SHM_PER_PROC;
    for (p.pid = 101; p.pid < 120; p.pid++)
        while (sys_shmget(IPC_PRIVATE, 1, 0, 0, 0) >= 0)
            made++;
    CHECK(made == SHM_SEGS - SHM_RESERVE);
    p.pid = 1000;
    screen_mapped = 1;
    for (int i = 0; i < SHM_RESERVE; i++)
        CHECK(sys_shmget(IPC_PRIVATE, 1, 0, 0, 0) >= 0);
    CHECK(sys_shmget(IPC_PRIVATE, 1, 0, 0, 0) == -ENOSPC);     /* (the table is full) */
    screen_mapped = 0;
    shm_proc_exit(100);
    p.pid = 200;
    CHECK(sys_shmget(IPC_PRIVATE, 1, 0, 0, 0) >= 0);
    for (int pid = 100; pid < 120; pid++)
        shm_proc_exit(pid);
    shm_proc_exit(200);
    shm_proc_exit(1000);
    p.pid = 0;
    puts("kernel syscall: ftruncate lengths, open truncation error propagation, clone leak, bounced I/O, AF_UNIX flags, non-blocking terminals and shm limits passed");
}

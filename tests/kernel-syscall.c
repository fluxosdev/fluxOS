#include "kernel-test.h"
#include <sys/mman.h>
#include "../kernel/core/syscall.c"
struct proc *current;
void *kmalloc(size_t n) { return malloc(n); }
void kfree(void *p) { free(p); }
static int calls, truncate_error, opens;
static struct vnode open_vnode = { .type = VN_FILE };
static struct vnode *resolves_to = &open_vnode;
static struct file open_file;
int vfs_truncate(struct vnode *vn, uint32_t size)
{ (void)vn; (void)size; calls++; return truncate_error; }
/* User memory: the host's own, but for a page made to be missing (another
 * thread's munmap), which the copies must find without touching it. */
static uint32_t hole = 1;
static int missing(uint32_t va) { return (va & ~0xFFFu) == hole; }
uint32_t vmm_translate(uint32_t *pd, uint32_t va) { (void)pd; return missing(va) ? 0 : va; }
int vmm_write_ok(uint32_t *pd, uint32_t va) { (void)pd; return !missing(va); }
int proc_grow_stack(uint32_t va) { (void)va; return 0; }
int proc_page_fault(uint32_t va, int write) { (void)va; (void)write; return 0; }
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
int file_read(struct file *f, void *buf, uint32_t n)
{
    (void)f;
    int r = io_step(buf, n);
    if (r > 0)
        memset(buf, io_fill, (uint32_t)r);
    return r;
}
int file_write(struct file *f, const void *buf, uint32_t n) { (void)f; return io_step(buf, n); }
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
    CHECK(munmap(mem, 4 * IO_CHUNK) == 0);
    puts("kernel syscall: ftruncate lengths, open truncation error propagation, clone leak and bounced I/O passed");
}

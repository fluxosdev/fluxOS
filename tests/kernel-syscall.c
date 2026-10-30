#include "kernel-test.h"
#include <sys/mman.h>
#include "../kernel/core/syscall.c"
struct proc *current;
void *kmalloc(size_t n) { return malloc(n); }
static int calls, truncate_error, opens;
static struct vnode open_vnode = { .type = VN_FILE };
static struct file open_file;
int vfs_truncate(struct vnode *vn, uint32_t size)
{ (void)vn; (void)size; calls++; return truncate_error; }
uint32_t vmm_translate(uint32_t *pd, uint32_t va) { (void)pd; return va; }
int proc_grow_stack(uint32_t va) { (void)va; return 0; }
int vfs_resolve(const char *path, struct vnode *cwd, struct vnode **out)
{ (void)path; (void)cwd; *out = &open_vnode; return 0; }
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
static int ended(int r) { put_held_files(); return r; }
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
    puts("kernel syscall: ftruncate lengths and open truncation error propagation passed");
}

#include "kernel-test.h"
#include "../kernel/syscall.c"
struct proc *current;
static int calls;
int vfs_truncate(struct vnode *vn, uint32_t size) { (void)vn; (void)size; calls++; return 0; }
int main(void)
{
    struct vnode vn = { .type = VN_FILE };
    struct file f = { .type = FT_VNODE, .flags = O_RDWR, .vn = &vn };
    struct proc p = {0}; p.fds[0] = &f; current = &p;
    CHECK(sys_ftruncate(0, UINT32_MAX, 0, 0, 0) == -EINVAL);
    CHECK(sys_ftruncate(0, 0x80000000u, 0, 0, 0) == -EINVAL);
    CHECK(calls == 0);
    CHECK(sys_ftruncate(0, 0, 0, 0, 0) == 0 && calls == 1);
    CHECK(sys_ftruncate(0, 128, 0, 0, 0) == 0 && calls == 2);
    puts("kernel syscall: negative ftruncate rejection and valid lengths passed");
}

/*
 * execve: load a static ELF32 executable (or a "#!" script, or a DOS
 * program, which runs under /bin/dos) into a new address space and
 * build the initial user stack:
 *
 *   sp -> argc, argv[0..argc-1], NULL, envp[0..n-1], NULL, strings...
 */
#include "kernel.h"
#include "mm.h"
#include "proc.h"
#include "vfs.h"

#define ELF_MAGIC "\x7f" "ELF"
#define ET_EXEC   2
#define EM_386    3
#define PT_LOAD   1

#define DOS_LOADER "/bin/dos"

#define MAX_ARG_BYTES (64 * 1024)
#define MAX_IMAGE     (32u << 20)
#define MAX_ARGS      1024
#define STACK_SLACK   (4 * PAGE_SIZE)

struct elf32_ehdr {
    uint8_t ident[16];
    uint16_t type, machine;
    uint32_t version, entry, phoff, shoff, flags;
    uint16_t ehsize, phentsize, phnum, shentsize, shnum, shstrndx;
} __attribute__((packed));

struct elf32_phdr {
    uint32_t type, offset, vaddr, paddr, filesz, memsz, flags, align;
} __attribute__((packed));

static int count_strings(char *const v[], uint32_t *bytes)
{
    int n = 0;
    for (; v && v[n]; n++) {
        if (n >= MAX_ARGS)
            return -E2BIG;
        *bytes += strlen(v[n]) + 1;
    }
    return n;
}

/* An executable's bytes: ramfs files are used in place, others read in. */
struct image {
    const uint8_t *data;
    uint32_t size;
    uint8_t *owned;
};

static int image_load(struct vnode *vn, struct image *img)
{
    img->owned = NULL;
    img->size = vn->size;
    if (!vn->fs) {
        img->data = vn->data;
        return 0;
    }
    if (vn->size > MAX_IMAGE)
        return -E2BIG;
    img->owned = kmalloc(vn->size ? vn->size : 1);
    if (!img->owned)
        return -ENOMEM;
    int r = vfs_read(vn, img->owned, 0, vn->size);
    if (r != (int)vn->size) {
        kfree(img->owned);
        return r < 0 ? r : -EIO;
    }
    img->data = img->owned;
    return 0;
}

static void image_free(struct image *img)
{
    kfree(img->owned);
    img->owned = NULL;
}

static int load_segments(uint32_t *pd, const struct image *vn, uint32_t *entry, uint32_t *top)
{
    const struct elf32_ehdr *eh = (const struct elf32_ehdr *)vn->data;

    if (vn->size < sizeof(*eh) || memcmp(eh->ident, ELF_MAGIC, 4) != 0)
        return -ENOEXEC;
    if (eh->ident[4] != 1 || eh->ident[5] != 1 || eh->type != ET_EXEC ||
        eh->machine != EM_386 || eh->phentsize != sizeof(struct elf32_phdr))
        return -ENOEXEC;
    if (eh->phoff > vn->size || eh->phnum * sizeof(struct elf32_phdr) > vn->size - eh->phoff)
        return -ENOEXEC;

    const struct elf32_phdr *ph = (const struct elf32_phdr *)(vn->data + eh->phoff);
    *top = 0;
    for (int i = 0; i < eh->phnum; i++, ph++) {
        if (ph->type != PT_LOAD || ph->memsz == 0)
            continue;
        uint32_t end = ph->vaddr + ph->memsz;
        if (ph->memsz < ph->filesz || ph->vaddr < USER_BASE || end < ph->vaddr ||
            end > USER_STACK_LIMIT || ph->offset > vn->size ||
            ph->filesz > vn->size - ph->offset)
            return -ENOEXEC;
        for (uint32_t va = ALIGN_DOWN(ph->vaddr, PAGE_SIZE); va < end; va += PAGE_SIZE)
            if (vmm_alloc(pd, va) < 0)
                return -ENOMEM;
        if (vmm_copy_to(pd, ph->vaddr, vn->data + ph->offset, ph->filesz) < 0)
            return -EFAULT;
        *top = MAX(*top, end);
    }
    if (*top == 0 || eh->entry < USER_BASE || eh->entry >= *top)
        return -ENOEXEC;
    *entry = eh->entry;
    return 0;
}

static int build_stack(uint32_t *pd, char *const argv[], char *const envp[], uint32_t *sp_out)
{
    uint32_t str_bytes = 0;
    int argc = count_strings(argv, &str_bytes);
    int envc = count_strings(envp, &str_bytes);
    if (argc < 0 || envc < 0 || str_bytes > MAX_ARG_BYTES)
        return -E2BIG;

    uint32_t nptrs = 1 + (uint32_t)argc + 1 + (uint32_t)envc + 1;
    uint32_t str_base = USER_STACK_TOP - ALIGN_UP(str_bytes, 16u);
    uint32_t sp = ALIGN_DOWN(str_base - nptrs * 4, 16u);
    uint32_t size = USER_STACK_TOP - sp;

    uint8_t *frame = kzalloc(size);
    if (!frame)
        return -ENOMEM;
    uint32_t *ptrs = (uint32_t *)frame;
    char *strs = (char *)frame + (str_base - sp);
    uint32_t va = str_base;
    uint32_t k = 0;

    ptrs[k++] = (uint32_t)argc;
    for (int i = 0; i < argc; i++) {
        uint32_t len = strlen(argv[i]) + 1;
        memcpy(strs, argv[i], len);
        ptrs[k++] = va;
        strs += len;
        va += len;
    }
    ptrs[k++] = 0;
    for (int i = 0; i < envc; i++) {
        uint32_t len = strlen(envp[i]) + 1;
        memcpy(strs, envp[i], len);
        ptrs[k++] = va;
        strs += len;
        va += len;
    }
    ptrs[k++] = 0;

    int r = 0;
    for (uint32_t pg = ALIGN_DOWN(sp, PAGE_SIZE) - STACK_SLACK; pg < USER_STACK_TOP && !r; pg += PAGE_SIZE)
        r = vmm_alloc(pd, pg);
    if (!r)
        r = vmm_copy_to(pd, sp, frame, size);
    kfree(frame);
    *sp_out = sp;
    return r;
}

/* Run interp with the program's path as argv[1], followed by argv[1..]. */
static int exec_interp(struct proc *p, const char *interp, const char *path,
                       char *const argv[], char *const envp[])
{
    int argc = 0;
    while (argv && argv[argc])
        argc++;
    char **nargv = kmalloc(sizeof(char *) * (uint32_t)(argc + 3));
    if (!nargv)
        return -ENOMEM;
    nargv[0] = (char *)interp;
    nargv[1] = (char *)path;
    for (int k = 1; k < argc; k++)
        nargv[k + 1] = argv[k];
    nargv[argc < 1 ? 2 : argc + 1] = NULL;

    struct vnode *ivn;
    char magic[2] = { 0, 0 };
    int r = vfs_resolve(interp, p->cwd, &ivn);
    if (r == 0 && ivn->type == VN_FILE && vfs_read(ivn, magic, 0, 2) == 2 &&
        magic[0] == '#' && magic[1] == '!')
        r = -ENOEXEC;                       /* no nested interpreters */
    if (r == 0)
        r = proc_exec(p, interp, nargv, envp);
    kfree(nargv);
    return r;
}

/* "#!/path/to/interp" -> run interp with the script path as argv[1]. */
static int exec_script(struct proc *p, struct image *vn, const char *path,
                       char *const argv[], char *const envp[])
{
    char interp[PATH_MAX];
    uint32_t i = 2, n = 0;
    while (i < vn->size && vn->data[i] == ' ')
        i++;
    while (i < vn->size && n < sizeof(interp) - 1 && vn->data[i] != '\n' &&
           vn->data[i] != ' ' && vn->data[i] != '\r')
        interp[n++] = (char)vn->data[i++];
    interp[n] = 0;
    image_free(vn);
    if (!n)
        return -ENOEXEC;
    return exec_interp(p, interp, path, argv, envp);
}

/* A DOS program: an "MZ" executable, or a .COM file, which has no header. */
static int is_dos_program(const struct image *img, const char *path)
{
    if (img->size >= 2 && ((img->data[0] == 'M' && img->data[1] == 'Z') ||
                           (img->data[0] == 'Z' && img->data[1] == 'M')))
        return 1;
    if (img->size >= 4 && memcmp(img->data, ELF_MAGIC, 4) == 0)
        return 0;
    uint32_t n = strlen(path);
    return n > 4 && img->size > 0 && img->size <= 0xFF00 && path[n - 4] == '.' &&
           (path[n - 3] | 0x20) == 'c' && (path[n - 2] | 0x20) == 'o' &&
           (path[n - 1] | 0x20) == 'm';
}

int proc_exec(struct proc *p, const char *path, char *const argv[], char *const envp[])
{
    struct vnode *vn;
    int r = vfs_resolve(path, p->cwd, &vn);
    if (r < 0)
        return r;
    if (vn->type == VN_DIR)
        return -EISDIR;
    if (vn->type != VN_FILE || !(vn->mode & 0111))
        return -EACCES;
    struct image img;
    r = image_load(vn, &img);
    if (r < 0)
        return r;
    if (img.size >= 2 && img.data[0] == '#' && img.data[1] == '!')
        return exec_script(p, &img, path, argv, envp);
    if (is_dos_program(&img, path)) {
        image_free(&img);
        return exec_interp(p, DOS_LOADER, path, argv, envp);
    }

    uint32_t *pd = vmm_create();
    if (!pd) {
        image_free(&img);
        return -ENOMEM;
    }

    uint32_t entry, top, sp;
    r = load_segments(pd, &img, &entry, &top);
    image_free(&img);
    if (r == 0)
        r = build_stack(pd, argv, envp, &sp);
    if (r < 0) {
        vmm_destroy(pd);
        return r;
    }

    /* Point of no return: replace the old image. */
    uint32_t *old = p->pd;
    p->pd = pd;
    if (p == current)
        vmm_switch(pd);
    vmm_destroy(old);

    p->heap_start = p->brk = ALIGN_UP(top, PAGE_SIZE);
    p->stack_low = ALIGN_DOWN(sp, PAGE_SIZE) - STACK_SLACK;

    const char *base = strrchr(path, '/');
    strlcpy(p->name, base ? base + 1 : path, sizeof(p->name));

    struct trapframe *tf = p->tf;
    memset(tf, 0, sizeof(*tf));
    tf->cs = SEG_UCODE;
    tf->ds = tf->es = tf->fs = tf->gs = tf->ss = SEG_UDATA;
    tf->eflags = 0x202;                     /* IF set */
    tf->eip = entry;
    tf->useresp = sp;
    return 0;
}

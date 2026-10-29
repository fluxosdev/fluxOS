/*
 * execve: load a static ELF32 executable (or a "#!" script, a Windows
 * program, which runs under /bin/winrun, or a DOS program, which runs
 * under /bin/dos) into a new address space and build the initial user
 * stack:
 *
 *   sp -> argc, argv[0..argc-1], NULL, envp[0..n-1], NULL, strings...
 */
#include "kernel.h"
#include "drivers.h"
#include "mm.h"
#include "proc.h"
#include "vfs.h"

#define ELF_MAGIC "\x7f" "ELF"
#define ET_EXEC   2
#define EM_386    3
#define PT_LOAD   1
#define PF_X      1
#define PF_W      2

#define DOS_LOADER "/bin/dos"
#define WIN_LOADER "/bin/winrun"

#define MAX_ARG_BYTES (256 * 1024)
#define MAX_ARGS      4096
#define STACK_SLACK   (4 * PAGE_SIZE)
#define HEAD_BYTES    4096              /* of a file on disk: enough for the ELF header, "#!" or "MZ" */
#define COPY_CHUNK    (64 * 1024)       /* a segment is read in pieces this big */

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

/*
 * An executable's bytes: a ramfs file whole, in place; a file on disk by
 * its first HEAD_BYTES, the rest read straight into the new address space
 * as it is loaded, so that a large program never needs the kernel heap
 * to hold all of it.
 */
struct image {
    struct vnode *vn;
    const uint8_t *data;
    uint32_t size;                      /* of the file */
    uint32_t have;                      /* bytes at data */
    uint8_t *owned;
};

static int image_load(struct vnode *vn, struct image *img)
{
    img->vn = vn;
    img->owned = NULL;
    img->size = vn->size;
    if (!vn->fs) {
        img->data = vn->data;
        img->have = vn->size;
        return 0;
    }
    uint32_t n = MIN(vn->size, (uint32_t)HEAD_BYTES);
    img->owned = kmalloc(n ? n : 1);
    if (!img->owned)
        return -ENOMEM;
    int r = vfs_read(vn, img->owned, 0, n);
    if (r != (int)n) {
        kfree(img->owned);
        img->owned = NULL;
        return r < 0 ? r : -EIO;
    }
    img->data = img->owned;
    img->have = n;
    return 0;
}

static void image_free(struct image *img)
{
    kfree(img->owned);
    img->owned = NULL;
}

/* n bytes of the file from off into the new address space at va. */
static int image_copy_in(uint32_t *pd, const struct image *img, uint32_t va, uint32_t off, uint32_t n)
{
    if (!n)
        return 0;
    if (off + n <= img->have)
        return vmm_copy_to(pd, va, img->data + off, n) < 0 ? -EFAULT : 0;
    uint8_t *buf = kmalloc(COPY_CHUNK);
    if (!buf)
        return -ENOMEM;
    int r = 0;
    while (n && r == 0) {
        uint32_t chunk = MIN(n, (uint32_t)COPY_CHUNK);
        int got = vfs_read(img->vn, buf, off, chunk);
        if (got != (int)chunk)
            r = got < 0 ? got : -EIO;
        else if (vmm_copy_to(pd, va, buf, chunk) < 0)
            r = -EFAULT;
        va += chunk;
        off += chunk;
        n -= chunk;
    }
    kfree(buf);
    return r;
}

/* Is the page at va part of a writable segment? */
static int page_writable(const struct elf32_phdr *ph, int nph, uint32_t va)
{
    for (int i = 0; i < nph; i++) {
        if (ph[i].type != PT_LOAD || !ph[i].memsz || !(ph[i].flags & PF_W))
            continue;
        if (va + PAGE_SIZE > ALIGN_DOWN(ph[i].vaddr, PAGE_SIZE) && va < ph[i].vaddr + ph[i].memsz)
            return 1;
    }
    return 0;
}

static int load_segments(uint32_t *pd, const struct image *img, uint32_t *entry, uint32_t *top)
{
    const struct elf32_ehdr *eh = (const struct elf32_ehdr *)img->data;

    if (img->have < sizeof(*eh) || memcmp(eh->ident, ELF_MAGIC, 4) != 0)
        return -ENOEXEC;
    if (eh->ident[4] != 1 || eh->ident[5] != 1 || eh->type != ET_EXEC ||
        eh->machine != EM_386 || eh->phentsize != sizeof(struct elf32_phdr))
        return -ENOEXEC;
    uint32_t ph_bytes = (uint32_t)eh->phnum * sizeof(struct elf32_phdr);
    if (eh->phoff > img->size || ph_bytes > img->size - eh->phoff)
        return -ENOEXEC;

    /* The program headers: in the head read already, or else fetched. */
    struct elf32_phdr *fetched = NULL;
    const struct elf32_phdr *ph;
    if (eh->phoff + ph_bytes <= img->have) {
        ph = (const struct elf32_phdr *)(img->data + eh->phoff);
    } else {
        fetched = kmalloc(ph_bytes ? ph_bytes : 1);
        if (!fetched)
            return -ENOMEM;
        if (vfs_read(img->vn, fetched, eh->phoff, ph_bytes) != (int)ph_bytes) {
            kfree(fetched);
            return -EIO;
        }
        ph = fetched;
    }

    int r = 0;
    *top = 0;
    for (int i = 0; i < eh->phnum && r == 0; i++) {
        const struct elf32_phdr *p = &ph[i];
        if (p->type != PT_LOAD || p->memsz == 0)
            continue;
        uint32_t end = p->vaddr + p->memsz;
        if (p->memsz < p->filesz || p->vaddr < USER_BASE || end < p->vaddr ||
            end > USER_MMIO_BASE || p->offset > img->size ||
            p->filesz > img->size - p->offset) {
            r = -ENOEXEC;
            break;
        }
        for (uint32_t va = ALIGN_DOWN(p->vaddr, PAGE_SIZE); va < end && r == 0; va += PAGE_SIZE)
            if (vmm_alloc(pd, va) < 0)
                r = -ENOMEM;
        if (r == 0)
            r = image_copy_in(pd, img, p->vaddr, p->offset, p->filesz);
        *top = MAX(*top, end);
    }
    if (r == 0 && (*top == 0 || eh->entry < USER_BASE || eh->entry >= *top))
        r = -ENOEXEC;

    /* The segments' own permissions: code and read-only data stay as the
     * compiler left them, unless a writable segment shares the page. */
    for (int i = 0; i < eh->phnum && r == 0; i++) {
        const struct elf32_phdr *p = &ph[i];
        if (p->type != PT_LOAD || p->memsz == 0 || (p->flags & PF_W))
            continue;
        uint32_t prot = PROT_READ | ((p->flags & PF_X) ? PROT_EXEC : 0);
        for (uint32_t va = ALIGN_DOWN(p->vaddr, PAGE_SIZE); va < p->vaddr + p->memsz; va += PAGE_SIZE)
            if (!page_writable(ph, eh->phnum, va))
                vmm_protect_page(pd, va, prot);
    }
    kfree(fetched);
    if (r == 0)
        *entry = eh->entry;
    return r;
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
    int r = vfs_resolve(interp, p->leader->cwd, &ivn);
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
    char interp[256];                   /* (an interpreter's path on a #! line is short) */
    uint32_t i = 2, n = 0;
    while (i < vn->have && vn->data[i] == ' ')
        i++;
    while (i < vn->have && n < sizeof(interp) - 1 && vn->data[i] != '\n' &&
           vn->data[i] != ' ' && vn->data[i] != '\r')
        interp[n++] = (char)vn->data[i++];
    interp[n] = 0;
    image_free(vn);
    if (!n)
        return -ENOEXEC;
    return exec_interp(p, interp, path, argv, envp);
}

/* A Windows program: an "MZ" executable whose header points at a "PE" one. */
static int is_pe_program(const struct image *img)
{
    if (img->have < 0x40 || !((img->data[0] == 'M' && img->data[1] == 'Z') ||
                              (img->data[0] == 'Z' && img->data[1] == 'M')))
        return 0;
    uint32_t off;
    memcpy(&off, img->data + 0x3C, 4);
    if (off < 0x40 || off > img->size - 4 || (off & 3))
        return 0;
    uint8_t sig[4];
    if (off + 4 <= img->have)
        memcpy(sig, img->data + off, 4);
    else if (vfs_read(img->vn, sig, off, 4) != 4)
        return 0;
    return sig[0] == 'P' && sig[1] == 'E' && sig[2] == 0 && sig[3] == 0;
}

/* A DOS program: an "MZ" executable, or a .COM file, which has no header. */
static int is_dos_program(const struct image *img, const char *path)
{
    if (img->have >= 2 && ((img->data[0] == 'M' && img->data[1] == 'Z') ||
                           (img->data[0] == 'Z' && img->data[1] == 'M')))
        return 1;
    if (img->have >= 4 && memcmp(img->data, ELF_MAGIC, 4) == 0)
        return 0;
    uint32_t n = strlen(path);
    return n > 4 && img->size > 0 && img->size <= 0xFF00 && path[n - 4] == '.' &&
           (path[n - 3] | 0x20) == 'c' && (path[n - 2] | 0x20) == 'o' &&
           (path[n - 1] | 0x20) == 'm';
}

int proc_exec(struct proc *p, const char *path, char *const argv[], char *const envp[])
{
    struct vnode *vn;
    int r = vfs_resolve(path, p->leader->cwd, &vn);
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
    if (img.have >= 2 && img.data[0] == '#' && img.data[1] == '!')
        return exec_script(p, &img, path, argv, envp);
    if (is_pe_program(&img)) {
        image_free(&img);
        return exec_interp(p, WIN_LOADER, path, argv, envp);
    }
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

    /* Point of no return: replace the old image. The screen, the 3D
     * engine and buffers lent to the GPU were the old image's: give them
     * up before its memory is freed, as at exit. */
    proc_exec_alone(p);                     /* only this thread goes on, as the process */
    fb_proc_exit(p->pid);
    vmm_destroy(proc_set_pd(p, pd));
    p->tls = 0;
    p->clear_tid = 0;
    p->alt_size = 0;                        /* the signal stack was in the old image */
    p->it_virt_left = p->it_prof_left = 0;  /* (the real-time timer goes on, as POSIX has it) */

    fpu_release(p);
    memset(p->leader->vmas, 0, sizeof(p->leader->vmas));
    proc_signal_exec(p);
    for (int i = 0; i < MAX_FDS; i++) {
        if (p->leader->fds[i] && fd_cloexec_get(p, i)) {
            struct file *f = p->leader->fds[i];
            p->leader->fds[i] = NULL;
            fd_cloexec_set(p, i, 0);
            file_close(f);
        }
    }
    p->leader->heap_start = p->leader->brk = ALIGN_UP(top, PAGE_SIZE);
    p->leader->stack_low = ALIGN_DOWN(sp, PAGE_SIZE) - STACK_SLACK;

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

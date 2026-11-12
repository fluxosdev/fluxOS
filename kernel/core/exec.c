/*
 * execve: load a static ELF executable (or a "#!" script, a Windows
 * program, which runs under /bin/winrun, or a DOS program, which runs
 * under /bin/dos) into a new address space and build the initial user
 * stack:
 *
 *   sp -> argc, argv[0..argc-1], NULL, envp[0..n-1], NULL, strings...
 *
 * The executable is a 64-bit one for x86-64 (ELF64), which runs in 64-bit
 * mode, or a 32-bit one for the 386 (ELF32), which runs in the
 * processor's compatibility mode. Both are loaded from USER_BASE; a 32-bit
 * program's heap follows it and its stack ends at USER_TOP, a 64-bit
 * program's heap and stack are far above (kernel.h), the words of its
 * stack are 8 bytes, and its stack pointer is 16-byte aligned.
 */
#include "kernel.h"
#include "drivers.h"
#include "mm.h"
#include "proc.h"
#include "vfs.h"

#define ELF_MAGIC "\x7f" "ELF"
#define ET_EXEC   2
#define EM_386    3
#define EM_X86_64 62
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

struct elf64_ehdr {
    uint8_t ident[16];
    uint16_t type, machine;
    uint32_t version;
    uint64_t entry, phoff, shoff;
    uint32_t flags;
    uint16_t ehsize, phentsize, phnum, shentsize, shnum, shstrndx;
} __attribute__((packed));

struct elf64_phdr {
    uint32_t type, flags;
    uint64_t offset, vaddr, paddr, filesz, memsz, align;
} __attribute__((packed));

/* A loadable segment of either kind (a 64-bit program's are below 4 GiB too: checked). */
struct segment {
    uint32_t flags, offset, vaddr, filesz, memsz;
};

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
    if (vn->size > UINT32_MAX)          /* (no program is: its image has to fit below the device window) */
        return -ENOEXEC;
    img->size = (uint32_t)vn->size;
    if (!vn->fs) {
        img->data = vn->data;
        img->have = img->size;
        return 0;
    }
    uint32_t n = MIN(img->size, (uint32_t)HEAD_BYTES);
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
static int image_copy_in(pte_t *pd, const struct image *img, uint32_t va, uint32_t off, uint32_t n)
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
static int page_writable(const struct segment *seg, int nseg, uint32_t va)
{
    for (int i = 0; i < nseg; i++) {
        if (!(seg[i].flags & PF_W))
            continue;
        if (va + PAGE_SIZE > ALIGN_DOWN(seg[i].vaddr, PAGE_SIZE) && va < seg[i].vaddr + seg[i].memsz)
            return 1;
    }
    return 0;
}

/* n bytes of the file at off: in the head read already, or else fetched (*fetched, for the caller to free). */
static const void *image_bytes(const struct image *img, uint64_t off, uint32_t n, void **fetched)
{
    *fetched = NULL;
    if (off > img->size || n > img->size - off)
        return NULL;
    if (off + n <= img->have)
        return img->data + off;
    void *buf = kmalloc(n ? n : 1);
    if (!buf)
        return NULL;
    if (vfs_read(img->vn, buf, (uint32_t)off, n) != (int)n) {
        kfree(buf);
        return NULL;
    }
    return *fetched = buf;
}

/*
 * The executable's loadable segments (*out, for the caller to free), its
 * entry point and which kind it is: 0 an ELF32 program for the 386, 1 an
 * ELF64 one for x86-64.
 */
static int read_segments(const struct image *img, struct segment **out, int *nseg, uint32_t *entry, int *is64)
{
    const struct elf32_ehdr *e32 = (const struct elf32_ehdr *)img->data;
    const struct elf64_ehdr *e64 = (const struct elf64_ehdr *)img->data;
    if (img->have < sizeof(*e32) || memcmp(e32->ident, ELF_MAGIC, 4) != 0 || e32->ident[5] != 1)
        return -ENOEXEC;
    uint64_t phoff, entry64;
    uint32_t phnum, phentsize;
    if (e32->ident[4] == 1 && e32->type == ET_EXEC && e32->machine == EM_386 &&
        e32->phentsize == sizeof(struct elf32_phdr)) {
        *is64 = 0;
        phoff = e32->phoff;
        phnum = e32->phnum;
        entry64 = e32->entry;
        phentsize = sizeof(struct elf32_phdr);
    } else if (e32->ident[4] == 2 && img->have >= sizeof(*e64) && e64->type == ET_EXEC &&
               e64->machine == EM_X86_64 && e64->phentsize == sizeof(struct elf64_phdr)) {
        *is64 = 1;
        phoff = e64->phoff;
        phnum = e64->phnum;
        entry64 = e64->entry;
        phentsize = sizeof(struct elf64_phdr);
    } else {
        return -ENOEXEC;
    }
    void *fetched;
    const uint8_t *ph = image_bytes(img, phoff, phnum * phentsize, &fetched);
    if (!ph)
        return -ENOEXEC;
    struct segment *seg = kmalloc((phnum ? phnum : 1) * sizeof(*seg));
    if (!seg) {
        kfree(fetched);
        return -ENOMEM;
    }
    int n = 0, r = 0;
    for (uint32_t i = 0; i < phnum && r == 0; i++) {
        uint64_t offset, vaddr, filesz, memsz;
        uint32_t type, flags;
        if (*is64) {
            const struct elf64_phdr *p = (const struct elf64_phdr *)(ph + i * phentsize);
            type = p->type; flags = p->flags;
            offset = p->offset; vaddr = p->vaddr; filesz = p->filesz; memsz = p->memsz;
        } else {
            const struct elf32_phdr *p = (const struct elf32_phdr *)(ph + i * phentsize);
            type = p->type; flags = p->flags;
            offset = p->offset; vaddr = p->vaddr; filesz = p->filesz; memsz = p->memsz;
        }
        if (type != PT_LOAD || memsz == 0)
            continue;
        /* Where programs are loaded (below the device memory, a 64-bit
         * program too: its code is linked for an address under 2 GiB), and
         * within the file. */
        if (memsz < filesz || vaddr < USER_BASE || vaddr > USER_MMIO_BASE || memsz > USER_MMIO_BASE - vaddr ||
            offset > img->size || filesz > img->size - offset) {
            r = -ENOEXEC;
            break;
        }
        seg[n++] = (struct segment){ flags, (uint32_t)offset, (uint32_t)vaddr, (uint32_t)filesz, (uint32_t)memsz };
    }
    kfree(fetched);
    if (r == 0 && entry64 >> 32)
        r = -ENOEXEC;
    if (r < 0) {
        kfree(seg);
        return r;
    }
    *out = seg;
    *nseg = n;
    *entry = (uint32_t)entry64;
    return 0;
}

static int load_segments(pte_t *pd, const struct image *img, uint32_t *entry, uint32_t *top, int *is64)
{
    struct segment *seg;
    int nseg;
    int r = read_segments(img, &seg, &nseg, entry, is64);
    if (r < 0)
        return r;
    *top = 0;
    for (int i = 0; i < nseg && r == 0; i++) {
        const struct segment *p = &seg[i];
        uint32_t end = p->vaddr + p->memsz;
        for (uint32_t va = ALIGN_DOWN(p->vaddr, PAGE_SIZE); va < end && r == 0; va += PAGE_SIZE)
            if (vmm_alloc(pd, va) < 0)
                r = -ENOMEM;
        if (r == 0)
            r = image_copy_in(pd, img, p->vaddr, p->offset, p->filesz);
        *top = MAX(*top, end);
    }
    if (r == 0 && (*top == 0 || *entry < USER_BASE || *entry >= *top))
        r = -ENOEXEC;
    /* The segments' own permissions: code and read-only data stay as the
     * compiler left them, unless a writable segment shares the page. */
    for (int i = 0; i < nseg && r == 0; i++) {
        const struct segment *p = &seg[i];
        if (p->flags & PF_W)
            continue;
        uint32_t prot = PROT_READ | ((p->flags & PF_X) ? PROT_EXEC : 0);
        for (uint32_t va = ALIGN_DOWN(p->vaddr, PAGE_SIZE); va < p->vaddr + p->memsz; va += PAGE_SIZE)
            if (!page_writable(seg, nseg, va))
                vmm_protect_page(pd, va, prot);
    }
    kfree(seg);
    return r;
}

/* word: the size of the stack's words, 4 bytes for a 32-bit program and 8 for a 64-bit one;
 * top: where the stack ends. */
static int build_stack(pte_t *pd, char *const argv[], char *const envp[], uint32_t word, uint64_t top,
                       uint64_t *sp_out)
{
    uint32_t str_bytes = 0;
    int argc = count_strings(argv, &str_bytes);
    int envc = count_strings(envp, &str_bytes);
    if (argc < 0 || envc < 0 || str_bytes > MAX_ARG_BYTES)
        return -E2BIG;

    uint32_t nptrs = 1 + (uint32_t)argc + 1 + (uint32_t)envc + 1;
    uint64_t str_base = top - ALIGN_UP(str_bytes, 16u);
    uint64_t sp = ALIGN_DOWN(str_base - nptrs * word, 16u);
    uint32_t size = (uint32_t)(top - sp);

    uint8_t *frame = kzalloc(size);
    if (!frame)
        return -ENOMEM;
    /* (The frame is zeroed: a word's high half stays 0, and so do the two NULLs.) */
    uint8_t *ptrs = frame;
    char *strs = (char *)frame + (str_base - sp);
    uint64_t va = str_base;
    uint32_t k = 0;

    memcpy(ptrs + word * k++, &(uint32_t){ (uint32_t)argc }, 4);
    for (int i = 0; i < argc; i++) {
        uint32_t len = strlen(argv[i]) + 1;
        memcpy(strs, argv[i], len);
        memcpy(ptrs + word * k++, &va, word);
        strs += len;
        va += len;
    }
    k++;
    for (int i = 0; i < envc; i++) {
        uint32_t len = strlen(envp[i]) + 1;
        memcpy(strs, envp[i], len);
        memcpy(ptrs + word * k++, &va, word);
        strs += len;
        va += len;
    }

    int r = 0;
    for (uint64_t pg = ALIGN_DOWN(sp, PAGE_SIZE) - STACK_SLACK; pg < top && !r; pg += PAGE_SIZE)
        r = vmm_alloc(pd, pg);
    if (!r)
        r = vmm_copy_to(pd, sp, frame, size);
    kfree(frame);
    *sp_out = sp;
    return r;
}

/* Run interp, with its argument iarg if there is one (NULL: none), then the program's path,
 * followed by argv[1..]. */
static int exec_interp(struct proc *p, const char *interp, const char *iarg, const char *path,
                       char *const argv[], char *const envp[])
{
    int argc = 0;
    while (argv && argv[argc])
        argc++;
    char **nargv = kmalloc(sizeof(char *) * (uint32_t)(argc + 4));
    if (!nargv)
        return -ENOMEM;
    int k = 0;
    nargv[k++] = (char *)interp;
    if (iarg)
        nargv[k++] = (char *)iarg;
    nargv[k++] = (char *)path;
    for (int a = 1; a < argc; a++)
        nargv[k++] = argv[a];
    nargv[k] = NULL;

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

static int hash_bang_blank(uint8_t c)
{
    return c == ' ' || c == '\t';
}

/* "#!/path/to/interp [arg]" -> run interp (with arg, the rest of the line as one argument, as
 * Linux has it) with the script path after them. */
static int exec_script(struct proc *p, struct image *vn, const char *path,
                       char *const argv[], char *const envp[])
{
    char interp[256], arg[256];         /* (a #! line is short) */
    uint32_t i = 2, n = 0, m = 0;
    while (i < vn->have && hash_bang_blank(vn->data[i]))
        i++;
    while (i < vn->have && n < sizeof(interp) - 1 && vn->data[i] != '\n' &&
           !hash_bang_blank(vn->data[i]) && vn->data[i] != '\r')
        interp[n++] = (char)vn->data[i++];
    interp[n] = 0;
    while (i < vn->have && hash_bang_blank(vn->data[i]))
        i++;
    while (i < vn->have && m < sizeof(arg) - 1 && vn->data[i] != '\n' && vn->data[i] != '\r')
        arg[m++] = (char)vn->data[i++];
    while (m && hash_bang_blank((uint8_t)arg[m - 1]))
        m--;
    arg[m] = 0;
    image_free(vn);
    if (!n)
        return -ENOEXEC;
    return exec_interp(p, interp, m ? arg : NULL, path, argv, envp);
}

/* A Windows program: an "MZ" executable whose header points at a "PE" one. */
static int is_pe_program(const struct image *img)
{
    if (img->have < 0x40 || !((img->data[0] == 'M' && img->data[1] == 'Z') ||
                              (img->data[0] == 'Z' && img->data[1] == 'M')))
        return 0;
    uint32_t off;
    memcpy(&off, img->data + 0x3C, 4);
    if (off < 0x40 || off > img->size - 4)
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
        return exec_interp(p, WIN_LOADER, NULL, path, argv, envp);
    }
    if (is_dos_program(&img, path)) {
        image_free(&img);
        return exec_interp(p, DOS_LOADER, NULL, path, argv, envp);
    }

    pte_t *pd = vmm_create();
    if (!pd) {
        image_free(&img);
        return -ENOMEM;
    }

    uint32_t entry, top;
    uint64_t sp = 0;
    int is64 = 0;
    r = load_segments(pd, &img, &entry, &top, &is64);
    image_free(&img);
    /* A 64-bit program's stack and heap are where a 32-bit one's cannot be (kernel.h). */
    if (r == 0)
        r = build_stack(pd, argv, envp, is64 ? 8 : 4, is64 ? USER64_TOP : USER_STACK_TOP, &sp);
    if (r < 0) {
        vmm_destroy(pd);
        return r;
    }

    /* Only this thread goes on, as the process. The others are waited
     * for without the VFS lock: one waiting for it could not die. */
    int vfs_held = vfs_locked();
    if (vfs_held)
        vfs_unlock();
    r = proc_exec_alone(p);
    if (vfs_held)
        vfs_lock();
    if (r < 0) {
        vmm_destroy(pd);
        return r;
    }

    /* Point of no return: replace the old image. The screen, the 3D
     * engine and buffers lent to the GPU were the old image's: give them
     * up before its memory is freed, as at exit. */
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
    p->leader->heap_start = p->leader->brk = is64 ? USER64_HEAP_BASE : ALIGN_UP(top, PAGE_SIZE);
    p->leader->stack_low = ALIGN_DOWN(sp, PAGE_SIZE) - STACK_SLACK;

    const char *base = strrchr(path, '/');
    strlcpy(p->name, base ? base + 1 : path, sizeof(p->name));

    struct trapframe *tf = p->tf;
    memset(tf, 0, sizeof(*tf));
    p->user64 = is64;
    tf->cs = is64 ? SEG_UCODE64 : SEG_UCODE;
    tf->ds = tf->es = tf->fs = tf->gs = tf->ss = SEG_UDATA;
    if (is64)
        tf->fs = tf->gs = 0;                /* no selectors: a 64-bit program's FS has a base of its own (proc_load_tls) */
    tf->eflags = 0x202;                     /* IF set */
    tf->rip = entry;
    tf->rsp = sp;
    /* No thread block yet, of the kind of program it now is. */
    uint32_t flags = irq_save();
    proc_load_tls(p);
    irq_restore(flags);
    return 0;
}

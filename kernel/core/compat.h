/*
 * A 32-bit program's layout of the <flux/abi.h> structures whose fields are
 * as wide as a pointer (addresses, and the sizes of memory and of files):
 * what the kernel, which is 64-bit code and has the 64-bit layout under the
 * structures' own names, reads from and writes to such a program's memory.
 */
#ifndef COMPAT_H
#define COMPAT_H

#include <stdint.h>

struct flux_siginfo32 {
    int32_t  fsi_signo, fsi_errno, fsi_code, fsi_pid;
    uint32_t fsi_uid;
    int32_t  fsi_status;
    uint32_t fsi_addr;
};

struct flux_sigaction32 {
    uint32_t fsa_handler, fsa_mask, fsa_flags, fsa_restorer;
};

struct flux_stack32 {
    uint32_t sp, flags, size;
};

struct flux_mmap_args32 {
    uint32_t addr, len, prot, flags;
    int32_t  fd;
    uint32_t offset;
};

struct stat32 {
    uint32_t st_dev, st_ino, st_mode, st_nlink, st_uid, st_gid, st_rdev;
    int32_t  st_size;
    uint32_t st_blksize, st_blocks;
    int32_t  st_atime, st_mtime, st_ctime;
};

struct dispbuf32 {
    uint32_t addr, size, handle, gpu_lo, gpu_hi;
};

#endif

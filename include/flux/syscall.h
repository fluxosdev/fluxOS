/*
 * System call numbers. Invoked with `int $0x80`:
 *   eax = number, ebx/ecx/edx/esi/edi = arguments, eax = result.
 * Results in [-4095, -1] are negated errno values.
 */
#ifndef FLUX_SYSCALL_H
#define FLUX_SYSCALL_H

#define SYS_exit       1
#define SYS_fork       2
#define SYS_read       3
#define SYS_write      4
#define SYS_open       5
#define SYS_close      6
#define SYS_waitpid    7
#define SYS_execve     8
#define SYS_chdir      9
#define SYS_time      10
#define SYS_getpid    11
#define SYS_getppid   12
#define SYS_kill      13
#define SYS_mkdir     14
#define SYS_rmdir     15
#define SYS_unlink    16
#define SYS_dup       17
#define SYS_dup2      18
#define SYS_pipe      19
#define SYS_sbrk      20
#define SYS_sleep     21
#define SYS_lseek     22
#define SYS_stat      23
#define SYS_fstat     24
#define SYS_getcwd    25
#define SYS_getdents  26
#define SYS_uname     27
#define SYS_yield     28
#define SYS_signal    29
#define SYS_setpgid   30
#define SYS_getpgid   31
#define SYS_tcsetpgrp 32
#define SYS_tcgetpgrp 33
#define SYS_ttymode   34
#define SYS_procinfo  35
#define SYS_meminfo   36
#define SYS_uptime    37
#define SYS_reboot    38
#define SYS_rename    39
#define SYS_dmesg     40
#define SYS_chmod     41
#define SYS_mount     42
#define SYS_umount    43
#define SYS_statfs    44
#define SYS_getmounts 45
#define SYS_blkinfo   46
#define SYS_sync      47
#define SYS_fsync     48
#define SYS_blkrescan 49
#define SYS_fbinfo    50
#define SYS_fbmap     51
#define SYS_openpty   52
#define SYS_poll      53
#define SYS_socket    54
#define SYS_bind      55
#define SYS_connect   56
#define SYS_listen    57
#define SYS_accept    58
#define SYS_sendto    59
#define SYS_recvfrom  60
#define SYS_shutdown  61
#define SYS_sockname  62
#define SYS_netinfo   63
#define SYS_netconf   64
#define SYS_ftruncate 65
#define SYS_display   66
#define SYS_ioctl     67
#define SYS_usbinfo   68

#define NSYSCALLS     69

#endif

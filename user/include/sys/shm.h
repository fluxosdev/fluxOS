/*
 * Shared memory segments. shmget makes one (IPC_PRIVATE) or finds one by
 * key, shmat maps it, shmdt unmaps it, and shmctl(id, IPC_RMID, NULL)
 * takes its id away: the memory lasts while anything still maps it. A
 * segment its creator never removed goes when the creator exits.
 */
#ifndef _SYS_SHM_H
#define _SYS_SHM_H

#ifdef __cplusplus
extern "C" {
#endif

#include <sys/ipc.h>

#define SHM_RDONLY 010000               /* not provided: mappings are read-write */

struct shmid_ds {
    size_t shm_segsz;
    pid_t shm_cpid;
    unsigned long shm_nattch;           /* not kept: 0 */
};

int shmget(key_t key, size_t size, int flags);
void *shmat(int id, const void *addr, int flags);     /* addr must be NULL */
int shmdt(const void *addr);
int shmctl(int id, int cmd, struct shmid_ds *buf);

#ifdef __cplusplus
}
#endif

#endif

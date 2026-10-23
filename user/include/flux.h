/* fluxOS-specific system calls. */
#ifndef _FLUX_H
#define _FLUX_H

#include <flux/abi.h>

int ttymode(int mode);                  /* returns old mode; -1 queries */
int procinfo(int slot, struct procinfo *pi);
int meminfo(struct meminfo *mi);
unsigned int uptime_ms(void);
int reboot(int cmd);
int dmesg(char *buf, unsigned int size);
int sched_yield(void);
int msleep(unsigned int ms);
int getmounts(char *buf, unsigned int size);
int blkinfo(int index, struct blkinfo *bi);
int blkrescan(const char *disk);         /* re-read the partition table */

#endif

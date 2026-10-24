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
int usbinfo(int index, struct usbinfo *ui); /* the index-th USB device plugged in */

/* Pseudo-terminal: fds[0] is the master, fds[1] the slave (a terminal). */
int openpty(int fds[2]);

/* The network interface. netconf with NETF_DHCP set asks DHCP for an address. */
int netinfo(struct netinfo *ni);
int netconf(const struct netinfo *ni);

/* The boot framebuffer. fbmap maps it and takes the screen from the console. */
int fbinfo(struct fbinfo *fi);
void *fbmap(void);

/* The display adapter: op is DISP_INFO, DISP_MODES (returns the number of
 * modes), DISP_SETMODE or DISP_MAP3D; see <flux/abi.h>. */
int display(int op, void *arg);

#endif

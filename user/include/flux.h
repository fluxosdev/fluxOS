/* fluxOS-specific system calls. */
#ifndef _FLUX_H
#define _FLUX_H

#ifdef __cplusplus
extern "C" {
#endif

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

/* Batteries, the AC adapter, the lid, suspend and the backlight: op is
 * POWER_INFO (arg: struct powerinfo *), POWER_SLEEP (returns once the
 * machine is back), or POWER_BRIGHTNESS (arg: a percentage, BRIGHTNESS_UP
 * or BRIGHTNESS_DOWN; returns the percentage set). See <flux/abi.h>. */
int power(int op, unsigned int arg);

/* Wireless networking: op is WIFI_STATUS, WIFI_SCAN, WIFI_BSS, WIFI_CONNECT or
 * WIFI_DISCONNECT, with the struct each takes (see <flux/abi.h>). */
int wifi(int op, void *arg);
/* The remembered network, /etc/wifi.conf (wificonf.c). Each returns 0, or -1 with errno. */
int wifi_conf_load(struct wifi_connect *c);
int wifi_conf_save(const struct wifi_connect *c);
int wifi_conf_forget(void);
/* A WPA2 password (8-63 characters, or the key as 64 hex digits) into the key. */
int wifi_make_psk(const char *ssid, unsigned int ssid_len, const char *pass, uint8_t psk[32]);

/* Pseudo-terminal: fds[0] is the master, fds[1] the slave (a terminal). */
int openpty(int fds[2]);

/* The network interface. netconf with NETF_DHCP set asks DHCP for an address. */
int netinfo(struct netinfo *ni);
int netconf(const struct netinfo *ni);

/* The boot framebuffer. fbmap maps it and takes the screen from the console. */
int fbinfo(struct fbinfo *fi);
void *fbmap(void);

/* The display adapter: op is DISP_INFO, DISP_MODES (returns the number of
 * modes), DISP_SETMODE, DISP_MAP3D, DISP_FLIP, DISP_CURSOR or
 * DISP_CURSORPOS; see <flux/abi.h>. */
int display(int op, void *arg);

#ifdef __cplusplus
}
#endif

#endif

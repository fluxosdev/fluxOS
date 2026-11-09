/*
 * The wireless icon in the taskbar's tray: there when the kernel has a
 * wireless card, crossed out while it is not connected. Clicking it opens
 * Wireless Networks (wireless.c), a program of its own.
 */
#include <flux.h>
#include <string.h>
#include "desktop.h"

static int tray_present, tray_connected;

static void read_tray(int *present, int *connected)
{
    struct wifi_status st;
    if (wifi(WIFI_STATUS, &st) < 0)
        memset(&st, 0, sizeof(st));
    *present = st.state != WIFI_ST_NONE;
    *connected = st.state == WIFI_ST_CONNECTED;
}

void wireless_init(void)
{
    read_tray(&tray_present, &tray_connected);
}

int wireless_tray_present(void)
{
    return tray_present;
}

int wireless_tray_connected(void)
{
    return tray_connected;
}

void wireless_tray_tick(void)
{
    int present, connected;
    read_tray(&present, &connected);
    if (present != tray_present || connected != tray_connected) {
        tray_present = present;
        tray_connected = connected;
        wm_tray_changed();
    }
}

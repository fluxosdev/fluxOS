/*
 * wifi - join and manage wireless networks
 *
 *   wifi                       the card and the connection
 *   wifi scan                  list the networks in range
 *   wifi connect SSID [PASS]   join a network (asks for the password of a
 *                              protected one when not given) and remember it
 *   wifi disconnect            leave, and stop reconnecting
 *   wifi forget                leave, and forget the remembered network
 *   wifi auto                  join the remembered network (init runs this)
 *
 * The remembered network is in /etc/wifi.conf: its name, and the key
 * derived from the password (not the password itself).
 */
#include <errno.h>
#include <flux.h>
#include <flux/sha1.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>

#define CONF "/etc/wifi.conf"

static const char *state_name(uint32_t st)
{
    static const char *names[] = { "no wireless card", "starting", "failed", "disconnected",
                                   "scanning", "connecting", "connected" };
    return st < sizeof(names) / sizeof(names[0]) ? names[st] : "?";
}

static const char *sec_name(int sec)
{
    return sec == WIFI_SEC_OPEN ? "open" : sec == WIFI_SEC_WPA2_PSK ? "WPA2" : "unsupported";
}

static int get_status(struct wifi_status *st)
{
    if (wifi(WIFI_STATUS, st) < 0) {
        perror("wifi");
        return -1;
    }
    if (st->state == WIFI_ST_NONE) {
        fprintf(stderr, "wifi: no wireless card\n");
        return -1;
    }
    return 0;
}

static int status(void)
{
    struct wifi_status st;
    if (get_status(&st) < 0)
        return 1;
    printf("Card:     %s, firmware %s\n", st.driver, st.firmware[0] ? st.firmware : "-");
    printf("Address:  %02x:%02x:%02x:%02x:%02x:%02x\n", st.mac[0], st.mac[1], st.mac[2],
           st.mac[3], st.mac[4], st.mac[5]);
    printf("State:    %s\n", state_name(st.state));
    if (st.ssid[0])
        printf("Network:  %s\n", st.ssid);
    if (st.state == WIFI_ST_CONNECTED || st.state == WIFI_ST_CONNECTING) {
        if (st.channel)
            printf("Access point: %02x:%02x:%02x:%02x:%02x:%02x, channel %u (%u MHz), %d dBm, %s\n",
                   st.bssid[0], st.bssid[1], st.bssid[2], st.bssid[3], st.bssid[4], st.bssid[5],
                   st.channel, st.freq, st.rssi, sec_name(st.security));
    }
    if (st.error[0])
        printf("Last error: %s\n", st.error);
    return 0;
}

static int cmp_rssi(const void *a, const void *b)
{
    return ((const struct wifi_bss_info *)b)->rssi - ((const struct wifi_bss_info *)a)->rssi;
}

/* Scan and wait for it to finish. 0, or -1. */
static int do_scan(int quiet)
{
    struct wifi_status st;
    if (get_status(&st) < 0)
        return -1;
    uint32_t before = st.scans;
    if (wifi(WIFI_SCAN, NULL) < 0) {
        perror("wifi: scan");
        return -1;
    }
    for (int i = 0; i < 150; i++) {     /* 15 seconds */
        msleep(100);
        if (wifi(WIFI_STATUS, &st) == 0 && st.scans != before)
            return 0;
    }
    if (!quiet)
        fprintf(stderr, "wifi: the scan did not finish\n");
    return -1;
}

static int scan(void)
{
    if (do_scan(0) < 0)
        return 1;
    static struct wifi_bss_info list[64];
    int n = 0;
    while (n < 64) {
        list[n].index = (uint32_t)n;
        if (wifi(WIFI_BSS, &list[n]) < 0)
            break;
        n++;
    }
    qsort(list, (size_t)n, sizeof(list[0]), cmp_rssi);
    printf("%-32s %-17s %4s %5s  %s\n", "NETWORK", "ACCESS POINT", "CHAN", "dBm", "SECURITY");
    for (int i = 0; i < n; i++) {
        const struct wifi_bss_info *b = &list[i];
        printf("%-32s %02x:%02x:%02x:%02x:%02x:%02x %4u %5d  %s\n",
               b->ssid_len ? b->ssid : "(hidden)", b->bssid[0], b->bssid[1], b->bssid[2],
               b->bssid[3], b->bssid[4], b->bssid[5], b->channel, b->rssi, sec_name(b->security));
    }
    if (!n)
        printf("No networks found.\n");
    return 0;
}

/* The security of the strongest access point of a network in the last scan, or -1. */
static int network_security(const char *ssid)
{
    struct wifi_bss_info b;
    int best = -1, best_rssi = -1000;
    for (uint32_t i = 0;; i++) {
        b.index = i;
        if (wifi(WIFI_BSS, &b) < 0)
            break;
        if (!strcmp(b.ssid, ssid) && b.rssi > best_rssi) {
            best = b.security;
            best_rssi = b.rssi;
        }
    }
    return best;
}

static void read_password(char *buf, size_t size)
{
    struct termios old, quiet;
    int tty = tcgetattr(0, &old) == 0;
    if (tty) {
        quiet = old;
        quiet.c_lflag &= ~(tcflag_t)ECHO;
        tcsetattr(0, TCSANOW, &quiet);
    }
    printf("Password: ");
    fflush(stdout);
    if (!fgets(buf, (int)size, stdin))
        buf[0] = 0;
    buf[strcspn(buf, "\r\n")] = 0;
    if (tty) {
        tcsetattr(0, TCSANOW, &old);
        printf("\n");
    }
}

static int hexval(int c)
{
    return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10
         : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
}

/* A passphrase of 8-63 characters, or the key itself as 64 hex digits. */
static int make_psk(const char *ssid, const char *pass, uint8_t psk[32])
{
    size_t len = strlen(pass);
    if (len == 64) {
        for (int i = 0; i < 32; i++) {
            int hi = hexval(pass[2 * i]), lo = hexval(pass[2 * i + 1]);
            if (hi < 0 || lo < 0)
                goto passphrase;
            psk[i] = (uint8_t)(hi << 4 | lo);
        }
        return 0;
    }
passphrase:
    if (len < 8 || len > 63) {
        fprintf(stderr, "wifi: a WPA2 password has 8 to 63 characters\n");
        return -1;
    }
    pbkdf2_sha1(pass, len, (const uint8_t *)ssid, strlen(ssid), 4096, psk, 32);
    return 0;
}

static int save_conf(const struct wifi_connect *c)
{
    FILE *f = fopen(CONF, "w");
    if (!f)
        return -1;
    fprintf(f, "# The wireless network to join at startup (wifi connect / wifi forget).\n");
    fprintf(f, "ssid=%.*s\n", c->ssid_len, c->ssid);
    if (c->has_psk) {
        fprintf(f, "psk=");
        for (int i = 0; i < 32; i++)
            fprintf(f, "%02x", c->psk[i]);
        fprintf(f, "\n");
    }
    return fclose(f);
}

static int load_conf(struct wifi_connect *c)
{
    FILE *f = fopen(CONF, "r");
    if (!f)
        return -1;
    memset(c, 0, sizeof(*c));
    char line[160];
    while (fgets(line, sizeof(line), f)) {
        line[strcspn(line, "\r\n")] = 0;
        if (!strncmp(line, "ssid=", 5)) {
            size_t n = strlen(line + 5);
            if (n > sizeof(c->ssid))
                n = sizeof(c->ssid);
            memcpy(c->ssid, line + 5, n);
            c->ssid_len = (uint8_t)n;
        } else if (!strncmp(line, "psk=", 4) && strlen(line + 4) == 64) {
            for (int i = 0; i < 32; i++)
                c->psk[i] = (uint8_t)(hexval(line[4 + 2 * i]) << 4 | hexval(line[5 + 2 * i]));
            c->has_psk = 1;
        }
    }
    fclose(f);
    return c->ssid_len ? 0 : -1;
}

/* Follow a connection attempt until it succeeds or fails. */
static int wait_connected(void)
{
    struct wifi_status st;
    int saw_attempt = 0;
    for (int i = 0; i < 300; i++) {     /* 30 seconds */
        msleep(100);
        if (wifi(WIFI_STATUS, &st) < 0)
            return 1;
        if (st.state == WIFI_ST_CONNECTED) {
            printf("Connected to %s (channel %u, %d dBm).\n", st.ssid, st.channel, st.rssi);
            return 0;
        }
        if (st.state == WIFI_ST_CONNECTING || st.state == WIFI_ST_STARTING)
            saw_attempt = 1;
        if (saw_attempt && st.error[0] && st.state != WIFI_ST_CONNECTED) {
            fprintf(stderr, "wifi: %s\n", st.error);
            if (st.state == WIFI_ST_IDLE)
                return 1;               /* given up */
            saw_attempt = 0;            /* report each new failure once */
            printf("Trying again...\n");
        }
    }
    fprintf(stderr, "wifi: not connected yet; it keeps trying in the background ('wifi' shows how it goes)\n");
    return 1;
}

static int connect_cmd(int argc, char **argv)
{
    int save = 1, i = 2;
    if (i < argc && !strcmp(argv[i], "-n")) {
        save = 0;
        i++;
    }
    if (i >= argc) {
        fprintf(stderr, "usage: wifi connect [-n] SSID [PASSWORD]\n");
        return 2;
    }
    const char *ssid = argv[i++];
    const char *pass = i < argc ? argv[i] : NULL;
    if (strlen(ssid) > 32) {
        fprintf(stderr, "wifi: a network name has at most 32 bytes\n");
        return 2;
    }
    struct wifi_connect c;
    memset(&c, 0, sizeof(c));
    memcpy(c.ssid, ssid, strlen(ssid));
    c.ssid_len = (uint8_t)strlen(ssid);
    char buf[128];
    if (!pass) {
        int sec = network_security(ssid);
        if (sec < 0 && do_scan(1) == 0)
            sec = network_security(ssid);
        if (sec == WIFI_SEC_WPA2_PSK) {
            read_password(buf, sizeof(buf));
            pass = buf;
        } else if (sec == WIFI_SEC_OTHER) {
            fprintf(stderr, "wifi: %s uses security fluxOS does not support (WPA3, Enterprise, WEP...)\n", ssid);
            return 1;
        }
    }
    if (pass && pass[0]) {
        if (make_psk(ssid, pass, c.psk) < 0)
            return 2;
        c.has_psk = 1;
    }
    memset(buf, 0, sizeof(buf));
    if (wifi(WIFI_CONNECT, &c) < 0) {
        perror("wifi: connect");
        return 1;
    }
    printf("Connecting to %s...\n", ssid);
    int r = wait_connected();
    if (r == 0 && save && save_conf(&c) < 0)
        fprintf(stderr, "wifi: could not save %s: %s\n", CONF, strerror(errno));
    return r;
}

int main(int argc, char **argv)
{
    const char *cmd = argc > 1 ? argv[1] : "status";
    if (!strcmp(cmd, "status"))
        return status();
    if (!strcmp(cmd, "scan"))
        return scan();
    if (!strcmp(cmd, "connect"))
        return connect_cmd(argc, argv);
    if (!strcmp(cmd, "disconnect") || !strcmp(cmd, "forget")) {
        if (wifi(WIFI_DISCONNECT, NULL) < 0) {
            perror("wifi");
            return 1;
        }
        if (!strcmp(cmd, "forget") && unlink(CONF) < 0 && errno != ENOENT) {
            fprintf(stderr, "wifi: %s: %s\n", CONF, strerror(errno));
            return 1;
        }
        return 0;
    }
    if (!strcmp(cmd, "auto")) {
        struct wifi_connect c;
        struct wifi_status st;
        if (load_conf(&c) < 0 || wifi(WIFI_STATUS, &st) < 0 || st.state == WIFI_ST_NONE)
            return 0;                   /* nothing remembered, or no card: nothing to do */
        return wifi(WIFI_CONNECT, &c) < 0;
    }
    fprintf(stderr, "usage: wifi [status | scan | connect [-n] SSID [PASSWORD] | disconnect | forget | auto]\n");
    return 2;
}

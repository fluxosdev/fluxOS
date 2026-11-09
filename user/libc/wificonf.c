/*
 * The remembered wireless network, /etc/wifi.conf, shared by the wifi
 * command and the desktop's Wireless Networks: its name, and the key
 * derived from its password (never the password itself).
 *
 *   ssid=HomeNet
 *   psk=<64 hex digits>        (absent for an open network)
 */
#include <errno.h>
#include <flux.h>
#include <flux/sha1.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define WIFI_CONF "/etc/wifi.conf"

static int hexval(int c)
{
    return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10
         : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
}

/* 2n hex digits into n bytes: 0, or -1 if any is not a hex digit. */
static int unhex(const char *s, uint8_t *out, int n)
{
    for (int i = 0; i < n; i++) {
        int hi = hexval(s[2 * i]), lo = hi < 0 ? -1 : hexval(s[2 * i + 1]);
        if (lo < 0)
            return -1;
        out[i] = (uint8_t)(hi << 4 | lo);
    }
    return 0;
}

int wifi_make_psk(const char *ssid, unsigned int ssid_len, const char *pass, uint8_t psk[32])
{
    size_t len = strlen(pass);
    if (len == 64 && unhex(pass, psk, 32) == 0)
        return 0;                       /* the key itself */
    if (len < 8 || len > 63) {
        errno = EINVAL;
        return -1;
    }
    pbkdf2_sha1(pass, len, (const uint8_t *)ssid, ssid_len, 4096, psk, 32);
    return 0;
}

int wifi_conf_load(struct wifi_connect *c)
{
    FILE *f = fopen(WIFI_CONF, "r");
    if (!f)
        return -1;
    memset(c, 0, sizeof(*c));
    char line[160];
    int bad = 0;
    while (fgets(line, sizeof(line), f)) {
        line[strcspn(line, "\r\n")] = 0;
        if (!strncmp(line, "ssid=", 5)) {
            size_t n = strlen(line + 5);
            if (n > sizeof(c->ssid))
                n = sizeof(c->ssid);
            memcpy(c->ssid, line + 5, n);
            c->ssid_len = (uint8_t)n;
        } else if (!strncmp(line, "psk=", 4)) {
            if (strlen(line + 4) != 64 || unhex(line + 4, c->psk, 32) < 0)
                bad = 1;
            else
                c->has_psk = 1;
        }
    }
    fclose(f);
    if (bad || !c->ssid_len) {
        memset(c, 0, sizeof(*c));
        errno = EINVAL;
        return -1;
    }
    return 0;
}

int wifi_conf_save(const struct wifi_connect *c)
{
    FILE *f = fopen(WIFI_CONF, "w");
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

int wifi_conf_forget(void)
{
    return unlink(WIFI_CONF) < 0 && errno != ENOENT ? -1 : 0;
}

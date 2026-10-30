/*
 * Calendar time: broken-down times both ways (any year, before 1970
 * too), the local time zone (a fixed offset from UTC, which the desktop
 * sets from its saved zone; no daylight saving), and strftime.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "libc.h"

static const char *const wday_names[] = { "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" };
static const char *const wday_full[] = { "Sunday", "Monday", "Tuesday", "Wednesday",
                                         "Thursday", "Friday", "Saturday" };
static const char *const mon_names[] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                         "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };
static const char *const mon_full[] = { "January", "February", "March", "April", "May", "June",
                                        "July", "August", "September", "October", "November",
                                        "December" };

/* ---- days and dates (proleptic Gregorian) ---- */

static long long days_from_civil(long long y, int m, int d)
{
    y -= m <= 2;
    long long era = (y >= 0 ? y : y - 399) / 400;
    long long yoe = y - era * 400;
    long long doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    long long doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

static void civil_from_days(long long z, long long *y, int *m, int *d)
{
    z += 719468;
    long long era = (z >= 0 ? z : z - 146096) / 146097;
    long long doe = z - era * 146097;
    long long yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    long long doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    long long mp = (5 * doy + 2) / 153;
    *d = (int)(doy - (153 * mp + 2) / 5 + 1);
    *m = (int)(mp < 10 ? mp + 3 : mp - 9);
    *y = yoe + era * 400 + (*m <= 2);
}

static int is_leap(long long y)
{
    return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0;
}

static int yday_of(long long y, int mon, int mday)
{
    return (int)(days_from_civil(y, mon + 1, mday) - days_from_civil(y, 1, 1));
}

/* Seconds since 1970 into a broken-down time, offset seconds east of UTC added. */
static struct tm *breakdown(long long t, long offset, const char *zone, struct tm *tm)
{
    t += offset;
    long long days = t >= 0 ? t / 86400 : -((-t + 86399) / 86400);
    long long rem = t - days * 86400;
    long long y;
    int m, d;
    civil_from_days(days, &y, &m, &d);
    if (y - 1900 > 0x7FFFFFFF || y - 1900 < -0x7FFFFFFF - 1)
        return NULL;
    tm->tm_year = (int)(y - 1900);
    tm->tm_mon = m - 1;
    tm->tm_mday = d;
    tm->tm_hour = (int)(rem / 3600);
    tm->tm_min = (int)(rem % 3600 / 60);
    tm->tm_sec = (int)(rem % 60);
    tm->tm_wday = (int)(((days % 7) + 11) % 7);         /* 1970-01-01 was a Thursday */
    tm->tm_yday = yday_of(y, m - 1, d);
    tm->tm_isdst = 0;
    tm->tm_gmtoff = offset;
    tm->tm_zone = zone;
    return tm;
}

/* ---- the time zone ---- */

static int tz_minutes;
static char zone_name[8] = "UTC";
char *tzname[2] = { zone_name, zone_name };
long timezone;
int daylight;

static void zone_update(void)
{
    if (!tz_minutes) {
        strcpy(zone_name, "UTC");
    } else {
        int m = tz_minutes < 0 ? -tz_minutes : tz_minutes;
        if (m % 60)
            snprintf(zone_name, sizeof(zone_name), "%c%02d%02d", tz_minutes < 0 ? '-' : '+', m / 60, m % 60);
        else
            snprintf(zone_name, sizeof(zone_name), "%c%02d", tz_minutes < 0 ? '-' : '+', m / 60);
    }
    timezone = -(long)tz_minutes * 60;
}

void tz_set(int minutes)
{
    tz_minutes = minutes;
    zone_update();
}

int tz_get(void) { return tz_minutes; }

long __tz_offset(void)
{
    return (long)tz_minutes * 60;
}

/* TZ="UTC", "UTC0", "<+0530>-5:30" or "EST5": a fixed offset, if it says one. */
void tzset(void)
{
    const char *tz = getenv("TZ");
    if (tz && *tz) {
        const char *p = tz;
        if (*p == ':')
            p++;
        if (*p == '<') {
            while (*p && *p != '>')
                p++;
            if (*p)
                p++;
        } else {
            while ((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z'))
                p++;
        }
        if (p == tz || !*p) {
            if (!strncmp(tz, "UTC", 3) || !strncmp(tz, "GMT", 3))
                tz_minutes = 0;
        } else {
            int sign = 1;
            if (*p == '+' || *p == '-')
                sign = *p++ == '-' ? -1 : 1;
            int h = 0, mi = 0;
            while (*p >= '0' && *p <= '9')
                h = h * 10 + (*p++ - '0');
            if (*p == ':') {
                p++;
                while (*p >= '0' && *p <= '9')
                    mi = mi * 10 + (*p++ - '0');
            }
            tz_minutes = -sign * (h * 60 + mi);   /* POSIX TZ offsets are west of UTC */
        }
    }
    zone_update();
}

/* ---- conversions ---- */

struct tm *gmtime_r(const time_t *t, struct tm *tm)
{
    return breakdown((long long)*t, 0, "UTC", tm);
}

struct tm *gmtime(const time_t *t)
{
    static struct tm tm;
    return gmtime_r(t, &tm);
}

struct tm *localtime_r(const time_t *t, struct tm *tm)
{
    return breakdown((long long)*t, (long)tz_minutes * 60, zone_name, tm);
}

struct tm *localtime(const time_t *t)
{
    static struct tm tm;
    return localtime_r(t, &tm);
}

/* A broken-down time with fields out of range made right, as seconds. */
static long long normalize(struct tm *tm, long offset)
{
    long long y = (long long)tm->tm_year + 1900;
    long long mon = tm->tm_mon;
    y += mon / 12;
    mon %= 12;
    if (mon < 0) {
        mon += 12;
        y--;
    }
    long long days = days_from_civil(y, (int)mon + 1, 1) + (long long)tm->tm_mday - 1;
    long long t = days * 86400 + (long long)tm->tm_hour * 3600 + (long long)tm->tm_min * 60 + tm->tm_sec;
    return t - offset;
}

time_t timegm(struct tm *tm)
{
    long long t = normalize(tm, 0);
    if (t > 0x7FFFFFFF || t < -0x7FFFFFFFLL - 1 || !breakdown(t, 0, "UTC", tm))
        return (time_t)-1;
    return (time_t)t;
}

time_t mktime(struct tm *tm)
{
    long offset = (long)tz_minutes * 60;
    long long t = normalize(tm, offset);
    if (t > 0x7FFFFFFF || t < -0x7FFFFFFFLL - 1 || !breakdown(t, offset, zone_name, tm))
        return (time_t)-1;
    return (time_t)t;
}

double difftime(time_t a, time_t b)
{
    return (double)a - (double)b;
}

char *asctime_r(const struct tm *tm, char *buf)
{
    snprintf(buf, 26, "%.3s %.3s%3d %.2d:%.2d:%.2d %d\n",
             tm->tm_wday >= 0 && tm->tm_wday < 7 ? wday_names[tm->tm_wday] : "???",
             tm->tm_mon >= 0 && tm->tm_mon < 12 ? mon_names[tm->tm_mon] : "???",
             tm->tm_mday, tm->tm_hour, tm->tm_min, tm->tm_sec, tm->tm_year + 1900);
    return buf;
}

char *asctime(const struct tm *tm)
{
    static char buf[32];
    return asctime_r(tm, buf);
}

char *ctime_r(const time_t *t, char *buf)
{
    struct tm tm;
    if (!localtime_r(t, &tm))
        return NULL;
    return asctime_r(&tm, buf);
}

char *ctime(const time_t *t)
{
    static char buf[32];
    return ctime_r(t, buf);
}

/* ---- strftime ---- */

/* ISO 8601 week-based year and week number. */
static void iso_week(const struct tm *tm, long long *year, int *week)
{
    long long y = (long long)tm->tm_year + 1900;
    int wday = (tm->tm_wday + 6) % 7;           /* Monday 0 */
    int w = (tm->tm_yday - wday + 10) / 7;
    if (w < 1) {
        y--;
        int pyday = tm->tm_yday + (is_leap(y) ? 366 : 365);
        w = (pyday - wday + 10) / 7;
    } else if (w == 53) {
        int days = is_leap(y) ? 366 : 365;
        if (tm->tm_yday - wday + 3 >= days) {   /* the Thursday of this week is next year's */
            y++;
            w = 1;
        }
    }
    *year = y;
    *week = w;
}

static size_t format(char *buf, size_t size, const char *fmt, const struct tm *tm, int depth)
{
    size_t n = 0;
    char tmp[64];
    for (; *fmt; fmt++) {
        const char *s = tmp;
        size_t len;
        if (*fmt != '%') {
            tmp[0] = *fmt;
            tmp[1] = 0;
        } else {
            tmp[0] = 0;
            fmt++;
            while (*fmt == 'E' || *fmt == 'O' || *fmt == '_' || *fmt == '-' || *fmt == '0' || *fmt == '^' || *fmt == '#')
                fmt++;                  /* modifiers: the "C" locale's forms are the plain ones */
            long long iy;
            int iw;
            switch (*fmt) {
            case 'Y': snprintf(tmp, sizeof(tmp), "%lld", (long long)tm->tm_year + 1900); break;
            case 'C': snprintf(tmp, sizeof(tmp), "%02lld", ((long long)tm->tm_year + 1900) / 100); break;
            case 'y': snprintf(tmp, sizeof(tmp), "%02d", (int)(((tm->tm_year + 1900) % 100 + 100) % 100)); break;
            case 'G': iso_week(tm, &iy, &iw); snprintf(tmp, sizeof(tmp), "%lld", iy); break;
            case 'g': iso_week(tm, &iy, &iw); snprintf(tmp, sizeof(tmp), "%02d", (int)((iy % 100 + 100) % 100)); break;
            case 'V': iso_week(tm, &iy, &iw); snprintf(tmp, sizeof(tmp), "%02d", iw); break;
            case 'U': snprintf(tmp, sizeof(tmp), "%02d", (tm->tm_yday + 7 - tm->tm_wday) / 7); break;
            case 'W': snprintf(tmp, sizeof(tmp), "%02d", (tm->tm_yday + 7 - (tm->tm_wday + 6) % 7) / 7); break;
            case 'm': snprintf(tmp, sizeof(tmp), "%02d", tm->tm_mon + 1); break;
            case 'd': snprintf(tmp, sizeof(tmp), "%02d", tm->tm_mday); break;
            case 'e': snprintf(tmp, sizeof(tmp), "%2d", tm->tm_mday); break;
            case 'H': snprintf(tmp, sizeof(tmp), "%02d", tm->tm_hour); break;
            case 'k': snprintf(tmp, sizeof(tmp), "%2d", tm->tm_hour); break;
            case 'I': snprintf(tmp, sizeof(tmp), "%02d", tm->tm_hour % 12 ? tm->tm_hour % 12 : 12); break;
            case 'l': snprintf(tmp, sizeof(tmp), "%2d", tm->tm_hour % 12 ? tm->tm_hour % 12 : 12); break;
            case 'M': snprintf(tmp, sizeof(tmp), "%02d", tm->tm_min); break;
            case 'S': snprintf(tmp, sizeof(tmp), "%02d", tm->tm_sec); break;
            case 'p': s = tm->tm_hour < 12 ? "AM" : "PM"; break;
            case 'P': s = tm->tm_hour < 12 ? "am" : "pm"; break;
            case 'j': snprintf(tmp, sizeof(tmp), "%03d", tm->tm_yday + 1); break;
            case 'u': snprintf(tmp, sizeof(tmp), "%d", tm->tm_wday ? tm->tm_wday : 7); break;
            case 'w': snprintf(tmp, sizeof(tmp), "%d", tm->tm_wday); break;
            case 'a': s = tm->tm_wday >= 0 && tm->tm_wday < 7 ? wday_names[tm->tm_wday] : "?"; break;
            case 'A': s = tm->tm_wday >= 0 && tm->tm_wday < 7 ? wday_full[tm->tm_wday] : "?"; break;
            case 'b': case 'h': s = tm->tm_mon >= 0 && tm->tm_mon < 12 ? mon_names[tm->tm_mon] : "?"; break;
            case 'B': s = tm->tm_mon >= 0 && tm->tm_mon < 12 ? mon_full[tm->tm_mon] : "?"; break;
            case 'Z': s = tm->tm_zone ? tm->tm_zone : "UTC"; break;
            case 'z': {
                long off = tm->tm_gmtoff;
                long a = off < 0 ? -off : off;
                snprintf(tmp, sizeof(tmp), "%c%02ld%02ld", off < 0 ? '-' : '+', a / 3600, a % 3600 / 60);
                break;
            }
            case 's': {
                struct tm copy = *tm;
                snprintf(tmp, sizeof(tmp), "%lld", (long long)normalize(&copy, tm->tm_gmtoff));
                break;
            }
            case 'T': snprintf(tmp, sizeof(tmp), "%02d:%02d:%02d", tm->tm_hour, tm->tm_min, tm->tm_sec); break;
            case 'R': snprintf(tmp, sizeof(tmp), "%02d:%02d", tm->tm_hour, tm->tm_min); break;
            case 'D': case 'x':
                snprintf(tmp, sizeof(tmp), "%02d/%02d/%02d", tm->tm_mon + 1, tm->tm_mday,
                         ((tm->tm_year + 1900) % 100 + 100) % 100);
                break;
            case 'F': snprintf(tmp, sizeof(tmp), "%lld-%02d-%02d", (long long)tm->tm_year + 1900, tm->tm_mon + 1, tm->tm_mday); break;
            case 'X': snprintf(tmp, sizeof(tmp), "%02d:%02d:%02d", tm->tm_hour, tm->tm_min, tm->tm_sec); break;
            case 'r':
                snprintf(tmp, sizeof(tmp), "%02d:%02d:%02d %s", tm->tm_hour % 12 ? tm->tm_hour % 12 : 12, tm->tm_min,
                         tm->tm_sec, tm->tm_hour < 12 ? "AM" : "PM");
                break;
            case 'c':
                if (depth > 2)
                    break;
                len = format(tmp, sizeof(tmp), "%a %b %e %H:%M:%S %Y", tm, depth + 1);
                break;
            case 'n': s = "\n"; break;
            case 't': s = "\t"; break;
            case '%': s = "%"; break;
            case 0: fmt--; s = ""; break;
            default: snprintf(tmp, sizeof(tmp), "%%%c", *fmt); break;
            }
        }
        len = strlen(s);
        if (n + len >= size)
            return 0;
        memcpy(buf + n, s, len);
        n += len;
    }
    buf[n] = 0;
    return n;
}

size_t strftime(char *buf, size_t size, const char *fmt, const struct tm *tm)
{
    if (!size)
        return 0;
    return format(buf, size, fmt, tm, 0);
}

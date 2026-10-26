/* Calendar conversion (UTC only) and strftime. */
#include <stdio.h>
#include <string.h>
#include <time.h>

static const char *wday_names[] = { "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" };
static const char *wday_full[] = { "Sunday", "Monday", "Tuesday", "Wednesday",
                                   "Thursday", "Friday", "Saturday" };
static const char *mon_names[] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                   "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };
static const char *mon_full[] = { "January", "February", "March", "April", "May", "June",
                                  "July", "August", "September", "October", "November",
                                  "December" };

static int is_leap(int y)
{
    return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0;
}

struct tm *gmtime_r(const time_t *t, struct tm *tm)
{
    static const int mdays[] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    unsigned int secs = (unsigned int)*t;
    unsigned int days = secs / 86400;
    unsigned int rem = secs % 86400;

    tm->tm_hour = (int)(rem / 3600);
    tm->tm_min = (int)(rem % 3600 / 60);
    tm->tm_sec = (int)(rem % 60);
    tm->tm_wday = (int)((days + 4) % 7);        /* 1970-01-01 was a Thursday */

    int year = 1970;
    for (;;) {
        unsigned int ylen = is_leap(year) ? 366 : 365;
        if (days < ylen)
            break;
        days -= ylen;
        year++;
    }
    tm->tm_year = year - 1900;
    tm->tm_yday = (int)days;

    int mon = 0;
    for (;; mon++) {
        unsigned int ml = (unsigned int)mdays[mon] + (mon == 1 && is_leap(year));
        if (days < ml)
            break;
        days -= ml;
    }
    tm->tm_mon = mon;
    tm->tm_mday = (int)days + 1;
    tm->tm_isdst = 0;
    return tm;
}

struct tm *gmtime(const time_t *t)
{
    static struct tm tm;
    return gmtime_r(t, &tm);
}

static int tz_minutes;

void tz_set(int minutes) { tz_minutes = minutes; }
int tz_get(void) { return tz_minutes; }

struct tm *localtime(const time_t *t)
{
    time_t local = (time_t)((long)*t + tz_minutes * 60L);
    return gmtime(&local);
}

size_t strftime(char *buf, size_t size, const char *fmt, const struct tm *tm)
{
    size_t n = 0;
    char tmp[64];

    if (!size)
        return 0;
    for (; *fmt; fmt++) {
        const char *s = tmp;
        if (*fmt != '%') {
            tmp[0] = *fmt;
            tmp[1] = 0;
        } else {
            switch (*++fmt) {
            case 'Y': snprintf(tmp, sizeof(tmp), "%d", tm->tm_year + 1900); break;
            case 'y': snprintf(tmp, sizeof(tmp), "%02d", tm->tm_year % 100); break;
            case 'm': snprintf(tmp, sizeof(tmp), "%02d", tm->tm_mon + 1); break;
            case 'd': snprintf(tmp, sizeof(tmp), "%02d", tm->tm_mday); break;
            case 'e': snprintf(tmp, sizeof(tmp), "%2d", tm->tm_mday); break;
            case 'H': snprintf(tmp, sizeof(tmp), "%02d", tm->tm_hour); break;
            case 'I': snprintf(tmp, sizeof(tmp), "%02d", tm->tm_hour % 12 ? tm->tm_hour % 12 : 12); break;
            case 'M': snprintf(tmp, sizeof(tmp), "%02d", tm->tm_min); break;
            case 'S': snprintf(tmp, sizeof(tmp), "%02d", tm->tm_sec); break;
            case 'p': s = tm->tm_hour < 12 ? "AM" : "PM"; break;
            case 'j': snprintf(tmp, sizeof(tmp), "%03d", tm->tm_yday + 1); break;
            case 'a': s = wday_names[tm->tm_wday]; break;
            case 'A': s = wday_full[tm->tm_wday]; break;
            case 'b': case 'h': s = mon_names[tm->tm_mon]; break;
            case 'B': s = mon_full[tm->tm_mon]; break;
            case 'Z': s = "UTC"; break;
            case 'z': s = "+0000"; break;
            case 'T': snprintf(tmp, sizeof(tmp), "%02d:%02d:%02d", tm->tm_hour, tm->tm_min, tm->tm_sec); break;
            case 'R': snprintf(tmp, sizeof(tmp), "%02d:%02d", tm->tm_hour, tm->tm_min); break;
            case 'D': snprintf(tmp, sizeof(tmp), "%02d/%02d/%02d", tm->tm_mon + 1, tm->tm_mday, tm->tm_year % 100); break;
            case 'F': snprintf(tmp, sizeof(tmp), "%d-%02d-%02d", tm->tm_year + 1900, tm->tm_mon + 1, tm->tm_mday); break;
            case 'n': s = "\n"; break;
            case 't': s = "\t"; break;
            case '%': s = "%"; break;
            case 0: fmt--; s = ""; break;
            default: snprintf(tmp, sizeof(tmp), "%%%c", *fmt); break;
            }
        }
        size_t len = strlen(s);
        if (n + len >= size)
            return 0;
        memcpy(buf + n, s, len);
        n += len;
    }
    buf[n] = 0;
    return n;
}

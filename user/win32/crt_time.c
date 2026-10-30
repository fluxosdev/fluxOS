/*
 * The C runtime's time functions: time_t comes in 32-bit (time, _time32)
 * and 64-bit (_time64) sizes, struct tm is laid out as everywhere, and
 * the results of localtime, gmtime, asctime and ctime live in static
 * buffers in the program's memory.
 */
#include <stdio.h>
#include <string.h>
#include <time.h>
#include "crt.h"
#include "os.h"

#define A(i) (a[i])
#define S(i) gstr(a[i])
#define WS(i) gwstr(a[i])
#define P(i) gp(a[i])

static int64_t i64(const uint32_t *p) { int64_t v; memcpy(&v, p, 8); return v; }
static uint32_t ret64(uint64_t v) { api_edx = (uint32_t)(v >> 32); return (uint32_t)v; }

/* ---- calendar arithmetic (proleptic Gregorian, from the C++20 paper) ---- */

static int64_t days_from_civil(int64_t y, int m, int d)
{
    y -= m <= 2;
    int64_t era = (y >= 0 ? y : y - 399) / 400;
    int64_t yoe = y - era * 400;
    int64_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

static void civil_from_days(int64_t z, int *y, int *m, int *d)
{
    z += 719468;
    int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    int64_t doe = z - era * 146097;
    int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int64_t yy = yoe + era * 400;
    int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    int64_t mp = (5 * doy + 2) / 153;
    *d = (int)(doy - (153 * mp + 2) / 5 + 1);
    *m = (int)(mp < 10 ? mp + 3 : mp - 9);
    *y = (int)(yy + (*m <= 2));
}

static int is_leap(int y) { return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0; }

void crt_gmtime(int64_t t, struct tm *out)
{
    memset(out, 0, sizeof(*out));
    int64_t days = t >= 0 ? t / 86400 : -((-t + 86399) / 86400);
    int64_t rem = t - days * 86400;
    int y, m, d;
    civil_from_days(days, &y, &m, &d);
    out->tm_year = y - 1900;
    out->tm_mon = m - 1;
    out->tm_mday = d;
    out->tm_hour = (int)(rem / 3600);
    out->tm_min = (int)(rem % 3600 / 60);
    out->tm_sec = (int)(rem % 60);
    out->tm_wday = (int)((days % 7 + 11) % 7);  /* 1970-01-01 was a Thursday */
    out->tm_yday = (int)(days - days_from_civil(y, 1, 1));
    out->tm_isdst = 0;
}

void crt_localtime(int64_t t, struct tm *out)
{
    crt_gmtime(t + (int64_t)os_tz_minutes() * 60, out);
}

int64_t crt_mktime(const struct tm *tm, int local)
{
    /* normalize the month, then everything else falls out of the day count */
    int64_t y = tm->tm_year + 1900, m = tm->tm_mon;
    y += m >= 0 ? m / 12 : -((-m + 11) / 12);
    m = ((m % 12) + 12) % 12;
    int64_t days = days_from_civil(y, (int)m + 1, 1) + tm->tm_mday - 1;
    int64_t t = days * 86400 + (int64_t)tm->tm_hour * 3600 + (int64_t)tm->tm_min * 60 + tm->tm_sec;
    if (local)
        t -= (int64_t)os_tz_minutes() * 60;
    return t;
}

/* The program's struct tm: nine ints, as the host's begins. */
struct gtm { int32_t sec, min, hour, mday, mon, year, wday, yday, isdst; };

static void to_gtm(const struct tm *tm, struct gtm *g)
{
    g->sec = tm->tm_sec; g->min = tm->tm_min; g->hour = tm->tm_hour; g->mday = tm->tm_mday;
    g->mon = tm->tm_mon; g->year = tm->tm_year; g->wday = tm->tm_wday; g->yday = tm->tm_yday;
    g->isdst = tm->tm_isdst;
}

static void from_gtm(const struct gtm *g, struct tm *tm)
{
    memset(tm, 0, sizeof(*tm));
    tm->tm_sec = g->sec; tm->tm_min = g->min; tm->tm_hour = g->hour; tm->tm_mday = g->mday;
    tm->tm_mon = g->mon; tm->tm_year = g->year; tm->tm_wday = g->wday; tm->tm_yday = g->yday;
    tm->tm_isdst = g->isdst;
}

static gp_t tm_buf, asc_buf, wasc_buf;
static int64_t now_s(void) { return (int64_t)(os_time_us() / 1000000u); }

static gp_t guest_tm(int64_t t, int local)
{
    if (!tm_buf)
        tm_buf = vm_alloc_loader(sizeof(struct gtm));
    struct tm tm;
    if (local) crt_localtime(t, &tm); else crt_gmtime(t, &tm);
    to_gtm(&tm, GP(tm_buf));
    return tm_buf;
}

/* A time read from the program: 32 or 64 bits, through a pointer. */
static int64_t read_time(gp_t p, int wide)
{
    if (!p)
        return -1;
    return wide ? i64(GP(p)) : (int64_t)*(int32_t *)GP(p);
}

/* ---- time() and friends ---- */

XDEF(time)
{
    int64_t t = now_s();
    if (A(0)) *(int32_t *)P(0) = (int32_t)t;
    return (uint32_t)t;
}
XDEF(_time32) { return api_time(a); }
XDEF(_time64)
{
    int64_t t = now_s();
    if (A(0)) memcpy(P(0), &t, 8);
    return ret64((uint64_t)t);
}
XDEF(clock) { (void)a; static uint32_t start; if (!start) start = os_ticks_ms(); return os_ticks_ms() - start; }
XDEF(_getsystime)
{
    /* SYSTEMTIME, local */
    SYSTEMTIME *st = P(0);
    int64_t us = (int64_t)os_time_us() + (int64_t)os_tz_minutes() * 60000000;
    struct tm tm;
    crt_gmtime(us / 1000000, &tm);
    st->year = (uint16_t)(tm.tm_year + 1900); st->month = (uint16_t)(tm.tm_mon + 1); st->dow = (uint16_t)tm.tm_wday;
    st->day = (uint16_t)tm.tm_mday; st->hour = (uint16_t)tm.tm_hour; st->minute = (uint16_t)tm.tm_min;
    st->second = (uint16_t)tm.tm_sec; st->ms = (uint16_t)(us % 1000000 / 1000);
    return 0;
}

static void fill_timeb(gp_t p, int wide)
{
    uint64_t us = os_time_us();
    uint8_t *b = GP(p);
    if (wide) {
        int64_t t = (int64_t)(us / 1000000u);
        memcpy(b, &t, 8);
        b += 8;
    } else {
        int32_t t = (int32_t)(us / 1000000u);
        memcpy(b, &t, 4);
        b += 4;
    }
    uint16_t ms = (uint16_t)(us % 1000000u / 1000);
    int16_t tz = (int16_t)-os_tz_minutes(), dst = 0;
    memcpy(b, &ms, 2); memcpy(b + 2, &tz, 2); memcpy(b + 4, &dst, 2);
}
XDEF(_ftime) { fill_timeb(A(0), 0); return 0; }
XDEF(_ftime32) { fill_timeb(A(0), 0); return 0; }
XDEF(_ftime64) { fill_timeb(A(0), 1); return 0; }
XDEF(_ftime_s) { if (!A(0)) return CRT_EINVAL; fill_timeb(A(0), 0); return 0; }
XDEF(_ftime32_s) { return api__ftime_s(a); }
XDEF(_ftime64_s) { if (!A(0)) return CRT_EINVAL; fill_timeb(A(0), 1); return 0; }

XDEF(gmtime) { int64_t t = read_time(A(0), 0); if (t < 0) return 0; return guest_tm(t, 0); }
XDEF(localtime) { int64_t t = read_time(A(0), 0); if (t < 0) return 0; return guest_tm(t, 1); }
XDEF(_gmtime32) { return api_gmtime(a); }
XDEF(_localtime32) { return api_localtime(a); }
XDEF(_gmtime64) { int64_t t = read_time(A(0), 1); if (t < 0) return 0; return guest_tm(t, 0); }
XDEF(_localtime64) { int64_t t = read_time(A(0), 1); if (t < 0) return 0; return guest_tm(t, 1); }

/* (struct tm *out, const time_t *t): an errno value */
static uint32_t tm_s(uint32_t *a, int wide, int local)
{
    if (!A(0) || !A(1)) return CRT_EINVAL;
    int64_t t = read_time(A(1), wide);
    if (t < 0) { memset(P(0), 0xFF, sizeof(struct gtm)); return CRT_EINVAL; }
    struct tm tm;
    if (local) crt_localtime(t, &tm); else crt_gmtime(t, &tm);
    to_gtm(&tm, P(0));
    return 0;
}
XDEF(gmtime_s) { return tm_s(a, 0, 0); }
XDEF(localtime_s) { return tm_s(a, 0, 1); }
XDEF(_gmtime32_s) { return tm_s(a, 0, 0); }
XDEF(_localtime32_s) { return tm_s(a, 0, 1); }
XDEF(_gmtime64_s) { return tm_s(a, 1, 0); }
XDEF(_localtime64_s) { return tm_s(a, 1, 1); }

XDEF(mktime)
{
    struct tm tm;
    from_gtm(P(0), &tm);
    int64_t t = crt_mktime(&tm, 1);
    crt_localtime(t, &tm);
    to_gtm(&tm, P(0));                  /* normalized */
    return (uint32_t)t;
}
XDEF(_mktime32) { return api_mktime(a); }
XDEF(_mktime64)
{
    struct tm tm;
    from_gtm(P(0), &tm);
    int64_t t = crt_mktime(&tm, 1);
    crt_localtime(t, &tm);
    to_gtm(&tm, P(0));
    return ret64((uint64_t)t);
}
XDEF(_mkgmtime) { struct tm tm; from_gtm(P(0), &tm); return (uint32_t)crt_mktime(&tm, 0); }
XDEF(_mkgmtime32) { return api__mkgmtime(a); }
XDEF(_mkgmtime64) { struct tm tm; from_gtm(P(0), &tm); return ret64((uint64_t)crt_mktime(&tm, 0)); }
XDEF(timegm) { return api__mkgmtime(a); }

static const char *const wdays[] = { "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" };
static const char *const months[] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };

static void asctime_into(const struct gtm *g, char *out)
{
    int wd = g->wday >= 0 && g->wday < 7 ? g->wday : 0, mo = g->mon >= 0 && g->mon < 12 ? g->mon : 0;
    sprintf(out, "%s %s %2d %02d:%02d:%02d %d\n", wdays[wd], months[mo], g->mday, g->hour, g->min, g->sec, g->year + 1900);
}

XDEF(asctime)
{
    if (!asc_buf) asc_buf = vm_alloc_loader(32);
    asctime_into(P(0), GP(asc_buf));
    return asc_buf;
}
XDEF(_wasctime)
{
    if (!wasc_buf) wasc_buf = vm_alloc_loader(64);
    char tmp[32];
    asctime_into(P(0), tmp);
    ansi_to_wide(tmp, -1, GP(wasc_buf), 32);
    return wasc_buf;
}
XDEF(asctime_s)
{
    if (!A(0) || A(1) < 26 || !A(2)) return CRT_EINVAL;
    asctime_into(P(2), P(0));
    return 0;
}
XDEF(_wasctime_s)
{
    if (!A(0) || A(1) < 26 || !A(2)) return CRT_EINVAL;
    char tmp[32];
    asctime_into(P(2), tmp);
    ansi_to_wide(tmp, -1, P(0), (int)A(1));
    return 0;
}
static uint32_t ctime_common(uint32_t *a, int wide, int w)
{
    int64_t t = read_time(A(0), wide);
    if (t < 0) return 0;
    gp_t g = guest_tm(t, 1);
    uint32_t b[1] = { g };
    return w ? api__wasctime(b) : api_asctime(b);
}
XDEF(ctime) { return ctime_common(a, 0, 0); }
XDEF(_ctime32) { return ctime_common(a, 0, 0); }
XDEF(_ctime64) { return ctime_common(a, 1, 0); }
XDEF(_wctime) { return ctime_common(a, 0, 1); }
XDEF(_wctime32) { return ctime_common(a, 0, 1); }
XDEF(_wctime64) { return ctime_common(a, 1, 1); }
static uint32_t ctime_s_common(uint32_t *a, int wide, int w)
{
    /* (buf, size, const time_t *) */
    if (!A(0) || A(1) < 26 || !A(2)) return CRT_EINVAL;
    int64_t t = read_time(A(2), wide);
    if (t < 0) return CRT_EINVAL;
    gp_t g = guest_tm(t, 1);
    uint32_t b[3] = { A(0), A(1), g };
    return w ? api__wasctime_s(b) : api_asctime_s(b);
}
XDEF(ctime_s) { return ctime_s_common(a, 0, 0); }
XDEF(_ctime32_s) { return ctime_s_common(a, 0, 0); }
XDEF(_ctime64_s) { return ctime_s_common(a, 1, 0); }
XDEF(_wctime_s) { return ctime_s_common(a, 0, 1); }
XDEF(_wctime32_s) { return ctime_s_common(a, 0, 1); }
XDEF(_wctime64_s) { return ctime_s_common(a, 1, 1); }

/* strftime, the C library's: the formats are the same */
static uint32_t strftime_common(gp_t out, uint32_t max, const char *fmt, gp_t gtm, int wide)
{
    if (!out || !max || !fmt || !gtm) return 0;
    struct tm tm;
    from_gtm(GP(gtm), &tm);
    char buf[4096];
    size_t n = strftime(buf, sizeof(buf), fmt, &tm);
    if (!n && *fmt) { if (wide) *(uint16_t *)GP(out) = 0; else *(char *)GP(out) = 0; return 0; }
    if (n + 1 > max) { if (wide) *(uint16_t *)GP(out) = 0; else *(char *)GP(out) = 0; return 0; }
    if (wide) ansi_to_wide(buf, -1, GP(out), (int)max);
    else memcpy(GP(out), buf, n + 1);
    return (uint32_t)n;
}
XDEF(strftime) { return strftime_common(A(0), A(1), S(2), A(3), 0); }
XDEF(_strftime_l) { return strftime_common(A(0), A(1), S(2), A(3), 0); }
XDEF(wcsftime) { return strftime_common(A(0), A(1), A(2) ? wtmp(A(2)) : NULL, A(3), 1); }
XDEF(_wcsftime_l) { return api_wcsftime(a); }

XDEF(_strdate)
{
    struct tm tm; crt_localtime(now_s(), &tm);
    sprintf(P(0), "%02d/%02d/%02d", tm.tm_mon + 1, tm.tm_mday, tm.tm_year % 100);
    return A(0);
}
XDEF(_strtime)
{
    struct tm tm; crt_localtime(now_s(), &tm);
    sprintf(P(0), "%02d:%02d:%02d", tm.tm_hour, tm.tm_min, tm.tm_sec);
    return A(0);
}
XDEF(_strdate_s) { if (!A(0) || A(1) < 9) return CRT_EINVAL; api__strdate(a); return 0; }
XDEF(_strtime_s) { if (!A(0) || A(1) < 9) return CRT_EINVAL; api__strtime(a); return 0; }
XDEF(_wstrdate) { char t[16]; struct tm tm; crt_localtime(now_s(), &tm); sprintf(t, "%02d/%02d/%02d", tm.tm_mon + 1, tm.tm_mday, tm.tm_year % 100); ansi_to_wide(t, -1, P(0), 9); return A(0); }
XDEF(_wstrtime) { char t[16]; struct tm tm; crt_localtime(now_s(), &tm); sprintf(t, "%02d:%02d:%02d", tm.tm_hour, tm.tm_min, tm.tm_sec); ansi_to_wide(t, -1, P(0), 9); return A(0); }
XDEF(_wstrdate_s) { if (!A(0) || A(1) < 9) return CRT_EINVAL; api__wstrdate(a); return 0; }
XDEF(_wstrtime_s) { if (!A(0) || A(1) < 9) return CRT_EINVAL; api__wstrtime(a); return 0; }

/* the time zone variables: _timezone (seconds west of UTC), _daylight, _tzname */
gp_t crt_timezone_cell, crt_daylight_cell, crt_dstbias_cell, crt_tzname_gp;
static gp_t tzname0, tzname1;

void crt_tz_init(void)
{
    crt_timezone_cell = vm_alloc_loader(4);
    crt_daylight_cell = vm_alloc_loader(4);
    crt_dstbias_cell = vm_alloc_loader(4);
    crt_tzname_gp = vm_alloc_loader(8);
    tzname0 = vm_alloc_loader(32);
    tzname1 = vm_alloc_loader(32);
    int mins = os_tz_minutes();
    *(int32_t *)GP(crt_timezone_cell) = -mins * 60;
    *(int32_t *)GP(crt_daylight_cell) = 0;
    *(int32_t *)GP(crt_dstbias_cell) = -3600;
    snprintf(GP(tzname0), 32, "UTC%+03d", mins / 60);
    if (mins == 0) strcpy(GP(tzname0), "UTC");
    strcpy(GP(tzname1), GP(tzname0));
    ((gp_t *)GP(crt_tzname_gp))[0] = tzname0;
    ((gp_t *)GP(crt_tzname_gp))[1] = tzname1;
}
XDEF(_tzset) { (void)a; *(int32_t *)GP(crt_timezone_cell) = -os_tz_minutes() * 60; return 0; }
XDEF(tzset) { return api__tzset(a); }
XDEF(_get_timezone) { if (!A(0)) return CRT_EINVAL; *(int32_t *)P(0) = *(int32_t *)GP(crt_timezone_cell); return 0; }
XDEF(_get_daylight) { if (!A(0)) return CRT_EINVAL; *(int32_t *)P(0) = 0; return 0; }
XDEF(_get_dstbias) { if (!A(0)) return CRT_EINVAL; *(int32_t *)P(0) = -3600; return 0; }
XDEF(_get_tzname)
{
    /* (size_t *ret, char *buf, size_t size, int index) */
    const char *s = GP(A(3) ? tzname1 : tzname0);
    if (A(0)) *(uint32_t *)P(0) = (uint32_t)strlen(s) + 1;
    if (A(1)) { if (A(2) <= strlen(s)) return CRT_ERANGE; strcpy(P(1), s); }
    return 0;
}
XDEF(__p__timezone) { (void)a; return crt_timezone_cell; }
XDEF(__p__daylight) { (void)a; return crt_daylight_cell; }
XDEF(__p__dstbias) { (void)a; return crt_dstbias_cell; }
XDEF(__p__tzname) { (void)a; return crt_tzname_gp; }
XDEF(__timezone) { (void)a; return crt_timezone_cell; }
XDEF(__daylight) { (void)a; return crt_daylight_cell; }
XDEF(__tzname) { (void)a; return crt_tzname_gp; }

XDEF(difftime) { api_set_fret((double)(int32_t)A(0) - (double)(int32_t)A(1)); return 0; }
XDEF(_difftime32) { return api_difftime(a); }
XDEF(_difftime64) { api_set_fret((double)i64(a) - (double)i64(a + 2)); return 0; }

XDEF(_utime)
{
    /* (path, struct _utimbuf *): actime, modtime (32-bit) */
    char host[PATH_MAX];
    if (path_to_host(S(0), host) < 0) { crt_set_errno(CRT_ENOENT); return (uint32_t)-1; }
    path_fix_case(host);
    (void)A(1);                         /* times are not kept on fluxOS's file systems yet */
    return 0;
}
XDEF(_utime32) { return api__utime(a); }
XDEF(_utime64) { return api__utime(a); }
XDEF(_wutime) { gp_t n = gstrdup(wtmp(A(0))); uint32_t b[2] = { n, A(1) }; uint32_t r = api__utime(b); gfree(n); return r; }
XDEF(_wutime32) { return api__wutime(a); }
XDEF(_wutime64) { return api__wutime(a); }
XDEF(_futime) { (void)a; return 0; }
XDEF(_futime32) { (void)a; return 0; }
XDEF(_futime64) { (void)a; return 0; }

const struct api_entry crt_time_entries[] = {
    APIC(time), APIC(_time32), APIC(_time64), APIC(clock), APIC(_getsystime),
    APIC(_ftime), APIC(_ftime32), APIC(_ftime64), APIC(_ftime_s), APIC(_ftime32_s), APIC(_ftime64_s),
    APIC(gmtime), APIC(localtime), APIC(_gmtime32), APIC(_localtime32), APIC(_gmtime64), APIC(_localtime64),
    APIC(gmtime_s), APIC(localtime_s), APIC(_gmtime32_s), APIC(_localtime32_s), APIC(_gmtime64_s), APIC(_localtime64_s),
    APIC(mktime), APIC(_mktime32), APIC(_mktime64), APIC(_mkgmtime), APIC(_mkgmtime32), APIC(_mkgmtime64), APIC(timegm),
    APIC(asctime), APIC(_wasctime), APIC(asctime_s), APIC(_wasctime_s),
    APIC(ctime), APIC(_ctime32), APIC(_ctime64), APIC(_wctime), APIC(_wctime32), APIC(_wctime64),
    APIC(ctime_s), APIC(_ctime32_s), APIC(_ctime64_s), APIC(_wctime_s), APIC(_wctime32_s), APIC(_wctime64_s),
    APIC(strftime), APIC(_strftime_l), APIC(wcsftime), APIC(_wcsftime_l),
    APIC(_strdate), APIC(_strtime), APIC(_strdate_s), APIC(_strtime_s), APIC(_wstrdate), APIC(_wstrtime),
    APIC(_wstrdate_s), APIC(_wstrtime_s),
    APIC(_tzset), APIC(tzset), APIC(_get_timezone), APIC(_get_daylight), APIC(_get_dstbias), APIC(_get_tzname),
    APIC(__p__timezone), APIC(__p__daylight), APIC(__p__dstbias), APIC(__p__tzname),
    APIC(__timezone), APIC(__daylight), APIC(__tzname),
    DATA(_timezone, crt_timezone_cell), DATA(_daylight, crt_daylight_cell), DATA(_dstbias, crt_dstbias_cell),
    DATA(_tzname, crt_tzname_gp),
    APIF(difftime), APIF(_difftime32), APIF(_difftime64),
    APIC(_utime), APIC(_utime32), APIC(_utime64), APIC(_wutime), APIC(_wutime32), APIC(_wutime64),
    APIC(_futime), APIC(_futime32), APIC(_futime64),
};
const int crt_time_count = sizeof(crt_time_entries) / sizeof(crt_time_entries[0]);

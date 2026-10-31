/*
 * WINMM: the multimedia timer calls games time their frames with. Sound
 * (waveOut, PlaySound, MIDI) is stage 4 of the roadmap; timeSetEvent's
 * callbacks would need a thread of their own.
 */
#include "win32.h"
#include "os.h"

#define A(i) (a[i])
#define P(i) gp(a[i])
#define TIMERR_NOERROR 0
#define TIMERR_NOCANDO 97
#define TIME_MS 1

DEF(timeGetTime) { (void)a; return os_ticks_ms(); }
/* The clock counts milliseconds whatever the period asked for. */
DEF(timeBeginPeriod) { return A(0) ? TIMERR_NOERROR : TIMERR_NOCANDO; }
DEF(timeEndPeriod) { return A(0) ? TIMERR_NOERROR : TIMERR_NOCANDO; }

DEF(timeGetDevCaps)
{
    if (!A(0) || A(1) < 8)
        return TIMERR_NOCANDO;
    uint32_t *caps = P(0);
    caps[0] = 1;                        /* wPeriodMin */
    caps[1] = 1000000;                  /* wPeriodMax */
    return TIMERR_NOERROR;
}

DEF(timeGetSystemTime)
{
    if (!A(0) || A(1) < 8)
        return TIMERR_NOCANDO;
    uint32_t *mmt = P(0);               /* MMTIME: wType, then the time */
    mmt[0] = TIME_MS;
    mmt[1] = os_ticks_ms();
    return TIMERR_NOERROR;
}

static const struct api_entry winmm_entries[] = {
    API(timeBeginPeriod, 1),
    API(timeEndPeriod, 1),
    API(timeGetDevCaps, 2),
    API(timeGetSystemTime, 2),
    API(timeGetTime, 0),
};

struct api_dll winmm_dll = { "WINMM.DLL", winmm_entries, sizeof(winmm_entries) / sizeof(winmm_entries[0]),
                             NULL, { NULL }, { 0 }, NULL, NULL, 0 };

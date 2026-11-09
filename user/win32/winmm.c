/* Multimedia clock and timers. Workers are real Windows threads with their
 * own TEB/TLS; callback execution and cancellation share the runtime lock. */
#include <stdlib.h>
#include "ui.h"
#include "os.h"
#include "threads.h"
#include "audio.h"
#define TIMERR_NOERROR 0
#define TIMERR_NOCANDO 97
struct mm_timer {
    struct mm_timer *next;
    uint32_t id,delay,flags,callback,user,deadline,tid;
    HANDLE worker;
    int cancelled,done,waiters;
};
static struct mm_timer *timers;
static uint32_t next_timer=1;
static uint32_t timer_worker(void *arg)
{
    struct mm_timer *t=arg;t->tid=teb->thread_id;
    while(!t->cancelled) {
        int32_t wait=(int32_t)(t->deadline-os_ticks_ms());
        if(wait>0) {thread_wait_ms(wait);continue;}
        if(t->flags&0x10) K(SetEvent,t->callback);
        else {
            uint32_t args[]={t->id,0,t->user,0,0};
            call_guest(t->callback,5,args);
        }
        if(!(t->flags&1)) break;
        /* Skip missed periods instead of executing a callback storm. */
        uint32_t now=os_ticks_ms();
        t->deadline+=t->delay;
        if((int32_t)(t->deadline-now)<=0) t->deadline=now+t->delay;
    }
    t->done=1;thread_notify();return 0;
}
static void reap_timers(void)
{
    for(struct mm_timer **p=&timers;*p;) {
        struct mm_timer *t=*p;
        if(t->done && !t->waiters && win_thread_signaled(t->worker)==1) {
            handle_close(t->worker);*p=t->next;free(t);
        } else p=&t->next;
    }
}
DEF(timeSetEvent)
{
    reap_timers();
    /* Pulse events are not supported by the kernel event implementation. */
    if(!A(0) || A(0)>1000000 || A(1)>1000000 || !A(2) || (A(4)&~0x111u)) return 0;
    if(A(4)&0x10) {if(!handle_get_type(A(2),H_EVENT)) return 0;}
    else if(!vm_readable(A(2),1)) return 0;
    struct mm_timer *t=calloc(1,sizeof(*t));if(!t) return 0;
    t->id=next_timer++;if(!t->id) t->id=next_timer++;
    t->delay=A(0);t->flags=A(4);t->callback=A(2);t->user=A(3);t->deadline=os_ticks_ms()+t->delay;
    t->worker=win_thread_create_native(timer_worker,t);
    if(!t->worker) {free(t);return 0;}
    t->next=timers;timers=t;return t->id;
}
DEF(timeKillEvent)
{
    struct mm_timer *t;
    for(t=timers;t;t=t->next) if(t->id==A(0) && !t->cancelled && !t->done) break;
    if(!t) return TIMERR_NOCANDO;
    t->cancelled=1;thread_notify();
    /* A callback can cancel itself; waiting for its own return would deadlock.
     * Other callbacks cannot start after cancellation because of the lock. */
    if((t->flags&0x100) && t->tid!=teb->thread_id) {
        t->waiters++;
        while(!t->done) thread_wait_ms(-1);
        t->waiters--;
    }
    return TIMERR_NOERROR;
}
DEF(timeGetTime) {(void)a;return os_ticks_ms();}
DEF(timeBeginPeriod) {return A(0)>=1 && A(0)<=1000000?0:TIMERR_NOCANDO;}
DEF(timeEndPeriod) {return api_timeBeginPeriod(a);}
DEF(timeGetDevCaps)
{
    if(!A(0) || A(1)<8) return TIMERR_NOCANDO;
    uint32_t *caps=P(0);caps[0]=1;caps[1]=1000000;return 0;
}
DEF(timeGetSystemTime)
{
    if(!A(0) || A(1)<8) return TIMERR_NOCANDO;
    uint32_t *mmt=P(0);mmt[0]=1;mmt[1]=os_ticks_ms();return 0;
}
extern uint32_t api_PlaySoundA(uint32_t *),api_PlaySoundW(uint32_t *),api_sndPlaySoundA(uint32_t *),api_sndPlaySoundW(uint32_t *);
extern const struct api_entry midi_entries[];
extern const int midi_count;
static const struct api_entry winmm_entries[]={
    API(PlaySoundA,3),API(PlaySoundW,3),API(sndPlaySoundA,2),API(sndPlaySoundW,2),
    API(timeSetEvent,5),API(timeKillEvent,1),API(timeBeginPeriod,1),API(timeEndPeriod,1),
    API(timeGetDevCaps,2),API(timeGetSystemTime,2),API(timeGetTime,0)
};
struct api_dll winmm_dll={.name="WINMM.DLL",.entries=winmm_entries,.count=sizeof(winmm_entries)/sizeof(winmm_entries[0])};

void winmm_link(void) {winmm_dll.tables[0]=waveout_entries;winmm_dll.counts[0]=waveout_count;winmm_dll.tables[1]=midi_entries;winmm_dll.counts[1]=midi_count;}

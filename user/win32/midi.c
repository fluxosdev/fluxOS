/* Basic General MIDI channel messages on the native nine-voice OPL2 synth.
 * Timbres are synthesized locally; no external instrument bank is required. */
#include <fcntl.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <sys/soundcard.h>
#include <sys/stat.h>
#include <unistd.h>
#include "ui.h"
struct midi {uint32_t handle,callback,instance,flags,volume;int fd;uint8_t programs[16],volumes[16],sustain[16];uint16_t bend[16];
    struct {int note,channel,velocity,held;uint32_t age;} voices[9];uint32_t age;};
static struct midi *device;
static uint32_t next_midi=0x79000001;
static const uint8_t operators[9]={0,1,2,8,9,10,16,17,18};
static void regwrite(struct midi *m,int reg,int val) {struct opl_write w={reg,val,0};write(m->fd,&w,sizeof(w));}
static void midi_notify(struct midi *m,uint32_t msg,gp_t header)
{
    switch(m->flags&0x70000) {
    case 0x30000:{uint32_t args[]={m->handle,msg,m->instance,header,0};call_guest(m->callback,5,args);break;}
    case 0x10000:U(PostMessageA,m->callback,msg,m->handle,header);break;
    case 0x20000:U(PostThreadMessageA,m->callback,msg,m->handle,header);break;
    case 0x50000:K(SetEvent,m->callback);break;
    }
}
static void note_off(struct midi *m,int v) {regwrite(m,0xb0+v,0);m->voices[v].note=-1;m->voices[v].held=0;}
static void tune(struct midi *m,int v)
{
    int ch=m->voices[v].channel,note=m->voices[v].note;
    double freq=440.0*pow(2.0,(note-69+((int)m->bend[ch]-8192)/4096.0)/12.0);
    int block=0,fnum=0;for(;block<7;block++) {fnum=(int)(freq*pow(2.0,20-block)/49716.0);if(fnum<1024)break;}
    if(block==7)fnum=(int)(freq*8192.0/49716.0);
    if(fnum>1023)fnum=1023;
    regwrite(m,0xa0+v,fnum&255);regwrite(m,0xb0+v,0x20|(block<<2)|((fnum>>8)&3));
}
static void level(struct midi *m,int v)
{
    int ch=m->voices[v].channel;uint32_t master=((m->volume&65535)+(m->volume>>16))/2;
    int value=(int)((uint64_t)m->voices[v].velocity*m->volumes[ch]*master/(127*65535));
    regwrite(m,0x40+operators[v]+3,63-value*63/127);
}
static int available(void) {struct stat st;return stat("/dev/opl",&st)==0;}
XDEF(midiOutGetNumDevs) {(void)a;return available()?1:0;}
XDEF(midiOutOpen)
{
    if(!vm_writable(A(0),4) || (A(1)!=0 && A(1)!=0xffffffffu))return 11;
    uint32_t kind=A(4)&0x70000;if((A(4)&~0x70000u) || (kind && !A(2)) || (kind!=0 && kind!=0x10000 && kind!=0x20000 && kind!=0x30000 && kind!=0x50000))return 11;
    if(!available())return 6;
    if(device)return 4;
    struct midi *m=calloc(1,sizeof(*m));if(!m)return 7;m->fd=open("/dev/opl",O_WRONLY);if(m->fd<0){free(m);return 4;}
    m->handle=next_midi++;m->volume=0xffffffff;m->callback=A(2);m->instance=A(3);m->flags=A(4);
    for(int i=0;i<16;i++){m->volumes[i]=100;m->bend[i]=8192;}for(int i=0;i<9;i++)m->voices[i].note=-1;
    ioctl(m->fd,OPL_RESET);regwrite(m,1,0x20);device=m;*(uint32_t *)P(0)=m->handle;midi_notify(m,0x3c7,0);return 0;
}
XDEF(midiOutReset) {if(!device || A(0)!=device->handle)return 5;for(int i=0;i<9;i++)note_off(device,i);return 0;}
XDEF(midiOutClose) {struct midi *m=device;if(!m || A(0)!=m->handle)return 5;device=NULL;ioctl(m->fd,OPL_RESET);close(m->fd);midi_notify(m,0x3c8,0);free(m);return 0;}
XDEF(midiOutShortMsg)
{
    struct midi *m=device;if(!m || A(0)!=m->handle)return 5;
    uint32_t msg=A(1);int status=msg&0xf0,ch=msg&15,key=(msg>>8)&127,val=(msg>>16)&127;
    if(status==0x90 && val) {
        int voice=-1;for(int i=0;i<9;i++)if(m->voices[i].note<0){voice=i;break;}
        if(voice<0){voice=0;for(int i=1;i<9;i++)if(m->voices[i].age<m->voices[voice].age)voice=i;}
        note_off(m,voice);m->voices[voice].note=key;m->voices[voice].channel=ch;m->voices[voice].velocity=val;m->voices[voice].age=++m->age;
        int op=operators[voice],tone=m->programs[ch]%8;
        regwrite(m,0x20+op,0x21|(tone&3));regwrite(m,0x20+op+3,0x21);regwrite(m,0x40+op,24+tone*3);
        regwrite(m,0x60+op,0xf2);regwrite(m,0x60+op+3,0xf2);regwrite(m,0x80+op,0x73);regwrite(m,0x80+op+3,0x73);
        regwrite(m,0xe0+op,tone&3);regwrite(m,0xe0+op+3,0);regwrite(m,0xc0+voice,2|(tone&1));level(m,voice);tune(m,voice);
    } else if(status==0x80 || (status==0x90 && !val)) {
        for(int i=0;i<9;i++)if(m->voices[i].note==key && m->voices[i].channel==ch) {if(m->sustain[ch])m->voices[i].held=1;else note_off(m,i);}
    } else if(status==0xc0)m->programs[ch]=key;
    else if(status==0xe0) {m->bend[ch]=key|(val<<7);for(int i=0;i<9;i++)if(m->voices[i].note>=0 && m->voices[i].channel==ch)tune(m,i);}
    else if(status==0xb0) {
        if(key==7)m->volumes[ch]=val;
        if(key==64)m->sustain[ch]=val>=64;
        if(key==121){m->sustain[ch]=0;m->volumes[ch]=100;m->bend[ch]=8192;}
        for(int i=0;i<9;i++)if(m->voices[i].channel==ch && m->voices[i].note>=0) {
            if(key==120 || key==123 || (!m->sustain[ch] && m->voices[i].held))note_off(m,i);else {level(m,i);tune(m,i);}
        }
    } else if(status!=0xa0 && status!=0xd0) return 11;
    return 0;
}
XDEF(midiOutGetVolume) {if(!device || A(0)!=device->handle)return 5;if(!vm_writable(A(1),4))return 11;*(uint32_t *)P(1)=device->volume;return 0;}
XDEF(midiOutSetVolume) {if(!device || A(0)!=device->handle)return 5;device->volume=A(1);for(int i=0;i<9;i++)if(device->voices[i].note>=0)level(device,i);return 0;}
XDEF(midiOutGetID) {if(!device || A(0)!=device->handle)return 5;if(!vm_writable(A(1),4))return 11;*(uint32_t *)P(1)=0;return 0;}
static uint32_t caps(uint32_t *a,int wide)
{
    if(A(0)!=0 && A(0)!=0xffffffffu)return 2;
    if(!available())return 6;
    uint8_t out[84]={0};uint16_t one=1,tech=4,voices=9,channels=0xffff;uint32_t version=0x100,support=3;
    memcpy(out,&one,2);memcpy(out+2,&one,2);memcpy(out+4,&version,4);
    int tail=wide?72:40;if(wide)ansi_to_wide("fluxOS OPL FM",-1,(uint16_t *)(out+8),32);else strcpy((char *)out+8,"fluxOS OPL FM");
    memcpy(out+tail,&tech,2);memcpy(out+tail+2,&voices,2);memcpy(out+tail+4,&voices,2);memcpy(out+tail+6,&channels,2);memcpy(out+tail+8,&support,4);
    uint32_t n=A(2)<(uint32_t)(tail+12)?A(2):(uint32_t)(tail+12);if(!vm_writable(A(1),n))return 11;memcpy(P(1),out,n);return 0;
}
XDEF(midiOutGetDevCapsA) {return caps(a,0);}
XDEF(midiOutGetDevCapsW) {return caps(a,1);}
XDEF(midiOutPrepareHeader)
{
    if(!device || A(0)!=device->handle)return 5;
    if(A(2)<28 || !vm_writable(A(1),28))return 11;
    uint32_t *h=P(1);if(!h[1] || !vm_readable(h[0],h[1]))return 11;h[4]=(h[4]&~1u)|2;return 0;
}
XDEF(midiOutUnprepareHeader)
{
    if(!device || A(0)!=device->handle)return 5;
    if(A(2)<28 || !vm_writable(A(1),28))return 11;
    uint32_t *h=P(1);
    if(h[4]&4)return 65;
    h[4]&=~2u;
    return 0;
}
XDEF(midiOutLongMsg)
{
    struct midi *m=device;if(!m || A(0)!=m->handle)return 5;
    if(A(2)<28 || !vm_writable(A(1),28))return 11;
    uint32_t *h=P(1);
    if(!(h[4]&2))return 64;
    if(h[1]<6 || !vm_readable(h[0],h[1]))return 11;
    const uint8_t *p=GP(h[0]);
    if(h[1]==6 && p[0]==0xf0 && p[1]==0x7e && p[3]==9 && p[4]==1 && p[5]==0xf7) {
        for(int i=0;i<9;i++)note_off(m,i);
        for(int i=0;i<16;i++){m->programs[i]=0;m->volumes[i]=100;m->sustain[i]=0;m->bend[i]=8192;}
    } else if(h[1]==8 && p[0]==0xf0 && p[1]==0x7f && p[3]==4 && p[4]==1 && p[7]==0xf7) {
        uint32_t v=((p[6]&127)<<7)|(p[5]&127);v=v*65535/16383;m->volume=v|(v<<16);
        for(int i=0;i<9;i++)if(m->voices[i].note>=0)level(m,i);
    } else return 8;
    h[4]=(h[4]&~4u)|1;midi_notify(m,0x3c9,A(1));return 0;
}
XDEF(midiOutGetErrorTextA) {if(!A(1)||!A(2))return 11;snprintf(P(1),A(2),"MIDI error %u",A(0));return 0;}
XDEF(midiOutGetErrorTextW) {if(!A(1)||!A(2))return 11;char text[64];snprintf(text,sizeof(text),"MIDI error %u",A(0));ansi_to_wide(text,-1,P(1),A(2));return 0;}
const struct api_entry midi_entries[]={API(midiOutPrepareHeader,3),API(midiOutUnprepareHeader,3),API(midiOutLongMsg,3),API(midiOutGetErrorTextA,3),API(midiOutGetErrorTextW,3),API(midiOutGetNumDevs,0),API(midiOutOpen,5),API(midiOutClose,1),API(midiOutReset,1),API(midiOutShortMsg,2),API(midiOutGetVolume,2),API(midiOutSetVolume,2),API(midiOutGetID,2),API(midiOutGetDevCapsA,3),API(midiOutGetDevCapsW,3)};
const int midi_count=sizeof(midi_entries)/sizeof(midi_entries[0]);

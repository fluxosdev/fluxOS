/* Shell PIDLs use the COM task allocator even without COM object activation. */
#include "ui.h"
DEF(CoTaskMemAlloc) {
    return galloc(A(0) ? A(0):1);
}
DEF(CoTaskMemFree) {
    if(A(0)) gfree(A(0));
    return 0;
}
DEF(CoTaskMemRealloc) {
    if(!A(0)) return galloc(A(1) ? A(1):1);
    if(!A(1)) {
        gfree(A(0));
        return 0;
    }
    return heap_realloc(process_heap,A(0),A(1),0,0);
}
extern uint32_t api_DirectDrawCreate(uint32_t *),api_DirectDrawCreateClipper(uint32_t *),api_DirectSoundCreate(uint32_t *),api_DirectSoundCreate8(uint32_t *);
struct com_init_state {struct com_init_state *next;uint32_t tid,model,count;};
static struct com_init_state *init_states;
DEF(CoInitializeEx)
{
    if(A(0) || (A(1)&~0xeu))return 0x80070057;
    uint32_t model=A(1)&2;
    for(struct com_init_state *s=init_states;s;s=s->next)if(s->tid==teb->thread_id){if(s->model!=model)return 0x80010106;s->count++;return 1;}
    struct com_init_state *s=calloc(1,sizeof(*s));if(!s)return 0x8007000e;s->tid=teb->thread_id;s->model=model;s->count=1;s->next=init_states;init_states=s;return 0;
}
DEF(CoInitialize) {uint32_t b[]={A(0),2};return api_CoInitializeEx(b);}
DEF(CoUninitialize)
{
    (void)a;for(struct com_init_state **p=&init_states;*p;p=&(*p)->next)if((*p)->tid==teb->thread_id){struct com_init_state *s=*p;if(!--s->count){*p=s->next;free(s);}break;}return 0;
}
void ole_thread_exit(uint32_t tid)
{for(struct com_init_state **p=&init_states;*p;)if((*p)->tid==tid){struct com_init_state *s=*p;*p=s->next;free(s);}else p=&(*p)->next;}
DEF(CoCreateInstance)
{
    if(!vm_writable(A(4),4))return 0x80070057;
    *(gp_t *)P(4)=0;
    if(!vm_readable(A(0),16)||!vm_readable(A(3),16))return 0x80070057;
    if(A(1))return 0x80040110;
    if(!(A(2)&1))return 0x80040154;
    int initialized=0;for(struct com_init_state *s=init_states;s;s=s->next)if(s->tid==teb->thread_id)initialized=1;
    if(!initialized)return 0x800401f0;
    static const uint32_t classes[][4]={
        {0xd7b70ee0,0x11cf4340,0x200063b0,0x35cdc2af},
        {0x3c305196,0x11d350db,0xc000fe9c,0xc530d94f},
        {0x593817a0,0x11cf7db3,0xaa00dea2,0x5633b900},
        {0x47d4d946,0x11cf62e8,0x4544bc93,0x00005453},
        {0x3901cc3f,0x4fa484b5,0x81aa35ba,0x9ba0b872}
    };
    uint32_t (*factories[])(uint32_t *)={api_DirectDrawCreate,api_DirectDrawCreate,api_DirectDrawCreateClipper,api_DirectSoundCreate,api_DirectSoundCreate8};
    for(int i=0;i<5;i++)if(!memcmp(P(0),classes[i],16)) {
        gp_t output=galloc(4);if(!output)return 0x8007000e;uint32_t args[]={0,output,0};uint32_t result=factories[i](args);
        if(!result){gp_t object=*(gp_t *)GP(output);gp_t *vtable=GP(*(gp_t *)GP(object));result=call_guest3(vtable[0],object,A(3),A(4));call_guest1(vtable[2],object);}
        gfree(output);return result;
    }
    return 0x80040154;
}

static const struct api_entry entries[]={
    API(CoInitialize,1),API(CoInitializeEx,2),API(CoUninitialize,0),API(CoCreateInstance,5),
    API(CoTaskMemAlloc,1),API(CoTaskMemFree,1),API(CoTaskMemRealloc,2)
};
struct api_dll ole32_dll={
    .name="OLE32.DLL",.entries=entries,.count=sizeof(entries)/sizeof(entries[0])
};

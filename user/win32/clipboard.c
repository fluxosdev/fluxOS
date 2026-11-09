/* Text clipboard. Handles handed to the program are real GlobalAlloc blocks in guest memory. */
#include "ui.h"
static int opened, fetched, available;
static HWND opener, owner;
static HANDLE text_handle, wide_handle;
static uint16_t *local;
static uint32_t sequence;
static void clear_handles(void)
{
    if(text_handle) K(GlobalFree,text_handle);
    if(wide_handle) K(GlobalFree,wide_handle);
    text_handle=wide_handle=0;
}
static HANDLE make_handle(const uint16_t *text,int wide)
{
    int n=wstrlen(text);
    HANDLE h=K(GlobalAlloc,0x42,(n+1)*(wide ? 2 : 1));
    gp_t p=K(GlobalLock,h);
    if(!p) { K(GlobalFree,h); return 0; }
    if(wide) memcpy(GP(p),text,(n+1)*2);
    else wide_to_ansi(text,n+1,GP(p),n+1);
    K(GlobalUnlock,h); return h;
}
uint16_t *ui_clip_get(void)
{
    char *utf8=gui_clip_get();
    if(utf8) {
        size_t n=strlen(utf8);
        uint16_t *out=calloc(n+1,2);
        if(!out) wdie("out of memory for clipboard");
        utf8_to_wide(utf8,-1,out,n+1);
        free(utf8); return out;
    }
    return ui_wdup(local ? local : (const uint16_t *)u"",-1);
}
int ui_clip_set(const uint16_t *text)
{
    free(local); local=ui_wdup(text,-1);
    int n=wstrlen(text);
    char *utf8=malloc((size_t)n*4+1);
    if(!utf8) return 0;
    int len=wide_to_utf8(text,n,utf8,n*4);
    utf8[len]=0; gui_clip_set(utf8); free(utf8); available=1; sequence++; return 1;
}
DEF(OpenClipboard)
{
    if(opened) { set_last_error(5); return 0; }
    if(A(0) && !ui_win(A(0))) return 0;
    opened=1; opener=A(0); fetched=0; return 1;
}
DEF(CloseClipboard) { if(!opened) return 0; opened=0; opener=0; return 1; }
DEF(EmptyClipboard)
{
    if(!opened) return 0;
    clear_handles(); owner=opener; fetched=1; int r=ui_clip_set((const uint16_t *)u"");available=0;return r;
}
DEF(SetClipboardData)
{
    if(!opened || (A(0)!=1 && A(0)!=13) || !A(1)) return 0;
    uint32_t size=K(GlobalSize,A(1));
    gp_t p=K(GlobalLock,A(1));
    if(!p || !size) return 0;
    uint32_t unit=A(0)==13 ? 2 : 1,n=0;
    while(n<size/unit && (unit==2 ? ((uint16_t *)GP(p))[n] : ((uint8_t *)GP(p))[n])) n++;
    if(n==size/unit) { K(GlobalUnlock,A(1)); return 0; }
    uint16_t *s=calloc(n+1,2);
    if(!s) { K(GlobalUnlock,A(1)); return 0; }
    if(unit==2) memcpy(s,GP(p),n*2); else ansi_to_wide(GP(p),n,s,n);
    K(GlobalUnlock,A(1));
    if(A(1)==text_handle) text_handle=0;
    if(A(1)==wide_handle) wide_handle=0;
    clear_handles();
    if(A(0)==13) wide_handle=A(1); else text_handle=A(1);
    ui_clip_set(s); free(s); fetched=1; return A(1);
}
DEF(GetClipboardData)
{
    if(!opened || (A(0)!=1 && A(0)!=13)) return 0;
    if(!fetched) {
        uint16_t *s=ui_clip_get();
        clear_handles(); if(s[0]) available=1;else if(!local || local[0]) available=0; free(local); local=s; fetched=1;
    }
    if(!local || !available) return 0;
    HANDLE *h=A(0)==13 ? &wide_handle : &text_handle;
    if(!*h) *h=make_handle(local,A(0)==13);
    return *h;
}
static int formats_available(void)
{
    uint16_t *s=ui_clip_get();int present=s[0]!=0 || (available && local && !local[0]);free(s);return present;
}
DEF(IsClipboardFormatAvailable) { return (A(0)==1 || A(0)==13) && formats_available(); }
DEF(CountClipboardFormats) { (void)a; return formats_available() ? 2:0; }
DEF(EnumClipboardFormats) { return !formats_available() ? 0:A(0)==0 ? 1 : A(0)==1 ? 13 : 0; }
DEF(GetClipboardOwner) { (void)a; return owner; }
DEF(GetOpenClipboardWindow) { (void)a; return opened ? opener : 0; }
DEF(GetClipboardSequenceNumber) { (void)a; return sequence; }
DEF(GetPriorityClipboardFormat)
{
    uint32_t *formats=P(0);
    for(uint32_t i=0;i<A(1);i++) if(formats[i]==1 || formats[i]==13) return formats[i];
    return (uint32_t)-1;
}
const struct api_entry clipboard_entries[]={
    API(OpenClipboard,1), API(CloseClipboard,0), API(EmptyClipboard,0), API(SetClipboardData,2),
    API(GetClipboardData,1), API(IsClipboardFormatAvailable,1), API(CountClipboardFormats,0),
    API(EnumClipboardFormats,1), API(GetClipboardOwner,0), API(GetOpenClipboardWindow,0),
    API(GetClipboardSequenceNumber,0), API(GetPriorityClipboardFormat,2),
};
const int clipboard_count=sizeof(clipboard_entries)/sizeof(clipboard_entries[0]);

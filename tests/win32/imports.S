/* Shared PE import table for the freestanding regression programs. */
#define IB 0x400000
#define IMP(dll,ret,name,args) IMP_##dll(name)
#define IAT(name) .global __imp_##name; __imp_##name: .long hint_##name - IB;
#define HINT(name) hint_##name: .short 0; .asciz #name; .balign 2;
#define NONE(name)
.section .idata,"aw"
.long 0,0,0,dll_K32-IB,iat_K32-IB
.long 0,0,0,dll_U32-IB,iat_U32-IB
.long 0,0,0,dll_G32-IB,iat_G32-IB
.long 0,0,0,dll_WMM-IB,iat_WMM-IB
.long 0,0,0,dll_ADV-IB,iat_ADV-IB
.long 0,0,0,dll_SH-IB,iat_SH-IB
.long 0,0,0,dll_CCTL-IB,iat_CCTL-IB
.long 0,0,0,dll_CDLG-IB,iat_CDLG-IB
.long 0,0,0,dll_OLE-IB,iat_OLE-IB
.long 0,0,0,dll_GP-IB,iat_GP-IB
.long 0,0,0,0,0
#define IMP_K32 IAT
#define IMP_U32 NONE
#define IMP_G32 NONE
#define IMP_WMM NONE
#define IMP_ADV NONE
#define IMP_SH NONE
#define IMP_CCTL NONE
#define IMP_CDLG NONE
#define IMP_OLE NONE
#define IMP_GP NONE
iat_K32:
#include "imports.h"
.long 0
#undef IMP_K32
#undef IMP_U32
#undef IMP_G32
#undef IMP_WMM
#undef IMP_ADV
#undef IMP_SH
#undef IMP_CCTL
#undef IMP_CDLG
#undef IMP_OLE
#undef IMP_GP
#define IMP_K32 NONE
#define IMP_U32 IAT
#define IMP_G32 NONE
#define IMP_WMM NONE
#define IMP_ADV NONE
#define IMP_SH NONE
#define IMP_CCTL NONE
#define IMP_CDLG NONE
#define IMP_OLE NONE
#define IMP_GP NONE
iat_U32:
#include "imports.h"
.long 0
#undef IMP_K32
#undef IMP_U32
#undef IMP_G32
#undef IMP_WMM
#undef IMP_ADV
#undef IMP_SH
#undef IMP_CCTL
#undef IMP_CDLG
#undef IMP_OLE
#undef IMP_GP
#define IMP_K32 NONE
#define IMP_U32 NONE
#define IMP_G32 IAT
#define IMP_WMM NONE
#define IMP_ADV NONE
#define IMP_SH NONE
#define IMP_CCTL NONE
#define IMP_CDLG NONE
#define IMP_OLE NONE
#define IMP_GP NONE
iat_G32:
#include "imports.h"
.long 0
#undef IMP_K32
#undef IMP_U32
#undef IMP_G32
#undef IMP_WMM
#undef IMP_ADV
#undef IMP_SH
#undef IMP_CCTL
#undef IMP_CDLG
#undef IMP_OLE
#undef IMP_GP
#define IMP_K32 NONE
#define IMP_U32 NONE
#define IMP_G32 NONE
#define IMP_WMM IAT
#define IMP_ADV NONE
#define IMP_SH NONE
#define IMP_CCTL NONE
#define IMP_CDLG NONE
#define IMP_OLE NONE
#define IMP_GP NONE
iat_WMM:
#include "imports.h"
.long 0
#undef IMP_K32
#undef IMP_U32
#undef IMP_G32
#undef IMP_WMM
#undef IMP_ADV
#undef IMP_SH
#undef IMP_CCTL
#undef IMP_CDLG
#undef IMP_OLE
#undef IMP_GP
#define IMP_K32 NONE
#define IMP_U32 NONE
#define IMP_G32 NONE
#define IMP_WMM NONE
#define IMP_ADV IAT
#define IMP_SH NONE
#define IMP_CCTL NONE
#define IMP_CDLG NONE
#define IMP_OLE NONE
#define IMP_GP NONE
iat_ADV:
#include "imports.h"
.long 0
#undef IMP_K32
#undef IMP_U32
#undef IMP_G32
#undef IMP_WMM
#undef IMP_ADV
#undef IMP_SH
#undef IMP_CCTL
#undef IMP_CDLG
#undef IMP_OLE
#undef IMP_GP
#define IMP_K32 NONE
#define IMP_U32 NONE
#define IMP_G32 NONE
#define IMP_WMM NONE
#define IMP_ADV NONE
#define IMP_SH IAT
#define IMP_CCTL NONE
#define IMP_CDLG NONE
#define IMP_OLE NONE
#define IMP_GP NONE
iat_SH:
#include "imports.h"
.long 0
#undef IMP_K32
#undef IMP_U32
#undef IMP_G32
#undef IMP_WMM
#undef IMP_ADV
#undef IMP_SH
#undef IMP_CCTL
#undef IMP_CDLG
#undef IMP_OLE
#undef IMP_GP
#define IMP_K32 NONE
#define IMP_U32 NONE
#define IMP_G32 NONE
#define IMP_WMM NONE
#define IMP_ADV NONE
#define IMP_SH NONE
#define IMP_CCTL IAT
#define IMP_CDLG NONE
#define IMP_OLE NONE
#define IMP_GP NONE
iat_CCTL:
#include "imports.h"
.long 0
#undef IMP_K32
#undef IMP_U32
#undef IMP_G32
#undef IMP_WMM
#undef IMP_ADV
#undef IMP_SH
#undef IMP_CCTL
#undef IMP_CDLG
#undef IMP_OLE
#undef IMP_GP
#define IMP_K32 NONE
#define IMP_U32 NONE
#define IMP_G32 NONE
#define IMP_WMM NONE
#define IMP_ADV NONE
#define IMP_SH NONE
#define IMP_CCTL NONE
#define IMP_CDLG IAT
#define IMP_OLE NONE
#define IMP_GP NONE
iat_CDLG:
#include "imports.h"
.long 0
#undef IMP_K32
#undef IMP_U32
#undef IMP_G32
#undef IMP_WMM
#undef IMP_ADV
#undef IMP_SH
#undef IMP_CCTL
#undef IMP_CDLG
#undef IMP_OLE
#undef IMP_GP
#define IMP_K32 NONE
#define IMP_U32 NONE
#define IMP_G32 NONE
#define IMP_WMM NONE
#define IMP_ADV NONE
#define IMP_SH NONE
#define IMP_CCTL NONE
#define IMP_CDLG NONE
#define IMP_OLE IAT
#define IMP_GP NONE
iat_OLE:
#include "imports.h"
.long 0
#undef IMP_K32
#undef IMP_U32
#undef IMP_G32
#undef IMP_WMM
#undef IMP_ADV
#undef IMP_SH
#undef IMP_CCTL
#undef IMP_CDLG
#undef IMP_OLE
#undef IMP_GP
#define IMP_K32 NONE
#define IMP_U32 NONE
#define IMP_G32 NONE
#define IMP_WMM NONE
#define IMP_ADV NONE
#define IMP_SH NONE
#define IMP_CCTL NONE
#define IMP_CDLG NONE
#define IMP_OLE NONE
#define IMP_GP IAT
iat_GP:
#include "imports.h"
.long 0
#undef IMP_K32
#undef IMP_U32
#undef IMP_G32
#undef IMP_WMM
#undef IMP_ADV
#undef IMP_SH
#undef IMP_CCTL
#undef IMP_CDLG
#undef IMP_OLE
#undef IMP_GP
#define IMP_K32 HINT
#define IMP_U32 HINT
#define IMP_G32 HINT
#define IMP_WMM HINT
#define IMP_ADV HINT
#define IMP_SH HINT
#define IMP_CCTL HINT
#define IMP_CDLG HINT
#define IMP_OLE HINT
#define IMP_GP HINT
#include "imports.h"
dll_K32: .asciz "KERNEL32.dll"
dll_U32: .asciz "USER32.dll"
dll_G32: .asciz "GDI32.dll"
dll_WMM: .asciz "WINMM.dll"
dll_ADV: .asciz "ADVAPI32.dll"
dll_SH: .asciz "SHELL32.dll"
dll_CCTL: .asciz "COMCTL32.dll"
dll_CDLG: .asciz "COMDLG32.dll"
dll_OLE: .asciz "OLE32.dll"
dll_GP: .asciz "GDIPLUS.dll"
.section .note.GNU-stack,"",@progbits

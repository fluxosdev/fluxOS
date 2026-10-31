/* The import directory for the names in imports.h (pe.ld puts it in .idata, which objcopy makes
 * the PE's import directory). */
#define IB 0x400000
#define IMP(dll, ret, name, args) IMP_##dll(name)
#define IAT(name) .global __imp_##name; __imp_##name: .long hint_##name - IB;
#define HINT(name) hint_##name: .short 0; .asciz #name; .balign 2;
#define NONE(name)

    .section .idata, "aw"
    .long 0, 0, 0, dll_k32 - IB, iat_k32 - IB
    .long 0, 0, 0, dll_u32 - IB, iat_u32 - IB
    .long 0, 0, 0, dll_g32 - IB, iat_g32 - IB
    .long 0, 0, 0, 0, 0

#define IMP_K32 IAT
#define IMP_U32 NONE
#define IMP_G32 NONE
iat_k32:
#include "imports.h"
    .long 0
#undef IMP_K32
#undef IMP_U32
#define IMP_K32 NONE
#define IMP_U32 IAT
iat_u32:
#include "imports.h"
    .long 0
#undef IMP_U32
#undef IMP_G32
#define IMP_U32 NONE
#define IMP_G32 IAT
iat_g32:
#include "imports.h"
    .long 0
#undef IMP_K32
#undef IMP_U32
#undef IMP_G32
#define IMP_K32 HINT
#define IMP_U32 HINT
#define IMP_G32 HINT
#include "imports.h"
dll_k32: .asciz "KERNEL32.dll"
dll_u32: .asciz "USER32.dll"
dll_g32: .asciz "GDI32.dll"

    .section .note.GNU-stack, "", @progbits

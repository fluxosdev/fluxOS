/* NTDLL: the few native functions programs and runtimes reach for directly. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "win32.h"
#include "os.h"

#define A(i) (a[i])
#define P(i) gp(a[i])
#define S(i) gstr(a[i])

static uint32_t os_version_hi(void) { return 5; }

DEF(RtlGetVersion)
{
    uint32_t *v = P(0);
    if (!v || v[0] < 20)
        return 0xC0000004u;
    uint32_t size = v[0];
    memset(v, 0, size);
    v[0] = size;
    v[1] = os_version_hi();
    v[2] = 1;
    v[3] = 2600;
    v[4] = 2;
    return 0;
}
DEF(RtlGetNtVersionNumbers)
{
    if (A(0)) *(uint32_t *)P(0) = 5;
    if (A(1)) *(uint32_t *)P(1) = 1;
    if (A(2)) *(uint32_t *)P(2) = 0xF0000000u | 2600;
    return 0;
}
DEF(NtQuerySystemInformation) { (void)a; return 0xC0000003u; }   /* STATUS_INVALID_INFO_CLASS */
DEF(NtQueryInformationProcess)
{
    /* (handle, class, info, len, retlen) */
    uint32_t cls = A(1);
    uint32_t *info = P(2);
    if (cls == 0 && A(3) >= 24 && info) {      /* ProcessBasicInformation */
        info[0] = 0; info[1] = peb_gp; info[2] = 0; info[3] = 0; info[4] = teb->process_id; info[5] = 0;
        if (A(4)) *(uint32_t *)P(4) = 24;
        return 0;
    }
    if (cls == 7 && A(3) >= 4 && info) {       /* ProcessDebugPort */
        info[0] = 0;
        return 0;
    }
    return 0xC0000003u;
}
DEF(NtQueryInformationThread) { (void)a; return 0xC0000003u; }
DEF(NtSetInformationThread) { (void)a; return 0; }
DEF(NtSetInformationProcess) { (void)a; return 0; }
DEF(NtClose) { handle_close(A(0)); return 0; }
DEF(NtTerminateProcess) { process_exit(A(1)); }
DEF(NtDelayExecution) { (void)a; usleep(1000); return 0; }
DEF(NtYieldExecution) { (void)a; return 0; }
DEF(RtlNtStatusToDosError)
{
    switch (A(0)) {
    case 0: return 0;
    case 0xC0000005: return ERROR_NOACCESS;
    case 0xC000000F: case 0xC0000034: return ERROR_FILE_NOT_FOUND;
    case 0xC000003A: return ERROR_PATH_NOT_FOUND;
    case 0xC0000022: return ERROR_ACCESS_DENIED;
    case 0xC0000008: return ERROR_INVALID_HANDLE;
    case 0xC000000D: return ERROR_INVALID_PARAMETER;
    case 0xC0000017: return ERROR_NOT_ENOUGH_MEMORY;
    case 0xC0000002: return ERROR_INVALID_FUNCTION;
    case 0xC00000BB: return ERROR_NOT_SUPPORTED;
    default: return 317;               /* ERROR_MR_MID_NOT_FOUND */
    }
}
DEF(RtlInitializeCriticalSection) { uint32_t *cs = P(0); memset(cs, 0, 24); cs[1] = (uint32_t)-1; return 0; }
DEF(RtlInitializeCriticalSectionAndSpinCount) { uint32_t *cs = P(0); memset(cs, 0, 24); cs[1] = (uint32_t)-1; return 0; }
DEF(RtlDeleteCriticalSection) { memset(P(0), 0, 24); return 0; }
DEF(RtlEnterCriticalSection) { uint32_t *cs = P(0); cs[1]++; cs[2]++; cs[3] = teb->thread_id; return 0; }
DEF(RtlLeaveCriticalSection) { uint32_t *cs = P(0); cs[1]--; if (cs[2]) cs[2]--; return 0; }
DEF(RtlTryEnterCriticalSection) { api_RtlEnterCriticalSection(a); return 1; }
DEF(RtlAllocateHeap) { return heap_alloc(A(0), A(2), (A(1) & 8) != 0); }
DEF(RtlFreeHeap) { return heap_free(A(0), A(2)) == 0; }
DEF(RtlReAllocateHeap) { return heap_realloc(A(0), A(2), A(3), (A(1) & 8) != 0, 0); }
DEF(RtlSizeHeap) { return heap_size(A(0), A(2)); }
DEF(RtlCreateHeap) { return heap_create(A(3), A(2), A(0)); }
DEF(RtlDestroyHeap) { heap_destroy(A(0)); return 0; }
DEF(RtlGetProcessHeaps) { if (A(0) >= 1 && A(1)) *(uint32_t *)P(1) = process_heap; return 1; }
DEF(RtlImageNtHeader)
{
    const uint8_t *img = P(0);
    if (!img || !vm_readable(A(0), 0x40) || img[0] != 'M' || img[1] != 'Z')
        return 0;
    uint32_t off;
    memcpy(&off, img + 0x3C, 4);
    return A(0) + off;
}
DEF(RtlImageDirectoryEntryToData)
{
    /* (base, mapped, directory, size*) */
    struct module *m = module_by_base(A(0));
    if (!m || A(2) >= 16 || !m->dir[A(2)][0])
        return 0;
    if (A(3)) *(uint32_t *)P(3) = m->dir[A(2)][1];
    return m->base + m->dir[A(2)][0];
}
DEF(RtlInitUnicodeString)
{
    struct unicode_string *u = P(0);
    if (A(1)) {
        int n = wstrlen(gwstr(A(1)));
        u->len = (uint16_t)(n * 2);
        u->max = (uint16_t)(n * 2 + 2);
        u->buf = A(1);
    } else {
        u->len = u->max = 0;
        u->buf = 0;
    }
    return 0;
}
DEF(RtlInitAnsiString)
{
    struct { uint16_t len, max; gp_t buf; } *s = P(0);
    if (A(1)) {
        int n = (int)strlen(gstr(A(1)));
        s->len = (uint16_t)n;
        s->max = (uint16_t)(n + 1);
        s->buf = A(1);
    } else {
        s->len = s->max = 0;
        s->buf = 0;
    }
    return 0;
}
DEF(RtlFreeUnicodeString) { struct unicode_string *u = P(0); if (u && u->buf) gfree(u->buf); return 0; }
DEF(RtlFreeAnsiString) { struct unicode_string *u = P(0); if (u && u->buf) gfree(u->buf); return 0; }
DEF(RtlAnsiStringToUnicodeString)
{
    struct unicode_string *dst = P(0);
    const struct { uint16_t len, max; gp_t buf; } *src = P(1);
    int n = src->len;
    if (A(2)) {
        dst->buf = galloc((uint32_t)(n + 1) * 2);
        dst->max = (uint16_t)((n + 1) * 2);
    }
    if (dst->max < (n + 1) * 2)
        return 0xC0000023u;             /* STATUS_BUFFER_TOO_SMALL */
    ansi_to_wide(gstr(src->buf), n, gwstr(dst->buf), n);
    gwstr(dst->buf)[n] = 0;
    dst->len = (uint16_t)(n * 2);
    return 0;
}
DEF(RtlUnicodeStringToAnsiString)
{
    struct { uint16_t len, max; gp_t buf; } *dst = P(0);
    const struct unicode_string *src = P(1);
    int n = src->len / 2;
    if (A(2)) {
        dst->buf = galloc((uint32_t)n + 1);
        dst->max = (uint16_t)(n + 1);
    }
    if (dst->max < n + 1)
        return 0xC0000023u;
    wide_to_ansi(gwstr(src->buf), n, gstr(dst->buf), n);
    gstr(dst->buf)[n] = 0;
    dst->len = (uint16_t)n;
    return 0;
}
DEF(RtlCompareUnicodeString)
{
    const struct unicode_string *x = P(0), *y = P(1);
    int ci = A(2) != 0;
    int n1 = x->len / 2, n2 = y->len / 2, n = n1 < n2 ? n1 : n2;
    const uint16_t *s1 = gwstr(x->buf), *s2 = gwstr(y->buf);
    for (int i = 0; i < n; i++) {
        uint16_t c1 = s1[i], c2 = s2[i];
        if (ci) {
            if (c1 >= 'A' && c1 <= 'Z') c1 += 32;
            if (c2 >= 'A' && c2 <= 'Z') c2 += 32;
        }
        if (c1 != c2)
            return (uint32_t)((int)c1 - (int)c2);
    }
    return (uint32_t)(n1 - n2);
}
DEF(RtlEqualUnicodeString) { return api_RtlCompareUnicodeString(a) == 0; }
DEF(RtlUpcaseUnicodeChar) { uint32_t c = A(0); return c >= 'a' && c <= 'z' ? c - 32 : c; }
DEF(RtlDowncaseUnicodeChar) { uint32_t c = A(0); return c >= 'A' && c <= 'Z' ? c + 32 : c; }
DEF(RtlUnicodeToMultiByteSize)
{
    /* (size*, wide, bytes) */
    *(uint32_t *)P(0) = A(2) / 2;
    return 0;
}
DEF(RtlMultiByteToUnicodeN)
{
    /* (out, outsize, outlen*, in, insize) */
    uint32_t n = A(4) < A(1) / 2 ? A(4) : A(1) / 2;
    ansi_to_wide(S(3), (int)n, P(0), (int)n);
    if (A(2)) *(uint32_t *)P(2) = n * 2;
    return 0;
}
DEF(RtlUnicodeToMultiByteN)
{
    uint32_t n = A(4) / 2 < A(1) ? A(4) / 2 : A(1);
    wide_to_ansi(P(3), (int)n, P(0), (int)n);
    if (A(2)) *(uint32_t *)P(2) = n;
    return 0;
}
DEF(RtlAcquirePebLock) { (void)a; return 0; }
DEF(RtlReleasePebLock) { (void)a; return 0; }
DEF(RtlGetLastWin32Error) { (void)a; return get_last_error(); }
DEF(RtlSetLastWin32Error) { set_last_error(A(0)); return 0; }
DEF(RtlGetCurrentPeb) { (void)a; return peb_gp; }
DEF(NtCurrentTeb) { (void)a; return teb_gp; }
DEF(RtlAddVectoredExceptionHandler)
{
    if (nvectored >= 8) return 0;
    vectored_handlers[nvectored++] = A(1);
    return A(1);
}
DEF(RtlRemoveVectoredExceptionHandler) { (void)a; return 1; }
DEF(RtlRaiseException)
{
    struct exception_record *rec = P(0);
    struct context ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.flags = CONTEXT_FULL;
    ctx.cs = gate_win_cs; ctx.ds = ctx.es = ctx.ss = gate_win_ds; ctx.fs = gate_win_fs;
    ctx.ebx = api_frame->ebx; ctx.esi = api_frame->esi; ctx.edi = api_frame->edi; ctx.ebp = api_frame->ebp;
    ctx.eip = api_frame->ret_addr; ctx.esp = api_frame->args + 4; ctx.eflags = 0x202;
    struct exception_record local = *rec;
    seh_raise(&local, &ctx);
    return 0;
}
DEF(RtlUnwind_nt)
{
    struct context ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.flags = CONTEXT_FULL;
    ctx.cs = gate_win_cs; ctx.ds = ctx.es = ctx.ss = gate_win_ds; ctx.fs = gate_win_fs;
    ctx.ebx = api_frame->ebx; ctx.esi = api_frame->esi; ctx.edi = api_frame->edi; ctx.ebp = api_frame->ebp;
    ctx.eip = api_frame->ret_addr; ctx.esp = api_frame->args + 16; ctx.eflags = 0x202;
    seh_unwind(A(0), A(1), A(2) ? P(2) : NULL, A(3), &ctx);
    return 0;
}
DEF(RtlRandom) { uint32_t *seed = P(0); *seed = *seed * 1103515245u + 12345u; return *seed >> 1; }
DEF(RtlUniform) { return api_RtlRandom(a); }
DEF(RtlComputeCrc32)
{
    uint32_t crc = ~A(0);
    const uint8_t *p = P(1);
    for (uint32_t i = 0; i < A(2); i++) {
        crc ^= p[i];
        for (int k = 0; k < 8; k++)
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1)));
    }
    return ~crc;
}
DEF(RtlGetThreadPreferredUILanguages) { (void)a; return 0xC00000BBu; }
DEF(RtlQueryPerformanceCounter) { uint64_t us = os_time_us(); memcpy(P(0), &us, 8); return 1; }
DEF(RtlDllShutdownInProgress) { (void)a; return 0; }
DEF(RtlExitUserProcess) { process_exit(A(0)); }
DEF(RtlExitUserThread) { process_exit(A(0)); }
DEF(RtlSetUnhandledExceptionFilter) { gp_t old = unhandled_filter; unhandled_filter = A(0); return old; }
DEF(RtlIsProcessorFeaturePresent) { return A(0) == 3 || A(0) == 6 || A(0) == 10 || A(0) == 8; }
DEF(RtlTimeToSecondsSince1970)
{
    uint64_t t;
    memcpy(&t, P(0), 8);
    *(uint32_t *)P(1) = (uint32_t)(t / 10000000ull - 11644473600ull);
    return 1;
}
DEF(RtlSecondsSince1970ToTime)
{
    uint64_t t = ((uint64_t)A(0) + 11644473600ull) * 10000000ull;
    memcpy(P(1), &t, 8);
    return 0;
}
DEF(NtAllocateVirtualMemory)
{
    /* (process, base**, zerobits, size*, type, protect) */
    gp_t *base = P(1);
    uint32_t *size = P(3);
    uint32_t b[4] = { *base, *size, A(4), A(5) };
    extern struct api_dll kernel32_dll;
    const struct api_entry *e = api_find(&kernel32_dll, "VirtualAlloc");
    gp_t r = e->fn(b);
    if (!r)
        return 0xC0000017u;
    *base = r;
    *size = (*size + 0xFFF) & ~0xFFFu;
    return 0;
}
DEF(NtFreeVirtualMemory)
{
    gp_t *base = P(1);
    uint32_t *size = P(2);
    if (A(3) == MEM_RELEASE)
        vm_free(*base, *size);
    else
        vm_decommit(*base, *size);
    return 0;
}
DEF(NtProtectVirtualMemory)
{
    gp_t *base = P(1);
    uint32_t *size = P(2);
    uint32_t old;
    vm_protect(*base, *size, A(3), &old);
    if (A(4)) *(uint32_t *)P(4) = old;
    return 0;
}
DEF(NtQueryVirtualMemory)
{
    struct vm_info info;
    if (vm_query(A(1), &info) < 0 || A(4) < 28)
        return 0xC000000Du;
    uint32_t *mbi = P(3);
    mbi[0] = info.base; mbi[1] = info.alloc_base; mbi[2] = info.alloc_protect; mbi[3] = info.size;
    mbi[4] = info.state; mbi[5] = info.protect; mbi[6] = info.type;
    if (A(5)) *(uint32_t *)P(5) = 28;
    return 0;
}
DEF(LdrGetDllHandle) { (void)a; return 0xC0000135u; }
DEF(LdrLoadDll)
{
    /* (path, flags, name(UNICODE_STRING*), handle*) */
    const struct unicode_string *u = P(2);
    char name[260];
    wide_to_ansi(gwstr(u->buf), u->len / 2, name, sizeof(name));
    name[u->len / 2 < 259 ? u->len / 2 : 259] = 0;
    char err[256];
    struct module *m = module_load(name, exe_dir_host, err, sizeof(err));
    if (!m)
        return 0xC0000135u;
    *(uint32_t *)P(3) = m->base;
    return 0;
}
DEF(LdrGetProcedureAddress)
{
    /* (module, name(ANSI_STRING*), ordinal, address*) */
    struct module *m = module_by_base(A(0));
    if (!m)
        return 0xC0000135u;
    gp_t r = 0;
    if (A(1)) {
        const struct { uint16_t len, max; gp_t buf; } *s = P(1);
        char name[256];
        snprintf(name, sizeof(name), "%.*s", s->len, gstr(s->buf));
        r = m->builtin ? loader_resolve(m->name, name, 0, NULL) : pe_export(m, name, 0);
    } else {
        r = m->builtin ? loader_resolve(m->name, NULL, (int)A(2), NULL) : pe_export(m, NULL, (int)A(2));
    }
    if (!r)
        return 0xC000007Au;
    *(uint32_t *)P(3) = r;
    return 0;
}
DEF(DbgPrint) { if (api_trace) fprintf(stderr, "[DbgPrint] %s", S(0)); return 0; }
DEF(DbgBreakPoint) { (void)a; return 0; }
DEF(RtlNormalizeProcessParams) { return A(0); }
DEF(wcslen_nt) { return (uint32_t)wstrlen(P(0)); }
DEF(strlen_nt) { return (uint32_t)strlen(S(0)); }
DEF(memcpy_nt) { memcpy(P(0), P(1), A(2)); return A(0); }
DEF(memset_nt) { memset(P(0), (int)A(1), A(2)); return A(0); }
DEF(memmove_nt) { memmove(P(0), P(1), A(2)); return A(0); }
DEF(memcmp_nt) { return (uint32_t)memcmp(P(0), P(1), A(2)); }
DEF(_stricmp_nt) { return (uint32_t)strcasecmp(S(0), S(1)); }
DEF(_strnicmp_nt) { return (uint32_t)strncasecmp(S(0), S(1), A(2)); }
DEF(_wcsicmp_nt) { return (uint32_t)wstrcasecmp(P(0), P(1)); }
DEF(wcscmp_nt) { return (uint32_t)wstrcmp(P(0), P(1)); }
DEF(strcmp_nt) { return (uint32_t)strcmp(S(0), S(1)); }
DEF(strncmp_nt) { return (uint32_t)strncmp(S(0), S(1), A(2)); }
DEF(strchr_nt) { char *r = strchr(S(0), (int)A(1)); return r ? GA(r) : 0; }
DEF(strrchr_nt) { char *r = strrchr(S(0), (int)A(1)); return r ? GA(r) : 0; }
DEF(strstr_nt) { char *r = strstr(S(0), S(1)); return r ? GA(r) : 0; }
DEF(toupper_nt) { uint32_t c = A(0); return c >= 'a' && c <= 'z' ? c - 32 : c; }
DEF(tolower_nt) { uint32_t c = A(0); return c >= 'A' && c <= 'Z' ? c + 32 : c; }
DEF(_vsnprintf_nt);
DEF(sprintf_nt);
DEF(_snprintf_nt);
DEF(_vsnwprintf_nt);
DEF(swprintf_nt);
DEF(_snwprintf_nt);
DEF(atoi_nt) { return (uint32_t)atoi(S(0)); }
DEF(atol_nt) { return (uint32_t)atol(S(0)); }
DEF(strtoul_nt) { char *e; unsigned long r = strtoul(S(0), &e, (int)A(2)); if (A(1)) *(uint32_t *)P(1) = GA(e); return (uint32_t)r; }
DEF(_ultoa_nt) { static const char digits[] = "0123456789abcdefghijklmnopqrstuvwxyz"; char tmp[36]; int n = 0; uint32_t v = A(0), b = A(2); if (b < 2 || b > 36) b = 10; do { tmp[n++] = digits[v % b]; v /= b; } while (v); char *out = P(1); for (int i = 0; i < n; i++) out[i] = tmp[n - 1 - i]; out[n] = 0; return A(1); }
DEF(_itoa_nt) { int32_t v = (int32_t)A(0); char *out = P(1); if (v < 0 && A(2) == 10) { *out++ = '-'; uint32_t b[3] = { (uint32_t)-v, GA(out), 10 }; api__ultoa_nt(b); return A(1); } return api__ultoa_nt(a); }
DEF(isdigit_nt) { return A(0) >= '0' && A(0) <= '9'; }
DEF(isspace_nt) { return A(0) == ' ' || (A(0) >= 9 && A(0) <= 13); }
DEF(isalpha_nt) { uint32_t c = A(0) | 0x20; return c >= 'a' && c <= 'z'; }
DEF(NtQuerySystemTime) { uint64_t t = (os_time_us() + 11644473600ull * 1000000ull) * 10; memcpy(P(0), &t, 8); return 0; }
DEF(RtlSystemTimeToLocalTime) { uint64_t t; memcpy(&t, P(0), 8); t += (uint64_t)((int64_t)os_tz_minutes() * 600000000); memcpy(P(1), &t, 8); return 0; }
DEF(RtlLocalTimeToSystemTime) { uint64_t t; memcpy(&t, P(0), 8); t -= (uint64_t)((int64_t)os_tz_minutes() * 600000000); memcpy(P(1), &t, 8); return 0; }
DEF(RtlTimeToTimeFields)
{
    uint64_t t;
    memcpy(&t, P(0), 8);
    SYSTEMTIME st;
    extern void filetime_to_systemtime_ex(uint64_t ft, SYSTEMTIME *st);
    filetime_to_systemtime_ex(t, &st);
    uint16_t *tf = P(1);
    tf[0] = st.year; tf[1] = st.month; tf[2] = st.day; tf[3] = st.hour; tf[4] = st.minute; tf[5] = st.second; tf[6] = st.ms; tf[7] = st.dow;
    return 0;
}
DEF(RtlAreBitsSet) { (void)a; return 0; }
DEF(RtlInitializeBitMap) { uint32_t *bm = P(0); bm[0] = A(2); bm[1] = A(1); return 0; }
DEF(RtlClearAllBits) { uint32_t *bm = P(0); memset(gp(bm[1]), 0, (bm[0] + 7) / 8); return 0; }
DEF(RtlSetBits) { uint32_t *bm = P(0); uint8_t *bits = gp(bm[1]); for (uint32_t i = A(1); i < A(1) + A(2) && i < bm[0]; i++) bits[i / 8] |= (uint8_t)(1 << (i % 8)); return 0; }
DEF(RtlClearBits) { uint32_t *bm = P(0); uint8_t *bits = gp(bm[1]); for (uint32_t i = A(1); i < A(1) + A(2) && i < bm[0]; i++) bits[i / 8] &= (uint8_t)~(1 << (i % 8)); return 0; }
DEF(RtlFindClearBitsAndSet)
{
    uint32_t *bm = P(0);
    uint8_t *bits = gp(bm[1]);
    uint32_t want = A(1);
    for (uint32_t i = 0; i + want <= bm[0]; i++) {
        uint32_t k;
        for (k = 0; k < want && !(bits[(i + k) / 8] & (1 << ((i + k) % 8))); k++) ;
        if (k == want) {
            for (k = 0; k < want; k++) bits[(i + k) / 8] |= (uint8_t)(1 << ((i + k) % 8));
            return i;
        }
    }
    return 0xFFFFFFFFu;
}
DEF(RtlNumberOfSetBits) { uint32_t *bm = P(0); const uint8_t *bits = gp(bm[1]); uint32_t n = 0; for (uint32_t i = 0; i < bm[0]; i++) n += (bits[i / 8] >> (i % 8)) & 1; return n; }
DEF(RtlCheckBit) { uint32_t *bm = P(0); const uint8_t *bits = gp(bm[1]); return (bits[A(1) / 8] >> (A(1) % 8)) & 1; }
DEF(RtlFlushSecureMemoryCache) { (void)a; return 1; }
DEF(RtlEncodePointer) { return A(0) ^ 0x5A5A5A5Au; }
DEF(RtlDecodePointer) { return A(0) ^ 0x5A5A5A5Au; }
DEF(RtlInterlockedPushEntrySList) { uint32_t *h = P(0), *e = P(1); e[0] = h[0]; h[0] = A(1); return e[0]; }
DEF(RtlInterlockedPopEntrySList) { uint32_t *h = P(0); uint32_t e = h[0]; if (e) h[0] = *(uint32_t *)GP(e); return e; }
DEF(RtlInitializeSListHead) { uint32_t *h = P(0); h[0] = h[1] = 0; return 0; }
DEF(RtlQueryDepthSList) { uint32_t *h = P(0); return h[1] & 0xFFFF; }
DEF(RtlWow64EnableFsRedirection) { (void)a; return 0; }
DEF(RtlGetCompressionWorkSpaceSize) { (void)a; return 0xC00000BBu; }
DEF(NtOpenFile) { (void)a; return 0xC0000034u; }
DEF(NtCreateFile) { (void)a; return 0xC0000034u; }
DEF(NtReadFile) { (void)a; return 0xC0000008u; }
DEF(NtWriteFile) { (void)a; return 0xC0000008u; }
DEF(NtDeviceIoControlFile) { (void)a; return 0xC0000002u; }
DEF(NtQueryDirectoryFile) { (void)a; return 0xC0000008u; }
DEF(NtQueryInformationFile) { (void)a; return 0xC0000008u; }
DEF(NtSetInformationFile) { (void)a; return 0xC0000008u; }
DEF(NtQueryAttributesFile) { (void)a; return 0xC0000034u; }
DEF(NtCreateEvent) { *(uint32_t *)P(0) = handle_new(H_EVENT); return 0; }
DEF(NtSetEvent) { struct hobj *o = handle_get_type(A(0), H_EVENT); if (o) o->signaled = 1; return 0; }
DEF(NtWaitForSingleObject) { (void)a; return 0; }
DEF(NtRaiseHardError) { (void)a; return 0; }
DEF(NtQueryKey) { (void)a; return 0xC0000034u; }
DEF(NtOpenKey) { (void)a; return 0xC0000034u; }
DEF(NtQueryValueKey) { (void)a; return 0xC0000034u; }
DEF(RtlOpenCurrentUser) { (void)a; return 0xC0000034u; }
DEF(RtlQueryEnvironmentVariable_U) { (void)a; return 0xC0000100u; }
DEF(RtlExpandEnvironmentStrings_U) { (void)a; return 0xC0000100u; }
DEF(RtlDosPathNameToNtPathName_U) { (void)a; return 0; }
DEF(RtlGetFullPathName_U) { (void)a; return 0; }
DEF(RtlDetermineDosPathNameType_U) { (void)a; return 1; }
DEF(RtlIsDosDeviceName_U) { (void)a; return 0; }
DEF(RtlGetNtProductType) { if (A(0)) *(uint32_t *)P(0) = 1; return 1; }
DEF(RtlGetNtGlobalFlags) { (void)a; return 0; }
DEF(RtlQueryProcessDebugInformation) { (void)a; return 0; }
DEF(RtlSetProcessIsCritical) { (void)a; return 0xC0000061u; }
DEF(RtlAdjustPrivilege) { (void)a; return 0xC0000061u; }
DEF(RtlUnhandledExceptionFilter) { (void)a; return 1; }
DEF(RtlUnhandledExceptionFilter2) { (void)a; return 1; }
DEF(RtlInstallFunctionTableCallback) { (void)a; return 1; }
DEF(RtlAddFunctionTable) { (void)a; return 1; }
DEF(RtlDeleteFunctionTable) { (void)a; return 1; }
DEF(RtlRestoreContext) { struct context *c = P(0); struct guest_regs r = { c->edi, c->esi, c->ebp, c->esp, c->ebx, c->edx, c->ecx, c->eax, c->eip, c->eflags }; guest_resume(&r); }
DEF(RtlCaptureContext_nt) { extern struct api_dll kernel32_dll; return api_find(&kernel32_dll, "RtlCaptureContext")->fn(a); }
DEF(RtlUserThreadStart) { (void)a; return 0; }
DEF(RtlGetProductInfo) { if (A(4)) *(uint32_t *)P(4) = 1; return 1; }
DEF(RtlVerifyVersionInfo) { (void)a; return 0; }
DEF(RtlWalkHeap) { (void)a; return 0xC0000120u; }
DEF(RtlValidateHeap) { return heap_validate(A(0), A(2)); }
DEF(RtlLockHeap) { (void)a; return 1; }
DEF(RtlUnlockHeap) { (void)a; return 1; }
DEF(RtlSetHeapInformation) { (void)a; return 0; }
DEF(RtlCreateUnicodeStringFromAsciiz)
{
    struct unicode_string *u = P(0);
    const char *s = S(1);
    int n = (int)strlen(s);
    u->buf = galloc((uint32_t)(n + 1) * 2);
    ansi_to_wide(s, -1, gwstr(u->buf), n + 1);
    u->len = (uint16_t)(n * 2);
    u->max = (uint16_t)((n + 1) * 2);
    return 1;
}
DEF(RtlAppendUnicodeToString)
{
    struct unicode_string *u = P(0);
    const uint16_t *s = P(1);
    int n = wstrlen(s);
    if (u->len + n * 2 + 2 > u->max)
        return 0xC0000023u;
    memcpy((uint8_t *)gp(u->buf) + u->len, s, (size_t)(n + 1) * 2);
    u->len += (uint16_t)(n * 2);
    return 0;
}
DEF(RtlPrefixUnicodeString)
{
    const struct unicode_string *x = P(0), *y = P(1);
    if (x->len > y->len) return 0;
    const uint16_t *s1 = gwstr(x->buf), *s2 = gwstr(y->buf);
    for (int i = 0; i < x->len / 2; i++) {
        uint16_t c1 = s1[i], c2 = s2[i];
        if (A(2)) { if (c1 >= 'a' && c1 <= 'z') c1 -= 32; if (c2 >= 'a' && c2 <= 'z') c2 -= 32; }
        if (c1 != c2) return 0;
    }
    return 1;
}
DEF(RtlIntegerToUnicodeString) { (void)a; return 0xC00000BBu; }
DEF(RtlUnicodeStringToInteger) { const struct unicode_string *u = P(0); *(uint32_t *)P(2) = (uint32_t)strtoul(wtmp_n(gwstr(u->buf), u->len / 2), NULL, (int)A(1)); return 0; }
DEF(RtlCharToInteger) { *(uint32_t *)P(2) = (uint32_t)strtoul(S(0), NULL, (int)A(1)); return 0; }
DEF(RtlIntegerToChar) { (void)a; return 0xC00000BBu; }
DEF(RtlUpperChar) { uint32_t c = A(0); return c >= 'a' && c <= 'z' ? c - 32 : c; }
DEF(RtlUpperString) { (void)a; return 0; }
DEF(RtlHashUnicodeString)
{
    const struct unicode_string *u = P(0);
    uint32_t h = 0;
    const uint16_t *s = gwstr(u->buf);
    for (int i = 0; i < u->len / 2; i++) {
        uint16_t c = s[i];
        if (A(1) && c >= 'a' && c <= 'z') c -= 32;
        h = h * 65599 + c;
    }
    *(uint32_t *)P(3) = h;
    return 0;
}
DEF(NtQueryPerformanceCounter)
{
    uint64_t us = os_time_us(), f = 1000000;
    memcpy(P(0), &us, 8);
    if (A(1)) memcpy(P(1), &f, 8);
    return 0;
}
DEF(NtGetTickCount) { (void)a; return os_ticks_ms(); }
DEF(RtlGetSystemTimePrecise) { (void)a; uint64_t t = (os_time_us() + 11644473600ull * 1000000ull) * 10; api_edx = (uint32_t)(t >> 32); return (uint32_t)t; }
DEF(RtlQueryRegistryValues) { (void)a; return 0xC0000034u; }
DEF(RtlCompareMemory_nt) { const uint8_t *x = P(0), *y = P(1); uint32_t i; for (i = 0; i < A(2) && x[i] == y[i]; i++) ; return i; }
DEF(RtlCopyUnicodeString)
{
    struct unicode_string *d = P(0);
    const struct unicode_string *s = P(1);
    if (!s) { d->len = 0; return 0; }
    uint16_t n = s->len < d->max ? s->len : d->max;
    memcpy(gp(d->buf), gp(s->buf), n);
    d->len = n;
    if (n < d->max) gwstr(d->buf)[n / 2] = 0;
    return 0;
}
DEF(RtlDuplicateUnicodeString)
{
    const struct unicode_string *s = P(1);
    struct unicode_string *d = P(2);
    d->buf = galloc(s->len + 2);
    memcpy(gp(d->buf), gp(s->buf), s->len);
    d->len = s->len;
    d->max = (uint16_t)(s->len + 2);
    return 0;
}
DEF(RtlUpcaseUnicodeString)
{
    struct unicode_string *d = P(0);
    const struct unicode_string *s = P(1);
    if (A(2)) { d->buf = galloc(s->len + 2); d->max = (uint16_t)(s->len + 2); }
    uint16_t *o = gwstr(d->buf);
    const uint16_t *i = gwstr(s->buf);
    int n = s->len / 2;
    for (int k = 0; k < n; k++)
        o[k] = i[k] >= 'a' && i[k] <= 'z' ? (uint16_t)(i[k] - 32) : i[k];
    d->len = s->len;
    return 0;
}
DEF(RtlDowncaseUnicodeString)
{
    struct unicode_string *d = P(0);
    const struct unicode_string *s = P(1);
    if (A(2)) { d->buf = galloc(s->len + 2); d->max = (uint16_t)(s->len + 2); }
    uint16_t *o = gwstr(d->buf);
    const uint16_t *i = gwstr(s->buf);
    int n = s->len / 2;
    for (int k = 0; k < n; k++)
        o[k] = i[k] >= 'A' && i[k] <= 'Z' ? (uint16_t)(i[k] + 32) : i[k];
    d->len = s->len;
    return 0;
}
DEF(RtlMultiByteToUnicodeSize) { *(uint32_t *)P(0) = A(2) * 2; return 0; }
DEF(RtlOemToUnicodeN) { return api_RtlMultiByteToUnicodeN(a); }
DEF(RtlUnicodeToOemN) { return api_RtlUnicodeToMultiByteN(a); }
DEF(RtlAnsiCharToUnicodeChar) { gp_t *pp = P(0); uint8_t c = *(uint8_t *)gp(*pp); (*pp)++; return c; }
DEF(RtlxUnicodeStringToAnsiSize) { const struct unicode_string *u = P(0); return u->len / 2 + 1; }
DEF(RtlxAnsiStringToUnicodeSize) { const struct { uint16_t len, max; gp_t buf; } *s = P(0); return (uint32_t)(s->len + 1) * 2; }

/* printf-style functions from the C runtime, as ntdll exports them */
extern struct api_dll msvcrt_dll;
static uint32_t crt_call(const char *name, uint32_t *a)
{
    const struct api_entry *e = api_find(&msvcrt_dll, name);
    return e ? e->fn(a) : 0;
}
DEF(_vsnprintf_nt) { return crt_call("_vsnprintf", a); }
DEF(sprintf_nt) { return crt_call("sprintf", a); }
DEF(_snprintf_nt) { return crt_call("_snprintf", a); }
DEF(_vsnwprintf_nt) { return crt_call("_vsnwprintf", a); }
DEF(swprintf_nt) { return crt_call("swprintf", a); }
DEF(_snwprintf_nt) { return crt_call("_snwprintf", a); }

#define NT(nm, n) { #nm, api_##nm, (n), 0, NULL, 0 }
#define NTC(nm, fn) { #nm, api_##fn, 0, API_CDECL, NULL, 0 }

static const struct api_entry ntdll_entries[] = {
    NT(RtlGetVersion, 1), NT(RtlGetNtVersionNumbers, 3), NT(NtQuerySystemInformation, 4),
    NT(NtQueryInformationProcess, 5), NT(NtQueryInformationThread, 5), NT(NtSetInformationThread, 4),
    NT(NtSetInformationProcess, 4), NT(NtClose, 1), NT(NtTerminateProcess, 2), NT(NtDelayExecution, 2),
    NT(NtYieldExecution, 0), NT(RtlNtStatusToDosError, 1), NT(RtlInitializeCriticalSection, 1),
    NT(RtlInitializeCriticalSectionAndSpinCount, 2), NT(RtlDeleteCriticalSection, 1), NT(RtlEnterCriticalSection, 1),
    NT(RtlLeaveCriticalSection, 1), NT(RtlTryEnterCriticalSection, 1), NT(RtlAllocateHeap, 3), NT(RtlFreeHeap, 3),
    NT(RtlReAllocateHeap, 4), NT(RtlSizeHeap, 3), NT(RtlCreateHeap, 6), NT(RtlDestroyHeap, 1), NT(RtlGetProcessHeaps, 2),
    NT(RtlImageNtHeader, 1), NT(RtlImageDirectoryEntryToData, 4), NT(RtlInitUnicodeString, 2), NT(RtlInitAnsiString, 2),
    NT(RtlFreeUnicodeString, 1), NT(RtlFreeAnsiString, 1), NT(RtlAnsiStringToUnicodeString, 3),
    NT(RtlUnicodeStringToAnsiString, 3), NT(RtlCompareUnicodeString, 3), NT(RtlEqualUnicodeString, 3),
    NT(RtlUpcaseUnicodeChar, 1), NT(RtlDowncaseUnicodeChar, 1), NT(RtlUnicodeToMultiByteSize, 3),
    NT(RtlMultiByteToUnicodeN, 5), NT(RtlUnicodeToMultiByteN, 5), NT(RtlAcquirePebLock, 0), NT(RtlReleasePebLock, 0),
    NT(RtlGetLastWin32Error, 0), NT(RtlSetLastWin32Error, 1), NT(RtlGetCurrentPeb, 0), NT(NtCurrentTeb, 0),
    NT(RtlAddVectoredExceptionHandler, 2), NT(RtlRemoveVectoredExceptionHandler, 1), NT(RtlRaiseException, 1),
    { "RtlUnwind", api_RtlUnwind_nt, 4, 0, NULL, 0 }, NT(RtlRandom, 1), NT(RtlUniform, 1), NT(RtlComputeCrc32, 3),
    NT(RtlGetThreadPreferredUILanguages, 4), NT(RtlQueryPerformanceCounter, 1), NT(RtlDllShutdownInProgress, 0),
    NT(RtlExitUserProcess, 1), NT(RtlExitUserThread, 1), NT(RtlSetUnhandledExceptionFilter, 1),
    NT(RtlIsProcessorFeaturePresent, 1), NT(RtlTimeToSecondsSince1970, 2), NT(RtlSecondsSince1970ToTime, 2),
    NT(NtAllocateVirtualMemory, 6), NT(NtFreeVirtualMemory, 4), NT(NtProtectVirtualMemory, 5), NT(NtQueryVirtualMemory, 6),
    NT(LdrGetDllHandle, 4), NT(LdrLoadDll, 4), NT(LdrGetProcedureAddress, 4), NTC(DbgPrint, DbgPrint), NT(DbgBreakPoint, 0),
    NT(RtlNormalizeProcessParams, 1),
    NTC(wcslen, wcslen_nt), NTC(strlen, strlen_nt), NTC(memcpy, memcpy_nt), NTC(memset, memset_nt), NTC(memmove, memmove_nt),
    NTC(memcmp, memcmp_nt), NTC(_stricmp, _stricmp_nt), NTC(_strnicmp, _strnicmp_nt), NTC(_wcsicmp, _wcsicmp_nt),
    NTC(wcscmp, wcscmp_nt), NTC(strcmp, strcmp_nt), NTC(strncmp, strncmp_nt), NTC(strchr, strchr_nt), NTC(strrchr, strrchr_nt),
    NTC(strstr, strstr_nt), NTC(toupper, toupper_nt), NTC(tolower, tolower_nt), NTC(_vsnprintf, _vsnprintf_nt),
    NTC(sprintf, sprintf_nt), NTC(_snprintf, _snprintf_nt), NTC(_vsnwprintf, _vsnwprintf_nt), NTC(swprintf, swprintf_nt),
    NTC(_snwprintf, _snwprintf_nt), NTC(atoi, atoi_nt), NTC(atol, atol_nt), NTC(strtoul, strtoul_nt), NTC(_ultoa, _ultoa_nt),
    NTC(_itoa, _itoa_nt), NTC(isdigit, isdigit_nt), NTC(isspace, isspace_nt), NTC(isalpha, isalpha_nt),
    NT(NtQuerySystemTime, 1), NT(RtlSystemTimeToLocalTime, 2), NT(RtlLocalTimeToSystemTime, 2), NT(RtlTimeToTimeFields, 2),
    NT(RtlAreBitsSet, 3), NT(RtlInitializeBitMap, 3), NT(RtlClearAllBits, 1), NT(RtlSetBits, 3), NT(RtlClearBits, 3),
    NT(RtlFindClearBitsAndSet, 3), NT(RtlNumberOfSetBits, 1), NT(RtlCheckBit, 2), NT(RtlFlushSecureMemoryCache, 2),
    NT(RtlEncodePointer, 1), NT(RtlDecodePointer, 1), NT(RtlInterlockedPushEntrySList, 2), NT(RtlInterlockedPopEntrySList, 1),
    NT(RtlInitializeSListHead, 1), NT(RtlQueryDepthSList, 1), NT(RtlWow64EnableFsRedirection, 1),
    NT(RtlGetCompressionWorkSpaceSize, 3), NT(NtOpenFile, 6), NT(NtCreateFile, 11), NT(NtReadFile, 9), NT(NtWriteFile, 9),
    NT(NtDeviceIoControlFile, 10), NT(NtQueryDirectoryFile, 11), NT(NtQueryInformationFile, 5), NT(NtSetInformationFile, 5),
    NT(NtQueryAttributesFile, 2), NT(NtCreateEvent, 5), NT(NtSetEvent, 2), NT(NtWaitForSingleObject, 3),
    NT(NtRaiseHardError, 6), NT(NtQueryKey, 5), NT(NtOpenKey, 3), NT(NtQueryValueKey, 6), NT(RtlOpenCurrentUser, 2),
    NT(RtlQueryEnvironmentVariable_U, 3), NT(RtlExpandEnvironmentStrings_U, 4), NT(RtlDosPathNameToNtPathName_U, 4),
    NT(RtlGetFullPathName_U, 4), NT(RtlDetermineDosPathNameType_U, 1), NT(RtlIsDosDeviceName_U, 1),
    NT(RtlGetNtProductType, 1), NT(RtlGetNtGlobalFlags, 0), NT(RtlQueryProcessDebugInformation, 3),
    NT(RtlSetProcessIsCritical, 3), NT(RtlAdjustPrivilege, 4), NT(RtlUnhandledExceptionFilter, 1),
    NT(RtlUnhandledExceptionFilter2, 2), NT(RtlInstallFunctionTableCallback, 6), NT(RtlAddFunctionTable, 3),
    NT(RtlDeleteFunctionTable, 1), NT(RtlRestoreContext, 2), { "RtlCaptureContext", api_RtlCaptureContext_nt, 1, 0, NULL, 0 },
    NT(RtlUserThreadStart, 2), NT(RtlGetProductInfo, 5), NT(RtlVerifyVersionInfo, 4), NT(RtlWalkHeap, 2),
    NT(RtlValidateHeap, 3), NT(RtlLockHeap, 1), NT(RtlUnlockHeap, 1), NT(RtlSetHeapInformation, 4),
    NT(RtlCreateUnicodeStringFromAsciiz, 2), NT(RtlAppendUnicodeToString, 2), NT(RtlPrefixUnicodeString, 3),
    NT(RtlIntegerToUnicodeString, 3), NT(RtlUnicodeStringToInteger, 3), NT(RtlCharToInteger, 3), NT(RtlIntegerToChar, 4),
    NT(RtlUpperChar, 1), NT(RtlUpperString, 2), NT(RtlHashUnicodeString, 4), NT(NtQueryPerformanceCounter, 2),
    NT(NtGetTickCount, 0), NT(RtlGetSystemTimePrecise, 0), NT(RtlQueryRegistryValues, 5),
    { "RtlCompareMemory", api_RtlCompareMemory_nt, 3, 0, NULL, 0 }, NT(RtlCopyUnicodeString, 2),
    NT(RtlDuplicateUnicodeString, 3), NT(RtlUpcaseUnicodeString, 3), NT(RtlDowncaseUnicodeString, 3),
    NT(RtlMultiByteToUnicodeSize, 3), NT(RtlOemToUnicodeN, 5), NT(RtlUnicodeToOemN, 5), NT(RtlAnsiCharToUnicodeChar, 1),
    NT(RtlxUnicodeStringToAnsiSize, 1), NT(RtlxAnsiStringToUnicodeSize, 1),
};

struct api_dll ntdll_dll = { "NTDLL.DLL", ntdll_entries, sizeof(ntdll_entries) / sizeof(ntdll_entries[0]), NULL };

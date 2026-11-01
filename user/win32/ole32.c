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
static const struct api_entry entries[]={
    API(CoTaskMemAlloc,1),API(CoTaskMemFree,1),API(CoTaskMemRealloc,2)
};
struct api_dll ole32_dll={
    .name="OLE32.DLL",.entries=entries,.count=sizeof(entries)/sizeof(entries[0])
};

/* Shell path helpers. Keep offsets in guest character units so pointer-returning
 * APIs point into the caller's original ANSI or UTF-16 buffer. */
#include "win32.h"

static uint16_t path_char(gp_t path, uint32_t i, int wide)
{
    return wide ? ((const uint16_t *)GP(path))[i] : ((const uint8_t *)GP(path))[i];
}

static gp_t path_find(gp_t path, int wide, int extension)
{
    if (!path)
        return 0;
    uint32_t found = extension ? 0xffffffffu : 0, i;
    for (i = 0; path_char(path, i, wide); i++) {
        uint16_t c = path_char(path, i, wide);
        if (extension) {
            if (c == '\\' || c == ' ')
                found = 0xffffffffu;
            else if (c == '.')
                found = i;
        } else if ((c == '\\' || c == '/' || c == ':') &&
                   path_char(path, i + 1, wide)) {
            found = i + 1;
        }
    }
    return path + (extension && found == 0xffffffffu ? i : found) * (wide ? 2u : 1u);
}

static uint32_t path_relative(gp_t path, int wide)
{
    if (!path || !path_char(path, 0, wide))
        return TRUE;
    return path_char(path, 0, wide) != '\\' && path_char(path, 1, wide) != ':';
}

static uint32_t path_remove_extension(gp_t path, int wide)
{
    gp_t ext = path_find(path, wide, 1);
    if (ext) {
        if (wide)
            *(uint16_t *)GP(ext) = 0;
        else
            *(uint8_t *)GP(ext) = 0;
    }
    return 0;
}

DEF(PathFindFileNameA) { return path_find(a[0], 0, 0); }
DEF(PathFindFileNameW) { return path_find(a[0], 1, 0); }
DEF(PathFindExtensionA) { return path_find(a[0], 0, 1); }
DEF(PathFindExtensionW) { return path_find(a[0], 1, 1); }
DEF(PathRemoveExtensionA) { return path_remove_extension(a[0], 0); }
DEF(PathRemoveExtensionW) { return path_remove_extension(a[0], 1); }
DEF(PathIsRelativeA) { return path_relative(a[0], 0); }
DEF(PathIsRelativeW) { return path_relative(a[0], 1); }

static const struct api_entry shlwapi_entries[] = {
    API(PathFindFileNameA, 1), API(PathFindFileNameW, 1),
    API(PathFindExtensionA, 1), API(PathFindExtensionW, 1),
    API(PathRemoveExtensionA, 1), API(PathRemoveExtensionW, 1),
    API(PathIsRelativeA, 1), API(PathIsRelativeW, 1)
};
struct api_dll shlwapi_dll = {
    .name = "SHLWAPI.DLL", .entries = shlwapi_entries,
    .count = sizeof(shlwapi_entries) / sizeof(shlwapi_entries[0])
};

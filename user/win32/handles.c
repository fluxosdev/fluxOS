/* Kernel object handles: files, events, processes and the rest. */
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "win32.h"

#define MAX_HANDLES 1024
static struct hobj *table[MAX_HANDLES];
HANDLE std_handles[3];

static HANDLE h_value(int i) { return (HANDLE)((i + 1) * 4); }
static int h_index(HANDLE h)
{
    if (!h || (h & 3) || h == INVALID_HANDLE_VALUE)
        return -1;
    int i = (int)(h / 4) - 1;
    return i >= 0 && i < MAX_HANDLES && table[i] ? i : -1;
}

HANDLE handle_new(enum htype type)
{
    for (int i = 0; i < MAX_HANDLES; i++) {
        if (!table[i]) {
            struct hobj *o = calloc(1, sizeof(*o));
            if (!o)
                return 0;
            o->type = type;
            o->refs = 1;
            o->fd = -1;
            table[i] = o;
            return h_value(i);
        }
    }
    return 0;
}

struct hobj *handle_get(HANDLE h)
{
    int i = h_index(h);
    return i < 0 ? NULL : table[i];
}

struct hobj *handle_get_type(HANDLE h, enum htype type)
{
    struct hobj *o = handle_get(h);
    return o && o->type == type ? o : NULL;
}

static void release(struct hobj *o)
{
    if (--o->refs > 0)
        return;
    switch (o->type) {
    case H_FILE: case H_PIPE_R: case H_PIPE_W:
        if (o->fd > 2)
            close(o->fd);
        break;
    case H_MAPPING:
        if (o->priv && ((struct mapping *)o->priv)->fd >= 0)
            close(((struct mapping *)o->priv)->fd);
        break;
    case H_CONSOLE_IN: case H_CONSOLE_OUT:
        break;
    default:
        break;
    }
    free(o->path);
    free(o->priv);
    free(o);
}

int handle_close(HANDLE h)
{
    int i = h_index(h);
    if (i < 0)
        return -1;
    struct hobj *o = table[i];
    table[i] = NULL;
    release(o);
    return 0;
}

HANDLE handle_dup(HANDLE h)
{
    int i = h_index(h);
    if (i < 0)
        return 0;
    for (int k = 0; k < MAX_HANDLES; k++) {
        if (!table[k]) {
            table[k] = table[i];
            table[k]->refs++;
            return h_value(k);
        }
    }
    return 0;
}

HANDLE handle_for_fd(int fd, enum htype type, int console)
{
    HANDLE h = handle_new(type);
    struct hobj *o = handle_get(h);
    if (!o)
        return 0;
    o->fd = fd;
    o->console = console;
    o->access = GENERIC_READ | GENERIC_WRITE;
    return h;
}

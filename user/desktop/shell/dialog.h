/* The dialogs' controls and framework (apps.c), for the dialogs built on it (sysdlg.c). */
#ifndef DIALOG_H
#define DIALOG_H

#include "desktop.h"

enum ctl_type { CT_LABEL, CT_BOLD, CT_ICON, CT_BUTTON, CT_RADIO, CT_EDIT, CT_SEPARATOR };

struct control {
    enum ctl_type type;
    struct rect r;
    char text[96];
    int id;                     /* buttons: command; radios: value */
};

#define MAX_CONTROLS 24

struct dialog {
    struct window *win;
    struct control ctl[MAX_CONTROLS];
    int n;
    int focus;                  /* focused control, or -1 */
    int pressed;                /* button held down with the mouse, or -1 */
    int pressed_in;             /* ...and the pointer is still over it */
    int radio;                  /* value of the checked radio button */
    int default_id, cancel_id;
    char edit[128];
    int edit_len, edit_cur;
    int masked;                 /* the edit field shows asterisks (a password) */
    void (*command)(struct dialog *d, int id);
    struct dialog **self;       /* cleared when the dialog closes */
    /* message boxes and prompts: what to call when the user agrees */
    void (*on_yes)(void *ctx);
    void (*on_no)(void *ctx);
    void (*on_text)(void *ctx, const char *text);
    void (*on_cancel)(void *ctx);       /* a prompt closed without an answer */
    void *ctx;
};

enum { ID_OK = 1, ID_CANCEL, ID_YES, ID_NO, ID_SLEEP, ID_DIMMER, ID_BRIGHTER };

struct control *dialog_add(struct dialog *d, enum ctl_type type, int x, int y, int w, int h,
                           const char *text, int id);
/* A dialog's window; *self points at it until it closes. NULL if it cannot be made. */
struct dialog *dialog_new(const char *title, enum icon_id icon, int w, int h, int flags,
                          struct dialog **self);
void dialog_show(struct dialog *d, int focus_index);   /* centered, with that control focused */
void dialog_paint(struct dialog *d);
void dialog_close(struct dialog *d);                    /* frees d */

#endif

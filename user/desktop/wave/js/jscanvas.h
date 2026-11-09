/* <canvas> for scripts (jscanvas.c) and what it needs of jsdom.c. */
#ifndef JSCANVAS_H
#define JSCANVAS_H

#include "js.h"

struct node;

void jscanvas_install(struct js *J, struct jsobj *canvas_proto);
void jscanvas_free(struct js *J);                   /* the page is gone: free the bitmaps */
void jscanvas_laid_out(struct js *J);               /* the page was laid out: replaced bitmaps may go */
void jscanvas_resized(struct js *J, struct node *n, jsval wrapper);    /* width or height was set */
void jscanvas_size(struct node *n, int *w, int *h);                    /* from its attributes: 300 x 150 */

/* jsdom.c */
void jsdom_canvas_dirty(struct js *J);              /* paint again (no layout) when the script returns */
struct node *jsdom_node_of(jsval v);

#endif

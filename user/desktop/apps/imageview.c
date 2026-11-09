/*
 * The image viewer: a picture the File Manager opens (PNG, JPEG, GIF, WebP,
 * SVG or ICO), in a window fitted to it as far as the screen allows. The
 * decoding is the image decoder process's (imagejobs.c).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "desktop.h"
#include "imagejobs.h"

struct image_view { struct window *win; struct image *image; int failed; };
static void iv_destroy(struct window *w)
{
    struct image_view *view=w->priv;
    image_jobs_cancel(view);
    image_free(view->image);free(view);
}

static void iv_paint(struct window *w)
{
    struct image_view *view=w->priv;
    struct surface *s=&w->content;
    gfx_fill(s,0,0,s->w,s->h,C_SHADOW);
    if(view->image)gfx_image(s,(s->w-view->image->w)/2,(s->h-view->image->h)/2,view->image);
    else gfx_text(s,&font_sans,12,12,view->failed ? "Could not load image" : "Loading image...",C_WHITE);
    wm_damage_client(w,mkrect(0,0,s->w,s->h));
}
static void iv_ready(void *context, struct image *image, int full_w, int full_h)
{
    (void)full_w;(void)full_h;
    struct image_view *view=context;
    view->image=image;view->failed=!image;
    if(image) {
        int cw=image->w+16,ch=image->h+16;
        int maxw=wm_work_right()-2*BORDER-40,maxh=screen_h-TASKBAR_H-TITLE_H-60;
        if(cw<160)cw=160;
        if(ch<100)ch=100;
        if(cw>maxw)cw=maxw;
        if(ch>maxh)ch=maxh;
        wm_resize_client(view->win,cw,ch);
    }
    iv_paint(view->win);
}

static const struct window_ops iv_ops = {
    .restyle = iv_paint,
    .resize = iv_paint,
    .destroy = iv_destroy,
};

struct window *image_view_open(const char *path)
{
    struct image_view *view=calloc(1,sizeof(*view));
    if(!view)return NULL;
    const char *name=strrchr(path,'/');
    view->win=wm_create(name ? name+1 : path,ICON_IMAGE,320,200,WF_MINIMIZE,&iv_ops,view);
    if(!view->win){free(view);return NULL;}
    struct window *w=view->win;
    if(image_job_path(view,path,0,iv_ready,view,NULL)<0)view->failed=1;
    iv_paint(w);
    return w;
}

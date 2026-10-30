/*
 * The desktop wallpaper: a JPEG or PNG picture shown behind the icons in
 * one of four ways.
 *
 *   Fill     scaled evenly until it covers the screen, the overflow cut off
 *            at the edges, so nothing is stretched out of shape;
 *   Stretch  scaled to the screen's exact size, whatever its shape;
 *   Center   at its own size in the middle, the desktop color around it;
 *   Tile     at its own size, repeated across the screen from the top left.
 *
 * A helper decodes and draws the picture into a screen-sized surface
 * (wallpaint.c draws it). The compositor copies that surface, keeping the
 * old wallpaper visible while a replacement is prepared. Screen changes
 * queue a fresh render.
 *
 * The Kinetik theme has a wallpaper of its own, drawn rather than decoded
 * (WALLPAPER_KINETIK): its color's gradient running from the top left
 * corner, a glow towards the top right and three sweeping curves, as in
 * the Kinetik studies, at whatever size the screen is.
 *
 * The choice is kept for the next start in GRUB's environment block when
 * the system runs from a hard disk (fluxos_wall, fluxos_wallmode).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "desktop.h"
#include "imagejobs.h"

static int wallpaper_owner;
static char applied_path[256];
static int applied_mode = WALL_FILL;
static int kinetik;                     /* the Kinetik theme's own instead */
static struct surface wall;             /* screen-sized; px NULL if none */
static char cur_path[256];
static int cur_mode = WALL_FILL;

static const char *const mode_keys[WALL_MODES] = { "fill", "stretch", "center", "tile" };

/* ---- the desktop's wallpaper ---- */

static uint32_t desk_color = RGB(0x16, 0x46, 0x7D);     /* (Kinetik Blue's middle color) */

uint32_t wallpaper_color(void)
{
    return desk_color;
}

/* Rendering and scaling happen in the decoder process. Keep the current
 * surface visible until its replacement has been transferred completely. */
static void rebuild(void)
{
    if (cur_path[0]) wallpaper_set(cur_path,cur_mode,0);
    else surface_free(&wall);
}

int wallpaper_active(void)
{
    return wall.px != NULL;
}

const char *wallpaper_path(void)
{
    return cur_path;
}

int wallpaper_mode(void)
{
    return cur_mode;
}

void wallpaper_draw(struct surface *s, struct rect clip)
{
    if (wall.px)
        gfx_blit(s, clip.x, clip.y, &wall, clip.x, clip.y, clip.w, clip.h);
    else
        gfx_fill(s, clip.x, clip.y, clip.w, clip.h, desk_color);
}

void wallpaper_resize(void)
{
    if (cur_path[0])
        rebuild();
}

static void save_setting(void)
{
    char dir[128];
    if (!grubenv_boot_dir(dir, sizeof(dir)))
        return;
    char color[8];
    snprintf(color, sizeof(color), "%06X", (unsigned)desk_color);
    const char *keys[3] = { "fluxos_wall", "fluxos_wallmode", "fluxos_wallcolor" };
    const char *vals[3] = { cur_path[0] ? cur_path : "none", mode_keys[cur_mode], color };
    grubenv_update(dir, keys, vals, 3);
}

void wallpaper_set_color(uint32_t rgb, int save)
{
    desk_color = rgb & 0xFFFFFF;
    rebuild();
    if (save)
        save_setting();
    wm_wallpaper_changed();
}

struct wall_request {
    char path[256];
    int mode, save, w, h;
};
static void wallpaper_ready(void *context, struct image *image, int full_w, int full_h)
{
    (void)full_w;(void)full_h;
    struct wall_request *request=context;
    if (!image) {
        snprintf(cur_path,sizeof(cur_path),"%s",applied_path);
        cur_mode=applied_mode;
        kinetik=!strcmp(cur_path,WALLPAPER_KINETIK);
        if (request->save) message_box("Wallpaper",ICON_WARNING,"The wallpaper could not be loaded.");
        else if (!applied_path[0] && strcmp(request->path,WALLPAPER_DEFAULT))
            wallpaper_set(WALLPAPER_DEFAULT,request->mode,0);
        return;
    }
    if (request->w!=screen_w || request->h!=screen_h) {
        image_free(image);
        wallpaper_set(request->path,request->mode,request->save);
        return;
    }
    surface_free(&wall);
    wall.px=image->px;wall.w=image->w;wall.h=image->h;wall.stride=image->w;
    wall.clip=mkrect(0,0,wall.w,wall.h);
    image->px=NULL;image_free(image);
    snprintf(cur_path,sizeof(cur_path),"%s",request->path);
    snprintf(applied_path,sizeof(applied_path),"%s",request->path);
    applied_mode=cur_mode=request->mode;
    kinetik=!strcmp(cur_path,WALLPAPER_KINETIK);
    if (request->save) save_setting();
    wm_wallpaper_changed();
}
int wallpaper_set(const char *path, int mode, int save)
{
    struct wall_request *request=calloc(1,sizeof(*request));
    if (!request) return -1;
    snprintf(request->path,sizeof(request->path),"%s",path ? path : "");
    request->mode=mode>=0 && mode<WALL_MODES ? mode : WALL_FILL;
    request->save=save;request->w=screen_w;request->h=screen_h;
    image_jobs_cancel(&wallpaper_owner);
    if (!request->path[0]) {
        surface_free(&wall);cur_path[0]=applied_path[0]=0;kinetik=0;
        applied_mode=cur_mode=request->mode;
        if (save) save_setting();
        free(request);wm_wallpaper_changed();return 0;
    }
    if (image_job_wallpaper(&wallpaper_owner,request->path,request->mode,desk_color,screen_w,screen_h,
                            wallpaper_ready,request,free)<0) {free(request);return -1;}
    snprintf(cur_path,sizeof(cur_path),"%s",request->path);
    cur_mode=request->mode;kinetik=!strcmp(cur_path,WALLPAPER_KINETIK);
    return 0;
}

void wallpaper_theme_changed(int old_style)
{
    (void)old_style;
    if (kinetik && KINETIK)
        wallpaper_set(WALLPAPER_KINETIK, cur_mode, 0);  /* the drawn one, in the new color */
    else if (kinetik && wallpaper_set(WALLPAPER_DEFAULT, cur_mode, 1) < 0)
        wallpaper_set("", cur_mode, 1);                 /* Classic has no drawn wallpaper of its own */
}

/* At startup: the saved choice, or the bamboo photograph, filled. */
void wallpaper_init(void)
{
    char dir[128], path[256], name[16];
    const char *use = WALLPAPER_DEFAULT;
    int mode = WALL_FILL;
    if (grubenv_boot_dir(dir, sizeof(dir))) {
        if (grubenv_get(dir, "fluxos_wall", path, sizeof(path)) && path[0])
            use = !strcmp(path, "none") ? "" : path;
        uint32_t rgb;
        if (grubenv_get(dir, "fluxos_wallcolor", name, sizeof(name)) && parse_hex_color(name, &rgb) == 0)
            desk_color = rgb;
        if (grubenv_get(dir, "fluxos_wallmode", name, sizeof(name)))
            for (int i = 0; i < WALL_MODES; i++)
                if (!strcmp(name, mode_keys[i]))
                    mode = i;
    }
    if (wallpaper_set(use, mode, 0) < 0 && strcmp(use, WALLPAPER_DEFAULT) != 0)
        wallpaper_set(WALLPAPER_DEFAULT, mode, 0);      /* the saved file is gone */
}

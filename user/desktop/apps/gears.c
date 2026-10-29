/*
 * 3D Gears: three meshing gears turning under a light, after Brian Paul's
 * gears demo, the classic first OpenGL program. It draws with the GL
 * library: on an ATI Radeon HD 4000, Haswell's Intel HD Graphics or a 3dfx
 * Voodoo card through its 3D engine, anywhere else with the software
 * renderer. The status bar says
 * which, and how many frames a second it manages.
 *
 * Drag with the mouse or use the arrow keys to turn the view; Space
 * pauses, Home puts the view back, S moves on to the next renderer there
 * is (Radeon, Intel, Voodoo, software), to compare them, T puts a checked
 * texture on the gears or takes it off, W draws them as wireframes
 * (glPolygonMode) and B makes them see-through (blending).
 *
 * This file is built with the FPU allowed (USER_FP_CFLAGS).
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <GL/fluxgl.h>
#include "desktop.h"

#define GEARS_W   320
#define GEARS_H   300
#define STATUS_H  20
#define FRAME_MS  16                    /* at most about 60 frames a second */
#define TURN_RATE 70.0f                 /* degrees a second */

struct gears {
    struct window *win;
    FGLcontext *ctx;
    int ctx_w, ctx_h;                   /* the context's picture size */
    GLuint gear1, gear2, gear3;
    GLuint texture;
    int textured, wireframe, see_through;
    float angle, rot_x, rot_y;
    int paused;
    unsigned int last_ms;               /* when the angle last advanced */
    unsigned int fps_since;
    int frames;
    char status[128];
    int dragging, drag_x, drag_y;
    struct gears *next;
};

static struct gears *all_gears;
static unsigned int next_frame_ms;

/* A vertex with texture coordinates from its x and y: the texture lies flat
 * across the gear's faces (and is drawn out along its sides). */
static void vtx(float x, float y, float z)
{
    glTexCoord2f(x * 0.125f + 0.5f, y * 0.125f + 0.5f);
    glVertex3f(x, y, z);
}

/* One gear: a toothed ring of the given radii and width around the z axis. */
static void gear(float inner, float outer, float width, int teeth, float tooth_depth)
{
    float r0 = inner, r1 = outer - tooth_depth / 2, r2 = outer + tooth_depth / 2;
    float da = 2 * (float)M_PI / teeth / 4, w2 = width * 0.5f;

    glShadeModel(GL_FLAT);
    glNormal3f(0, 0, 1);

    /* front face */
    glBegin(GL_QUAD_STRIP);
    for (int i = 0; i <= teeth; i++) {
        float a = i * 2 * (float)M_PI / teeth;
        vtx(r0 * cosf(a), r0 * sinf(a), w2);
        vtx(r1 * cosf(a), r1 * sinf(a), w2);
        if (i < teeth) {
            vtx(r0 * cosf(a), r0 * sinf(a), w2);
            vtx(r1 * cosf(a + 3 * da), r1 * sinf(a + 3 * da), w2);
        }
    }
    glEnd();

    /* front sides of the teeth */
    glBegin(GL_QUADS);
    for (int i = 0; i < teeth; i++) {
        float a = i * 2 * (float)M_PI / teeth;
        vtx(r1 * cosf(a), r1 * sinf(a), w2);
        vtx(r2 * cosf(a + da), r2 * sinf(a + da), w2);
        vtx(r2 * cosf(a + 2 * da), r2 * sinf(a + 2 * da), w2);
        vtx(r1 * cosf(a + 3 * da), r1 * sinf(a + 3 * da), w2);
    }
    glEnd();

    glNormal3f(0, 0, -1);

    /* back face */
    glBegin(GL_QUAD_STRIP);
    for (int i = 0; i <= teeth; i++) {
        float a = i * 2 * (float)M_PI / teeth;
        vtx(r1 * cosf(a), r1 * sinf(a), -w2);
        vtx(r0 * cosf(a), r0 * sinf(a), -w2);
        if (i < teeth) {
            vtx(r1 * cosf(a + 3 * da), r1 * sinf(a + 3 * da), -w2);
            vtx(r0 * cosf(a), r0 * sinf(a), -w2);
        }
    }
    glEnd();

    /* back sides of the teeth */
    glBegin(GL_QUADS);
    for (int i = 0; i < teeth; i++) {
        float a = i * 2 * (float)M_PI / teeth;
        vtx(r1 * cosf(a + 3 * da), r1 * sinf(a + 3 * da), -w2);
        vtx(r2 * cosf(a + 2 * da), r2 * sinf(a + 2 * da), -w2);
        vtx(r2 * cosf(a + da), r2 * sinf(a + da), -w2);
        vtx(r1 * cosf(a), r1 * sinf(a), -w2);
    }
    glEnd();

    /* outward faces of the teeth */
    glBegin(GL_QUAD_STRIP);
    for (int i = 0; i < teeth; i++) {
        float a = i * 2 * (float)M_PI / teeth;
        vtx(r1 * cosf(a), r1 * sinf(a), w2);
        vtx(r1 * cosf(a), r1 * sinf(a), -w2);
        float u = r2 * cosf(a + da) - r1 * cosf(a), v = r2 * sinf(a + da) - r1 * sinf(a);
        float len = sqrtf(u * u + v * v);
        glNormal3f(v / len, -u / len, 0);
        vtx(r2 * cosf(a + da), r2 * sinf(a + da), w2);
        vtx(r2 * cosf(a + da), r2 * sinf(a + da), -w2);
        glNormal3f(cosf(a), sinf(a), 0);
        vtx(r2 * cosf(a + 2 * da), r2 * sinf(a + 2 * da), w2);
        vtx(r2 * cosf(a + 2 * da), r2 * sinf(a + 2 * da), -w2);
        u = r1 * cosf(a + 3 * da) - r2 * cosf(a + 2 * da);
        v = r1 * sinf(a + 3 * da) - r2 * sinf(a + 2 * da);
        glNormal3f(v, -u, 0);
        vtx(r1 * cosf(a + 3 * da), r1 * sinf(a + 3 * da), w2);
        vtx(r1 * cosf(a + 3 * da), r1 * sinf(a + 3 * da), -w2);
        glNormal3f(cosf(a), sinf(a), 0);
    }
    vtx(r1, 0, w2);
    vtx(r1, 0, -w2);
    glEnd();

    glShadeModel(GL_SMOOTH);

    /* the inside of the hole */
    glBegin(GL_QUAD_STRIP);
    for (int i = 0; i <= teeth; i++) {
        float a = i * 2 * (float)M_PI / teeth;
        glNormal3f(-cosf(a), -sinf(a), 0);
        vtx(r0 * cosf(a), r0 * sinf(a), -w2);
        vtx(r0 * cosf(a), r0 * sinf(a), w2);
    }
    glEnd();
}

static GLuint make_gear(const GLfloat *color, float inner, float outer, float width, int teeth)
{
    GLuint list = glGenLists(1);
    glNewList(list, GL_COMPILE);
    glMaterialfv(GL_FRONT, GL_AMBIENT_AND_DIFFUSE, color);
    gear(inner, outer, width, teeth, 0.7f);
    glEndList();
    return list;
}

/* The W and B switches: wireframes, and see-through gears (their alpha,
 * which only blending shows, is 0.6) drawn without the depth test. */
static void apply_modes(struct gears *g)
{
    glPolygonMode(GL_FRONT_AND_BACK, g->wireframe ? GL_LINE : GL_FILL);
    if (g->see_through) {
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        glDisable(GL_DEPTH_TEST);
    } else {
        glDisable(GL_BLEND);
        glEnable(GL_DEPTH_TEST);
    }
}

static void scene_init(struct gears *g)
{
    static const GLfloat pos[4] = { 5, 5, 10, 0 };
    static const GLfloat red[4] = { 0.8f, 0.1f, 0, 0.6f };
    static const GLfloat green[4] = { 0, 0.8f, 0.2f, 0.6f };
    static const GLfloat blue[4] = { 0.2f, 0.2f, 1, 0.6f };

    glLightfv(GL_LIGHT0, GL_POSITION, pos);
    glEnable(GL_CULL_FACE);
    glEnable(GL_LIGHTING);
    glEnable(GL_LIGHT0);
    glEnable(GL_DEPTH_TEST);
    g->gear1 = make_gear(red, 1, 4, 1, 20);
    g->gear2 = make_gear(green, 0.5f, 2, 2, 10);
    g->gear3 = make_gear(blue, 1.3f, 2, 0.5f, 10);
    glEnable(GL_NORMALIZE);

    /* A 64 x 64 check of light and darker squares with a thin dark line
     * between them, which the gears' lit colors modulate. */
    static uint8_t texels[64 * 64 * 3];
    for (int y = 0; y < 64; y++) {
        for (int x = 0; x < 64; x++) {
            uint8_t v = ((x / 8) ^ (y / 8)) & 1 ? 255 : 150;
            if (x % 8 == 0 || y % 8 == 0)
                v = 60;
            uint8_t *p = texels + (y * 64 + x) * 3;
            p[0] = p[1] = p[2] = v;
        }
    }
    glGenTextures(1, &g->texture);
    glBindTexture(GL_TEXTURE_2D, g->texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, 64, 64, 0, GL_RGB, GL_UNSIGNED_BYTE, texels);
    if (g->textured)
        glEnable(GL_TEXTURE_2D);
    apply_modes(g);
}

static void reshape(int w, int h)
{
    float aspect = (float)h / (float)w;
    glViewport(0, 0, w, h);
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    if (aspect >= 1)
        glFrustum(-1, 1, -aspect, aspect, 5, 60);
    else
        glFrustum(-1 / aspect, 1 / aspect, -1, 1, 5, 60);
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
    glTranslatef(0, 0, -40);
}

static void scene_draw(struct gears *g)
{
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glPushMatrix();
    glRotatef(g->rot_x, 1, 0, 0);
    glRotatef(g->rot_y, 0, 1, 0);

    glPushMatrix();
    glTranslatef(-3, -2, 0);
    glRotatef(g->angle, 0, 0, 1);
    glCallList(g->gear1);
    glPopMatrix();

    glPushMatrix();
    glTranslatef(3.1f, -2, 0);
    glRotatef(-2 * g->angle - 9, 0, 0, 1);
    glCallList(g->gear2);
    glPopMatrix();

    glPushMatrix();
    glTranslatef(-3.1f, 4.2f, 0);
    glRotatef(-2 * g->angle - 25, 0, 0, 1);
    glCallList(g->gear3);
    glPopMatrix();

    glPopMatrix();
}

static int view_h(struct gears *g)
{
    int h = g->win->content.h - STATUS_H;
    return h > 1 ? h : 1;
}

static void draw_status(struct gears *g)
{
    struct surface *s = &g->win->content;
    struct rect r = mkrect(0, s->h - STATUS_H, s->w, STATUS_H);
    gfx_fill(s, r.x, r.y, r.w, r.h, C_FACE);
    struct rect well = mkrect(2, r.y + 2, r.w - 4, r.h - 3);
    gfx_bevel(s, well, BEVEL_THIN_SUNKEN);
    char fit[64];
    gfx_ellipsize(&font_sans, g->status, well.w - 8, fit, sizeof(fit));
    gfx_text(s, &font_sans, well.x + 4, well.y + (well.h - font_sans.height) / 2, fit, C_BLACK);
}

static void render(struct gears *g)
{
    struct surface *s = &g->win->content;
    if (g->ctx_w != s->w || g->ctx_h != view_h(g))
        return;                         /* the context could not follow a resize */
    fglMakeCurrent(g->ctx);
    scene_draw(g);
    fglSwapBuffers(g->ctx, s->px, s->stride);
    wm_damage_client(g->win, mkrect(0, 0, s->w, view_h(g)));
}

static void update_status(struct gears *g, unsigned int now)
{
    unsigned int span = now - g->fps_since;
    const char *who = fglIsAccelerated(g->ctx) ? (const char *)glGetString(GL_RENDERER) : "Software";
    char modes[48];
    snprintf(modes, sizeof(modes), "%s%s%s", g->textured ? ", textured" : "",
             g->wireframe ? ", wireframe" : "", g->see_through ? ", see-through" : "");
    if (g->paused) {
        snprintf(g->status, sizeof(g->status), "%s%s - paused (Space resumes)", who, modes);
    } else if (span > 0) {
        unsigned int tenths = (unsigned int)g->frames * 10000u / span;
        snprintf(g->status, sizeof(g->status), "%s%s - %u.%u frames per second", who, modes, tenths / 10,
                 tenths % 10);
    }
    draw_status(g);
    struct surface *s = &g->win->content;
    wm_damage_client(g->win, mkrect(0, s->h - STATUS_H, s->w, STATUS_H));
}

/* Called from the event loop: draw a frame in every running gears window. */
void gears_frame(void)
{
    unsigned int now = now_ms();
    if (!all_gears || (int)(now - next_frame_ms) < 0)
        return;
    next_frame_ms = now + FRAME_MS;
    for (struct gears *g = all_gears; g; g = g->next) {
        if (g->paused || g->win->minimized) {
            g->last_ms = now;
            continue;
        }
        g->angle += TURN_RATE * (float)(now - g->last_ms) / 1000.0f;
        if (g->angle >= 360)
            g->angle -= 360;
        g->last_ms = now;
        render(g);
        g->frames++;
        if (now - g->fps_since >= 1000) {
            update_status(g, now);
            g->frames = 0;
            g->fps_since = now;
        }
    }
}

/* Milliseconds until the next frame is due, or -1 with nothing to animate. */
int gears_timeout(void)
{
    int any = 0;
    for (struct gears *g = all_gears; g; g = g->next)
        if (!g->paused && !g->win->minimized)
            any = 1;
    if (!any)
        return -1;
    int left = (int)(next_frame_ms - now_ms());
    return left > 0 ? left : 0;
}

static void redraw(struct gears *g)
{
    render(g);
    update_status(g, now_ms());
}

/* Start again with a new context on the next renderer that works, in the
 * order Radeon, Intel, Voodoo, software. */
static void switch_renderer(struct gears *g)
{
    static const struct {
        int driver;
        unsigned int flags;
    } order[4] = {
        { FGL_DRIVER_RADEON, FGL_NO_INTEL | FGL_NO_TDFX },
        { FGL_DRIVER_INTEL, FGL_NO_RADEON | FGL_NO_TDFX },
        { FGL_DRIVER_TDFX, FGL_NO_RADEON | FGL_NO_INTEL },
        { FGL_DRIVER_SOFTWARE, FGL_SOFTWARE },
    };
    int cur = 0;
    while (cur < 3 && order[cur].driver != fglGetDriver(g->ctx))
        cur++;
    FGLcontext *ctx = NULL;
    for (int step = 1; step < 4 && !ctx; step++) {
        int k = (cur + step) % 4;
        ctx = fglCreateContext(g->ctx_w, g->ctx_h, order[k].flags);
        if (ctx && fglGetDriver(ctx) != order[k].driver) {
            fglDestroyContext(ctx);     /* that card is not there */
            ctx = NULL;
        }
    }
    if (!ctx)
        return;
    fglDestroyContext(g->ctx);
    g->ctx = ctx;
    fglMakeCurrent(ctx);
    scene_init(g);
    reshape(g->ctx_w, g->ctx_h);
    g->frames = 0;
    g->fps_since = now_ms();
}

static void gears_key(struct window *w, const struct input_event *ev)
{
    struct gears *g = w->priv;
    if (!(ev->flags & IEV_PRESSED))
        return;
    switch (ev->code) {
    case KEY_UP: g->rot_x += 5; break;
    case KEY_DOWN: g->rot_x -= 5; break;
    case KEY_LEFT: g->rot_y += 5; break;
    case KEY_RIGHT: g->rot_y -= 5; break;
    case KEY_HOME: g->rot_x = 20; g->rot_y = 30; break;
    case 0x1F:                          /* S */
        switch_renderer(g);
        break;
    case 0x14:                          /* T */
        g->textured = !g->textured;
        fglMakeCurrent(g->ctx);
        if (g->textured)
            glEnable(GL_TEXTURE_2D);
        else
            glDisable(GL_TEXTURE_2D);
        break;
    case 0x11:                          /* W */
        g->wireframe = !g->wireframe;
        fglMakeCurrent(g->ctx);
        apply_modes(g);
        break;
    case 0x30:                          /* B */
        g->see_through = !g->see_through;
        fglMakeCurrent(g->ctx);
        apply_modes(g);
        break;
    case KEY_SPACE:
        g->paused = !g->paused;
        g->frames = 0;
        g->fps_since = g->last_ms = now_ms();
        break;
    default:
        return;
    }
    redraw(g);
}

static void gears_mouse(struct window *w, int x, int y, int buttons, int changed)
{
    struct gears *g = w->priv;
    if ((changed & MOUSE_LEFT) && (buttons & MOUSE_LEFT)) {
        g->dragging = 1;
    } else if (changed & MOUSE_LEFT) {
        g->dragging = 0;
    } else if (g->dragging) {
        g->rot_y += (float)(x - g->drag_x) * 0.5f;
        g->rot_x += (float)(y - g->drag_y) * 0.5f;
        if (g->paused)
            redraw(g);
    }
    g->drag_x = x;
    g->drag_y = y;
}

static void gears_resize(struct window *w)
{
    struct gears *g = w->priv;
    fglMakeCurrent(g->ctx);
    if (fglResize(g->ctx, w->content.w, view_h(g)) == 0) {
        g->ctx_w = w->content.w;
        g->ctx_h = view_h(g);
        reshape(g->ctx_w, g->ctx_h);
    }
    redraw(g);
}

static void gears_destroy(struct window *w)
{
    struct gears *g = w->priv;
    for (struct gears **pp = &all_gears; *pp; pp = &(*pp)->next) {
        if (*pp == g) {
            *pp = g->next;
            break;
        }
    }
    fglDestroyContext(g->ctx);
    free(g);
}

static const struct window_ops gears_ops = {
    .key = gears_key,
    .mouse = gears_mouse,
    .resize = gears_resize,
    .destroy = gears_destroy,
};

struct window *gears_open(void)
{
    struct gears *g = calloc(1, sizeof(*g));
    if (!g)
        return NULL;
    g->ctx = fglCreateContext(GEARS_W, GEARS_H - STATUS_H, 0);
    if (!g->ctx) {
        free(g);
        message_box("3D Gears", ICON_WARNING, "There is not enough memory for 3D Gears.");
        return NULL;
    }
    g->win = wm_create("3D Gears", ICON_GEARS, GEARS_W, GEARS_H, WF_MINIMIZE | WF_MAXIMIZE, &gears_ops, g);
    if (!g->win) {
        fglDestroyContext(g->ctx);
        free(g);
        return NULL;
    }
    wm_set_min_size(g->win, 120, 100);
    g->rot_x = 20;
    g->rot_y = 30;
    g->ctx_w = GEARS_W;
    g->ctx_h = GEARS_H - STATUS_H;
    fglMakeCurrent(g->ctx);
    scene_init(g);
    reshape(GEARS_W, GEARS_H - STATUS_H);
    g->last_ms = g->fps_since = now_ms();
    snprintf(g->status, sizeof(g->status), "%s", fglIsAccelerated(g->ctx) ?
             (const char *)glGetString(GL_RENDERER) : "Software");
    g->next = all_gears;
    all_gears = g;
    render(g);
    draw_status(g);
    wm_damage_client(g->win, mkrect(0, 0, GEARS_W, GEARS_H));
    return g->win;
}

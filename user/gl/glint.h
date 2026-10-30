/*
 * The GL library's insides. gl.c keeps the state, prim.c turns vertices
 * into lit, clipped polygons in window coordinates, array.c feeds it from
 * vertex arrays, list.c records and replays display lists, tex.c keeps
 * textures, pixels.c reads the picture back, and a backend draws the
 * polygons: radeon.c on a
 * Radeon HD 4000's 3D engine, intel.c on Haswell HD Graphics', tdfx.c on a
 * Voodoo card's, soft.c in memory.
 *
 * There is no destination alpha anywhere: GL_DST_ALPHA blends as 1 and
 * GL_ALPHA_BITS is 0.
 */
#ifndef GLINT_H
#define GLINT_H

#include <stdint.h>
#include <xmmintrin.h>
#include <GL/fluxgl.h>
#include <GL/gl.h>

#define MAX_LIGHTS       8
#define MODELVIEW_DEPTH  32
#define PROJECTION_DEPTH 4
#define TEXTURE_DEPTH    2
#define MAX_VIEWPORT     2048
#define MAX_TEXTURE      2048           /* texels on a side */

/* Four-float vectors and matrices are 16-byte aligned for SSE. */
#define VEC4 __attribute__((aligned(16)))

/* A vertex after transformation: clip coordinates, its color, and its
 * texture coordinates (through the texture matrix). */
struct cvert {
    float clip[4] VEC4;
    float color[4] VEC4;
    float tex[4] VEC4;
};

/* A vertex as the backends draw it: window pixels with the origin at the
 * top left, depth from 0 to 1, color components from 0 to 1, texture
 * coordinates (divided by q), and the clip w, for interpolating the
 * texture coordinates with perspective. */
struct wvert {
    float x, y, z;
    float r, g, b, a;
    float s, t, w;
};

/* A texture object. Texels are kept as 0xAABBGGRR (bytes R, G, B, A),
 * whatever the format they came in; an alpha or luminance texture is
 * widened to that. */
struct gltex {
    GLuint name;
    int w, h;
    uint32_t *texels;
    GLenum format;                      /* the base internal format: GL_RGB, GL_RGBA, ... */
    GLenum min_filter, mag_filter, wrap_s, wrap_t;
    float border[4];
    /* the backend's copy */
    int hw_stale;                       /* texels changed since it was made */
    uint32_t hw_off, hw_size, hw_pitch;
};

struct light {
    float ambient[4] VEC4, diffuse[4] VEC4, specular[4] VEC4;
    float position[4] VEC4;     /* eye coordinates */
    float spot_dir[3];          /* eye coordinates */
    float spot_exp, spot_cutoff, spot_cos;
    float att[3];               /* constant, linear, quadratic */
    float dir[3], half[3];      /* directional lights: unit L and half vector */
};

struct material {
    float ambient[4] VEC4, diffuse[4] VEC4, specular[4] VEC4, emission[4] VEC4;
    float shininess;
};

struct dlist {
    float *ops;                 /* opcode, argument count, arguments... */
    int n, cap;
};

/* A client vertex array (glVertexPointer and the like). */
enum { ARRAY_VERTEX, ARRAY_NORMAL, ARRAY_COLOR, ARRAY_TEXCOORD, NARRAYS };
struct varray {
    int enabled;
    int size;                   /* components a vertex */
    GLenum type;
    int stride;                 /* as given: 0 means tightly packed */
    const void *ptr;
};

struct fgl_context;

struct backend {
    const char *name;
    /* Allocate buffers for c->width x c->height; returns -1 if it cannot. */
    int (*resize)(struct fgl_context *c);
    void (*destroy)(struct fgl_context *c);
    /* Clear the buffers in mask within the rectangle [x0, x1) x [y0, y1). */
    void (*clear)(struct fgl_context *c, GLbitfield mask, int x0, int y0, int x1, int y1);
    /* Draw a convex polygon of n >= 3 vertices. */
    void (*polygon)(struct fgl_context *c, const struct wvert *v, int n);
    void (*finish)(struct fgl_context *c);
    void (*read)(struct fgl_context *c, uint32_t *dst, int stride);
    /* Depths from 0 to 1 of the rectangle [x, x + w) x [y, y + h), top-left
     * origin, a row at a time (optional: without it depth cannot be read). */
    void (*read_depth)(struct fgl_context *c, int x, int y, int w, int h, float *dst);
    /* A texture is going away, or its texels changed: drop the backend's
     * copy (optional). */
    void (*tex_free)(struct fgl_context *c, struct gltex *t);
};

struct fgl_context {
    const struct backend *be;
    void *bpriv;                /* the backend's state */
    int width, height;
    char renderer[64];
    GLenum error;
    int hw_dirty;               /* render state the hardware holds changed */

    /* transformation */
    GLenum matrix_mode;
    float modelview[MODELVIEW_DEPTH][16] VEC4;
    float projection[PROJECTION_DEPTH][16] VEC4;
    float texture[TEXTURE_DEPTH][16] VEC4;
    int mv_depth, proj_depth, tex_depth;
    float normal_matrix[9];     /* inverse transpose of the modelview's 3x3 */
    int normal_dirty;
    int vp_x, vp_y, vp_w, vp_h;
    float depth_near, depth_far;
    int scissor_box[4];

    /* switches and render state */
    int cull, depth_test, lighting, normalize, color_material, scissor_test, dither;
    GLenum cull_face, front_face, shade_model, depth_func;
    int depth_mask;
    float clear_color[4], clear_depth;
    float point_size, line_width;
    GLenum polygon_mode[2];             /* front faces, back faces */
    int offset_fill, offset_line, offset_point;
    float offset_factor, offset_units;

    /* fragments */
    int blend, alpha_test;
    GLenum blend_src, blend_dst, alpha_func;
    float alpha_ref;
    int color_mask[4];
    GLenum draw_buffer, read_buffer;
    int pack_align, pack_row_length, pack_skip_rows, pack_skip_pixels;

    /* lighting */
    struct light lights[MAX_LIGHTS];
    int light_on[MAX_LIGHTS];
    struct material mat;
    float model_ambient[4] VEC4;
    GLenum cm_face, cm_mode;

    /* textures */
    int texture_2d;                     /* GL_TEXTURE_2D enabled */
    struct gltex **textures;            /* by name; textures[0] is the default texture */
    GLuint ntextures;
    GLuint bound;                       /* GL_TEXTURE_BINDING_2D */
    GLenum tex_env_mode;
    float tex_env_color[4];
    int unpack_align, unpack_row_length, unpack_skip_rows, unpack_skip_pixels;

    /* current attributes */
    float texcoord[4] VEC4;
    float color[4] VEC4, normal[3];
    float eye_normal[3];
    int eye_normal_valid;

    /* glBegin ... glEnd */
    GLenum prim;
    int in_begin;
    struct cvert *verts;
    int nverts, vcap;

    /* display lists */
    struct dlist **lists;
    GLuint nlists;              /* lists[0..nlists) exist as slots; 0 is unused */
    struct dlist *compiling;
    GLuint compiling_id;
    GLenum list_mode;
    int executing;              /* nesting of glCallList */
    GLuint list_base;

    /* vertex arrays */
    struct varray arrays[NARRAYS];
};

extern struct fgl_context *gl_ctx;

void gl_error(struct fgl_context *c, GLenum e);
void gl_mat_identity(float *m);
void gl_mat_mul(float *out, const float *a, const float *b);
float *gl_top_matrix(struct fgl_context *c);
void gl_matrix_changed(struct fgl_context *c);
/* The drawing rectangle: the buffer, cut down by the scissor box. */
void gl_clip_rect(struct fgl_context *c, int *x0, int *y0, int *x1, int *y1);
int gl_depth_bits(struct fgl_context *c);
/* Before drawing: a backend that cannot draw with today's state gives way
 * to software, keeping the picture. */
void gl_check_backend(struct fgl_context *c);

/* The color channels glColorMask lets through, as a 0xRRGGBB mask. */
static inline uint32_t gl_rgb_write_mask(const struct fgl_context *c)
{
    return (c->color_mask[0] ? 0xFF0000u : 0) | (c->color_mask[1] ? 0xFF00u : 0) |
           (c->color_mask[2] ? 0xFFu : 0);
}

/* tex.c */
void gl_tex_init(struct fgl_context *c);
void gl_tex_free_all(struct fgl_context *c);
/* The texture to draw with: GL_TEXTURE_2D is on and the bound texture has texels. */
struct gltex *gl_active_texture(struct fgl_context *c);

/* prim.c */
void gl_prim_init(struct fgl_context *c);
void gl_update_normal_matrix(struct fgl_context *c);

/* array.c: the client array a glEnableClientState capability names, or NULL. */
struct varray *gl_client_array(struct fgl_context *c, GLenum cap);
/* glGet of vertex array state; returns the number of values, 0 if pname is not one. */
int gl_array_get(struct fgl_context *c, GLenum pname, double *v);

/* list.c: when a list is being compiled, record the call. Returns 1 if the
 * call must not run now (GL_COMPILE). */
enum {
    OP_BEGIN = 1, OP_END, OP_VERTEX, OP_NORMAL, OP_COLOR, OP_MATERIAL, OP_LIGHT,
    OP_LIGHT_MODEL, OP_COLOR_MATERIAL, OP_SHADE_MODEL, OP_ENABLE, OP_DISABLE,
    OP_MATRIX_MODE, OP_LOAD_IDENTITY, OP_LOAD_MATRIX, OP_MULT_MATRIX, OP_PUSH_MATRIX,
    OP_POP_MATRIX, OP_TRANSLATE, OP_ROTATE, OP_SCALE, OP_FRUSTUM, OP_ORTHO, OP_CALL_LIST,
    OP_CULL_FACE, OP_FRONT_FACE, OP_DEPTH_FUNC, OP_DEPTH_MASK, OP_DEPTH_RANGE, OP_CLEAR,
    OP_CLEAR_COLOR, OP_CLEAR_DEPTH, OP_VIEWPORT, OP_SCISSOR, OP_POINT_SIZE, OP_LINE_WIDTH,
    OP_TEXCOORD, OP_BIND_TEXTURE, OP_TEX_ENV, OP_TEX_PARAMETER, OP_BLEND_FUNC, OP_ALPHA_FUNC,
    OP_COLOR_MASK, OP_POLYGON_MODE, OP_POLYGON_OFFSET, OP_LIST_BASE, OP_CALL_LISTS,
};
int gl_record(struct fgl_context *c, int op, int nargs, const float *args);
void gl_free_lists(struct fgl_context *c);

#define RECORD(c, op, n, ...)                                                   \
    do {                                                                        \
        if ((c)->compiling && !(c)->executing &&                                \
            gl_record((c), (op), (n), (const float[]){ __VA_ARGS__ }))          \
            return;                                                             \
    } while (0)
#define RECORD0(c, op)                                                          \
    do {                                                                        \
        if ((c)->compiling && !(c)->executing && gl_record((c), (op), 0, NULL)) \
            return;                                                             \
    } while (0)

/* backends */
extern const struct backend gl_soft_backend;
extern const struct backend gl_tdfx_backend;
extern const struct backend gl_radeon_backend;
extern const struct backend gl_intel_backend;
int gl_tdfx_attach(struct fgl_context *c);     /* 0: the hardware took the context */
int gl_tdfx_check(struct fgl_context *c);      /* 0: its video memory is still free */
int gl_radeon_attach(struct fgl_context *c);
int gl_radeon_check(struct fgl_context *c);    /* 0: the engine still runs */
int gl_intel_attach(struct fgl_context *c);
int gl_intel_check(struct fgl_context *c);     /* 0: the engine still runs */
int gl_tdfx_can_draw(struct fgl_context *c);   /* 1: the Voodoo can draw with today's state */
/* Draw in memory from now on, starting from the picture as it is. */
int gl_use_software(struct fgl_context *c, int keep_picture);
void gl_soft_load(struct fgl_context *c, const uint32_t *picture);

/* Round to the nearest integer (CVTSS2SI, in MXCSR's default rounding). */
static inline int gl_iround(float f)
{
    return _mm_cvt_ss2si(_mm_set_ss(f));
}

static inline int gl_iceil(float f)
{
    int i = gl_iround(f);
    return (float)i < f ? i + 1 : i;
}

static inline float gl_clampf(float f)
{
    return f < 0 ? 0 : f > 1 ? 1 : f;
}

#endif

/*
 * The GL library's insides. gl.c keeps the state, prim.c turns vertices
 * into lit, clipped polygons in window coordinates, array.c feeds it from
 * vertex arrays, list.c records and replays display lists, tex.c keeps
 * textures, pixels.c reads the picture back and draws pixels and bitmaps
 * at the raster position, and a backend draws the polygons: radeon.c on a
 * Radeon HD 4000's 3D engine, intel.c on Haswell HD Graphics', tdfx.c on a
 * Voodoo card's, soft.c in memory.
 *
 * There is no destination alpha anywhere: GL_DST_ALPHA blends as 1 and
 * GL_ALPHA_BITS is 0. Only the software backend has a stencil buffer, a
 * second texture unit and fog after texturing; gl_check_backend moves a
 * context that needs them there.
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
#define MAX_LEVELS       12             /* mipmap levels: 2048 down to 1 */
#define MAX_UNITS        2              /* texture units */
#define MAX_CLIP_PLANES  6
#define ATTRIB_DEPTH     16             /* glPushAttrib's and glPushClientAttrib's stacks */

/* Four-float vectors and matrices are 16-byte aligned for SSE. */
#define VEC4 __attribute__((aligned(16)))

/* A vertex after transformation: clip coordinates, its color, its texture
 * coordinates for each unit (through the unit's texture matrix), and aux:
 * its distances to the user clip planes, then its fog factor. */
#define AUX_FOG 6
struct cvert {
    float clip[4] VEC4;
    float color[4] VEC4;
    float tex[MAX_UNITS][4] VEC4;
    float aux[8] VEC4;
};

/* A vertex as the backends draw it: window pixels with the origin at the
 * top left, depth from 0 to 1, color components from 0 to 1, texture
 * coordinates (divided by q), and the clip w, for interpolating the
 * texture coordinates with perspective; then the second unit's texture
 * coordinates and the fog factor (1: none), which only the software
 * backend reads. */
struct wvert {
    float x, y, z;
    float r, g, b, a;
    float s, t, w;
    float s1, t1;
    float f;
};

/* A texture object. Texels are kept as 0xAABBGGRR (bytes R, G, B, A),
 * whatever the format they came in; an alpha or luminance texture is
 * widened to that. w, h and texels are level 0, which is all the hardware
 * backends draw with; mip[i] is level i, for i from 1. */
struct texlevel {
    int w, h;
    uint32_t *texels;
};

struct gltex {
    GLuint name;
    int w, h;
    uint32_t *texels;
    struct texlevel mip[MAX_LEVELS];
    int nlevels;                        /* levels 0 to nlevels - 1 make a mipmap (at least 1) */
    GLenum format;                      /* the base internal format: GL_RGB, GL_RGBA, ... */
    GLint internal;                     /* the internal format as the program gave it */
    GLenum min_filter, mag_filter, wrap_s, wrap_t;
    float border[4];
    float priority;
    /* the backend's copy */
    int hw_stale;                       /* texels changed since it was made */
    uint32_t hw_off, hw_size, hw_pitch;
};

/* A texture unit: what GL_ARB_multitexture has one of each of. */
struct texunit {
    int enabled;                        /* GL_TEXTURE_2D */
    GLuint bound;                       /* GL_TEXTURE_BINDING_2D */
    GLenum env_mode;
    float env_color[4];
    float matrix[TEXTURE_DEPTH][16] VEC4;
    int depth;
    float coord[4] VEC4;                /* the current texture coordinates */
    int gen_on[4];                      /* GL_TEXTURE_GEN_S, T, R and Q */
    GLenum gen_mode[4];
    float gen_object[4][4];
    float gen_eye[4][4];                /* in eye coordinates */
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

/* A client vertex array (glVertexPointer and the like); a texture
 * coordinate array for each unit. */
enum { ARRAY_VERTEX, ARRAY_NORMAL, ARRAY_COLOR, ARRAY_TEXCOORD, NARRAYS = ARRAY_TEXCOORD + MAX_UNITS };
struct varray {
    int enabled;
    int size;                   /* components a vertex */
    GLenum type;
    int stride;                 /* as given: 0 means tightly packed */
    const void *ptr;
};

/* How this draw fogs (fog_draw). */
enum { FOG_NONE, FOG_FOLD, FOG_PIXEL };

struct fgl_context;
struct attrib_frame;
struct client_frame;

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
    int mv_depth, proj_depth;
    float normal_matrix[9];     /* inverse transpose of the modelview's 3x3 */
    int normal_dirty;
    int vp_x, vp_y, vp_w, vp_h;
    float depth_near, depth_far;
    int scissor_box[4];
    float clip_plane[MAX_CLIP_PLANES][4];       /* eye coordinates */
    int clip_on[MAX_CLIP_PLANES];

    /* switches and render state */
    int cull, depth_test, lighting, normalize, color_material, scissor_test, dither;
    GLenum cull_face, front_face, shade_model, depth_func;
    int depth_mask;
    float clear_color[4], clear_depth;
    float point_size, line_width;
    GLenum polygon_mode[2];             /* front faces, back faces */
    int offset_fill, offset_line, offset_point;
    float offset_factor, offset_units;
    GLenum hints[5];                    /* GL_PERSPECTIVE_CORRECTION_HINT to GL_FOG_HINT */

    /* fragments */
    int blend, alpha_test;
    GLenum blend_src, blend_dst, alpha_func;
    float alpha_ref;
    int color_mask[4];
    GLenum draw_buffer, read_buffer;
    int pack_align, pack_row_length, pack_skip_rows, pack_skip_pixels;
    int stencil_test;
    GLenum stencil_func, stencil_fail, stencil_zfail, stencil_zpass;
    GLint stencil_ref, clear_stencil;
    GLuint stencil_value_mask, stencil_writemask;

    /* fog */
    int fog;
    GLenum fog_mode;
    float fog_density, fog_start, fog_end, fog_index;
    float fog_color[4] VEC4;
    int fog_draw;                       /* FOG_*, for the draw under way (gl_check_backend) */

    /* lighting */
    struct light lights[MAX_LIGHTS];
    int light_on[MAX_LIGHTS];
    struct material mat;
    float model_ambient[4] VEC4;
    GLenum cm_face, cm_mode;

    /* textures */
    struct texunit unit[MAX_UNITS];
    int active_unit, client_unit;
    struct gltex **textures;            /* by name; textures[0] is the default texture */
    GLuint ntextures;
    struct gltex *tex_override;         /* unit 0 draws with this instead (glDrawPixels) */
    int unpack_align, unpack_row_length, unpack_skip_rows, unpack_skip_pixels, unpack_lsb_first;

    /* current attributes */
    float color[4] VEC4, normal[3];
    float eye_normal[3];
    int eye_normal_valid;

    /* the raster position: window coordinates (GL's, bottom-left origin),
     * depth and clip w, with what the fragments drawn there get */
    float raster_pos[4];
    float raster_color[4];
    float raster_tex[MAX_UNITS][4];
    float raster_distance;
    int raster_valid;
    float zoom_x, zoom_y;

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
    GLint lock_first;
    GLsizei lock_count;

    /* glPushAttrib and glPushClientAttrib */
    struct attrib_frame *attrib[ATTRIB_DEPTH];
    struct client_frame *client_attrib[ATTRIB_DEPTH];
    int attrib_depth, client_depth;
};

extern struct fgl_context *gl_ctx;

void gl_error(struct fgl_context *c, GLenum e);
void gl_mat_identity(float *m);
void gl_mat_mul(float *out, const float *a, const float *b);
/* out = the inverse of m; -1 (and the identity) if it has none. */
int gl_mat_invert(float *out, const float *m);
/* The plane p (a row vector) through the inverse of today's modelview, as
 * glClipPlane and the eye planes of glTexGen keep it. */
void gl_plane_to_eye(struct fgl_context *c, const float *p, float *out);
float *gl_top_matrix(struct fgl_context *c);
void gl_matrix_changed(struct fgl_context *c);
/* The drawing rectangle: the buffer, cut down by the scissor box. */
void gl_clip_rect(struct fgl_context *c, int *x0, int *y0, int *x1, int *y1);
int gl_depth_bits(struct fgl_context *c);
/* Before drawing: work out how the draw fogs, and move a context whose
 * backend cannot draw with today's state to software, keeping the picture. */
void gl_check_backend(struct fgl_context *c);
/* The fog factor at eye distance d. */
float gl_fog_factor(struct fgl_context *c, float d);
/* Integer color components map to 0 to 1 (or -1 to 1): GL's int, short and byte rules. */
static inline float gl_int_color(GLint v) { return (float)((2.0 * v + 1) / 4294967295.0); }

/* The color channels glColorMask lets through, as a 0xRRGGBB mask. */
static inline uint32_t gl_rgb_write_mask(const struct fgl_context *c)
{
    return (c->color_mask[0] ? 0xFF0000u : 0) | (c->color_mask[1] ? 0xFF00u : 0) |
           (c->color_mask[2] ? 0xFFu : 0);
}

/* tex.c */
void gl_tex_init(struct fgl_context *c);
void gl_tex_free_all(struct fgl_context *c);
/* The texture unit u draws with: it is enabled and its texture has texels. */
struct gltex *gl_unit_texture(struct fgl_context *c, int u);
static inline struct gltex *gl_active_texture(struct fgl_context *c)
{
    return gl_unit_texture(c, 0);
}
/* The texture bound to the active unit. */
struct gltex *gl_bound_texture(struct fgl_context *c);
/* Level i of a texture (no texels if it has none). */
struct texlevel gl_tex_level(const struct gltex *t, int i);
/* A texture of no name, w x h RGBA texels, nearest and clamped (glDrawPixels'). */
struct gltex *gl_tex_temp(int w, int h);
void gl_tex_destroy(struct fgl_context *c, struct gltex *t);
/* Components a pixel of a color format has in client memory; 0 if it is not one. */
int gl_format_size(GLenum format);
/* A w x h image from client memory (unsigned bytes, with the unpack rules)
 * as 0xAABBGGRR texels of base format base, dst_stride texels apart. */
void gl_unpack_image(struct fgl_context *c, uint32_t *dst, int dst_stride, int w, int h, GLenum format,
                     GLenum base, const void *pixels);

/* pixels.c: the whole picture, finished, as 0xRRGGBB rows top first; NULL if out of memory. */
uint32_t *gl_read_picture(struct fgl_context *c);

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
    OP_FOG, OP_STENCIL_FUNC, OP_STENCIL_OP, OP_STENCIL_MASK, OP_CLEAR_STENCIL, OP_CLIP_PLANE,
    OP_TEX_GEN, OP_RASTER_POS, OP_BITMAP, OP_PIXEL_ZOOM, OP_ACTIVE_TEXTURE, OP_MULTI_TEXCOORD,
    OP_PUSH_ATTRIB, OP_POP_ATTRIB, OP_HINT,
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

/* pixels.c: a bitmap of w x h bytes, 1 for each pixel set, drawn at the
 * raster position, which then moves by (xmove, ymove). */
void gl_draw_bitmap(struct fgl_context *c, int w, int h, float xorig, float yorig, float xmove, float ymove,
                    const uint8_t *on);

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
/* Draw in memory from now on, starting from the picture (and the depths,
 * if the backend can read them) as they are. */
int gl_use_software(struct fgl_context *c, int keep_picture);
/* Load the software buffers: a picture, depths from 0 to 1 (either may be
 * NULL), and the stencil buffer cleared to its clear value. */
void gl_soft_load(struct fgl_context *c, const uint32_t *picture, const float *depths);

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

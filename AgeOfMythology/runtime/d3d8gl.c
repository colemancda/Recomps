/* Direct3D 8 drawing with OpenGL ES 3 (plan step 3).
 *
 * An EGL context without a window surface (Mesa's surfaceless platform, or
 * any EGL display that allows EGL_NO_SURFACE) renders into a framebuffer
 * object the size of the back buffer. Present reads the frame back as RGB565
 * for video.c, which shows it in the window and dumps it, as for every other
 * frame source. The fixed-function pipeline is a generated GLSL ES program per
 * combination of the states that change its code (vertex format, lighting,
 * texture stages, fog, alpha test); uniforms carry everything else.
 *
 * Conventions: D3D matrices are row-major and used as v * M; uploaded as they
 * are, GLSL reads their transpose, so the shader computes M^T * v. D3D clip z is
 * [0, w], GL's [-w, w]. D3D pixel centres are on integers, GL's at +0.5.
 */
#include "d3d8.h"

#ifdef RT_NO_SDL
/* Headless test builds have no OpenGL. */
int d3dgl_init(int w, int h) { (void)w, (void)h; return 0; }
void d3dgl_resize(int w, int h) { (void)w, (void)h; }
void d3dgl_clear(uint32_t count, uint32_t rects, uint32_t flags, uint32_t color, float z, uint32_t stencil) {
    (void)count, (void)rects, (void)flags, (void)color, (void)z, (void)stencil;
}
void d3dgl_draw(uint32_t type, uint32_t start_vertex, uint32_t prim_count) { (void)type, (void)start_vertex, (void)prim_count; }
void d3dgl_draw_indexed(uint32_t type, uint32_t min_index, uint32_t num_vertices, uint32_t start_index,
                        uint32_t prim_count) {
    (void)type, (void)min_index, (void)num_vertices, (void)start_index, (void)prim_count;
}
void d3dgl_draw_up(uint32_t type, uint32_t prim_count, uint32_t data, uint32_t stride) {
    (void)type, (void)prim_count, (void)data, (void)stride;
}
void d3dgl_draw_indexed_up(uint32_t type, uint32_t min_index, uint32_t num_vertices, uint32_t prim_count,
                           uint32_t indices, uint32_t index_format, uint32_t data, uint32_t stride) {
    (void)type, (void)min_index, (void)num_vertices, (void)prim_count, (void)indices, (void)index_format,
        (void)data, (void)stride;
}
const uint16_t *d3dgl_present(int *w, int *h) { (void)w, (void)h; return NULL; }
void d3dgl_texture_free(Obj *tex) { (void)tex; }
#else

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl32.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

#define dev d3d_dev
#define objs d3d_objs

static struct {
    int on;
    EGLDisplay dpy;
    EGLContext ctx;
    GLuint fbo, color, depth;
    int w, h;
    GLuint vbo[16], ibo, vao;
    uint8_t *rgba;     /* readback */
    uint16_t *frame;   /* RGB565 */
    uint8_t *conv;     /* texture conversion scratch */
    size_t conv_cap;
    unsigned warned;
} gl;

static float fbits(uint32_t v) {
    float f;
    memcpy(&f, &v, 4);
    return f;
}

/* ------------------------------------------------------------------ setup */
static GLuint make_fbo_storage(int w, int h) {
    if (gl.color) glDeleteRenderbuffers(1, &gl.color);
    if (gl.depth) glDeleteRenderbuffers(1, &gl.depth);
    glGenRenderbuffers(1, &gl.color);
    glBindRenderbuffer(GL_RENDERBUFFER, gl.color);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA8, w, h);
    glGenRenderbuffers(1, &gl.depth);
    glBindRenderbuffer(GL_RENDERBUFFER, gl.depth);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, w, h);
    glBindFramebuffer(GL_FRAMEBUFFER, gl.fbo);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, gl.color);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, gl.depth);
    GLenum st = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    gl.w = w, gl.h = h;
    free(gl.rgba);
    free(gl.frame);
    gl.rgba = malloc((size_t)w * h * 4);
    gl.frame = malloc((size_t)w * h * 2);
    return st;
}

/* Headless test builds have no OpenGL. */
/* An OpenGL ES 3 context on display dpy, made current without a surface. */
static int try_display(EGLDisplay dpy, const char *what) {
    EGLint major, minor;
    if (dpy == EGL_NO_DISPLAY || !eglInitialize(dpy, &major, &minor)) return 0;
    eglBindAPI(EGL_OPENGL_ES_API);
    EGLint cattr[] = {EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT, EGL_NONE};
    EGLConfig cfg = 0;
    EGLint n = 0;
    eglChooseConfig(dpy, cattr, &cfg, 1, &n);
    EGLint xattr[] = {EGL_CONTEXT_MAJOR_VERSION, 3, EGL_CONTEXT_MINOR_VERSION, 2, EGL_NONE};
    EGLContext ctx = eglCreateContext(dpy, n ? cfg : (EGLConfig)0, EGL_NO_CONTEXT, xattr);
    if (ctx == EGL_NO_CONTEXT) {
        xattr[3] = 0; /* OpenGL ES 3.0 then */
        ctx = eglCreateContext(dpy, n ? cfg : (EGLConfig)0, EGL_NO_CONTEXT, xattr);
    }
    if (ctx == EGL_NO_CONTEXT) {
        rt_log("d3d8: %s: cannot create an OpenGL ES 3 context (EGL %#x)\n", what, eglGetError());
        return 0;
    }
    if (!eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, ctx)) {
        rt_log("d3d8: %s: cannot make the context current (EGL %#x)\n", what, eglGetError());
        eglDestroyContext(dpy, ctx);
        return 0;
    }
    gl.dpy = dpy;
    gl.ctx = ctx;
    return 1;
}

int d3dgl_init(int w, int h) {
    if (getenv("RECOMP_NO_GL")) return 0;
    PFNEGLGETPLATFORMDISPLAYEXTPROC get_platform_display =
        (PFNEGLGETPLATFORMDISPLAYEXTPROC)eglGetProcAddress("eglGetPlatformDisplayEXT");
    /* Mesa's surfaceless platform needs no window system; the default display is the fallback. */
    if (!(get_platform_display &&
          try_display(get_platform_display(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, NULL), "surfaceless")) &&
        !try_display(eglGetDisplay(EGL_DEFAULT_DISPLAY), "default display")) {
        rt_log("d3d8: no OpenGL ES 3; drawing nothing\n");
        return 0;
    }
    glGenFramebuffers(1, &gl.fbo);
    if (make_fbo_storage(w, h) != GL_FRAMEBUFFER_COMPLETE) {
        rt_log("d3d8: framebuffer incomplete; drawing nothing\n");
        return 0;
    }
    glGenVertexArrays(1, &gl.vao);
    glBindVertexArray(gl.vao);
    glGenBuffers(16, gl.vbo);
    glGenBuffers(1, &gl.ibo);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    rt_log("d3d8: drawing with %s (%s), %dx%d\n", glGetString(GL_RENDERER), glGetString(GL_VERSION), w, h);
    gl.on = 1;
    return 1;
}

static int current(void) {
    if (!gl.on) return 0;
    if (eglGetCurrentContext() != gl.ctx) eglMakeCurrent(gl.dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, gl.ctx);
    return 1;
}

void d3dgl_resize(int w, int h) {
    if (!current() || (w == gl.w && h == gl.h)) return;
    make_fbo_storage(w, h);
}

/* ---------------------------------------------------------------- textures */
static int is_dxt(uint32_t f) { return f >= FMT_DXT1 && f <= FMT_DXT5 && (f & 0xFFFFFFu) == 0x545844; }

static void rgb565(uint16_t v, uint8_t *o) {
    o[0] = (uint8_t)(((v >> 11) & 31) * 255 / 31);
    o[1] = (uint8_t)(((v >> 5) & 63) * 255 / 63);
    o[2] = (uint8_t)((v & 31) * 255 / 31);
}

/* One DXT block to 4x4 RGBA at out (pitch in pixels). */
static void dxt_block(const uint8_t *b, uint32_t fmt, uint8_t *out, int pitch, int bw, int bh) {
    uint8_t alpha[16];
    const uint8_t *cb = b;
    if (fmt == FMT_DXT1) {
        memset(alpha, 255, 16);
    } else if (fmt == FMT_DXT2 || fmt == FMT_DXT3) {
        for (int i = 0; i < 16; i++) alpha[i] = (uint8_t)(((b[i / 2] >> (4 * (i & 1))) & 15) * 17);
        cb = b + 8;
    } else {
        uint8_t a0 = b[0], a1 = b[1], pal[8];
        pal[0] = a0, pal[1] = a1;
        if (a0 > a1)
            for (int i = 1; i < 7; i++) pal[i + 1] = (uint8_t)(((7 - i) * a0 + i * a1) / 7);
        else {
            for (int i = 1; i < 5; i++) pal[i + 1] = (uint8_t)(((5 - i) * a0 + i * a1) / 5);
            pal[6] = 0, pal[7] = 255;
        }
        uint64_t bits = 0;
        for (int i = 0; i < 6; i++) bits |= (uint64_t)b[2 + i] << (8 * i);
        for (int i = 0; i < 16; i++) alpha[i] = pal[(bits >> (3 * i)) & 7];
        cb = b + 8;
    }
    uint16_t c0 = (uint16_t)(cb[0] | cb[1] << 8), c1 = (uint16_t)(cb[2] | cb[3] << 8);
    uint8_t col[4][4];
    rgb565(c0, col[0]);
    rgb565(c1, col[1]);
    col[0][3] = col[1][3] = col[2][3] = 255;
    col[3][3] = 255;
    if (c0 > c1 || fmt != FMT_DXT1) {
        for (int k = 0; k < 3; k++) {
            col[2][k] = (uint8_t)((2 * col[0][k] + col[1][k]) / 3);
            col[3][k] = (uint8_t)((col[0][k] + 2 * col[1][k]) / 3);
        }
    } else {
        for (int k = 0; k < 3; k++) col[2][k] = (uint8_t)((col[0][k] + col[1][k]) / 2), col[3][k] = 0;
        col[3][3] = 0;
    }
    uint32_t idx = (uint32_t)(cb[4] | cb[5] << 8 | cb[6] << 16 | (uint32_t)cb[7] << 24);
    for (int y = 0; y < bh; y++)
        for (int x = 0; x < bw; x++) {
            int i = y * 4 + x;
            const uint8_t *c = col[(idx >> (2 * i)) & 3];
            uint8_t *o = out + 4 * ((size_t)y * pitch + x);
            o[0] = c[0], o[1] = c[1], o[2] = c[2];
            o[3] = fmt == FMT_DXT1 ? c[3] : alpha[i];
        }
}

/* Surface s converted to RGBA8 in gl.conv. */
static const uint8_t *to_rgba(const Obj *s) {
    size_t need = (size_t)s->width * s->height * 4;
    if (need > gl.conv_cap) {
        gl.conv = realloc(gl.conv, need);
        gl.conv_cap = need;
    }
    uint8_t *o = gl.conv;
    uint32_t w = s->width, h = s->height;
    if (is_dxt(s->format)) {
        for (uint32_t by = 0; by < (h + 3) / 4; by++)
            for (uint32_t bx = 0; bx < (w + 3) / 4; bx++) {
                const uint8_t *b = G(s->data + by * s->pitch + bx * (s->format == FMT_DXT1 ? 8 : 16));
                int bw = (int)(w - bx * 4 < 4 ? w - bx * 4 : 4), bh = (int)(h - by * 4 < 4 ? h - by * 4 : 4);
                dxt_block(b, s->format, o + 4 * ((size_t)by * 4 * w + bx * 4), (int)w, bw, bh);
            }
        return o;
    }
    for (uint32_t y = 0; y < h; y++) {
        const uint8_t *row = G(s->data + y * s->pitch);
        uint8_t *d = o + (size_t)y * w * 4;
        for (uint32_t x = 0; x < w; x++, d += 4) {
            uint32_t v;
            switch (s->format) {
            case FMT_A8R8G8B8: case FMT_X8R8G8B8:
                d[0] = row[4 * x + 2], d[1] = row[4 * x + 1], d[2] = row[4 * x];
                d[3] = s->format == FMT_A8R8G8B8 ? row[4 * x + 3] : 255;
                break;
            case FMT_R5G6B5:
                rgb565((uint16_t)(row[2 * x] | row[2 * x + 1] << 8), d);
                d[3] = 255;
                break;
            case FMT_X1R5G5B5: case FMT_A1R5G5B5:
                v = (uint32_t)(row[2 * x] | row[2 * x + 1] << 8);
                d[0] = (uint8_t)(((v >> 10) & 31) * 255 / 31), d[1] = (uint8_t)(((v >> 5) & 31) * 255 / 31);
                d[2] = (uint8_t)((v & 31) * 255 / 31);
                d[3] = s->format == FMT_X1R5G5B5 || (v & 0x8000) ? 255 : 0;
                break;
            case FMT_A4R4G4B4: case FMT_X4R4G4B4:
                v = (uint32_t)(row[2 * x] | row[2 * x + 1] << 8);
                d[0] = (uint8_t)(((v >> 8) & 15) * 17), d[1] = (uint8_t)(((v >> 4) & 15) * 17);
                d[2] = (uint8_t)((v & 15) * 17);
                d[3] = s->format == FMT_X4R4G4B4 ? 255 : (uint8_t)((v >> 12) * 17);
                break;
            case FMT_A8: d[0] = d[1] = d[2] = 255, d[3] = row[x]; break;
            case FMT_L8: d[0] = d[1] = d[2] = row[x], d[3] = 255; break;
            case FMT_A8L8: d[0] = d[1] = d[2] = row[2 * x], d[3] = row[2 * x + 1]; break;
            default: d[0] = d[2] = 255, d[1] = 0, d[3] = 255; break; /* unknown: magenta */
            }
        }
    }
    return o;
}

static void texture_upload(Obj *t) {
    if (!t->gltex) glGenTextures(1, &t->gltex);
    glBindTexture(GL_TEXTURE_2D, t->gltex);
    for (uint32_t l = 0; l < t->levels; l++) {
        const Obj *s = &objs[t->level_surf[l]];
        if (!s->data) continue;
        glTexImage2D(GL_TEXTURE_2D, (GLint)l, GL_RGBA8, (GLsizei)s->width, (GLsizei)s->height, 0, GL_RGBA,
                     GL_UNSIGNED_BYTE, to_rgba(s));
    }
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, (GLint)(t->levels ? t->levels - 1 : 0));
    t->dirty = 0;
    t->gl_sampler_key = 0xFFFFFFFFu;
}

void d3dgl_texture_free(Obj *tex) {
    if (tex->gltex && current()) glDeleteTextures(1, &tex->gltex);
    tex->gltex = 0;
}

static GLenum gl_wrap(uint32_t a) {
    switch (a) {
    case 2: return GL_MIRRORED_REPEAT;
    case 3: case 4: return GL_CLAMP_TO_EDGE; /* clamp, border */
    default: return GL_REPEAT;
    }
}

/* Bind the texture of stage i (with its sampler states) to unit i; 0 if none. */
static int bind_stage_texture(int i) {
    glActiveTexture(GL_TEXTURE0 + (GLenum)i);
    int ti = dev.texture[i];
    if (ti < 0 || objs[ti].kind != K_TEXTURE) {
        glBindTexture(GL_TEXTURE_2D, 0);
        return 0;
    }
    Obj *t = &objs[ti];
    if (!t->gltex || t->dirty) texture_upload(t);
    glBindTexture(GL_TEXTURE_2D, t->gltex);
    const uint32_t *ts = dev.tss[i];
    uint32_t key = ts[13] | ts[14] << 4 | ts[16] << 8 | ts[17] << 12 | ts[18] << 16;
    if (key != t->gl_sampler_key) {
        t->gl_sampler_key = key;
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, (GLint)gl_wrap(ts[13]));
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, (GLint)gl_wrap(ts[14]));
        int mag_lin = ts[16] >= 2, min_lin = ts[17] >= 2;
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, mag_lin ? GL_LINEAR : GL_NEAREST);
        GLint minf;
        if (ts[18] == 0 || t->levels <= 1) minf = min_lin ? GL_LINEAR : GL_NEAREST;
        else if (ts[18] == 1) minf = min_lin ? GL_LINEAR_MIPMAP_NEAREST : GL_NEAREST_MIPMAP_NEAREST;
        else minf = min_lin ? GL_LINEAR_MIPMAP_LINEAR : GL_NEAREST_MIPMAP_LINEAR;
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, minf);
    }
    return 1;
}

/* ---------------------------------------------------------- vertex input */
typedef struct Layout {
    int n;
    VElem e[16];
    int rhw;    /* pretransformed positions (D3DFVF_XYZRHW) */
} Layout;

/* The current vertex layout: a declaration handle or an FVF code. */
static int current_layout(Layout *L) {
    memset(L, 0, sizeof *L);
    uint32_t vs = dev.vshader;
    if (vs >= D3D_DECL_BASE) {
        uint32_t i = vs - D3D_DECL_BASE;
        if (i >= D3D_MAX_DECLS || !dev.decl[i].used) return 0;
        L->n = dev.decl[i].n;
        memcpy(L->e, dev.decl[i].e, sizeof L->e);
        return 1;
    }
    uint32_t fvf = vs, off = 0;
    switch (fvf & 0xE) {
    case 0x2: L->e[L->n++] = (VElem){0, VT_FLOAT3, VR_POSITION, 0}; off = 12; break;
    case 0x4: L->e[L->n++] = (VElem){0, VT_FLOAT4, VR_POSITION, 0}; off = 16; L->rhw = 1; break;
    case 0x6: case 0x8: case 0xA: case 0xC: case 0xE:
        L->e[L->n++] = (VElem){0, VT_FLOAT3, VR_POSITION, 0};
        off = 12 + 4 * (((fvf & 0xE) - 4) / 2); /* skip the blend weights */
        break;
    default: return 0;
    }
    if (fvf & 0x10) L->e[L->n++] = (VElem){0, VT_FLOAT3, VR_NORMAL, (uint16_t)off}, off += 12;
    if (fvf & 0x20) off += 4; /* PSIZE */
    if (fvf & 0x40) L->e[L->n++] = (VElem){0, VT_D3DCOLOR, VR_DIFFUSE, (uint16_t)off}, off += 4;
    if (fvf & 0x80) L->e[L->n++] = (VElem){0, VT_D3DCOLOR, VR_SPECULAR, (uint16_t)off}, off += 4;
    unsigned ntex = (fvf >> 8) & 0xF;
    for (unsigned t = 0; t < ntex && t < 8; t++) {
        unsigned fmt = (fvf >> (16 + 2 * t)) & 3; /* 0: 2 floats, 1: 3, 2: 4, 3: 1 */
        static const uint8_t types[4] = {VT_FLOAT2, VT_FLOAT3, VT_FLOAT4, VT_FLOAT1};
        static const uint8_t sizes[4] = {8, 12, 16, 4};
        L->e[L->n++] = (VElem){0, types[fmt], (uint8_t)(VR_TEX0 + t), (uint16_t)off};
        off += sizes[fmt];
    }
    return 1;
}

/* Attribute locations: 0 position, 1 normal, 2 diffuse, 3 specular, 4..11 texcoord 0..7. */
static int attr_of(int reg) {
    switch (reg) {
    case VR_POSITION: return 0;
    case VR_NORMAL: return 1;
    case VR_DIFFUSE: return 2;
    case VR_SPECULAR: return 3;
    default: return reg >= VR_TEX0 && reg < VR_TEX0 + 8 ? 4 + (reg - VR_TEX0) : -1;
    }
}

/* Point the attributes at the vertices first..first+count of each stream, copied from
 * guest memory (up: the DrawPrimitiveUP data with its stride, as stream 0). */
static unsigned set_vertex_input(const Layout *L, uint32_t first, uint32_t count, uint32_t up, uint32_t up_stride) {
    unsigned have = 0;
    for (int a = 0; a < 12; a++) glDisableVertexAttribArray((GLuint)a);
    uint32_t done = 0;
    for (int k = 0; k < L->n; k++) {
        const VElem *e = &L->e[k];
        int loc = attr_of(e->reg);
        if (loc < 0) continue;
        uint32_t base, stride;
        if (up) {
            if (e->stream) continue;
            base = up, stride = up_stride;
        } else {
            int si = dev.stream[e->stream];
            if (si < 0 || !objs[si].data) continue;
            base = objs[si].data, stride = dev.stride[e->stream];
        }
        glBindBuffer(GL_ARRAY_BUFFER, gl.vbo[e->stream]);
        if (!(done & (1u << e->stream))) {
            done |= 1u << e->stream;
            glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)stride * count, G(base + first * stride), GL_STREAM_DRAW);
        }
        static const GLint comps[8] = {1, 2, 3, 4, 4, 4, 2, 4};
        static const GLenum types[8] = {GL_FLOAT, GL_FLOAT, GL_FLOAT, GL_FLOAT, GL_UNSIGNED_BYTE, GL_UNSIGNED_BYTE,
                                        GL_SHORT, GL_SHORT};
        glVertexAttribPointer((GLuint)loc, comps[e->type], types[e->type], e->type == VT_D3DCOLOR,
                              (GLsizei)stride, (const void *)(uintptr_t)e->offset);
        glEnableVertexAttribArray((GLuint)loc);
        have |= 1u << loc;
    }
    /* Defaults for missing inputs: white diffuse (BGRA order, like D3DCOLOR), no specular. */
    if (!(have & 4)) glVertexAttrib4f(2, 1, 1, 1, 1);
    if (!(have & 8)) glVertexAttrib4f(3, 0, 0, 0, 0);
    if (!(have & 2)) glVertexAttrib4f(1, 0, 0, 1, 0);
    for (int t = 0; t < 8; t++)
        if (!(have & (16u << t))) glVertexAttrib4f((GLuint)(4 + t), 0, 0, 0, 1);
    return have;
}

/* -------------------------------------------------------------- shaders */
typedef struct Key {
    uint8_t rhw, has_normal, has_diffuse, has_specular;
    uint8_t lighting, specular_enable, local_viewer, normalize;
    uint8_t mat_src[4];      /* diffuse, specular, ambient, emissive: 0 material, 1 color1, 2 color2 */
    uint8_t nlights;
    uint8_t light_type[8];
    uint8_t fog_mode, fog_table, fog_enable, alpha_func;
    uint8_t nstages;
    uint8_t has_tex[8];
    uint8_t cop[8], ca1[8], ca2[8], ca0[8], aop[8], aa1[8], aa2[8], aa0[8], res[8];
    uint8_t tci[8], tcgen[8], ttf[8], tcsize[8]; /* tcsize: projected */
} Key;

typedef struct Prog {
    Key key;
    GLuint prog;
    GLint u_world, u_view, u_proj, u_viewport, u_target, u_texm, u_campos;
    GLint u_mat, u_ambient, u_light, u_tfactor, u_fog, u_fogcolor, u_alpharef, u_sampler;
} Prog;

#define MAX_PROGS 512
static Prog progs[MAX_PROGS];
static int nprogs;

static void append(char **buf, size_t *len, size_t *cap, const char *fmt, ...) __attribute__((format(printf, 4, 5)));
static void append(char **buf, size_t *len, size_t *cap, const char *fmt, ...) {
    va_list ap;
    for (;;) {
        va_start(ap, fmt);
        int n = vsnprintf(*buf + *len, *cap - *len, fmt, ap);
        va_end(ap);
        if ((size_t)n < *cap - *len) {
            *len += (size_t)n;
            return;
        }
        *cap = *cap * 2 + (size_t)n;
        *buf = realloc(*buf, *cap);
    }
}
#define A(...) append(&src, &len, &cap, __VA_ARGS__)

static const char *arg_src(int a, int stage, int has_tex) {
    static char out[8][64];
    static int slot;
    char *o = out[slot++ & 7];
    const char *base;
    switch (a & 7) {
    case 0: base = "vDiffuse"; break;
    case 1: base = stage ? "cur" : "vDiffuse"; break;
    case 2: base = has_tex ? "tex" : "vec4(1.0)"; break;
    case 3: base = "uTFactor"; break;
    case 4: base = "vSpecular"; break;
    case 5: base = "tmp"; break;
    default: base = "cur";
    }
    if (a & 0x20) snprintf(o, 64, "vec4(%s.a)", base);
    else snprintf(o, 64, "%s", base);
    if (a & 0x10) {
        char t[64];
        snprintf(t, sizeof t, "(vec4(1.0) - %s)", o);
        snprintf(o, 64, "%s", t);
    }
    return o;
}

/* The expression of texture operation op on arguments x, y, z (a0) for component suffix c. */
static void op_expr(char *out, size_t cap, int op, const char *x, const char *y, const char *z, const char *c) {
    switch (op) {
    case 2: snprintf(out, cap, "%s%s", x, c); break;
    case 3: snprintf(out, cap, "%s%s", y, c); break;
    case 4: snprintf(out, cap, "%s%s * %s%s", x, c, y, c); break;
    case 5: snprintf(out, cap, "2.0 * %s%s * %s%s", x, c, y, c); break;
    case 6: snprintf(out, cap, "4.0 * %s%s * %s%s", x, c, y, c); break;
    case 7: snprintf(out, cap, "%s%s + %s%s", x, c, y, c); break;
    case 8: snprintf(out, cap, "%s%s + %s%s - 0.5", x, c, y, c); break;
    case 9: snprintf(out, cap, "2.0 * (%s%s + %s%s - 0.5)", x, c, y, c); break;
    case 10: snprintf(out, cap, "%s%s - %s%s", x, c, y, c); break;
    case 11: snprintf(out, cap, "%s%s + %s%s - %s%s * %s%s", x, c, y, c, x, c, y, c); break;
    case 12: snprintf(out, cap, "mix(%s%s, %s%s, vDiffuse.a)", y, c, x, c); break;
    case 13: snprintf(out, cap, "mix(%s%s, %s%s, tex.a)", y, c, x, c); break;
    case 14: snprintf(out, cap, "mix(%s%s, %s%s, uTFactor.a)", y, c, x, c); break;
    case 15: snprintf(out, cap, "%s%s + %s%s * (1.0 - tex.a)", x, c, y, c); break;
    case 16: snprintf(out, cap, "mix(%s%s, %s%s, cur.a)", y, c, x, c); break;
    case 18: snprintf(out, cap, "%s%s + %s.a * %s%s", x, c, x, y, c); break;
    case 19: snprintf(out, cap, "%s%s * %s%s + %s.a", x, c, y, c, x); break;
    case 20: snprintf(out, cap, "%s%s + (1.0 - %s.a) * %s%s", x, c, x, y, c); break;
    case 21: snprintf(out, cap, "(1.0 - %s%s) * %s%s + %s.a", x, c, y, c, x); break;
    case 24: snprintf(out, cap, "vec4(clamp(4.0 * dot(%s.rgb - 0.5, %s.rgb - 0.5), 0.0, 1.0))%s", x, y, c); break;
    case 25: snprintf(out, cap, "%s%s + %s%s * %s%s", z, c, x, c, y, c); break;
    case 26: snprintf(out, cap, "mix(%s%s, %s%s, %s%s)", y, c, x, c, z, c); break;
    default: snprintf(out, cap, "%s%s", x, c); break; /* bump mapping and the rest: pass the first argument */
    }
}

static GLuint compile(GLenum type, const char *src) {
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, NULL);
    glCompileShader(s);
    GLint ok;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[2048];
        glGetShaderInfoLog(s, sizeof log, NULL, log);
        rt_log("d3d8: shader compile error:\n%s\n%s\n", log, src);
    }
    return s;
}

static Prog *build_prog(const Key *k) {
    char *src = malloc(4096);
    size_t len = 0, cap = 4096;
    /* ---- vertex shader */
    A("#version 300 es\nprecision highp float;\n");
    A("layout(location=0) in vec4 aPos;\nlayout(location=1) in vec3 aNormal;\n");
    A("layout(location=2) in vec4 aDiffuse;\nlayout(location=3) in vec4 aSpecular;\n");
    for (int t = 0; t < 8; t++) A("layout(location=%d) in vec4 aTex%d;\n", 4 + t, t);
    A("uniform mat4 uWorld, uView, uProj, uTexM[8];\nuniform vec4 uViewport; uniform vec2 uTarget;\n");
    A("uniform vec3 uCamPos;\nuniform vec4 uMat[5];\nuniform vec4 uAmbient;\nuniform vec4 uLight[8*7];\n");
    A("uniform vec4 uFog;\n");
    A("out vec4 vDiffuse, vSpecular;\nout float vFogDist;\n");
    for (int t = 0; t < k->nstages; t++) A("out vec4 vTex%d;\n", t);
    A("void main() {\n");
    A("  vec4 dif = aDiffuse.bgra, spe = aSpecular.bgra;\n");
    if (k->rhw) {
        /* pretransformed: screen pixels, z in [0,1], w = 1/rhw */
        A("  float w = aPos.w != 0.0 ? 1.0 / aPos.w : 1.0;\n");
        A("  vec2 ndc = vec2((aPos.x + 0.5 - uViewport.x) / uViewport.z * 2.0 - 1.0,\n"
          "                  1.0 - (aPos.y + 0.5 - uViewport.y) / uViewport.w * 2.0);\n");
        A("  gl_Position = vec4(ndc * w, (aPos.z * 2.0 - 1.0) * w, w);\n");
        A("  vFogDist = 0.0;\n  vec3 P = aPos.xyz, N = vec3(0.0);\n");
    } else {
        A("  vec4 wp = uWorld * vec4(aPos.xyz, 1.0);\n  vec4 ep = uView * wp;\n  vec4 cp = uProj * ep;\n");
        A("  cp.z = 2.0 * cp.z - cp.w;\n");
        A("  cp.x += cp.w / uViewport.z; cp.y -= cp.w / uViewport.w;\n"); /* pixel centres */
        A("  gl_Position = cp;\n  vFogDist = abs(ep.z);\n");
        A("  vec3 P = wp.xyz;\n  vec3 N = mat3(uWorld) * aNormal;\n");
        if (k->normalize) A("  N = normalize(N);\n");
    }
    if (k->lighting) {
        const char *src_of[3] = {"", "dif", "spe"};
        char md[64], ms[64], ma[64], me[64];
        snprintf(md, sizeof md, "%s", k->mat_src[0] ? src_of[k->mat_src[0]] : "uMat[0]");
        snprintf(ms, sizeof ms, "%s", k->mat_src[1] ? src_of[k->mat_src[1]] : "uMat[2]");
        snprintf(ma, sizeof ma, "%s", k->mat_src[2] ? src_of[k->mat_src[2]] : "uMat[1]");
        snprintf(me, sizeof me, "%s", k->mat_src[3] ? src_of[k->mat_src[3]] : "uMat[3]");
        A("  vec3 accD = vec3(0.0), accS = vec3(0.0), accA = vec3(0.0);\n");
        A("  vec3 V = %s;\n", k->local_viewer ? "normalize(uCamPos - P)" : "vec3(0.0, 0.0, -1.0)");
        for (int l = 0; l < k->nlights; l++) {
            /* uLight[l*7 + 0] diffuse, 1 specular, 2 ambient, 3 position+range, 4 direction,
             * 5 attenuation (a0, a1, a2, falloff), 6 (cos theta/2, cos phi/2) */
            int b = l * 7;
            A("  {\n");
            if (k->light_type[l] == 3) {
                A("    vec3 L = normalize(-uLight[%d].xyz); float att = 1.0;\n", b + 4);
            } else {
                A("    vec3 Ld = uLight[%d].xyz - P; float d = length(Ld); vec3 L = Ld / max(d, 1e-6);\n", b + 3);
                A("    float att = d > uLight[%d].w ? 0.0 : 1.0 / max(uLight[%d].x + uLight[%d].y * d + uLight[%d].z * d * d, 1e-6);\n",
                  b + 3, b + 5, b + 5, b + 5);
                if (k->light_type[l] == 2) {
                    A("    float rho = dot(-L, normalize(uLight[%d].xyz));\n", b + 4);
                    A("    att *= rho > uLight[%d].x ? 1.0 : rho <= uLight[%d].y ? 0.0 :\n"
                      "           pow(clamp((rho - uLight[%d].y) / max(uLight[%d].x - uLight[%d].y, 1e-6), 0.0, 1.0), uLight[%d].w);\n",
                      b + 6, b + 6, b + 6, b + 6, b + 6, b + 5);
                }
            }
            A("    float nl = max(dot(N, L), 0.0);\n");
            A("    accA += uLight[%d].rgb * att;\n", b + 2);
            A("    accD += uLight[%d].rgb * nl * att;\n", b);
            if (k->specular_enable)
                A("    if (nl > 0.0) accS += uLight[%d].rgb * pow(max(dot(N, normalize(L + V)), 0.0), uMat[4].x) * att;\n", b + 1);
            A("  }\n");
        }
        A("  vDiffuse = vec4(clamp(%s.rgb + (uAmbient.rgb + accA) * %s.rgb + accD * %s.rgb, 0.0, 1.0), %s.a);\n", me, ma,
          md, md);
        A("  vSpecular = vec4(clamp(accS * %s.rgb, 0.0, 1.0), spe.a);\n", ms);
    } else {
        A("  vDiffuse = %s;\n  vSpecular = %s;\n", k->has_diffuse ? "dif" : "vec4(1.0)", k->has_specular ? "spe" : "vec4(0.0)");
    }
    for (int t = 0; t < k->nstages; t++) {
        if (!k->has_tex[t]) {
            A("  vTex%d = vec4(0.0);\n", t);
            continue;
        }
        switch (k->tcgen[t]) {
        case 1: A("  vec4 tc%d = vec4(normalize(mat3(uView) * N), 1.0);\n", t); break;        /* camera-space normal */
        case 2: A("  vec4 tc%d = vec4((uView * vec4(P, 1.0)).xyz, 1.0);\n", t); break;       /* camera-space position */
        case 3: A("  vec4 tc%d = vec4(reflect(normalize((uView * vec4(P, 1.0)).xyz), normalize(mat3(uView) * N)), 1.0);\n", t); break;
        default: A("  vec4 tc%d = aTex%d;\n", t, k->tci[t] & 7); break;
        }
        if ((k->ttf[t] & 0xFF) && !k->rhw) A("  tc%d = uTexM[%d] * vec4(tc%d.xyz, 1.0);\n", t, t, t);
        A("  vTex%d = tc%d;\n", t, t);
    }
    A("}\n");
    GLuint vs = compile(GL_VERTEX_SHADER, src);

    /* ---- fragment shader */
    len = 0;
    A("#version 300 es\nprecision highp float;\n");
    A("in vec4 vDiffuse, vSpecular;\nin float vFogDist;\n");
    for (int t = 0; t < k->nstages; t++) A("in vec4 vTex%d;\n", t);
    A("uniform sampler2D uSampler[8];\nuniform vec4 uTFactor;\nuniform vec4 uFog;\nuniform vec4 uFogColor;\nuniform float uAlphaRef;\n");
    A("out vec4 oColor;\n");
    A("void main() {\n  vec4 cur = vDiffuse, tmp = vec4(0.0), tex = vec4(1.0);\n");
    for (int t = 0; t < k->nstages; t++) {
        A("  {\n");
        if (k->has_tex[t]) {
            int count = k->ttf[t] & 0xFF;
            if (k->tcsize[t]) /* projected */
                A("    tex = texture(uSampler[%d], vTex%d.xy / vTex%d.%c);\n", t, t, t, "wxyzw"[count < 5 ? count : 4]);
            else
                A("    tex = texture(uSampler[%d], vTex%d.xy);\n", t, t);
        } else {
            A("    tex = vec4(1.0);\n");
        }
        const char *x = arg_src(k->ca1[t], t, k->has_tex[t]), *y = arg_src(k->ca2[t], t, k->has_tex[t]);
        const char *z = arg_src(k->ca0[t], t, k->has_tex[t]);
        char ce[512], ae[512];
        op_expr(ce, sizeof ce, k->cop[t], x, y, z, ".rgb");
        const char *ax = arg_src(k->aa1[t], t, k->has_tex[t]), *ay = arg_src(k->aa2[t], t, k->has_tex[t]);
        const char *az = arg_src(k->aa0[t], t, k->has_tex[t]);
        if (k->aop[t] <= 1) snprintf(ae, sizeof ae, "%s", t ? "cur.a" : "vDiffuse.a");
        else op_expr(ae, sizeof ae, k->aop[t], ax, ay, az, ".a");
        const char *dst = k->res[t] == 5 ? "tmp" : "cur";
        A("    vec3 c = clamp(%s, 0.0, 1.0);\n    float a = clamp(%s, 0.0, 1.0);\n    %s = vec4(c, a);\n  }\n", ce, ae, dst);
    }
    if (k->specular_enable) A("  cur.rgb = clamp(cur.rgb + vSpecular.rgb, 0.0, 1.0);\n");
    if (k->alpha_func && k->alpha_func != 8) {
        static const char *cmp[9] = {"", "false", "<", "==", "<=", ">", "!=", ">=", "true"};
        if (k->alpha_func == 1) A("  discard;\n");
        else A("  if (!(floor(cur.a * 255.0 + 0.5) %s uAlphaRef)) discard;\n", cmp[k->alpha_func]);
    }
    if (k->fog_enable) {
        /* uFog: start, end, density, (unused) */
        if (k->fog_mode == 1) A("  float f = clamp(exp(-uFog.z * vFogDist), 0.0, 1.0);\n");
        else if (k->fog_mode == 2) A("  float f = clamp(exp(-(uFog.z * vFogDist) * (uFog.z * vFogDist)), 0.0, 1.0);\n");
        else if (k->fog_mode == 3) A("  float f = clamp((uFog.y - vFogDist) / max(uFog.y - uFog.x, 1e-6), 0.0, 1.0);\n");
        else A("  float f = vSpecular.a;\n");
        A("  cur.rgb = mix(uFogColor.rgb, cur.rgb, f);\n");
    }
    A("  oColor = cur;\n}\n");
    GLuint fs = compile(GL_FRAGMENT_SHADER, src);
    free(src);

    GLuint p = glCreateProgram();
    glAttachShader(p, vs);
    glAttachShader(p, fs);
    glLinkProgram(p);
    glDeleteShader(vs);
    glDeleteShader(fs);
    GLint ok;
    glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[2048];
        glGetProgramInfoLog(p, sizeof log, NULL, log);
        rt_log("d3d8: program link error: %s\n", log);
    }
    if (nprogs == MAX_PROGS) {
        rt_log("d3d8: shader cache full; starting over\n");
        for (int i = 0; i < nprogs; i++) glDeleteProgram(progs[i].prog);
        nprogs = 0;
    }
    Prog *g = &progs[nprogs++];
    g->key = *k;
    g->prog = p;
#define U(f, n) g->f = glGetUniformLocation(p, n)
    U(u_world, "uWorld"), U(u_view, "uView"), U(u_proj, "uProj"), U(u_viewport, "uViewport");
    U(u_target, "uTarget"), U(u_texm, "uTexM"), U(u_campos, "uCamPos"), U(u_mat, "uMat");
    U(u_ambient, "uAmbient"), U(u_light, "uLight"), U(u_tfactor, "uTFactor"), U(u_fog, "uFog");
    U(u_fogcolor, "uFogColor"), U(u_alpharef, "uAlphaRef"), U(u_sampler, "uSampler");
#undef U
    glUseProgram(p);
    GLint units[8] = {0, 1, 2, 3, 4, 5, 6, 7};
    if (g->u_sampler >= 0) glUniform1iv(g->u_sampler, 8, units);
    if (g_opt.verbose) rt_log("d3d8: program %d built (%d stages, %d lights%s)\n", nprogs, k->nstages, k->nlights,
                              k->rhw ? ", pretransformed" : "");
    return g;
}

static Prog *program_for(const Key *k) {
    for (int i = 0; i < nprogs; i++)
        if (!memcmp(&progs[i].key, k, sizeof *k)) return &progs[i];
    return build_prog(k);
}

/* ------------------------------------------------------------ draw state */
static void color4(uint32_t c, float *o) {
    o[0] = ((c >> 16) & 255) / 255.0f, o[1] = ((c >> 8) & 255) / 255.0f, o[2] = (c & 255) / 255.0f,
    o[3] = (c >> 24) / 255.0f;
}

static GLenum gl_cmp(uint32_t f) {
    static const GLenum m[9] = {GL_ALWAYS, GL_NEVER, GL_LESS, GL_EQUAL, GL_LEQUAL, GL_GREATER, GL_NOTEQUAL, GL_GEQUAL, GL_ALWAYS};
    return f < 9 ? m[f] : GL_ALWAYS;
}
static GLenum gl_blend(uint32_t b) {
    switch (b) {
    case 1: return GL_ZERO;
    case 2: return GL_ONE;
    case 3: return GL_SRC_COLOR;
    case 4: return GL_ONE_MINUS_SRC_COLOR;
    case 5: return GL_SRC_ALPHA;
    case 6: return GL_ONE_MINUS_SRC_ALPHA;
    case 7: return GL_DST_ALPHA;
    case 8: return GL_ONE_MINUS_DST_ALPHA;
    case 9: return GL_DST_COLOR;
    case 10: return GL_ONE_MINUS_DST_COLOR;
    case 11: return GL_SRC_ALPHA_SATURATE;
    default: return GL_ONE;
    }
}
static GLenum gl_stencil_op(uint32_t o) {
    static const GLenum m[9] = {GL_KEEP, GL_KEEP, GL_ZERO, GL_REPLACE, GL_INCR, GL_DECR, GL_INVERT, GL_INCR_WRAP, GL_DECR_WRAP};
    return o < 9 ? m[o] : GL_KEEP;
}

static void set_viewport(void) {
    int x = (int)dev.viewport[0], y = (int)dev.viewport[1], w = (int)dev.viewport[2], h = (int)dev.viewport[3];
    glViewport(x, gl.h - y - h, w, h);
    glDepthRangef(fbits(dev.viewport[4]), fbits(dev.viewport[5]));
}

static void apply_fixed_state(void) {
    const uint32_t *rs = dev.rs;
    set_viewport();
    if (rs[7]) {
        glEnable(GL_DEPTH_TEST);
        glDepthFunc(gl_cmp(rs[23]));
    } else {
        glDisable(GL_DEPTH_TEST);
    }
    glDepthMask(rs[14] ? GL_TRUE : GL_FALSE);
    if (rs[47]) { /* ZBIAS */
        glEnable(GL_POLYGON_OFFSET_FILL);
        glPolygonOffset(0.0f, -(float)rs[47]);
    } else {
        glDisable(GL_POLYGON_OFFSET_FILL);
    }
    /* D3D keeps the winding of NDC (both have y up in NDC), and GL's window y
     * runs the same way as NDC, so D3D's clockwise is GL's clockwise. */
    glFrontFace(GL_CCW);
    if (rs[22] == 1) glDisable(GL_CULL_FACE);
    else {
        glEnable(GL_CULL_FACE);
        glCullFace(rs[22] == 2 ? GL_BACK : GL_FRONT); /* D3DCULL_CW culls clockwise, D3DCULL_CCW counter-clockwise */
    }
    if (rs[27]) {
        glEnable(GL_BLEND);
        uint32_t s = rs[19], d = rs[20];
        if (s == 12) s = 5, d = 6;      /* BOTHSRCALPHA */
        else if (s == 13) s = 6, d = 5; /* BOTHINVSRCALPHA */
        glBlendFunc(gl_blend(s), gl_blend(d));
        static const GLenum ops[6] = {GL_FUNC_ADD, GL_FUNC_ADD, GL_FUNC_SUBTRACT, GL_FUNC_REVERSE_SUBTRACT, GL_MIN, GL_MAX};
        glBlendEquation(rs[171] < 6 ? ops[rs[171]] : GL_FUNC_ADD);
    } else {
        glDisable(GL_BLEND);
    }
    uint32_t cw = rs[168];
    glColorMask(cw & 1, (cw >> 1) & 1, (cw >> 2) & 1, (cw >> 3) & 1);
    if (rs[52]) {
        glEnable(GL_STENCIL_TEST);
        glStencilFunc(gl_cmp(rs[56]), (GLint)rs[57], rs[58]);
        glStencilOp(gl_stencil_op(rs[53]), gl_stencil_op(rs[54]), gl_stencil_op(rs[55]));
        glStencilMask(rs[59]);
    } else {
        glDisable(GL_STENCIL_TEST);
    }
    glDisable(GL_SCISSOR_TEST);
    /* debugging switches */
    static int nodepth = -1, nocull = -1;
    if (nodepth < 0) nodepth = getenv("RECOMP_D3D_NODEPTH") != NULL, nocull = getenv("RECOMP_D3D_NOCULL") != NULL;
    if (nodepth) glDisable(GL_DEPTH_TEST);
    if (nocull) glDisable(GL_CULL_FACE);
}

/* RECOMP_D3D_LOG=N: describe every draw of presented frame N. */
static int log_frame(void) {
    static int n = -1;
    if (n < 0) n = getenv("RECOMP_D3D_LOG") ? atoi(getenv("RECOMP_D3D_LOG")) : 0;
    return n && (int)dev.frames + 1 == n;
}
static void log_draw(const Layout *L, unsigned have, uint32_t count, const char *kind) {
    const uint32_t *rs = dev.rs;
    char st[256] = "";
    size_t o = 0;
    for (int t = 0; t < 8 && dev.tss[t][1] > 1; t++) {
        int ti = dev.texture[t];
        o += (size_t)snprintf(st + o, sizeof st - o, " [%d c%u(%x,%x) a%u(%x,%x) tex %s%ux%u fmt %#x tci %#x]", t,
                              dev.tss[t][1], dev.tss[t][2], dev.tss[t][3], dev.tss[t][4], dev.tss[t][5],
                              dev.tss[t][6], ti >= 0 ? "" : "none ", ti >= 0 ? objs[ti].width : 0,
                              ti >= 0 ? objs[ti].height : 0, ti >= 0 ? objs[ti].format : 0, dev.tss[t][11]);
        if (o >= sizeof st) break;
    }
    rt_log("[draw %u] %s n=%u vs=%#x%s attrs=%#x light=%u z=%u/%u/%u cull=%u blend=%u(%u,%u) atest=%u(%u) fog=%u%s\n",
           dev.draws, kind, count, dev.vshader, L->rhw ? " rhw" : "", have, rs[137], rs[7], rs[14], rs[23], rs[22],
           rs[27], rs[19], rs[20], rs[15], rs[24], rs[28], st);
}

static Prog *prepare(const Layout *L, unsigned have) {
    const uint32_t *rs = dev.rs;
    Key k;
    memset(&k, 0, sizeof k);
    k.rhw = (uint8_t)L->rhw;
    k.has_normal = (have & 2) != 0;
    k.has_diffuse = (have & 4) != 0;
    k.has_specular = (have & 8) != 0;
    k.lighting = !L->rhw && rs[137] && k.has_normal;
    k.specular_enable = rs[29] != 0;
    if (k.lighting) {
        k.local_viewer = rs[142] != 0;
        k.normalize = rs[143] != 0;
        for (int i = 0; i < 4; i++) {
            uint32_t m = rs[145 + i];
            if (!rs[141] || (m == 1 && !k.has_diffuse) || (m == 2 && !k.has_specular)) m = 0;
            k.mat_src[i] = (uint8_t)(m > 2 ? 0 : m);
        }
        for (int i = 0; i < 16 && k.nlights < 8; i++)
            if (dev.light_on[i]) k.light_type[k.nlights++] = (uint8_t)dev.light[i][0];
    }
    k.fog_enable = rs[28] != 0;
    if (k.fog_enable) {
        k.fog_table = rs[35] != 0;
        k.fog_mode = (uint8_t)(rs[35] ? rs[35] : rs[140]);
        if (L->rhw && !rs[35]) k.fog_mode = 0; /* vertex fog of pretransformed vertices: specular alpha */
    }
    k.alpha_func = rs[15] ? (uint8_t)rs[25] : 0;
    for (int t = 0; t < 8; t++) {
        const uint32_t *ts = dev.tss[t];
        if (ts[1] <= 1) break;
        k.nstages = (uint8_t)(t + 1);
        k.has_tex[t] = (uint8_t)bind_stage_texture(t);
        k.cop[t] = (uint8_t)ts[1], k.ca1[t] = (uint8_t)ts[2], k.ca2[t] = (uint8_t)ts[3], k.ca0[t] = (uint8_t)ts[26];
        k.aop[t] = (uint8_t)ts[4], k.aa1[t] = (uint8_t)ts[5], k.aa2[t] = (uint8_t)ts[6], k.aa0[t] = (uint8_t)ts[27];
        k.res[t] = (uint8_t)ts[28];
        k.tci[t] = (uint8_t)(ts[11] & 7);
        k.tcgen[t] = (uint8_t)((ts[11] >> 16) & 3);
        k.ttf[t] = (uint8_t)(ts[24] & 0xFF);          /* D3DTTFF count */
        k.tcsize[t] = (uint8_t)((ts[24] >> 8) & 1);   /* D3DTTFF_PROJECTED */
    }
    Prog *g = program_for(&k);
    glUseProgram(g->prog);

    float vp[4] = {(float)dev.viewport[0], (float)dev.viewport[1], (float)dev.viewport[2], (float)dev.viewport[3]};
    glUniform4fv(g->u_viewport, 1, vp);
    if (!L->rhw) {
        glUniformMatrix4fv(g->u_world, 1, GL_FALSE, (const float *)dev.transform[256]);
        glUniformMatrix4fv(g->u_view, 1, GL_FALSE, (const float *)dev.transform[2]);
        glUniformMatrix4fv(g->u_proj, 1, GL_FALSE, (const float *)dev.transform[3]);
        glUniformMatrix4fv(g->u_texm, 8, GL_FALSE, (const float *)dev.transform[16]);
    }
    if (k.lighting) {
        /* The camera position: the inverse of the view matrix applied to the origin. With a
         * rigid view matrix (rows 0-2 a rotation, row 3 the translation), it is -t * R^T. */
        const float *v = (const float *)dev.transform[2];
        float cam[3];
        for (int i = 0; i < 3; i++) cam[i] = -(v[12] * v[i * 4 + 0] + v[13] * v[i * 4 + 1] + v[14] * v[i * 4 + 2]);
        glUniform3fv(g->u_campos, 1, cam);
        float mat[20];
        for (int i = 0; i < 16; i++) mat[i] = fbits(dev.material[i]);
        mat[16] = fbits(dev.material[16]), mat[17] = mat[18] = mat[19] = 0;
        glUniform4fv(g->u_mat, 5, mat);
        float amb[4];
        color4(rs[139], amb);
        glUniform4fv(g->u_ambient, 1, amb);
        float lt[8 * 7 * 4];
        memset(lt, 0, sizeof lt);
        int n = 0;
        for (int i = 0; i < 16 && n < 8; i++) {
            if (!dev.light_on[i]) continue;
            const uint32_t *L8 = dev.light[i];
            float *o = lt + n * 28;
            for (int c = 0; c < 4; c++) o[c] = fbits(L8[1 + c]), o[4 + c] = fbits(L8[5 + c]), o[8 + c] = fbits(L8[9 + c]);
            o[12] = fbits(L8[13]), o[13] = fbits(L8[14]), o[14] = fbits(L8[15]), o[15] = fbits(L8[19]);
            o[16] = fbits(L8[16]), o[17] = fbits(L8[17]), o[18] = fbits(L8[18]);
            o[20] = fbits(L8[21]), o[21] = fbits(L8[22]), o[22] = fbits(L8[23]), o[23] = fbits(L8[20]);
            o[24] = cosf(fbits(L8[24]) / 2), o[25] = cosf(fbits(L8[25]) / 2);
            n++;
        }
        glUniform4fv(g->u_light, 8 * 7, lt);
    }
    float tf[4], fc[4];
    color4(rs[60], tf);
    glUniform4fv(g->u_tfactor, 1, tf);
    if (k.fog_enable) {
        float fog[4] = {fbits(rs[36]), fbits(rs[37]), fbits(rs[38]), 0};
        glUniform4fv(g->u_fog, 1, fog);
        color4(rs[34], fc);
        glUniform4fv(g->u_fogcolor, 1, fc);
    }
    if (k.alpha_func) glUniform1f(g->u_alpharef, (float)(rs[24] & 255));
    apply_fixed_state();
    return g;
}

/* ------------------------------------------------------------------ draws */
static GLenum gl_prim(uint32_t type, uint32_t prims, uint32_t *count) {
    switch (type) {
    case 1: *count = prims; return GL_POINTS;
    case 2: *count = prims * 2; return GL_LINES;
    case 3: *count = prims + 1; return GL_LINE_STRIP;
    case 4: *count = prims * 3; return GL_TRIANGLES;
    case 5: *count = prims + 2; return GL_TRIANGLE_STRIP;
    case 6: *count = prims + 2; return GL_TRIANGLE_FAN;
    default: *count = 0; return GL_TRIANGLES;
    }
}

static int begin_draw(Layout *L) {
    if (!current()) return 0;
    if (dev.render_target != dev.backbuffer) {
        if (!(gl.warned & 1)) rt_log("d3d8: drawing into a render target other than the back buffer is not drawn yet\n");
        gl.warned |= 1;
        return 0;
    }
    if (!current_layout(L)) {
        if (!(gl.warned & 2)) rt_log("d3d8: vertex format %#x not supported\n", dev.vshader);
        gl.warned |= 2;
        return 0;
    }
    glBindFramebuffer(GL_FRAMEBUFFER, gl.fbo);
    return 1;
}

void d3dgl_draw(uint32_t type, uint32_t start_vertex, uint32_t prim_count) {
    Layout L;
    if (!begin_draw(&L)) return;
    uint32_t count;
    GLenum mode = gl_prim(type, prim_count, &count);
    if (!count) return;
    unsigned have = set_vertex_input(&L, start_vertex, count, 0, 0);
    if (log_frame()) log_draw(&L, have, count, "draw");
    prepare(&L, have);
    glDrawArrays(mode, 0, (GLsizei)count);
}

static void draw_elements(GLenum mode, uint32_t count, const void *idx, int idx32, int32_t base) {
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, gl.ibo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, (GLsizeiptr)count * (idx32 ? 4 : 2), idx, GL_STREAM_DRAW);
    glDrawElementsBaseVertex(mode, (GLsizei)count, idx32 ? GL_UNSIGNED_INT : GL_UNSIGNED_SHORT, 0, base);
}

void d3dgl_draw_indexed(uint32_t type, uint32_t min_index, uint32_t num_vertices, uint32_t start_index,
                        uint32_t prim_count) {
    Layout L;
    if (!begin_draw(&L)) return;
    if (dev.indices < 0 || !objs[dev.indices].data) return;
    uint32_t count;
    GLenum mode = gl_prim(type, prim_count, &count);
    if (!count) return;
    const Obj *ib = &objs[dev.indices];
    int idx32 = ib->format == FMT_INDEX32;
    /* vertices base+min .. base+min+num; the indices are relative to base */
    unsigned have = set_vertex_input(&L, dev.base_vertex + min_index, num_vertices, 0, 0);
    if (log_frame()) log_draw(&L, have, count, "draw_indexed");
    prepare(&L, have);
    draw_elements(mode, count, G(ib->data + start_index * (idx32 ? 4u : 2u)), idx32, -(int32_t)min_index);
}

void d3dgl_draw_up(uint32_t type, uint32_t prim_count, uint32_t data, uint32_t stride) {
    Layout L;
    if (!begin_draw(&L)) return;
    uint32_t count;
    GLenum mode = gl_prim(type, prim_count, &count);
    if (!count) return;
    unsigned have = set_vertex_input(&L, 0, count, data, stride);
    if (log_frame()) log_draw(&L, have, count, "draw_up");
    prepare(&L, have);
    glDrawArrays(mode, 0, (GLsizei)count);
}

void d3dgl_draw_indexed_up(uint32_t type, uint32_t min_index, uint32_t num_vertices, uint32_t prim_count,
                           uint32_t indices, uint32_t index_format, uint32_t data, uint32_t stride) {
    Layout L;
    if (!begin_draw(&L)) return;
    uint32_t count;
    GLenum mode = gl_prim(type, prim_count, &count);
    if (!count) return;
    unsigned have = set_vertex_input(&L, min_index, num_vertices, data, stride);
    if (log_frame()) log_draw(&L, have, count, "draw_indexed_up");
    prepare(&L, have);
    draw_elements(mode, count, G(indices), index_format == FMT_INDEX32, -(int32_t)min_index);
}

/* ---------------------------------------------------------- clear, present */
void d3dgl_clear(uint32_t count, uint32_t rects, uint32_t flags, uint32_t color, float z, uint32_t stencil) {
    if (!current() || dev.render_target != dev.backbuffer) return;
    glBindFramebuffer(GL_FRAMEBUFFER, gl.fbo);
    GLbitfield mask = 0;
    if (flags & 1) { /* D3DCLEAR_TARGET */
        float c[4];
        color4(color, c);
        glClearColor(c[0], c[1], c[2], c[3]);
        glColorMask(1, 1, 1, 1);
        mask |= GL_COLOR_BUFFER_BIT;
    }
    if (flags & 2) { /* ZBUFFER */
        glClearDepthf(z);
        glDepthMask(GL_TRUE);
        mask |= GL_DEPTH_BUFFER_BIT;
    }
    if (flags & 4) { /* STENCIL */
        glClearStencil((GLint)stencil);
        glStencilMask(0xFF);
        mask |= GL_STENCIL_BUFFER_BIT;
    }
    /* Clear works on the viewport, or on the given rectangles inside it. */
    glEnable(GL_SCISSOR_TEST);
    int vx = (int)dev.viewport[0], vy = (int)dev.viewport[1], vw = (int)dev.viewport[2], vh = (int)dev.viewport[3];
    if (!count || !rects) {
        glScissor(vx, gl.h - vy - vh, vw, vh);
        glClear(mask);
    } else {
        for (uint32_t i = 0; i < count; i++) {
            int32_t l = (int32_t)ld32(rects + 16 * i), t = (int32_t)ld32(rects + 16 * i + 4);
            int32_t r = (int32_t)ld32(rects + 16 * i + 8), b = (int32_t)ld32(rects + 16 * i + 12);
            glScissor(l, gl.h - b, r - l, b - t);
            glClear(mask);
        }
    }
    glDisable(GL_SCISSOR_TEST);
}

const uint16_t *d3dgl_present(int *w, int *h) {
    if (!current()) return NULL;
    glBindFramebuffer(GL_FRAMEBUFFER, gl.fbo);
    glReadPixels(0, 0, gl.w, gl.h, GL_RGBA, GL_UNSIGNED_BYTE, gl.rgba);
    for (int y = 0; y < gl.h; y++) {
        const uint8_t *s = gl.rgba + (size_t)(gl.h - 1 - y) * gl.w * 4;
        uint16_t *d = gl.frame + (size_t)y * gl.w;
        for (int x = 0; x < gl.w; x++, s += 4)
            d[x] = (uint16_t)((s[0] >> 3) << 11 | (s[1] >> 2) << 5 | (s[2] >> 3));
    }
    *w = gl.w, *h = gl.h;
    return gl.frame;
}
#endif

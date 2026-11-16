/* A GL backend that draws: the game's OpenGL ES 1.1 calls made on a real GL.
 *
 * On iOS this is OpenGL ES 1.1 itself. On the Mac it's the legacy desktop GL
 * (2.1, a superset of ES 1.1 apart from fixed point) in an offscreen CGL
 * context with a SCREEN_W x SCREEN_H framebuffer object, so the host build can draw
 * without a window and save frames.
 *
 * What changes on the way:
 *   - arguments: floats arrive in core registers (softfp), fixed point (16.16)
 *     becomes float, pointers are guest addresses (G());
 *   - GL_FIXED client arrays (the game's vertex positions): desktop GL has no
 *     such type, so they are converted to float at each draw, over the index
 *     range the draw uses;
 *   - the game never binds a framebuffer: on the iPhone EGL's dummy did that.
 *     We bind ours when the context is made.
 *
 * The host backend is chosen with RT_GL=1 (the default stays the null
 * backend, which is faster for discovery runs). RT_GL_DUMP=n saves every nth
 * frame as frame_NNNNN.png in RT_GL_DUMP_DIR, by default "frames" next to the
 * save directory (build/frames when run from build/).
 */
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include <TargetConditionals.h>
#if TARGET_OS_IPHONE
#define GLES_SILENCE_DEPRECATION
#include <OpenGLES/ES1/gl.h>
#include <OpenGLES/ES1/glext.h>
#define GL_DESKTOP 0
#else
#define GL_SILENCE_DEPRECATION
#include <OpenGL/OpenGL.h>
#include <OpenGL/gl.h>
#include <OpenGL/glext.h>
#include <CoreFoundation/CoreFoundation.h>
#include <CoreGraphics/CoreGraphics.h>
#include <ImageIO/ImageIO.h>
#define GL_DESKTOP 1
#endif

#include "rt.h"

#ifndef GL_FIXED
#define GL_FIXED 0x140C
#endif

#define A(n) arg(c, n)
static inline float F(uint32_t bits) { float f; memcpy(&f, &bits, 4); return f; }
static inline float X(uint32_t fx) { return (float)(int32_t)fx / 65536.0f; }
#define FA(n) F(A(n))
#define XA(n) X(A(n))
static inline void *P(uint32_t a) { return a ? (void *)G(a) : NULL; }

int g_gl_real;   /* the real backend is active */

/* --------------------------------------------------------------- context */
#if GL_DESKTOP
static CGLContextObj ctx;
static GLuint fbo;
static int width = SCREEN_W, height = SCREEN_H;
#endif

void gl_real_make_current(void) {
#if GL_DESKTOP
    if (ctx) return;
    CGLPixelFormatAttribute attrs[] = {kCGLPFAAccelerated, kCGLPFAColorSize, 24, kCGLPFADepthSize, 24,
                                       kCGLPFAStencilSize, 8, 0};
    CGLPixelFormatObj pf;
    GLint n;
    if (CGLChoosePixelFormat(attrs, &pf, &n) || !pf || CGLCreateContext(pf, NULL, &ctx)) {
        rt_log("GL: no CGL context; drawing disabled");
        g_gl_real = 0;
        return;
    }
    CGLDestroyPixelFormat(pf);
    CGLSetCurrentContext(ctx);
    GLuint rb[2];
    glGenFramebuffersEXT(1, &fbo);
    glBindFramebufferEXT(GL_FRAMEBUFFER_EXT, fbo);
    glGenRenderbuffersEXT(2, rb);
    glBindRenderbufferEXT(GL_RENDERBUFFER_EXT, rb[0]);
    glRenderbufferStorageEXT(GL_RENDERBUFFER_EXT, GL_RGBA8, width, height);
    glFramebufferRenderbufferEXT(GL_FRAMEBUFFER_EXT, GL_COLOR_ATTACHMENT0_EXT, GL_RENDERBUFFER_EXT, rb[0]);
    glBindRenderbufferEXT(GL_RENDERBUFFER_EXT, rb[1]);
    glRenderbufferStorageEXT(GL_RENDERBUFFER_EXT, GL_DEPTH24_STENCIL8_EXT, width, height);
    glFramebufferRenderbufferEXT(GL_FRAMEBUFFER_EXT, GL_DEPTH_ATTACHMENT_EXT, GL_RENDERBUFFER_EXT, rb[1]);
    glFramebufferRenderbufferEXT(GL_FRAMEBUFFER_EXT, GL_STENCIL_ATTACHMENT_EXT, GL_RENDERBUFFER_EXT, rb[1]);
    rt_log("GL: %s, %s; framebuffer %dx%d status %#x", glGetString(GL_RENDERER), glGetString(GL_VERSION),
           width, height, glCheckFramebufferStatusEXT(GL_FRAMEBUFFER_EXT));
    glViewport(0, 0, width, height);
#endif
}

#if GL_DESKTOP
static void save_png(const char *path) {
    uint8_t *px = malloc((size_t)width * height * 4), *flip = malloc((size_t)width * height * 4);
    glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, px);
    for (int y = 0; y < height; y++)
        memcpy(flip + (size_t)y * width * 4, px + (size_t)(height - 1 - y) * width * 4, (size_t)width * 4);
    CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
    CGContextRef bc = CGBitmapContextCreate(flip, width, height, 8, width * 4, cs,
                                            kCGImageAlphaNoneSkipLast | kCGBitmapByteOrder32Big);
    CGImageRef img = CGBitmapContextCreateImage(bc);
    CFURLRef url = CFURLCreateFromFileSystemRepresentation(NULL, (const UInt8 *)path, (CFIndex)strlen(path), false);
    CGImageDestinationRef d = CGImageDestinationCreateWithURL(url, CFSTR("public.png"), 1, NULL);
    if (d) {
        CGImageDestinationAddImage(d, img, NULL);
        CGImageDestinationFinalize(d);
        CFRelease(d);
    }
    CFRelease(url);
    CGImageRelease(img);
    CGContextRelease(bc);
    CGColorSpaceRelease(cs);
    free(px);
    free(flip);
}
#endif

/* eglSwapBuffers: the frame is done. */
void gl_real_swap(uint32_t frame) {
#if GL_DESKTOP
    for (GLenum e; (e = glGetError()) != GL_NO_ERROR;) {
        static int n;
        if (n++ < 20) rt_log("GL error %#x before frame %u", e, frame);
    }
    static int every = -1;
    static const char *dir;
    if (every < 0) {
        const char *e = getenv("RT_GL_DUMP");
        every = e ? atoi(e) : 0;
        dir = getenv("RT_GL_DUMP_DIR");
        if (!dir) {   /* next to the save directory: main() has moved into the app */
            static char buf[1024];
            snprintf(buf, sizeof buf, "%s/../frames", g_save_dir);
            dir = buf;
        }
        if (every > 0) mkdir(dir, 0755);
    }
    glFlush();
    if (every > 0 && frame % (uint32_t)every == 0) {
        char path[1024];
        snprintf(path, sizeof path, "%s/frame_%05u.png", dir, frame);
        save_png(path);
    }
#else
    (void)frame;
#endif
}

/* -------------------------------------------------------- buffer objects */
/* The game puts level geometry in buffer objects. Each keeps a host copy of
 * its contents, for the GL_FIXED conversion and for reading indices. */
typedef struct { uint8_t *data; uint32_t size; } Buf;
static Buf *bufs;
static uint32_t nbufs;
static uint32_t bound_array, bound_element;   /* glBindBuffer */

static Buf *buf_of(uint32_t name) {
    if (!name) return NULL;
    if (name >= nbufs) {
        uint32_t n = name * 2 + 16;
        bufs = realloc(bufs, sizeof(Buf) * n);
        memset(bufs + nbufs, 0, sizeof(Buf) * (n - nbufs));
        nbufs = n;
    }
    return &bufs[name];
}
static void glGenBuffers_(CPU *c) {
    uint32_t n = A(0);
    GLuint *t = calloc(n ? n : 1, sizeof(GLuint));
    glGenBuffers((GLsizei)n, t);
    for (uint32_t k = 0; k < n; k++) st32(A(1) + 4 * k, t[k]);
    free(t);
}
static void glDeleteBuffers_(CPU *c) {
    uint32_t n = A(0);
    GLuint *t = calloc(n ? n : 1, sizeof(GLuint));
    for (uint32_t k = 0; k < n; k++) {
        t[k] = ld32(A(1) + 4 * k);
        Buf *b = buf_of(t[k]);
        if (b) { free(b->data); b->data = NULL; b->size = 0; }
        if (t[k] == bound_array) bound_array = 0;
        if (t[k] == bound_element) bound_element = 0;
    }
    glDeleteBuffers((GLsizei)n, t);
    free(t);
}
static void glBindBuffer_(CPU *c) {
    if (A(0) == GL_ARRAY_BUFFER) bound_array = A(1);
    else if (A(0) == GL_ELEMENT_ARRAY_BUFFER) bound_element = A(1);
    glBindBuffer(A(0), A(1));
}
static Buf *bound_buf(uint32_t target) {
    return buf_of(target == GL_ARRAY_BUFFER ? bound_array : target == GL_ELEMENT_ARRAY_BUFFER ? bound_element : 0);
}
static void glBufferData_(CPU *c) {   /* (target, size, data, usage) */
    uint32_t size = A(1);
    Buf *b = bound_buf(A(0));
    if (b) {
        b->data = realloc(b->data, size ? size : 1);
        b->size = size;
        if (A(2)) memcpy(b->data, G(A(2)), size);
        else memset(b->data, 0, size);
    }
    glBufferData(A(0), (GLsizeiptr)size, P(A(2)), A(3));
}
static void glBufferSubData_(CPU *c) {   /* (target, offset, size, data) */
    uint32_t off = A(1), size = A(2);
    Buf *b = bound_buf(A(0));
    if (b && off <= b->size && size <= b->size - off) memcpy(b->data + off, G(A(3)), size);
    glBufferSubData(A(0), (GLintptr)off, (GLsizeiptr)size, P(A(3)));
}

/* -------------------------------------------------------- client arrays */
/* Arrays as the game set them: ptr is a guest address, or an offset into
 * buffer buf when one was bound. Desktop GL gets GL_FIXED ones converted at
 * draw time; everything else goes straight through. */
typedef struct { GLint size; GLenum type; GLsizei stride; uint32_t ptr, buf; } Array;
enum { ARR_VERTEX, ARR_NORMAL, ARR_COLOR, ARR_TEX0, ARR_TEX1, ARR_N };
static Array arrays[ARR_N];
static int client_unit;   /* glClientActiveTexture */
#if GL_DESKTOP
static float *fixed_buf[ARR_N];
static size_t fixed_cap[ARR_N];
#endif

static void set_array(int k) {
    Array *a = &arrays[k];
    GLenum type = a->type;
    if (GL_DESKTOP && type == GL_FIXED) return;   /* converted at draw time */
    void *p = a->buf ? (void *)(uintptr_t)a->ptr : P(a->ptr);
    switch (k) {
    case ARR_VERTEX: glVertexPointer(a->size, type, a->stride, p); break;
    case ARR_NORMAL: glNormalPointer(type, a->stride, p); break;
    case ARR_COLOR: glColorPointer(a->size, type, a->stride, p); break;
    default: glTexCoordPointer(a->size, type, a->stride, p); break;
    }
}

#if GL_DESKTOP
/* The bytes an array starts at, wherever they live. */
static const uint8_t *array_bytes(const Array *a, uint32_t *avail) {
    if (a->buf) {
        Buf *b = buf_of(a->buf);
        if (!b || !b->data || a->ptr >= b->size) { *avail = 0; return NULL; }
        *avail = b->size - a->ptr;
        return b->data + a->ptr;
    }
    *avail = 0xffffffffu - a->ptr;
    return G(a->ptr);
}
#endif

static void convert_fixed(uint32_t count) {
#if GL_DESKTOP
    int unbound = 0;
    for (int k = 0; k < ARR_N; k++) {
        Array *a = &arrays[k];
        if (a->type != GL_FIXED || (!a->ptr && !a->buf)) continue;
        int n = k == ARR_NORMAL ? 3 : a->size;
        uint32_t stride = a->stride ? (uint32_t)a->stride : 4u * n, avail;
        const uint8_t *src = array_bytes(a, &avail);
        size_t need = (size_t)count * n;
        if (need > fixed_cap[k]) {
            fixed_cap[k] = need * 2;
            fixed_buf[k] = realloc(fixed_buf[k], fixed_cap[k] * sizeof(float));
        }
        float *out = fixed_buf[k];
        for (uint32_t v = 0; v < count; v++)
            for (int j = 0; j < n; j++) {
                uint32_t off = v * stride + 4 * j;
                int32_t x = 0;
                if (src && off + 4 <= avail) memcpy(&x, src + off, 4);
                out[v * n + j] = (float)x / 65536.0f;
            }
        if (!unbound++) glBindBuffer(GL_ARRAY_BUFFER, 0);   /* the floats are client memory */
        switch (k) {
        case ARR_VERTEX: glVertexPointer(n, GL_FLOAT, 0, out); break;
        case ARR_NORMAL: glNormalPointer(GL_FLOAT, 0, out); break;
        case ARR_COLOR: glColorPointer(n, GL_FLOAT, 0, out); break;
        default:
            glClientActiveTexture(GL_TEXTURE0 + (k - ARR_TEX0));
            glTexCoordPointer(n, GL_FLOAT, 0, out);
            glClientActiveTexture(GL_TEXTURE0 + client_unit);
            break;
        }
    }
    if (unbound) glBindBuffer(GL_ARRAY_BUFFER, bound_array);
#else
    (void)count;
#endif
}

static int any_fixed(void) {
    if (!GL_DESKTOP) return 0;
    for (int k = 0; k < ARR_N; k++)
        if (arrays[k].type == GL_FIXED && (arrays[k].ptr || arrays[k].buf)) return 1;
    return 0;
}

#define SET(k, size, type, stride, ptr) \
    do { arrays[k] = (Array){(GLint)(size), (type), (GLsizei)(stride), (ptr), bound_array}; set_array(k); } while (0)
static void glVertexPointer_(CPU *c) { SET(ARR_VERTEX, A(0), A(1), A(2), A(3)); }
static void glNormalPointer_(CPU *c) { SET(ARR_NORMAL, 3, A(0), A(1), A(2)); }
static void glColorPointer_(CPU *c) { SET(ARR_COLOR, A(0), A(1), A(2), A(3)); }
static void glTexCoordPointer_(CPU *c) { SET(ARR_TEX0 + (client_unit & 1), A(0), A(1), A(2), A(3)); }
#undef SET
static void glClientActiveTexture_(CPU *c) { client_unit = (int)(A(0) - GL_TEXTURE0); glClientActiveTexture(A(0)); }

static void glDrawArrays_(CPU *c) {   /* (mode, first, count) */
    if (any_fixed()) {
        /* convert from 0 so that `first` still indexes the converted arrays */
        convert_fixed(A(1) + A(2));
    }
    glDrawArrays(A(0), (GLint)A(1), (GLsizei)A(2));
}
static void glDrawElements_(CPU *c) { /* (mode, count, type, indices) */
    uint32_t count = A(1), type = A(2), ind = A(3);
    Buf *eb = buf_of(bound_element);
    if (any_fixed()) {
        uint32_t hi = 0, esize = type == GL_UNSIGNED_BYTE ? 1 : 2;
        const uint8_t *src = eb ? (eb->data && ind < eb->size ? eb->data + ind : NULL) : G(ind);
        uint32_t avail = eb ? (src ? eb->size - ind : 0) : 0xffffffffu;
        for (uint32_t k = 0; src && k < count && (k + 1) * esize <= avail; k++) {
            uint32_t v = esize == 1 ? src[k] : (uint32_t)(src[2 * k] | src[2 * k + 1] << 8);
            if (v > hi) hi = v;
        }
        convert_fixed(hi + 1);
    }
    glDrawElements(A(0), (GLsizei)count, type, eb ? (void *)(uintptr_t)ind : P(ind));
}

/* ------------------------------------------------------------- textures */
static void glTexImage2D_(CPU *c) {   /* (target, level, internal, w, h, border, format, type, pixels) */
    GLenum format = A(6);
    GLint internal = (GLint)A(2);
    if (getenv("RT_LOG_GL"))
        rt_log("glTexImage2D level %u internal %#x %ux%u format %#x type %#x pixels %08x", A(1), A(2), A(3), A(4),
               A(6), A(7), A(8));
    glTexImage2D(A(0), (GLint)A(1), internal, (GLsizei)A(3), (GLsizei)A(4), (GLint)A(5), format, A(7), P(A(8)));
}
static void glTexSubImage2D_(CPU *c) {   /* (target, level, x, y, w, h, format, type, pixels) */
    glTexSubImage2D(A(0), (GLint)A(1), (GLint)A(2), (GLint)A(3), (GLsizei)A(4), (GLsizei)A(5), A(6), A(7), P(A(8)));
}
void pvrtc_decode(const uint8_t *src, int w, int h, int two_bpp, uint8_t *out);   /* pvrtc.c */

static void glCompressedTexImage2D_(CPU *c) {   /* (target, level, internal, w, h, border, size, data) */
#if GL_DESKTOP
    /* PVRTC (0x8c00 RGB 4bpp, 0x8c01 RGB 2bpp, 0x8c02 RGBA 4bpp, 0x8c03 RGBA 2bpp):
     * decoded to RGBA8 here. Anything else is left empty. */
    uint32_t fmt = A(2);
    int w = (int)A(3), h = (int)A(4);
    if (fmt >= 0x8c00 && fmt <= 0x8c03 && w > 0 && h > 0 && A(7)) {
        uint8_t *px = malloc((size_t)w * h * 4);
        pvrtc_decode(G(A(7)), w, h, fmt & 1, px);
        glTexImage2D(A(0), (GLint)A(1), GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, px);
        free(px);
        return;
    }
    static int said;
    if (!said++) rt_log("GL: compressed texture %#x has no desktop format; left empty", fmt);
#else
    glCompressedTexImage2D(A(0), (GLint)A(1), A(2), (GLsizei)A(3), (GLsizei)A(4), (GLint)A(5), (GLsizei)A(6), P(A(7)));
#endif
}
static void glGenTextures_(CPU *c) {
    uint32_t n = A(0);
    GLuint *t = calloc(n ? n : 1, sizeof(GLuint));
    glGenTextures((GLsizei)n, t);
    for (uint32_t k = 0; k < n; k++) st32(A(1) + 4 * k, t[k]);
    free(t);
}
static void glDeleteTextures_(CPU *c) {
    uint32_t n = A(0);
    GLuint *t = calloc(n ? n : 1, sizeof(GLuint));
    for (uint32_t k = 0; k < n; k++) t[k] = ld32(A(1) + 4 * k);
    glDeleteTextures((GLsizei)n, t);
    free(t);
}
static void glBindTexture_(CPU *c) { glBindTexture(A(0), A(1)); }
static void glPixelStorei_(CPU *c) { glPixelStorei(A(0), (GLint)A(1)); }
static void glTexParameteri_(CPU *c) { glTexParameteri(A(0), A(1), (GLint)A(2)); }
static void glTexParameterf_(CPU *c) { glTexParameterf(A(0), A(1), FA(2)); }
/* glTexParameterx: every ES 1.1 pname but GENERATE_MIPMAP takes an enum, passed as is */
static void glTexParameterx_(CPU *c) { glTexParameteri(A(0), A(1), (GLint)A(2)); }

/* glTexEnvx: enums as is, the two scales as fixed point */
static void glTexEnvx_(CPU *c) {
    uint32_t p = A(1);
    if (p == 0x8573 /* RGB_SCALE */ || p == 0x0D1C /* ALPHA_SCALE */) glTexEnvf(A(0), p, XA(2));
    else glTexEnvi(A(0), p, (GLint)A(2));
}
static void glTexEnvi_(CPU *c) { glTexEnvi(A(0), A(1), (GLint)A(2)); }
static void glTexEnvf_(CPU *c) { glTexEnvf(A(0), A(1), FA(2)); }
static void glTexEnvxv_(CPU *c) {
    uint32_t p = A(1), v = A(2);
    if (p == GL_TEXTURE_ENV_COLOR) {
        GLfloat f[4] = {X(ld32(v)), X(ld32(v + 4)), X(ld32(v + 8)), X(ld32(v + 12))};
        glTexEnvfv(A(0), p, f);
    } else if (p == 0x8573 || p == 0x0D1C) {
        glTexEnvf(A(0), p, X(ld32(v)));
    } else {
        glTexEnvi(A(0), p, (GLint)ld32(v));
    }
}

/* -------------------------------------------------------------- matrices */
static void glMatrixMode_(CPU *c) { glMatrixMode(A(0)); }
static void glLoadIdentity_(CPU *c) { glLoadIdentity(); }
static void glLoadMatrixx_(CPU *c) {
    GLfloat m[16];
    for (int k = 0; k < 16; k++) m[k] = X(ld32(A(0) + 4 * k));
    glLoadMatrixf(m);
}
static void glLoadMatrixf_(CPU *c) {
    GLfloat m[16];
    memcpy(m, G(A(0)), sizeof m);
    glLoadMatrixf(m);
}
static void glMultMatrixx_(CPU *c) {
    GLfloat m[16];
    for (int k = 0; k < 16; k++) m[k] = X(ld32(A(0) + 4 * k));
    glMultMatrixf(m);
}
static void glMultMatrixf_(CPU *c) {
    GLfloat m[16];
    memcpy(m, G(A(0)), sizeof m);
    glMultMatrixf(m);
}
static void glPushMatrix_(CPU *c) { glPushMatrix(); }
static void glPopMatrix_(CPU *c) { glPopMatrix(); }
static void glRotatex_(CPU *c) { glRotatef(XA(0), XA(1), XA(2), XA(3)); }
static void glRotatef_(CPU *c) { glRotatef(FA(0), FA(1), FA(2), FA(3)); }
static void glScalex_(CPU *c) { glScalef(XA(0), XA(1), XA(2)); }
static void glScalef_(CPU *c) { glScalef(FA(0), FA(1), FA(2)); }
static void glTranslatex_(CPU *c) { glTranslatef(XA(0), XA(1), XA(2)); }
static void glTranslatef_(CPU *c) { glTranslatef(FA(0), FA(1), FA(2)); }
#if GL_DESKTOP
#define ORTHO glOrtho
#define FRUSTUM glFrustum
#define DEPTHRANGE glDepthRange
#define CLEARDEPTH glClearDepth
#else
#define ORTHO glOrthof
#define FRUSTUM glFrustumf
#define DEPTHRANGE glDepthRangef
#define CLEARDEPTH glClearDepthf
#endif
static void glOrthox_(CPU *c) { ORTHO(XA(0), XA(1), XA(2), XA(3), XA(4), XA(5)); }
static void glOrthof_(CPU *c) { ORTHO(FA(0), FA(1), FA(2), FA(3), FA(4), FA(5)); }
static void glFrustumx_(CPU *c) { FRUSTUM(XA(0), XA(1), XA(2), XA(3), XA(4), XA(5)); }
static void glFrustumf_(CPU *c) { FRUSTUM(FA(0), FA(1), FA(2), FA(3), FA(4), FA(5)); }

/* ----------------------------------------------------------------- state */
static void glEnable_(CPU *c) { glEnable(A(0)); }
static void glDisable_(CPU *c) { glDisable(A(0)); }
static void glEnableClientState_(CPU *c) { glEnableClientState(A(0)); }
static void glDisableClientState_(CPU *c) { glDisableClientState(A(0)); }
static void glActiveTexture_(CPU *c) { glActiveTexture(A(0)); }
static void glBlendFunc_(CPU *c) { glBlendFunc(A(0), A(1)); }
static void glShadeModel_(CPU *c) { glShadeModel(A(0)); }
static void glDepthFunc_(CPU *c) { glDepthFunc(A(0)); }
static void glDepthMask_(CPU *c) { glDepthMask((GLboolean)A(0)); }
static void glDepthRangex_(CPU *c) { DEPTHRANGE(XA(0), XA(1)); }
static void glDepthRangef_(CPU *c) { DEPTHRANGE(FA(0), FA(1)); }
static void glColorMask_(CPU *c) { glColorMask((GLboolean)A(0), (GLboolean)A(1), (GLboolean)A(2), (GLboolean)A(3)); }
static void glCullFace_(CPU *c) { glCullFace(A(0)); }
static void glFrontFace_(CPU *c) { glFrontFace(A(0)); }
static void glHint_(CPU *c) { glHint(A(0), A(1)); }
static void glAlphaFuncx_(CPU *c) { glAlphaFunc(A(0), XA(1)); }
static void glAlphaFunc_(CPU *c) { glAlphaFunc(A(0), FA(1)); }
static void glPolygonOffsetx_(CPU *c) { glPolygonOffset(XA(0), XA(1)); }
static void glPolygonOffset_(CPU *c) { glPolygonOffset(FA(0), FA(1)); }
static void glLineWidthx_(CPU *c) { glLineWidth(XA(0)); }
static void glPointSizex_(CPU *c) { glPointSize(XA(0)); }
static void glScissor_(CPU *c) { glScissor((GLint)A(0), (GLint)A(1), (GLsizei)A(2), (GLsizei)A(3)); }
static void glViewport_(CPU *c) { glViewport((GLint)A(0), (GLint)A(1), (GLsizei)A(2), (GLsizei)A(3)); }
static void glClear_(CPU *c) { glClear(A(0)); }
static void glClearColorx_(CPU *c) { glClearColor(XA(0), XA(1), XA(2), XA(3)); }
static void glClearColor_(CPU *c) { glClearColor(FA(0), FA(1), FA(2), FA(3)); }
static void glClearDepthx_(CPU *c) { CLEARDEPTH(XA(0)); }
static void glClearDepthf_(CPU *c) { CLEARDEPTH(FA(0)); }
static void glClearStencil_(CPU *c) { glClearStencil((GLint)A(0)); }
static void glStencilFunc_(CPU *c) { glStencilFunc(A(0), (GLint)A(1), A(2)); }
static void glStencilOp_(CPU *c) { glStencilOp(A(0), A(1), A(2)); }
static void glStencilMask_(CPU *c) { glStencilMask(A(0)); }
static void glColor4x_(CPU *c) { glColor4f(XA(0), XA(1), XA(2), XA(3)); }
static void glColor4f_(CPU *c) { glColor4f(FA(0), FA(1), FA(2), FA(3)); }
static void glColor4ub_(CPU *c) { glColor4ub((GLubyte)A(0), (GLubyte)A(1), (GLubyte)A(2), (GLubyte)A(3)); }
static void glNormal3x_(CPU *c) { glNormal3f(XA(0), XA(1), XA(2)); }
static void glFlush_(CPU *c) { glFlush(); }
static void glFinish_(CPU *c) { glFinish(); }
static void glReadPixels_(CPU *c) {   /* (x, y, w, h, format, type, pixels) */
    glReadPixels((GLint)A(0), (GLint)A(1), (GLsizei)A(2), (GLsizei)A(3), A(4), A(5), P(A(6)));
}

/* ------------------------------------------------- fog, lights, materials */
static int count_of(uint32_t pname) {
    switch (pname) {
    case GL_AMBIENT: case GL_DIFFUSE: case GL_SPECULAR: case GL_EMISSION: case GL_POSITION:
    case GL_AMBIENT_AND_DIFFUSE: case GL_FOG_COLOR: case GL_LIGHT_MODEL_AMBIENT:
        return 4;
    case GL_SPOT_DIRECTION:
        return 3;
    default:
        return 1;
    }
}
static void glFogx_(CPU *c) {
    if (A(0) == GL_FOG_MODE) glFogf(A(0), (GLfloat)A(1));
    else glFogf(A(0), XA(1));
}
static void glFogf_(CPU *c) { glFogf(A(0), FA(1)); }
static void glFogxv_(CPU *c) {
    uint32_t p = A(0), v = A(1);
    if (p == GL_FOG_MODE) { glFogf(p, (GLfloat)ld32(v)); return; }
    GLfloat f[4];
    for (int k = 0; k < count_of(p); k++) f[k] = X(ld32(v + 4 * k));
    glFogfv(p, f);
}
static void glFogfv_(CPU *c) {
    GLfloat f[4] = {0};
    memcpy(f, G(A(1)), 4 * (size_t)count_of(A(0)));
    glFogfv(A(0), f);
}
static void glLightx_(CPU *c) { glLightf(A(0), A(1), XA(2)); }
static void glLightxv_(CPU *c) {
    GLfloat f[4];
    for (int k = 0; k < count_of(A(1)); k++) f[k] = X(ld32(A(2) + 4 * k));
    glLightfv(A(0), A(1), f);
}
static void glLightfv_(CPU *c) {
    GLfloat f[4] = {0};
    memcpy(f, G(A(2)), 4 * (size_t)count_of(A(1)));
    glLightfv(A(0), A(1), f);
}
static void glLightModelx_(CPU *c) {
    if (A(0) == GL_LIGHT_MODEL_TWO_SIDE) glLightModelf(A(0), (GLfloat)A(1));
    else glLightModelf(A(0), XA(1));
}
static void glLightModelxv_(CPU *c) {
    GLfloat f[4];
    for (int k = 0; k < count_of(A(0)); k++) f[k] = X(ld32(A(1) + 4 * k));
    if (A(0) == GL_LIGHT_MODEL_TWO_SIDE) glLightModelf(A(0), (GLfloat)ld32(A(1)));
    else glLightModelfv(A(0), f);
}
static void glMaterialx_(CPU *c) { glMaterialf(A(0), A(1), XA(2)); }
static void glMaterialxv_(CPU *c) {
    GLfloat f[4];
    for (int k = 0; k < count_of(A(1)); k++) f[k] = X(ld32(A(2) + 4 * k));
    glMaterialfv(A(0), A(1), f);
}
static void glMaterialfv_(CPU *c) {
    GLfloat f[4] = {0};
    memcpy(f, G(A(2)), 4 * (size_t)count_of(A(1)));
    glMaterialfv(A(0), A(1), f);
}

typedef struct { const char *name; ImportFn fn; } RealFn;
static const RealFn real_fns[] = {
#define R(n) {#n, n##_}
    R(glGenBuffers), R(glDeleteBuffers), R(glBindBuffer), R(glBufferData), R(glBufferSubData),
    R(glVertexPointer), R(glNormalPointer), R(glColorPointer), R(glTexCoordPointer), R(glClientActiveTexture),
    R(glDrawArrays), R(glDrawElements),
    R(glTexImage2D), R(glTexSubImage2D), R(glCompressedTexImage2D), R(glGenTextures), R(glDeleteTextures),
    R(glBindTexture), R(glPixelStorei), R(glTexParameteri), R(glTexParameterf), R(glTexParameterx),
    R(glTexEnvx), R(glTexEnvi), R(glTexEnvf), R(glTexEnvxv),
    R(glMatrixMode), R(glLoadIdentity), R(glLoadMatrixx), R(glLoadMatrixf), R(glMultMatrixx), R(glMultMatrixf),
    R(glPushMatrix), R(glPopMatrix), R(glRotatex), R(glRotatef), R(glScalex), R(glScalef), R(glTranslatex),
    R(glTranslatef), R(glOrthox), R(glOrthof), R(glFrustumx), R(glFrustumf),
    R(glEnable), R(glDisable), R(glEnableClientState), R(glDisableClientState), R(glActiveTexture),
    R(glBlendFunc), R(glShadeModel), R(glDepthFunc), R(glDepthMask), R(glDepthRangex), R(glDepthRangef),
    R(glColorMask), R(glCullFace), R(glFrontFace), R(glHint), R(glAlphaFuncx), R(glAlphaFunc),
    R(glPolygonOffsetx), R(glPolygonOffset), R(glLineWidthx), R(glPointSizex), R(glScissor), R(glViewport),
    R(glClear), R(glClearColorx), R(glClearColor), R(glClearDepthx), R(glClearDepthf), R(glClearStencil),
    R(glStencilFunc), R(glStencilOp), R(glStencilMask), R(glColor4x), R(glColor4f), R(glColor4ub),
    R(glNormal3x), R(glFlush), R(glFinish), R(glReadPixels),
    R(glFogx), R(glFogf), R(glFogxv), R(glFogfv), R(glLightx), R(glLightxv), R(glLightfv),
    R(glLightModelx), R(glLightModelxv), R(glMaterialx), R(glMaterialxv), R(glMaterialfv),
#undef R
};

ImportFn gl_real_lookup(const char *name) {
    for (size_t k = 0; k < sizeof real_fns / sizeof real_fns[0]; k++)
        if (!strcmp(real_fns[k].name, name)) return real_fns[k].fn;
    return NULL;
}

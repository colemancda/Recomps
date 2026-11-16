/* s3eGL, EGL and OpenGL ES.
 *
 * s3eGL: the loader's s3eGLGetInt (0x2aafc) is what IwGx asks first; a GL
 * version of 0x100 or more selects the hardware path.
 *
 * EGL: the loader has no real EGL. Its "Ideaworks3D (dummy iPhone impl)"
 * (0x460c8-0x46900) hands out fixed handles over EAGL; the game reaches it
 * through 34 stubs in alphabetical EGL 1.2 order (0x2ba6b0 and a second copy at
 * 0x2bc168). We answer the same way, with the same handles:
 *
 *   native window 0xa, display 0xb, surface 0xc, config 0xd, context 0xe
 *
 * GL: the stubs are named from the executable's indirect symbols. The host
 * build has no GL context, so its backend is null: queries get the answers of
 * the original iPhone (PowerVR MBX, OpenGL ES-CM 1.1), names count up, and
 * everything else does nothing. RT_LOG_GL=1 logs the first call of each GL
 * function that does nothing.
 */
#include <stdlib.h>
#include <string.h>

#include <TargetConditionals.h>

#include "rt.h"

#define NATIVE_WINDOW 0xau
#define DISPLAY       0xbu
#define SURFACE       0xcu
#define CONFIG        0xdu
#define CONTEXT       0xeu

#define EGL_SUCCESS       0x3000
#define EGL_BAD_DISPLAY   0x3008
#define EGL_BAD_CONFIG    0x3005
#define EGL_BAD_SURFACE   0x300d
#define EGL_BAD_PARAMETER 0x300c
#define EGL_NONE          0x3038

/* gl_real.c */
extern int g_gl_real;
ImportFn gl_real_lookup(const char *name);
void gl_real_make_current(void);
void gl_real_swap(uint32_t frame);

static void gl_backend_init(void) {
    static int done;
    if (done++) return;
#if TARGET_OS_IPHONE
    g_gl_real = 1;
#else
    const char *e = getenv("RT_GL");
    g_gl_real = e && *e && strcmp(e, "0");
#endif
    rt_log("GL backend: %s", g_gl_real ? "real" : "null");
}

static uint32_t egl_error = EGL_SUCCESS;
static uint32_t current_context;

/* ---------------------------------------------------------------- s3eGL */
/* 0x2aafc s3eGLGetInt(prop): 0 GL version, 1 EGL version (major << 8 | minor). */
static void s3eGLGetInt(CPU *c) {
    switch (arg(c, 0)) {
    case 0: ret32(c, 0x101); break;    /* OpenGL ES 1.1 */
    case 1: ret32(c, 0x103); break;    /* EGL 1.3 */
    default: ret32(c, (uint32_t)-1); break;
    }
}
/* 0x2ab4c s3eGLGetNativeWindow() */
static void s3eGLGetNativeWindow(CPU *c) { ret32(c, NATIVE_WINDOW); }
/* 0x2aaf4: returns 0 (the iPhone loader has nothing to do here). */
static void s3eGLNothing(CPU *c) { ret32(c, 0); }

/* ------------------------------------------------------------------ EGL */
static int egl_check_display(uint32_t d) {
    if (d == DISPLAY) return 1;
    egl_error = EGL_BAD_DISPLAY;
    return 0;
}
static void egl_true(CPU *c, uint32_t ok) {
    if (ok) egl_error = EGL_SUCCESS;
    ret32(c, ok ? 1 : 0);
}
static void eglFalse(CPU *c) { ret32(c, 0); }   /* the dummy's unsupported functions */

static void eglGetDisplay(CPU *c) {             /* (native): 0 or the native window */
    uint32_t n = arg(c, 0);
    if (n == 0 || n == NATIVE_WINDOW) ret32(c, DISPLAY);
    else { egl_error = EGL_BAD_PARAMETER; ret32(c, 0); }
}
static void eglGetError(CPU *c) {
    ret32(c, egl_error);
    egl_error = EGL_SUCCESS;
}
static void eglInitialize(CPU *c) {             /* (dpy, *major, *minor) */
    if (!egl_check_display(arg(c, 0))) { ret32(c, 0); return; }
    if (arg(c, 1)) st32(arg(c, 1), 1);
    if (arg(c, 2)) st32(arg(c, 2), 3);
    egl_true(c, 1);
}
static void eglTerminate(CPU *c) { egl_true(c, egl_check_display(arg(c, 0))); }
static void eglGetConfigs(CPU *c) {             /* (dpy, *configs, size, *num) */
    if (!egl_check_display(arg(c, 0))) { ret32(c, 0); return; }
    if (arg(c, 1) && arg(c, 2)) st32(arg(c, 1), CONFIG);
    if (arg(c, 3)) st32(arg(c, 3), 1);
    egl_true(c, 1);
}
static void eglChooseConfig(CPU *c) {           /* (dpy, attribs, *configs, size, *num) */
    if (!egl_check_display(arg(c, 0))) { ret32(c, 0); return; }
    uint32_t list = arg(c, 1);
    if (list && g_log_imports)
        for (uint32_t k = 0; k < 64 && ld32(list + 8 * k) != EGL_NONE; k++)
            rt_log("eglChooseConfig %#x = %d", ld32(list + 8 * k), (int32_t)ld32(list + 8 * k + 4));
    if (arg(c, 2) && arg(c, 3)) st32(arg(c, 2), CONFIG);
    if (arg(c, 4)) st32(arg(c, 4), 1);
    egl_true(c, 1);
}
static void eglGetConfigAttrib(CPU *c) {        /* (dpy, config, attr, *value) */
    if (!egl_check_display(arg(c, 0))) { ret32(c, 0); return; }
    if (arg(c, 1) != CONFIG) { egl_error = EGL_BAD_CONFIG; ret32(c, 0); return; }
    int32_t v;
    switch (arg(c, 2)) {
    case 0x3020: v = 16; break;          /* BUFFER_SIZE */
    case 0x3021: v = 0; break;           /* ALPHA_SIZE */
    case 0x3022: v = 5; break;           /* BLUE_SIZE */
    case 0x3023: v = 6; break;           /* GREEN_SIZE */
    case 0x3024: v = 5; break;           /* RED_SIZE */
    case 0x3025: v = 24; break;          /* DEPTH_SIZE */
    case 0x3026: v = 8; break;           /* STENCIL_SIZE */
    case 0x3027: v = EGL_NONE; break;    /* CONFIG_CAVEAT */
    case 0x3028: v = 1; break;           /* CONFIG_ID */
    case 0x3029: v = 0; break;           /* LEVEL */
    case 0x302a: case 0x302c: v = 1024; break;   /* MAX_PBUFFER_HEIGHT, _WIDTH */
    case 0x302b: v = 1024 * 1024; break; /* MAX_PBUFFER_PIXELS */
    case 0x3031: case 0x3032: v = 0; break;      /* SAMPLES, SAMPLE_BUFFERS */
    case 0x3033: v = 4; break;           /* SURFACE_TYPE: WINDOW_BIT */
    case 0x3040: v = 1; break;           /* RENDERABLE_TYPE: OPENGL_ES_BIT */
    default: v = 0; break;
    }
    if (arg(c, 3)) st32(arg(c, 3), (uint32_t)v);
    egl_true(c, 1);
}
static void eglCreateWindowSurface(CPU *c) { ret32(c, egl_check_display(arg(c, 0)) ? SURFACE : 0); }
static void eglDestroySurface(CPU *c) {
    egl_true(c, egl_check_display(arg(c, 0)) && arg(c, 1) == SURFACE);
}
static void eglCreateContext(CPU *c) {          /* (dpy, config, share, attribs) */
    if (!egl_check_display(arg(c, 0))) { ret32(c, 0); return; }
    if (arg(c, 1) != CONFIG) { egl_error = EGL_BAD_CONFIG; ret32(c, 0); return; }
    egl_error = EGL_SUCCESS;
    ret32(c, CONTEXT);
}
static void eglDestroyContext(CPU *c) {
    egl_true(c, egl_check_display(arg(c, 0)) && arg(c, 1) == CONTEXT);
}
static void eglMakeCurrent(CPU *c) {            /* (dpy, draw, read, ctx) */
    if (!egl_check_display(arg(c, 0))) { ret32(c, 0); return; }
    current_context = arg(c, 3);
    gl_backend_init();
    if (g_gl_real && current_context) gl_real_make_current();
    egl_true(c, 1);
}
static void eglGetCurrentContext(CPU *c) { ret32(c, current_context); }
static void eglGetCurrentDisplay(CPU *c) { ret32(c, DISPLAY); }
static void eglGetCurrentSurface(CPU *c) { ret32(c, SURFACE); }
static void eglQueryString(CPU *c) {            /* (dpy, name) */
    static uint32_t vendor, version, extensions;
    if (!vendor) {
        vendor = guest_strdup("Ideaworks3D (dummy iPhone impl)");
        version = guest_strdup("1.3");
        extensions = guest_strdup("");
    }
    switch (arg(c, 1)) {
    case 0x3053: ret32(c, vendor); break;
    case 0x3054: ret32(c, version); break;
    case 0x3055: ret32(c, extensions); break;
    default: ret32(c, 0); break;
    }
}
static void eglSwapBuffers(CPU *c) {            /* (dpy, surface) */
    static uint32_t frames;
    if (!egl_check_display(arg(c, 0)) || arg(c, 1) != SURFACE) { ret32(c, 0); return; }
    static uint32_t every;
    if (!every) { const char *e = getenv("RT_FRAME_LOG"); every = e && atoi(e) > 0 ? (uint32_t)atoi(e) : 600; }
    if (++frames <= 3 || frames % every == 0) rt_log("eglSwapBuffers: frame %u", frames);
    if (g_gl_real) gl_real_swap(frames);
    input_frame(frames);
    egl_true(c, 1);
}
/* (name): the stub of the GL import of that name, with any OES suffix dropped
 * (the executable imports the GLES 2 names: glBindFramebuffer for
 * glBindFramebufferOES). The game calls it through a register, which reaches
 * the stub like any other call to one. */
static void eglGetProcAddress(CPU *c) {
    const char *name = gstr(arg(c, 0));
    char buf[128];
    uint32_t stub = 0;
    if (name && strlen(name) < sizeof buf) {
        stub = import_stub_by_name(name);
        size_t n = strlen(name);
        if (!stub && n > 3 && !strcmp(name + n - 3, "OES")) {
            memcpy(buf, name, n - 3);
            buf[n - 3] = 0;
            stub = import_stub_by_name(buf);
        }
    }
    if (!stub) rt_log("eglGetProcAddress(\"%s\"): none", name ? name : "");
    ret32(c, stub);
}
static void eglTrue(CPU *c) { egl_true(c, 1); }

/* ------------------------------------------------------------- GL (null) */
static uint32_t gl_string(uint32_t name) {
    static uint32_t s[4];
    static const char *text[4] = {
        "Imagination Technologies",
        "PowerVR MBX",
        "OpenGL ES-CM 1.1",
        "GL_OES_blend_subtract GL_OES_compressed_paletted_texture GL_OES_depth24 GL_OES_draw_texture "
        "GL_OES_framebuffer_object GL_OES_mapbuffer GL_OES_matrix_palette GL_OES_point_size_array "
        "GL_OES_point_sprite GL_OES_read_format GL_OES_rgb8_rgba8 GL_OES_texture_mirrored_repeat "
        "GL_EXT_texture_filter_anisotropic GL_EXT_texture_lod_bias GL_IMG_read_format "
        "GL_IMG_texture_compression_pvrtc GL_IMG_texture_format_BGRA8888",
    };
    if (name < 0x1f00 || name > 0x1f03) return 0;
    uint32_t k = name - 0x1f00;
    if (!s[k]) s[k] = guest_strdup(text[k]);
    return s[k];
}
/* 0x47e40: the loader's glGetString wrapper. */
static void glGetString_(CPU *c) { ret32(c, gl_string(arg(c, 0))); }

static uint32_t gl_bound_texture, gl_viewport[4] = {0, 0, SCREEN_W, SCREEN_H};

static int gl_integer(uint32_t pname, int32_t *v) {
    switch (pname) {
    case 0x0ba2: memcpy(v, gl_viewport, 16); return 4;           /* VIEWPORT */
    case 0x0d31: v[0] = 8; return 1;                             /* MAX_LIGHTS */
    case 0x0d32: v[0] = 6; return 1;                             /* MAX_CLIP_PLANES */
    case 0x0d33: v[0] = 1024; return 1;                          /* MAX_TEXTURE_SIZE */
    case 0x0d36: v[0] = 16; return 1;                            /* MAX_MODELVIEW_STACK_DEPTH */
    case 0x0d38: case 0x0d39: v[0] = 2; return 1;                /* MAX_PROJECTION/TEXTURE_STACK_DEPTH */
    case 0x0d3a: v[0] = 1024; v[1] = 1024; return 2;             /* MAX_VIEWPORT_DIMS */
    case 0x0d50: v[0] = 4; return 1;                             /* SUBPIXEL_BITS */
    case 0x0d52: v[0] = 5; return 1;                             /* RED_BITS */
    case 0x0d53: v[0] = 6; return 1;                             /* GREEN_BITS */
    case 0x0d54: v[0] = 5; return 1;                             /* BLUE_BITS */
    case 0x0d55: v[0] = 0; return 1;                             /* ALPHA_BITS */
    case 0x0d56: v[0] = 24; return 1;                            /* DEPTH_BITS */
    case 0x0d57: v[0] = 8; return 1;                             /* STENCIL_BITS */
    case 0x8069: v[0] = (int32_t)gl_bound_texture; return 1;     /* TEXTURE_BINDING_2D */
    case 0x84e2: v[0] = 2; return 1;                             /* MAX_TEXTURE_UNITS */
    case 0x86a2: v[0] = 4; return 1;                             /* NUM_COMPRESSED_TEXTURE_FORMATS */
    case 0x86a3: v[0] = 0x8c00; v[1] = 0x8c01; v[2] = 0x8c02; v[3] = 0x8c03; return 4;   /* PVRTC */
    case 0x8b9a: v[0] = 0x1401; return 1;                        /* IMPLEMENTATION_COLOR_READ_TYPE: UNSIGNED_BYTE */
    case 0x8b9b: v[0] = 0x1908; return 1;                        /* IMPLEMENTATION_COLOR_READ_FORMAT: RGBA */
    case 0x8ca6: v[0] = 1; return 1;                             /* FRAMEBUFFER_BINDING_OES */
    case 0x84e8: v[0] = 1024; return 1;                          /* MAX_RENDERBUFFER_SIZE_OES */
    case 0x8869: v[0] = 8; return 1;                             /* MAX_VERTEX_ATTRIBS */
    case 0x8872: v[0] = 8; return 1;                             /* MAX_TEXTURE_IMAGE_UNITS */
    case 0x8b4a: case 0x8dfb: v[0] = 128; return 1;              /* MAX_VERTEX/FRAGMENT_UNIFORM_* */
    default: v[0] = 0; return 0;
    }
}
static void glGetIntegerv_(CPU *c) {
    int32_t v[4] = {0};
    int n = gl_integer(arg(c, 0), v);
    if (!n) { rt_log("glGetIntegerv(%#x): unknown, 0", arg(c, 0)); n = 1; }
    for (int k = 0; k < n; k++) st32(arg(c, 1) + 4 * k, (uint32_t)v[k]);
}
static void glGetBooleanv_(CPU *c) {
    int32_t v[4] = {0};
    int n = gl_integer(arg(c, 0), v);
    if (!n) { rt_log("glGetBooleanv(%#x): unknown, false", arg(c, 0)); n = 1; }
    for (int k = 0; k < n; k++) st8(arg(c, 1) + k, v[k] != 0);
}
static void glGetError_(CPU *c) { ret32(c, 0); }

static uint32_t gl_next_name = 1;
static void gl_gen(CPU *c) {                     /* glGen*(n, *names) */
    for (uint32_t k = 0; k < arg(c, 0); k++) st32(arg(c, 1) + 4 * k, gl_next_name++);
}
static void gl_create(CPU *c) { ret32(c, gl_next_name++); }   /* glCreateShader/Program */
static void glBindTexture_(CPU *c) { gl_bound_texture = arg(c, 1); }
static void glViewport_(CPU *c) { for (int k = 0; k < 4; k++) gl_viewport[k] = arg(c, k); }
static void gl_get_iv_true(CPU *c) {             /* glGetShaderiv/glGetProgramiv(obj, pname, *v) */
    uint32_t p = arg(c, 1), v;
    switch (p) {
    case 0x8b81: case 0x8b82: case 0x8b83: v = 1; break;   /* COMPILE/LINK/VALIDATE_STATUS */
    default: v = 0; break;                               /* lengths and counts */
    }
    st32(arg(c, 2), v);
}
static void gl_get_info_log(CPU *c) {            /* (obj, size, *length, *log) */
    if (arg(c, 2)) st32(arg(c, 2), 0);
    if (arg(c, 3) && arg(c, 1)) st8(arg(c, 3), 0);
}
static void gl_location(CPU *c) {                /* glGetAttribLocation/glGetUniformLocation */
    static uint32_t next;
    ret32(c, next++ & 0xff);
}
static void glCheckFramebufferStatusOES_(CPU *c) { ret32(c, 0x8cd5); }   /* COMPLETE */
static void glIsEnabled_(CPU *c) { ret32(c, 0); }

/* Everything else: nothing to do. dispatch.c looks each stub up once and keeps
 * the answer, so RT_LOG_GL reports each null function once. */
static void gl_null(CPU *c) { ret32(c, 0); }

typedef struct { const char *name; ImportFn fn; } GLFn;
static const GLFn gl_fns[] = {
    {"glGetString", glGetString_},
    {"glGetIntegerv", glGetIntegerv_},
    {"glGetBooleanv", glGetBooleanv_},
    {"glGetError", glGetError_},
    {"glGenTextures", gl_gen},
    {"glGenBuffers", gl_gen},
    {"glCreateShader", gl_create},
    {"glCreateProgram", gl_create},
    {"glBindTexture", glBindTexture_},
    {"glViewport", glViewport_},
    {"glGetShaderiv", gl_get_iv_true},
    {"glGetProgramiv", gl_get_iv_true},
    {"glGetShaderInfoLog", gl_get_info_log},
    {"glGetProgramInfoLog", gl_get_info_log},
    {"glGetAttribLocation", gl_location},
    {"glGetUniformLocation", gl_location},
    {"glGenRenderbuffers", gl_gen},
    {"glGenFramebuffers", gl_gen},
    {"glCheckFramebufferStatus", glCheckFramebufferStatusOES_},
    {"glIsEnabled", glIsEnabled_},
};

static int log_gl = -1;
#define GL_NULL_LOGGED 1024
static const char *null_logged[GL_NULL_LOGGED];

/* The real backend draws what it can and the queries keep the iPhone's
 * answers, so the game takes the same paths either way. */
ImportFn gl_lookup(const char *name) {
    if (log_gl < 0) log_gl = getenv("RT_LOG_GL") != NULL;
    gl_backend_init();
    if (g_gl_real) {
        ImportFn f = gl_real_lookup(name);
        if (f) return f;
    }
    for (size_t k = 0; k < sizeof gl_fns / sizeof gl_fns[0]; k++)
        if (!strcmp(gl_fns[k].name, name)) return gl_fns[k].fn;
    if (log_gl) {
        for (int k = 0; k < GL_NULL_LOGGED; k++) {
            if (null_logged[k] == name) break;
            if (!null_logged[k]) {
                null_logged[k] = name;
                rt_log("GL: %s does nothing (null backend)", name);
                break;
            }
        }
    }
    return gl_null;
}

const ImportImpl g_gl_impls[] = {
    {0x0002aaf4, "s3eGLNothing", s3eGLNothing},
    {0x0002aafc, "s3eGLGetInt", s3eGLGetInt},
    {0x0002ab4c, "s3eGLGetNativeWindow", s3eGLGetNativeWindow},
    {0x00047e40, "glGetString", glGetString_},
    /* EGL 1.2, in stub order */
    {0x00046200, "eglBindAPI", eglFalse},
    {0x00046334, "eglBindTexImage", eglFalse},
    {0x000461c0, "eglChooseConfig", eglChooseConfig},
    {0x000463c4, "eglCopyBuffers", eglFalse},
    {0x000463f8, "eglCreateContext", eglCreateContext},
    {0x00046210, "eglCreatePbufferFromClientBuffer", eglFalse},
    {0x00046308, "eglCreatePbufferSurface", eglFalse},
    {0x00046300, "eglCreatePixmapSurface", eglFalse},
    {0x000462f8, "eglCreateWindowSurface", eglCreateWindowSurface},
    {0x0004634c, "eglDestroyContext", eglDestroyContext},
    {0x00046310, "eglDestroySurface", eglDestroySurface},
    {0x00046220, "eglGetConfigAttrib", eglGetConfigAttrib},
    {0x00046180, "eglGetConfigs", eglGetConfigs},
    {0x00046388, "eglGetCurrentContext", eglGetCurrentContext},
    {0x000463a4, "eglGetCurrentDisplay", eglGetCurrentDisplay},
    {0x0004639c, "eglGetCurrentSurface", eglGetCurrentSurface},
    {0x000460e0, "eglGetDisplay", eglGetDisplay},
    {0x000460d0, "eglGetError", eglGetError},
    {0x0004645c, "eglGetProcAddress", eglGetProcAddress},
    {0x00046108, "eglInitialize", eglInitialize},
    {0x00046378, "eglMakeCurrent", eglMakeCurrent},
    {0x00046208, "eglQueryAPI", eglFalse},
    {0x000463ac, "eglQueryContext", eglFalse},
    {0x00046138, "eglQueryString", eglQueryString},
    {0x00046324, "eglQuerySurface", eglFalse},
    {0x0004633c, "eglReleaseTexImage", eglFalse},
    {0x000461f8, "eglReleaseThread", eglTrue},
    {0x0004632c, "eglSurfaceAttrib", eglFalse},
    {0x000463cc, "eglSwapBuffers", eglSwapBuffers},
    {0x00046344, "eglSwapInterval", eglTrue},
    {0x00046888, "eglTerminate", eglTerminate},
    {0x00046218, "eglWaitClient", eglTrue},
    {0x000463b4, "eglWaitGL", eglTrue},
    {0x000463bc, "eglWaitNative", eglTrue},
};
const uint32_t g_gl_impl_count = sizeof g_gl_impls / sizeof g_gl_impls[0];

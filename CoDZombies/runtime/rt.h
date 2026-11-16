/* Runtime internals shared by the runtime's source files. */
#ifndef CODZ_RT_H
#define CODZ_RT_H

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>

#include "cpu.h"
#include "imports.h"

/* Guest address space layout (the image itself sits below 0x00a1b000). */
#define GUEST_STACK_TOP  0x0f800000u   /* main thread stack: 8 MB below this */
#define GUEST_STACK_SIZE 0x00800000u
#define GUEST_HEAP_LO    0x10000000u   /* heap for s3eMalloc and friends */
#define GUEST_HEAP_HI    0x30000000u
#define GUEST_SENTINEL   0xffff0000u   /* lr of the outermost call: returning here ends it */

/* The screen. The game is landscape only (UIInterfaceOrientationLandscapeRight).
 * On the iPhone the loader reported the portrait 320x480 device and IwGx turned
 * its picture a quarter turn by the blit direction; we report a landscape
 * surface and draw into a landscape framebuffer, as a landscape view gives on
 * iOS today. */
#define SCREEN_W 480
#define SCREEN_H 320

/* ------------------------------------------------------------- logging */
extern int g_log_imports;   /* RT_LOG_IMPORTS: log every import call */
void rt_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void rt_fatal(CPU *c, const char *fmt, ...) __attribute__((format(printf, 2, 3), noreturn));
void rt_dump(CPU *c);

/* -------------------------------------------------------------- memory */
void mem_init(void);
uint32_t guest_alloc(uint32_t size);
uint32_t guest_realloc(uint32_t p, uint32_t size);
void guest_free(uint32_t p);
uint32_t guest_alloc_size(uint32_t p);
uint32_t guest_strdup(const char *s);
static inline char *gstr(uint32_t a) { return a ? (char *)G(a) : NULL; }

/* -------------------------------------------------------------- loader */
/* Copies the executable's segments into guest memory and builds the function table. */
void load_image(const char *exe);

/* -------------------------------------------------------------- config */
void config_init(void);
const char *config_get(const char *group, const char *key);
int config_get_int(const char *group, const char *key, int32_t *out);

/* ---------------------------------------------------------------- files */
extern const char *g_app_dir;   /* the .app bundle: rom:// and relative paths */
extern const char *g_save_dir;  /* writable: ram:// */
/* Host path for a guest path (drive prefixes stripped); NULL if it can't be mapped. */
const char *host_path(const char *guest, char *buf, size_t size, int for_write);

/* ------------------------------------------------------------- imports */
typedef void (*ImportFn)(CPU *c);
typedef struct ImportImpl {
    uint32_t impl;         /* loader implementation address it replaces (s3e) */
    const char *name;      /* our name for it */
    ImportFn fn;
} ImportImpl;
extern const ImportImpl g_s3e_impls[];       /* s3e.c */
extern const uint32_t g_s3e_impl_count;
extern const ImportImpl g_compress_impls[];  /* compress.c */
extern const uint32_t g_compress_impl_count;
extern const ImportImpl g_gl_impls[];        /* gl.c: s3eGL, EGL */
extern const uint32_t g_gl_impl_count;
void imports_init(void);
/* Stub address of the GL import with this name, or 0. */
uint32_t import_stub_by_name(const char *name);

/* Arguments of an import: r0-r3, then the guest stack. */
static inline uint32_t arg(CPU *c, int n) { return n < 4 ? c->r[n] : ld32(c->r[13] + 4 * (n - 4)); }
static inline void ret32(CPU *c, uint32_t v) { c->r[0] = v; }
static inline void ret64(CPU *c, uint64_t v) { c->r[0] = (uint32_t)v; c->r[1] = (uint32_t)(v >> 32); }

/* Calls a guest function with up to 7 arguments (callbacks), returning r0. */
uint32_t guest_call(CPU *c, uint32_t fn, int nargs, ...);

#endif

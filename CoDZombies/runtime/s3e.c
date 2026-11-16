/* The s3e API, natively. Each entry replaces the loader function at `impl`
 * (see docs/S3E_BOUNDARY.md and build/s3e_boundary.tsv); the names are ours,
 * from reading that implementation and the Marmalade API it belongs to. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>

#include "rt.h"

enum { S3E_RESULT_SUCCESS = 0, S3E_RESULT_ERROR = 1 };
#define S3E_CONFIG_STRING_MAX 128

/* ---------------------------------------------------------------- memory */
/* A user memory manager: guest malloc, realloc and free the game provides
 * (it wraps the base allocator below with its own bookkeeping). */
static uint32_t user_malloc, user_realloc, user_free;

/* 0x2e718 s3eMemorySetUserMemMgr(const s3eMemoryUsrMgr *mgr): NULL resets to
 * the default; all three functions must be set. */
static void s3eMemorySetUserMemMgr(CPU *c) {
    uint32_t mgr = arg(c, 0);
    if (!mgr) {
        user_malloc = user_realloc = user_free = 0;
        ret32(c, S3E_RESULT_SUCCESS);
        return;
    }
    uint32_t m = ld32(mgr), r = ld32(mgr + 4), f = ld32(mgr + 8);
    if (!m || !r || !f) {
        ret32(c, S3E_RESULT_ERROR);
        return;
    }
    user_malloc = m;
    user_realloc = r;
    user_free = f;
    rt_log("user memory manager: malloc %08x realloc %08x free %08x", m, r, f);
    ret32(c, S3E_RESULT_SUCCESS);
}

/* 0x2ed9c s3eMallocBase(int size): from the current heap. */
static void s3eMallocBase(CPU *c) { ret32(c, guest_alloc(arg(c, 0))); }

/* 0x2f16c s3eReallocBase(void *p, int size) */
static void s3eReallocBase(CPU *c) { ret32(c, guest_realloc(arg(c, 0), arg(c, 1))); }

/* 0x2e098 s3eFreeBase(void *p) */
static void s3eFreeBase(CPU *c) { guest_free(arg(c, 0)); }

/* ---------------------------------------------------------------- config */
/* 0x19b10 s3eConfigGetString(const char *group, const char *name, char *value) */
static void s3eConfigGetString(CPU *c) {
    const char *v = config_get(gstr(arg(c, 0)), gstr(arg(c, 1)));
    if (!v || !arg(c, 2)) {
        ret32(c, S3E_RESULT_ERROR);
        return;
    }
    snprintf(gstr(arg(c, 2)), S3E_CONFIG_STRING_MAX, "%s", v);
    ret32(c, S3E_RESULT_SUCCESS);
}

/* 0x19cb0 s3eConfigGetInt(const char *group, const char *name, int *value) */
static void s3eConfigGetInt(CPU *c) {
    int32_t v;
    if (!arg(c, 2) || !config_get_int(gstr(arg(c, 0)), gstr(arg(c, 1)), &v)) {
        ret32(c, S3E_RESULT_ERROR);
        return;
    }
    st32(arg(c, 2), (uint32_t)v);
    ret32(c, S3E_RESULT_SUCCESS);
}

/* ----------------------------------------------------------------- files */
/* 0x277a8 s3eFileCheckExists(const char *filename) */
static void s3eFileCheckExists(CPU *c) {
    char buf[1024];
    const char *p = host_path(gstr(arg(c, 0)), buf, sizeof buf, 0);
    struct stat st;
    ret32(c, p && stat(p, &st) == 0 && S_ISREG(st.st_mode));
}

/* 0x24ef8 s3eFileOpenFromMemory(void *buffer, uint32 len), 0x25f30
 * s3eFileRead(void *buf, uint32 elemSize, uint32 count, s3eFile *f),
 * 0x27cdc s3eFileSeek(s3eFile *f, int32 offset, s3eFileSeekOrigin origin),
 * 0x24db4 s3eFileTell(s3eFile *f), 0x254e8 s3eFileClose(s3eFile *f).
 *
 * A guest s3eFile* is a small guest allocation (so it's a unique non-null
 * pointer); the host side lives in `files`, found by that address. */
typedef struct HostFile {
    uint32_t handle;      /* guest address, 0 when the slot is free */
    FILE *fp;             /* a host file, or */
    uint32_t mem, len;    /* a guest memory buffer */
    uint32_t pos;
} HostFile;
static HostFile files[64];

static HostFile *file_of(uint32_t h) {
    if (!h) return NULL;
    for (int k = 0; k < 64; k++) if (files[k].handle == h) return &files[k];
    return NULL;
}

static HostFile *file_new(void) {
    for (int k = 0; k < 64; k++) {
        if (!files[k].handle) {
            memset(&files[k], 0, sizeof files[k]);
            files[k].handle = guest_alloc(16);
            return &files[k];
        }
    }
    return NULL;
}

static void s3eFileOpenFromMemory(CPU *c) {
    uint32_t buf = arg(c, 0), len = arg(c, 1);
    HostFile *f = buf ? file_new() : NULL;
    if (!f) { ret32(c, 0); return; }
    f->mem = buf;
    f->len = len;
    ret32(c, f->handle);
}

/* 0x277a0 s3eFileOpen(const char *filename, const char *mode): stdio modes;
 * reads from the save directory or the bundle, writes to the save directory. */
static void s3eFileOpen(CPU *c) {
    const char *name = gstr(arg(c, 0)), *mode = gstr(arg(c, 1));
    if (!name || !mode) { ret32(c, 0); return; }
    int write = strchr(mode, 'w') || strchr(mode, 'a') || strchr(mode, '+');
    char buf[1024];
    const char *path = host_path(name, buf, sizeof buf, write);
    FILE *fp = path ? fopen(path, mode) : NULL;
    if (!fp) {
        rt_log("s3eFileOpen(\"%s\", \"%s\"): not found", name, mode);
        ret32(c, 0);
        return;
    }
    HostFile *f = file_new();
    if (!f) { fclose(fp); ret32(c, 0); return; }
    f->fp = fp;
    rt_log("s3eFileOpen(\"%s\", \"%s\") -> %s", name, mode, path);
    ret32(c, f->handle);
}

static void s3eFileRead(CPU *c) {
    uint32_t buf = arg(c, 0), size = arg(c, 1), count = arg(c, 2);
    HostFile *f = file_of(arg(c, 3));
    if (!f || !buf || !size) { ret32(c, 0); return; }
    if (f->fp) {
        ret32(c, (uint32_t)fread(G(buf), size, count, f->fp));
        return;
    }
    uint32_t avail = f->pos < f->len ? f->len - f->pos : 0;
    uint32_t n = count;
    if ((uint64_t)n * size > avail) n = avail / size;
    memcpy(G(buf), G(f->mem + f->pos), (size_t)n * size);
    f->pos += n * size;
    ret32(c, n);
}

/* 0x25de4 s3eFileGetChar(s3eFile *f): the next byte, or -1 at the end. */
static void s3eFileGetChar(CPU *c) {
    HostFile *f = file_of(arg(c, 0));
    int ch = -1;
    if (f && f->fp) ch = fgetc(f->fp);
    else if (f && f->pos < f->len) ch = ld8(f->mem + f->pos++);
    ret32(c, (uint32_t)(ch < 0 ? -1 : ch));
}

static void s3eFileSeek(CPU *c) {
    HostFile *f = file_of(arg(c, 0));
    int32_t off = (int32_t)arg(c, 1);
    uint32_t origin = arg(c, 2);   /* S3E_FILESEEK_SET, CUR, END */
    if (!f || origin > 2) { ret32(c, S3E_RESULT_ERROR); return; }
    if (f->fp) {
        static const int whence[] = {SEEK_SET, SEEK_CUR, SEEK_END};
        ret32(c, fseek(f->fp, off, whence[origin]) ? S3E_RESULT_ERROR : S3E_RESULT_SUCCESS);
        return;
    }
    int64_t base = origin == 0 ? 0 : origin == 1 ? f->pos : f->len;
    int64_t p = base + off;
    if (p < 0 || p > f->len) { ret32(c, S3E_RESULT_ERROR); return; }
    f->pos = (uint32_t)p;
    ret32(c, S3E_RESULT_SUCCESS);
}

static void s3eFileTell(CPU *c) {
    HostFile *f = file_of(arg(c, 0));
    if (!f) { ret32(c, (uint32_t)-1); return; }
    ret32(c, f->fp ? (uint32_t)ftell(f->fp) : f->pos);
}

static void s3eFileClose(CPU *c) {
    HostFile *f = file_of(arg(c, 0));
    if (!f) { ret32(c, S3E_RESULT_ERROR); return; }
    if (f->fp) fclose(f->fp);
    guest_free(f->handle);
    f->handle = 0;
    ret32(c, S3E_RESULT_SUCCESS);
}

/* ---------------------------------------------------------------- timers */
/* 0x3ae20 s3eTimerGetMs(): milliseconds since start, 64-bit. */
static void s3eTimerGetMs(CPU *c) {
    static struct timespec t0;
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    if (!t0.tv_sec && !t0.tv_nsec) t0 = t;
    ret64(c, (uint64_t)(t.tv_sec - t0.tv_sec) * 1000 + (uint64_t)((t.tv_nsec - t0.tv_nsec) / 1000000));
}

/* 0x3a974 s3eTimerGetUTC(): milliseconds since 1970 (the loader uses gettimeofday). */
static void s3eTimerGetUTC(CPU *c) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    ret64(c, (uint64_t)tv.tv_sec * 1000 + (uint64_t)tv.tv_usec / 1000);
}

/* ---------------------------------------------------------------- device */
/* 0x1ed28 s3eDeviceGetInt(s3eDeviceProperty): the iPhone values from the
 * loader's table at 0x457b4. Unsupported properties are -1. */
static void s3eDeviceGetInt(CPU *c) {
    int32_t v;
    switch (arg(c, 0)) {
    case 0: v = 14; break;       /* S3E_DEVICE_OS: iPhone */
    case 1: v = 0x2ee0; break;
    case 8: v = 5; break;
    case 10: v = 100; break;     /* battery level */
    case 11: v = 0; break;
    case 16: v = 1; break;
    default:
        rt_log("s3eDeviceGetInt(%u): not known, -1", arg(c, 0));
        v = -1;
    }
    ret32(c, (uint32_t)v);
}

/* 0x21c98 s3eExtGet(const char *name, void *funcs, int size): no extensions. */
static void s3eExtGet(CPU *c) {
    rt_log("extension %s: not available", gstr(arg(c, 0)));
    ret32(c, S3E_RESULT_ERROR);
}

/* ----------------------------------------------------------- callbacks */
/* The loader's callback registry, 0x16e3c(device, kind, id, fn, userdata):
 * 0x170fc registers for device 13 and 0x171ec for device 2 (id, fn, userdata). */
typedef struct { uint32_t device, id, fn, userdata; } Callback;
static Callback callbacks[64];
static int ncallbacks;

static void register_callback(CPU *c, uint32_t device) {
    if (ncallbacks < 64) callbacks[ncallbacks++] = (Callback){device, arg(c, 0), arg(c, 1), arg(c, 2)};
    rt_log("callback: device %u id %u fn %08x userdata %08x", device, arg(c, 0), arg(c, 1), arg(c, 2));
    ret32(c, S3E_RESULT_SUCCESS);
}
static void s3eRegisterDevice13(CPU *c) { register_callback(c, 13); }
static void s3eRegisterDevice2(CPU *c) { register_callback(c, 2); }

/* ----------------------------------------------------------------- debug */
/* 0x1e068 s3eDebugErrorShow(s3eMessageType type, const char *text) */
static void s3eDebugErrorShow(CPU *c) {
    rt_log("game error (type %u): %s", arg(c, 0), arg(c, 1) ? gstr(arg(c, 1)) : "(null)");
    ret32(c, 0);   /* S3E_ERROR_SHOW_CONTINUE */
}

/* --------------------------------------------------------------- surface */
/* 0x39c3c s3eSurfacePtr(): the 320x480 RGB565 frame buffer, made on first use. */
static void s3eSurfacePtr(CPU *c) {
    static uint32_t surface;
    if (!surface) surface = guest_alloc(320 * 480 * 2);
    ret32(c, surface);
}

/* 0x39c6c s3eSurfaceSetup(pixelType, ...) */
static void s3eSurfaceSetup(CPU *c) {
    rt_log("s3eSurfaceSetup(%#x, %u, %u, %u)", arg(c, 0), arg(c, 1), arg(c, 2), arg(c, 3));
    ret32(c, S3E_RESULT_SUCCESS);
}

const ImportImpl g_s3e_impls[] = {
    {0x000170fc, "s3eRegisterDevice13", s3eRegisterDevice13},
    {0x000171ec, "s3eRegisterDevice2", s3eRegisterDevice2},
    {0x0001e068, "s3eDebugErrorShow", s3eDebugErrorShow},
    {0x0001ed28, "s3eDeviceGetInt", s3eDeviceGetInt},
    {0x00021c98, "s3eExtGet", s3eExtGet},
    {0x00024db4, "s3eFileTell", s3eFileTell},
    {0x00024ef8, "s3eFileOpenFromMemory", s3eFileOpenFromMemory},
    {0x000254e8, "s3eFileClose", s3eFileClose},
    {0x00025de4, "s3eFileGetChar", s3eFileGetChar},
    {0x00025f30, "s3eFileRead", s3eFileRead},
    {0x00027cdc, "s3eFileSeek", s3eFileSeek},
    {0x00039c3c, "s3eSurfacePtr", s3eSurfacePtr},
    {0x00039c6c, "s3eSurfaceSetup", s3eSurfaceSetup},
    {0x0003a974, "s3eTimerGetUTC", s3eTimerGetUTC},
    {0x0003ae20, "s3eTimerGetMs", s3eTimerGetMs},
    {0x00019b10, "s3eConfigGetString", s3eConfigGetString},
    {0x00019cb0, "s3eConfigGetInt", s3eConfigGetInt},
    {0x000277a0, "s3eFileOpen", s3eFileOpen},
    {0x000277a8, "s3eFileCheckExists", s3eFileCheckExists},
    {0x0002e098, "s3eFreeBase", s3eFreeBase},
    {0x0002e718, "s3eMemorySetUserMemMgr", s3eMemorySetUserMemMgr},
    {0x0002ed9c, "s3eMallocBase", s3eMallocBase},
    {0x0002f16c, "s3eReallocBase", s3eReallocBase},
};
const uint32_t g_s3e_impl_count = sizeof g_s3e_impls / sizeof g_s3e_impls[0];

/* The s3e API, natively. Each entry replaces the loader function at `impl`
 * (see docs/S3E_BOUNDARY.md and build/s3e_boundary.tsv); the names are ours,
 * from reading that implementation and the Marmalade API it belongs to. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>
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
static int log_config = -1;   /* RT_LOG_CONFIG: log every lookup and its result */

static void s3eConfigGetString(CPU *c) {
    const char *v = config_get(gstr(arg(c, 0)), gstr(arg(c, 1)));
    if (log_config < 0) log_config = getenv("RT_LOG_CONFIG") != NULL;
    if (log_config) rt_log("config [%s] %s -> %s", gstr(arg(c, 0)), gstr(arg(c, 1)), v ? v : "(none)");
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
    int ok = arg(c, 2) && config_get_int(gstr(arg(c, 0)), gstr(arg(c, 1)), &v);
    if (log_config < 0) log_config = getenv("RT_LOG_CONFIG") != NULL;
    if (log_config) {
        if (ok) rt_log("config [%s] %s -> %d", gstr(arg(c, 0)), gstr(arg(c, 1)), v);
        else rt_log("config [%s] %s -> (none)", gstr(arg(c, 0)), gstr(arg(c, 1)));
    }
    if (!ok) {
        ret32(c, S3E_RESULT_ERROR);
        return;
    }
    st32(arg(c, 2), (uint32_t)v);
    ret32(c, S3E_RESULT_SUCCESS);
}

/* ----------------------------------------------------------------- files */
/* An s3eFile* handed to the game is a small guest allocation (a unique non-null
 * pointer); the host side lives in `files`, found by that address. A file is a
 * host FILE, a guest memory buffer (s3eFileOpenFromMemory), or a file of a user
 * file system the game registered (its derbh archives), reached through the
 * game's own callbacks. */

/* s3eFileUserFileSys, as the loader's s3eFileAddUserFileSys (0x29b54) checks it
 * (the first nine are required) and as the game fills it in. */
enum {
    UFS_OPEN = 0x00,         /* void *(const char *name, const char *mode) */
    UFS_READ = 0x04,         /* uint32 (void *buf, uint32 size, uint32 count, void *h) */
    UFS_EOF = 0x08,          /* int (void *h) */
    UFS_SEEK = 0x0c,         /* s3eResult (void *h, int32 offset, origin) */
    UFS_TELL = 0x10,         /* int32 (void *h) */
    UFS_CLOSE = 0x14,        /* s3eResult (void *h) */
    UFS_LIST_DIR = 0x18,     /* void *(const char *dir): a listing handle, or NULL */
    UFS_LIST_NEXT = 0x1c,    /* s3eResult (void *list, char *name, int size) */
    UFS_LIST_CLOSE = 0x20,   /* (void *list) */
    UFS_SIZE = 0x2c          /* 11 words; 0x24 and 0x28 are optional and unused by the game */
};
static uint32_t user_fs[8];  /* guest copies of the registered structs */
static int nuser_fs;

typedef struct HostFile {
    uint32_t handle;      /* guest address, 0 when the slot is free */
    FILE *fp;             /* a host file, or */
    uint32_t mem, len;    /* a guest memory buffer, or */
    uint32_t ufs, uh;     /* a user file system and its handle */
    uint32_t pos;
} HostFile;
static HostFile files[64];
static uint32_t scratch;  /* one guest byte for s3eFileGetChar on user files */

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
    rt_log("out of file handles");
    return NULL;
}

static uint32_t ufs_call(CPU *c, HostFile *f, uint32_t slot, int nargs, uint32_t a0, uint32_t a1, uint32_t a2,
                         uint32_t a3) {
    return guest_call(c, ld32(f->ufs + slot), nargs, a0, a1, a2, a3);
}

/* 0x29b54 s3eFileAddUserFileSys(const s3eFileUserFileSys *fs) */
static void s3eFileAddUserFileSys(CPU *c) {
    uint32_t fs = arg(c, 0);
    for (uint32_t k = 0; k <= UFS_LIST_CLOSE; k += 4) {
        if (!fs || !ld32(fs + k)) { ret32(c, S3E_RESULT_ERROR); return; }
    }
    if (nuser_fs == 8) { ret32(c, S3E_RESULT_ERROR); return; }
    uint32_t copy = guest_alloc(UFS_SIZE);
    memcpy(G(copy), G(fs), UFS_SIZE);
    user_fs[nuser_fs++] = copy;
    rt_log("user file system %d: open %08x read %08x eof %08x seek %08x tell %08x close %08x list %08x",
           nuser_fs - 1, ld32(copy), ld32(copy + 4), ld32(copy + 8), ld32(copy + 12), ld32(copy + 16),
           ld32(copy + 20), ld32(copy + 24));
    ret32(c, S3E_RESULT_SUCCESS);
}

/* 0x277a8 s3eFileCheckExists(const char *filename): a user file system has no
 * exists callback, so its file exists if it opens; then the host. */
static void s3eFileCheckExists(CPU *c) {
    uint32_t name = arg(c, 0);
    static uint32_t rb;
    if (!rb) rb = guest_strdup("rb");
    for (int k = nuser_fs - 1; k >= 0; k--) {
        uint32_t h = guest_call(c, ld32(user_fs[k] + UFS_OPEN), 2, name, rb);
        if (h) {
            guest_call(c, ld32(user_fs[k] + UFS_CLOSE), 1, h);
            ret32(c, 1);
            return;
        }
    }
    char buf[1024];
    const char *p = host_path(gstr(name), buf, sizeof buf, 0);
    struct stat st;
    ret32(c, p && stat(p, &st) == 0 && S_ISREG(st.st_mode));
}

/* 0x24ef8 s3eFileOpenFromMemory(void *buffer, uint32 len) */
static void s3eFileOpenFromMemory(CPU *c) {
    uint32_t buf = arg(c, 0), len = arg(c, 1);
    HostFile *f = buf ? file_new() : NULL;
    if (!f) { ret32(c, 0); return; }
    f->mem = buf;
    f->len = len;
    ret32(c, f->handle);
}

/* 0x277a0 s3eFileOpen(const char *filename, const char *mode): user file
 * systems first (reading only), then the save directory or the bundle. */
static void s3eFileOpen(CPU *c) {
    uint32_t gname = arg(c, 0), gmode = arg(c, 1);
    const char *name = gstr(gname), *mode = gstr(gmode);
    if (!name || !mode) { ret32(c, 0); return; }
    int write = strchr(mode, 'w') || strchr(mode, 'a') || strchr(mode, '+');
    for (int k = nuser_fs - 1; k >= 0 && !write; k--) {
        uint32_t h = guest_call(c, ld32(user_fs[k] + UFS_OPEN), 2, gname, gmode);
        if (h) {
            HostFile *f = file_new();
            if (!f) { ret32(c, 0); return; }
            f->ufs = user_fs[k];
            f->uh = h;
            rt_log("s3eFileOpen(\"%s\", \"%s\") -> user file system %d", name, mode, k);
            ret32(c, f->handle);
            return;
        }
    }
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

/* 0x25f30 s3eFileRead(void *buf, uint32 elemSize, uint32 count, s3eFile *f) */
static void s3eFileRead(CPU *c) {
    uint32_t buf = arg(c, 0), size = arg(c, 1), count = arg(c, 2);
    HostFile *f = file_of(arg(c, 3));
    if (!f || !buf || !size) { ret32(c, 0); return; }
    if (f->ufs) { ret32(c, ufs_call(c, f, UFS_READ, 4, buf, size, count, f->uh)); return; }
    if (f->fp) { ret32(c, (uint32_t)fread(G(buf), size, count, f->fp)); return; }
    uint32_t avail = f->pos < f->len ? f->len - f->pos : 0;
    uint32_t n = count;
    if ((uint64_t)n * size > avail) n = avail / size;
    memcpy(G(buf), G(f->mem + f->pos), (size_t)n * size);
    f->pos += n * size;
    ret32(c, n);
}

/* 0x259d0 s3eFileWrite(const void *buf, uint32 elemSize, uint32 count, s3eFile *f):
 * host files only (user file systems and memory files are read-only). */
static void s3eFileWrite(CPU *c) {
    uint32_t buf = arg(c, 0), size = arg(c, 1), count = arg(c, 2);
    HostFile *f = file_of(arg(c, 3));
    if (!f || !f->fp || !buf || !size) { ret32(c, 0); return; }
    ret32(c, (uint32_t)fwrite(G(buf), size, count, f->fp));
}

/* 0x25de4 s3eFileGetChar(s3eFile *f): the next byte, or -1 at the end. */
static void s3eFileGetChar(CPU *c) {
    HostFile *f = file_of(arg(c, 0));
    int ch = -1;
    if (f && f->ufs) {
        if (!scratch) scratch = guest_alloc(4);
        if (ufs_call(c, f, UFS_READ, 4, scratch, 1, 1, f->uh) == 1) ch = ld8(scratch);
    } else if (f && f->fp) {
        ch = fgetc(f->fp);
    } else if (f && f->pos < f->len) {
        ch = (int)ld8(f->mem + f->pos++);
    }
    ret32(c, (uint32_t)(ch < 0 ? -1 : ch));
}

/* 0x27cdc s3eFileSeek(s3eFile *f, int32 offset, s3eFileSeekOrigin origin) */
static void s3eFileSeek(CPU *c) {
    HostFile *f = file_of(arg(c, 0));
    int32_t off = (int32_t)arg(c, 1);
    uint32_t origin = arg(c, 2);   /* S3E_FILESEEK_SET, CUR, END */
    if (!f || origin > 2) { ret32(c, S3E_RESULT_ERROR); return; }
    if (f->ufs) { ret32(c, ufs_call(c, f, UFS_SEEK, 3, f->uh, (uint32_t)off, origin, 0)); return; }
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

/* 0x24db4 s3eFileTell(s3eFile *f) */
static void s3eFileTell(CPU *c) {
    HostFile *f = file_of(arg(c, 0));
    if (!f) { ret32(c, (uint32_t)-1); return; }
    if (f->ufs) { ret32(c, ufs_call(c, f, UFS_TELL, 1, f->uh, 0, 0, 0)); return; }
    ret32(c, f->fp ? (uint32_t)ftell(f->fp) : f->pos);
}

/* 0x26488 s3eFileGetSize(s3eFile *f): -1 for a bad handle. A user file system
 * has no size callback: tell, seek to the end, tell, seek back. */
static void s3eFileGetSize(CPU *c) {
    HostFile *f = file_of(arg(c, 0));
    if (!f) { ret32(c, (uint32_t)-1); return; }
    if (f->ufs) {
        uint32_t here = ufs_call(c, f, UFS_TELL, 1, f->uh, 0, 0, 0);
        ufs_call(c, f, UFS_SEEK, 3, f->uh, 0, 2, 0);
        uint32_t size = ufs_call(c, f, UFS_TELL, 1, f->uh, 0, 0, 0);
        ufs_call(c, f, UFS_SEEK, 3, f->uh, here, 0, 0);
        ret32(c, size);
        return;
    }
    if (f->fp) {
        long here = ftell(f->fp);
        fseek(f->fp, 0, SEEK_END);
        long size = ftell(f->fp);
        fseek(f->fp, here, SEEK_SET);
        ret32(c, (uint32_t)size);
        return;
    }
    ret32(c, f->len);
}

/* 0x285b8 s3eFileCheckDirectoryExists(const char *dir) (via 0x2815c): the save
 * directory or the bundle. */
static void s3eFileCheckDirectoryExists(CPU *c) {
    char buf[1024];
    struct stat st;
    const char *p = host_path(gstr(arg(c, 0)), buf, sizeof buf, 0);
    ret64(c, p && stat(p, &st) == 0 && S_ISDIR(st.st_mode));
}

/* 0x27fb0 s3eFileMakeDirectory(const char *dir): in the save directory. */
static void s3eFileMakeDirectory(CPU *c) {
    char buf[1024];
    const char *p = host_path(gstr(arg(c, 0)), buf, sizeof buf, 1);
    ret32(c, p && (mkdir(p, 0755) == 0 || access(p, F_OK) == 0) ? S3E_RESULT_SUCCESS : S3E_RESULT_ERROR);
}

/* 0x27120 s3eFileDelete(const char *name): only files the game wrote. */
static void s3eFileDelete(CPU *c) {
    char buf[1024];
    const char *p = host_path(gstr(arg(c, 0)), buf, sizeof buf, 1);
    ret32(c, p && unlink(p) == 0 ? S3E_RESULT_SUCCESS : S3E_RESULT_ERROR);
}

/* 0x254e8 s3eFileClose(s3eFile *f) */
static void s3eFileClose(CPU *c) {
    HostFile *f = file_of(arg(c, 0));
    if (!f) { ret32(c, S3E_RESULT_ERROR); return; }
    if (f->ufs) ufs_call(c, f, UFS_CLOSE, 1, f->uh, 0, 0, 0);
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
static void s3eRegisterDevice24(CPU *c) { register_callback(c, 24); }   /* 0x45b54 */
static void s3eRegisterDevice6(CPU *c) { register_callback(c, 6); }     /* 0x2fe2c, the pointer range */

/* 0x1f068 s3eDeviceYield(int32 ms): where the loader delivers events. Nothing to
 * deliver yet. */
static void s3eDeviceYield(CPU *c) { ret64(c, 0); }

/* 0x3a964 -> 0x4fda0: 1 for an argument of 0, otherwise a Timer error and -1. */
static void s3eTimer3a964(CPU *c) { ret32(c, arg(c, 0) == 0 ? 1 : (uint32_t)-1); }

/* 0x1dea8, 0x1deb0: debugging hooks compiled out of the loader; always 0. */
static void s3eReturnZero(CPU *c) { ret32(c, 0); }

/* ----------------------------------------------------------------- sound */
/* 0x398bc s3eSoundGetInt(s3eSoundProperty), in Marmalade's property order. No
 * audio yet, so S3E_SOUND_AVAILABLE is 0; the rate matters even so, the game
 * divides by it. */
static void s3eSoundGetInt(CPU *c) {
    static const int32_t values[] = {
        256,    /* 0 S3E_SOUND_VOLUME */
        22050,  /* 1 S3E_SOUND_DEFAULT_FREQ */
        22050,  /* 2 S3E_SOUND_OUTPUT_FREQ */
        16,     /* 3 S3E_SOUND_NUM_CHANNELS */
        0,      /* 4 S3E_SOUND_USED_CHANNELS */
        0,      /* 5 S3E_SOUND_AVAILABLE */
        256,    /* 6 S3E_SOUND_VOLUME_DEFAULT */
    };
    uint32_t p = arg(c, 0);
    if (p < sizeof values / sizeof values[0]) { ret32(c, (uint32_t)values[p]); return; }
    if (p <= 13) { ret32(c, 0); return; }
    ret32(c, (uint32_t)-1);
}

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
    {0x0001dea8, "s3eDebugCompiledOut", s3eReturnZero},
    {0x0001deb0, "s3eDebugCompiledOut", s3eReturnZero},
    {0x0001e068, "s3eDebugErrorShow", s3eDebugErrorShow},
    {0x0001f068, "s3eDeviceYield", s3eDeviceYield},
    {0x0001ed28, "s3eDeviceGetInt", s3eDeviceGetInt},
    {0x00021c98, "s3eExtGet", s3eExtGet},
    {0x00024db4, "s3eFileTell", s3eFileTell},
    {0x00024ef8, "s3eFileOpenFromMemory", s3eFileOpenFromMemory},
    {0x000254e8, "s3eFileClose", s3eFileClose},
    {0x000259d0, "s3eFileWrite", s3eFileWrite},
    {0x00025de4, "s3eFileGetChar", s3eFileGetChar},
    {0x00025f30, "s3eFileRead", s3eFileRead},
    {0x00026488, "s3eFileGetSize", s3eFileGetSize},
    {0x00027cdc, "s3eFileSeek", s3eFileSeek},
    {0x00027fb0, "s3eFileMakeDirectory", s3eFileMakeDirectory},
    {0x000285b8, "s3eFileCheckDirectoryExists", s3eFileCheckDirectoryExists},
    {0x000398bc, "s3eSoundGetInt", s3eSoundGetInt},
    {0x00039c3c, "s3eSurfacePtr", s3eSurfacePtr},
    {0x00039c6c, "s3eSurfaceSetup", s3eSurfaceSetup},
    {0x0003a964, "s3eTimer3a964", s3eTimer3a964},
    {0x0003a974, "s3eTimerGetUTC", s3eTimerGetUTC},
    {0x0003ae20, "s3eTimerGetMs", s3eTimerGetMs},
    {0x00045b54, "s3eRegisterDevice24", s3eRegisterDevice24},
    {0x00019b10, "s3eConfigGetString", s3eConfigGetString},
    {0x00019cb0, "s3eConfigGetInt", s3eConfigGetInt},
    {0x00027120, "s3eFileDelete", s3eFileDelete},
    {0x000277a0, "s3eFileOpen", s3eFileOpen},
    {0x000277a8, "s3eFileCheckExists", s3eFileCheckExists},
    {0x00029b54, "s3eFileAddUserFileSys", s3eFileAddUserFileSys},
    {0x0002e098, "s3eFreeBase", s3eFreeBase},
    {0x0002fe2c, "s3eRegisterDevice6", s3eRegisterDevice6},
    {0x0002e718, "s3eMemorySetUserMemMgr", s3eMemorySetUserMemMgr},
    {0x0002ed9c, "s3eMallocBase", s3eMallocBase},
    {0x0002f16c, "s3eReallocBase", s3eReallocBase},
};
const uint32_t g_s3e_impl_count = sizeof g_s3e_impls / sizeof g_s3e_impls[0];

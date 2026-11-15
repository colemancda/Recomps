/* Small Win32 functions that need no real implementation: each answers as a
 * system with no such device or feature would, so the caller takes its normal
 * fallback path. Anything with real work behind it lives in its own file. */
#define _GNU_SOURCE
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "com.h"
#include "rt.h"

#define MMSYSERR_NODRIVER 6u
#define MMSYSERR_BADDEVICEID 2u

H(st_zero) { UNUSED; return 0; }
H(st_one) { UNUSED; return 1; }
H(st_nodriver) { UNUSED; return MMSYSERR_NODRIVER; }
H(st_baddevice) { UNUSED; return MMSYSERR_BADDEVICEID; }

/* ---------------------------------------------------------------- winmm */
H(mm_timeBeginPeriod) { UNUSED; return 0; } /* TIMERR_NOERROR */
H(mm_mciSendCommandA) { UNUSED; return 263; /* MCIERR_INVALID_DEVICE_NAME */ }
/* Multimedia file I/O (mmio): the game reads its WAV files with it. Unbuffered
 * reads of a host file, or of guest memory when MMIOINFO names FOURCC_MEM. The
 * RIFF helpers walk chunk headers as winmm does. */
#define FOURCC(a, b, c, d) ((uint32_t)(a) | (uint32_t)(b) << 8 | (uint32_t)(c) << 16 | (uint32_t)(d) << 24)
#define MMIO_FINDCHUNK 0x10
#define MMIO_FINDRIFF 0x20
#define MMIO_FINDLIST 0x40
#define MMIOERR_CANNOTOPEN 257
#define MMIOERR_CANNOTSEEK 263
#define MMIOERR_CHUNKNOTFOUND 265
typedef struct Mmio {
    int used;
    FILE *f;            /* a file, or */
    uint32_t mem, size; /* guest memory */
    uint32_t pos;
} Mmio;
static Mmio mmios[64];
static pthread_mutex_t mmio_mu = PTHREAD_MUTEX_INITIALIZER;
#define MMIO_HANDLE_BASE 0x4D4D0000u

static Mmio *mmio_of(uint32_t h) {
    uint32_t i = h - MMIO_HANDLE_BASE - 1;
    return i < RT_COUNTOF(mmios) && mmios[i].used ? &mmios[i] : NULL;
}

/* mmioOpen(name, MMIOINFO *, flags) */
static uint32_t mmio_open(const char *name, uint32_t info, uint32_t flags) {
    if (flags & 0x3) { /* MMIO_WRITE, MMIO_READWRITE */
        if (g_opt.verbose) rt_log("[winmm] mmioOpen(%s, flags %#x): writing not provided\n", name ? name : "", flags);
        if (info) st32(info + 12, MMIOERR_CANNOTOPEN);
        return 0;
    }
    Mmio m = {.used = 1};
    if (info && ld32(info + 4) == FOURCC('M', 'E', 'M', ' ')) {
        m.mem = ld32(info + 24);
        m.size = ld32(info + 20);
    } else if (name) {
        char host[1024];
        if (rt_resolve_path(name, host, sizeof host)) m.f = fopen(host, "rb");
        if (!m.f) {
            if (info) st32(info + 12, MMIOERR_CANNOTOPEN);
            return 0;
        }
    } else {
        if (info) st32(info + 12, MMIOERR_CANNOTOPEN);
        return 0;
    }
    pthread_mutex_lock(&mmio_mu);
    uint32_t h = 0;
    for (size_t i = 0; i < RT_COUNTOF(mmios) && !h; i++)
        if (!mmios[i].used) mmios[i] = m, h = MMIO_HANDLE_BASE + 1 + (uint32_t)i;
    pthread_mutex_unlock(&mmio_mu);
    if (!h) {
        if (m.f) fclose(m.f);
        if (info) st32(info + 12, MMIOERR_CANNOTOPEN);
        rt_log("[winmm] mmioOpen: too many open files\n");
        return 0;
    }
    if (info) st32(info + 12, 0), st32(info + 68, h);
    return h;
}
H(mm_mmioOpenA) {
    UNUSED;
    return mmio_open(a[0] ? rt_gstr(a[0]) : NULL, a[1], a[2]);
}
H(mm_mmioOpenW) {
    UNUSED;
    char name[1024];
    if (a[0]) rt_get_wstr(a[0], name, sizeof name);
    return mmio_open(a[0] ? name : NULL, a[1], a[2]);
}
H(mm_mmioClose) {
    UNUSED;
    pthread_mutex_lock(&mmio_mu);
    Mmio *m = mmio_of(a[0]);
    if (m) {
        if (m->f) fclose(m->f);
        m->used = 0;
    }
    pthread_mutex_unlock(&mmio_mu);
    return 0;
}

/* Reads up to n bytes at the position into guest memory; returns the count. */
static uint32_t mmio_read(Mmio *m, uint32_t dst, uint32_t n) {
    if (m->f) {
        if (fseek(m->f, (long)m->pos, SEEK_SET) != 0) return 0;
        size_t got = fread(G(dst), 1, n, m->f);
        m->pos += (uint32_t)got;
        return (uint32_t)got;
    }
    uint32_t left = m->pos < m->size ? m->size - m->pos : 0;
    if (n > left) n = left;
    memmove(G(dst), G(m->mem + m->pos), n);
    m->pos += n;
    return n;
}
static int mmio_read_host(Mmio *m, void *dst, uint32_t n) {
    if (m->f) {
        if (fseek(m->f, (long)m->pos, SEEK_SET) != 0 || fread(dst, 1, n, m->f) != n) return 0;
    } else {
        if (m->pos > m->size || m->size - m->pos < n) return 0;
        memcpy(dst, G(m->mem + m->pos), n);
    }
    m->pos += n;
    return 1;
}
static uint32_t mmio_size(Mmio *m) {
    if (!m->f) return m->size;
    long cur = ftell(m->f);
    fseek(m->f, 0, SEEK_END);
    long end = ftell(m->f);
    fseek(m->f, cur, SEEK_SET);
    return end < 0 ? 0 : (uint32_t)end;
}

/* mmioRead(h, buffer, count): bytes read, -1 on error */
H(mm_mmioRead) {
    UNUSED;
    pthread_mutex_lock(&mmio_mu);
    Mmio *m = mmio_of(a[0]);
    uint32_t r = m && (int32_t)a[2] >= 0 ? mmio_read(m, a[1], a[2]) : 0xFFFFFFFFu;
    pthread_mutex_unlock(&mmio_mu);
    return r;
}
/* mmioSeek(h, offset, SEEK_SET/CUR/END): the new position, -1 on error */
H(mm_mmioSeek) {
    UNUSED;
    pthread_mutex_lock(&mmio_mu);
    Mmio *m = mmio_of(a[0]);
    int64_t pos = -1;
    if (m) {
        int64_t base = a[2] == 0 ? 0 : a[2] == 1 ? (int64_t)m->pos : (int64_t)mmio_size(m);
        pos = base + (int32_t)a[1];
        if (pos < 0) pos = -1;
        else m->pos = (uint32_t)pos;
    }
    pthread_mutex_unlock(&mmio_mu);
    return (uint32_t)pos;
}

/* mmioDescend(h, MMCKINFO *ck, const MMCKINFO *parent, flags). MMCKINFO: ckid,
 * cksize, fccType, dwDataOffset, dwFlags. Reads chunk headers from the position
 * (searching up to the parent's end with a FIND flag) and leaves the position at
 * the chunk's data, after the form type of a RIFF or LIST chunk. */
H(mm_mmioDescend) {
    UNUSED;
    pthread_mutex_lock(&mmio_mu);
    Mmio *m = mmio_of(a[0]);
    uint32_t r = MMIOERR_CHUNKNOTFOUND;
    if (!m || !a[1]) goto out;
    uint32_t end = a[2] ? ld32(a[2] + 12) + ld32(a[2] + 4) : 0xFFFFFFFFu;
    uint32_t flags = a[3], want_id = ld32(a[1]), want_type = ld32(a[1] + 8);
    for (;;) {
        if (m->pos + 8 > end) break;
        uint32_t hdr[2], type = 0;
        if (!mmio_read_host(m, hdr, 8)) break;
        uint32_t data = m->pos;
        int list = hdr[0] == FOURCC('R', 'I', 'F', 'F') || hdr[0] == FOURCC('L', 'I', 'S', 'T');
        if (list && !mmio_read_host(m, &type, 4)) break;
        int match;
        if (flags & MMIO_FINDCHUNK) match = hdr[0] == want_id;
        else if (flags & MMIO_FINDRIFF) match = hdr[0] == FOURCC('R', 'I', 'F', 'F') && type == want_type;
        else if (flags & MMIO_FINDLIST) match = hdr[0] == FOURCC('L', 'I', 'S', 'T') && type == want_type;
        else match = 1;
        if (match) {
            st32(a[1], hdr[0]);
            st32(a[1] + 4, hdr[1]);
            st32(a[1] + 8, type);
            st32(a[1] + 12, data);
            st32(a[1] + 16, 0);
            r = 0;
            break;
        }
        m->pos = data + hdr[1] + (hdr[1] & 1);
    }
out:
    pthread_mutex_unlock(&mmio_mu);
    return r;
}
/* mmioAscend(h, MMCKINFO *, flags): past the chunk's (even-padded) end. */
H(mm_mmioAscend) {
    UNUSED;
    pthread_mutex_lock(&mmio_mu);
    Mmio *m = mmio_of(a[0]);
    uint32_t r = MMIOERR_CANNOTSEEK;
    if (m && a[1]) {
        uint32_t size = ld32(a[1] + 4);
        m->pos = ld32(a[1] + 12) + size + (size & 1);
        r = 0;
    }
    pthread_mutex_unlock(&mmio_mu);
    return r;
}

/* --------------------------------------------------------------- imm32 */
/* No input method editor: every query reports none. */
H(imm_ImmGetIMEFileNameA) {
    UNUSED;
    if (a[1] && a[2]) st8(a[1], 0);
    return 0;
}

/* ------------------------------------------------------ advapi32 security */
/* The game sets security on the files it creates; access checks always grant. */
H(adv_AllocateAndInitializeSid) {
    UNUSED;
    st32(a[10], rt_heap_alloc(32, 1));
    return 1;
}
H(adv_FreeSid) {
    UNUSED;
    if (a[0]) rt_heap_free(a[0]);
    return 0;
}
H(adv_OpenProcessToken) {
    UNUSED;
    st32(a[2], 0xC0DE0001u);
    return 1;
}
H(adv_OpenThreadToken) {
    UNUSED;
    rt_set_last_error(c, 1008); /* ERROR_NO_TOKEN: the thread is not impersonating */
    return 0;
}
/* AccessCheck(sd, token, desired, mapping, privileges, privlen, granted*, status*) */
H(adv_AccessCheck) {
    UNUSED;
    st32(a[6], a[2]);
    st32(a[7], 1);
    return 1;
}
H(adv_GetLengthSid) { UNUSED; return 12; }
H(adv_InitializeSecurityDescriptor) {
    UNUSED;
    memset(G(a[0]), 0, 20);
    st8(a[0], 1);
    return 1;
}
H(adv_GetUserNameW) {
    UNUSED;
    uint32_t n = ld32(a[1]);
    if (n < 7) {
        st32(a[1], 7);
        rt_set_last_error(c, 122);
        return 0;
    }
    rt_put_wstr(a[0], n, "Player");
    st32(a[1], 7);
    return 1;
}

/* ------------------------------------------------------- ole32, oleaut32 */
H(ole_CoCreateGuid) {
    UNUSED;
    for (int i = 0; i < 16; i++) st8(a[0] + (uint32_t)i, (uint8_t)rand());
    return 0;
}
/* COM servers the recompilation lifts: CoCreateInstance offers a CLSID to each
 * one's DllGetClassObject, as Windows would after looking it up in the registry. */
static const char *const com_servers[] = {"msxml4.dll"};

/* CoCreateInstance(clsid, outer, context, iid, ppv) */
H(ole_CoCreateInstance) {
    char cls[40], iid[40];
    snprintf(cls, sizeof cls, "%s", rt_guid_text(a[0]));
    snprintf(iid, sizeof iid, "%s", rt_guid_text(a[3]));
    if (a[4]) st32(a[4], 0);
    static const uint8_t IID_IClassFactory[16] = {1, 0, 0, 0, 0, 0, 0, 0, 0xC0, 0, 0, 0, 0, 0, 0, 0x46};
    for (size_t i = 0; i < RT_COUNTOF(com_servers); i++) {
        RtImage *im = rt_load_dll(com_servers[i]);
        if (!im || !rt_dll_attach(c)) continue;
        uint32_t get = rt_image_export(im, "DllGetClassObject", 0);
        if (!get) continue;
        uint32_t tmp = rt_heap_alloc(32, 1);
        memcpy(G(tmp), IID_IClassFactory, 16);
        uint32_t args[3] = {a[0], tmp, tmp + 16}, hr = 0x80040111u;
        if (!rt_call_guest(c, get, RT_STDCALL, 3, args, &hr)) return 0;
        uint32_t cf = ld32(tmp + 16);
        rt_heap_free(tmp);
        if (hr != 0 || !cf) continue;
        /* IClassFactory::CreateInstance(this, outer, iid, ppv), then Release */
        uint32_t vt = ld32(cf);
        uint32_t cargs[4] = {cf, a[1], a[3], a[4]}, r = 0;
        if (!rt_call_guest(c, ld32(vt + 12), RT_STDCALL, 4, cargs, &hr)) return 0;
        uint32_t rargs[1] = {cf};
        if (!rt_call_guest(c, ld32(vt + 8), RT_STDCALL, 1, rargs, &r)) return 0;
        if (g_opt.verbose) rt_log("[com] CoCreateInstance %s (%s) by %s -> %#x\n", cls, iid, im->name, hr);
        return hr;
    }
    rt_log("[com] CoCreateInstance %s (%s): class not registered\n", cls, iid);
    return 0x80040154u; /* REGDB_E_CLASSNOTREG */
}
H(ole_CoGetMalloc) {
    UNUSED;
    st32(a[1], 0);
    return 0x80004001u;
}
/* StringFromGUID2(guid, buf, cch) */
H(ole_StringFromGUID2) {
    UNUSED;
    char s[40];
    uint32_t g = a[0];
    snprintf(s, sizeof s, "{%08X-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X}", ld32(g), ld16(g + 4), ld16(g + 6),
             ld8(g + 8), ld8(g + 9), ld8(g + 10), ld8(g + 11), ld8(g + 12), ld8(g + 13), ld8(g + 14), ld8(g + 15));
    if (a[2] < 39) return 0;
    rt_put_wstr(a[1], a[2], s);
    return 39;
}
/* BSTR: a 32-bit byte length before the characters, which are NUL-terminated. */
static uint32_t bstr_alloc(uint32_t src, uint32_t bytes) {
    uint32_t p = rt_heap_alloc(4 + bytes + 2, 0);
    st32(p, bytes);
    if (src) memcpy(G(p + 4), G(src), bytes);
    else memset(G(p + 4), 0, bytes);
    st16(p + 4 + bytes, 0);
    return p + 4;
}
H(oa_SysAllocString) { UNUSED; return a[0] ? bstr_alloc(a[0], 2 * rt_wlen(a[0])) : 0; }
H(oa_SysAllocStringLen) { UNUSED; return bstr_alloc(a[0], 2 * a[1]); }
H(oa_SysAllocStringByteLen) { UNUSED; return bstr_alloc(a[0], a[1]); }
H(oa_SysFreeString) {
    UNUSED;
    if (a[0]) rt_heap_free(a[0] - 4);
    return 0;
}
/* SysReAllocString(BSTR*, src) / SysReAllocStringLen(BSTR*, src, len) */
H(oa_SysReAllocString) {
    UNUSED;
    uint32_t old = ld32(a[0]);
    st32(a[0], a[1] ? bstr_alloc(a[1], 2 * rt_wlen(a[1])) : 0);
    if (old) rt_heap_free(old - 4);
    return 1;
}
H(oa_SysReAllocStringLen) {
    UNUSED;
    uint32_t old = ld32(a[0]);
    st32(a[0], bstr_alloc(a[1], 2 * a[2]));
    if (old) rt_heap_free(old - 4);
    return 1;
}
H(oa_SysStringLen) { UNUSED; return a[0] ? ld32(a[0] - 4) / 2 : 0; }
H(oa_SysStringByteLen) { UNUSED; return a[0] ? ld32(a[0] - 4) : 0; }

/* VARIANT: vt (16 bits), 3 reserved words, then 8 bytes of value. */
#define VT_EMPTY 0
#define VT_DISPATCH 9
#define VT_BSTR 8
#define VT_UNKNOWN 13
#define VT_BYREF 0x4000
H(oa_VariantInit) {
    UNUSED;
    memset(G(a[0]), 0, 16);
    return 0;
}
/* IUnknown::AddRef / Release of a guest object */
static void com_call(CPU *c, uint32_t obj, int slot) {
    uint32_t r, args[1] = {obj};
    if (obj) rt_call_guest(c, ld32(ld32(obj) + 4 * (uint32_t)slot), RT_STDCALL, 1, args, &r);
}
H(oa_VariantClear) {
    uint16_t vt = ld16(a[0]);
    if (!(vt & VT_BYREF)) {
        if (vt == VT_BSTR && ld32(a[0] + 8)) rt_heap_free(ld32(a[0] + 8) - 4);
        if (vt == VT_UNKNOWN || vt == VT_DISPATCH) com_call(c, ld32(a[0] + 8), 2);
    }
    memset(G(a[0]), 0, 16);
    return 0;
}
/* VariantCopy(dst, src) */
H(oa_VariantCopy) {
    if (a[0] == a[1]) return 0;
    uint32_t args[1] = {a[0]};
    oa_VariantClear(c, args);
    memcpy(G(a[0]), G(a[1]), 16);
    uint16_t vt = ld16(a[1]);
    if (vt == VT_BSTR && ld32(a[1] + 8)) st32(a[0] + 8, bstr_alloc(ld32(a[1] + 8), ld32(ld32(a[1] + 8) - 4)));
    if (vt == VT_UNKNOWN || vt == VT_DISPATCH) com_call(c, ld32(a[1] + 8), 1);
    return 0;
}
/* VariantChangeType(dst, src, flags, vt): the conversions to and from strings and integers. */
H(oa_VariantChangeType) {
    uint16_t from = ld16(a[1]), to = (uint16_t)a[3];
    uint8_t val[8];
    memcpy(val, G(a[1] + 8), 8);
    if (from == to) return oa_VariantCopy(c, (uint32_t[]){a[0], a[1]});
    char s[64];
    int64_t iv = 0;
    int have_int = 1;
    switch (from) {
    case 2: iv = (int16_t)(val[0] | val[1] << 8); break;            /* VT_I2 */
    case 3: case 22: iv = (int32_t)ld32(a[1] + 8); break;           /* VT_I4, VT_INT */
    case 11: iv = (val[0] | val[1]) ? -1 : 0; break;                /* VT_BOOL */
    case 17: iv = val[0]; break;                                    /* VT_UI1 */
    case 19: case 23: iv = ld32(a[1] + 8); break;                   /* VT_UI4, VT_UINT */
    case VT_BSTR:
        rt_get_wstr(ld32(a[1] + 8), s, sizeof s);
        iv = strtoll(s, NULL, 0);
        break;
    default: have_int = 0;
    }
    if (!have_int) {
        rt_log("[oleaut32] VariantChangeType %u -> %u not provided\n", from, to);
        return 0x80020005u; /* DISP_E_TYPEMISMATCH */
    }
    uint32_t args[1] = {a[0]};
    if (a[0] != a[1]) oa_VariantClear(c, args);
    memset(G(a[0]), 0, 16);
    st16(a[0], to);
    switch (to) {
    case 2: st16(a[0] + 8, (uint16_t)iv); break;
    case 3: case 22: case 19: case 23: st32(a[0] + 8, (uint32_t)iv); break;
    case 11: st16(a[0] + 8, iv ? 0xFFFF : 0); break;
    case 17: st8(a[0] + 8, (uint8_t)iv); break;
    case VT_BSTR: {
        snprintf(s, sizeof s, "%lld", (long long)iv);
        uint32_t w = rt_wstr_dup(s);
        st32(a[0] + 8, bstr_alloc(w, 2 * rt_wlen(w)));
        rt_heap_free(w);
        break;
    }
    default:
        rt_log("[oleaut32] VariantChangeType %u -> %u not provided\n", from, to);
        return 0x80020005u;
    }
    return 0;
}
H(rpc_UuidCreate) {
    UNUSED;
    for (int i = 0; i < 16; i++) st8(a[0] + (uint32_t)i, (uint8_t)rand());
    return 0;
}

/* ------------------------------------------------------------- version */
/* No version resources are provided: the game falls back to its own numbers. */
H(ver_GetFileVersionInfoSize) {
    UNUSED;
    if (g_opt.verbose) {
        char n[300];
        rt_get_wstr(a[0], n, sizeof n);
        rt_log("[version] GetFileVersionInfoSize(\"%s\")\n", n);
    }
    if (a[1]) st32(a[1], 0);
    rt_set_last_error(c, 1813); /* ERROR_RESOURCE_TYPE_NOT_FOUND */
    return 0;
}

static const ImportDef defs[] = {
    {"winmm.dll", "timeBeginPeriod", mm_timeBeginPeriod, 1, 0},
    {"winmm.dll", "timeEndPeriod", mm_timeBeginPeriod, 1, 0},
    {"winmm.dll", "waveOutGetNumDevs", st_zero, 0, 0},
    {"winmm.dll", "waveInGetNumDevs", st_zero, 0, 0},
    {"winmm.dll", "midiOutGetNumDevs", st_zero, 0, 0},
    {"winmm.dll", "auxGetNumDevs", st_zero, 0, 0},
    {"winmm.dll", "mixerGetNumDevs", st_zero, 0, 0},
    {"winmm.dll", "waveOutOpen", st_nodriver, 6, 0},
    {"winmm.dll", "waveInOpen", st_nodriver, 6, 0},
    {"winmm.dll", "midiOutOpen", st_nodriver, 5, 0},
    {"winmm.dll", "mixerOpen", st_nodriver, 5, 0},
    {"winmm.dll", "waveOutGetDevCapsA", st_baddevice, 3, 0},
    {"winmm.dll", "waveOutGetID", st_baddevice, 2, 0},
    {"winmm.dll", "auxGetDevCapsA", st_baddevice, 3, 0},
    {"winmm.dll", "auxGetVolume", st_baddevice, 2, 0},
    {"winmm.dll", "auxSetVolume", st_baddevice, 2, 0},
    {"winmm.dll", "mixerGetLineInfoA", st_baddevice, 3, 0},
    {"winmm.dll", "mixerGetLineControlsA", st_baddevice, 3, 0},
    {"winmm.dll", "mixerGetControlDetailsA", st_baddevice, 3, 0},
    {"winmm.dll", "mixerSetControlDetails", st_baddevice, 3, 0},
    {"winmm.dll", "mixerClose", st_zero, 1, 0},
    {"winmm.dll", "mciSendCommandA", mm_mciSendCommandA, 4, 0},
    {"winmm.dll", "mmioOpenA", mm_mmioOpenA, 3, 0},
    {"winmm.dll", "mmioOpenW", mm_mmioOpenW, 3, 0},
    {"winmm.dll", "mmioClose", mm_mmioClose, 2, 0},
    {"winmm.dll", "mmioRead", mm_mmioRead, 3, 0},
    {"winmm.dll", "mmioSeek", mm_mmioSeek, 3, 0},
    {"winmm.dll", "mmioDescend", mm_mmioDescend, 4, 0},
    {"winmm.dll", "mmioAscend", mm_mmioAscend, 3, 0},
    {"imm32.dll", "ImmGetIMEFileNameA", imm_ImmGetIMEFileNameA, 3, 0},
    {"imm32.dll", "ImmIsIME", st_zero, 1, 0},
    {"imm32.dll", "ImmGetDefaultIMEWnd", st_zero, 1, 0},
    {"imm32.dll", "ImmAssociateContext", st_zero, 2, 0},
    {"imm32.dll", "ImmReleaseContext", st_one, 2, 0},
    {"imm32.dll", "ImmNotifyIME", st_zero, 4, 0},
    {"imm32.dll", "ImmGetConversionStatus", st_zero, 3, 0},
    {"imm32.dll", "ImmGetCompositionStringW", st_zero, 4, 0},
    {"imm32.dll", "ImmGetCandidateListA", st_zero, 4, 0},
    {"imm32.dll", "ImmGetCandidateListW", st_zero, 4, 0},
    {"imm32.dll", "ImmGetVirtualKey", st_zero, 1, 0},
    {"imm32.dll", "ImmDisableTextFrameService", st_one, 1, 0},
    {"imm32.dll", "ImmLockIMC", st_zero, 1, 0},
    {"imm32.dll", "ImmUnlockIMC", st_one, 1, 0},
    {"imm32.dll", "ImmLockIMCC", st_zero, 1, 0},
    {"imm32.dll", "ImmUnlockIMCC", st_one, 1, 0},
    {"advapi32.dll", "AllocateAndInitializeSid", adv_AllocateAndInitializeSid, 11, 0},
    {"advapi32.dll", "FreeSid", adv_FreeSid, 1, 0},
    {"advapi32.dll", "OpenProcessToken", adv_OpenProcessToken, 3, 0},
    {"advapi32.dll", "OpenThreadToken", adv_OpenThreadToken, 4, 0},
    {"advapi32.dll", "ImpersonateSelf", st_one, 1, 0},
    {"advapi32.dll", "RevertToSelf", st_one, 0, 0},
    {"advapi32.dll", "AccessCheck", adv_AccessCheck, 8, 0},
    {"advapi32.dll", "GetLengthSid", adv_GetLengthSid, 1, 0},
    {"advapi32.dll", "InitializeAcl", st_one, 3, 0},
    {"advapi32.dll", "AddAccessAllowedAce", st_one, 4, 0},
    {"advapi32.dll", "InitializeSecurityDescriptor", adv_InitializeSecurityDescriptor, 2, 0},
    {"advapi32.dll", "IsValidSecurityDescriptor", st_one, 1, 0},
    {"advapi32.dll", "SetSecurityDescriptorDacl", st_one, 4, 0},
    {"advapi32.dll", "SetSecurityDescriptorGroup", st_one, 3, 0},
    {"advapi32.dll", "SetSecurityDescriptorOwner", st_one, 3, 0},
    {"advapi32.dll", "SetFileSecurityW", st_one, 3, 0},
    {"advapi32.dll", "GetUserNameW", adv_GetUserNameW, 2, 0},
    {"ole32.dll", "CoCreateGuid", ole_CoCreateGuid, 1, 0},
    {"ole32.dll", "CoCreateInstance", ole_CoCreateInstance, 5, 0},
    {"ole32.dll", "StringFromGUID2", ole_StringFromGUID2, 3, 0},
    {"oleaut32.dll", "#2", oa_SysAllocString, 1, 0},
    {"oleaut32.dll", "SysAllocString", oa_SysAllocString, 1, 0},
    {"oleaut32.dll", "#3", oa_SysReAllocString, 2, 0},
    {"oleaut32.dll", "#4", oa_SysAllocStringLen, 2, 0},
    {"oleaut32.dll", "SysAllocStringLen", oa_SysAllocStringLen, 2, 0},
    {"oleaut32.dll", "#5", oa_SysReAllocStringLen, 3, 0},
    {"oleaut32.dll", "#6", oa_SysFreeString, 1, 0},
    {"oleaut32.dll", "SysFreeString", oa_SysFreeString, 1, 0},
    {"oleaut32.dll", "#7", oa_SysStringLen, 1, 0},
    {"oleaut32.dll", "SysStringLen", oa_SysStringLen, 1, 0},
    {"oleaut32.dll", "#8", oa_VariantInit, 1, 0},
    {"oleaut32.dll", "VariantInit", oa_VariantInit, 1, 0},
    {"oleaut32.dll", "#9", oa_VariantClear, 1, 0},
    {"oleaut32.dll", "VariantClear", oa_VariantClear, 1, 0},
    {"oleaut32.dll", "#10", oa_VariantCopy, 2, 0},
    {"oleaut32.dll", "VariantCopy", oa_VariantCopy, 2, 0},
    {"oleaut32.dll", "#12", oa_VariantChangeType, 4, 0},
    {"oleaut32.dll", "VariantChangeType", oa_VariantChangeType, 4, 0},
    {"oleaut32.dll", "#149", oa_SysStringByteLen, 1, 0},
    {"oleaut32.dll", "#150", oa_SysAllocStringByteLen, 2, 0},
    {"rpcrt4.dll", "UuidCreate", rpc_UuidCreate, 1, 0},
    {"version.dll", "GetFileVersionInfoSizeA", ver_GetFileVersionInfoSize, 2, 0},
    {"version.dll", "GetFileVersionInfoSizeW", ver_GetFileVersionInfoSize, 2, 0},
    {"version.dll", "GetFileVersionInfoA", st_zero, 4, 0},
    {"version.dll", "GetFileVersionInfoW", st_zero, 4, 0},
    {"version.dll", "VerQueryValueA", st_zero, 4, 0},
    {"version.dll", "VerQueryValueW", st_zero, 4, 0},
};
RT_EXPORT_IMPORTS(rt_imports_stubs, defs);

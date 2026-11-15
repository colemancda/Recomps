/* shlwapi, and the ole32 and kernel32 functions msxml4.dll needs.
 *
 * msxml4 resolves the documents it loads as URLs; only local files are
 * supported, as file:///C:/path URLs or plain paths. Strings here are guest
 * UTF-16 and are handled in place where possible.
 */
#define _GNU_SOURCE
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#include "rt.h"

#define E_POINTER 0x80004003u
#define E_INVALIDARG 0x80070057u
#define E_NOTIMPL 0x80004001u
#define S_FALSE 1u

static uint32_t wlower(uint32_t ch) { return ch >= 'A' && ch <= 'Z' ? ch + 32 : ch; }

/* Compare up to n characters (n < 0: unlimited) of two guest UTF-16 strings. */
static int wcmp(uint32_t a, uint32_t b, int n, int icase) {
    for (int i = 0; n < 0 || i < n; i++) {
        uint32_t x = ld16(a + 2 * (uint32_t)i), y = ld16(b + 2 * (uint32_t)i);
        if (icase) x = wlower(x), y = wlower(y);
        if (x != y) return x < y ? -1 : 1;
        if (!x) break;
    }
    return 0;
}
static void wcopy(uint32_t dst, uint32_t src) {
    uint32_t i = 0;
    do st16(dst + 2 * i, ld16(src + 2 * i));
    while (ld16(src + 2 * i++));
}
static const ImportDef *def_of(const char *dll, const char *name) {
    const ImportDef *d = rt_find_import_def(dll, name);
    if (!d) rt_log("shlwapi: no handler %s!%s to forward to\n", dll, name);
    return d;
}

/* ------------------------------------------------------------ strings */
H(sh_StrCmpW) { UNUSED; return (uint32_t)wcmp(a[0], a[1], -1, 0); }
H(sh_StrCmpIW) { UNUSED; return (uint32_t)wcmp(a[0], a[1], -1, 1); }
H(sh_StrCmpNW) { UNUSED; return (uint32_t)wcmp(a[0], a[1], (int)a[2], 0); }
H(sh_StrCmpNIW) { UNUSED; return (uint32_t)wcmp(a[0], a[1], (int)a[2], 1); }
H(sh_StrCpyW) { UNUSED; wcopy(a[0], a[1]); return a[0]; }
H(sh_StrCatW) { UNUSED; wcopy(a[0] + 2 * rt_wlen(a[0]), a[1]); return a[0]; }
H(sh_StrToIntW) {
    UNUSED;
    uint32_t p = a[0];
    int neg = 0, v = 0;
    while (ld16(p) == ' ' || ld16(p) == '\t') p += 2;
    if (ld16(p) == '-') neg = 1, p += 2;
    else if (ld16(p) == '+') p += 2;
    while (ld16(p) >= '0' && ld16(p) <= '9') v = v * 10 + (ld16(p) - '0'), p += 2;
    return (uint32_t)(neg ? -v : v);
}
H(sh_GetAcceptLanguagesW) {
    UNUSED;
    uint32_t cap = ld32(a[1]);
    if (cap < 6) {
        st32(a[1], 6);
        return 0x8007007Au; /* HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER) */
    }
    rt_put_wstr(a[0], cap, "en-us");
    st32(a[1], 5);
    return 0;
}
H(sh_IsCharAlphaNumericW) { UNUSED; return a[0] < 128 ? isalnum((int)a[0]) != 0 : a[0] >= 0xC0; }
H(sh_IsCharSpaceW) { UNUSED; return a[0] == ' ' || (a[0] >= 9 && a[0] <= 13) || a[0] == 0xA0 || a[0] == 0x3000; }
H(sh_CharLowerW) {
    UNUSED;
    if (a[0] < 0x10000) return wlower(a[0]);
    for (uint32_t p = a[0]; ld16(p); p += 2) st16(p, (uint16_t)wlower(ld16(p)));
    return a[0];
}
H(sh_CharUpperBuffW) {
    UNUSED;
    for (uint32_t i = 0; i < a[1]; i++) {
        uint16_t ch = ld16(a[0] + 2 * i);
        if (ch >= 'a' && ch <= 'z') st16(a[0] + 2 * i, (uint16_t)(ch - 32));
    }
    return a[1];
}
H(sh_CompareStringW) { return def_of("kernel32.dll", "CompareStringW")->fn(c, a); }
H(sh_CreateFileW) { return def_of("kernel32.dll", "CreateFileW")->fn(c, a); }
H(sh_FindResourceW) { return def_of("kernel32.dll", "FindResourceW")->fn(c, a); }
H(sh_OutputDebugStringW) { return def_of("kernel32.dll", "OutputDebugStringW")->fn(c, a); }

/* wvsprintfW(buf, format, va_list): %[-0][width][.prec][l|h]{d,i,u,x,X,c,s,S}, %% */
H(sh_wvsprintfW) {
    UNUSED;
    uint32_t out = a[0], f = a[1], va = a[2], o = 0;
    char num[64];
    for (;;) {
        uint16_t ch = ld16(f);
        f += 2;
        if (!ch) break;
        if (ch != '%') {
            if (o < 1023) st16(out + 2 * o++, ch);
            continue;
        }
        int left = 0, zero = 0, width = 0, prec = -1, wide_s = 1;
        for (;; f += 2) {
            ch = ld16(f);
            if (ch == '-') left = 1;
            else if (ch == '0') zero = 1;
            else break;
        }
        while ((ch = ld16(f)) >= '0' && ch <= '9') width = width * 10 + (ch - '0'), f += 2;
        if (ld16(f) == '.') {
            f += 2;
            prec = 0;
            while ((ch = ld16(f)) >= '0' && ch <= '9') prec = prec * 10 + (ch - '0'), f += 2;
        }
        if (ld16(f) == 'l') f += 2;
        else if (ld16(f) == 'h') wide_s = 0, f += 2;
        ch = ld16(f);
        f += 2;
        char sbuf[1024];
        const char *s = num;
        switch (ch) {
        case 'd': case 'i': snprintf(num, sizeof num, "%d", (int32_t)ld32(va)), va += 4; break;
        case 'u': snprintf(num, sizeof num, "%u", ld32(va)), va += 4; break;
        case 'x': snprintf(num, sizeof num, "%x", ld32(va)), va += 4; break;
        case 'X': snprintf(num, sizeof num, "%X", ld32(va)), va += 4; break;
        case 'c': num[0] = (char)ld32(va), num[1] = 0, va += 4; break;
        case 's': case 'S': {
            uint32_t p = ld32(va);
            va += 4;
            if (!p) snprintf(sbuf, sizeof sbuf, "(null)");
            else if ((ch == 's') == (wide_s != 0)) rt_get_wstr(p, sbuf, sizeof sbuf);
            else snprintf(sbuf, sizeof sbuf, "%s", rt_gstr(p));
            if (prec >= 0 && (size_t)prec < strlen(sbuf)) sbuf[prec] = 0;
            s = sbuf;
            break;
        }
        case '%': num[0] = '%', num[1] = 0; break;
        default: num[0] = 0; break;
        }
        int n = (int)strlen(s);
        for (int k = n; !left && k < width && o < 1023; k++) st16(out + 2 * o++, zero ? '0' : ' ');
        for (const unsigned char *p = (const unsigned char *)s; *p && o < 1023; p++) st16(out + 2 * o++, *p);
        for (int k = n; left && k < width && o < 1023; k++) st16(out + 2 * o++, ' ');
    }
    st16(out + 2 * o, 0);
    return o;
}

/* --------------------------------------------------------------- URLs */
static void get_w(uint32_t p, char *out, size_t cap) {
    if (p) rt_get_wstr(p, out, cap);
    else out[0] = 0;
}
/* Write a result string and its length (characters, without the NUL) to (buf, *len). */
static uint32_t put_result(const char *s, uint32_t buf, uint32_t plen) {
    uint32_t n = (uint32_t)strlen(s), cap = ld32(plen);
    if (n + 1 > cap) {
        st32(plen, n + 1);
        return E_POINTER;
    }
    rt_put_wstr(buf, cap, s);
    st32(plen, n);
    return 0;
}
static int is_url(const char *s) {
    const char *colon = strchr(s, ':');
    if (!colon || colon - s < 2) return 0; /* "C:" is a drive, not a scheme */
    for (const char *p = s; p < colon; p++)
        if (!isalnum((unsigned char)*p) && *p != '+' && *p != '-' && *p != '.') return 0;
    return 1;
}
static void unescape(char *s) {
    char *o = s;
    for (char *p = s; *p; p++) {
        if (p[0] == '%' && isxdigit((unsigned char)p[1]) && isxdigit((unsigned char)p[2])) {
            char h[3] = {p[1], p[2], 0};
            *o++ = (char)strtol(h, NULL, 16);
            p += 2;
        } else {
            *o++ = *p;
        }
    }
    *o = 0;
}
/* file:///C:/a/b -> C:\a\b ; other URLs unchanged */
static int url_to_path(const char *url, char *out, size_t cap) {
    if (strncasecmp(url, "file:", 5)) return 0;
    const char *p = url + 5;
    while (*p == '/') p++;
    snprintf(out, cap, "%s", p);
    unescape(out);
    for (char *q = out; *q; q++)
        if (*q == '/') *q = '\\';
    return 1;
}

H(sh_PathIsURLW) {
    UNUSED;
    char s[1024];
    get_w(a[0], s, sizeof s);
    return (uint32_t)is_url(s);
}
/* UrlIsW(url, URLIS): URL 0, OPAQUE 1, NOHISTORY 2, FILEURL 3, APPLIABLE 4, DIRECTORY 5, HASQUERY 6 */
H(sh_UrlIsW) {
    UNUSED;
    char s[1024];
    get_w(a[0], s, sizeof s);
    switch (a[1]) {
    case 0: return (uint32_t)is_url(s);
    case 3: return !strncasecmp(s, "file:", 5);
    case 6: return strchr(s, '?') != NULL;
    default: return 0;
    }
}
H(sh_PathIsRelativeW) {
    UNUSED;
    char s[1024];
    get_w(a[0], s, sizeof s);
    return !(s[0] == '\\' || s[0] == '/' || (s[0] && s[1] == ':'));
}
/* UrlCreateFromPathW(path, url, &len, reserved) */
H(sh_UrlCreateFromPathW) {
    UNUSED;
    char p[1024], u[1100];
    get_w(a[0], p, sizeof p);
    if (is_url(p)) return put_result(p, a[1], a[2]) == 0 ? S_FALSE : E_POINTER;
    snprintf(u, sizeof u, "file:///%s", p);
    for (char *q = u + 8; *q; q++)
        if (*q == '\\') *q = '/';
    return put_result(u, a[1], a[2]);
}
/* PathCreateFromUrlW(url, path, &len, reserved) */
H(sh_PathCreateFromUrlW) {
    UNUSED;
    char u[1024], p[1024];
    get_w(a[0], u, sizeof u);
    if (!url_to_path(u, p, sizeof p)) return E_INVALIDARG;
    return put_result(p, a[1], a[2]);
}
/* UrlCanonicalizeW(url, out, &len, flags) */
H(sh_UrlCanonicalizeW) {
    UNUSED;
    char u[1024];
    get_w(a[0], u, sizeof u);
    if (!strncasecmp(u, "file:", 5))
        for (char *q = u; *q; q++)
            if (*q == '\\') *q = '/';
    return put_result(u, a[1], a[2]);
}
/* UrlCombineW(base, relative, out, &len, flags) */
H(sh_UrlCombineW) {
    UNUSED;
    char b[1024], r[1024], o[2100];
    get_w(a[0], b, sizeof b);
    get_w(a[1], r, sizeof r);
    if (is_url(r) || r[0] == '\\' || (r[0] && r[1] == ':')) {
        snprintf(o, sizeof o, "%s", r);
    } else {
        char *cut = NULL;
        for (char *q = b; *q; q++)
            if (*q == '/' || *q == '\\') cut = q;
        if (cut) cut[1] = 0;
        else b[0] = 0;
        snprintf(o, sizeof o, "%s%s", b, r);
    }
    return put_result(o, a[2], a[3]);
}
/* UrlUnescapeW(url, out, &len, flags); URL_UNESCAPE_INPLACE writes back into url. */
H(sh_UrlUnescapeW) {
    UNUSED;
    char u[1024];
    get_w(a[0], u, sizeof u);
    unescape(u);
    if (a[3] & 0x00100000u) {
        rt_put_wstr(a[0], rt_wlen(a[0]) + 1, u);
        return 0;
    }
    return put_result(u, a[1], a[2]);
}
/* UrlGetLocationW(url): the '#' fragment, or NULL */
H(sh_UrlGetLocationW) {
    UNUSED;
    for (uint32_t p = a[0]; ld16(p); p += 2)
        if (ld16(p) == '#') return p;
    return 0;
}
/* PathSearchAndQualifyW(path, buf, cch) */
H(sh_PathSearchAndQualifyW) {
    uint32_t args[4] = {a[0], a[2], a[1], 0};
    uint32_t n = def_of("kernel32.dll", "GetFullPathNameW")->fn(c, args);
    return n && n < a[2];
}

/* ---------------------------------------------------------------- ole32 */
H(ole_CoTaskMemAlloc) { UNUSED; return rt_heap_alloc(a[0] ? a[0] : 1, 0); }
H(ole_CoTaskMemFree) {
    UNUSED;
    if (a[0]) rt_heap_free(a[0]);
    return 0;
}
H(ole_CoTaskMemRealloc) { UNUSED; return a[0] ? rt_heap_realloc(a[0], a[1] ? a[1] : 1, 0) : rt_heap_alloc(a[1], 0); }
/* StringFromCLSID(clsid, &string) */
H(ole_StringFromCLSID) {
    UNUSED;
    char s[40];
    uint32_t g = a[0];
    snprintf(s, sizeof s, "{%08X-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X}", ld32(g), ld16(g + 4), ld16(g + 6),
             ld8(g + 8), ld8(g + 9), ld8(g + 10), ld8(g + 11), ld8(g + 12), ld8(g + 13), ld8(g + 14), ld8(g + 15));
    st32(a[1], rt_wstr_dup(s));
    return 0;
}
/* CLSIDFromProgID(progid, &clsid): the MSXML 4.0 classes. */
H(ole_CLSIDFromProgID) {
    UNUSED;
    static const struct { const char *progid; uint32_t d1; uint16_t d2, d3; uint8_t d4[8]; } ids[] = {
        {"Msxml2.DOMDocument.4.0", 0x88d969c0, 0xf192, 0x11d4, {0xa6, 0x5f, 0x00, 0x40, 0x96, 0x32, 0x51, 0xe5}},
        {"Msxml2.FreeThreadedDOMDocument.4.0", 0x88d969c1, 0xf192, 0x11d4, {0xa6, 0x5f, 0x00, 0x40, 0x96, 0x32, 0x51, 0xe5}},
        {"Msxml2.XMLSchemaCache.4.0", 0x88d969c2, 0xf192, 0x11d4, {0xa6, 0x5f, 0x00, 0x40, 0x96, 0x32, 0x51, 0xe5}},
        {"Msxml2.XSLTemplate.4.0", 0x88d969c3, 0xf192, 0x11d4, {0xa6, 0x5f, 0x00, 0x40, 0x96, 0x32, 0x51, 0xe5}},
    };
    char p[128];
    get_w(a[0], p, sizeof p);
    for (size_t i = 0; i < RT_COUNTOF(ids); i++) {
        if (strcasecmp(p, ids[i].progid)) continue;
        st32(a[1], ids[i].d1);
        st16(a[1] + 4, ids[i].d2);
        st16(a[1] + 6, ids[i].d3);
        memcpy(G(a[1] + 8), ids[i].d4, 8);
        return 0;
    }
    rt_log("[com] CLSIDFromProgID(\"%s\"): unknown\n", p);
    return 0x800401F3u; /* CO_E_CLASSSTRING */
}
static uint32_t ole_notimpl(const char *name) {
    rt_log("[com] %s: not provided\n", name);
    return E_NOTIMPL;
}
H(ole_CreateBindCtx) { UNUSED; if (a[1]) st32(a[1], 0); return ole_notimpl("CreateBindCtx"); }
H(ole_CoCreateFreeThreadedMarshaler) { UNUSED; if (a[1]) st32(a[1], 0); return ole_notimpl("CoCreateFreeThreadedMarshaler"); }
H(ole_CreateStreamOnHGlobal) { UNUSED; if (a[2]) st32(a[2], 0); return ole_notimpl("CreateStreamOnHGlobal"); }
H(ole_GetHGlobalFromStream) { UNUSED; if (a[1]) st32(a[1], 0); return ole_notimpl("GetHGlobalFromStream"); }

/* ------------------------------------------------------------- kernel32 */
H(k32_lstrcmpiW) { UNUSED; return (uint32_t)wcmp(a[0], a[1], -1, 1); }
H(k32_lstrcmpW) { UNUSED; return (uint32_t)wcmp(a[0], a[1], -1, 0); }
H(k32_lstrcpyW) { UNUSED; wcopy(a[0], a[1]); return a[0]; }
H(k32_lstrcatW) { UNUSED; wcopy(a[0] + 2 * rt_wlen(a[0]), a[1]); return a[0]; }
/* lstrcpynW(dst, src, n): at most n-1 characters and a NUL */
H(k32_lstrcpynW) {
    UNUSED;
    if (!a[2]) return a[0];
    uint32_t i = 0;
    for (; i + 1 < a[2] && ld16(a[1] + 2 * i); i++) st16(a[0] + 2 * i, ld16(a[1] + 2 * i));
    st16(a[0] + 2 * i, 0);
    return a[0];
}
H(k32_lstrcpynA) {
    UNUSED;
    if (!a[2]) return a[0];
    uint32_t i = 0;
    for (; i + 1 < a[2] && ld8(a[1] + i); i++) st8(a[0] + i, ld8(a[1] + i));
    st8(a[0] + i, 0);
    return a[0];
}
H(k32_lstrcmpiA) { UNUSED; return (uint32_t)strcasecmp(rt_gstr(a[0]), rt_gstr(a[1])); }
H(k32_lstrcmpA) { UNUSED; return (uint32_t)strcmp(rt_gstr(a[0]), rt_gstr(a[1])); }
H(k32_lstrcpyA) {
    UNUSED;
    strcpy((char *)G(a[0]), rt_gstr(a[1]));
    return a[0];
}
H(k32_ExpandEnvironmentStringsA) {
    UNUSED;
    const char *s = rt_gstr(a[0]);
    uint32_t n = (uint32_t)strlen(s) + 1;
    if (a[1] && a[2] >= n) memcpy(G(a[1]), s, n);
    return n;
}
/* DuplicateHandle(srcProcess, src, dstProcess, &dst, access, inherit, options):
 * the same handle value serves (handles here are not reference counted per copy). */
H(k32_DuplicateHandle) {
    UNUSED;
    if (a[3]) st32(a[3], a[1]);
    return 1;
}
H(k32_LoadLibraryExA) { return def_of("kernel32.dll", "LoadLibraryA")->fn(c, a); }
H(k32_SuspendThread) { UNUSED; rt_log("[kernel32] SuspendThread: not provided\n"); return 0xFFFFFFFFu; }
H(k32_GetThreadContext) { UNUSED; rt_log("[kernel32] GetThreadContext: not provided\n"); return 0; }

/* GetTimeFormat / GetDateFormat (lcid, flags, SYSTEMTIME*, format, buf, cch):
 * the en-US defaults "h:mm:ss tt" and "M/d/yyyy"; a format string is not interpreted. */
static void now_st(int *v) {
    time_t t = time(NULL);
    struct tm tm;
    gmtime_r(&t, &tm);
    v[0] = tm.tm_year + 1900, v[1] = tm.tm_mon + 1, v[2] = tm.tm_mday, v[3] = tm.tm_hour, v[4] = tm.tm_min,
    v[5] = tm.tm_sec;
}
static uint32_t fmt_out(const uint32_t *a, const char *s, int wide) {
    uint32_t n = (uint32_t)strlen(s) + 1;
    if (!a[5]) return n;
    if (n > a[5]) return 0;
    if (wide) rt_put_wstr(a[4], n, s);
    else memcpy(G(a[4]), s, n);
    return n;
}
static uint32_t time_format(const uint32_t *a, int wide) {
    int v[6];
    if (a[2]) v[3] = ld16(a[2] + 8), v[4] = ld16(a[2] + 10), v[5] = ld16(a[2] + 12);
    else now_st(v);
    char s[32];
    int h = v[3] % 12 ? v[3] % 12 : 12;
    snprintf(s, sizeof s, "%d:%02d:%02d %s", h, v[4], v[5], v[3] < 12 ? "AM" : "PM");
    return fmt_out(a, s, wide);
}
static uint32_t date_format(const uint32_t *a, int wide) {
    int v[6];
    if (a[2]) v[0] = ld16(a[2]), v[1] = ld16(a[2] + 2), v[2] = ld16(a[2] + 6);
    else now_st(v);
    char s[32];
    snprintf(s, sizeof s, "%d/%d/%d", v[1], v[2], v[0]);
    return fmt_out(a, s, wide);
}
H(k32_GetTimeFormatA) { UNUSED; return time_format(a, 0); }
H(k32_GetTimeFormatW) { UNUSED; return time_format(a, 1); }
H(k32_GetDateFormatA) { UNUSED; return date_format(a, 0); }
H(k32_GetDateFormatW) { UNUSED; return date_format(a, 1); }

static const ImportDef defs[] = {
    {"shlwapi.dll", "StrCmpW", sh_StrCmpW, 2, 0},
    {"shlwapi.dll", "#156", sh_StrCmpW, 2, 0}, /* StrCmpCW */
    {"shlwapi.dll", "StrCmpIW", sh_StrCmpIW, 2, 0},
    {"shlwapi.dll", "#158", sh_StrCmpIW, 2, 0}, /* StrCmpICW */
    {"shlwapi.dll", "StrCmpNW", sh_StrCmpNW, 3, 0},
    {"shlwapi.dll", "StrCmpNIW", sh_StrCmpNIW, 3, 0},
    {"shlwapi.dll", "StrCpyW", sh_StrCpyW, 2, 0},
    {"shlwapi.dll", "StrCatW", sh_StrCatW, 2, 0},
    {"shlwapi.dll", "StrToIntW", sh_StrToIntW, 1, 0},
    {"shlwapi.dll", "#15", sh_GetAcceptLanguagesW, 2, 0},
    {"shlwapi.dll", "#28", sh_IsCharAlphaNumericW, 1, 0},
    {"shlwapi.dll", "#29", sh_IsCharSpaceW, 1, 0},
    {"shlwapi.dll", "#38", sh_CharLowerW, 1, 0},
    {"shlwapi.dll", "#44", sh_CharUpperBuffW, 2, 0},
    {"shlwapi.dll", "#45", sh_CompareStringW, 6, 0},
    {"shlwapi.dll", "#52", sh_CreateFileW, 7, 0},
    {"shlwapi.dll", "#66", sh_FindResourceW, 3, 0},
    {"shlwapi.dll", "#115", sh_OutputDebugStringW, 1, 0},
    {"shlwapi.dll", "#150", sh_wvsprintfW, 3, 0},
    {"shlwapi.dll", "PathIsURLW", sh_PathIsURLW, 1, 0},
    {"shlwapi.dll", "UrlIsW", sh_UrlIsW, 2, 0},
    {"shlwapi.dll", "PathIsRelativeW", sh_PathIsRelativeW, 1, 0},
    {"shlwapi.dll", "UrlCreateFromPathW", sh_UrlCreateFromPathW, 4, 0},
    {"shlwapi.dll", "PathCreateFromUrlW", sh_PathCreateFromUrlW, 4, 0},
    {"shlwapi.dll", "UrlCanonicalizeW", sh_UrlCanonicalizeW, 4, 0},
    {"shlwapi.dll", "UrlCombineW", sh_UrlCombineW, 5, 0},
    {"shlwapi.dll", "UrlUnescapeW", sh_UrlUnescapeW, 4, 0},
    {"shlwapi.dll", "UrlGetLocationW", sh_UrlGetLocationW, 1, 0},
    {"shlwapi.dll", "PathSearchAndQualifyW", sh_PathSearchAndQualifyW, 3, 0},
    {"ole32.dll", "CoTaskMemAlloc", ole_CoTaskMemAlloc, 1, 0},
    {"ole32.dll", "CoTaskMemFree", ole_CoTaskMemFree, 1, 0},
    {"ole32.dll", "CoTaskMemRealloc", ole_CoTaskMemRealloc, 2, 0},
    {"ole32.dll", "StringFromCLSID", ole_StringFromCLSID, 2, 0},
    {"ole32.dll", "CLSIDFromProgID", ole_CLSIDFromProgID, 2, 0},
    {"ole32.dll", "CreateBindCtx", ole_CreateBindCtx, 2, 0},
    {"ole32.dll", "CoCreateFreeThreadedMarshaler", ole_CoCreateFreeThreadedMarshaler, 2, 0},
    {"ole32.dll", "CreateStreamOnHGlobal", ole_CreateStreamOnHGlobal, 3, 0},
    {"ole32.dll", "GetHGlobalFromStream", ole_GetHGlobalFromStream, 2, 0},
    {"kernel32.dll", "lstrcmpiW", k32_lstrcmpiW, 2, 0},
    {"kernel32.dll", "lstrcmpA", k32_lstrcmpA, 2, 0},
    {"kernel32.dll", "lstrcmpW", k32_lstrcmpW, 2, 0},
    {"kernel32.dll", "lstrcmpiA", k32_lstrcmpiA, 2, 0},
    {"kernel32.dll", "lstrcpyW", k32_lstrcpyW, 2, 0},
    {"kernel32.dll", "lstrcatW", k32_lstrcatW, 2, 0},
    {"kernel32.dll", "lstrcpynW", k32_lstrcpynW, 3, 0},
    {"kernel32.dll", "lstrcpynA", k32_lstrcpynA, 3, 0},
    {"kernel32.dll", "lstrcpyA", k32_lstrcpyA, 2, 0},
    {"kernel32.dll", "ExpandEnvironmentStringsA", k32_ExpandEnvironmentStringsA, 3, 0},
    {"kernel32.dll", "DuplicateHandle", k32_DuplicateHandle, 7, 0},
    {"kernel32.dll", "LoadLibraryExA", k32_LoadLibraryExA, 3, 0},
    {"kernel32.dll", "SuspendThread", k32_SuspendThread, 1, 0},
    {"kernel32.dll", "GetThreadContext", k32_GetThreadContext, 2, 0},
    {"kernel32.dll", "GetTimeFormatA", k32_GetTimeFormatA, 6, 0},
    {"kernel32.dll", "GetTimeFormatW", k32_GetTimeFormatW, 6, 0},
    {"kernel32.dll", "GetDateFormatA", k32_GetDateFormatA, 6, 0},
    {"kernel32.dll", "GetDateFormatW", k32_GetDateFormatW, 6, 0},
};
RT_EXPORT_IMPORTS(rt_imports_shlwapi, defs);

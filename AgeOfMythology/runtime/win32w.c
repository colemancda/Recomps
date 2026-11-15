/* Wide-character (W) Win32 functions, and the kernel32 functions Age of
 * Mythology needs beyond GunBound's set.
 *
 * The game is a Unicode program (MSLU): it calls the W forms of nearly every
 * API. Where an A handler exists, its W form converts the string arguments to
 * UTF-8 in guest memory and calls it; the host side is UTF-8 throughout.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <strings.h>
#include <time.h>

#include "rt.h"

/* ------------------------------------------------------- UTF-16 strings */
uint32_t rt_wlen(uint32_t a) {
    uint32_t n = 0;
    while (ld16(a + 2 * n)) n++;
    return n;
}

size_t rt_get_wstr(uint32_t a, char *out, size_t cap) {
    size_t o = 0;
    if (!cap) return 0;
    for (uint32_t i = 0; a; i++) {
        uint32_t ch = ld16(a + 2 * i);
        if (!ch) break;
        if (ch >= 0xD800 && ch < 0xDC00) {
            uint32_t lo = ld16(a + 2 * (i + 1));
            if (lo >= 0xDC00 && lo < 0xE000) {
                ch = 0x10000 + ((ch - 0xD800) << 10) + (lo - 0xDC00);
                i++;
            }
        }
        char buf[4];
        size_t k;
        if (ch < 0x80) buf[0] = (char)ch, k = 1;
        else if (ch < 0x800) buf[0] = (char)(0xC0 | ch >> 6), buf[1] = (char)(0x80 | (ch & 0x3F)), k = 2;
        else if (ch < 0x10000)
            buf[0] = (char)(0xE0 | ch >> 12), buf[1] = (char)(0x80 | ((ch >> 6) & 0x3F)),
            buf[2] = (char)(0x80 | (ch & 0x3F)), k = 3;
        else
            buf[0] = (char)(0xF0 | ch >> 18), buf[1] = (char)(0x80 | ((ch >> 12) & 0x3F)),
            buf[2] = (char)(0x80 | ((ch >> 6) & 0x3F)), buf[3] = (char)(0x80 | (ch & 0x3F)), k = 4;
        if (o + k >= cap) break;
        memcpy(out + o, buf, k);
        o += k;
    }
    out[o] = 0;
    return o;
}

/* Decode one UTF-8 character (invalid bytes are taken as Latin-1). */
static uint32_t utf8_next(const unsigned char **pp) {
    const unsigned char *p = *pp;
    uint32_t ch = *p++;
    int extra = ch >= 0xF0 ? 3 : ch >= 0xE0 ? 2 : ch >= 0xC0 ? 1 : 0;
    if (extra) {
        uint32_t v = ch & (0x3F >> extra);
        int ok = 1;
        for (int k = 0; k < extra; k++) {
            if ((p[k] & 0xC0) != 0x80) ok = 0;
        }
        if (ok) {
            for (int k = 0; k < extra; k++) v = (v << 6) | (p[k] & 0x3F);
            p += extra;
            ch = v;
        }
    }
    *pp = p;
    return ch;
}

uint32_t rt_put_wstr(uint32_t a, uint32_t cap, const char *s) {
    const unsigned char *p = (const unsigned char *)s;
    uint32_t n = 0;
    if (!cap) return 0;
    while (*p) {
        uint32_t ch = utf8_next(&p);
        if (ch >= 0x10000) {
            if (n + 2 >= cap) break;
            ch -= 0x10000;
            st16(a + 2 * n++, (uint16_t)(0xD800 + (ch >> 10)));
            st16(a + 2 * n++, (uint16_t)(0xDC00 + (ch & 0x3FF)));
        } else {
            if (n + 1 >= cap) break;
            st16(a + 2 * n++, (uint16_t)ch);
        }
    }
    st16(a + 2 * n, 0);
    return n;
}

uint32_t rt_wstr_dup(const char *s) {
    size_t n = strlen(s) + 1;
    uint32_t a = rt_heap_alloc((uint32_t)(2 * n), 1);
    rt_put_wstr(a, (uint32_t)n, s);
    return a;
}

/* Call the A handler `dll!name` for a W function: the arguments whose bit is
 * set in `wmask` are UTF-16 strings, passed as UTF-8 copies. */
uint32_t rt_call_a(CPU *c, const uint32_t *a, int nargs, const char *dll, const char *name, unsigned wmask) {
    const ImportDef *def = rt_find_import_def(dll, name);
    if (!def) rt_fatal(c, 3, "no handler %s!%s for its W form", dll, name);
    uint32_t args[16], tmp[16] = {0};
    for (int i = 0; i < nargs && i < 16; i++) {
        args[i] = a[i];
        if ((wmask >> i & 1) && a[i]) {
            uint32_t n = rt_wlen(a[i]);
            tmp[i] = rt_heap_alloc(4 * n + 1, 0);
            rt_get_wstr(a[i], (char *)G(tmp[i]), 4 * n + 1);
            args[i] = tmp[i];
        }
    }
    uint32_t r = def->fn(c, args);
    for (int i = 0; i < nargs && i < 16; i++)
        if (tmp[i]) rt_heap_free(tmp[i]);
    return r;
}

/* An A handler that writes a string to (buf, size) and returns its length,
 * called for its W form: the result is widened into the guest's buffer. */
static uint32_t call_a_out(CPU *c, uint32_t *args, int nargs, int buf_arg, int size_arg, const char *name,
                           unsigned wmask) {
    uint32_t wbuf = args[buf_arg], wsize = args[size_arg];
    uint32_t tmp = rt_heap_alloc(1024, 1);
    args[buf_arg] = tmp;
    args[size_arg] = 1024;
    uint32_t r = rt_call_a(c, args, nargs, "kernel32.dll", name, wmask);
    if (r && r < 1024) {
        uint32_t need = 0;
        const unsigned char *p = G(tmp);
        while (*p) {
            uint32_t ch = utf8_next(&p);
            need += ch >= 0x10000 ? 2 : 1;
        }
        if (need + 1 > wsize) r = need + 1;
        else r = rt_put_wstr(wbuf, wsize, (const char *)G(tmp));
    }
    rt_heap_free(tmp);
    return r;
}

/* ------------------------------------------------- W forms of A handlers */
#define WRAP(fn, nargs, aname, mask) \
    H(fn) { return rt_call_a(c, a, nargs, "kernel32.dll", aname, mask); }
WRAP(k32_CreateFileW, 7, "CreateFileA", 1)
WRAP(k32_DeleteFileW, 1, "DeleteFileA", 1)
WRAP(k32_CreateDirectoryW, 2, "CreateDirectoryA", 1)
WRAP(k32_GetFileAttributesW, 1, "GetFileAttributesA", 1)
WRAP(k32_SetFileAttributesW, 2, "SetFileAttributesA", 1)
WRAP(k32_MoveFileW, 2, "MoveFileA", 3)
WRAP(k32_SetEnvironmentVariableW, 2, "SetEnvironmentVariableA", 3)
WRAP(k32_GetModuleHandleW, 1, "GetModuleHandleA", 1)
WRAP(k32_LoadLibraryW, 1, "LoadLibraryA", 1)
WRAP(k32_CreateEventW, 4, "CreateEventA", 8)
WRAP(k32_CreateMutexW, 3, "CreateMutexA", 4)
H(k32_LoadLibraryExW) { return rt_call_a(c, a, 1, "kernel32.dll", "LoadLibraryA", 1); }
H(k32_GetModuleFileNameW) {
    uint32_t args[3] = {a[0], a[1], a[2]};
    return call_a_out(c, args, 3, 1, 2, "GetModuleFileNameA", 0);
}

/* ------------------------------------------------------- directories */
static char cur_dir[260];
static const char *current_dir(void) {
    if (!cur_dir[0]) {
        snprintf(cur_dir, sizeof cur_dir, "%s", g_image.guest_path);
        char *bs = strrchr(cur_dir, '\\');
        if (bs) *bs = 0;
    }
    return cur_dir;
}
static uint32_t put_a(uint32_t buf, uint32_t size, const char *s) {
    uint32_t n = (uint32_t)strlen(s);
    if (n + 1 > size) return n + 1;
    memcpy(G(buf), s, n + 1);
    return n;
}
static uint32_t put_w(uint32_t buf, uint32_t size, const char *s) {
    uint32_t n = (uint32_t)strlen(s); /* ASCII paths */
    if (n + 1 > size) return n + 1;
    return rt_put_wstr(buf, size, s);
}
H(k32_GetCurrentDirectoryA) { UNUSED; return put_a(a[1], a[0], current_dir()); }
H(k32_GetCurrentDirectoryW) { UNUSED; return put_w(a[1], a[0], current_dir()); }
static uint32_t set_cur(const char *d) {
    /* Only the game directory is supported as the current directory. */
    if (strcasecmp(d, current_dir()) && g_opt.verbose) rt_log("[file] SetCurrentDirectory(\"%s\") ignored\n", d);
    return 1;
}
H(k32_SetCurrentDirectoryA) { UNUSED; return set_cur(rt_gstr(a[0])); }
H(k32_SetCurrentDirectoryW) {
    UNUSED;
    char d[600];
    rt_get_wstr(a[0], d, sizeof d);
    return set_cur(d);
}
static void full_path(const char *p, char *out, size_t cap) {
    if ((p[0] && p[1] == ':') || p[0] == '\\' || p[0] == '/') snprintf(out, cap, "%s", p);
    else snprintf(out, cap, "%s\\%s", current_dir(), p);
}
/* GetFullPathName(name, size, buf, &filepart) */
H(k32_GetFullPathNameA) {
    UNUSED;
    char full[600];
    full_path(rt_gstr(a[0]), full, sizeof full);
    uint32_t r = put_a(a[2], a[1], full);
    if (a[3] && r < a[1]) {
        char *bs = strrchr(full, '\\');
        st32(a[3], a[2] + (uint32_t)(bs ? bs + 1 - full : 0));
    }
    return r;
}
H(k32_GetFullPathNameW) {
    UNUSED;
    char p[600], full[600];
    rt_get_wstr(a[0], p, sizeof p);
    full_path(p, full, sizeof full);
    uint32_t r = put_w(a[2], a[1], full);
    if (a[3] && r < a[1]) {
        char *bs = strrchr(full, '\\');
        st32(a[3], a[2] + 2 * (uint32_t)(bs ? bs + 1 - full : 0));
    }
    return r;
}
H(k32_GetWindowsDirectoryA) { UNUSED; return put_a(a[0], a[1], "C:\\WINDOWS"); }
H(k32_GetWindowsDirectoryW) { UNUSED; return put_w(a[0], a[1], "C:\\WINDOWS"); }
H(k32_GetSystemDirectoryW) { UNUSED; return put_w(a[0], a[1], "C:\\WINDOWS\\system32"); }
H(k32_GetTempPathA) { UNUSED; return put_a(a[1], a[0], "C:\\WINDOWS\\Temp\\"); }
H(k32_GetTempPathW) { UNUSED; return put_w(a[1], a[0], "C:\\WINDOWS\\Temp\\"); }
H(k32_GetDriveType) { UNUSED; return 3; /* DRIVE_FIXED */ }
H(k32_GetLogicalDrives) { UNUSED; return 1u << 2; /* C: */ }
H(k32_GetDiskFreeSpaceA) {
    UNUSED;
    if (a[1]) st32(a[1], 8);       /* sectors per cluster */
    if (a[2]) st32(a[2], 512);     /* bytes per sector */
    if (a[3]) st32(a[3], 0x100000); /* free clusters: 4 GiB */
    if (a[4]) st32(a[4], 0x200000);
    return 1;
}

/* ------------------------------------------------------ process, misc */
H(k32_GetCommandLineW) {
    UNUSED;
    static uint32_t w;
    if (!w) w = rt_wstr_dup(rt_gstr(g_proc.cmdline_a));
    return w;
}
H(k32_SetHandleCount) { UNUSED; return a[0]; }
H(k32_DisableThreadLibraryCalls) { UNUSED; return 1; }
H(k32_IsProcessorFeaturePresent) { UNUSED; return 0; }
H(k32_QueryPerformanceFrequency) {
    UNUSED;
    st64(a[0], 10000000); /* QueryPerformanceCounter counts at 10 MHz */
    return 1;
}
H(k32_SetErrorMode) { UNUSED; static uint32_t mode; uint32_t old = mode; mode = a[0]; return old; }
H(k32_OutputDebugStringA) {
    UNUSED;
    if (g_opt.verbose) rt_log("[debug] %s", rt_gstr(a[0]));
    return 0;
}
H(k32_OutputDebugStringW) {
    UNUSED;
    if (g_opt.verbose) {
        char s[1024];
        rt_get_wstr(a[0], s, sizeof s);
        rt_log("[debug] %s", s);
    }
    return 0;
}
H(k32_GetCurrentThread) { UNUSED; return 0xFFFFFFFEu; }
H(k32_SetThreadPriority) { UNUSED; return 1; }
H(k32_GetThreadPriority) { UNUSED; return 0; }
H(k32_GetPriorityClass) { UNUSED; return 0x20; /* NORMAL_PRIORITY_CLASS */ }
H(k32_SetPriorityClass) { UNUSED; return 1; }
H(k32_SleepEx) {
    const ImportDef *d = rt_find_import_def("kernel32.dll", "Sleep");
    d->fn(c, a);
    return 0;
}
H(k32_InterlockedIncrement) { UNUSED; return rt_atomic_add32(a[0], 1) + 1; }
H(k32_InterlockedDecrement) { UNUSED; return rt_atomic_add32(a[0], 0xFFFFFFFFu) - 1; }
H(k32_InterlockedCompareExchange) {
    UNUSED;
    uint32_t expected = a[2];
    __atomic_compare_exchange_n((uint32_t *)G(a[0]), &expected, a[1], 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
    return expected; /* the initial value */
}
H(k32_InterlockedExchangeAdd) { UNUSED; return rt_atomic_add32(a[0], a[1]); }
H(k32_HeapValidate) { UNUSED; return 1; }
H(k32_HeapCompact) { UNUSED; return 0x100000; }
H(k32_HeapWalk) { UNUSED; rt_set_last_error(c, 259); return 0; }
H(k32_Beep) { UNUSED; return 1; }
H(k32_DebugBreak) { rt_fatal(c, 4, "DebugBreak"); }
H(k32_LockFile) { UNUSED; return 1; }
H(k32_GetConsoleMode) { UNUSED; return 0; }
H(k32_zero) { UNUSED; return 0; }
H(k32_one) { UNUSED; return 1; }
H(k32_FormatMessage) {
    UNUSED;
    if (g_opt.verbose) rt_log("[kernel32] FormatMessage(%#x, msg %#x): not provided\n", a[0], a[2]);
    return 0;
}

/* Global/Local memory: a handle is the block's address (GMEM_FIXED behaviour
 * also for movable blocks, which Win32 programs cannot tell apart once locked). */
H(k32_GlobalAlloc) { UNUSED; return rt_heap_alloc(a[1] ? a[1] : 1, (a[0] & 0x40) != 0); }
H(k32_GlobalLock) { UNUSED; return a[0]; }
H(k32_GlobalUnlock) { UNUSED; return 0; }
H(k32_GlobalHandle) { UNUSED; return a[0]; }
H(k32_GlobalFree) {
    UNUSED;
    if (a[0]) rt_heap_free(a[0]);
    return 0;
}
H(k32_GlobalReAlloc) { UNUSED; return rt_heap_realloc(a[0], a[1] ? a[1] : 1, (a[2] & 0x40) != 0); }
H(k32_GlobalMemoryStatus) {
    UNUSED;
    uint32_t p = a[0];
    st32(p, 32);
    st32(p + 4, 30);           /* dwMemoryLoad */
    st32(p + 8, 512u << 20);   /* TotalPhys */
    st32(p + 12, 384u << 20);  /* AvailPhys */
    st32(p + 16, 1024u << 20); /* TotalPageFile */
    st32(p + 20, 900u << 20);
    st32(p + 24, 0x7FFE0000u); /* TotalVirtual */
    st32(p + 28, 0x70000000u);
    return 0;
}

/* -------------------------------------------------------------- locale */
static const struct { uint32_t type; const char *s; } locale_info[] = {
    {0x0001, "0409"},       {0x0002, "English (United States)"}, {0x0003, "ENU"},
    {0x0005, "1"},          {0x0006, "United States"},           {0x0007, "USA"},
    {0x000B, "437"},        {0x000C, ","},                        {0x000D, "1"},
    {0x000E, "."},          {0x000F, ","},                        {0x0010, "3;0"},
    {0x0011, "2"},          {0x0014, "$"},                        {0x0015, "$"},
    {0x0016, "."},          {0x0017, ","},                        {0x0018, "3;0"},
    {0x0019, "2"},          {0x001B, "0"},                        {0x001C, "0"},
    {0x001D, "/"},          {0x001E, ":"},                        {0x001F, "M/d/yyyy"},
    {0x0020, "dddd, MMMM dd, yyyy"}, {0x0021, "0"},              {0x0023, "0"},
    {0x0025, "0"},          {0x0028, "AM"},                       {0x0029, "PM"},
    {0x002A, "Monday"},     {0x002B, "Tuesday"},                  {0x002C, "Wednesday"},
    {0x002D, "Thursday"},   {0x002E, "Friday"},                   {0x002F, "Saturday"},
    {0x0030, "Sunday"},     {0x0031, "Mon"},                      {0x0032, "Tue"},
    {0x0033, "Wed"},        {0x0034, "Thu"},                      {0x0035, "Fri"},
    {0x0036, "Sat"},        {0x0037, "Sun"},                      {0x0038, "January"},
    {0x0039, "February"},   {0x003A, "March"},                    {0x003B, "April"},
    {0x003C, "May"},        {0x003D, "June"},                     {0x003E, "July"},
    {0x003F, "August"},     {0x0040, "September"},                {0x0041, "October"},
    {0x0042, "November"},   {0x0043, "December"},                 {0x0044, "Jan"},
    {0x0045, "Feb"},        {0x0046, "Mar"},                      {0x0047, "Apr"},
    {0x0048, "May"},        {0x0049, "Jun"},                      {0x004A, "Jul"},
    {0x004B, "Aug"},        {0x004C, "Sep"},                      {0x004D, "Oct"},
    {0x004E, "Nov"},        {0x004F, "Dec"},                      {0x0059, "en"},
    {0x005A, "US"},         {0x1001, "English"},                  {0x1002, "United States"},
    {0x1003, "h:mm:ss tt"}, {0x1004, "1252"},                     {0x1009, "1"},
    {0x100C, "0"},
};
static const char *locale_str(uint32_t type) {
    type &= 0xFFFF; /* drop LOCALE_NOUSEROVERRIDE etc. */
    for (size_t i = 0; i < RT_COUNTOF(locale_info); i++)
        if (locale_info[i].type == type) return locale_info[i].s;
    return NULL;
}
/* GetLocaleInfo(lcid, type, buf, cch): characters written including the NUL. */
H(k32_GetLocaleInfoA) {
    const char *s = locale_str(a[1]);
    if (!s) {
        if (g_opt.verbose) rt_log("[kernel32] GetLocaleInfo type %#x not provided\n", a[1]);
        rt_set_last_error(c, 1004); /* ERROR_INVALID_FLAGS */
        return 0;
    }
    uint32_t n = (uint32_t)strlen(s) + 1;
    if (!a[3]) return n;
    if (n > a[3]) {
        rt_set_last_error(c, 122);
        return 0;
    }
    memcpy(G(a[2]), s, n);
    return n;
}
H(k32_GetLocaleInfoW) {
    const char *s = locale_str(a[1]);
    if (!s) {
        if (g_opt.verbose) rt_log("[kernel32] GetLocaleInfo type %#x not provided\n", a[1]);
        rt_set_last_error(c, 1004);
        return 0;
    }
    uint32_t n = (uint32_t)strlen(s) + 1;
    if (!a[3]) return n;
    if (n > a[3]) {
        rt_set_last_error(c, 122);
        return 0;
    }
    rt_put_wstr(a[2], n, s);
    return n;
}
/* GetCPInfoExW(codepage, flags, CPINFOEXW*): single-byte code pages only (CP_ACP is 1252). */
H(k32_GetCPInfoExW) {
    UNUSED;
    uint32_t cp = a[0] == 0 || a[0] == 3 ? 1252 : a[0] == 1 ? 437 : a[0];
    uint32_t p = a[2];
    memset(G(p), 0, 544);
    st32(p, cp == 65001 ? 4 : 1);
    st8(p + 4, '?');
    st16(p + 18, '?');
    st32(p + 20, cp);
    char name[64];
    snprintf(name, sizeof name, "%u (%s)", cp, cp == 1252 ? "ANSI - Latin I" : cp == 437 ? "OEM - United States" : "");
    rt_put_wstr(p + 24, 260, name);
    return 1;
}
H(k32_GetUserDefaultLCID) { UNUSED; return 0x409; }
H(k32_GetEnvironmentVariableA) {
    UNUSED;
    rt_set_last_error(c, 203); /* ERROR_ENVVAR_NOT_FOUND */
    return 0;
}

/* ---------------------------------------------------------------- time */
/* FILETIME (100 ns since 1601) <-> SYSTEMTIME, all in UTC: the guest's local time is UTC. */
static void ft_to_st(uint64_t ft, uint32_t st) {
    time_t t = (time_t)(ft / 10000000ull) - 11644473600ll;
    struct tm tm;
    gmtime_r(&t, &tm);
    st16(st, (uint16_t)(tm.tm_year + 1900));
    st16(st + 2, (uint16_t)(tm.tm_mon + 1));
    st16(st + 4, (uint16_t)tm.tm_wday);
    st16(st + 6, (uint16_t)tm.tm_mday);
    st16(st + 8, (uint16_t)tm.tm_hour);
    st16(st + 10, (uint16_t)tm.tm_min);
    st16(st + 12, (uint16_t)tm.tm_sec);
    st16(st + 14, (uint16_t)(ft / 10000 % 1000));
}
H(k32_FileTimeToSystemTime) { UNUSED; ft_to_st(ld64(a[0]), a[1]); return 1; }
H(k32_SystemTimeToFileTime) {
    UNUSED;
    struct tm tm = {0};
    tm.tm_year = ld16(a[0]) - 1900;
    tm.tm_mon = ld16(a[0] + 2) - 1;
    tm.tm_mday = ld16(a[0] + 6);
    tm.tm_hour = ld16(a[0] + 8);
    tm.tm_min = ld16(a[0] + 10);
    tm.tm_sec = ld16(a[0] + 12);
    time_t t = timegm(&tm);
    st64(a[1], ((uint64_t)t + 11644473600ull) * 10000000ull + (uint64_t)ld16(a[0] + 14) * 10000ull);
    return 1;
}
H(k32_FileTimeCopy) { UNUSED; st64(a[1], ld64(a[0])); return 1; }
H(k32_SystemTimeToTzSpecificLocalTime) { UNUSED; memcpy(G(a[2]), G(a[1]), 16); return 1; }
H(k32_GetSystemTime) {
    UNUSED;
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ft_to_st(((uint64_t)ts.tv_sec + 11644473600ull) * 10000000ull + (uint64_t)ts.tv_nsec / 100, a[0]);
    return 0;
}

H(k32_lstrlenA) { UNUSED; return a[0] ? (uint32_t)strlen(rt_gstr(a[0])) : 0; }
H(k32_lstrlenW) { UNUSED; return a[0] ? rt_wlen(a[0]) : 0; }
H(k32_lstrcatA) {
    UNUSED;
    uint32_t n = (uint32_t)strlen(rt_gstr(a[0]));
    strcpy((char *)G(a[0] + n), rt_gstr(a[1]));
    return a[0];
}

/* OSVERSIONINFOW (276 bytes) or OSVERSIONINFOEXW (284): Windows XP SP2, as GetVersionExA. */
H(k32_GetVersionExW) {
    UNUSED;
    uint32_t p = a[0], size = ld32(p);
    if (size < 276) {
        rt_set_last_error(c, 122);
        return 0;
    }
    st32(p + 4, 5);
    st32(p + 8, 1);
    st32(p + 12, 2600);
    st32(p + 16, 2); /* VER_PLATFORM_WIN32_NT */
    memset(G(p + 20), 0, 256);
    rt_put_wstr(p + 20, 128, "Service Pack 2");
    if (size >= 284) {
        st16(p + 276, 2); /* wServicePackMajor */
        st16(p + 278, 0);
        st16(p + 280, 0x100);
        st8(p + 282, 1); /* VER_NT_WORKSTATION */
        st8(p + 283, 0);
    }
    return 1;
}

/* ------------------------------------------------------- shell folders */
/* SHGetFolderPath(hwnd, csidl, token, flags, path): the guest's special folders.
 * They live under <savedir>/drive_c like every path outside the game folder. */
static const char *folder_path(uint32_t csidl) {
    switch (csidl & 0xFF) {
    case 0x05: return "C:\\Documents and Settings\\Player\\My Documents"; /* CSIDL_PERSONAL */
    case 0x1A: return "C:\\Documents and Settings\\Player\\Application Data"; /* CSIDL_APPDATA */
    case 0x1C: return "C:\\Documents and Settings\\Player\\Local Settings\\Application Data";
    case 0x23: return "C:\\Documents and Settings\\All Users\\Application Data"; /* COMMON_APPDATA */
    case 0x2E: return "C:\\Documents and Settings\\All Users\\Documents"; /* COMMON_DOCUMENTS */
    case 0x10: return "C:\\Documents and Settings\\Player\\Desktop";
    case 0x24: return "C:\\WINDOWS";
    case 0x25: return "C:\\WINDOWS\\system32";
    case 0x26: return "C:\\Program Files";
    default: return NULL;
    }
}
static uint32_t sh_folder(CPU *c, const uint32_t *a, int wide) {
    const char *p = folder_path(a[1]);
    if (!p) {
        rt_log("[shell] SHGetFolderPath: folder %#x not provided\n", a[1]);
        return 0x80070057u; /* E_INVALIDARG */
    }
    /* Create it (and its parents) when asked, and always for the player's own
     * folders, which exist on any real system. */
    const ImportDef *mk = rt_find_import_def("kernel32.dll", "CreateDirectoryA");
    uint32_t tmp = rt_heap_alloc(260, 1);
    for (const char *q = p + 3;; q++) {
        if (*q == '\\' || !*q) {
            memcpy(G(tmp), p, (size_t)(q - p));
            st8(tmp + (uint32_t)(q - p), 0);
            uint32_t args[2] = {tmp, 0};
            mk->fn(c, args);
        }
        if (!*q) break;
    }
    rt_heap_free(tmp);
    if (wide) rt_put_wstr(a[4], 260, p);
    else snprintf((char *)G(a[4]), 260, "%s", p);
    if (g_opt.verbose) rt_log("[shell] SHGetFolderPath(%#x) -> %s\n", a[1], p);
    return 0;
}
H(sh_SHGetFolderPathW) { return sh_folder(c, a, 1); }
H(sh_SHGetFolderPathA) { return sh_folder(c, a, 0); }

static const ImportDef defs[] = {
    {"shfolder.dll", "SHGetFolderPathW", sh_SHGetFolderPathW, 5, 0},
    {"shfolder.dll", "SHGetFolderPathA", sh_SHGetFolderPathA, 5, 0},
    {"shell32.dll", "SHGetFolderPathW", sh_SHGetFolderPathW, 5, 0},
    {"shell32.dll", "SHGetFolderPathA", sh_SHGetFolderPathA, 5, 0},
    {"kernel32.dll", "CreateFileW", k32_CreateFileW, 7, 0},
    {"kernel32.dll", "DeleteFileW", k32_DeleteFileW, 1, 0},
    {"kernel32.dll", "CreateDirectoryW", k32_CreateDirectoryW, 2, 0},
    {"kernel32.dll", "GetFileAttributesW", k32_GetFileAttributesW, 1, 0},
    {"kernel32.dll", "SetFileAttributesW", k32_SetFileAttributesW, 2, 0},
    {"kernel32.dll", "MoveFileW", k32_MoveFileW, 2, 0},
    {"kernel32.dll", "SetEnvironmentVariableW", k32_SetEnvironmentVariableW, 2, 0},
    {"kernel32.dll", "GetModuleHandleW", k32_GetModuleHandleW, 1, 0},
    {"kernel32.dll", "LoadLibraryW", k32_LoadLibraryW, 1, 0},
    {"kernel32.dll", "LoadLibraryExW", k32_LoadLibraryExW, 3, 0},
    {"kernel32.dll", "CreateEventW", k32_CreateEventW, 4, 0},
    {"kernel32.dll", "CreateMutexW", k32_CreateMutexW, 3, 0},
    {"kernel32.dll", "GetModuleFileNameW", k32_GetModuleFileNameW, 3, 0},
    {"kernel32.dll", "GetCurrentDirectoryA", k32_GetCurrentDirectoryA, 2, 0},
    {"kernel32.dll", "GetCurrentDirectoryW", k32_GetCurrentDirectoryW, 2, 0},
    {"kernel32.dll", "SetCurrentDirectoryA", k32_SetCurrentDirectoryA, 1, 0},
    {"kernel32.dll", "SetCurrentDirectoryW", k32_SetCurrentDirectoryW, 1, 0},
    {"kernel32.dll", "GetFullPathNameA", k32_GetFullPathNameA, 4, 0},
    {"kernel32.dll", "GetFullPathNameW", k32_GetFullPathNameW, 4, 0},
    {"kernel32.dll", "GetWindowsDirectoryA", k32_GetWindowsDirectoryA, 2, 0},
    {"kernel32.dll", "GetWindowsDirectoryW", k32_GetWindowsDirectoryW, 2, 0},
    {"kernel32.dll", "GetSystemDirectoryW", k32_GetSystemDirectoryW, 2, 0},
    {"kernel32.dll", "GetTempPathA", k32_GetTempPathA, 2, 0},
    {"kernel32.dll", "GetTempPathW", k32_GetTempPathW, 2, 0},
    {"kernel32.dll", "GetDriveTypeA", k32_GetDriveType, 1, 0},
    {"kernel32.dll", "GetDriveTypeW", k32_GetDriveType, 1, 0},
    {"kernel32.dll", "GetLogicalDrives", k32_GetLogicalDrives, 0, 0},
    {"kernel32.dll", "GetDiskFreeSpaceA", k32_GetDiskFreeSpaceA, 5, 0},
    {"kernel32.dll", "GetCommandLineW", k32_GetCommandLineW, 0, 0},
    {"kernel32.dll", "GetVersionExW", k32_GetVersionExW, 1, 0},
    {"kernel32.dll", "SetHandleCount", k32_SetHandleCount, 1, 0},
    {"kernel32.dll", "DisableThreadLibraryCalls", k32_DisableThreadLibraryCalls, 1, 0},
    {"kernel32.dll", "IsProcessorFeaturePresent", k32_IsProcessorFeaturePresent, 1, 0},
    {"kernel32.dll", "QueryPerformanceFrequency", k32_QueryPerformanceFrequency, 1, 0},
    {"kernel32.dll", "SetErrorMode", k32_SetErrorMode, 1, 0},
    {"kernel32.dll", "OutputDebugStringA", k32_OutputDebugStringA, 1, 0},
    {"kernel32.dll", "OutputDebugStringW", k32_OutputDebugStringW, 1, 0},
    {"kernel32.dll", "GetCurrentThread", k32_GetCurrentThread, 0, 0},
    {"kernel32.dll", "SetThreadPriority", k32_SetThreadPriority, 2, 0},
    {"kernel32.dll", "GetThreadPriority", k32_GetThreadPriority, 1, 0},
    {"kernel32.dll", "GetPriorityClass", k32_GetPriorityClass, 1, 0},
    {"kernel32.dll", "SetPriorityClass", k32_SetPriorityClass, 2, 0},
    {"kernel32.dll", "SleepEx", k32_SleepEx, 2, 0},
    {"kernel32.dll", "InterlockedIncrement", k32_InterlockedIncrement, 1, 0},
    {"kernel32.dll", "InterlockedDecrement", k32_InterlockedDecrement, 1, 0},
    {"kernel32.dll", "InterlockedCompareExchange", k32_InterlockedCompareExchange, 3, 0},
    {"kernel32.dll", "InterlockedExchangeAdd", k32_InterlockedExchangeAdd, 2, 0},
    {"kernel32.dll", "HeapValidate", k32_HeapValidate, 3, 0},
    {"kernel32.dll", "HeapCompact", k32_HeapCompact, 2, 0},
    {"kernel32.dll", "HeapWalk", k32_HeapWalk, 2, 0},
    {"kernel32.dll", "Beep", k32_Beep, 2, 0},
    {"kernel32.dll", "DebugBreak", k32_DebugBreak, 0, 0},
    {"kernel32.dll", "LockFile", k32_LockFile, 5, 0},
    {"kernel32.dll", "UnlockFile", k32_LockFile, 5, 0},
    {"kernel32.dll", "GetConsoleMode", k32_GetConsoleMode, 2, 0},
    {"kernel32.dll", "SetConsoleMode", k32_zero, 2, 0},
    {"kernel32.dll", "SetConsoleCtrlHandler", k32_one, 2, 0},
    {"kernel32.dll", "GetNumberOfConsoleInputEvents", k32_zero, 2, 0},
    {"kernel32.dll", "FormatMessageA", k32_FormatMessage, 7, 0},
    {"kernel32.dll", "FormatMessageW", k32_FormatMessage, 7, 0},
    {"kernel32.dll", "GlobalAlloc", k32_GlobalAlloc, 2, 0},
    {"kernel32.dll", "LocalAlloc", k32_GlobalAlloc, 2, 0},
    {"kernel32.dll", "GlobalLock", k32_GlobalLock, 1, 0},
    {"kernel32.dll", "GlobalUnlock", k32_GlobalUnlock, 1, 0},
    {"kernel32.dll", "GlobalHandle", k32_GlobalHandle, 1, 0},
    {"kernel32.dll", "GlobalFree", k32_GlobalFree, 1, 0},
    {"kernel32.dll", "GlobalReAlloc", k32_GlobalReAlloc, 3, 0},
    {"kernel32.dll", "GlobalMemoryStatus", k32_GlobalMemoryStatus, 1, 0},
    {"kernel32.dll", "GetLocaleInfoA", k32_GetLocaleInfoA, 4, 0},
    {"kernel32.dll", "GetLocaleInfoW", k32_GetLocaleInfoW, 4, 0},
    {"kernel32.dll", "GetUserDefaultLCID", k32_GetUserDefaultLCID, 0, 0},
    {"kernel32.dll", "GetSystemDefaultLCID", k32_GetUserDefaultLCID, 0, 0},
    {"kernel32.dll", "GetUserDefaultLangID", k32_GetUserDefaultLCID, 0, 0},
    {"kernel32.dll", "GetCPInfoExW", k32_GetCPInfoExW, 3, 0},
    {"kernel32.dll", "IsValidLocale", k32_one, 2, 0},
    {"kernel32.dll", "IsValidCodePage", k32_one, 1, 0},
    {"kernel32.dll", "IsDBCSLeadByteEx", k32_zero, 2, 0},
    {"kernel32.dll", "EnumSystemLocalesA", k32_one, 2, 0},
    {"kernel32.dll", "GetEnvironmentVariableA", k32_GetEnvironmentVariableA, 3, 0},
    {"kernel32.dll", "GetEnvironmentVariableW", k32_GetEnvironmentVariableA, 3, 0},
    {"kernel32.dll", "FileTimeToSystemTime", k32_FileTimeToSystemTime, 2, 0},
    {"kernel32.dll", "SystemTimeToFileTime", k32_SystemTimeToFileTime, 2, 0},
    {"kernel32.dll", "FileTimeToLocalFileTime", k32_FileTimeCopy, 2, 0},
    {"kernel32.dll", "LocalFileTimeToFileTime", k32_FileTimeCopy, 2, 0},
    {"kernel32.dll", "SystemTimeToTzSpecificLocalTime", k32_SystemTimeToTzSpecificLocalTime, 3, 0},
    {"kernel32.dll", "GetSystemTime", k32_GetSystemTime, 1, 0},
    {"kernel32.dll", "SetLocalTime", k32_zero, 1, 0},
    {"kernel32.dll", "lstrlenA", k32_lstrlenA, 1, 0},
    {"kernel32.dll", "lstrcatA", k32_lstrcatA, 2, 0},
    {"kernel32.dll", "lstrlenW", k32_lstrlenW, 1, 0},
};
RT_EXPORT_IMPORTS(rt_imports_win32w, defs);

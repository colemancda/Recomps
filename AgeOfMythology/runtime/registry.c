/* advapi32 registry, backed by a text file in the format of a .reg export:
 *
 *   [HKEY_CURRENT_USER\Software\Microsoft\Microsoft Games\Age of Mythology Expansion Pack\1.0]
 *   "SystemInitialization"="1"
 *   "FIRSTRUN"=dword:00000001
 *   "Blob"=hex:01,02,03
 *
 * The file is read at the first registry call and rewritten whenever the guest
 * changes a value. When it does not exist it is created with the key the
 * installer makes; the game writes everything else itself. Strings are kept
 * as UTF-8; the W functions convert.
 */
#define _GNU_SOURCE
#include <ctype.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <strings.h>

#include "rt.h"

#define REG_SZ 1u
#define REG_BINARY 3u
#define REG_DWORD 4u
#define ERROR_FILE_NOT_FOUND 2u
#define ERROR_INVALID_HANDLE 6u
#define ERROR_MORE_DATA 234u
#define ERROR_NO_MORE_ITEMS 259u

#define HKEY_ROOT_FIRST 0x80000000u
#define HKEY_OPEN_BASE 0x00E00000u

typedef struct RegValue {
    char *name;
    uint32_t type, len;
    uint8_t *data;
    struct RegValue *next;
} RegValue;

typedef struct RegKey {
    char *path; /* e.g. HKEY_CURRENT_USER\Software\Microsoft\Microsoft Games\... */
    RegValue *values;
    struct RegKey *next;
} RegKey;

static RegKey *keys;
static char *open_keys[256];
static pthread_mutex_t reg_mu = PTHREAD_MUTEX_INITIALIZER;
static int loaded;

static const char *const root_names[] = {
    "HKEY_CLASSES_ROOT", "HKEY_CURRENT_USER", "HKEY_LOCAL_MACHINE", "HKEY_USERS",
};

static RegKey *key_find(const char *path) {
    for (RegKey *k = keys; k; k = k->next)
        if (!strcasecmp(k->path, path)) return k;
    return NULL;
}

static RegKey *key_create(const char *path) {
    RegKey *k = key_find(path);
    if (k) return k;
    k = calloc(1, sizeof *k);
    k->path = strdup(path);
    /* keep file order: append */
    RegKey **pp = &keys;
    while (*pp) pp = &(*pp)->next;
    *pp = k;
    return k;
}

static RegValue *value_find(RegKey *k, const char *name) {
    for (RegValue *v = k->values; v; v = v->next)
        if (!strcasecmp(v->name, name)) return v;
    return NULL;
}

static void value_set(RegKey *k, const char *name, uint32_t type, const void *data, uint32_t len) {
    RegValue *v = value_find(k, name);
    if (!v) {
        v = calloc(1, sizeof *v);
        v->name = strdup(name);
        RegValue **pp = &k->values;
        while (*pp) pp = &(*pp)->next;
        *pp = v;
    }
    free(v->data);
    v->type = type;
    v->len = len;
    v->data = malloc(len ? len : 1);
    memcpy(v->data, data, len);
}

static void set_sz(RegKey *k, const char *name, const char *s) { value_set(k, name, REG_SZ, s, (uint32_t)strlen(s) + 1); }
static void set_dword(RegKey *k, const char *name, uint32_t v) { value_set(k, name, REG_DWORD, &v, 4); }

static void reg_save(void) {
    FILE *f = fopen(g_opt.regfile, "w");
    if (!f) {
        rt_log("recomp: cannot write the registry file %s\n", g_opt.regfile);
        return;
    }
    fprintf(f, "; Registry of the recompiled game. Edit while the game is not running.\n");
    for (RegKey *k = keys; k; k = k->next) {
        fprintf(f, "\n[%s]\n", k->path);
        for (RegValue *v = k->values; v; v = v->next) {
            fputc('"', f);
            for (const char *p = v->name; *p; p++) {
                if (*p == '"' || *p == '\\') fputc('\\', f);
                fputc(*p, f);
            }
            fputs("\"=", f);
            if (v->type == REG_SZ) {
                fputc('"', f);
                for (uint32_t i = 0; i < v->len && v->data[i]; i++) {
                    if (v->data[i] == '"' || v->data[i] == '\\') fputc('\\', f);
                    fputc(v->data[i], f);
                }
                fputs("\"\n", f);
            } else if (v->type == REG_DWORD && v->len == 4) {
                uint32_t d;
                memcpy(&d, v->data, 4);
                fprintf(f, "dword:%08x\n", d);
            } else {
                if (v->type == REG_BINARY) fputs("hex:", f);
                else fprintf(f, "hex(%x):", v->type);
                for (uint32_t i = 0; i < v->len; i++) fprintf(f, i ? ",%02x" : "%02x", v->data[i]);
                fputc('\n', f);
            }
        }
    }
    fclose(f);
}

/* Parse a quoted string with backslash escapes starting at *pp (after the opening quote). */
static char *parse_quoted(char **pp) {
    char *p = *pp, *out = malloc(strlen(p) + 1);
    size_t o = 0;
    while (*p && *p != '"') {
        if (*p == '\\' && p[1]) p++;
        out[o++] = *p++;
    }
    out[o] = 0;
    if (*p == '"') p++;
    *pp = p;
    return out;
}

/* What the installer creates (an empty key); the game adds its own values. */
static const char *const install_keys[] = {
    "HKEY_LOCAL_MACHINE\\Software\\Microsoft\\Microsoft Games\\Age of Mythology Expansion Pack\\1.0",
    "HKEY_LOCAL_MACHINE\\Software\\Microsoft\\Microsoft Games\\Age of Mythology\\1.0",
};

static void reg_load(void) {
    if (loaded) return;
    loaded = 1;
    FILE *f = fopen(g_opt.regfile, "r");
    if (!f) {
        for (size_t i = 0; i < RT_COUNTOF(install_keys); i++) key_create(install_keys[i]);
        rt_log("recomp: no registry file at %s; created one with the installer's keys\n", g_opt.regfile);
        reg_save();
        return;
    }
    char line[4096];
    RegKey *cur = NULL;
    while (fgets(line, sizeof line, f)) {
        char *p = line;
        while (isspace((unsigned char)*p)) p++;
        size_t n = strlen(p);
        while (n && isspace((unsigned char)p[n - 1])) p[--n] = 0;
        if (!*p || *p == ';' || *p == '#') continue;
        if (*p == '[') {
            char *e = strrchr(p, ']');
            if (e) *e = 0;
            cur = key_create(p + 1);
            continue;
        }
        if (*p != '"' || !cur) continue;
        p++;
        char *name = parse_quoted(&p);
        while (isspace((unsigned char)*p)) p++;
        if (*p == '=') {
            p++;
            while (isspace((unsigned char)*p)) p++;
            if (*p == '"') {
                p++;
                char *val = parse_quoted(&p);
                set_sz(cur, name, val);
                free(val);
            } else if (!strncasecmp(p, "dword:", 6)) {
                set_dword(cur, name, (uint32_t)strtoul(p + 6, NULL, 16));
            } else if (!strncasecmp(p, "hex", 3)) {
                uint32_t type = REG_BINARY;
                p += 3;
                if (*p == '(') {
                    type = (uint32_t)strtoul(p + 1, &p, 16);
                    if (*p == ')') p++;
                }
                if (*p == ':') p++;
                uint8_t buf[2048];
                uint32_t len = 0;
                while (*p && len < sizeof buf) {
                    char *e;
                    unsigned long b = strtoul(p, &e, 16);
                    if (e == p) break;
                    buf[len++] = (uint8_t)b;
                    p = e;
                    while (*p == ',' || isspace((unsigned char)*p)) p++;
                }
                value_set(cur, name, type, buf, len);
            }
        }
        free(name);
    }
    fclose(f);
}

/* Path of a key handle, or NULL. Under reg_mu. */
static const char *hkey_path(uint32_t h) {
    if (h >= HKEY_ROOT_FIRST && h - HKEY_ROOT_FIRST < RT_COUNTOF(root_names)) return root_names[h - HKEY_ROOT_FIRST];
    uint32_t i = h - HKEY_OPEN_BASE;
    if (h >= HKEY_OPEN_BASE && i < RT_COUNTOF(open_keys)) return open_keys[i];
    return NULL;
}

static uint32_t hkey_open(const char *path) {
    for (uint32_t i = 0; i < RT_COUNTOF(open_keys); i++) {
        if (!open_keys[i]) {
            open_keys[i] = strdup(path);
            return HKEY_OPEN_BASE + i;
        }
    }
    return 0;
}

static int join_path(char *out, size_t cap, uint32_t hkey, uint32_t subkey) {
    const char *base = hkey_path(hkey);
    if (!base) return 0;
    const char *sub = subkey ? rt_gstr(subkey) : "";
    while (*sub == '\\') sub++;
    if (*sub) snprintf(out, cap, "%s\\%s", base, sub);
    else snprintf(out, cap, "%s", base);
    size_t n = strlen(out);
    while (n && out[n - 1] == '\\') out[--n] = 0;
    return 1;
}

/* A key exists if it has an entry of its own or any key below it does. */
static int key_exists(const char *path) {
    size_t n = strlen(path);
    for (RegKey *k = keys; k; k = k->next)
        if (!strncasecmp(k->path, path, n) && (k->path[n] == 0 || k->path[n] == '\\')) return 1;
    for (size_t i = 0; i < RT_COUNTOF(root_names); i++)
        if (!strcasecmp(root_names[i], path)) return 1;
    return 0;
}

/* RegOpenKeyExA(key, subKey, options, access, &result) */
H(adv_RegOpenKeyExA) {
    UNUSED;
    char path[1024];
    uint32_t r = ERROR_FILE_NOT_FOUND;
    pthread_mutex_lock(&reg_mu);
    reg_load();
    if (!join_path(path, sizeof path, a[0], a[1])) {
        r = ERROR_INVALID_HANDLE;
    } else if (key_exists(path)) {
        uint32_t h = hkey_open(path);
        if (h) {
            st32(a[4], h);
            r = 0;
        }
    }
    pthread_mutex_unlock(&reg_mu);
    if (g_opt.verbose || g_trace_imports) rt_log("[reg] open %s -> %s\n", path, r ? "not found" : "ok");
    return r;
}

/* RegCreateKeyExA(key, subKey, reserved, class, options, access, security, &result, &disposition) */
H(adv_RegCreateKeyExA) {
    UNUSED;
    char path[1024];
    uint32_t r = ERROR_INVALID_HANDLE;
    pthread_mutex_lock(&reg_mu);
    reg_load();
    if (join_path(path, sizeof path, a[0], a[1])) {
        int existed = key_exists(path);
        key_create(path);
        uint32_t h = hkey_open(path);
        if (h) {
            st32(a[7], h);
            if (a[8]) st32(a[8], existed ? 2 : 1);
            if (!existed) reg_save();
            r = 0;
        }
    }
    pthread_mutex_unlock(&reg_mu);
    return r;
}

H(adv_RegCloseKey) {
    UNUSED;
    uint32_t i = a[0] - HKEY_OPEN_BASE;
    pthread_mutex_lock(&reg_mu);
    if (a[0] >= HKEY_OPEN_BASE && i < RT_COUNTOF(open_keys) && open_keys[i]) {
        free(open_keys[i]);
        open_keys[i] = NULL;
    }
    pthread_mutex_unlock(&reg_mu);
    return 0;
}

/* RegQueryValueExA(key, name, reserved, &type, data, &size) */
H(adv_RegQueryValueExA) {
    UNUSED;
    uint32_t r = ERROR_FILE_NOT_FOUND;
    const char *name = a[1] ? rt_gstr(a[1]) : "";
    pthread_mutex_lock(&reg_mu);
    reg_load();
    const char *path = hkey_path(a[0]);
    RegKey *k = path ? key_find(path) : NULL;
    RegValue *v = k ? value_find(k, name) : NULL;
    if (!path) {
        r = ERROR_INVALID_HANDLE;
    } else if (v) {
        if (a[3]) st32(a[3], v->type);
        r = 0;
        if (a[5]) {
            uint32_t cap = ld32(a[5]);
            st32(a[5], v->len);
            if (a[4]) {
                if (cap >= v->len) memcpy(G(a[4]), v->data, v->len);
                else r = ERROR_MORE_DATA;
            }
        }
    }
    if (g_opt.verbose || g_trace_imports)
        rt_log("[reg] query %s \"%s\" -> %s\n", path ? path : "?", name, r == 0 ? "ok" : r == ERROR_MORE_DATA ? "buffer too small" : "not found");
    pthread_mutex_unlock(&reg_mu);
    return r;
}

/* RegSetValueExA(key, name, reserved, type, data, size) */
H(adv_RegSetValueExA) {
    UNUSED;
    uint32_t r = ERROR_INVALID_HANDLE;
    pthread_mutex_lock(&reg_mu);
    reg_load();
    const char *path = hkey_path(a[0]);
    if (path) {
        uint32_t len = a[5];
        if (a[3] == REG_SZ && a[4]) {
            /* store exactly one terminator whatever length the caller counted */
            uint32_t n = 0;
            while (n < len && ld8(a[4] + n)) n++;
            char *tmp = malloc(n + 1);
            memcpy(tmp, G(a[4]), n);
            tmp[n] = 0;
            value_set(key_create(path), a[1] ? rt_gstr(a[1]) : "", REG_SZ, tmp, n + 1);
            free(tmp);
        } else {
            value_set(key_create(path), a[1] ? rt_gstr(a[1]) : "", a[3], G(a[4]), len);
        }
        reg_save();
        r = 0;
    }
    pthread_mutex_unlock(&reg_mu);
    return r;
}

H(adv_RegDeleteValueA) {
    UNUSED;
    uint32_t r = ERROR_FILE_NOT_FOUND;
    pthread_mutex_lock(&reg_mu);
    reg_load();
    const char *path = hkey_path(a[0]);
    RegKey *k = path ? key_find(path) : NULL;
    if (k) {
        for (RegValue **pp = &k->values; *pp; pp = &(*pp)->next) {
            if (!strcasecmp((*pp)->name, a[1] ? rt_gstr(a[1]) : "")) {
                RegValue *v = *pp;
                *pp = v->next;
                free(v->name);
                free(v->data);
                free(v);
                reg_save();
                r = 0;
                break;
            }
        }
    }
    pthread_mutex_unlock(&reg_mu);
    return r;
}

/* RegEnumKeyExA(key, index, name, &nameLen, reserved, class, &classLen, &lastWrite) */
H(adv_RegEnumKeyExA) {
    UNUSED;
    uint32_t r = ERROR_NO_MORE_ITEMS;
    pthread_mutex_lock(&reg_mu);
    reg_load();
    const char *path = hkey_path(a[0]);
    if (!path) {
        r = ERROR_INVALID_HANDLE;
    } else {
        size_t n = strlen(path);
        char seen[64][128];
        unsigned nseen = 0;
        for (RegKey *k = keys; k && r == ERROR_NO_MORE_ITEMS; k = k->next) {
            if (strncasecmp(k->path, path, n) || k->path[n] != '\\') continue;
            char child[128];
            snprintf(child, sizeof child, "%.*s", (int)strcspn(k->path + n + 1, "\\"), k->path + n + 1);
            unsigned i;
            for (i = 0; i < nseen; i++)
                if (!strcasecmp(seen[i], child)) break;
            if (i < nseen) continue;
            if (nseen == a[1]) {
                uint32_t cap = ld32(a[3]);
                if (strlen(child) + 1 > cap) {
                    r = ERROR_MORE_DATA;
                } else {
                    strcpy((char *)G(a[2]), child);
                    st32(a[3], (uint32_t)strlen(child));
                    if (a[6]) st32(a[6], 0);
                    if (a[5] && a[6]) st8(a[5], 0);
                    r = 0;
                }
                break;
            }
            if (nseen < 64) snprintf(seen[nseen++], 128, "%s", child);
        }
    }
    pthread_mutex_unlock(&reg_mu);
    return r;
}

/* ------------------------------------------------------------ W forms */
#define REG_EXPAND_SZ 2u
#define REG_MULTI_SZ 7u
static int is_string(uint32_t type) { return type == REG_SZ || type == REG_EXPAND_SZ || type == REG_MULTI_SZ; }

/* n UTF-16 units (NULs included) -> UTF-8 bytes; returns the length written. */
static uint32_t w_to_utf8(uint32_t src, uint32_t n, char *out, uint32_t cap) {
    uint32_t o = 0;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t ch = ld16(src + 2 * i);
        char tmp[4];
        uint32_t k;
        if (ch < 0x80) tmp[0] = (char)ch, k = 1;
        else if (ch < 0x800) tmp[0] = (char)(0xC0 | ch >> 6), tmp[1] = (char)(0x80 | (ch & 0x3F)), k = 2;
        else tmp[0] = (char)(0xE0 | ch >> 12), tmp[1] = (char)(0x80 | ((ch >> 6) & 0x3F)), tmp[2] = (char)(0x80 | (ch & 0x3F)), k = 3;
        if (o + k > cap) break;
        memcpy(out + o, tmp, k);
        o += k;
    }
    return o;
}
/* UTF-8 bytes (NULs included) -> UTF-16 units into guest memory (dst 0: count only). */
static uint32_t utf8_to_w(const uint8_t *p, uint32_t len, uint32_t dst) {
    uint32_t n = 0;
    for (uint32_t i = 0; i < len;) {
        uint32_t ch = p[i++];
        if (ch >= 0xE0 && i + 1 < len) ch = ((ch & 0x0F) << 12) | ((p[i] & 0x3Fu) << 6) | (p[i + 1] & 0x3Fu), i += 2;
        else if (ch >= 0xC0 && i < len) ch = ((ch & 0x1F) << 6) | (p[i] & 0x3Fu), i += 1;
        if (dst) st16(dst + 2 * n, (uint16_t)ch);
        n++;
    }
    return n;
}

H(adv_RegOpenKeyExW) { return rt_call_a(c, a, 5, "advapi32.dll", "RegOpenKeyExA", 2); }
H(adv_RegOpenKeyA) {
    uint32_t args[5] = {a[0], a[1], 0, 0x20019, a[2]};
    return adv_RegOpenKeyExA(c, args);
}
H(adv_RegOpenKeyW) {
    uint32_t args[5] = {a[0], a[1], 0, 0x20019, a[2]};
    return rt_call_a(c, args, 5, "advapi32.dll", "RegOpenKeyExA", 2);
}
H(adv_RegCreateKeyExW) { return rt_call_a(c, a, 9, "advapi32.dll", "RegCreateKeyExA", 2 | 8); }
H(adv_RegCreateKeyA) {
    uint32_t args[9] = {a[0], a[1], 0, 0, 0, 0xF003F, 0, a[2], 0};
    return adv_RegCreateKeyExA(c, args);
}
H(adv_RegCreateKeyW) {
    uint32_t args[9] = {a[0], a[1], 0, 0, 0, 0xF003F, 0, a[2], 0};
    return rt_call_a(c, args, 9, "advapi32.dll", "RegCreateKeyExA", 2);
}
H(adv_RegDeleteValueW) { return rt_call_a(c, a, 2, "advapi32.dll", "RegDeleteValueA", 2); }

static void wname(uint32_t w, char *out, size_t cap) {
    if (w) rt_get_wstr(w, out, cap);
    else out[0] = 0;
}

/* Copy value v to the W caller's (type*, data, size*): strings become UTF-16. Under reg_mu. */
static uint32_t put_value_w(RegValue *v, uint32_t ptype, uint32_t data, uint32_t psize) {
    if (ptype) st32(ptype, v->type);
    uint32_t need = is_string(v->type) ? 2 * utf8_to_w(v->data, v->len, 0) : v->len;
    if (!psize) return data ? ERROR_INVALID_HANDLE : 0;
    uint32_t cap = ld32(psize);
    st32(psize, need);
    if (!data) return 0;
    if (cap < need) return ERROR_MORE_DATA;
    if (is_string(v->type)) utf8_to_w(v->data, v->len, data);
    else memcpy(G(data), v->data, v->len);
    return 0;
}

/* RegQueryValueExW(key, name, reserved, &type, data, &size) */
H(adv_RegQueryValueExW) {
    UNUSED;
    char name[256];
    wname(a[1], name, sizeof name);
    uint32_t r = ERROR_FILE_NOT_FOUND;
    pthread_mutex_lock(&reg_mu);
    reg_load();
    const char *path = hkey_path(a[0]);
    RegKey *k = path ? key_find(path) : NULL;
    RegValue *v = k ? value_find(k, name) : NULL;
    if (!path) r = ERROR_INVALID_HANDLE;
    else if (v) r = put_value_w(v, a[3], a[4], a[5]);
    if (g_opt.verbose || g_trace_imports)
        rt_log("[reg] query %s \"%s\" -> %s\n", path ? path : "?", name, r == 0 ? "ok" : r == ERROR_MORE_DATA ? "buffer too small" : "not found");
    pthread_mutex_unlock(&reg_mu);
    return r;
}

/* RegSetValueExW(key, name, reserved, type, data, size) */
H(adv_RegSetValueExW) {
    UNUSED;
    char name[256];
    wname(a[1], name, sizeof name);
    uint32_t r = ERROR_INVALID_HANDLE;
    pthread_mutex_lock(&reg_mu);
    reg_load();
    const char *path = hkey_path(a[0]);
    if (path) {
        if (is_string(a[3]) && a[4]) {
            uint32_t n = a[5] / 2;
            if (a[3] == REG_SZ) { /* exactly one terminator */
                uint32_t k = 0;
                while (k < n && ld16(a[4] + 2 * k)) k++;
                n = k + 1;
            }
            char *tmp = malloc(3 * n + 1);
            uint32_t len = w_to_utf8(a[4], n, tmp, 3 * n + 1);
            if (a[3] == REG_SZ) tmp[len - 1] = 0;
            value_set(key_create(path), name, a[3], tmp, len);
            free(tmp);
        } else {
            value_set(key_create(path), name, a[3], a[4] ? G(a[4]) : (const uint8_t *)"", a[4] ? a[5] : 0);
        }
        reg_save();
        r = 0;
    }
    pthread_mutex_unlock(&reg_mu);
    if (g_opt.verbose) rt_log("[reg] set %s \"%s\"\n", path ? path : "?", name);
    return r;
}

/* RegEnumKeyExW(key, index, name, &nameLen, reserved, class, &classLen, &lastWrite) */
H(adv_RegEnumKeyExW) {
    uint32_t tmp = rt_heap_alloc(260 + 4, 1);
    uint32_t args[8] = {a[0], a[1], tmp, tmp + 260, 0, 0, 0, 0};
    st32(tmp + 260, 255);
    uint32_t r = adv_RegEnumKeyExA(c, args);
    if (r == 0) {
        uint32_t n = (uint32_t)strlen((const char *)G(tmp));
        if (n + 1 > ld32(a[3])) r = ERROR_MORE_DATA;
        else {
            rt_put_wstr(a[2], n + 1, (const char *)G(tmp));
            st32(a[3], n);
            if (a[6]) st32(a[6], 0);
        }
    }
    rt_heap_free(tmp);
    return r;
}

/* RegEnumValueW(key, index, name, &nameLen, reserved, &type, data, &dataLen) */
H(adv_RegEnumValueW) {
    UNUSED;
    uint32_t r = ERROR_NO_MORE_ITEMS;
    pthread_mutex_lock(&reg_mu);
    reg_load();
    const char *path = hkey_path(a[0]);
    RegKey *k = path ? key_find(path) : NULL;
    if (!path) r = ERROR_INVALID_HANDLE;
    RegValue *v = k ? k->values : NULL;
    for (uint32_t i = 0; v && i < a[1]; i++) v = v->next;
    if (v) {
        uint32_t n = (uint32_t)strlen(v->name);
        if (n + 1 > ld32(a[3])) r = ERROR_MORE_DATA;
        else {
            rt_put_wstr(a[2], n + 1, v->name);
            st32(a[3], n);
            r = put_value_w(v, a[5], a[6], a[7]);
        }
    }
    pthread_mutex_unlock(&reg_mu);
    return r;
}

/* RegQueryInfoKeyW(key, class, &classLen, reserved, &subKeys, &maxSubKeyLen, &maxClassLen,
 *                  &values, &maxValueNameLen, &maxValueLen, &securityDescriptor, &lastWrite) */
H(adv_RegQueryInfoKeyW) {
    UNUSED;
    pthread_mutex_lock(&reg_mu);
    reg_load();
    const char *path = hkey_path(a[0]);
    if (!path) {
        pthread_mutex_unlock(&reg_mu);
        return ERROR_INVALID_HANDLE;
    }
    RegKey *k = key_find(path);
    uint32_t nsub = 0, maxsub = 0, nval = 0, maxname = 0, maxval = 0;
    size_t n = strlen(path);
    char seen[64][128];
    for (RegKey *q = keys; q; q = q->next) {
        if (strncasecmp(q->path, path, n) || q->path[n] != '\\') continue;
        char child[128];
        snprintf(child, sizeof child, "%.*s", (int)strcspn(q->path + n + 1, "\\"), q->path + n + 1);
        uint32_t i;
        for (i = 0; i < nsub && i < 64; i++)
            if (!strcasecmp(seen[i], child)) break;
        if (i < nsub) continue;
        if (nsub < 64) snprintf(seen[nsub], 128, "%s", child);
        nsub++;
        if (strlen(child) > maxsub) maxsub = (uint32_t)strlen(child);
    }
    for (RegValue *v = k ? k->values : NULL; v; v = v->next) {
        nval++;
        if (strlen(v->name) > maxname) maxname = (uint32_t)strlen(v->name);
        uint32_t len = is_string(v->type) ? 2 * v->len : v->len;
        if (len > maxval) maxval = len;
    }
    pthread_mutex_unlock(&reg_mu);
    if (a[2]) st32(a[2], 0);
    if (a[1] && a[2]) st16(a[1], 0);
    if (a[4]) st32(a[4], nsub);
    if (a[5]) st32(a[5], maxsub);
    if (a[6]) st32(a[6], 0);
    if (a[7]) st32(a[7], nval);
    if (a[8]) st32(a[8], maxname);
    if (a[9]) st32(a[9], maxval);
    if (a[10]) st32(a[10], 0);
    if (a[11]) memset(G(a[11]), 0, 8);
    return 0;
}

static const ImportDef defs[] = {
    {"advapi32.dll", "RegOpenKeyExA", adv_RegOpenKeyExA, 5, 0},
    {"advapi32.dll", "RegOpenKeyExW", adv_RegOpenKeyExW, 5, 0},
    {"advapi32.dll", "RegOpenKeyA", adv_RegOpenKeyA, 3, 0},
    {"advapi32.dll", "RegOpenKeyW", adv_RegOpenKeyW, 3, 0},
    {"advapi32.dll", "RegCreateKeyExW", adv_RegCreateKeyExW, 9, 0},
    {"advapi32.dll", "RegCreateKeyA", adv_RegCreateKeyA, 3, 0},
    {"advapi32.dll", "RegCreateKeyW", adv_RegCreateKeyW, 3, 0},
    {"advapi32.dll", "RegDeleteValueW", adv_RegDeleteValueW, 2, 0},
    {"advapi32.dll", "RegQueryValueExW", adv_RegQueryValueExW, 6, 0},
    {"advapi32.dll", "RegSetValueExW", adv_RegSetValueExW, 6, 0},
    {"advapi32.dll", "RegEnumKeyExW", adv_RegEnumKeyExW, 8, 0},
    {"advapi32.dll", "RegEnumValueW", adv_RegEnumValueW, 8, 0},
    {"advapi32.dll", "RegQueryInfoKeyW", adv_RegQueryInfoKeyW, 12, 0},
    {"advapi32.dll", "RegCreateKeyExA", adv_RegCreateKeyExA, 9, 0},
    {"advapi32.dll", "RegCloseKey", adv_RegCloseKey, 1, 0},
    {"advapi32.dll", "RegQueryValueExA", adv_RegQueryValueExA, 6, 0},
    {"advapi32.dll", "RegSetValueExA", adv_RegSetValueExA, 6, 0},
    {"advapi32.dll", "RegDeleteValueA", adv_RegDeleteValueA, 2, 0},
    {"advapi32.dll", "RegEnumKeyExA", adv_RegEnumKeyExA, 8, 0},
};
RT_EXPORT_IMPORTS(rt_imports_registry, defs);

/* The app configuration (.icf) embedded in s3e_data after the XE3U header.
 *
 *   [GROUP]                 group names and keys are case-insensitive
 *   Key = value  # comment  later definitions override earlier ones
 *   {OS=IPHONE} ... {}      conditional blocks: a condition holds until the next
 *                           condition or {}; group headers inside it belong to it.
 *                           We are OS=IPHONE; ID=ANY, CLASS=ANY and OS=ANY hold;
 *                           {[GROUP] Key==value} (or !=) compares a setting
 *                           defined earlier
 *   Key = [GROUP]Other + 4  integer values can be expressions over other settings
 */
#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "rt.h"

#define S3E_DATA 0x00a0e000u

typedef struct { char *group, *key, *value; } Setting;
static Setting *settings;
static int nsettings, cap;

static char *trim(char *s) {
    while (isspace((unsigned char)*s)) s++;
    char *e = s + strlen(s);
    while (e > s && isspace((unsigned char)e[-1])) *--e = 0;
    return s;
}

/* A setting-comparison condition: "[GROUP] Key==value" or "!=". */
static int compare_condition(const char *cond) {
    char group[64], key[128], value[128];
    const char *e = strchr(cond, ']');
    if (!e) return 0;
    snprintf(group, sizeof group, "%.*s", (int)(e - cond - 1), cond + 1);
    const char *op = strstr(e, "==");
    int negate = 0;
    if (!op) { op = strstr(e, "!="); negate = 1; }
    if (!op) return 0;
    snprintf(key, sizeof key, "%.*s", (int)(op - e - 1), e + 1);
    snprintf(value, sizeof value, "%s", op + 2);
    char *k = trim(key), *v = trim(value);
    size_t n = strlen(v);
    if (n >= 2 && v[0] == '"' && v[n - 1] == '"') { v[n - 1] = 0; v++; }
    const char *have = config_get(group, k);
    int eq = have && !strcasecmp(have, v);
    return negate ? !eq : eq;
}

/* {OS=IPHONE}, {ID=ANY}, {OS=BREW, OS=IPHONE}, {[GX] Rasterisation==SW}, {} */
static int condition_true(const char *cond) {
    if (!*cond) return 1;                        /* {} ends a block */
    if (*cond == '[') return compare_condition(cond);
    char buf[256];
    snprintf(buf, sizeof buf, "%s", cond);
    char *save;
    for (char *t = strtok_r(buf, ",", &save); t; t = strtok_r(NULL, ",", &save)) {
        t = trim(t);
        char *eq = strchr(t, '=');
        if (!eq) continue;
        char *what = trim(eq + 1);
        if (!strcasecmp(what, "ANY")) return 1;
        if (!strncasecmp(t, "OS=", 3) && !strcasecmp(what, "IPHONE")) return 1;
    }
    return 0;
}

static void add(const char *group, const char *key, const char *value) {
    if (nsettings == cap) {
        cap = cap ? cap * 2 : 256;
        settings = realloc(settings, cap * sizeof *settings);
    }
    settings[nsettings++] = (Setting){strdup(group), strdup(key), strdup(value)};
}

void config_init(void) {
    if (ld32(S3E_DATA) != 0x55334558u) return;
    uint32_t off = ld32(S3E_DATA + 0x2c), len = ld32(S3E_DATA + 0x30);
    char *text = malloc(len + 1);
    memcpy(text, G(S3E_DATA + off), len);
    text[len] = 0;
    char group[64] = "";
    int active = 1;
    char *save;
    for (char *line = strtok_r(text, "\r\n", &save); line; line = strtok_r(NULL, "\r\n", &save)) {
        char *l = trim(line);
        if (!*l || *l == '#') continue;
        if (*l == '{') {
            char *e = strchr(l, '}');
            if (e) *e = 0;
            active = condition_true(trim(l + 1));
            continue;
        }
        if (!active) continue;
        if (*l == '[') {
            char *e = strchr(l, ']');
            if (!e) continue;
            *e = 0;
            snprintf(group, sizeof group, "%s", l + 1);
            l = trim(e + 1);                     /* "[GAME]Key=value" on one line */
            if (!*l) continue;
        }
        char *eq = strchr(l, '=');
        if (!eq) continue;
        *eq = 0;
        char *key = trim(l), *value = eq + 1;
        /* strip a comment outside quotes */
        int q = 0;
        for (char *p = value; *p; p++) {
            if (*p == '"') q = !q;
            else if (*p == '#' && !q) { *p = 0; break; }
        }
        value = trim(value);
        size_t n = strlen(value);
        if (n >= 2 && value[0] == '"' && value[n - 1] == '"') { value[n - 1] = 0; value++; }
        add(group, key, value);
    }
    free(text);
    rt_log("config: %d settings", nsettings);
}

const char *config_get(const char *group, const char *key) {
    for (int k = nsettings - 1; k >= 0; k--)
        if (!strcasecmp(settings[k].group, group) && !strcasecmp(settings[k].key, key)) return settings[k].value;
    return NULL;
}

/* ------------------------------------------------- integer expressions */
static int eval_expr(const char **s, int64_t *out, int depth);

static int eval_term(const char **s, int64_t *out, int depth) {
    while (isspace((unsigned char)**s)) (*s)++;
    if (**s == '(') {
        (*s)++;
        if (!eval_expr(s, out, depth)) return 0;
        while (isspace((unsigned char)**s)) (*s)++;
        if (**s == ')') (*s)++;
        return 1;
    }
    if (**s == '-') { (*s)++; if (!eval_term(s, out, depth)) return 0; *out = -*out; return 1; }
    if (**s == '[') {                            /* [GROUP]Key */
        const char *e = strchr(*s, ']');
        if (!e) return 0;
        char group[64], key[128];
        snprintf(group, sizeof group, "%.*s", (int)(e - *s - 1), *s + 1);
        const char *k = e + 1;
        size_t n = 0;
        while (k[n] && (isalnum((unsigned char)k[n]) || k[n] == '_')) n++;
        snprintf(key, sizeof key, "%.*s", (int)n, k);
        *s = k + n;
        int32_t v;
        if (!config_get_int(group, key, &v)) *out = 0;   /* a missing referenced setting counts as 0 */
        else *out = v;
        return 1;
    }
    char *end;
    long long v = strtoll(*s, &end, 0);
    if (end == *s) return 0;
    *s = end;
    *out = v;
    return 1;
}

static int eval_product(const char **s, int64_t *out, int depth) {
    if (!eval_term(s, out, depth)) return 0;
    for (;;) {
        while (isspace((unsigned char)**s)) (*s)++;
        char op = **s;
        if (op != '*' && op != '/') return 1;
        (*s)++;
        int64_t r;
        if (!eval_term(s, &r, depth)) return 0;
        *out = op == '*' ? *out * r : (r ? *out / r : 0);
    }
}

static int eval_expr(const char **s, int64_t *out, int depth) {
    if (depth > 16) return 0;
    if (!eval_product(s, out, depth + 1)) return 0;
    for (;;) {
        while (isspace((unsigned char)**s)) (*s)++;
        char op = **s;
        if (op != '+' && op != '-') return 1;
        (*s)++;
        int64_t r;
        if (!eval_product(s, &r, depth + 1)) return 0;
        *out = op == '+' ? *out + r : *out - r;
    }
}

int config_get_int(const char *group, const char *key, int32_t *out) {
    const char *v = config_get(group, key);
    if (!v) return 0;
    int64_t r;
    const char *s = v;
    if (!eval_expr(&s, &r, 0)) return 0;
    *out = (int32_t)r;
    return 1;
}

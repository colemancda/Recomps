/* Imports, indirect control flow that misses the function table, callbacks
 * into the game, and diagnostics. */
#include <stdlib.h>
#include <string.h>

#include "rt.h"

#define TEXT_LO 0x00002000u
#define TEXT_HI 0x0005a0c4u
#define BRANCH_TABLE_LO 0x00065f80u
#define BRANCH_TABLE_HI 0x0006636cu

int g_log_imports;
static int g_missing_zero;           /* RT_MISSING=zero: unimplemented imports return 0 */
static ImportFn *s3e_by_impl;        /* indexed by (impl - TEXT_LO) / 4 */
static const char **s3e_names;
static uint8_t *missing_seen;        /* one log line per missing stub */

/* ------------------------------------------------------------- logging */
void rt_log(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    fputs("[codz] ", stderr);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
}

static int is_code(uint32_t a) {
    uint32_t i = ((a & ~1u) - g_ftab_base) >> 1;
    return i < g_ftab_size;
}

void rt_dump(CPU *c) {
    for (int k = 0; k < 16; k += 4)
        fprintf(stderr, "  r%-2d %08x  r%-2d %08x  r%-2d %08x  r%-2d %08x\n", k, c->r[k], k + 1, c->r[k + 1],
                k + 2, c->r[k + 2], k + 3, c->r[k + 3]);
    fprintf(stderr, "  nzcv %d%d%d%d\n  stack words that look like code addresses:", c->n, c->z, c->cf, c->v);
    int shown = 0;
    for (uint32_t a = c->r[13]; a < c->r[13] + 0x800 && shown < 16; a += 4) {
        uint32_t v = ld32(a);
        if (is_code(v)) { fprintf(stderr, " %08x@%08x", v, a); shown++; }
    }
    fputc('\n', stderr);
}

void rt_fatal(CPU *c, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    fputs("[codz] fatal: ", stderr);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
    if (c) rt_dump(c);
    exit(3);
}

/* ------------------------------------------------------------- imports */
void imports_init(void) {
    const char *m = getenv("RT_MISSING");
    g_missing_zero = m && !strcmp(m, "zero");
    g_log_imports = getenv("RT_LOG_IMPORTS") != NULL;
    uint32_t n = (TEXT_HI - TEXT_LO) / 4 + 1;
    s3e_by_impl = calloc(n, sizeof(ImportFn));
    s3e_names = calloc(n, sizeof(char *));
    missing_seen = calloc(g_import_stub_count, 1);
    const ImportImpl *tables[] = {g_s3e_impls, g_compress_impls};
    const uint32_t counts[] = {g_s3e_impl_count, g_compress_impl_count};
    for (int t = 0; t < 2; t++) {
        for (uint32_t k = 0; k < counts[t]; k++) {
            uint32_t i = (tables[t][k].impl - TEXT_LO) / 4;
            s3e_by_impl[i] = tables[t][k].fn;
            s3e_names[i] = tables[t][k].name;
        }
    }
}

static const ImportStub *find_stub(uint32_t stub) {
    uint32_t lo = 0, hi = g_import_stub_count;
    while (lo < hi) {
        uint32_t mid = (lo + hi) / 2;
        if (g_import_stubs[mid].stub < stub) lo = mid + 1;
        else hi = mid;
    }
    return lo < g_import_stub_count && g_import_stubs[lo].stub == stub ? &g_import_stubs[lo] : NULL;
}

ImportFn gl_lookup(const char *name);   /* gl.c */

/* For the import log: a guest pointer to a short printable C string, else NULL. */
static const char *maybe_str(uint32_t a) {
    if (a < 0x1000 || a >= 0x40000000u) return NULL;
    const char *s = (const char *)G(a);
    int n = 0;
    while (n < 80 && s[n] >= 32 && s[n] < 127) n++;
    return (n >= 2 && s[n] == 0) ? s : NULL;
}

static uint64_t log_lines;    /* RT_LOG_IMPORTS stops after RT_LOG_LIMIT lines (default 200000) */

void rt_import(CPU *c, uint32_t stub) {
    const ImportStub *s = find_stub(stub);
    if (!s) rt_fatal(c, "import through unknown stub %08x", stub);
    uint32_t lr = c->r[14];
    ImportFn fn = NULL;
    const char *name = s->name;
    if (s->kind == IMPORT_S3E) {
        uint32_t i = (s->impl - TEXT_LO) / 4;
        fn = s3e_by_impl[i];
        if (s3e_names[i]) name = s3e_names[i];
    } else if (s->kind == IMPORT_GL) {
        fn = gl_lookup(s->name);
    } else {
        rt_fatal(c, "call to data export %08x (stub %08x)", s->impl, stub);
    }
    if (g_log_imports) {
        static uint64_t limit;
        if (!limit) { const char *l = getenv("RT_LOG_LIMIT"); limit = l ? strtoull(l, NULL, 0) : 200000; }
        if (++log_lines <= limit) {
            char strs[256] = "";
            for (int k = 0; k < 4; k++) {
                const char *t = maybe_str(c->r[k]);
                if (t) snprintf(strs + strlen(strs), sizeof strs - strlen(strs), " r%d=\"%s\"", k, t);
            }
            rt_log("import %s [%08x] (%08x, %08x, %08x, %08x) from %08x%s", name, s->impl, c->r[0], c->r[1],
                   c->r[2], c->r[3], lr, strs);
        } else if (log_lines == limit + 1) {
            rt_log("import log limit reached; stopping");
            exit(4);
        }
    }
    if (fn) {
        fn(c);
    } else {
        uint32_t k = (uint32_t)(s - g_import_stubs);
        if (!g_missing_zero)
            rt_fatal(c, "unimplemented import: %s impl %08x stub %08x (%s, %s args), called from %08x with "
                        "(%08x, %08x, %08x, %08x)", name, s->impl, stub,
                     s->ret == RET_VOID ? "void" : s->ret == RET_I32 ? "i32" : "i64",
                     s->stack_args ? "stack" : "register", lr, c->r[0], c->r[1], c->r[2], c->r[3]);
        if (!missing_seen[k]) {
            missing_seen[k] = 1;
            rt_log("missing import %s impl %08x stub %08x from %08x: returning 0", name, s->impl, stub, lr);
        }
        if (s->ret != RET_VOID) c->r[0] = 0;
        if (s->ret == RET_I64) c->r[1] = 0;
    }
    c->r[15] = lr;
}

/* ---------------------------------------------- indirect control flow */
/* RT_EXTRA_FUNCS=path: append unlifted code addresses the game reaches, as
 * "address mode" lines lift/lift.py reads with --extra. */
static void record_missing(uint32_t a) {
    const char *path = getenv("RT_EXTRA_FUNCS");
    if (!path || (a & ~1u) < 0x00065f00u || (a & ~1u) >= 0x002ba6acu) return;   /* game code only */
    FILE *f = fopen(path, "a");
    if (!f) return;
    fprintf(f, "%08x %s\n", a & ~1u, (a & 1) ? "thumb" : "arm");
    fclose(f);
}

/* An address that isn't a lifted function: a branch-table slot or stub (a
 * function pointer to an import), or code the lifter doesn't know. */
static int call_unlifted(CPU *c, uint32_t a) {
    uint32_t t = a & ~1u;
    if (t >= BRANCH_TABLE_LO && t < BRANCH_TABLE_HI) {
        int32_t imm = (int32_t)(ld32(t) << 8) >> 8;
        t = t + 8 + (uint32_t)imm * 4;
    }
    if (find_stub(t)) { rt_import(c, t); return 1; }
    if (find_stub(t + 4)) { rt_import(c, t + 4); return 1; }    /* a stub's Thumb entry */
    if (t >= TEXT_LO && t < TEXT_HI)
        rt_fatal(c, "call into the loader at %08x (an extension trampoline?), ip=%08x", a, c->r[12]);
    return 0;
}

static const char *why(uint32_t a) {
    uint32_t i = ((a & ~1u) - g_ftab_base) >> 1;
    if (i < g_ftab_size && g_ftab[i]) return (a & 1) ? "lifted as ARM, called as Thumb" : "lifted as Thumb, called as ARM";
    return "which isn't lifted code";
}

void rt_call_slow(CPU *c, uint32_t a) {
    if (call_unlifted(c, a)) return;
    record_missing(a);
    rt_fatal(c, "indirect call to %08x, %s", a, why(a));
}

void rt_jmp_slow(CPU *c, uint32_t a, uint32_t from) {
    if (call_unlifted(c, a)) return;
    record_missing(a);
    rt_fatal(c, "indirect jump from %08x to %08x, %s", from, a, why(a));
}

void rt_ret_mismatch(CPU *c, uint32_t expected, uint32_t from) {
    rt_fatal(c, "call at %08x returned to %08x instead of %08x", from, c->r[15], expected);
}

void rt_unimplemented(CPU *c, uint32_t addr, const char *text) {
    rt_fatal(c, "untranslated instruction at %08x: %s", addr, text);
}

void rt_bad_code(CPU *c, uint32_t from, uint32_t target) {
    rt_fatal(c, "branch from %08x to %08x, outside the lifted code", from, target);
}

uint32_t rt_strex(CPU *c, uint32_t addr, uint32_t value, int size) {
    if (c->excl_addr != addr) return 1;
    if (size == 4) st32(addr, value);
    else if (size == 2) st16(addr, value);
    else st8(addr, value);
    c->excl_addr = 1;
    return 0;
}

/* ------------------------------------------------------------ callbacks */
uint32_t guest_call(CPU *c, uint32_t fn, int nargs, ...) {
    va_list ap;
    va_start(ap, nargs);
    uint32_t a[7] = {0};
    for (int k = 0; k < nargs && k < 7; k++) a[k] = va_arg(ap, uint32_t);
    va_end(ap);
    uint32_t sp = c->r[13], lr = c->r[14];
    uint32_t r4_11[8];
    memcpy(r4_11, &c->r[4], sizeof r4_11);
    uint32_t nsp = (sp - 16) & ~7u;
    for (int k = 4; k < 7; k++) st32(nsp + 4 * (k - 4), a[k]);
    c->r[13] = nsp;
    for (int k = 0; k < 4; k++) c->r[k] = a[k];
    c->r[14] = GUEST_SENTINEL;
    rt_call(c, fn);
    if ((c->r[15] & ~1u) != GUEST_SENTINEL) rt_fatal(c, "callback %08x returned to %08x", fn, c->r[15]);
    uint32_t r0 = c->r[0];
    c->r[13] = sp;
    c->r[14] = lr;
    memcpy(&c->r[4], r4_11, sizeof r4_11);   /* callee-saved anyway; keeps the caller's view exact */
    return r0;
}

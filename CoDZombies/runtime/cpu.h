/* Guest CPU state (ARMv6 + VFPv2) and the inline helpers the lifted code uses.
 *
 * Every lifted function is `void f_XXXXXXXX(CPU *c)`. Calls are C calls: the
 * caller sets lr, calls, and checks that the callee "returned" to the address
 * it expected (c->r[15] holds the address the callee branched back to).
 */
#ifndef CODZ_CPU_H
#define CODZ_CPU_H

#include <math.h>
#include <stdint.h>
#include <string.h>

typedef struct CPU {
    uint32_t r[16];           /* r13 = sp, r14 = lr, r15 = address of the last return */
    uint8_t n, z, cf, v;      /* APSR flags */
    uint8_t q, pad[3];        /* sticky saturation flag */
    uint32_t ge;              /* GE[3:0] for the armv6 parallel instructions */
    union {
        float s[32];
        double d[16];
        uint32_t su[32];
        uint64_t du[16];
    } f;                      /* VFP registers: d[i] overlaps s[2i] (low) and s[2i+1] (high) */
    uint8_t fn, fz, fc, fv;   /* FPSCR flags */
    uint32_t fpscr;           /* other FPSCR bits (rounding mode, flush-to-zero) */
    uint32_t excl_addr;       /* ldrex/strex monitor */
    void *thread;             /* runtime bookkeeping for this guest thread */
} CPU;

#define SP_ (c->r[13])
#define LR_ (c->r[14])

/* ------------------------------------------------------------------ memory */
/* Guest address a lives at host address G(a). */
#ifndef RT_GUEST_BASE
#define RT_GUEST_BASE 0x100000000ULL
#endif
#ifdef RT_DYNAMIC_BASE
extern uint8_t *g_mem;
#define G(a) (g_mem + (uint32_t)(a))
#else
#define G(a) ((uint8_t *)(RT_GUEST_BASE + (uint64_t)(uint32_t)(a)))
#endif

#ifdef RT_LOG_STORES
/* Instruction tests: record every store so it can be compared with the emulator. */
void rt_log_store(uint32_t a, uint32_t size);
#define LOG_STORE(a, n) rt_log_store((a), (n))
#else
#define LOG_STORE(a, n) ((void)0)
#endif

static inline uint32_t ld8(uint32_t a) { return *G(a); }
static inline uint32_t ld16(uint32_t a) { uint16_t v; memcpy(&v, G(a), 2); return v; }
static inline uint32_t ld32(uint32_t a) { uint32_t v; memcpy(&v, G(a), 4); return v; }
static inline uint64_t ld64(uint32_t a) { uint64_t v; memcpy(&v, G(a), 8); return v; }
static inline void st8(uint32_t a, uint32_t v) { LOG_STORE(a, 1); *G(a) = (uint8_t)v; }
static inline void st16(uint32_t a, uint32_t v) { LOG_STORE(a, 2); uint16_t x = (uint16_t)v; memcpy(G(a), &x, 2); }
static inline void st32(uint32_t a, uint32_t v) { LOG_STORE(a, 4); memcpy(G(a), &v, 4); }
static inline void st64(uint32_t a, uint64_t v) { LOG_STORE(a, 8); memcpy(G(a), &v, 8); }

/* -------------------------------------------------------------- arithmetic */
static inline uint32_t add_nzcv(CPU *c, uint32_t a, uint32_t b, uint32_t cin) {
    uint64_t w = (uint64_t)a + b + cin;
    uint32_t r = (uint32_t)w;
    c->n = r >> 31;
    c->z = r == 0;
    c->cf = (uint8_t)(w >> 32);
    c->v = ((a ^ r) & (b ^ r)) >> 31;
    return r;
}
/* a - b - !cin is a + ~b + cin; C is NOT borrow. */
static inline uint32_t sub_nzcv(CPU *c, uint32_t a, uint32_t b, uint32_t cin) {
    return add_nzcv(c, a, ~b, cin);
}
static inline uint32_t nz(CPU *c, uint32_t r) {
    c->n = r >> 31;
    c->z = r == 0;
    return r;
}

/* Shifts. The *_c forms also produce the shifter carry-out in *co (which holds
 * the incoming C on entry, for the cases that leave it unchanged). */
static inline uint32_t lsl_c(uint32_t v, uint32_t n, uint8_t *co) {
    if (n == 0) return v;
    if (n < 32) { *co = (v >> (32 - n)) & 1; return v << n; }
    *co = n == 32 ? (v & 1) : 0;
    return 0;
}
static inline uint32_t lsr_c(uint32_t v, uint32_t n, uint8_t *co) {
    if (n == 0) return v;
    if (n < 32) { *co = (v >> (n - 1)) & 1; return v >> n; }
    *co = n == 32 ? (v >> 31) : 0;
    return 0;
}
static inline uint32_t asr_c(uint32_t v, uint32_t n, uint8_t *co) {
    if (n == 0) return v;
    if (n < 32) { *co = ((int32_t)v >> (n - 1)) & 1; return (uint32_t)((int32_t)v >> n); }
    *co = v >> 31;
    return (uint32_t)((int32_t)v >> 31);
}
static inline uint32_t ror_c(uint32_t v, uint32_t n, uint8_t *co) {
    if (n == 0) return v;
    n &= 31;
    if (n == 0) { *co = v >> 31; return v; }
    uint32_t r = (v >> n) | (v << (32 - n));
    *co = r >> 31;
    return r;
}
static inline uint32_t rrx_c(uint32_t v, uint8_t cin, uint8_t *co) {
    *co = v & 1;
    return (v >> 1) | ((uint32_t)cin << 31);
}
static inline uint32_t ror32(uint32_t v, uint32_t n) {
    n &= 31;
    return n ? (v >> n) | (v << (32 - n)) : v;
}

static inline int32_t ssat(int64_t v, int bits, uint8_t *q) {
    int64_t hi = ((int64_t)1 << (bits - 1)) - 1, lo = -((int64_t)1 << (bits - 1));
    if (v > hi) { *q = 1; return (int32_t)hi; }
    if (v < lo) { *q = 1; return (int32_t)lo; }
    return (int32_t)v;
}
static inline uint32_t usat(int64_t v, int bits, uint8_t *q) {
    int64_t hi = ((int64_t)1 << bits) - 1;
    if (v > hi) { *q = 1; return (uint32_t)hi; }
    if (v < 0) { *q = 1; return 0; }
    return (uint32_t)v;
}

static inline uint32_t clz32(uint32_t v) { return v ? (uint32_t)__builtin_clz(v) : 32; }

/* ---------------------------------------------------------------------- VFP */
/* vcmp: equal 0110, less 1000, greater 0010, unordered 0011 */
static inline void fcmp(CPU *c, double a, double b) {
    if (a != a || b != b) { c->fn = 0; c->fz = 0; c->fc = 1; c->fv = 1; }
    else if (a == b) { c->fn = 0; c->fz = 1; c->fc = 1; c->fv = 0; }
    else if (a < b) { c->fn = 1; c->fz = 0; c->fc = 0; c->fv = 0; }
    else { c->fn = 0; c->fz = 0; c->fc = 1; c->fv = 0; }
}
static inline uint32_t get_fpscr(CPU *c) {
    return ((uint32_t)c->fn << 31) | ((uint32_t)c->fz << 30) | ((uint32_t)c->fc << 29) |
           ((uint32_t)c->fv << 28) | (c->fpscr & 0x0fffffff);
}
static inline void set_fpscr(CPU *c, uint32_t v) {
    c->fn = v >> 31; c->fz = (v >> 30) & 1; c->fc = (v >> 29) & 1; c->fv = (v >> 28) & 1;
    c->fpscr = v & 0x0fffffff;
}
static inline uint32_t get_apsr(CPU *c) {
    return ((uint32_t)c->n << 31) | ((uint32_t)c->z << 30) | ((uint32_t)c->cf << 29) |
           ((uint32_t)c->v << 28) | ((uint32_t)c->q << 27) | (c->ge << 16) | 0x10;
}
static inline void set_apsr(CPU *c, uint32_t v, uint32_t mask) {
    if (mask & 8) {
        c->n = v >> 31; c->z = (v >> 30) & 1; c->cf = (v >> 29) & 1; c->v = (v >> 28) & 1;
        c->q = (v >> 27) & 1;
    }
    if (mask & 4) c->ge = (v >> 16) & 15;
}
/* Float -> integer, saturating, NaN -> 0 (VFP semantics), round toward zero. */
static inline int32_t f2s32(double d) {
    if (d != d) return 0;
    if (d >= 2147483647.0) return 2147483647;
    if (d <= -2147483648.0) return (int32_t)0x80000000u;
    return (int32_t)d;
}
static inline uint32_t f2u32(double d) {
    if (d != d || d <= 0) return 0;
    if (d >= 4294967295.0) return 0xffffffffu;
    return (uint32_t)d;
}
static inline float f_s(uint32_t u) { float f; memcpy(&f, &u, 4); return f; }

/* ------------------------------------------------------- runtime interface */
typedef void (*GuestFn)(CPU *);

extern GuestFn *g_ftab;       /* dense table indexed by (addr - g_ftab_base) / 2 */
extern uint8_t *g_fmode;      /* 1 where that function is Thumb */
extern uint32_t g_ftab_base, g_ftab_size;

void rt_call_slow(CPU *c, uint32_t addr);
void rt_jmp_slow(CPU *c, uint32_t addr, uint32_t from);
void rt_import(CPU *c, uint32_t stub);
void rt_ret_mismatch(CPU *c, uint32_t expected, uint32_t from);
void rt_unimplemented(CPU *c, uint32_t addr, const char *text) __attribute__((noreturn));
void rt_bad_code(CPU *c, uint32_t from, uint32_t target) __attribute__((noreturn));
uint32_t rt_strex(CPU *c, uint32_t addr, uint32_t value, int size);

/* Indirect call through a register. Bit 0 selects Thumb; the table is keyed by
 * the address without it, so a function lifted in the other mode is a miss. */
static inline void rt_call(CPU *c, uint32_t a) {
    uint32_t i = ((a & ~1u) - g_ftab_base) >> 1;
    if (i < g_ftab_size && g_ftab[i] && g_fmode[i] == (a & 1)) g_ftab[i](c);
    else rt_call_slow(c, a);
}
/* Indirect jump that leaves the function: a tail call. */
static inline void rt_jmp(CPU *c, uint32_t a, uint32_t from) {
    uint32_t i = ((a & ~1u) - g_ftab_base) >> 1;
    if (i < g_ftab_size && g_ftab[i] && g_fmode[i] == (a & 1)) g_ftab[i](c);
    else rt_jmp_slow(c, a, from);
}

#endif

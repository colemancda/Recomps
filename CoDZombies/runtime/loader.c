/* Puts the original executable's segments into guest memory and builds the
 * function table for the lifted code. */
#include <stdlib.h>
#include <string.h>

#include "rt.h"

typedef struct { uint32_t addr; uint8_t thumb; GuestFn fn; } LiftedFn;
extern const LiftedFn g_lifted[];
extern const uint32_t g_lifted_count;

GuestFn *g_ftab;
uint8_t *g_fmode;
uint32_t g_ftab_base, g_ftab_size;

static uint32_t rd32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }

/* The s3e image's initialized data is encrypted in the executable; the loader
 * (0x9198, called from 0x613c) decrypts it in place before starting the game.
 *
 *   key  = low 16 bits of h, where h = 1234 and h = h * 31 + byte over every
 *          byte from __text (0x2040) to the end of s3e_code (0x2c45e0)
 *          (0x8fc4; it also adds any time over 10 ms, an anti-debugging check)
 *   data = hdr + hdr[0x2c] + hdr[0x30] + hdr[0x3c], hdr[0x18] - hdr[0x44] bytes,
 *          where hdr is the XE3U header at the start of s3e_data
 *   each byte: out = in ^ (key >> 8); key = (key + in) * 0xce6d + 0x58bf
 */
#define S3E_DATA 0x00a0e000u
#define HASH_LO  0x00002040u
#define HASH_HI  0x002c45e0u

static void decrypt_data(void) {
    if (ld32(S3E_DATA) != 0x55334558u) {   /* "XE3U" */
        rt_log("no XE3U header at %08x; data left as is", S3E_DATA);
        return;
    }
    uint32_t h = 1234;
    for (uint32_t a = HASH_LO; a < HASH_HI; a++) h = h * 31 + ld8(a);
    uint16_t key = (uint16_t)h;
    uint32_t start = S3E_DATA + ld32(S3E_DATA + 0x2c) + ld32(S3E_DATA + 0x30) + ld32(S3E_DATA + 0x3c);
    uint32_t count = ld32(S3E_DATA + 0x18) - ld32(S3E_DATA + 0x44);
    uint8_t *p = G(start);
    for (uint32_t k = 0; k < count; k++) {
        uint8_t in = p[k];
        p[k] = in ^ (uint8_t)(key >> 8);
        key = (uint16_t)((key + in) * 0xce6du + 0x58bfu);
    }
    rt_log("decrypted %u bytes of game data at %08x (code hash %08x)", count, start, h);
}

void load_image(const char *exe) {
    FILE *f = fopen(exe, "rb");
    if (!f) { perror(exe); exit(1); }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *d = malloc((size_t)n);
    if (!d || fread(d, 1, (size_t)n, f) != (size_t)n) { fprintf(stderr, "reading %s failed\n", exe); exit(1); }
    fclose(f);
    if (rd32(d) != 0xfeedface || rd32(d + 4) != 12 /* CPU_TYPE_ARM */) {
        fprintf(stderr, "%s: not a thin 32-bit ARM Mach-O\n", exe);
        exit(1);
    }
    uint32_t ncmds = rd32(d + 16), off = 28;
    for (uint32_t k = 0; k < ncmds; k++) {
        uint32_t cmd = rd32(d + off), size = rd32(d + off + 4);
        if (cmd == 1 /* LC_SEGMENT */) {
            uint32_t vmaddr = rd32(d + off + 24), fileoff = rd32(d + off + 32), filesize = rd32(d + off + 36);
            if (vmaddr && filesize && fileoff + filesize <= (uint32_t)n) {
                memcpy(G(vmaddr), d + fileoff, filesize);
                rt_log("segment %.16s at %08x, %u bytes", (const char *)d + off + 8, vmaddr, filesize);
            }
        }
        off += size;
    }
    free(d);
    decrypt_data();

    /* Dense table over the lifted code, indexed by (address - base) / 2. */
    uint32_t lo = 0xffffffffu, hi = 0;
    for (uint32_t k = 0; k < g_lifted_count; k++) {
        if (g_lifted[k].addr < lo) lo = g_lifted[k].addr;
        if (g_lifted[k].addr > hi) hi = g_lifted[k].addr;
    }
    g_ftab_base = lo;
    g_ftab_size = (hi - lo) / 2 + 1;
    g_ftab = calloc(g_ftab_size, sizeof(GuestFn));
    g_fmode = calloc(g_ftab_size, 1);
    for (uint32_t k = 0; k < g_lifted_count; k++) {
        g_ftab[(g_lifted[k].addr - lo) / 2] = g_lifted[k].fn;
        g_fmode[(g_lifted[k].addr - lo) / 2] = g_lifted[k].thumb;
    }
    rt_log("%u lifted functions in %08x-%08x", g_lifted_count, lo, hi);
}

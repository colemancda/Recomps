/* PVRTC 1 decoding, for the desktop GL backend (the Mac has no PVRTC). Written
 * from the format's description:
 *
 * A texture is a grid of 64-bit words, one per 4x4 pixels (4 bpp) or 8x4
 * (2 bpp), at least 2x2 words, stored in Morton order. Each word holds two
 * colours, A and B (the high 32 bits), and modulation data (the low 32 bits).
 * The A and B images are upscaled bilinearly from word centres, and each
 * pixel blends A and B by its modulation value.
 *
 *   colour B: bits 31-16; bit 31 set: RGB 555, else ARGB 3444
 *   colour A: bits 15-1;  bit 15 set: RGB 554, else ARGB 3443
 *   bit 0:    modulation mode
 *
 * 4 bpp: 2 bits per pixel, weights 0, 3/8, 5/8, 1 of B; in mode 1
 * ("punch-through") 0, 1/2, 1/2 with alpha 0, 1.
 * 2 bpp: mode 0, 1 bit per pixel (A or B); mode 1, 2 bits for the pixels of
 * one checkerboard colour and the others the average of their four
 * neighbours. (The format also has horizontal-only and vertical-only
 * averaging, picked by the low modulation bits; this always averages both
 * ways, which is close.)
 */
#include <stdlib.h>
#include <string.h>

#include "rt.h"

typedef struct { int r, g, b, a; } C;   /* 8-bit channels */

static int x5(int v) { return (v << 3) | (v >> 2); }
static int x4(int v) { return (v << 4) | v; }
static int x3(int v) { return (v << 5) | (v << 2) | (v >> 1); }

static C colour_b(uint32_t w) {
    uint32_t v = w >> 16;
    if (v & 0x8000) return (C){x5((v >> 10) & 31), x5((v >> 5) & 31), x5(v & 31), 255};
    return (C){x4((v >> 8) & 15), x4((v >> 4) & 15), x4(v & 15), x3((v >> 12) & 7)};
}
static C colour_a(uint32_t w) {
    uint32_t v = w & 0xffff;
    if (v & 0x8000) return (C){x5((v >> 10) & 31), x5((v >> 5) & 31), x4((v >> 1) & 15), 255};
    return (C){x4((v >> 8) & 15), x4((v >> 4) & 15), x3((v >> 1) & 7), x3((v >> 12) & 7)};
}

/* Morton index of word (x, y) in a grid of bx by by words (powers of two):
 * y in the lower bit of each pair, then the larger side's remaining bits. */
static uint32_t twiddle(uint32_t x, uint32_t y, uint32_t bx, uint32_t by) {
    uint32_t out = 0, bit = 1, dst = 1, shift = 0, min = bx < by ? bx : by;
    while (bit < min) {
        if (y & bit) out |= dst;
        if (x & bit) out |= dst << 1;
        bit <<= 1;
        dst <<= 2;
        shift++;
    }
    uint32_t rest = (bx > by ? x : y) >> shift;
    return out | (rest << (2 * shift));
}

/* Decodes a PVRTC 1 image (format 0x8c00-0x8c03) of w x h into RGBA8 (w*h*4 bytes). */
void pvrtc_decode(const uint8_t *src, int w, int h, int two_bpp, uint8_t *out) {
    int ww = two_bpp ? 8 : 4, wh = 4;   /* pixels per word */
    int pw = w < (two_bpp ? 16 : 8) ? (two_bpp ? 16 : 8) : w;   /* padded size */
    int ph = h < 8 ? 8 : h;
    uint32_t bx = (uint32_t)(pw / ww), by = (uint32_t)(ph / wh);
    uint32_t n = bx * by;
    C *ca = malloc(sizeof(C) * n), *cb = malloc(sizeof(C) * n);
    uint32_t *mod = malloc(4 * n), *mode = malloc(4 * n);
    for (uint32_t y = 0; y < by; y++)
        for (uint32_t x = 0; x < bx; x++) {
            uint32_t t = twiddle(x, y, bx, by), k = y * bx + x;
            uint32_t m, col;
            memcpy(&m, src + 8 * t, 4);
            memcpy(&col, src + 8 * t + 4, 4);
            ca[k] = colour_a(col);
            cb[k] = colour_b(col);
            mod[k] = m;
            mode[k] = col & 1;
        }
    /* modulation weight (0-8) and punch-through flag per pixel */
    int8_t *wt = malloc((size_t)pw * ph);
    uint8_t *punch = calloc((size_t)pw * ph, 1);
    for (int y = 0; y < ph; y++)
        for (int x = 0; x < pw; x++) {
            uint32_t k = (uint32_t)(y / wh) * bx + (uint32_t)(x / ww);
            int lx = x % ww, ly = y % wh, v;
            if (!two_bpp) {
                v = (int)(mod[k] >> (2 * (ly * 4 + lx))) & 3;
                if (mode[k]) {
                    static const int pt[4] = {0, 4, 4, 8};
                    punch[y * pw + x] = v == 2;
                    v = pt[v];
                } else {
                    static const int w4[4] = {0, 3, 5, 8};
                    v = w4[v];
                }
            } else if (!mode[k]) {
                v = (mod[k] >> (ly * 8 + lx)) & 1 ? 8 : 0;
            } else if (((lx + ly) & 1) == 0) {
                static const int w4[4] = {0, 3, 5, 8};
                v = w4[(mod[k] >> (2 * ((ly * 8 + lx) / 2))) & 3];
            } else {
                v = -1;   /* filled in below */
            }
            wt[y * pw + x] = (int8_t)v;
        }
    if (two_bpp)
        for (int y = 0; y < ph; y++)
            for (int x = 0; x < pw; x++) {
                if (wt[y * pw + x] >= 0) continue;
                int s = 0, c = 0;
                const int dx[4] = {-1, 1, 0, 0}, dy[4] = {0, 0, -1, 1};
                for (int d = 0; d < 4; d++) {
                    int nx = (x + dx[d] + pw) % pw, ny = (y + dy[d] + ph) % ph;
                    int v = wt[ny * pw + nx];
                    if (v >= 0) { s += v; c++; }
                }
                wt[y * pw + x] = (int8_t)(c ? (s + c / 2) / c : 0);
            }
    /* upscale A and B from word centres and blend */
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            /* word whose centre is up and to the left, and the offset from it */
            int gx = x - ww / 2, gy = y - wh / 2;
            int x0 = (gx < 0 ? gx - ww + 1 : gx) / ww, y0 = (gy < 0 ? gy - wh + 1 : gy) / wh;
            int fx = gx - x0 * ww, fy = gy - y0 * wh;
            int xs[2] = {(x0 + (int)bx) % (int)bx, (x0 + 1 + (int)bx) % (int)bx};
            int ys[2] = {(y0 + (int)by) % (int)by, (y0 + 1 + (int)by) % (int)by};
            int wgt[4] = {(ww - fx) * (wh - fy), fx * (wh - fy), (ww - fx) * fy, fx * fy};
            int idx[4] = {ys[0] * (int)bx + xs[0], ys[0] * (int)bx + xs[1], ys[1] * (int)bx + xs[0],
                          ys[1] * (int)bx + xs[1]};
            int A[4] = {0}, B[4] = {0}, tot = ww * wh;
            for (int k = 0; k < 4; k++) {
                A[0] += ca[idx[k]].r * wgt[k]; A[1] += ca[idx[k]].g * wgt[k];
                A[2] += ca[idx[k]].b * wgt[k]; A[3] += ca[idx[k]].a * wgt[k];
                B[0] += cb[idx[k]].r * wgt[k]; B[1] += cb[idx[k]].g * wgt[k];
                B[2] += cb[idx[k]].b * wgt[k]; B[3] += cb[idx[k]].a * wgt[k];
            }
            int m = wt[y * pw + x];
            uint8_t *p = out + 4 * ((size_t)y * w + x);
            for (int ch = 0; ch < 4; ch++) {
                int a = A[ch], b = B[ch];
                p[ch] = (uint8_t)((a * (8 - m) + b * m + tot * 4) / (tot * 8));
            }
            if (punch[y * pw + x]) p[3] = 0;
        }
    free(ca); free(cb); free(mod); free(mode); free(wt); free(punch);
}

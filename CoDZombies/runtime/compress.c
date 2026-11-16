/* s3eCompression's streaming decompressor, on zlib.
 *
 *   0x189b0 s3eCompressionDecompInit(alg, readFn, userData) -> handle
 *   0x18638 s3eCompressionDecompRead(handle, void *out, uint32 *len)  (*len: wanted in, produced out)
 *   0x18424 s3eCompressionDecompFinal(handle)
 *
 * The loader pulls compressed input through the game's callback,
 * uint32 readFn(struct { void *buf; uint32 len; } *io, void *userData), which
 * returns the bytes it put in buf. The format is detected from the stream:
 * LZMA "alone" (props byte, 32-bit dictionary size, 64-bit unpacked size; the
 * game's derbh archives use this, algorithm 0), zlib or gzip headers, else raw
 * deflate.
 *
 * LZMA comes from liblzma (xz); the host build links Homebrew's. iOS will need
 * it built for the device or a vendored decoder.
 */
#include <lzma.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>

#include "rt.h"

#define CHUNK 16384

typedef struct Decomp {
    uint32_t handle;       /* guest address handed to the game, 0 when free */
    uint32_t read_fn, user_data;
    uint32_t io;           /* guest: { buf, len } followed by the CHUNK-byte buffer */
    z_stream zs;
    lzma_stream xs;
    int started, raw, is_lzma, eof, done;
    uint8_t in[CHUNK];
} Decomp;
static Decomp streams[4];

static Decomp *decomp_of(uint32_t h) {
    for (int k = 0; k < 4; k++) if (h && streams[k].handle == h) return &streams[k];
    return NULL;
}

static void s3eCompressionDecompInit(CPU *c) {
    uint32_t alg = arg(c, 0), fn = arg(c, 1), ud = arg(c, 2);
    if (alg > 4 || !fn) { ret32(c, 0); return; }
    for (int k = 0; k < 4; k++) {
        Decomp *d = &streams[k];
        if (d->handle) continue;
        memset(d, 0, sizeof *d);
        d->handle = guest_alloc(16);
        d->io = guest_alloc(8 + CHUNK);
        d->read_fn = fn;
        d->user_data = ud;
        ret32(c, d->handle);
        return;
    }
    rt_log("s3eCompressionDecompInit: out of handles");
    ret32(c, 0);
}

static void pull(CPU *c, Decomp *d) {
    st32(d->io, d->io + 8);
    st32(d->io + 4, CHUNK);
    uint32_t n = guest_call(c, d->read_fn, 2, d->io, d->user_data);
    if (n > CHUNK) n = CHUNK;
    if (n == 0) { d->eof = 1; return; }
    memcpy(d->in, G(d->io + 8), n);
    d->zs.next_in = d->in;
    d->zs.avail_in = n;
    d->xs.next_in = d->in;
    d->xs.avail_in = n;
}

static void read_lzma(CPU *c, Decomp *d, uint32_t out, uint32_t lenp, uint32_t want) {
    d->xs.next_out = G(out);
    d->xs.avail_out = want;
    while (d->xs.avail_out && !d->done) {
        if (!d->xs.avail_in && !d->eof) pull(c, d);
        lzma_ret r = lzma_code(&d->xs, d->eof && !d->xs.avail_in ? LZMA_FINISH : LZMA_RUN);
        if (r == LZMA_STREAM_END) { d->done = 1; break; }
        if (r == LZMA_BUF_ERROR && d->eof) break;
        if (r != LZMA_OK) {
            rt_log("lzma decompression error %d", r);
            st32(lenp, want - (uint32_t)d->xs.avail_out);
            ret32(c, 1);
            return;
        }
    }
    st32(lenp, want - (uint32_t)d->xs.avail_out);
    ret32(c, 0);
}

static void s3eCompressionDecompRead(CPU *c) {
    Decomp *d = decomp_of(arg(c, 0));
    uint32_t out = arg(c, 1), lenp = arg(c, 2);
    if (!d || !out || !lenp || !ld32(lenp)) { ret32(c, 1); return; }
    uint32_t want = ld32(lenp);
    if (!d->started) {
        pull(c, d);
        if (d->eof) { st32(lenp, 0); ret32(c, 0); return; }
        uint8_t b0 = d->in[0], b1 = d->zs.avail_in > 1 ? d->in[1] : 0;
        int zlib_hdr = (b0 & 0x0f) == 8 && ((b0 << 8) | b1) % 31 == 0;
        int gzip_hdr = b0 == 0x1f && b1 == 0x8b;
        d->is_lzma = !zlib_hdr && !gzip_hdr && b0 < 9 * 5 * 5;   /* a valid lc/lp/pb properties byte */
        d->raw = !zlib_hdr && !gzip_hdr && !d->is_lzma;
        if (d->is_lzma) {
            d->xs = (lzma_stream)LZMA_STREAM_INIT;
            d->xs.next_in = d->in;
            d->xs.avail_in = d->zs.avail_in;
            if (lzma_alone_decoder(&d->xs, UINT64_MAX) != LZMA_OK) { ret32(c, 1); return; }
        } else if (inflateInit2(&d->zs, d->raw ? -15 : 15 + 32) != Z_OK) {
            ret32(c, 1);
            return;
        }
        d->started = 1;
        static int logged;
        if (logged++ < 3)
            rt_log("decompressing a %s stream (first bytes %02x %02x)",
                   d->is_lzma ? "LZMA" : gzip_hdr ? "gzip" : zlib_hdr ? "zlib" : "raw deflate", b0, b1);
    }
    if (d->is_lzma) {
        read_lzma(c, d, out, lenp, want);
        return;
    }
    d->zs.next_out = G(out);
    d->zs.avail_out = want;
    while (d->zs.avail_out && !d->done) {
        if (!d->zs.avail_in && !d->eof) pull(c, d);
        int r = inflate(&d->zs, Z_NO_FLUSH);
        if (r == Z_STREAM_END) { d->done = 1; break; }
        if (r == Z_BUF_ERROR && d->eof) break;
        if (r != Z_OK && r != Z_BUF_ERROR) {
            rt_log("decompression error %d (%s)", r, d->zs.msg ? d->zs.msg : "");
            st32(lenp, want - d->zs.avail_out);
            ret32(c, 1);
            return;
        }
    }
    st32(lenp, want - d->zs.avail_out);
    ret32(c, 0);
}

static void s3eCompressionDecompFinal(CPU *c) {
    Decomp *d = decomp_of(arg(c, 0));
    if (!d) { ret32(c, 1); return; }
    if (d->started && d->is_lzma) lzma_end(&d->xs);
    else if (d->started) inflateEnd(&d->zs);
    guest_free(d->io);
    guest_free(d->handle);
    d->handle = 0;
    ret32(c, 0);
}

const ImportImpl g_compress_impls[] = {
    {0x00018424, "s3eCompressionDecompFinal", s3eCompressionDecompFinal},
    {0x00018638, "s3eCompressionDecompRead", s3eCompressionDecompRead},
    {0x000189b0, "s3eCompressionDecompInit", s3eCompressionDecompInit},
};
const uint32_t g_compress_impl_count = sizeof g_compress_impls / sizeof g_compress_impls[0];

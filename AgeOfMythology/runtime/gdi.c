/* gdi32: device contexts, GDI objects, and fonts rendered with FreeType.
 *
 * The game draws its interface text itself: it creates a font, asks for its
 * metrics and Unicode coverage, and rasterises each character it needs with
 * GetGlyphOutline into its own textures. That is what is implemented for real.
 * DCs, bitmaps, brushes and palettes are bookkeeping; blits and TextOut draw
 * nothing (the game's screen is Direct3D).
 *
 * Windows typefaces are mapped to metric-compatible free fonts (Liberation);
 * --font-dir names the directory holding them.
 */
#define _GNU_SOURCE
#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_TRUETYPE_TABLES_H
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <strings.h>

#include "rt.h"

enum { O_FREE = 0, O_DC, O_FONT, O_BITMAP, O_BRUSH, O_PEN, O_PALETTE };
/* OBJ_* values GetCurrentObject and GetObjectType use */
enum { OBJ_PEN = 1, OBJ_BRUSH = 2, OBJ_DC = 3, OBJ_PAL = 5, OBJ_FONT = 6, OBJ_BITMAP = 7, OBJ_MEMDC = 10 };

typedef struct Font {
    int height, width, weight, italic, charset; /* LOGFONT */
    char face[64];
    uint8_t logfont[92];  /* LOGFONTW as given (A fonts are widened) */
    FT_Face ft;
} Font;

typedef struct GdiObj {
    int type;
    int stock;
    union {
        struct { /* DC */
            uint32_t font, bitmap, brush, pen, palette;
            uint32_t text_color, bk_color, bk_mode, text_align, map_mode;
        } dc;
        Font font;
        struct { /* bitmap */
            int w, h, bpp;
            uint32_t bits; /* guest memory, or 0 */
        } bmp;
    } u;
} GdiObj;

#define HGDI_BASE 0x47000000u
#define MAX_OBJS 4096
static GdiObj objs[MAX_OBJS];
static pthread_mutex_t gdi_mu = PTHREAD_MUTEX_INITIALIZER;
static FT_Library ft_lib;
static char font_dir[512] = "/usr/share/fonts/truetype/liberation";

void rt_gdi_set_font_dir(const char *dir) { snprintf(font_dir, sizeof font_dir, "%s", dir); }

static uint32_t handle_of(int i) { return HGDI_BASE + 4u * (uint32_t)i; }
static GdiObj *get(uint32_t h, int type) {
    if (h == 0x0000DC00u) h = handle_of(1); /* user32's GetDC: the screen DC */
    uint32_t i = (h - HGDI_BASE) / 4;
    if (h < HGDI_BASE || (h & 3) || i >= MAX_OBJS || objs[i].type == O_FREE) return NULL;
    if (type && objs[i].type != type) return NULL;
    return &objs[i];
}
static uint32_t obj_new(int type) {
    for (int i = 32; i < MAX_OBJS; i++) /* 0..31: the screen DC and the stock objects */
        if (objs[i].type == O_FREE) {
            memset(&objs[i], 0, sizeof objs[i]);
            objs[i].type = type;
            return handle_of(i);
        }
    rt_log("gdi: too many objects\n");
    return 0;
}

/* ------------------------------------------------------------------ fonts */
/* Typeface -> font file stem (Liberation: metric-compatible with the Windows core fonts). */
static const char *font_file(const char *face, int bold, int italic, char *out, size_t cap) {
    const char *fam = "LiberationSans";
    if (!strcasecmp(face, "Times New Roman") || !strcasecmp(face, "Georgia") || !strcasecmp(face, "Perpetua") ||
        !strncasecmp(face, "Giovanni", 8) || !strcasecmp(face, "Friz Quadrata TT"))
        fam = "LiberationSerif";
    else if (!strcasecmp(face, "Courier New") || !strcasecmp(face, "Courier"))
        fam = "LiberationMono";
    const char *style = bold && italic ? "BoldItalic" : bold ? "Bold" : italic ? "Italic" : "Regular";
    snprintf(out, cap, "%s/%s-%s.ttf", font_dir, fam, style);
    return out;
}

static int font_load(Font *f) {
    if (!ft_lib && FT_Init_FreeType(&ft_lib)) {
        rt_log("gdi: FreeType initialisation failed\n");
        return 0;
    }
    char path[700];
    font_file(f->face, f->weight >= 600, f->italic, path, sizeof path);
    if (FT_New_Face(ft_lib, path, 0, &f->ft)) {
        rt_log("gdi: cannot open font %s (for \"%s\"); set --font-dir\n", path, f->face);
        f->ft = NULL;
        return 0;
    }
    /* lfHeight < 0: the em height in pixels; > 0: the cell height (ascent + descent);
     * 0: a default size. Windows' ascent and descent come from the OS/2 table. */
    int h = f->height;
    if (h == 0) h = -12;
    if (h < 0) {
        FT_Set_Pixel_Sizes(f->ft, 0, (FT_UInt)-h);
    } else {
        TT_OS2 *os2 = FT_Get_Sfnt_Table(f->ft, FT_SFNT_OS2);
        int cell = os2 ? os2->usWinAscent + os2->usWinDescent : f->ft->ascender - f->ft->descender;
        int em = (int)((int64_t)h * f->ft->units_per_EM / (cell ? cell : 1));
        FT_Set_Pixel_Sizes(f->ft, 0, (FT_UInt)(em > 0 ? em : 1));
    }
    if (g_opt.verbose)
        rt_log("[gdi] font \"%s\" height %d weight %d%s -> %s\n", f->face, f->height, f->weight,
               f->italic ? " italic" : "", path);
    return 1;
}

/* LOGFONTW at p (wide) or LOGFONTA. */
static uint32_t create_font(uint32_t p, int wide) {
    pthread_mutex_lock(&gdi_mu);
    uint32_t h = obj_new(O_FONT);
    if (!h) {
        pthread_mutex_unlock(&gdi_mu);
        return 0;
    }
    Font *f = &get(h, O_FONT)->u.font;
    f->height = (int32_t)ld32(p);
    f->width = (int32_t)ld32(p + 4);
    f->weight = (int32_t)ld32(p + 16);
    f->italic = ld8(p + 20);
    f->charset = ld8(p + 23);
    memcpy(f->logfont, G(p), 28);
    if (wide) {
        rt_get_wstr(p + 28, f->face, sizeof f->face);
        memcpy(f->logfont + 28, G(p + 28), 64);
    } else {
        snprintf(f->face, sizeof f->face, "%.31s", (const char *)G(p + 28));
        for (int i = 0; i < 32; i++) { /* stored widened, as LOGFONTW */
            f->logfont[28 + 2 * i] = ld8(p + 28 + (uint32_t)i);
            f->logfont[29 + 2 * i] = 0;
        }
    }
    font_load(f);
    pthread_mutex_unlock(&gdi_mu);
    return h;
}

H(gdi_CreateFontIndirectW) { UNUSED; return create_font(a[0], 1); }
H(gdi_CreateFontIndirectA) { UNUSED; return create_font(a[0], 0); }
/* CreateFont(height, width, esc, orient, weight, italic, underline, strike, charset,
 *            outprec, clipprec, quality, pitch, face) */
static uint32_t create_font_args(const uint32_t *a, int wide) {
    uint32_t p = rt_heap_alloc(92, 1);
    for (int i = 0; i < 5; i++) st32(p + 4 * (uint32_t)i, a[i]);
    for (int i = 0; i < 8; i++) st8(p + 20 + (uint32_t)i, (uint8_t)a[5 + i]);
    if (a[13]) {
        if (wide) {
            uint32_t n = rt_wlen(a[13]);
            memcpy(G(p + 28), G(a[13]), 2 * (n < 31 ? n : 31));
        } else {
            snprintf((char *)G(p + 28), 32, "%s", rt_gstr(a[13]));
        }
    }
    uint32_t h = create_font(p, wide);
    rt_heap_free(p);
    return h;
}
H(gdi_CreateFontW) { UNUSED; return create_font_args(a, 1); }
H(gdi_CreateFontA) { UNUSED; return create_font_args(a, 0); }

static Font *dc_font(uint32_t hdc) {
    GdiObj *dc = get(hdc, O_DC);
    if (!dc) return NULL;
    GdiObj *f = get(dc->u.dc.font, O_FONT);
    return f && f->u.font.ft ? &f->u.font : NULL;
}

/* Windows-style ascent/descent of the selected size, in pixels. */
static void font_vmetrics(Font *f, int *ascent, int *descent) {
    FT_Size_Metrics *m = &f->ft->size->metrics;
    TT_OS2 *os2 = FT_Get_Sfnt_Table(f->ft, FT_SFNT_OS2);
    if (os2) {
        *ascent = (int)(((int64_t)os2->usWinAscent * m->y_ppem + f->ft->units_per_EM - 1) / f->ft->units_per_EM);
        *descent = (int)(((int64_t)os2->usWinDescent * m->y_ppem + f->ft->units_per_EM - 1) / f->ft->units_per_EM);
    } else {
        *ascent = (int)(m->ascender >> 6);
        *descent = (int)(-m->descender >> 6);
    }
}

/* TEXTMETRICW (wide) or TEXTMETRICA. */
static uint32_t text_metrics(CPU *c, uint32_t hdc, uint32_t p, int wide) {
    pthread_mutex_lock(&gdi_mu);
    Font *f = dc_font(hdc);
    if (!f) {
        pthread_mutex_unlock(&gdi_mu);
        rt_set_last_error(c, 6);
        return 0;
    }
    int asc, desc;
    font_vmetrics(f, &asc, &desc);
    int ppem = f->ft->size->metrics.y_ppem;
    int avg = 0, maxw = (int)(f->ft->size->metrics.max_advance >> 6);
    TT_OS2 *os2 = FT_Get_Sfnt_Table(f->ft, FT_SFNT_OS2);
    if (os2 && os2->xAvgCharWidth)
        avg = (int)(((int64_t)os2->xAvgCharWidth * f->ft->size->metrics.x_ppem + f->ft->units_per_EM / 2) /
                    f->ft->units_per_EM);
    if (!avg && !FT_Load_Char(f->ft, 'x', FT_LOAD_DEFAULT)) avg = (int)(f->ft->glyph->advance.x >> 6);
    memset(G(p), 0, wide ? 60 : 56);
    st32(p, (uint32_t)(asc + desc));         /* tmHeight */
    st32(p + 4, (uint32_t)asc);              /* tmAscent */
    st32(p + 8, (uint32_t)desc);             /* tmDescent */
    st32(p + 12, (uint32_t)(asc + desc - ppem)); /* tmInternalLeading */
    st32(p + 20, (uint32_t)avg);             /* tmAveCharWidth */
    st32(p + 24, (uint32_t)maxw);            /* tmMaxCharWidth */
    st32(p + 28, (uint32_t)(f->weight ? f->weight : 400));
    st32(p + 36, 96);
    st32(p + 40, 96);
    if (wide) {
        st16(p + 44, 0x20);
        st16(p + 46, 0xFFFC);
        st16(p + 48, 0x1F);
        st16(p + 50, 0x20);
        st8(p + 52, (uint8_t)(f->italic != 0));
        st8(p + 55, 0x26); /* TMPF_VECTOR | TMPF_TRUETYPE, FF_SWISS */
        st8(p + 56, (uint8_t)f->charset);
    } else {
        st8(p + 44, 0x20);
        st8(p + 45, 0xFF);
        st8(p + 46, 0x1F);
        st8(p + 47, 0x20);
        st8(p + 48, (uint8_t)(f->italic != 0));
        st8(p + 51, 0x26);
        st8(p + 52, (uint8_t)f->charset);
    }
    pthread_mutex_unlock(&gdi_mu);
    return 1;
}
H(gdi_GetTextMetricsW) { return text_metrics(c, a[0], a[1], 1); }
H(gdi_GetTextMetricsA) { return text_metrics(c, a[0], a[1], 0); }

#define GGO_METRICS 0
#define GGO_BITMAP 1
#define GGO_GRAY2_BITMAP 4
#define GGO_GRAY4_BITMAP 5
#define GGO_GRAY8_BITMAP 6
#define GGO_GLYPH_INDEX 0x80

/* GetGlyphOutline(hdc, ch, format, GLYPHMETRICS*, cbBuffer, buffer, MAT2*). The
 * transformation is taken as the identity (the game passes it). */
static uint32_t glyph_outline(CPU *c, const uint32_t *a) {
    uint32_t fmt = a[2] & ~(uint32_t)(GGO_GLYPH_INDEX | 0x100 /* GGO_UNHINTED */);
    pthread_mutex_lock(&gdi_mu);
    Font *f = dc_font(a[0]);
    if (!f || (fmt != GGO_METRICS && fmt != GGO_BITMAP && (fmt < GGO_GRAY2_BITMAP || fmt > GGO_GRAY8_BITMAP))) {
        pthread_mutex_unlock(&gdi_mu);
        if (f) rt_log("gdi: GetGlyphOutline format %#x not provided\n", a[2]);
        return 0xFFFFFFFFu; /* GDI_ERROR */
    }
    FT_UInt gi = (a[2] & GGO_GLYPH_INDEX) ? a[1] : FT_Get_Char_Index(f->ft, a[1]);
    int mono = fmt == GGO_BITMAP;
    if (FT_Load_Glyph(f->ft, gi, mono ? FT_LOAD_TARGET_MONO : FT_LOAD_DEFAULT) ||
        FT_Render_Glyph(f->ft->glyph, mono ? FT_RENDER_MODE_MONO : FT_RENDER_MODE_NORMAL)) {
        pthread_mutex_unlock(&gdi_mu);
        return 0xFFFFFFFFu;
    }
    FT_GlyphSlot g = f->ft->glyph;
    FT_Bitmap *bm = &g->bitmap;
    int w = (int)bm->width, h = (int)bm->rows;
    int empty = w == 0 || h == 0;
    /* GLYPHMETRICS */
    st32(a[3], empty ? 1u : (uint32_t)w);
    st32(a[3] + 4, empty ? 1u : (uint32_t)h);
    st32(a[3] + 8, (uint32_t)g->bitmap_left);
    st32(a[3] + 12, (uint32_t)(empty ? 0 : g->bitmap_top));
    st16(a[3] + 16, (uint16_t)((g->advance.x + 32) >> 6));
    st16(a[3] + 18, 0);
    if (fmt == GGO_METRICS || empty) {
        pthread_mutex_unlock(&gdi_mu);
        return fmt == GGO_METRICS ? 1 : 0;
    }
    uint32_t pitch = mono ? (uint32_t)((w + 31) / 32 * 4) : (uint32_t)((w + 3) & ~3);
    uint32_t size = pitch * (uint32_t)h;
    if (!a[4] || !a[5]) {
        pthread_mutex_unlock(&gdi_mu);
        return size;
    }
    if (a[4] < size) {
        pthread_mutex_unlock(&gdi_mu);
        return 0xFFFFFFFFu;
    }
    int levels = fmt == GGO_GRAY2_BITMAP ? 4 : fmt == GGO_GRAY4_BITMAP ? 16 : 64;
    memset(G(a[5]), 0, size);
    for (int y = 0; y < h; y++) {
        const uint8_t *src = bm->buffer + (ptrdiff_t)y * bm->pitch;
        uint8_t *dst = G(a[5] + (uint32_t)y * pitch);
        if (mono) memcpy(dst, src, (size_t)(w + 7) / 8);
        else
            for (int x = 0; x < w; x++) dst[x] = (uint8_t)((src[x] * levels + 127) / 255);
    }
    pthread_mutex_unlock(&gdi_mu);
    return size;
}
H(gdi_GetGlyphOutline) { return glyph_outline(c, a); }

/* GetFontUnicodeRanges(hdc, GLYPHSET*): the characters the font maps, as ranges. */
H(gdi_GetFontUnicodeRanges) {
    pthread_mutex_lock(&gdi_mu);
    Font *f = dc_font(a[0]);
    if (!f) {
        pthread_mutex_unlock(&gdi_mu);
        return 0;
    }
    uint32_t nranges = 0, nglyphs = 0, out = a[1] ? a[1] + 16 : 0;
    FT_UInt gi;
    FT_ULong ch = FT_Get_First_Char(f->ft, &gi), lo = 0, prev = 0;
    int open = 0;
    while (gi && ch < 0x10000) {
        if (!open || ch != prev + 1) {
            if (open) {
                if (out) st16(out + 4 * (nranges - 1) + 2, (uint16_t)(prev - lo + 1));
            }
            lo = ch;
            nranges++;
            if (out) st16(out + 4 * (nranges - 1), (uint16_t)ch);
            open = 1;
        }
        prev = ch;
        nglyphs++;
        ch = FT_Get_Next_Char(f->ft, ch, &gi);
    }
    if (open && out) st16(out + 4 * (nranges - 1) + 2, (uint16_t)(prev - lo + 1));
    uint32_t size = 16 + 4 * nranges;
    if (a[1]) {
        st32(a[1], size);
        st32(a[1] + 4, 0);
        st32(a[1] + 8, nglyphs);
        st32(a[1] + 12, nranges);
    }
    pthread_mutex_unlock(&gdi_mu);
    return size;
}

/* GetTextExtentPoint32(hdc, str, count, SIZE*) */
static uint32_t text_extent(CPU *c, const uint32_t *a, int wide) {
    pthread_mutex_lock(&gdi_mu);
    Font *f = dc_font(a[0]);
    int w = 0, asc = 0, desc = 0;
    if (f) {
        font_vmetrics(f, &asc, &desc);
        for (uint32_t i = 0; i < a[2]; i++) {
            uint32_t ch = wide ? ld16(a[1] + 2 * i) : ld8(a[1] + i);
            if (!FT_Load_Char(f->ft, ch, FT_LOAD_DEFAULT)) w += (int)((f->ft->glyph->advance.x + 32) >> 6);
        }
    }
    pthread_mutex_unlock(&gdi_mu);
    st32(a[3], (uint32_t)w);
    st32(a[3] + 4, (uint32_t)(asc + desc));
    return f != NULL;
}
H(gdi_GetTextExtentPoint32W) { return text_extent(c, a, 1); }
H(gdi_GetTextExtentPoint32A) { return text_extent(c, a, 0); }

/* ------------------------------------------------------------- DCs, objects */
static void dc_defaults(GdiObj *dc) {
    dc->u.dc.font = handle_of(2 + 13); /* SYSTEM_FONT */
    dc->u.dc.brush = handle_of(2 + 0);
    dc->u.dc.pen = handle_of(2 + 7);
    dc->u.dc.palette = handle_of(2 + 15);
    dc->u.dc.text_color = 0;
    dc->u.dc.bk_color = 0xFFFFFF;
    dc->u.dc.bk_mode = 2; /* OPAQUE */
    dc->u.dc.map_mode = 1;
}

static void gdi_init_once(void) {
    static int done;
    if (done) return;
    done = 1;
    objs[1].type = O_DC; /* the screen DC */
    objs[1].stock = 1;
    dc_defaults(&objs[1]);
    /* stock objects 0..17 at indices 2..19 */
    for (int i = 0; i <= 17; i++) {
        GdiObj *o = &objs[2 + i];
        o->stock = 1;
        if (i <= 5) o->type = O_BRUSH;
        else if (i <= 8) o->type = O_PEN;
        else if (i == 15) o->type = O_PALETTE;
        else if (i >= 10) {
            o->type = O_FONT;
            Font *f = &o->u.font;
            f->height = -13;
            f->weight = 400;
            snprintf(f->face, sizeof f->face, "%s", i == 10 || i == 11 || i == 16 ? "Courier New" : "Arial");
        } else {
            o->type = O_BRUSH; /* 9 is unused */
        }
    }
}
#define INIT() do { pthread_mutex_lock(&gdi_mu); gdi_init_once(); } while (0)

H(gdi_GetStockObject) {
    UNUSED;
    if (a[0] > 17) return 0;
    INIT();
    GdiObj *o = &objs[2 + a[0]];
    if (o->type == O_FONT && !o->u.font.ft) font_load(&o->u.font);
    pthread_mutex_unlock(&gdi_mu);
    return handle_of(2 + (int)a[0]);
}

H(gdi_CreateCompatibleDC) {
    UNUSED;
    INIT();
    uint32_t h = obj_new(O_DC);
    if (h) {
        GdiObj *dc = get(h, O_DC);
        dc_defaults(dc);
        dc->u.dc.bitmap = 0; /* a 1x1 monochrome bitmap on Windows */
    }
    pthread_mutex_unlock(&gdi_mu);
    return h;
}
H(gdi_DeleteDC) {
    UNUSED;
    INIT();
    GdiObj *dc = get(a[0], O_DC);
    int ok = dc && !dc->stock;
    if (ok) dc->type = O_FREE;
    pthread_mutex_unlock(&gdi_mu);
    return (uint32_t)ok;
}

/* SelectObject(hdc, obj): returns the object of that kind it replaces. */
H(gdi_SelectObject) {
    UNUSED;
    INIT();
    GdiObj *dc = get(a[0], O_DC), *o = get(a[1], 0);
    uint32_t old = 0;
    if (dc && o) {
        uint32_t *slot = o->type == O_FONT      ? &dc->u.dc.font
                         : o->type == O_BITMAP  ? &dc->u.dc.bitmap
                         : o->type == O_BRUSH   ? &dc->u.dc.brush
                         : o->type == O_PEN     ? &dc->u.dc.pen
                         : o->type == O_PALETTE ? &dc->u.dc.palette
                                                : NULL;
        if (slot) {
            old = *slot;
            *slot = a[1];
            if (o->type == O_FONT && !o->u.font.ft) font_load(&o->u.font);
        }
    }
    pthread_mutex_unlock(&gdi_mu);
    return old;
}
H(gdi_GetCurrentObject) {
    UNUSED;
    INIT();
    GdiObj *dc = get(a[0], O_DC);
    uint32_t r = 0;
    if (dc) switch (a[1]) {
        case OBJ_PEN: r = dc->u.dc.pen; break;
        case OBJ_BRUSH: r = dc->u.dc.brush; break;
        case OBJ_PAL: r = dc->u.dc.palette; break;
        case OBJ_FONT: r = dc->u.dc.font; break;
        case OBJ_BITMAP: r = dc->u.dc.bitmap; break;
        }
    pthread_mutex_unlock(&gdi_mu);
    return r;
}
H(gdi_DeleteObject) {
    UNUSED;
    INIT();
    GdiObj *o = get(a[0], 0);
    int ok = o && !o->stock && o->type != O_DC;
    if (ok) {
        if (o->type == O_FONT && o->u.font.ft) FT_Done_Face(o->u.font.ft);
        if (o->type == O_BITMAP && o->u.bmp.bits) rt_heap_free(o->u.bmp.bits);
        o->type = O_FREE;
    }
    pthread_mutex_unlock(&gdi_mu);
    return (uint32_t)ok;
}
/* GetObject(h, size, buf): BITMAP or LOGFONT. */
static uint32_t get_object(const uint32_t *a, int wide) {
    INIT();
    GdiObj *o = get(a[0], 0);
    uint32_t r = 0;
    if (o && o->type == O_BITMAP) {
        r = 24;
        if (a[2] && a[1] >= 24) {
            int pitch = ((o->u.bmp.w * o->u.bmp.bpp + 15) / 16) * 2;
            st32(a[2], 0);
            st32(a[2] + 4, (uint32_t)o->u.bmp.w);
            st32(a[2] + 8, (uint32_t)o->u.bmp.h);
            st32(a[2] + 12, (uint32_t)pitch);
            st16(a[2] + 16, 1);
            st16(a[2] + 18, (uint16_t)o->u.bmp.bpp);
            st32(a[2] + 20, o->u.bmp.bits);
        }
    } else if (o && o->type == O_FONT) {
        r = wide ? 92 : 60;
        if (a[2] && a[1] >= r) {
            memcpy(G(a[2]), o->u.font.logfont, 28);
            if (wide) memcpy(G(a[2] + 28), o->u.font.logfont + 28, 64);
            else
                for (int i = 0; i < 32; i++) st8(a[2] + 28 + (uint32_t)i, o->u.font.logfont[28 + 2 * i]);
        }
    }
    pthread_mutex_unlock(&gdi_mu);
    return r;
}
H(gdi_GetObjectW) { UNUSED; return get_object(a, 1); }
H(gdi_GetObjectA) { UNUSED; return get_object(a, 0); }

static uint32_t new_bitmap(int w, int h, int bpp, int alloc) {
    uint32_t hb = obj_new(O_BITMAP);
    if (!hb) return 0;
    GdiObj *b = get(hb, O_BITMAP);
    b->u.bmp.w = w;
    b->u.bmp.h = h < 0 ? -h : h;
    b->u.bmp.bpp = bpp;
    if (alloc) {
        uint32_t pitch = (uint32_t)(((w * bpp + 31) / 32) * 4);
        b->u.bmp.bits = rt_heap_alloc(pitch * (uint32_t)b->u.bmp.h + 4, 1);
    }
    return hb;
}
/* CreateDIBSection(hdc, BITMAPINFO*, usage, void **bits, section, offset) */
H(gdi_CreateDIBSection) {
    UNUSED;
    INIT();
    uint32_t hb = new_bitmap((int32_t)ld32(a[1] + 4), (int32_t)ld32(a[1] + 8), ld16(a[1] + 14), 1);
    GdiObj *b = get(hb, O_BITMAP);
    if (a[3]) st32(a[3], b ? b->u.bmp.bits : 0);
    pthread_mutex_unlock(&gdi_mu);
    return hb;
}
/* CreateDIBitmap(hdc, BITMAPINFOHEADER*, init, bits, BITMAPINFO*, usage) */
H(gdi_CreateDIBitmap) {
    UNUSED;
    INIT();
    uint32_t hb = new_bitmap((int32_t)ld32(a[1] + 4), (int32_t)ld32(a[1] + 8), ld16(a[1] + 14), 1);
    pthread_mutex_unlock(&gdi_mu);
    return hb;
}
/* CreateBitmap(w, h, planes, bpp, bits) */
H(gdi_CreateBitmap) {
    UNUSED;
    INIT();
    uint32_t hb = new_bitmap((int32_t)a[0], (int32_t)a[1], (int)a[3], 1);
    pthread_mutex_unlock(&gdi_mu);
    return hb;
}
static uint32_t simple_obj(int type) {
    INIT();
    uint32_t h = obj_new(type);
    pthread_mutex_unlock(&gdi_mu);
    return h;
}
H(gdi_CreateSolidBrush) { UNUSED; return simple_obj(O_BRUSH); }
H(gdi_CreatePen) { UNUSED; return simple_obj(O_PEN); }
H(gdi_CreatePalette) { UNUSED; return simple_obj(O_PALETTE); }
H(gdi_SelectPalette) {
    UNUSED;
    INIT();
    GdiObj *dc = get(a[0], O_DC);
    uint32_t old = 0;
    if (dc) {
        old = dc->u.dc.palette;
        dc->u.dc.palette = a[1];
    }
    pthread_mutex_unlock(&gdi_mu);
    return old;
}
H(gdi_RealizePalette) { UNUSED; return 0; }

/* DC state: Set* return the previous value. */
static uint32_t dc_set(uint32_t hdc, int which, uint32_t v) {
    INIT();
    GdiObj *dc = get(hdc, O_DC);
    uint32_t old = 0;
    if (dc) {
        uint32_t *slot = which == 0   ? &dc->u.dc.text_color
                         : which == 1 ? &dc->u.dc.bk_color
                         : which == 2 ? &dc->u.dc.bk_mode
                         : which == 3 ? &dc->u.dc.text_align
                                      : &dc->u.dc.map_mode;
        old = *slot;
        *slot = v;
    }
    pthread_mutex_unlock(&gdi_mu);
    return old;
}
H(gdi_SetTextColor) { UNUSED; return dc_set(a[0], 0, a[1]); }
H(gdi_SetBkColor) { UNUSED; return dc_set(a[0], 1, a[1]); }
H(gdi_SetBkMode) { UNUSED; return dc_set(a[0], 2, a[1]); }
H(gdi_SetTextAlign) { UNUSED; return dc_set(a[0], 3, a[1]); }
H(gdi_SetMapMode) { UNUSED; return dc_set(a[0], 4, a[1]); }
H(gdi_SetWindowOrgEx) {
    UNUSED;
    if (a[3]) memset(G(a[3]), 0, 8);
    return 1;
}
/* Drawing through GDI is not shown anywhere: the game's screen is Direct3D. */
H(gdi_true) { UNUSED; return 1; }

static const ImportDef defs[] = {
    {"gdi32.dll", "CreateFontIndirectW", gdi_CreateFontIndirectW, 1, 0},
    {"gdi32.dll", "CreateFontIndirectA", gdi_CreateFontIndirectA, 1, 0},
    {"gdi32.dll", "CreateFontW", gdi_CreateFontW, 14, 0},
    {"gdi32.dll", "CreateFontA", gdi_CreateFontA, 14, 0},
    {"gdi32.dll", "GetTextMetricsW", gdi_GetTextMetricsW, 2, 0},
    {"gdi32.dll", "GetTextMetricsA", gdi_GetTextMetricsA, 2, 0},
    {"gdi32.dll", "GetGlyphOutlineW", gdi_GetGlyphOutline, 7, 0},
    {"gdi32.dll", "GetGlyphOutlineA", gdi_GetGlyphOutline, 7, 0},
    {"gdi32.dll", "GetFontUnicodeRanges", gdi_GetFontUnicodeRanges, 2, 0},
    {"gdi32.dll", "GetTextExtentPoint32W", gdi_GetTextExtentPoint32W, 4, 0},
    {"gdi32.dll", "GetTextExtentPoint32A", gdi_GetTextExtentPoint32A, 4, 0},
    {"gdi32.dll", "GetTextExtentPointW", gdi_GetTextExtentPoint32W, 4, 0},
    {"gdi32.dll", "GetStockObject", gdi_GetStockObject, 1, 0},
    {"gdi32.dll", "CreateCompatibleDC", gdi_CreateCompatibleDC, 1, 0},
    {"gdi32.dll", "DeleteDC", gdi_DeleteDC, 1, 0},
    {"gdi32.dll", "SelectObject", gdi_SelectObject, 2, 0},
    {"gdi32.dll", "GetCurrentObject", gdi_GetCurrentObject, 2, 0},
    {"gdi32.dll", "DeleteObject", gdi_DeleteObject, 1, 0},
    {"gdi32.dll", "GetObjectW", gdi_GetObjectW, 3, 0},
    {"gdi32.dll", "GetObjectA", gdi_GetObjectA, 3, 0},
    {"gdi32.dll", "CreateDIBSection", gdi_CreateDIBSection, 6, 0},
    {"gdi32.dll", "CreateDIBitmap", gdi_CreateDIBitmap, 6, 0},
    {"gdi32.dll", "CreateBitmap", gdi_CreateBitmap, 5, 0},
    {"gdi32.dll", "CreateSolidBrush", gdi_CreateSolidBrush, 1, 0},
    {"gdi32.dll", "CreatePen", gdi_CreatePen, 3, 0},
    {"gdi32.dll", "CreatePalette", gdi_CreatePalette, 1, 0},
    {"gdi32.dll", "SelectPalette", gdi_SelectPalette, 3, 0},
    {"gdi32.dll", "RealizePalette", gdi_RealizePalette, 1, 0},
    {"gdi32.dll", "SetTextColor", gdi_SetTextColor, 2, 0},
    {"gdi32.dll", "SetBkColor", gdi_SetBkColor, 2, 0},
    {"gdi32.dll", "SetBkMode", gdi_SetBkMode, 2, 0},
    {"gdi32.dll", "SetTextAlign", gdi_SetTextAlign, 2, 0},
    {"gdi32.dll", "SetMapMode", gdi_SetMapMode, 2, 0},
    {"gdi32.dll", "SetWindowOrgEx", gdi_SetWindowOrgEx, 4, 0},
    {"gdi32.dll", "BitBlt", gdi_true, 9, 0},
    {"gdi32.dll", "StretchBlt", gdi_true, 11, 0},
    {"gdi32.dll", "PatBlt", gdi_true, 6, 0},
    {"gdi32.dll", "TextOutW", gdi_true, 5, 0},
    {"gdi32.dll", "TextOutA", gdi_true, 5, 0},
    {"gdi32.dll", "ExtTextOutW", gdi_true, 8, 0},
    {"gdi32.dll", "ExtTextOutA", gdi_true, 8, 0},
};
RT_EXPORT_IMPORTS(rt_imports_gdi, defs);

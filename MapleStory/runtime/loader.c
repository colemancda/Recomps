/* PE32 loader: maps the EXE and the lifted DLLs into guest memory, relocates a DLL
 * that was lifted away from its preferred base, links imports between lifted
 * images directly and points every other IAT slot at a fake host-function address. */
#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <strings.h>
#include <unistd.h>

#include "rt.h"

RtImage g_image;
RtImage g_dlls[RT_MAX_DLLS];
unsigned g_ndlls;
/* DLLs in the order their DllMain must run: each after the DLLs it imports. */
static RtImage *init_order[RT_MAX_DLLS];
static unsigned ninit;

static uint16_t f16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t f32(const uint8_t *p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24); }

/* CRC-32 (IEEE, same polynomial as zlib) of the code section, to match the lifter's stamp. */
static uint32_t crc32_ieee(const uint8_t *p, size_t n) {
    static uint32_t table[256];
    if (!table[1]) {
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;
            for (int k = 0; k < 8; k++) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            table[i] = c;
        }
    }
    uint32_t c = 0xFFFFFFFFu;
    while (n--) c = table[(c ^ *p++) & 0xFF] ^ (c >> 8);
    return ~c;
}

static void die(const char *msg, const char *path) {
    fprintf(stderr, "recomp: %s: %s\n", path, msg);
    exit(2);
}

static const char *base_name(const char *path) {
    const char *b = strrchr(path, '\\');
    const char *f = strrchr(path, '/');
    if (f > b) b = f;
    return b ? b + 1 : path;
}

/* "MSVCRT" and "msvcrt.dll" name the same module. */
static int same_module(const char *a, const char *b) {
    a = base_name(a);
    b = base_name(b);
    size_t na = strcspn(a, "."), nb = strcspn(b, ".");
    if (na != nb || strncasecmp(a, b, na)) return 0;
    const char *ea = a[na] ? a + na : ".dll", *eb = b[nb] ? b + nb : ".dll";
    return !strcasecmp(ea, eb);
}

static const RtImageDesc *desc_named(const char *name) {
    for (unsigned m = 1; m < rt_nimages; m++)
        if (same_module(rt_images[m]->name, name)) return rt_images[m];
    return NULL;
}

RtImage *rt_image_named(const char *name) {
    for (unsigned i = 0; i < g_ndlls; i++)
        if (same_module(g_dlls[i].name, name)) return &g_dlls[i];
    return NULL;
}

RtImage *rt_image_at(uint32_t base) {
    for (unsigned i = 0; i < g_ndlls; i++)
        if (g_dlls[i].base == base) return &g_dlls[i];
    return NULL;
}

/* The file `name` in `dir`, matched case-insensitively. */
static int find_file(const char *dir, const char *name, char *out, size_t cap) {
    DIR *d = opendir(dir);
    if (!d) return 0;
    struct dirent *e;
    int found = 0;
    while ((e = readdir(d)) != NULL) {
        if (!strcasecmp(e->d_name, name)) {
            snprintf(out, cap, "%s/%s", dir, e->d_name);
            found = 1;
            break;
        }
    }
    closedir(d);
    return found;
}

static uint8_t *read_file(const char *path, long *size) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *p = malloc((size_t)n);
    if (!p || fread(p, 1, (size_t)n, f) != (size_t)n) die("read error", path);
    fclose(f);
    *size = n;
    return p;
}

/* Apply the base relocations for a load at im->base instead of `pref`. */
static void relocate(RtImage *im, uint32_t pref, uint32_t rva, uint32_t size) {
    uint32_t delta = im->base - pref;
    if (!rva || !size) die("lifted away from its preferred base, but it has no relocations", im->path);
    uint32_t p = im->base + rva, end = p + size;
    unsigned n = 0;
    while (p + 8 <= end) {
        uint32_t page = ld32(p), bsize = ld32(p + 4);
        if (bsize < 8) break;
        for (uint32_t e = p + 8; e + 2 <= p + bsize; e += 2) {
            uint16_t v = ld16(e);
            switch (v >> 12) {
            case 0: break; /* IMAGE_REL_BASED_ABSOLUTE: padding */
            case 3: {      /* IMAGE_REL_BASED_HIGHLOW */
                uint32_t a = im->base + page + (v & 0xFFFu);
                st32(a, ld32(a) + delta);
                n++;
                break;
            }
            default: die("unsupported relocation type", im->path);
            }
        }
        p += bsize;
    }
    if (g_opt.verbose) rt_log("[loader] %s: %u relocations applied (base %08x -> %08x)\n", im->name, n, pref,
                              im->base);
}

static RtImage *load_dll(const char *name, int depth);

/* The directory of the running program: system DLLs it lifts (msxml4) sit there. */
static void program_dir(char *out, size_t cap) {
    ssize_t n = readlink("/proc/self/exe", out, cap - 1);
    out[n > 0 ? n : 0] = 0;
    char *slash = strrchr(out, '/');
    if (slash) *slash = 0;
}

/* Point the IAT of `im` at lifted exports or fake host addresses. */
static void resolve_imports(RtImage *im, uint32_t imp_rva, int depth) {
    if (!imp_rva) return;
    for (uint32_t d = im->base + imp_rva;; d += 20) {
        uint32_t oft = ld32(d), name_rva = ld32(d + 12), ft = ld32(d + 16);
        if (!name_rva && !ft) break;
        const char *dll = (const char *)G(im->base + name_rva);
        RtImage *dep = desc_named(dll) ? load_dll(dll, depth + 1) : NULL;
        uint32_t lookup = oft ? oft : ft;
        for (uint32_t i = 0;; i++) {
            uint32_t t = ld32(im->base + lookup + 4 * i);
            if (!t) break;
            char ordname[32];
            const char *name = NULL;
            uint32_t ord = 0;
            if (t & 0x80000000u) {
                ord = t & 0xFFFF;
                snprintf(ordname, sizeof ordname, "#%u", ord);
            } else {
                name = (const char *)G(im->base + t + 2);
            }
            uint32_t slot = im->base + ft + 4 * i;
            if (dep) {
                uint32_t a = rt_image_export(dep, name, ord);
                if (!a) {
                    fprintf(stderr, "recomp: %s imports %s!%s, which it does not export\n", im->name, dll,
                            name ? name : ordname);
                    exit(2);
                }
                st32(slot, a);
            } else {
                st32(slot, rt_register_import(dll, name ? name : ordname, slot));
            }
        }
    }
}

/* Map `desc` from host file `path` into guest memory. */
static void map_image(RtImage *im, const RtImageDesc *desc, const char *path, int depth) {
    long fsize;
    uint8_t *file = read_file(path, &fsize);
    if (!file) {
        perror(path);
        exit(2);
    }
    if (fsize < 0x200 || f16(file) != 0x5A4D) die("not an MZ executable", path);
    uint32_t pe = f32(file + 0x3C);
    if (pe + 0x108 > (uint32_t)fsize || f32(file + pe) != 0x4550) die("not a PE executable", path);
    const uint8_t *fh = file + pe + 4;
    const uint8_t *oh = fh + 20;
    if (f16(fh) != 0x14C || f16(oh) != 0x10B) die("not a 32-bit x86 PE32 image", path);
    unsigned nsect = f16(fh + 2);
    unsigned ohsize = f16(fh + 16);
    uint32_t pref = f32(oh + 28);

    im->desc = desc;
    snprintf(im->name, sizeof im->name, "%s", desc->name);
    im->base = desc->base;
    im->size = (f32(oh + 56) + 0xFFFu) & ~0xFFFu; /* Windows maps whole pages */
    im->entry = im->base + f32(oh + 16);
    im->headers_size = f32(oh + 60);
    im->stack_reserve = f32(oh + 72);
    snprintf(im->path, sizeof im->path, "%s", path);

    if (pref != desc->pref_base || im->size != desc->size)
        die("headers differ from the lifted image: this is not the file that was lifted", path);
    if (im->base < GUEST_STACK_TOP || (uint64_t)im->base + im->size > GUEST_SIZE)
        die("image does not fit the guest layout (base/size)", path);
    for (unsigned i = 0; i <= g_ndlls; i++) {
        RtImage *o = i < g_ndlls ? &g_dlls[i] : &g_image;
        if (o != im && o->size && im->base < o->base + o->size && o->base < im->base + im->size)
            die("image overlaps another lifted image", path);
    }
    if (im->entry != desc->entry && f32(oh + 16))
        die("entry point differs from the lifted code: this is not the file that was lifted", path);

    rt_mem_commit(im->base, im->size);
    uint32_t hs = im->headers_size < (uint32_t)fsize ? im->headers_size : (uint32_t)fsize;
    memcpy(G(im->base), file, hs);
    const uint8_t *sh = oh + ohsize;
    for (unsigned i = 0; i < nsect; i++, sh += 40) {
        uint32_t vsize = f32(sh + 8), va = f32(sh + 12), rawsize = f32(sh + 16), raw = f32(sh + 20);
        uint32_t n = rawsize < vsize || vsize == 0 ? rawsize : vsize;
        if (raw >= (uint32_t)fsize) continue;
        if (raw + n > (uint32_t)fsize) n = (uint32_t)fsize - raw;
        if ((uint64_t)va + n > im->size) die("section outside image", path);
        memcpy(G(im->base + va), file + raw, n);
    }
    if (im->base != pref) {
        relocate(im, pref, f32(oh + 96 + 5 * 8), f32(oh + 96 + 5 * 8 + 4));
        st32(im->base + pe + 4 + 20 + 28, im->base); /* the mapped header's ImageBase, as Windows does */
    }

    uint32_t crc = crc32_ieee(G(desc->text_base), desc->text_size);
    if (crc != desc->text_crc)
        die("code differs from the lifted code: this is not the file that was lifted", path);

    im->exp_rva = f32(oh + 96);
    im->exp_size = f32(oh + 96 + 4);
    im->rsrc_rva = f32(oh + 96 + 2 * 8);
    im->rsrc_size = f32(oh + 96 + 2 * 8 + 4);
    uint32_t imp_rva = f32(oh + 96 + 8);
    free(file);

    resolve_imports(im, imp_rva, depth);
    /* Words the lifter found to be wrong in the image (see repair_scope_tables in lift.py);
     * these overwrite import stubs just stored above. */
    for (uint32_t i = 0; i < desc->npatches; i++) st32(desc->patches[i][0], desc->patches[i][1]);
}

static RtImage *load_dll(const char *name, int depth) {
    RtImage *im = rt_image_named(name);
    if (im) return im;
    const RtImageDesc *desc = desc_named(name);
    if (!desc) return NULL;
    if (depth > 8 || g_ndlls == RT_MAX_DLLS) die("too many lifted DLLs, or an import cycle", desc->name);
    char path[512];
    char self[512];
    program_dir(self, sizeof self);
    /* The game's DLLs are in its folder; system DLLs the program lifts (msxml4) next to the program. */
    if (!find_file(g_opt.gamedir, desc->name, path, sizeof path) &&
        !find_file(g_opt.exedir, desc->name, path, sizeof path) && !find_file(self, desc->name, path, sizeof path)) {
        fprintf(stderr, "recomp: lifted DLL %s not found in %s\n", desc->name, g_opt.gamedir);
        exit(2);
    }
    im = &g_dlls[g_ndlls++]; /* reserved before its imports, so a cycle finds it */
    memset(im, 0, sizeof *im);
    im->desc = desc;
    {
        char dir[260];
        snprintf(dir, sizeof dir, "%s", g_image.guest_path);
        char *bs = strrchr(dir, '\\');
        if (bs) bs[1] = 0;
        else dir[0] = 0;
        snprintf(im->guest_path, sizeof im->guest_path, "%s%s", dir, desc->name);
    }
    map_image(im, desc, path, depth);
    init_order[ninit++] = im;
    if (g_opt.verbose || g_trace_imports)
        rt_log("[loader] %s mapped at %08x, DllMain %08x\n", desc->name, im->base, im->entry);
    return im;
}

RtImage *rt_load_dll(const char *name) { return load_dll(name, 0); }

RtImage *rt_load_data_dll(const char *name) {
    RtImage *im = rt_image_named(name);
    if (im) return im;
    char path[512];
    char self[512];
    program_dir(self, sizeof self);
    if (g_ndlls == RT_MAX_DLLS || (!find_file(g_opt.gamedir, base_name(name), path, sizeof path) &&
                                   !find_file(self, base_name(name), path, sizeof path)))
        return NULL;
    long fsize;
    uint8_t *file = read_file(path, &fsize);
    if (!file) return NULL;
    uint32_t pe = fsize > 0x40 ? f32(file + 0x3C) : 0;
    if (fsize < 0x200 || f16(file) != 0x5A4D || pe + 0x108 > (uint32_t)fsize || f32(file + pe) != 0x4550) {
        free(file);
        return NULL;
    }
    const uint8_t *fh = file + pe + 4, *oh = fh + 20;
    if (f32(oh + 96 + 4)) { /* it has exports: code the recompilation does not have */
        free(file);
        return NULL;
    }
    uint32_t size = (f32(oh + 56) + 0xFFFu) & ~0xFFFu;
    uint32_t base = rt_valloc(0, size, 0x3000, 4);
    if (!base) {
        free(file);
        return NULL;
    }
    im = &g_dlls[g_ndlls++];
    memset(im, 0, sizeof *im);
    snprintf(im->name, sizeof im->name, "%s", base_name(name));
    for (char *p = im->name; *p; p++) *p = (char)tolower((unsigned char)*p);
    snprintf(im->path, sizeof im->path, "%s", path);
    snprintf(im->guest_path, sizeof im->guest_path, "%s", name);
    im->base = base;
    im->size = size;
    im->entry = base;
    im->attached = 1;
    uint32_t hs = f32(oh + 60) < (uint32_t)fsize ? f32(oh + 60) : (uint32_t)fsize;
    memcpy(G(base), file, hs);
    const uint8_t *sh = oh + f16(fh + 16);
    for (unsigned i = 0; i < f16(fh + 2); i++, sh += 40) {
        uint32_t vsize = f32(sh + 8), va = f32(sh + 12), rawsize = f32(sh + 16), raw = f32(sh + 20);
        uint32_t n = rawsize < vsize || vsize == 0 ? rawsize : vsize;
        if (raw >= (uint32_t)fsize || (uint64_t)va + n > size) continue;
        if (raw + n > (uint32_t)fsize) n = (uint32_t)fsize - raw;
        memcpy(G(base + va), file + raw, n);
    }
    im->rsrc_rva = f32(oh + 96 + 2 * 8);
    im->rsrc_size = f32(oh + 96 + 2 * 8 + 4);
    free(file);
    if (g_opt.verbose || g_trace_imports) rt_log("[loader] %s mapped as data at %08x\n", im->name, base);
    return im;
}

RtImage *rt_image_of(uint32_t addr) {
    if (g_image.size && addr - g_image.base < g_image.size) return &g_image;
    for (unsigned i = 0; i < g_ndlls; i++)
        if (addr - g_dlls[i].base < g_dlls[i].size) return &g_dlls[i];
    return NULL;
}

void rt_load_pe(const char *path) {
    const RtImageDesc *desc = rt_images[0];
    snprintf(g_image.path, sizeof g_image.path, "%s", path);
    if (!g_image.guest_path[0])
        snprintf(g_image.guest_path, sizeof g_image.guest_path, "C:\\Game\\%s", base_name(path));
    map_image(&g_image, desc, path, 0);
    if (g_image.base + g_image.size > GUEST_HEAP_BASE) die("the EXE overlaps the heap", path);
}

uint32_t rt_image_export(RtImage *im, const char *name, uint32_t ordinal) {
    if (!im->exp_rva) return 0;
    uint32_t d = im->base + im->exp_rva;
    uint32_t obase = ld32(d + 16), nfuncs = ld32(d + 20), nnames = ld32(d + 24);
    uint32_t funcs = im->base + ld32(d + 28), names = im->base + ld32(d + 32), ords = im->base + ld32(d + 36);
    uint32_t idx = 0xFFFFFFFFu;
    if (name) {
        uint32_t lo = 0, hi = nnames; /* the name table is sorted */
        while (lo < hi) {
            uint32_t mid = (lo + hi) / 2;
            int r = strcmp(name, (const char *)G(im->base + ld32(names + 4 * mid)));
            if (!r) {
                idx = ld16(ords + 2 * mid);
                break;
            }
            if (r < 0) hi = mid;
            else lo = mid + 1;
        }
    } else {
        idx = ordinal - obase;
    }
    if (idx >= nfuncs) return 0;
    uint32_t rva = ld32(funcs + 4 * idx);
    if (!rva) return 0;
    if (rva - im->exp_rva < im->exp_size) {
        /* Forwarder "DLL.name" or "DLL.#ord". */
        char fwd[256];
        snprintf(fwd, sizeof fwd, "%s", (const char *)G(im->base + rva));
        char *dot = strchr(fwd, '.');
        if (!dot) return 0;
        *dot = 0;
        const char *fname = dot + 1;
        char dll[264];
        snprintf(dll, sizeof dll, "%s.dll", fwd);
        RtImage *dep = desc_named(dll) ? load_dll(dll, 1) : NULL;
        if (dep) return fname[0] == '#' ? rt_image_export(dep, NULL, (uint32_t)atoi(fname + 1))
                                        : rt_image_export(dep, fname, 0);
        const ImportDef *def = rt_find_import_def(dll, fname);
        return rt_host_fn(dll, fname, def ? def->fn : NULL, def ? def->nargs : 0);
    }
    return im->base + rva;
}

int rt_dll_attach(CPU *c) {
    for (unsigned i = 0; i < ninit; i++) {
        RtImage *im = init_order[i];
        if (im->attached) continue;
        im->attached = 1;
        if (im->entry == im->base) continue; /* no entry point */
        uint32_t args[3] = {im->base, 1 /* DLL_PROCESS_ATTACH */, 1 /* static load */}, r = 0;
        if (g_opt.verbose || g_trace_imports) rt_log("[loader] DllMain(%s, PROCESS_ATTACH)\n", im->name);
        if (!rt_call_guest(c, im->entry, RT_STDCALL, 3, args, &r)) return 0;
        if (!r) {
            rt_log("recomp: DllMain of %s returned FALSE\n", im->name);
            return 0;
        }
    }
    return 1;
}

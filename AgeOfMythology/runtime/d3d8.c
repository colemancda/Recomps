/* Direct3D 8, null device (plan step 2).
 *
 * Every interface method the game can reach exists with its real argument count,
 * keeps the state it is given so that the Get methods answer consistently, and
 * returns success. Textures, surfaces and buffers are backed by guest memory, so
 * Lock, LockRect, CopyRects and UpdateTexture really move bytes. Nothing is
 * drawn: Present shows a black frame, which still counts frames for --frames
 * and the virtual clock. Step 3 replaces the drawing with OpenGL ES.
 *
 * Vtable orders follow d3d8.h. Object layout in guest memory is the one of
 * com.h: {vtable, host index, magic}.
 */
#include <stdio.h>
#include <stdlib.h>

#include "com.h"
#include "rt.h"
#include "video.h"

#define D3D_OK 0u
#define D3DERR_NOTAVAILABLE 0x8876086Au
#define D3DERR_INVALIDCALL 0x8876086Cu

enum { K_FREE = 0, K_D3D, K_DEVICE, K_TEXTURE, K_SURFACE, K_VB, K_IB, K_SWAPCHAIN };
#define MAGIC(k) (0xD3D80000u | (k))

/* D3DFORMAT values used here. */
enum {
    FMT_R8G8B8 = 20, FMT_A8R8G8B8 = 21, FMT_X8R8G8B8 = 22, FMT_R5G6B5 = 23, FMT_X1R5G5B5 = 24,
    FMT_A1R5G5B5 = 25, FMT_A4R4G4B4 = 26, FMT_R3G3B2 = 27, FMT_A8 = 28, FMT_A8R3G3B2 = 29,
    FMT_X4R4G4B4 = 30, FMT_A8P8 = 40, FMT_P8 = 41, FMT_L8 = 50, FMT_A8L8 = 51, FMT_A4L4 = 52,
    FMT_D16_LOCKABLE = 70, FMT_D32 = 71, FMT_D15S1 = 73, FMT_D24S8 = 75, FMT_D24X8 = 77, FMT_D24X4S4 = 79,
    FMT_D16 = 80, FMT_INDEX16 = 101, FMT_INDEX32 = 102,
    FMT_DXT1 = 0x31545844, FMT_DXT2 = 0x32545844, FMT_DXT3 = 0x33545844, FMT_DXT4 = 0x34545844,
    FMT_DXT5 = 0x35545844,
};
/* D3DRESOURCETYPE */
enum { RT_SURFACE = 1, RT_TEXTURE = 3, RT_VERTEXBUFFER = 6, RT_INDEXBUFFER = 7 };

typedef struct Obj {
    int kind;
    uint32_t obj; /* guest object */
    int refs;
    /* surfaces and textures */
    uint32_t width, height, format, usage, pool, levels;
    uint32_t data, pitch, bytes; /* surface: guest memory; buffers: data and size */
    int container;               /* surface of a texture: the texture's index, else -1 */
    int level_surf[16];          /* texture: the surface of each level */
    uint32_t fvf;                /* vertex buffer */
    uint32_t lod, priority;
} Obj;

#define MAX_OBJS 8192
static Obj objs[MAX_OBJS];
static int nobjs;
static uint32_t vt_d3d, vt_device, vt_texture, vt_surface, vt_vb, vt_ib, vt_swapchain;

/* The one device. */
static struct {
    int obj;                     /* index of the device object, -1 if none */
    uint32_t pp[13];             /* D3DPRESENT_PARAMETERS as created */
    uint32_t focus, flags;
    int backbuffer, depth;       /* surface indices */
    int render_target, zstencil; /* current */
    uint32_t rs[256];
    uint32_t tss[8][32];
    int texture[8];
    uint32_t transform[512][16];
    uint32_t viewport[6];
    uint32_t material[17];
    uint32_t light[16][26];
    uint32_t light_on[16];
    uint32_t clip[8][4];
    int stream[16];
    uint32_t stride[16];
    int indices;
    uint32_t base_vertex;
    uint32_t vshader, pshader;
    uint32_t next_vshader, next_pshader, next_block;
    uint32_t gamma[3 * 256 / 2]; /* D3DGAMMARAMP: 3 x 256 WORDs */
    int in_scene;
    unsigned draws, frames;
    uint16_t *black;
} dev = {.obj = -1};

/* ------------------------------------------------------------- objects */
static int obj_new(int kind) {
    int i;
    for (i = 0; i < nobjs && objs[i].kind != K_FREE; i++) {
    }
    if (i == nobjs) {
        if (nobjs == MAX_OBJS) {
            rt_log("d3d8: too many objects\n");
            rt_exit(3);
        }
        nobjs++;
    }
    memset(&objs[i], 0, sizeof objs[i]);
    objs[i].kind = kind;
    objs[i].refs = 1;
    objs[i].container = -1;
    uint32_t vt = kind == K_D3D       ? vt_d3d
                  : kind == K_DEVICE  ? vt_device
                  : kind == K_TEXTURE ? vt_texture
                  : kind == K_SURFACE ? vt_surface
                  : kind == K_VB      ? vt_vb
                  : kind == K_IB      ? vt_ib
                                      : vt_swapchain;
    objs[i].obj = rt_com_object(vt, (uint32_t)i, MAGIC(kind));
    return i;
}

static Obj *get(uint32_t o, int kind) {
    int i = rt_com_index(o, MAGIC(kind));
    return i < 0 ? NULL : &objs[i];
}

static int any_index(uint32_t o) {
    if (!o || !rt_guest_readable(o, 12) || (ld32(o + 8) & 0xFFFF0000u) != 0xD3D80000u) return -1;
    return (int)ld32(o + 4);
}

static void obj_destroy(int i) {
    Obj *ob = &objs[i];
    if (ob->kind == K_TEXTURE)
        for (uint32_t l = 0; l < ob->levels; l++) {
            int s = ob->level_surf[l];
            objs[s].container = -1;
            if (objs[s].data) rt_heap_free(objs[s].data);
            rt_com_free(objs[s].obj);
            objs[s].kind = K_FREE;
        }
    if ((ob->kind == K_SURFACE || ob->kind == K_VB || ob->kind == K_IB) && ob->data) rt_heap_free(ob->data);
    rt_com_free(ob->obj);
    ob->kind = K_FREE;
}

/* A surface of a texture lives as long as its texture: its reference count is the texture's. */
static int owner(int i) { return objs[i].kind == K_SURFACE && objs[i].container >= 0 ? objs[i].container : i; }

static uint32_t addref(uint32_t o) {
    int i = any_index(o);
    if (i < 0) return 0;
    i = owner(i);
    return (uint32_t)++objs[i].refs;
}

static uint32_t release(uint32_t o) {
    int i = any_index(o);
    if (i < 0) return 0;
    i = owner(i);
    int n = --objs[i].refs;
    if (n <= 0 && objs[i].kind != K_D3D && objs[i].kind != K_DEVICE) obj_destroy(i);
    return n < 0 ? 0 : (uint32_t)n;
}

/* ------------------------------------------------------------- formats */
static int is_dxt(uint32_t f) { return f >= FMT_DXT1 && f <= FMT_DXT5 && (f & 0xFFFFFFu) == 0x545844; }

static uint32_t bpp(uint32_t f) {
    switch (f) {
    case FMT_A8R8G8B8: case FMT_X8R8G8B8: case FMT_D32: case FMT_D24S8: case FMT_D24X8: case FMT_D24X4S4:
    case FMT_INDEX32: return 4;
    case FMT_R8G8B8: return 3;
    case FMT_R3G3B2: case FMT_A8: case FMT_P8: case FMT_L8: case FMT_A4L4: return 1;
    default: return 2;
    }
}

/* Bytes in one row of blocks/pixels, and the number of such rows. */
static uint32_t row_bytes(uint32_t f, uint32_t w) {
    if (is_dxt(f)) return ((w + 3) / 4) * (f == FMT_DXT1 ? 8 : 16);
    return w * bpp(f);
}
static uint32_t rows(uint32_t f, uint32_t h) { return is_dxt(f) ? (h + 3) / 4 : h; }

static int format_ok(uint32_t f) {
    switch (f) {
    case FMT_A8R8G8B8: case FMT_X8R8G8B8: case FMT_R5G6B5: case FMT_X1R5G5B5: case FMT_A1R5G5B5:
    case FMT_A4R4G4B4: case FMT_A8: case FMT_L8: case FMT_A8L8: case FMT_DXT1: case FMT_DXT3: case FMT_DXT5:
    case FMT_D16: case FMT_D16_LOCKABLE: case FMT_D24S8: case FMT_D24X8: case FMT_D32:
        return 1;
    default: return 0;
    }
}

static int surface_new(uint32_t w, uint32_t h, uint32_t fmt, uint32_t usage, uint32_t pool, int alloc) {
    int i = obj_new(K_SURFACE);
    Obj *s = &objs[i];
    s->width = w;
    s->height = h;
    s->format = fmt;
    s->usage = usage;
    s->pool = pool;
    s->pitch = (row_bytes(fmt, w) + 3) & ~3u;
    s->bytes = s->pitch * rows(fmt, h);
    if (alloc) {
        s->data = rt_heap_alloc(s->bytes ? s->bytes : 4, 1);
        if (!s->data) {
            rt_log("d3d8: out of guest memory for a %ux%u surface\n", w, h);
            rt_exit(3);
        }
    }
    return i;
}

static void surface_desc(Obj *s, uint32_t p, uint32_t type) {
    st32(p, s->format);
    st32(p + 4, type);
    st32(p + 8, s->usage);
    st32(p + 12, s->pool);
    st32(p + 16, s->bytes);
    st32(p + 20, 0); /* MultiSampleType */
    st32(p + 24, s->width);
    st32(p + 28, s->height);
}

static void lock_rect(Obj *s, uint32_t locked, uint32_t rect) {
    uint32_t off = 0;
    if (rect) {
        uint32_t x = ld32(rect), y = ld32(rect + 4);
        if (is_dxt(s->format)) off = (y / 4) * s->pitch + row_bytes(s->format, x & ~3u);
        else off = y * s->pitch + x * bpp(s->format);
    }
    st32(locked, s->pitch);
    st32(locked + 4, s->data + off);
}

/* -------------------------------------------------------- IUnknown parts */
H(d3d_QueryInterface) {
    UNUSED;
    if (a[2]) st32(a[2], a[0]);
    addref(a[0]);
    return S_OK;
}
H(d3d_AddRef) { UNUSED; return addref(a[0]); }
H(d3d_Release) { UNUSED; return release(a[0]); }
H(d3d_ok) { UNUSED; return D3D_OK; }
H(d3d_zero) { UNUSED; return 0; }
H(d3d_notavail) { UNUSED; return D3DERR_NOTAVAILABLE; }

/* --------------------------------------------------------------- IDirect3D8 */
static const struct { uint32_t w, h; } modes[] = {{640, 480}, {800, 600}, {1024, 768}, {1280, 1024}};
static const uint32_t mode_formats[] = {FMT_R5G6B5, FMT_X8R8G8B8};
#define NMODES (RT_COUNTOF(modes) * RT_COUNTOF(mode_formats))

static void put_mode(uint32_t p, uint32_t i) {
    st32(p, modes[i % RT_COUNTOF(modes)].w);
    st32(p + 4, modes[i % RT_COUNTOF(modes)].h);
    st32(p + 8, 60);
    st32(p + 12, mode_formats[i / RT_COUNTOF(modes)]);
}

H(d3d_GetAdapterCount) { UNUSED; return 1; }
H(d3d_GetAdapterIdentifier) {
    UNUSED;
    if (a[1]) return D3DERR_INVALIDCALL;
    uint32_t p = a[3];
    memset(G(p), 0, 1068);
    strcpy((char *)G(p), "recomp.dll");
    strcpy((char *)G(p + 512), "Recompilation null device");
    st32(p + 1024, 0x00000000u);
    st32(p + 1028, 0x0004000Au); /* DriverVersion 4.10... */
    st32(p + 1032, 0x10DE);      /* VendorId */
    st32(p + 1036, 0x0110);      /* DeviceId */
    st32(p + 1064, 1);           /* WHQLLevel */
    return D3D_OK;
}
H(d3d_GetAdapterModeCount) { UNUSED; return a[1] ? 0 : (uint32_t)NMODES; }
H(d3d_EnumAdapterModes) {
    UNUSED;
    if (a[1] || a[2] >= NMODES) return D3DERR_INVALIDCALL;
    put_mode(a[3], a[2]);
    return D3D_OK;
}
H(d3d_GetAdapterDisplayMode) {
    UNUSED;
    st32(a[2], 1024);
    st32(a[2] + 4, 768);
    st32(a[2] + 8, 60);
    st32(a[2] + 12, FMT_R5G6B5);
    return D3D_OK;
}
H(d3d_CheckDeviceType) { UNUSED; return a[2] == 1 /* HAL */ ? D3D_OK : D3DERR_NOTAVAILABLE; }
H(d3d_CheckDeviceFormat) {
    UNUSED;
    /* (this, Adapter, DeviceType, AdapterFormat, Usage, RType, CheckFormat) */
    return format_ok(a[6]) ? D3D_OK : D3DERR_NOTAVAILABLE;
}
H(d3d_CheckDeviceMultiSampleType) { UNUSED; return a[5] == 0 ? D3D_OK : D3DERR_NOTAVAILABLE; }
H(d3d_CheckDepthStencilMatch) { UNUSED; return D3D_OK; }

/* D3DCAPS8, 53 DWORDs: a fixed-function card of the game's time, without shaders. */
static void fill_caps(uint32_t p) {
    float f;
    uint32_t c[53];
    memset(c, 0, sizeof c);
    c[0] = 1;                 /* DeviceType: HAL */
    c[2] = 0x00020000u;       /* Caps: D3DCAPS_READ_SCANLINE */
    c[3] = 0x00020000u | 0x20000000u | 0x80000000u; /* Caps2: CANRENDERWINDOWED, DYNAMICTEXTURES, FULLSCREENGAMMA */
    c[4] = 0x00000020u;       /* Caps3: ALPHA_FULLSCREEN_FLIP_OR_DISCARD */
    c[5] = 0x8000000Fu;       /* PresentationIntervals: IMMEDIATE, ONE..FOUR */
    c[6] = 0x00000001u;       /* CursorCaps: COLOR */
    c[7] = 0x00000010u | 0x00000040u | 0x00000080u | 0x00000200u | 0x00000400u | 0x00000800u | 0x00004000u |
           0x00008000u | 0x00010000u | 0x00080000u; /* DevCaps: exec/tl vertex sysmem+vidmem, texture, drawprim2, HW raster */
    c[8] = 0x00000002u | 0x00000004u | 0x00000008u | 0x00000010u | 0x00000020u | 0x00000040u | 0x00000080u |
           0x00000100u | 0x00000800u; /* PrimitiveMiscCaps */
    c[9] = 0x00000001u | 0x00000010u | 0x00000080u | 0x00000100u | 0x00000400u | 0x00002000u | 0x00010000u |
           0x00020000u | 0x00100000u | 0x00200000u; /* RasterCaps: dither, ztest, fogvertex, fogtable, mipmaplodbias, ... */
    c[10] = 0xFF;             /* ZCmpCaps: all */
    c[11] = 0x1FFF;           /* SrcBlendCaps */
    c[12] = 0x1FFF;           /* DestBlendCaps */
    c[13] = 0xFF;             /* AlphaCmpCaps */
    c[14] = 0x00000008u | 0x00000200u | 0x00004000u | 0x00080000u; /* ShadeCaps: gouraud, specular, alpha, fog */
    c[15] = 0x00000001u | 0x00000004u | 0x00000008u | 0x00000040u | 0x00000400u | 0x00004000u; /* TextureCaps */
    c[16] = 0x00000100u | 0x00000200u | 0x00010000u | 0x00020000u | 0x01000000u | 0x02000000u; /* TextureFilterCaps */
    c[19] = 0x1F;             /* TextureAddressCaps */
    c[21] = 0x1F;             /* LineCaps */
    c[22] = 2048;             /* MaxTextureWidth */
    c[23] = 2048;             /* MaxTextureHeight */
    c[25] = 8192;             /* MaxTextureRepeat */
    c[26] = 2048;             /* MaxTextureAspectRatio */
    c[27] = 1;                /* MaxAnisotropy */
    f = 1e10f; memcpy(&c[28], &f, 4); /* MaxVertexW */
    f = -2048; memcpy(&c[29], &f, 4);
    memcpy(&c[30], &f, 4);
    f = 2048; memcpy(&c[31], &f, 4);
    memcpy(&c[32], &f, 4);
    c[34] = 0xFF;             /* StencilCaps */
    c[35] = 0x00080008u;      /* FVFCaps: 8 texture coordinate sets */
    c[36] = 0x03FFFFFFu;      /* TextureOpCaps */
    c[37] = 8;                /* MaxTextureBlendStages */
    c[38] = 2;                /* MaxSimultaneousTextures */
    c[39] = 0x0000003Bu;      /* VertexProcessingCaps */
    c[40] = 8;                /* MaxActiveLights */
    c[41] = 6;                /* MaxUserClipPlanes */
    c[42] = 4;                /* MaxVertexBlendMatrices */
    f = 1; memcpy(&c[44], &f, 4); /* MaxPointSize */
    c[45] = 0xFFFFF;          /* MaxPrimitiveCount */
    c[46] = 0xFFFFF;          /* MaxVertexIndex */
    c[47] = 16;               /* MaxStreams */
    c[48] = 255;              /* MaxStreamStride */
    c[49] = 0xFFFE0000u;      /* VertexShaderVersion 0.0 */
    c[51] = 0xFFFF0000u;      /* PixelShaderVersion 0.0 */
    for (int i = 0; i < 53; i++) st32(p + 4 * (uint32_t)i, c[i]);
}

H(d3d_GetDeviceCaps) {
    UNUSED;
    if (a[1]) return D3DERR_INVALIDCALL;
    fill_caps(a[3]);
    return D3D_OK;
}
H(d3d_GetAdapterMonitor) { UNUSED; return 0x00010001u; }

static void device_reset(uint32_t pp) {
    for (int i = 0; i < 13; i++) dev.pp[i] = ld32(pp + 4 * (uint32_t)i);
    if (!dev.pp[0]) dev.pp[0] = 640;
    if (!dev.pp[1]) dev.pp[1] = 480;
    if (!dev.pp[2]) dev.pp[2] = FMT_R5G6B5;
    if (dev.backbuffer >= 0 && objs[dev.backbuffer].kind == K_SURFACE) obj_destroy(dev.backbuffer);
    if (dev.depth >= 0 && objs[dev.depth].kind == K_SURFACE) obj_destroy(dev.depth);
    dev.backbuffer = surface_new(dev.pp[0], dev.pp[1], dev.pp[2], 1 /* RENDERTARGET */, 0, 1);
    dev.depth = dev.pp[8] ? surface_new(dev.pp[0], dev.pp[1], dev.pp[9], 2 /* DEPTHSTENCIL */, 0, 0) : -1;
    dev.render_target = dev.backbuffer;
    dev.zstencil = dev.depth;
    dev.viewport[0] = dev.viewport[1] = 0;
    dev.viewport[2] = dev.pp[0];
    dev.viewport[3] = dev.pp[1];
    float one = 1;
    dev.viewport[4] = 0;
    memcpy(&dev.viewport[5], &one, 4);
    free(dev.black);
    dev.black = calloc((size_t)dev.pp[0] * dev.pp[1], 2);
}

H(d3d_CreateDevice) {
    /* (this, Adapter, DeviceType, hFocusWindow, BehaviorFlags, pPresentationParameters, ppDevice) */
    UNUSED;
    if (dev.obj >= 0) {
        rt_log("d3d8: a second device was requested\n");
        return D3DERR_NOTAVAILABLE;
    }
    dev.focus = a[3];
    dev.flags = a[4];
    dev.backbuffer = dev.depth = -1;
    for (int i = 0; i < 8; i++) dev.texture[i] = -1;
    for (int i = 0; i < 16; i++) dev.stream[i] = -1;
    dev.indices = -1;
    dev.next_vshader = 0xF0000001u; /* above every FVF code, as Wine numbers them */
    dev.next_pshader = 1;
    dev.next_block = 1;
    for (int i = 0; i < 512; i++) {
        float one = 1;
        memset(dev.transform[i], 0, sizeof dev.transform[i]);
        for (int k = 0; k < 4; k++) memcpy(&dev.transform[i][k * 5], &one, 4);
    }
    dev.obj = obj_new(K_DEVICE);
    device_reset(a[5]);
    rt_log("d3d8: device %ux%u format %u, %s fullscreen, flags %#x\n", dev.pp[0], dev.pp[1], dev.pp[2],
           dev.pp[7] ? "windowed" : "", a[4]);
    st32(a[6], objs[dev.obj].obj);
    return D3D_OK;
}

static const ComMethod d3d_methods[] = {
    {"QueryInterface", d3d_QueryInterface, 3},
    {"AddRef", d3d_AddRef, 1},
    {"Release", d3d_Release, 1},
    {"RegisterSoftwareDevice", d3d_notavail, 2},
    {"GetAdapterCount", d3d_GetAdapterCount, 1},
    {"GetAdapterIdentifier", d3d_GetAdapterIdentifier, 4},
    {"GetAdapterModeCount", d3d_GetAdapterModeCount, 2},
    {"EnumAdapterModes", d3d_EnumAdapterModes, 4},
    {"GetAdapterDisplayMode", d3d_GetAdapterDisplayMode, 3},
    {"CheckDeviceType", d3d_CheckDeviceType, 6},
    {"CheckDeviceFormat", d3d_CheckDeviceFormat, 7},
    {"CheckDeviceMultiSampleType", d3d_CheckDeviceMultiSampleType, 6},
    {"CheckDepthStencilMatch", d3d_CheckDepthStencilMatch, 6},
    {"GetDeviceCaps", d3d_GetDeviceCaps, 4},
    {"GetAdapterMonitor", d3d_GetAdapterMonitor, 2},
    {"CreateDevice", d3d_CreateDevice, 7},
};

/* --------------------------------------------------------- IDirect3DDevice8 */
static uint32_t obj_of(int i) { return i >= 0 ? objs[i].obj : 0; }
static void out_obj(uint32_t pp, int i) {
    if (i >= 0) addref(objs[i].obj);
    st32(pp, obj_of(i));
}

H(dev_TestCooperativeLevel) { UNUSED; return D3D_OK; }
H(dev_GetAvailableTextureMem) { UNUSED; return 64u << 20; }
H(dev_GetDirect3D) {
    UNUSED;
    for (int i = 0; i < nobjs; i++)
        if (objs[i].kind == K_D3D) {
            out_obj(a[1], i);
            return D3D_OK;
        }
    return D3DERR_INVALIDCALL;
}
H(dev_GetDeviceCaps) { UNUSED; fill_caps(a[1]); return D3D_OK; }
H(dev_GetDisplayMode) {
    UNUSED;
    st32(a[1], dev.pp[0]);
    st32(a[1] + 4, dev.pp[1]);
    st32(a[1] + 8, 60);
    st32(a[1] + 12, dev.pp[2]);
    return D3D_OK;
}
H(dev_GetCreationParameters) {
    UNUSED;
    st32(a[1], 0);
    st32(a[1] + 4, 1);
    st32(a[1] + 8, dev.focus);
    st32(a[1] + 12, dev.flags);
    return D3D_OK;
}
H(dev_ShowCursor) {
    UNUSED;
    static uint32_t shown;
    uint32_t was = shown;
    shown = a[1] != 0;
    return was;
}
H(dev_Reset) {
    UNUSED;
    device_reset(a[1]);
    return D3D_OK;
}
H(dev_Present) {
    UNUSED;
    dev.frames++;
    if (g_opt.verbose && dev.frames <= 3) rt_log("d3d8: frame %u: %u draws\n", dev.frames, dev.draws);
    dev.draws = 0;
    rt_video_present(dev.black, (int)dev.pp[0], (int)dev.pp[1], (int)dev.pp[0] * 2, 0);
    return D3D_OK;
}
H(dev_GetBackBuffer) {
    UNUSED;
    out_obj(a[3], dev.backbuffer);
    return D3D_OK;
}
H(dev_GetRasterStatus) {
    UNUSED;
    st32(a[1], 0);
    st32(a[1] + 4, 0);
    return D3D_OK;
}
H(dev_SetGammaRamp) {
    UNUSED;
    memcpy(dev.gamma, G(a[2]), sizeof dev.gamma);
    return D3D_OK;
}
H(dev_GetGammaRamp) {
    UNUSED;
    memcpy(G(a[1]), dev.gamma, sizeof dev.gamma);
    return D3D_OK;
}

H(dev_CreateTexture) {
    /* (this, Width, Height, Levels, Usage, Format, Pool, ppTexture) */
    UNUSED;
    uint32_t w = a[1], h = a[2], levels = a[3];
    if (!w || !h || !format_ok(a[5])) return D3DERR_INVALIDCALL;
    uint32_t full = 1;
    for (uint32_t x = w > h ? w : h; x > 1; x >>= 1) full++;
    if (!levels || levels > full) levels = full;
    if (levels > 16) levels = 16;
    int t = obj_new(K_TEXTURE);
    Obj *tx = &objs[t];
    tx->width = w;
    tx->height = h;
    tx->levels = levels;
    tx->usage = a[4];
    tx->format = a[5];
    tx->pool = a[6];
    for (uint32_t l = 0; l < levels; l++) {
        uint32_t lw = w >> l ? w >> l : 1, lh = h >> l ? h >> l : 1;
        int s = surface_new(lw, lh, a[5], a[4], a[6], 1);
        objs[s].container = t;
        objs[t].level_surf[l] = s; /* objs may not move; tx stays valid */
    }
    st32(a[7], objs[t].obj);
    return D3D_OK;
}

static int buffer_new(int kind, uint32_t length, uint32_t usage, uint32_t fmt, uint32_t pool) {
    int i = obj_new(kind);
    Obj *b = &objs[i];
    b->bytes = length;
    b->usage = usage;
    b->pool = pool;
    if (kind == K_VB) {
        b->fvf = fmt;
        b->format = 100; /* D3DFMT_VERTEXDATA */
    } else {
        b->format = fmt;
    }
    b->data = rt_heap_alloc(length ? length : 4, 1);
    if (!b->data) {
        rt_log("d3d8: out of guest memory for a %u-byte buffer\n", length);
        rt_exit(3);
    }
    return i;
}

H(dev_CreateVertexBuffer) {
    UNUSED;
    st32(a[5], objs[buffer_new(K_VB, a[1], a[2], a[3], a[4])].obj);
    return D3D_OK;
}
H(dev_CreateIndexBuffer) {
    UNUSED;
    st32(a[5], objs[buffer_new(K_IB, a[1], a[2], a[3], a[4])].obj);
    return D3D_OK;
}
H(dev_CreateRenderTarget) {
    UNUSED;
    st32(a[6], objs[surface_new(a[1], a[2], a[3], 1, 0, 1)].obj);
    return D3D_OK;
}
H(dev_CreateDepthStencilSurface) {
    UNUSED;
    st32(a[5], objs[surface_new(a[1], a[2], a[3], 2, 0, 0)].obj);
    return D3D_OK;
}
H(dev_CreateImageSurface) {
    UNUSED;
    if (!format_ok(a[3])) return D3DERR_INVALIDCALL;
    st32(a[4], objs[surface_new(a[1], a[2], a[3], 0, 2 /* SYSTEMMEM */, 1)].obj);
    return D3D_OK;
}

static void copy_rect(Obj *src, uint32_t x, uint32_t y, uint32_t w, uint32_t h, Obj *dst, uint32_t dx, uint32_t dy) {
    if (!src->data || !dst->data) return;
    if (x + w > src->width) w = src->width - x;
    if (y + h > src->height) h = src->height - y;
    if (dx + w > dst->width) w = dst->width - dx;
    if (dy + h > dst->height) h = dst->height - dy;
    int dxt = is_dxt(src->format);
    uint32_t b = row_bytes(src->format, w), n = rows(src->format, h);
    uint32_t so = (dxt ? y / 4 : y) * src->pitch + row_bytes(src->format, x);
    uint32_t doff = (dxt ? dy / 4 : dy) * dst->pitch + row_bytes(dst->format, dx);
    for (uint32_t r = 0; r < n; r++) memmove(G(dst->data + doff + r * dst->pitch), G(src->data + so + r * src->pitch), b);
}

H(dev_CopyRects) {
    /* (this, pSourceSurface, pSourceRectsArray, cRects, pDestinationSurface, pDestPointsArray) */
    UNUSED;
    Obj *s = get(a[1], K_SURFACE), *d = get(a[4], K_SURFACE);
    if (!s || !d || s->format != d->format) return D3DERR_INVALIDCALL;
    if (!a[3] || !a[2]) {
        copy_rect(s, 0, 0, s->width, s->height, d, 0, 0);
        return D3D_OK;
    }
    for (uint32_t i = 0; i < a[3]; i++) {
        uint32_t r = a[2] + 16 * i;
        uint32_t l = ld32(r), t = ld32(r + 4), rr = ld32(r + 8), bb = ld32(r + 12);
        uint32_t dx = a[5] ? ld32(a[5] + 8 * i) : l, dy = a[5] ? ld32(a[5] + 8 * i + 4) : t;
        copy_rect(s, l, t, rr - l, bb - t, d, dx, dy);
    }
    return D3D_OK;
}
H(dev_UpdateTexture) {
    UNUSED;
    Obj *s = get(a[1], K_TEXTURE), *d = get(a[2], K_TEXTURE);
    if (!s || !d) return D3DERR_INVALIDCALL;
    for (uint32_t l = 0; l < s->levels && l < d->levels; l++) {
        Obj *ss = &objs[s->level_surf[l]], *ds = &objs[d->level_surf[l]];
        copy_rect(ss, 0, 0, ss->width, ss->height, ds, 0, 0);
    }
    return D3D_OK;
}
H(dev_SetRenderTarget) {
    UNUSED;
    if (a[1]) {
        int i = any_index(a[1]);
        if (i >= 0) dev.render_target = i;
    }
    dev.zstencil = a[2] ? any_index(a[2]) : -1;
    return D3D_OK;
}
H(dev_GetRenderTarget) { UNUSED; out_obj(a[1], dev.render_target); return D3D_OK; }
H(dev_GetDepthStencilSurface) {
    UNUSED;
    out_obj(a[1], dev.zstencil);
    return dev.zstencil >= 0 ? D3D_OK : 0x88760870u /* D3DERR_NOTFOUND */;
}
H(dev_BeginScene) { UNUSED; dev.in_scene = 1; return D3D_OK; }
H(dev_EndScene) { UNUSED; dev.in_scene = 0; return D3D_OK; }
H(dev_Clear) { UNUSED; return D3D_OK; }
H(dev_SetTransform) {
    UNUSED;
    if (a[1] < 512) memcpy(dev.transform[a[1]], G(a[2]), 64);
    return D3D_OK;
}
H(dev_GetTransform) {
    UNUSED;
    if (a[1] < 512) memcpy(G(a[2]), dev.transform[a[1]], 64);
    return D3D_OK;
}
H(dev_SetViewport) { UNUSED; memcpy(dev.viewport, G(a[1]), 24); return D3D_OK; }
H(dev_GetViewport) { UNUSED; memcpy(G(a[1]), dev.viewport, 24); return D3D_OK; }
H(dev_SetMaterial) { UNUSED; memcpy(dev.material, G(a[1]), 68); return D3D_OK; }
H(dev_GetMaterial) { UNUSED; memcpy(G(a[1]), dev.material, 68); return D3D_OK; }
H(dev_SetLight) {
    UNUSED;
    if (a[1] < 16) memcpy(dev.light[a[1]], G(a[2]), 104);
    return D3D_OK;
}
H(dev_GetLight) {
    UNUSED;
    if (a[1] < 16) memcpy(G(a[2]), dev.light[a[1]], 104);
    return D3D_OK;
}
H(dev_LightEnable) {
    UNUSED;
    if (a[1] < 16) dev.light_on[a[1]] = a[2] != 0;
    return D3D_OK;
}
H(dev_GetLightEnable) {
    UNUSED;
    st32(a[2], a[1] < 16 ? dev.light_on[a[1]] : 0);
    return D3D_OK;
}
H(dev_SetClipPlane) {
    UNUSED;
    if (a[1] < 8) memcpy(dev.clip[a[1]], G(a[2]), 16);
    return D3D_OK;
}
H(dev_GetClipPlane) {
    UNUSED;
    if (a[1] < 8) memcpy(G(a[2]), dev.clip[a[1]], 16);
    return D3D_OK;
}
H(dev_SetRenderState) {
    UNUSED;
    if (a[1] < 256) dev.rs[a[1]] = a[2];
    return D3D_OK;
}
H(dev_GetRenderState) {
    UNUSED;
    st32(a[2], a[1] < 256 ? dev.rs[a[1]] : 0);
    return D3D_OK;
}
H(dev_EndStateBlock) { UNUSED; st32(a[1], dev.next_block++); return D3D_OK; }
H(dev_CreateStateBlock) { UNUSED; st32(a[2], dev.next_block++); return D3D_OK; }
H(dev_GetClipStatus) { UNUSED; memset(G(a[1]), 0, 8); return D3D_OK; }
H(dev_GetTexture) {
    UNUSED;
    out_obj(a[2], a[1] < 8 ? dev.texture[a[1]] : -1);
    return D3D_OK;
}
H(dev_SetTexture) {
    UNUSED;
    if (a[1] < 8) dev.texture[a[1]] = a[2] ? any_index(a[2]) : -1;
    return D3D_OK;
}
H(dev_GetTextureStageState) {
    UNUSED;
    st32(a[3], a[1] < 8 && a[2] < 32 ? dev.tss[a[1]][a[2]] : 0);
    return D3D_OK;
}
H(dev_SetTextureStageState) {
    UNUSED;
    if (a[1] < 8 && a[2] < 32) dev.tss[a[1]][a[2]] = a[3];
    return D3D_OK;
}
H(dev_ValidateDevice) { UNUSED; st32(a[1], 1); return D3D_OK; }
H(dev_GetInfo) { UNUSED; return 1; /* S_FALSE: no information */ }
H(dev_GetCurrentTexturePalette) { UNUSED; st32(a[1], 0); return D3D_OK; }
H(dev_Draw) { UNUSED; dev.draws++; return D3D_OK; }
H(dev_CreateVertexShader) {
    /* (this, pDeclaration, pFunction, pHandle, Usage) */
    UNUSED;
    if (a[2]) rt_log("d3d8: a vertex shader program was created; the null device ignores it\n");
    st32(a[3], dev.next_vshader++);
    return D3D_OK;
}
H(dev_SetVertexShader) { UNUSED; dev.vshader = a[1]; return D3D_OK; }
H(dev_GetVertexShader) { UNUSED; st32(a[1], dev.vshader); return D3D_OK; }
H(dev_SetStreamSource) {
    UNUSED;
    if (a[1] < 16) {
        dev.stream[a[1]] = a[2] ? any_index(a[2]) : -1;
        dev.stride[a[1]] = a[3];
    }
    return D3D_OK;
}
H(dev_GetStreamSource) {
    UNUSED;
    out_obj(a[2], a[1] < 16 ? dev.stream[a[1]] : -1);
    st32(a[3], a[1] < 16 ? dev.stride[a[1]] : 0);
    return D3D_OK;
}
H(dev_SetIndices) {
    UNUSED;
    dev.indices = a[1] ? any_index(a[1]) : -1;
    dev.base_vertex = a[2];
    return D3D_OK;
}
H(dev_GetIndices) {
    UNUSED;
    out_obj(a[1], dev.indices);
    st32(a[2], dev.base_vertex);
    return D3D_OK;
}
H(dev_CreatePixelShader) { UNUSED; st32(a[2], dev.next_pshader++); return D3D_OK; }
H(dev_SetPixelShader) { UNUSED; dev.pshader = a[1]; return D3D_OK; }
H(dev_GetPixelShader) { UNUSED; st32(a[1], dev.pshader); return D3D_OK; }

#define M(name, fn, n) {#name, fn, n}
static const ComMethod device_methods[] = {
    M(QueryInterface, d3d_QueryInterface, 3),
    M(AddRef, d3d_AddRef, 1),
    M(Release, d3d_Release, 1),
    M(TestCooperativeLevel, dev_TestCooperativeLevel, 1),
    M(GetAvailableTextureMem, dev_GetAvailableTextureMem, 1),
    M(ResourceManagerDiscardBytes, d3d_ok, 2),
    M(GetDirect3D, dev_GetDirect3D, 2),
    M(GetDeviceCaps, dev_GetDeviceCaps, 2),
    M(GetDisplayMode, dev_GetDisplayMode, 2),
    M(GetCreationParameters, dev_GetCreationParameters, 2),
    M(SetCursorProperties, d3d_ok, 4),
    M(SetCursorPosition, d3d_zero, 4),
    M(ShowCursor, dev_ShowCursor, 2),
    M(CreateAdditionalSwapChain, d3d_notavail, 3),
    M(Reset, dev_Reset, 2),
    M(Present, dev_Present, 5),
    M(GetBackBuffer, dev_GetBackBuffer, 4),
    M(GetRasterStatus, dev_GetRasterStatus, 2),
    M(SetGammaRamp, dev_SetGammaRamp, 3),
    M(GetGammaRamp, dev_GetGammaRamp, 2),
    M(CreateTexture, dev_CreateTexture, 8),
    M(CreateVolumeTexture, d3d_notavail, 9),
    M(CreateCubeTexture, d3d_notavail, 7),
    M(CreateVertexBuffer, dev_CreateVertexBuffer, 6),
    M(CreateIndexBuffer, dev_CreateIndexBuffer, 6),
    M(CreateRenderTarget, dev_CreateRenderTarget, 7),
    M(CreateDepthStencilSurface, dev_CreateDepthStencilSurface, 6),
    M(CreateImageSurface, dev_CreateImageSurface, 5),
    M(CopyRects, dev_CopyRects, 6),
    M(UpdateTexture, dev_UpdateTexture, 3),
    M(GetFrontBuffer, d3d_ok, 2),
    M(SetRenderTarget, dev_SetRenderTarget, 3),
    M(GetRenderTarget, dev_GetRenderTarget, 2),
    M(GetDepthStencilSurface, dev_GetDepthStencilSurface, 2),
    M(BeginScene, dev_BeginScene, 1),
    M(EndScene, dev_EndScene, 1),
    M(Clear, dev_Clear, 7),
    M(SetTransform, dev_SetTransform, 3),
    M(GetTransform, dev_GetTransform, 3),
    M(MultiplyTransform, d3d_ok, 3),
    M(SetViewport, dev_SetViewport, 2),
    M(GetViewport, dev_GetViewport, 2),
    M(SetMaterial, dev_SetMaterial, 2),
    M(GetMaterial, dev_GetMaterial, 2),
    M(SetLight, dev_SetLight, 3),
    M(GetLight, dev_GetLight, 3),
    M(LightEnable, dev_LightEnable, 3),
    M(GetLightEnable, dev_GetLightEnable, 3),
    M(SetClipPlane, dev_SetClipPlane, 3),
    M(GetClipPlane, dev_GetClipPlane, 3),
    M(SetRenderState, dev_SetRenderState, 3),
    M(GetRenderState, dev_GetRenderState, 3),
    M(BeginStateBlock, d3d_ok, 1),
    M(EndStateBlock, dev_EndStateBlock, 2),
    M(ApplyStateBlock, d3d_ok, 2),
    M(CaptureStateBlock, d3d_ok, 2),
    M(DeleteStateBlock, d3d_ok, 2),
    M(CreateStateBlock, dev_CreateStateBlock, 3),
    M(SetClipStatus, d3d_ok, 2),
    M(GetClipStatus, dev_GetClipStatus, 2),
    M(GetTexture, dev_GetTexture, 3),
    M(SetTexture, dev_SetTexture, 3),
    M(GetTextureStageState, dev_GetTextureStageState, 4),
    M(SetTextureStageState, dev_SetTextureStageState, 4),
    M(ValidateDevice, dev_ValidateDevice, 2),
    M(GetInfo, dev_GetInfo, 4),
    M(SetPaletteEntries, d3d_ok, 3),
    M(GetPaletteEntries, d3d_ok, 3),
    M(SetCurrentTexturePalette, d3d_ok, 2),
    M(GetCurrentTexturePalette, dev_GetCurrentTexturePalette, 2),
    M(DrawPrimitive, dev_Draw, 4),
    M(DrawIndexedPrimitive, dev_Draw, 6),
    M(DrawPrimitiveUP, dev_Draw, 5),
    M(DrawIndexedPrimitiveUP, dev_Draw, 9),
    M(ProcessVertices, d3d_ok, 6),
    M(CreateVertexShader, dev_CreateVertexShader, 5),
    M(SetVertexShader, dev_SetVertexShader, 2),
    M(GetVertexShader, dev_GetVertexShader, 2),
    M(DeleteVertexShader, d3d_ok, 2),
    M(SetVertexShaderConstant, d3d_ok, 4),
    M(GetVertexShaderConstant, d3d_ok, 4),
    M(GetVertexShaderDeclaration, d3d_notavail, 4),
    M(GetVertexShaderFunction, d3d_notavail, 4),
    M(SetStreamSource, dev_SetStreamSource, 4),
    M(GetStreamSource, dev_GetStreamSource, 4),
    M(SetIndices, dev_SetIndices, 3),
    M(GetIndices, dev_GetIndices, 3),
    M(CreatePixelShader, dev_CreatePixelShader, 3),
    M(SetPixelShader, dev_SetPixelShader, 2),
    M(GetPixelShader, dev_GetPixelShader, 2),
    M(DeletePixelShader, d3d_ok, 2),
    M(SetPixelShaderConstant, d3d_ok, 4),
    M(GetPixelShaderConstant, d3d_ok, 4),
    M(GetPixelShaderFunction, d3d_notavail, 4),
    M(DrawRectPatch, d3d_ok, 4),
    M(DrawTriPatch, d3d_ok, 4),
    M(DeletePatch, d3d_ok, 2),
};

/* --------------------------------------------- resources: shared methods */
H(res_GetDevice) { UNUSED; out_obj(a[1], dev.obj); return D3D_OK; }
H(res_SetPriority) {
    UNUSED;
    int i = any_index(a[0]);
    uint32_t old = i >= 0 ? objs[i].priority : 0;
    if (i >= 0) objs[i].priority = a[1];
    return old;
}
H(res_GetPriority) { UNUSED; int i = any_index(a[0]); return i >= 0 ? objs[i].priority : 0; }
H(res_GetType) {
    UNUSED;
    int i = any_index(a[0]);
    if (i < 0) return 0;
    switch (objs[i].kind) {
    case K_TEXTURE: return RT_TEXTURE;
    case K_VB: return RT_VERTEXBUFFER;
    case K_IB: return RT_INDEXBUFFER;
    default: return RT_SURFACE;
    }
}
H(res_GetPrivateData) { UNUSED; return 0x88760870u; /* D3DERR_NOTFOUND */ }

#define RESOURCE_METHODS                                                                                        \
    M(QueryInterface, d3d_QueryInterface, 3), M(AddRef, d3d_AddRef, 1), M(Release, d3d_Release, 1),             \
        M(GetDevice, res_GetDevice, 2), M(SetPrivateData, d3d_ok, 5), M(GetPrivateData, res_GetPrivateData, 4), \
        M(FreePrivateData, d3d_ok, 2), M(SetPriority, res_SetPriority, 2), M(GetPriority, res_GetPriority, 1),   \
        M(PreLoad, d3d_zero, 1), M(GetType, res_GetType, 1)

/* ---------------------------------------------------------- IDirect3DTexture8 */
static Obj *level(uint32_t tex, uint32_t l) {
    Obj *t = get(tex, K_TEXTURE);
    return t && l < t->levels ? &objs[t->level_surf[l]] : NULL;
}
H(tex_SetLOD) {
    UNUSED;
    Obj *t = get(a[0], K_TEXTURE);
    uint32_t old = t ? t->lod : 0;
    if (t) t->lod = a[1];
    return old;
}
H(tex_GetLOD) { UNUSED; Obj *t = get(a[0], K_TEXTURE); return t ? t->lod : 0; }
H(tex_GetLevelCount) { UNUSED; Obj *t = get(a[0], K_TEXTURE); return t ? t->levels : 0; }
H(tex_GetLevelDesc) {
    UNUSED;
    Obj *s = level(a[0], a[1]);
    if (!s) return D3DERR_INVALIDCALL;
    surface_desc(s, a[2], RT_SURFACE);
    return D3D_OK;
}
H(tex_GetSurfaceLevel) {
    UNUSED;
    Obj *t = get(a[0], K_TEXTURE);
    if (!t || a[1] >= t->levels) return D3DERR_INVALIDCALL;
    out_obj(a[2], t->level_surf[a[1]]);
    return D3D_OK;
}
H(tex_LockRect) {
    UNUSED;
    Obj *s = level(a[0], a[1]);
    if (!s) return D3DERR_INVALIDCALL;
    lock_rect(s, a[2], a[3]);
    return D3D_OK;
}
H(tex_UnlockRect) { UNUSED; return level(a[0], a[1]) ? D3D_OK : D3DERR_INVALIDCALL; }

static const ComMethod texture_methods[] = {
    RESOURCE_METHODS,
    M(SetLOD, tex_SetLOD, 2),
    M(GetLOD, tex_GetLOD, 1),
    M(GetLevelCount, tex_GetLevelCount, 1),
    M(GetLevelDesc, tex_GetLevelDesc, 3),
    M(GetSurfaceLevel, tex_GetSurfaceLevel, 3),
    M(LockRect, tex_LockRect, 5),
    M(UnlockRect, tex_UnlockRect, 2),
    M(AddDirtyRect, d3d_ok, 2),
};

/* ---------------------------------------------------------- IDirect3DSurface8 */
H(surf_GetContainer) {
    UNUSED;
    Obj *s = get(a[0], K_SURFACE);
    if (!s) return D3DERR_INVALIDCALL;
    out_obj(a[2], s->container >= 0 ? s->container : dev.obj);
    return D3D_OK;
}
H(surf_GetDesc) {
    UNUSED;
    Obj *s = get(a[0], K_SURFACE);
    if (!s) return D3DERR_INVALIDCALL;
    surface_desc(s, a[1], RT_SURFACE);
    return D3D_OK;
}
H(surf_LockRect) {
    UNUSED;
    Obj *s = get(a[0], K_SURFACE);
    if (!s) return D3DERR_INVALIDCALL;
    if (!s->data) {
        s->data = rt_heap_alloc(s->bytes ? s->bytes : 4, 1); /* a depth buffer locked after all */
        if (!s->data) return D3DERR_INVALIDCALL;
    }
    lock_rect(s, a[1], a[2]);
    return D3D_OK;
}

static const ComMethod surface_methods[] = {
    M(QueryInterface, d3d_QueryInterface, 3),
    M(AddRef, d3d_AddRef, 1),
    M(Release, d3d_Release, 1),
    M(GetDevice, res_GetDevice, 2),
    M(SetPrivateData, d3d_ok, 5),
    M(GetPrivateData, res_GetPrivateData, 4),
    M(FreePrivateData, d3d_ok, 2),
    M(GetContainer, surf_GetContainer, 3),
    M(GetDesc, surf_GetDesc, 2),
    M(LockRect, surf_LockRect, 4),
    M(UnlockRect, d3d_ok, 1),
};

/* -------------------------------------------- vertex and index buffers */
H(buf_Lock) {
    /* (this, OffsetToLock, SizeToLock, ppbData, Flags) */
    UNUSED;
    int i = any_index(a[0]);
    if (i < 0 || (objs[i].kind != K_VB && objs[i].kind != K_IB) || a[1] > objs[i].bytes) return D3DERR_INVALIDCALL;
    st32(a[3], objs[i].data + a[1]);
    return D3D_OK;
}
H(buf_GetDesc) {
    UNUSED;
    int i = any_index(a[0]);
    if (i < 0) return D3DERR_INVALIDCALL;
    Obj *b = &objs[i];
    st32(a[1], b->format);
    st32(a[1] + 4, b->kind == K_VB ? RT_VERTEXBUFFER : RT_INDEXBUFFER);
    st32(a[1] + 8, b->usage);
    st32(a[1] + 12, b->pool);
    st32(a[1] + 16, b->bytes);
    if (b->kind == K_VB) st32(a[1] + 20, b->fvf);
    return D3D_OK;
}

static const ComMethod vb_methods[] = {
    RESOURCE_METHODS,
    M(Lock, buf_Lock, 5),
    M(Unlock, d3d_ok, 1),
    M(GetDesc, buf_GetDesc, 2),
};
static const ComMethod ib_methods[] = {
    RESOURCE_METHODS,
    M(Lock, buf_Lock, 5),
    M(Unlock, d3d_ok, 1),
    M(GetDesc, buf_GetDesc, 2),
};
static const ComMethod swapchain_methods[] = {
    M(QueryInterface, d3d_QueryInterface, 3),
    M(AddRef, d3d_AddRef, 1),
    M(Release, d3d_Release, 1),
    M(Present, dev_Present, 5),
    M(GetBackBuffer, dev_GetBackBuffer, 4),
};

/* ------------------------------------------------------------ d3d8.dll */
H(d3d8_Direct3DCreate8) {
    UNUSED;
    if (!vt_d3d) {
        vt_d3d = rt_com_vtable("d3d8.dll", "IDirect3D8", d3d_methods, RT_COUNTOF(d3d_methods));
        vt_device = rt_com_vtable("d3d8.dll", "IDirect3DDevice8", device_methods, RT_COUNTOF(device_methods));
        vt_texture = rt_com_vtable("d3d8.dll", "IDirect3DTexture8", texture_methods, RT_COUNTOF(texture_methods));
        vt_surface = rt_com_vtable("d3d8.dll", "IDirect3DSurface8", surface_methods, RT_COUNTOF(surface_methods));
        vt_vb = rt_com_vtable("d3d8.dll", "IDirect3DVertexBuffer8", vb_methods, RT_COUNTOF(vb_methods));
        vt_ib = rt_com_vtable("d3d8.dll", "IDirect3DIndexBuffer8", ib_methods, RT_COUNTOF(ib_methods));
        vt_swapchain =
            rt_com_vtable("d3d8.dll", "IDirect3DSwapChain8", swapchain_methods, RT_COUNTOF(swapchain_methods));
    }
    if (a[0] != 220) rt_log("d3d8: Direct3DCreate8 with SDK version %u (expected 220)\n", a[0]);
    return objs[obj_new(K_D3D)].obj;
}

_Static_assert(RT_COUNTOF(device_methods) == 97, "IDirect3DDevice8 has 97 methods");
_Static_assert(RT_COUNTOF(d3d_methods) == 16, "IDirect3D8 has 16 methods");
_Static_assert(RT_COUNTOF(texture_methods) == 19, "IDirect3DTexture8 has 19 methods");
_Static_assert(RT_COUNTOF(vb_methods) == 14, "IDirect3DVertexBuffer8 has 14 methods");

static const ImportDef defs[] = {
    {"d3d8.dll", "Direct3DCreate8", d3d8_Direct3DCreate8, 1, 0},
};
RT_EXPORT_IMPORTS(rt_imports_d3d8, defs);

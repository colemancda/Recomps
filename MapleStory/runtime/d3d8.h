/* Direct3D 8: objects and device state shared by d3d8.c (the COM interfaces)
 * and d3d8gl.c (drawing with OpenGL ES 3). */
#ifndef RECOMP_D3D8_H
#define RECOMP_D3D8_H

#include "rt.h"

enum { K_FREE = 0, K_D3D, K_DEVICE, K_TEXTURE, K_SURFACE, K_VB, K_IB, K_SWAPCHAIN };

/* D3DFORMAT values used here. */
enum {
    FMT_R8G8B8 = 20, FMT_A8R8G8B8 = 21, FMT_X8R8G8B8 = 22, FMT_R5G6B5 = 23, FMT_X1R5G5B5 = 24,
    FMT_A1R5G5B5 = 25, FMT_A4R4G4B4 = 26, FMT_R3G3B2 = 27, FMT_A8 = 28, FMT_A8R3G3B2 = 29,
    FMT_X4R4G4B4 = 30, FMT_A8P8 = 40, FMT_P8 = 41, FMT_L8 = 50, FMT_A8L8 = 51, FMT_A4L4 = 52,
    FMT_D16_LOCKABLE = 70, FMT_D32 = 71, FMT_D15S1 = 73, FMT_D24S8 = 75, FMT_D24X8 = 77, FMT_D24X4S4 = 79,
    FMT_D16 = 80, FMT_VERTEXDATA = 100, FMT_INDEX16 = 101, FMT_INDEX32 = 102,
    FMT_DXT1 = 0x31545844, FMT_DXT2 = 0x32545844, FMT_DXT3 = 0x33545844, FMT_DXT4 = 0x34545844,
    FMT_DXT5 = 0x35545844,
};

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
    /* d3d8gl.c */
    unsigned gltex;              /* texture: GL name, 0 before the first upload */
    int dirty;                   /* texture: guest data changed since the upload */
    uint32_t gl_sampler_key;     /* texture: the sampler state last applied */
} Obj;

#define D3D_MAX_OBJS 8192
extern Obj d3d_objs[D3D_MAX_OBJS];

/* A vertex element: where an input comes from (stream, offset), its D3DVSDT type
 * and the D3DVSDE register it feeds. */
typedef struct VElem {
    uint8_t stream, type, reg;
    uint16_t offset;
} VElem;
enum { VT_FLOAT1 = 0, VT_FLOAT2, VT_FLOAT3, VT_FLOAT4, VT_D3DCOLOR, VT_UBYTE4, VT_SHORT2, VT_SHORT4 };
enum { VR_POSITION = 0, VR_BLENDWEIGHT, VR_BLENDINDICES, VR_NORMAL, VR_PSIZE, VR_DIFFUSE, VR_SPECULAR, VR_TEX0 };
typedef struct VDecl {
    int used;
    int n;
    VElem e[16];
} VDecl;
#define D3D_MAX_DECLS 64
#define D3D_DECL_BASE 0xF0000001u /* handle of the first declaration (above every FVF code) */

typedef struct D3DDev {
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
    VDecl decl[D3D_MAX_DECLS];
    uint32_t gamma[3 * 256 / 2]; /* D3DGAMMARAMP: 3 x 256 WORDs */
    int in_scene;
    unsigned draws, frames;
    uint16_t *black;
    int gl;                      /* d3d8gl.c draws */
} D3DDev;
extern D3DDev d3d_dev;

/* d3d8gl.c. Every function is a no-op when the backend is not running. */
int d3dgl_init(int w, int h);    /* on the rendering thread; 0 if OpenGL ES 3 is unavailable */
void d3dgl_resize(int w, int h);
/* Clear(count, rects, flags, color, z, stencil) */
void d3dgl_clear(uint32_t count, uint32_t rects, uint32_t flags, uint32_t color, float z, uint32_t stencil);
/* D3DPRIMITIVETYPE, then the vertex range used and the primitive count. */
void d3dgl_draw(uint32_t type, uint32_t start_vertex, uint32_t prim_count);
void d3dgl_draw_indexed(uint32_t type, uint32_t min_index, uint32_t num_vertices, uint32_t start_index,
                        uint32_t prim_count);
void d3dgl_draw_up(uint32_t type, uint32_t prim_count, uint32_t data, uint32_t stride);
void d3dgl_draw_indexed_up(uint32_t type, uint32_t min_index, uint32_t num_vertices, uint32_t prim_count,
                           uint32_t indices, uint32_t index_format, uint32_t data, uint32_t stride);
/* The finished frame as 16-bit RGB565, w x h, top row first; NULL without the backend. */
const uint16_t *d3dgl_present(int *w, int *h);
void d3dgl_texture_free(Obj *tex);

#endif

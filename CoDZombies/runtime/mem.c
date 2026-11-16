/* Guest memory: one 4 GB reservation, plus the guest heap allocator.
 *
 * The heap lives in [GUEST_HEAP_LO, GUEST_HEAP_HI). Blocks carry an 8-byte
 * header (size, magic) and are 8-byte aligned. Freed blocks go to size-class
 * free lists (multiples of 16 up to 1 KB, then powers of two); anything else is
 * carved from the top of the heap.
 */
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

#include "rt.h"

#ifdef RT_DYNAMIC_BASE
uint8_t *g_mem;
#endif

#define HDR 8u
#define MAGIC 0xb10cb10cu
#define NCLASS 85

static uint32_t heap_top = GUEST_HEAP_LO;
static uint32_t free_lists[NCLASS];  /* guest address of the first free block's header */

void mem_init(void) {
    size_t size = (1ull << 32) + 0x10000;  /* a spare page or so for accesses that wrap */
#ifdef RT_DYNAMIC_BASE
    void *p = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON | MAP_NORESERVE, -1, 0);
    if (p == MAP_FAILED) { perror("mmap guest memory"); exit(1); }
    g_mem = p;
#else
    void *want = (void *)(uintptr_t)RT_GUEST_BASE;
    void *p = mmap(want, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON | MAP_NORESERVE | MAP_FIXED, -1, 0);
    if (p != want) { perror("mmap guest memory at its fixed base"); exit(1); }
#endif
}

/* Free-list slot for a request: 16-byte classes up to 1 KB (slots 0..63), then
 * powers of two from 2 KB (slots 64..84). *rounded is the block size used. */
static uint32_t slot_for(uint32_t n, uint32_t *rounded) {
    if (n <= 1024) {
        *rounded = (n + 15) & ~15u;
        return *rounded / 16 - 1;
    }
    uint32_t r = 2048, k = 0;
    while (r < n && r < 0x80000000u) { r <<= 1; k++; }
    *rounded = r;
    return 64 + k;
}

uint32_t guest_alloc(uint32_t size) {
    if (size == 0) size = 1;
    uint32_t rounded;
    uint32_t s = slot_for(size, &rounded);
    uint32_t *prev = &free_lists[s];
    for (uint32_t b = *prev; b; b = ld32(b + 4)) {      /* free block: [size][next] */
        if (ld32(b) >= rounded) {
            *prev = ld32(b + 4);
            st32(b + 4, MAGIC);
            memset(G(b + HDR), 0, ld32(b));
            return b + HDR;
        }
        prev = (uint32_t *)G(b + 4);
    }
    uint32_t b = heap_top;
    if ((uint64_t)b + HDR + rounded > GUEST_HEAP_HI) {
        rt_log("guest heap exhausted allocating %u bytes", size);
        return 0;
    }
    heap_top = b + HDR + rounded;
    st32(b, rounded);
    st32(b + 4, MAGIC);
    return b + HDR;
}

uint32_t guest_alloc_size(uint32_t p) {
    if (!p) return 0;
    return ld32(p - HDR);
}

void guest_free(uint32_t p) {
    if (!p) return;
    uint32_t b = p - HDR;
    if (ld32(b + 4) != MAGIC) {
        rt_log("guest_free: %08x is not an allocated block", p);
        return;
    }
    uint32_t rounded;
    uint32_t s = slot_for(ld32(b), &rounded);
    st32(b + 4, free_lists[s]);
    free_lists[s] = b;
}

uint32_t guest_realloc(uint32_t p, uint32_t size) {
    if (!p) return guest_alloc(size);
    if (!size) { guest_free(p); return 0; }
    uint32_t old = guest_alloc_size(p);
    if (size <= old) return p;
    uint32_t q = guest_alloc(size);
    if (q) {
        memcpy(G(q), G(p), old);
        guest_free(p);
    }
    return q;
}

uint32_t guest_strdup(const char *s) {
    uint32_t n = (uint32_t)strlen(s) + 1;
    uint32_t p = guest_alloc(n);
    if (p) memcpy(G(p), s, n);
    return p;
}

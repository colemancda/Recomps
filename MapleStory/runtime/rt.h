/* Runtime-internal interface: guest memory layout, loader, dispatch, imports. */
#ifndef RECOMP_RT_H
#define RECOMP_RT_H

#include "cpu.h"

/* ---- tables emitted by the lifter (tables.c) ---- */
typedef struct RtFuncDesc {
    uint32_t addr;
    GuestFn fn;
    const char *name;
} RtFuncDesc;

/* One lifted image of a multi-image build (lift.py --module NAME). */
typedef struct RtImageDesc {
    const char *name;             /* file name, lower case */
    uint32_t base, pref_base, size; /* lifted (load) base, the PE's own base, mapped size */
    uint32_t text_base, text_size, text_crc;
    uint32_t entry;               /* DllMain or the EXE entry point */
    const RtFuncDesc *funcs;
    uint32_t nfuncs;
    const uint32_t *retsites;
    uint32_t nretsites;
    const uint32_t (*patches)[2];
    uint32_t npatches;
} RtImageDesc;

/* Every lifted image of the program; the EXE is rt_images[0]. Single-image
 * builds get this from tables.c, multi-image builds from the generated images.c. */
extern const RtImageDesc *const rt_images[];
extern const unsigned rt_nimages;

/* ---- guest address space layout (all inside one GUEST_SIZE mapping) ----
 * The full 4 GiB, so the DLLs sit at their own bases and every guest pointer
 * has its Windows value:
 *   0x00400000 aomx.exe          0x10000000 rockalldll.dll
 *   0x10100000 granny.dll (rebased from 0x10000000 at lift time)
 *   0x21100000 mss32.dll         0x26f00000 mp3dec.asi
 *   0x78000000 msvcrt.dll        0x780c0000 msvcp60.dll
 * The heap, the VirtualAlloc arena and the runtime's own areas fit around them. */
#define GUEST_SIZE      0x100000000ull /* 4 GiB */
#define GUEST_NULL_GUARD 0x00010000u
#define GUEST_SYS_BASE  0x00020000u /* PEB, TEB, command line, environment */
#define GUEST_SYS_SIZE  0x00010000u
#define GUEST_PEB       0x00020000u
#define GUEST_TEB       0x00021000u
#define GUEST_STR_BASE  0x00023000u /* bump area for process strings */
#define GUEST_STACK_LOW 0x00030000u
#define GUEST_STACK_TOP 0x00130000u /* 1 MiB main-thread stack, as on Windows XP */
#define GUEST_HEAP_BASE 0x01000000u /* up to the first DLL */
#define GUEST_HEAP_END  0x10000000u
#define GUEST_VA_BASE   0x30000000u /* VirtualAlloc arena, 1 GiB */
#define GUEST_VA_END    0x70000000u
#define GUEST_IMP_BASE  0x7FF00000u /* fake host-function entry points, 16 bytes apart */
#define GUEST_IMP_SIZE  0x00010000u
#define GUEST_EXIT_ADDR 0x7FFF0000u /* return address of the entry point */
#define GUEST_CB_RET    0x7FFF0010u /* return address of a host-to-guest call */
#define GUEST_THREAD_RET 0x7FFF0020u /* return address of a thread start routine */
#define GUEST_MAP_BASE  0x80000000u    /* memory-mapped files (.wz), demand-paged */
#define GUEST_MAP_END   0x100000000ull /* up to the top of the 4 GiB guest space */

#define TEB_LAST_ERROR  0x34u
#define TEB_TLS_SLOTS   0xE10u
#define TLS_SLOT_COUNT  64
#define TEB_SIZE        0x1000u

/* ---- memory (mem.c); the allocators are thread safe ---- */
uint32_t rt_wait_handles(CPU *c, uint32_t handles, uint32_t n, int wait_all, uint32_t ms);
int rt_input_pending(void);
void rt_dinput_notify(void); /* signal DirectInput device events when raw input arrives */
void rt_ll_kbd_queue(int vk, int scan, int down); /* queue a key for the WH_KEYBOARD_LL hook */
void rt_mem_init(void);
void rt_mem_commit(uint32_t addr, uint32_t size); /* make [addr, addr+size) read/write */
/* Map the first `size` bytes of the open file `fd` into the guest's file-mapping
 * arena, demand-paged, and return the guest address (0 on failure). `writable`
 * makes the view copy-on-write (changes stay private, never reach the file). */
uint32_t rt_map_file(int fd, uint32_t size, int writable);
uint32_t rt_sys_alloc(uint32_t size);              /* bump allocation in the system area */
uint32_t rt_sys_strdup(const char *s);

uint32_t rt_heap_alloc(uint32_t size, int zero);
void rt_heap_free(uint32_t p);
uint32_t rt_heap_realloc(uint32_t p, uint32_t size, int zero);
uint32_t rt_heap_size(uint32_t p);                 /* 0xFFFFFFFF if p is not a live block */

uint32_t rt_valloc(uint32_t addr, uint32_t size, uint32_t type, uint32_t protect);
int rt_vfree(uint32_t addr, uint32_t size, uint32_t type);
int rt_vquery(uint32_t addr, uint32_t *base, uint32_t *size); /* region containing addr; 0 if none */
int rt_guest_readable(uint32_t addr, uint32_t size);

/* ---- loader (loader.c) ---- */
typedef struct RtImage {
    const RtImageDesc *desc; /* NULL for a resource-only DLL mapped as data */
    char name[64];           /* file name, lower case */
    uint32_t base, size, entry, headers_size, stack_reserve;
    uint32_t rsrc_rva, rsrc_size; /* resource directory */
    uint32_t exp_rva, exp_size;   /* export directory */
    char path[512];      /* host path of the file */
    char guest_path[260]; /* what GetModuleFileNameA reports */
    int attached;        /* DllMain(PROCESS_ATTACH) has run */
    int refs;            /* LoadLibrary count; mapped images are never unmapped */
} RtImage;
extern RtImage g_image; /* the EXE */
#define RT_MAX_DLLS 16
extern RtImage g_dlls[RT_MAX_DLLS]; /* lifted DLLs, in load order */
extern unsigned g_ndlls;
void rt_load_pe(const char *path); /* the EXE, and the lifted DLLs it imports */
/* Map a lifted DLL (and the lifted DLLs it imports) by file name; NULL if no
 * lifted image has that name. Its DllMain runs at the next rt_dll_attach(). */
RtImage *rt_load_dll(const char *name);
/* Map a DLL from the game folder that has no exports (a resource-only language
 * DLL) as data: its resources become available, no code runs. NULL otherwise. */
RtImage *rt_load_data_dll(const char *name);
RtImage *rt_image_at(uint32_t base);            /* lifted DLL with that base (module handle), or NULL */
RtImage *rt_image_named(const char *name);      /* a mapped lifted DLL, by file name; NULL if not mapped */
/* Export of a mapped image by name, or by ordinal when name is NULL; 0 if absent.
 * Forwarders are followed into lifted DLLs or turned into host functions. */
uint32_t rt_image_export(RtImage *im, const char *name, uint32_t ordinal);
/* Run DllMain(PROCESS_ATTACH) for every mapped DLL that has not had it, in
 * load order. Returns 0 if one of them returned FALSE. */
int rt_dll_attach(CPU *c);
int rt_in_lifted_code(uint32_t addr); /* inside some image's code range */
const char *rt_module_name(uint32_t h); /* DLL name of a host module handle (misc.c), or NULL */
RtImage *rt_image_of(uint32_t addr);  /* the mapped EXE or DLL containing addr, or NULL */

/* ---- host functions: imports, COM methods, GetProcAddress results (dispatch.c) ----
 * Every host-implemented entry point has a fake guest address in the import area.
 * A handler receives its arguments in a[] and returns the EAX value; the
 * dispatcher pops the arguments (stdcall) unless is_cdecl is set. */
typedef uint32_t (*ImportFn)(CPU *c, const uint32_t *a);
/* Guest functions replaced by host handlers (lift.py --hook): called like imports. */
typedef struct RtHook {
    uint32_t addr;
    const char *name;
    ImportFn fn;
    int nargs; /* stdcall argument count; the handler's return value goes to EAX */
} RtHook;
extern const RtHook rt_hooks[];
extern const unsigned rt_nhooks;
typedef struct ImportDef {
    const char *dll, *name;
    ImportFn fn;
    int nargs;     /* argument count (dwords) */
    int is_cdecl;  /* caller pops (wsprintfA) */
} ImportDef;
typedef struct Import {
    const char *dll, *name;
    ImportFn fn;
    int nargs, is_cdecl;
    uint32_t iat_va;
    uint64_t calls;
} Import;
#define RT_MAX_IMPORTS (GUEST_IMP_SIZE / 16)
extern Import g_imports[RT_MAX_IMPORTS];
extern uint32_t g_nimports;
const ImportDef *rt_find_import_def(const char *dll, const char *name);
uint32_t rt_register_import(const char *dll, const char *name, uint32_t iat_va); /* returns the fake address */
/* Fake address for a host function that is not in the import table. fn == NULL
 * makes a stub that stops with "UNIMPLEMENTED dll!name" when called. Repeated
 * calls with the same dll and name return the same address. */
uint32_t rt_host_fn(const char *dll, const char *name, ImportFn fn, int nargs);
void rt_import_call(CPU *c, uint32_t idx);

/* Handler tables, one per runtime source file. */
typedef struct ImportTable {
    const ImportDef *defs;
    unsigned count;
} ImportTable;
#define RT_COUNTOF(a) (sizeof(a) / sizeof((a)[0]))
#define RT_EXPORT_IMPORTS(sym, defs) const ImportTable sym = {defs, RT_COUNTOF(defs)}
extern const ImportTable rt_imports_kernel, rt_imports_thread, rt_imports_seh, rt_imports_user, rt_imports_file,
    rt_imports_registry, rt_imports_misc, rt_imports_winsock, rt_imports_ddraw, rt_imports_dinput, rt_imports_dsound, rt_imports_maple, rt_imports_extra,
    rt_imports_d3d8, rt_imports_win32w,
    rt_imports_stubs, rt_imports_gdi, rt_imports_shlwapi;
void rt_gdi_set_font_dir(const char *dir); /* gdi.c: where the Liberation fonts are */

/* ---- UTF-16 strings and W forms (win32w.c); the host side is UTF-8 ---- */
uint32_t rt_wlen(uint32_t a);                               /* characters before the NUL */
int rt_resolve_path(const char *guest, char *out, size_t cap); /* file.c: host path to read a guest file */
uint32_t rt_resource(uint32_t hmod, uint32_t type, uint32_t id, uint32_t *size); /* file.c */
size_t rt_get_wstr(uint32_t a, char *out, size_t cap);      /* guest UTF-16 -> host UTF-8 */
uint32_t rt_put_wstr(uint32_t a, uint32_t cap, const char *s); /* host UTF-8 -> guest UTF-16, cap in chars incl. NUL; returns chars */
uint32_t rt_wstr_dup(const char *s);                        /* new UTF-16 copy on the guest heap */
/* Call the A handler dll!name with the UTF-16 arguments in wmask (bit i = a[i]) converted. */
uint32_t rt_call_a(CPU *c, const uint32_t *a, int nargs, const char *dll, const char *name, unsigned wmask);

#define H(name) static uint32_t name(CPU *c, const uint32_t *a)
#define UNUSED (void)c, (void)a

/* ---- host-to-guest calls (dispatch.c) ----
 * Calls the guest function fn with nargs dword arguments. Returns 1 when the
 * function returned normally, with its EAX in *result (if not NULL). Returns 0
 * when a non-local return (SEH unwind, C++ catch) passed through the call: the
 * guest stack was unwound past the host caller, which must return to the import
 * dispatcher at once without touching guest state. */
#define RT_STDCALL 0
#define RT_CDECL 1
int rt_call_guest(CPU *c, uint32_t fn, int conv, int nargs, const uint32_t *args, uint32_t *result);

/* ---- dispatch / diagnostics (dispatch.c) ---- */
extern int g_trace_imports;
CPU *rt_cpu(void);          /* the calling host thread's guest CPU (NULL outside guest threads) */
void rt_set_cpu(CPU *c);
void rt_build_ftab(void);
const char *rt_func_name(uint32_t addr, uint32_t *start);
void rt_dump_state(CPU *c);
void rt_backtrace(CPU *c);
void rt_fatal(CPU *c, int code, const char *fmt, ...) __attribute__((noreturn, format(printf, 3, 4)));
void rt_install_fault_handler(void); /* call once on every host thread that runs guest code */
void rt_print_import_stats(void);
void rt_audio_report(void);          /* dsound.c: how much was mixed, and finish the WAV file */
void rt_profile_start(void);         /* profile.c: --profile */
void rt_profile_write(void);
/* login.c: the 96-digit session token for --login; 0 (after a message) if it cannot be made */
int rt_login_token(const char *user, const char *password, const char *key_file, char token[97]);
void rt_exit(int code) __attribute__((noreturn)); /* leave the process from any thread */
void rt_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/* ---- threads, handles and waitable objects (thread.c) ---- */
typedef struct RtWaitable RtWaitable;
typedef struct RtThread {
    CPU cpu;
    uint32_t tid, teb, stack_low, stack_top;
    uint32_t start, param;
    RtWaitable *done; /* signalled when the thread has exited */
    void *msgq;       /* user32 message queue, created on demand (user32.c) */
    int is_main;
} RtThread;
static inline RtThread *rt_thread_of(CPU *c) { return (RtThread *)c->thread; }
RtThread *rt_thread_init_main(void);
void rt_teb_init(uint32_t teb, uint32_t stack_low, uint32_t stack_top, uint32_t tid);

enum { RT_H_FREE = 0, RT_H_WAITABLE, RT_H_FILE, RT_H_FIND, RT_H_OTHER };
uint32_t rt_handle_alloc(int type, void *obj, void (*close)(void *obj));
void *rt_handle_get(uint32_t h, int type); /* NULL when h is not a live handle of that type */
int rt_handle_close(uint32_t h);

enum { RT_W_EVENT = 1, RT_W_MUTEX, RT_W_THREAD, RT_W_PROCESS, RT_W_SEMAPHORE };
RtWaitable *rt_waitable_new(int kind, int manual_reset, int signaled);
void rt_waitable_signal(RtWaitable *w);
void rt_waitable_reset(RtWaitable *w);
void rt_waitable_ref(RtWaitable *w);
void rt_waitable_unref(void *w);
void rt_waitable_set_exit_code(RtWaitable *w, uint32_t code);

static inline void rt_set_last_error(CPU *c, uint32_t e) { st32(c->fs_base + TEB_LAST_ERROR, e); }
static inline const char *rt_gstr(uint32_t a) { return a ? (const char *)G(a) : "(null)"; }
uint64_t rt_mono_ns(void);        /* the guest's monotonic clock */
uint64_t rt_host_ns(void);
/* --guest-cpu: the guest's threads all run on one CPU (see thread.c). */
void rt_affinity_init(void);        /* once, on the main thread, before any other thread exists */
void rt_affinity_host(int enter);   /* 1: lift the confinement while host threads are started; 0: restore it */        /* real time, also on the virtual clock */
void rt_sleep_ns(uint64_t ns);    /* let guest time pass on the calling thread */

/* ---- process state shared by import handlers ---- */
typedef struct RtProcess {
    uint32_t cmdline_a;      /* guest address of the ANSI command line */
    uint32_t env_a, env_w;   /* guest environment blocks */
    uint32_t process_heap;
} RtProcess;
extern RtProcess g_proc;

/* ---- run-time options (main.c) ---- */
typedef struct RtOptions {
    char exedir[512];      /* directory of the original executable */
    char gamedir[512];     /* host directory holding the game's files */
    char savedir[512];     /* where the guest's writes go; an overlay when it differs from gamedir */
    char regfile[512];     /* registry backing file */
    int headless;          /* no window; frames can still be dumped */
    int dump_frames[64];   /* 1-based presented frames to dump */
    int ndump_frames;
    int dump_every;        /* also dump every Nth frame, 0 = no */
    char dump_path[512];
    int exit_after_frame;  /* stop after this many presented frames, 0 = never */
    int scale;             /* window size as a multiple of the guest screen, 0 = automatic */
    int window_w, window_h; /* window size in pixels, 0 = from scale */
    int fullscreen;        /* start as a borderless full-screen window */
    int scale_mode;        /* RT_SCALE_*: how the picture is fitted into the window */
    int filter;            /* RT_FILTER_*: how it is sampled */
    int dump_window;       /* frame dumps also write what the window shows, after scaling */
    int verbose;           /* log window messages and COM calls */
    char input_script[16384]; /* synthetic input, see video.c */
    int virtual_clock;     /* guest time is a counter advanced by the main thread */
    int trace_net;         /* log every datagram and stream chunk sent or received */
    int no_sound;          /* DirectSoundCreate8 reports that there is no sound device */
    char dump_audio[512];  /* WAV file the mixed sound is written to instead of a device */
    int pad;               /* game controllers are used (default) */
    int pad_virtual;       /* attach an SDL virtual controller for the input script to drive */
    int trace_input;       /* log controller, touch and key events on their way to the game */
    int guest_cpu;         /* CPU the guest's threads are confined to; -1 = not confined (thread.c) */
    char profile[512];     /* file the sampling profile is written to (profile.c) */
    char local_address[32]; /* IPv4 address sockets are bound to, "" = any (winsock.c) */
} RtOptions;
extern RtOptions g_opt;
enum { RT_SCALE_FIT = 0, RT_SCALE_INTEGER, RT_SCALE_STRETCH };
enum { RT_FILTER_SHARP = 0, RT_FILTER_NEAREST, RT_FILTER_LINEAR };

#endif

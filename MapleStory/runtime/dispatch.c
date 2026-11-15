/* Indirect call dispatch, host-function dispatch, host-to-guest calls and diagnostics. */
#define _GNU_SOURCE
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <strings.h>
#include <unistd.h>

#include "rt.h"

GuestFn *g_fpage[1u << 20];
Import g_imports[RT_MAX_IMPORTS];
uint32_t g_nimports;
int g_trace_imports;
RtOptions g_opt;

static __thread CPU *t_cpu;
static pthread_mutex_t imports_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t log_mu = PTHREAD_MUTEX_INITIALIZER;

CPU *rt_cpu(void) { return t_cpu; }
void rt_set_cpu(CPU *c) { t_cpu = c; }

void rt_build_ftab(void) {
    for (unsigned m = 0; m < rt_nimages; m++) {
        const RtImageDesc *d = rt_images[m];
        for (uint32_t i = 0; i < d->nfuncs; i++) {
            uint32_t a = d->funcs[i].addr;
            GuestFn **pg = &g_fpage[a >> 12];
            if (!*pg && !(*pg = calloc(4096, sizeof(GuestFn)))) {
                perror("recomp: function table");
                exit(2);
            }
            (*pg)[a & 0xFFFu] = d->funcs[i].fn;
        }
    }
}

static const RtImageDesc *image_of(uint32_t addr) {
    for (unsigned m = 0; m < rt_nimages; m++)
        if (addr - rt_images[m]->base < rt_images[m]->size) return rt_images[m];
    return NULL;
}

int rt_in_lifted_code(uint32_t addr) {
    const RtImageDesc *d = image_of(addr);
    return d && addr - d->text_base < d->text_size;
}

void rt_log(const char *fmt, ...) {
    va_list ap;
    pthread_mutex_lock(&log_mu);
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    pthread_mutex_unlock(&log_mu);
}

void rt_exit(int code) {
    rt_profile_write();
    fflush(stdout);
    fflush(stderr);
    /* Other guest threads may be anywhere, including inside host libraries: do not
     * run atexit handlers or destructors under them. */
    _exit(code);
}

/* -------------------------------------------------------- host functions */
static const ImportTable *const import_tables[] = {
    &rt_imports_kernel,
    &rt_imports_thread,
    &rt_imports_seh,
    &rt_imports_user,
    &rt_imports_winsock,
    &rt_imports_file,
    &rt_imports_registry,
    &rt_imports_misc,
    &rt_imports_maple,
    &rt_imports_d3d8,
    &rt_imports_win32w,
    &rt_imports_stubs,
    &rt_imports_gdi,
    &rt_imports_shlwapi,
    &rt_imports_ddraw,
    &rt_imports_dinput,
    &rt_imports_dsound,
    &rt_imports_extra,
};

/* Winsock is imported by ordinal; these are ws2_32's (and wsock32's) fixed numbers. */
static const char *winsock_ordinal(unsigned n) {
    static const struct { unsigned n; const char *name; } ords[] = {
        {1, "accept"}, {2, "bind"}, {3, "closesocket"}, {4, "connect"}, {5, "getpeername"},
        {6, "getsockname"}, {7, "getsockopt"}, {8, "htonl"}, {9, "htons"}, {10, "ioctlsocket"},
        {11, "inet_addr"}, {12, "inet_ntoa"}, {13, "listen"}, {14, "ntohl"}, {15, "ntohs"},
        {16, "recv"}, {17, "recvfrom"}, {18, "select"}, {19, "send"}, {20, "sendto"},
        {21, "setsockopt"}, {22, "shutdown"}, {23, "socket"}, {51, "gethostbyaddr"},
        {52, "gethostbyname"}, {53, "getprotobyname"}, {54, "getprotobynumber"}, {55, "getservbyname"},
        {56, "getservbyport"}, {57, "gethostname"}, {101, "WSAAsyncSelect"},
        {102, "WSAAsyncGetHostByAddr"}, {103, "WSAAsyncGetHostByName"}, {108, "WSACancelAsyncRequest"},
        {111, "WSAGetLastError"}, {112, "WSASetLastError"}, {113, "WSACancelBlockingCall"},
        {114, "WSAIsBlocking"}, {115, "WSAStartup"}, {116, "WSACleanup"}, {151, "__WSAFDIsSet"},
    };
    for (size_t i = 0; i < RT_COUNTOF(ords); i++)
        if (ords[i].n == n) return ords[i].name;
    return NULL;
}

const ImportDef *rt_find_import_def(const char *dll, const char *name) {
    if (name[0] == '#' && (!strcasecmp(dll, "ws2_32.dll") || !strcasecmp(dll, "wsock32.dll"))) {
        const char *named = winsock_ordinal((unsigned)atoi(name + 1));
        if (named) name = named;
        dll = "ws2_32.dll";
    }
    for (size_t t = 0; t < RT_COUNTOF(import_tables); t++) {
        const ImportDef *defs = import_tables[t]->defs;
        for (unsigned i = 0; i < import_tables[t]->count; i++)
            if (!strcasecmp(defs[i].dll, dll) && !strcmp(defs[i].name, name)) return &defs[i];
    }
    return NULL;
}

static uint32_t import_add(const char *dll, const char *name, ImportFn fn, int nargs, int is_cdecl,
                           uint32_t iat_va) {
    if (g_nimports == RT_MAX_IMPORTS) {
        fprintf(stderr, "recomp: too many host functions\n");
        exit(2);
    }
    Import *im = &g_imports[g_nimports];
    memset(im, 0, sizeof *im);
    im->dll = strdup(dll);
    im->name = strdup(name);
    im->iat_va = iat_va;
    im->fn = fn;
    im->nargs = nargs;
    im->is_cdecl = is_cdecl;
    return GUEST_IMP_BASE + 16 * g_nimports++;
}

uint32_t rt_register_import(const char *dll, const char *name, uint32_t iat_va) {
    const ImportDef *def = rt_find_import_def(dll, name);
    pthread_mutex_lock(&imports_mu);
    uint32_t a = import_add(dll, name, def ? def->fn : NULL, def ? def->nargs : 0, def ? def->is_cdecl : 0, iat_va);
    pthread_mutex_unlock(&imports_mu);
    return a;
}

/* A host entry point for dll!name. Its calling convention comes from the import
 * table: a cdecl handler (wsprintf) must leave the arguments to the caller. */
uint32_t rt_host_fn(const char *dll, const char *name, ImportFn fn, int nargs) {
    const ImportDef *def = fn ? rt_find_import_def(dll, name) : NULL;
    int is_cdecl = def && def->fn == fn ? def->is_cdecl : 0;
    pthread_mutex_lock(&imports_mu);
    uint32_t a = 0;
    for (uint32_t i = 0; i < g_nimports; i++) {
        if (!g_imports[i].iat_va && !strcmp(g_imports[i].name, name) && !strcasecmp(g_imports[i].dll, dll)) {
            a = GUEST_IMP_BASE + 16 * i;
            break;
        }
    }
    if (!a) a = import_add(dll, name, fn, nargs, is_cdecl, 0);
    pthread_mutex_unlock(&imports_mu);
    return a;
}

void rt_import_call(CPU *c, uint32_t idx) {
    Import *im = &g_imports[idx];
    uint32_t sp = ESP;
    uint32_t ra = ld32(sp);
    __atomic_fetch_add(&im->calls, 1, __ATOMIC_RELAXED);
    if (!im->fn) {
        fflush(stdout);
        rt_log("\nUNIMPLEMENTED import %s!%s\n"
               "  called from 0x%08x; first stack arguments: %08x %08x %08x %08x %08x %08x\n",
               im->dll, im->name, ra, ld32(sp + 4), ld32(sp + 8), ld32(sp + 12), ld32(sp + 16), ld32(sp + 20),
               ld32(sp + 24));
        rt_backtrace(c);
        rt_print_import_stats();
        rt_exit(3);
    }
    uint32_t args[16];
    int n = im->nargs > 16 ? 16 : im->nargs;
    for (int i = 0; i < n; i++) args[i] = ld32(sp + 4 + 4 * (uint32_t)i);
    if (g_trace_imports) {
        char buf[512];
        int o = snprintf(buf, sizeof buf, "[import %x] %s!%s(", c->thread_id, im->dll, im->name);
        for (int i = 0; i < n && o < (int)sizeof buf; i++)
            o += snprintf(buf + o, sizeof buf - (size_t)o, i ? ", %#x" : "%#x", args[i]);
        rt_log("%s) from %08x\n", buf, ra);
    }
    uint32_t ret = im->fn(c, args);
    if (c->eip == RT_NL_PENDING) {
        /* A guest callback made by the handler unwound the stack past this call. */
        if (g_trace_imports) rt_log("[import %x] %s!%s left by a non-local return\n", c->thread_id, im->dll, im->name);
        return;
    }
    if (g_trace_imports) rt_log("[import %x] %s!%s -> %#x\n", c->thread_id, im->dll, im->name, ret);
    EAX = ret;
    ESP = sp + 4 + (im->is_cdecl ? 0 : 4 * (uint32_t)im->nargs);
    c->ret_sp = sp;
    c->eip = ra;
}

void rt_print_import_stats(void) {
    rt_audio_report();
    unsigned used = 0, impl = 0, total = 0;
    for (uint32_t i = 0; i < g_nimports; i++) {
        if (!g_imports[i].iat_va) continue;
        total++;
        if (g_imports[i].fn) impl++;
        if (g_imports[i].calls) used++;
    }
    pthread_mutex_lock(&log_mu);
    fprintf(stderr, "imports: %u in the image, %u with handlers, %u distinct called so far:\n ", total, impl, used);
    for (uint32_t i = 0; i < g_nimports; i++)
        if (g_imports[i].calls && g_imports[i].iat_va)
            fprintf(stderr, " %s%s(%llu)", g_imports[i].fn ? "" : "!", g_imports[i].name,
                    (unsigned long long)g_imports[i].calls);
    fprintf(stderr, "\nother host functions called:\n ");
    for (uint32_t i = 0; i < g_nimports; i++)
        if (g_imports[i].calls && !g_imports[i].iat_va)
            fprintf(stderr, " %s%s(%llu)", g_imports[i].fn ? "" : "!", g_imports[i].name,
                    (unsigned long long)g_imports[i].calls);
    fprintf(stderr, "\n");
    pthread_mutex_unlock(&log_mu);
}

static int import_index(uint32_t addr) {
    uint32_t off = addr - GUEST_IMP_BASE;
    if (off < 16 * g_nimports && (off & 15) == 0) return (int)(off / 16);
    return -1;
}

void rt_hook(CPU *c, uint32_t addr) {
    for (unsigned i = 0; i < rt_nhooks; i++) {
        if (rt_hooks[i].addr != addr) continue;
        uint32_t a = rt_host_fn("hook", rt_hooks[i].name, rt_hooks[i].fn, rt_hooks[i].nargs);
        rt_import_call(c, (a - GUEST_IMP_BASE) / 16);
        return;
    }
    rt_fatal(c, 4, "function 0x%08x was lifted as a hook, but the runtime has no handler for it", addr);
}

void rt_call_slow(CPU *c, uint32_t addr) {
    int idx = import_index(addr);
    if (idx >= 0) {
        rt_import_call(c, (uint32_t)idx);
        return;
    }
    /* Hook: a later milestone falls back to an interpreter for code the lifter did not see. */
    rt_fatal(c, 4, "indirect call to 0x%08x, which is not a lifted function (return address 0x%08x)", addr,
             ld32(ESP));
}

void rt_jmp_slow(CPU *c, uint32_t addr, uint32_t from) {
    int idx = import_index(addr);
    if (idx >= 0) {
        rt_import_call(c, (uint32_t)idx);
        return;
    }
    rt_fatal(c, 4, "indirect jump from 0x%08x to 0x%08x, which is not a lifted function entry", from, addr);
}

/* See RT_RETCHECK in cpu.h. `expected` and `slot` describe the calling frame: the
 * return address it pushed and where. Returns 1 if that frame should resume. */
int rt_ret_mismatch(CPU *c, uint32_t expected, uint32_t slot) {
    if (c->eip != RT_NL_PENDING) {
        c->nl_eip = c->eip;
        c->nl_slot = c->ret_sp;
        c->eip = RT_NL_PENDING;
        if (g_trace_imports)
            rt_log("[unwind %x] non-local return to %08x (slot %08x), first seen by the call site expecting %08x\n",
                   c->thread_id, c->nl_eip, c->nl_slot, expected);
    }
    if (c->nl_eip == expected && c->nl_slot == slot) {
        c->eip = expected;
        return 1;
    }
    return 0;
}

int rt_call_guest(CPU *c, uint32_t fn, int conv, int nargs, const uint32_t *args, uint32_t *result) {
    uint32_t sp0 = ESP;
    for (int i = nargs - 1; i >= 0; i--) PUSH32(args[i]);
    PUSH32(GUEST_CB_RET);
    uint32_t slot = ESP;
    rt_call(c, fn);
    /* Host-to-guest calls nest, and all of them use the same return address, so
     * the slot is compared even when the address matches. */
    if ((c->eip != GUEST_CB_RET || c->ret_sp != slot) && !rt_ret_mismatch(c, GUEST_CB_RET, slot)) return 0;
    if (conv == RT_CDECL) ESP += 4 * (uint32_t)nargs;
    if (ESP != sp0) {
        uint32_t start = 0;
        const char *name = rt_func_name(fn, &start);
        rt_fatal(c, 4, "guest callback 0x%08x (%s) returned with esp=%08x, expected %08x: wrong argument count "
                 "or calling convention in the host caller", fn, name ? name : "?", ESP, sp0);
    }
    if (result) *result = EAX;
    return 1;
}

void rt_unimplemented_insn(CPU *c, uint32_t addr, const char *mnemonic) {
    rt_fatal(c, 4, "UNIMPLEMENTED instruction `%s` at 0x%08x", mnemonic, addr);
}

void rt_bad_code(CPU *c, uint32_t from, uint32_t target) {
    rt_fatal(c, 4, "control reached 0x%08x (from 0x%08x), which the lifter could not decode", target, from);
}

void rt_divide_error(CPU *c, uint32_t addr) {
    rt_fatal(c, 4, "guest divide error (#DE) at 0x%08x; hardware exceptions are not dispatched to the guest", addr);
}

/* --------------------------------------------------------- diagnostics */
const char *rt_func_name(uint32_t addr, uint32_t *start) {
    const RtImageDesc *d = image_of(addr);
    if (!d || !d->nfuncs || addr < d->funcs[0].addr || addr - d->text_base >= d->text_size) return NULL;
    uint32_t lo = 0, hi = d->nfuncs;
    while (hi - lo > 1) {
        uint32_t mid = (lo + hi) / 2;
        if (d->funcs[mid].addr <= addr) lo = mid;
        else hi = mid;
    }
    if (start) *start = d->funcs[lo].addr;
    return d->funcs[lo].name;
}

static int is_retsite(uint32_t v) {
    const RtImageDesc *d = image_of(v);
    if (!d) return 0;
    uint32_t lo = 0, hi = d->nretsites;
    while (lo < hi) {
        uint32_t mid = (lo + hi) / 2;
        if (d->retsites[mid] == v) return 1;
        if (d->retsites[mid] < v) lo = mid + 1;
        else hi = mid;
    }
    return 0;
}

static void print_code_addr(uint32_t a) {
    uint32_t start = 0;
    const char *name = rt_func_name(a, &start);
    if (name) fprintf(stderr, "0x%08x  %s+0x%x", a, name, a - start);
    else fprintf(stderr, "0x%08x", a);
}

void rt_dump_state(CPU *c) {
    fprintf(stderr, "  eax=%08x ebx=%08x ecx=%08x edx=%08x esi=%08x edi=%08x\n", EAX, EBX, ECX, EDX, ESI, EDI);
    fprintf(stderr, "  esp=%08x ebp=%08x eflags=%08x fs=%08x x87: top=%u cw=%04x sw=%04x thread=%x\n", ESP, EBP,
            rt_get_eflags(c), c->fs_base, c->ftop, c->fcw, rt_fnstsw(c), c->thread_id);
}

/* Guest call stack: every stack slot holding an address that follows a call
 * instruction is reported; slots that are also on the EBP frame chain are
 * marked with '*'. Stale return addresses can appear; frames are never missed. */
void rt_backtrace(CPU *c) {
    uint32_t lo = GUEST_STACK_LOW, top = GUEST_STACK_TOP;
    RtThread *th = rt_thread_of(c);
    if (th) {
        lo = th->stack_low;
        top = th->stack_top;
    }
    uint32_t chain[64];
    unsigned nchain = 0;
    uint32_t bp = EBP;
    while (nchain < 64 && bp >= ESP && bp + 8 <= top && (bp & 3) == 0) {
        chain[nchain++] = bp + 4;
        uint32_t next = ld32(bp);
        if (next <= bp) break;
        bp = next;
    }
    pthread_mutex_lock(&log_mu);
    fprintf(stderr, "guest state:\n");
    rt_dump_state(c);
    fprintf(stderr, "guest call stack (most recent first):\n");
    if (ESP < lo || ESP >= top) {
        fprintf(stderr, "  (esp is outside the guest stack)\n");
        pthread_mutex_unlock(&log_mu);
        return;
    }
    unsigned shown = 0;
    for (uint32_t sp = ESP & ~3u; sp + 4 <= top && shown < 48; sp += 4) {
        uint32_t v = ld32(sp);
        if (!is_retsite(v)) continue;
        int on_chain = 0;
        for (unsigned i = 0; i < nchain; i++) on_chain |= chain[i] == sp;
        fprintf(stderr, "  %c#%-2u [%08x] ", on_chain ? '*' : ' ', shown, sp);
        print_code_addr(v);
        fprintf(stderr, "\n");
        shown++;
    }
    if (!shown) fprintf(stderr, "  (no return addresses found)\n");
    pthread_mutex_unlock(&log_mu);
}

void rt_fatal(CPU *c, int code, const char *fmt, ...) {
    va_list ap;
    fflush(stdout);
    pthread_mutex_lock(&log_mu);
    fprintf(stderr, "\nrecomp: fatal: ");
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fprintf(stderr, "\n");
    pthread_mutex_unlock(&log_mu);
    if (c) rt_backtrace(c);
    rt_exit(code);
}

static void fault_handler(int sig, siginfo_t *si, void *ctx) {
    (void)ctx;
    static volatile sig_atomic_t busy;
    if (busy) _exit(5);
    busy = 1;
    uintptr_t host = (uintptr_t)si->si_addr;
    uintptr_t base = (uintptr_t)G(0);
    fprintf(stderr, "\nrecomp: fatal: host signal %d (%s)", sig,
            sig == SIGSEGV ? "SIGSEGV" : sig == SIGBUS ? "SIGBUS" : "SIGFPE");
    if (sig != SIGFPE) {
        if (host >= base && host - base < GUEST_SIZE)
            fprintf(stderr, ": guest access to 0x%08x", (uint32_t)(host - base));
        else
            fprintf(stderr, ": host address %p (not a guest access; runtime bug or host stack overflow)", si->si_addr);
    }
    fprintf(stderr, "\n");
    if (t_cpu) rt_backtrace(t_cpu);
    _exit(5);
}

void rt_install_fault_handler(void) {
    static int installed;
    /* Each host thread gets its own alternate stack so that a host stack overflow is reported. */
    stack_t ss = {.ss_sp = malloc(1 << 16), .ss_size = 1 << 16, .ss_flags = 0};
    sigaltstack(&ss, NULL);
    if (__atomic_exchange_n(&installed, 1, __ATOMIC_SEQ_CST)) return;
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = fault_handler;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGBUS, &sa, NULL);
    sigaction(SIGFPE, &sa, NULL);
}

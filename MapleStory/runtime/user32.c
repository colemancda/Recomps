/* user32, gdi32 and imm32: window classes, windows, message queues, timers and input.
 *
 * Windows are bookkeeping only; nothing is drawn through them. The game's screen
 * reaches the host through DirectDraw (ddraw.c) and video.c. Each guest thread
 * has its own message queue. Input arrives from video.c on the thread that
 * opened the host window and is posted to the main window or the focus window.
 * A built-in EDIT class keeps the text the game types into its shared input
 * control; STATIC exists because the network engine creates hidden STATIC
 * windows to receive notifications.
 */
#define _GNU_SOURCE
#include <ctype.h>
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <strings.h>
#include <time.h>

#include "rt.h"
#include "user.h"
#include "video.h"

#define WM_CREATE 0x0001
#define WM_DESTROY 0x0002
#define WM_MOVE 0x0003
#define WM_SIZE 0x0005
#define WM_ACTIVATE 0x0006
#define WM_SETFOCUS 0x0007
#define WM_KILLFOCUS 0x0008
#define WM_SETTEXT 0x000C
#define WM_GETTEXT 0x000D
#define WM_GETTEXTLENGTH 0x000E
#define WM_PAINT 0x000F
#define WM_CLOSE 0x0010
#define WM_QUIT 0x0012
#define WM_ERASEBKGND 0x0014
#define WM_SHOWWINDOW 0x0018
#define WM_ACTIVATEAPP 0x001C
#define WM_NCCREATE 0x0081
#define WM_NCDESTROY 0x0082
#define WM_KEYDOWN 0x0100
#define WM_KEYUP 0x0101
#define WM_CHAR 0x0102
#define WM_SYSKEYDOWN 0x0104
#define WM_SYSKEYUP 0x0105
#define WM_SYSCOMMAND 0x0112
#define WM_TIMER 0x0113
#define WM_MOUSEMOVE 0x0200
#define WM_LBUTTONDOWN 0x0201
#define WM_LBUTTONUP 0x0202
#define WM_LBUTTONDBLCLK 0x0203
#define WM_RBUTTONDOWN 0x0204
#define WM_RBUTTONUP 0x0205
#define WM_RBUTTONDBLCLK 0x0206
#define WM_MBUTTONDOWN 0x0207
#define WM_MBUTTONUP 0x0208
#define EM_GETSEL 0x00B0
#define EM_SETSEL 0x00B1
#define EM_LIMITTEXT 0x00C5
#define SC_CLOSE 0xF060
#define CS_DBLCLKS 0x0008
#define WS_CHILD 0x40000000u
#define WS_VISIBLE 0x10000000u
#define GWL_WNDPROC (-4)
#define GWL_HINSTANCE (-6)
#define GWL_HWNDPARENT (-8)
#define GWL_ID (-12)
#define GWL_STYLE (-16)
#define GWL_EXSTYLE (-20)
#define GWL_USERDATA (-21)
#define PM_REMOVE 1

/* The guest's screen: the display mode the game sets with ChangeDisplaySettings. */
static int screen_w = 640, screen_h = 480, screen_bpp = 16;
#define SCREEN_W screen_w
#define SCREEN_H screen_h
#define HWND_BASE 0x00010100u
#define HDC_FAKE 0x0000DC00u

typedef struct WndClass {
    char *name;
    uint32_t wndproc, style, cls_extra, wnd_extra, hinstance, hcursor;
    struct WndClass *next;
} WndClass;

typedef struct Window {
    uint32_t hwnd;
    WndClass *cls;
    RtThread *owner;
    uint32_t wndproc, style, exstyle, parent, id, hinstance, userdata;
    int x, y, w, h;
    int visible, destroying;
    char *text;
    uint32_t text_limit, sel_start, sel_end; /* EDIT */
    uint8_t extra[64];
} Window;

typedef struct SendCtx {
    int done;
    uint32_t result;
} SendCtx;

typedef struct QMsg {
    uint32_t hwnd, msg, wparam, lparam, time;
    int32_t x, y;
    SendCtx *send; /* non-NULL: a cross-thread SendMessage waiting for the result */
} QMsg;

typedef struct MsgQueue {
    QMsg *buf;
    size_t count, cap;
    int quit;
    uint32_t quit_code;
} MsgQueue;

typedef struct Timer {
    uint32_t hwnd, id, proc, period_ms;
    uint64_t due_ns;
    RtThread *owner;
    int used;
} Timer;

/* One lock for all of user32's tables and queues; never held across a call into the guest. */
static pthread_mutex_t user_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t user_cv;
static pthread_once_t user_once = PTHREAD_ONCE_INIT;
static WndClass *classes;
static Window **windows;
static uint32_t nwindows;
static Timer timers[64];
static uint32_t next_timer_id = 0x7F00;
static uint32_t main_hwnd, focus_hwnd, active_hwnd;
static int cursor_count;
static uint32_t current_cursor = 0x00010000u | 12u; /* IDC_ARROW */
static uint32_t def_proc_addr, edit_proc_addr, static_proc_addr;

static uint32_t now_ms(void) { return (uint32_t)(rt_mono_ns() / 1000000ull); }

static uint32_t user_DefWindowProcA(CPU *c, const uint32_t *a);
static uint32_t edit_wndproc(CPU *c, const uint32_t *a);

static void add_class(const char *name, uint32_t proc, uint32_t style, uint32_t extra, uint32_t hinst) {
    WndClass *k = calloc(1, sizeof *k);
    k->name = strdup(name);
    k->wndproc = proc;
    k->style = style;
    k->wnd_extra = extra;
    k->hinstance = hinst;
    k->next = classes;
    classes = k;
}

static void user_init(void) {
    pthread_condattr_t at;
    pthread_condattr_init(&at);
    pthread_condattr_setclock(&at, CLOCK_MONOTONIC);
    pthread_cond_init(&user_cv, &at);
    def_proc_addr = rt_host_fn("user32.dll", "DefWindowProcA", user_DefWindowProcA, 4);
    edit_proc_addr = rt_host_fn("user32.dll", "<EDIT window procedure>", edit_wndproc, 4);
    static_proc_addr = def_proc_addr;
    add_class("EDIT", edit_proc_addr, 0, 0, 0);
    add_class("STATIC", static_proc_addr, 0, 0, 0);
}

static WndClass *find_class(const char *name) {
    for (WndClass *k = classes; k; k = k->next)
        if (!strcasecmp(k->name, name)) return k;
    return NULL;
}

/* Under user_mu. */
static Window *win_get(uint32_t hwnd) {
    uint32_t i = (hwnd - HWND_BASE) / 4;
    if (hwnd < HWND_BASE || (hwnd & 3) || i >= nwindows) return NULL;
    return windows[i];
}

static MsgQueue *queue_of(RtThread *th) {
    if (!th->msgq) th->msgq = calloc(1, sizeof(MsgQueue));
    return th->msgq;
}

/* Under user_mu. */
static void queue_push(RtThread *th, const QMsg *m) {
    MsgQueue *q = queue_of(th);
    if (q->count == q->cap) {
        q->cap = q->cap ? q->cap * 2 : 64;
        q->buf = realloc(q->buf, q->cap * sizeof *q->buf);
    }
    q->buf[q->count++] = *m;
    pthread_cond_broadcast(&user_cv);
}

int rt_post_message(uint32_t hwnd, uint32_t msg, uint32_t wparam, uint32_t lparam) {
    pthread_once(&user_once, user_init);
    int ok = 0;
    pthread_mutex_lock(&user_mu);
    Window *w = win_get(hwnd);
    if (w && !w->destroying) {
        QMsg m = {hwnd, msg, wparam, lparam, now_ms(), rt_mouse_x, rt_mouse_y, NULL};
        queue_push(w->owner, &m);
        ok = 1;
    }
    pthread_mutex_unlock(&user_mu);
    return ok;
}

/* Whether the calling thread has queued (posted/sent) window messages waiting.
 * MsgWaitForMultipleObjects uses this so a posted message (e.g. a WSAAsyncSelect
 * FD_READ socket notification) wakes the main loop, not just raw input. */
int rt_messages_pending(CPU *c) {
    RtThread *th = rt_thread_of(c);
    pthread_mutex_lock(&user_mu);
    MsgQueue *q = th->msgq;
    int has = q && q->count > 0;
    pthread_mutex_unlock(&user_mu);
    return has;
}

/* Calls a window procedure. Returns 0 if a non-local return passed through (see rt_call_guest). */
static int call_proc(CPU *c, uint32_t proc, uint32_t hwnd, uint32_t msg, uint32_t wp, uint32_t lp, uint32_t *res) {
    uint32_t args[4] = {hwnd, msg, wp, lp};
    if (g_opt.verbose && msg != WM_MOUSEMOVE && msg != WM_TIMER)
        rt_log("[msg %x] hwnd %x msg %04x wparam %x lparam %x -> proc %08x\n", c->thread_id, hwnd, msg, wp, lp, proc);
    if (!proc) {
        if (res) *res = 0;
        return 1;
    }
    return rt_call_guest(c, proc, RT_STDCALL, 4, args, res);
}

/* SendMessage semantics: direct call on the owning thread, queued and awaited otherwise. */
static int send_message(CPU *c, uint32_t hwnd, uint32_t msg, uint32_t wp, uint32_t lp, uint32_t *res) {
    pthread_mutex_lock(&user_mu);
    Window *w = win_get(hwnd);
    if (!w) {
        pthread_mutex_unlock(&user_mu);
        if (res) *res = 0;
        return 1;
    }
    uint32_t proc = w->wndproc;
    RtThread *owner = w->owner;
    if (owner == rt_thread_of(c)) {
        pthread_mutex_unlock(&user_mu);
        return call_proc(c, proc, hwnd, msg, wp, lp, res);
    }
    SendCtx ctx = {0, 0};
    QMsg m = {hwnd, msg, wp, lp, now_ms(), 0, 0, &ctx};
    queue_push(owner, &m);
    while (!ctx.done) pthread_cond_wait(&user_cv, &user_mu);
    pthread_mutex_unlock(&user_mu);
    if (res) *res = ctx.result;
    return 1;
}

/* -------------------------------------------------------------- classes */
H(user_RegisterClassA) {
    pthread_once(&user_once, user_init);
    uint32_t p = a[0];
    const char *name = rt_gstr(ld32(p + 36));
    if (ld32(p + 36) < 0x10000) {
        rt_log("recomp: RegisterClassA with an atom instead of a name is not supported\n");
        return 0;
    }
    pthread_mutex_lock(&user_mu);
    if (find_class(name)) {
        pthread_mutex_unlock(&user_mu);
        rt_set_last_error(c, 1410); /* ERROR_CLASS_ALREADY_EXISTS */
        return 0;
    }
    add_class(name, ld32(p + 4), ld32(p), ld32(p + 12), ld32(p + 16));
    classes->cls_extra = ld32(p + 8);
    classes->hcursor = ld32(p + 24);
    pthread_mutex_unlock(&user_mu);
    if (g_opt.verbose) rt_log("[user] class \"%s\" registered, window procedure %08x\n", name, ld32(p + 4));
    return 0xC000u + (nwindows & 0xFFF) + 1;
}

/* -------------------------------------------------------------- windows */
static void activate_window(CPU *c, uint32_t hwnd);

/* CreateWindowExA(exStyle, class, name, style, x, y, w, h, parent, menu, instance, param) */
H(user_CreateWindowExA) {
    pthread_once(&user_once, user_init);
    if (a[1] < 0x10000) {
        rt_log("recomp: CreateWindowExA with a class atom is not supported\n");
        return 0;
    }
    const char *cname = rt_gstr(a[1]);
    pthread_mutex_lock(&user_mu);
    WndClass *k = find_class(cname);
    if (!k) {
        pthread_mutex_unlock(&user_mu);
        rt_log("recomp: CreateWindowExA: unknown window class \"%s\"\n", cname);
        rt_set_last_error(c, 1407); /* ERROR_CANNOT_FIND_WND_CLASS */
        return 0;
    }
    Window *w = calloc(1, sizeof *w);
    uint32_t i;
    for (i = 0; i < nwindows; i++)
        if (!windows[i]) break;
    if (i == nwindows) {
        windows = realloc(windows, (nwindows + 16) * sizeof *windows);
        memset(windows + nwindows, 0, 16 * sizeof *windows);
        nwindows += 16;
    }
    windows[i] = w;
    w->hwnd = HWND_BASE + 4 * i;
    w->cls = k;
    w->owner = rt_thread_of(c);
    w->wndproc = k->wndproc;
    w->exstyle = a[0];
    w->style = a[3];
    w->x = (int32_t)a[4];
    w->y = (int32_t)a[5];
    w->w = (int32_t)a[6];
    w->h = (int32_t)a[7];
    w->parent = a[8];
    w->id = a[9];
    w->hinstance = a[10];
    w->text = strdup(a[2] ? rt_gstr(a[2]) : "");
    w->text_limit = 30000;
    w->visible = (a[3] & WS_VISIBLE) != 0;
    uint32_t hwnd = w->hwnd, proc = w->wndproc;
    queue_of(w->owner);
    pthread_mutex_unlock(&user_mu);
    if (g_opt.verbose || g_trace_imports)
        rt_log("[user] window %x created: class \"%s\" name \"%s\" style %08x %dx%d parent %x thread %x\n", hwnd, cname,
               a[2] ? rt_gstr(a[2]) : "", a[3], (int32_t)a[6], (int32_t)a[7], a[8], c->thread_id);

    /* CREATESTRUCTA on the guest stack for WM_NCCREATE and WM_CREATE. */
    uint32_t sp = ESP;
    uint32_t cs = (sp - 64) & ~3u;
    st32(cs + 0, a[11]);
    st32(cs + 4, a[10]);
    st32(cs + 8, a[9]);
    st32(cs + 12, a[8]);
    st32(cs + 16, a[7]);
    st32(cs + 20, a[6]);
    st32(cs + 24, a[5]);
    st32(cs + 28, a[4]);
    st32(cs + 32, a[3]);
    st32(cs + 36, a[2]);
    st32(cs + 40, a[1]);
    st32(cs + 44, a[0]);
    ESP = cs - 16;
    uint32_t r = 0;
    if (!call_proc(c, proc, hwnd, WM_NCCREATE, 0, cs, &r)) return 0;
    if (!call_proc(c, proc, hwnd, WM_CREATE, 0, cs, &r)) return 0;
    ESP = sp;
    if (r == 0xFFFFFFFFu) {
        pthread_mutex_lock(&user_mu);
        windows[i] = NULL;
        pthread_mutex_unlock(&user_mu);
        free(w->text);
        free(w);
        return 0;
    }
    return hwnd;
}

static int destroy_window(CPU *c, uint32_t hwnd) {
    pthread_mutex_lock(&user_mu);
    Window *w = win_get(hwnd);
    if (!w || w->destroying) {
        pthread_mutex_unlock(&user_mu);
        return 1;
    }
    w->destroying = 1;
    uint32_t proc = w->wndproc;
    uint32_t children[64];
    unsigned nchildren = 0;
    for (uint32_t i = 0; i < nwindows && nchildren < 64; i++)
        if (windows[i] && windows[i]->parent == hwnd && !windows[i]->destroying) children[nchildren++] = windows[i]->hwnd;
    pthread_mutex_unlock(&user_mu);

    uint32_t r;
    if (!call_proc(c, proc, hwnd, WM_DESTROY, 0, 0, &r)) return 0;
    for (unsigned i = 0; i < nchildren; i++)
        if (!destroy_window(c, children[i])) return 0;
    if (!call_proc(c, proc, hwnd, WM_NCDESTROY, 0, 0, &r)) return 0;

    pthread_mutex_lock(&user_mu);
    for (size_t i = 0; i < RT_COUNTOF(timers); i++)
        if (timers[i].used && timers[i].hwnd == hwnd) timers[i].used = 0;
    MsgQueue *q = w->owner->msgq;
    if (q) {
        size_t o = 0;
        for (size_t i = 0; i < q->count; i++) {
            if (q->buf[i].hwnd == hwnd) {
                if (q->buf[i].send) q->buf[i].send->done = 1;
                continue;
            }
            q->buf[o++] = q->buf[i];
        }
        q->count = o;
        pthread_cond_broadcast(&user_cv);
    }
    windows[(hwnd - HWND_BASE) / 4] = NULL;
    if (focus_hwnd == hwnd) focus_hwnd = 0;
    if (active_hwnd == hwnd) active_hwnd = 0;
    if (main_hwnd == hwnd) main_hwnd = 0;
    pthread_mutex_unlock(&user_mu);
    free(w->text);
    free(w);
    return 1;
}

H(user_DestroyWindow) {
    pthread_mutex_lock(&user_mu);
    int ok = win_get(a[0]) != NULL;
    pthread_mutex_unlock(&user_mu);
    if (!ok) {
        rt_set_last_error(c, 1400); /* ERROR_INVALID_WINDOW_HANDLE */
        return 0;
    }
    destroy_window(c, a[0]);
    return 1;
}

static void set_window_text(Window *w, const char *s) {
    free(w->text);
    w->text = strdup(s);
    size_t n = strlen(w->text);
    w->sel_start = w->sel_end = (uint32_t)n;
}

/* DefWindowProcA(hwnd, msg, wparam, lparam) */
static uint32_t user_DefWindowProcA(CPU *c, const uint32_t *a) {
    uint32_t hwnd = a[0], msg = a[1], wp = a[2], lp = a[3], r = 0;
    switch (msg) {
    case WM_NCCREATE:
        return 1;
    case WM_CLOSE:
        destroy_window(c, hwnd);
        return 0;
    case WM_SYSCOMMAND:
        if ((wp & 0xFFF0) == SC_CLOSE) send_message(c, hwnd, WM_CLOSE, 0, 0, &r);
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_GETTEXTLENGTH:
    case WM_GETTEXT:
    case WM_SETTEXT: {
        pthread_mutex_lock(&user_mu);
        Window *w = win_get(hwnd);
        if (w) {
            if (msg == WM_SETTEXT) {
                set_window_text(w, lp ? rt_gstr(lp) : "");
                r = 1;
            } else if (msg == WM_GETTEXTLENGTH) {
                r = (uint32_t)strlen(w->text);
            } else if (wp) {
                size_t n = strlen(w->text);
                if (n >= wp) n = wp - 1;
                memcpy(G(lp), w->text, n);
                st8(lp + (uint32_t)n, 0);
                r = (uint32_t)n;
            }
        }
        pthread_mutex_unlock(&user_mu);
        return r;
    }
    default:
        return 0;
    }
}

/* The built-in EDIT class: a single-line text buffer edited by WM_CHAR. */
static uint32_t edit_wndproc(CPU *c, const uint32_t *a) {
    uint32_t hwnd = a[0], msg = a[1], wp = a[2], lp = a[3], r = 0;
    pthread_mutex_lock(&user_mu);
    Window *w = win_get(hwnd);
    if (!w) {
        pthread_mutex_unlock(&user_mu);
        return 0;
    }
    size_t len = strlen(w->text);
    switch (msg) {
    case EM_LIMITTEXT:
        w->text_limit = wp ? wp : 30000;
        break;
    case EM_SETSEL:
        w->sel_start = wp > len ? (uint32_t)len : wp;
        w->sel_end = lp > len ? (uint32_t)len : lp;
        break;
    case EM_GETSEL:
        if (wp) st32(wp, w->sel_start);
        if (lp) st32(lp, w->sel_end);
        r = (w->sel_start & 0xFFFF) | (w->sel_end << 16);
        break;
    case WM_CHAR: {
        uint32_t s = w->sel_start < w->sel_end ? w->sel_start : w->sel_end;
        uint32_t e = w->sel_start < w->sel_end ? w->sel_end : w->sel_start;
        if (s > len) s = (uint32_t)len;
        if (e > len) e = (uint32_t)len;
        if (wp == 8) { /* backspace */
            if (s == e && s > 0) s--;
            memmove(w->text + s, w->text + e, len - e + 1);
            w->sel_start = w->sel_end = s;
        } else if (wp >= 0x20 && wp != 0x7F && wp < 0x100) {
            if (len - (e - s) + 1 <= w->text_limit) {
                char *t = malloc(len + 2);
                memcpy(t, w->text, s);
                t[s] = (char)wp;
                memcpy(t + s + 1, w->text + e, len - e + 1);
                free(w->text);
                w->text = t;
                w->sel_start = w->sel_end = s + 1;
            }
        }
        break;
    }
    case WM_KEYDOWN:
        if (wp == 0x2E && w->sel_start < len) { /* VK_DELETE */
            memmove(w->text + w->sel_start, w->text + w->sel_start + 1, len - w->sel_start);
        } else if (wp == 0x25 && w->sel_start > 0) { /* VK_LEFT */
            w->sel_start = w->sel_end = w->sel_start - 1;
        } else if (wp == 0x27 && w->sel_start < len) { /* VK_RIGHT */
            w->sel_start = w->sel_end = w->sel_start + 1;
        } else if (wp == 0x24) { /* VK_HOME */
            w->sel_start = w->sel_end = 0;
        } else if (wp == 0x23) { /* VK_END */
            w->sel_start = w->sel_end = (uint32_t)len;
        }
        break;
    default:
        pthread_mutex_unlock(&user_mu);
        return user_DefWindowProcA(c, a);
    }
    pthread_mutex_unlock(&user_mu);
    return r;
}

H(user_CallWindowProcA) {
    uint32_t r = 0;
    call_proc(c, a[0], a[1], a[2], a[3], a[4], &r);
    return r;
}

H(user_SendMessageA) {
    uint32_t r = 0;
    send_message(c, a[0], a[1], a[2], a[3], &r);
    return r;
}

H(user_GetWindowLongA) {
    UNUSED;
    int32_t idx = (int32_t)a[1];
    uint32_t r = 0;
    pthread_mutex_lock(&user_mu);
    Window *w = win_get(a[0]);
    if (w) {
        switch (idx) {
        case GWL_WNDPROC: r = w->wndproc; break;
        case GWL_HINSTANCE: r = w->hinstance; break;
        case GWL_HWNDPARENT: r = w->parent; break;
        case GWL_ID: r = w->id; break;
        case GWL_STYLE: r = w->style | (w->visible ? WS_VISIBLE : 0); break;
        case GWL_EXSTYLE: r = w->exstyle; break;
        case GWL_USERDATA: r = w->userdata; break;
        default:
            if (idx >= 0 && (size_t)idx + 4 <= sizeof w->extra) memcpy(&r, w->extra + idx, 4);
            break;
        }
    }
    pthread_mutex_unlock(&user_mu);
    return r;
}

H(user_SetWindowLongA) {
    UNUSED;
    int32_t idx = (int32_t)a[1];
    uint32_t r = 0, v = a[2];
    pthread_mutex_lock(&user_mu);
    Window *w = win_get(a[0]);
    if (w) {
        switch (idx) {
        case GWL_WNDPROC: r = w->wndproc; w->wndproc = v; break;
        case GWL_HINSTANCE: r = w->hinstance; w->hinstance = v; break;
        case GWL_HWNDPARENT: r = w->parent; w->parent = v; break;
        case GWL_ID: r = w->id; w->id = v; break;
        case GWL_STYLE: r = w->style; w->style = v; break;
        case GWL_EXSTYLE: r = w->exstyle; w->exstyle = v; break;
        case GWL_USERDATA: r = w->userdata; w->userdata = v; break;
        default:
            if (idx >= 0 && (size_t)idx + 4 <= sizeof w->extra) {
                memcpy(&r, w->extra + idx, 4);
                memcpy(w->extra + idx, &v, 4);
            }
            break;
        }
    }
    pthread_mutex_unlock(&user_mu);
    return r;
}

H(user_GetWindowTextA) {
    uint32_t r = 0;
    send_message(c, a[0], WM_GETTEXT, a[2], a[1], &r);
    return r;
}

H(user_SetWindowTextA) {
    uint32_t r = 0;
    send_message(c, a[0], WM_SETTEXT, 0, a[1], &r);
    if (a[0] == main_hwnd) rt_video_set_title(rt_gstr(a[1]));
    return 1;
}

static int is_top_level_game_window(const Window *w) {
    return w && !(w->style & WS_CHILD) && w->wndproc != def_proc_addr && w->wndproc != edit_proc_addr;
}

/* Make hwnd the active window of the application, with the messages Windows sends. */
static void activate_window(CPU *c, uint32_t hwnd) {
    pthread_mutex_lock(&user_mu);
    Window *w = win_get(hwnd);
    int go = w && active_hwnd != hwnd && is_top_level_game_window(w);
    uint32_t old = active_hwnd;
    if (go) active_hwnd = hwnd;
    pthread_mutex_unlock(&user_mu);
    if (!go) return;
    uint32_t r;
    if (!old && !send_message(c, hwnd, WM_ACTIVATEAPP, 1, 0, &r)) return;
    send_message(c, hwnd, WM_ACTIVATE, 1, old, &r);
}

static uint32_t set_focus(CPU *c, uint32_t hwnd) {
    pthread_mutex_lock(&user_mu);
    uint32_t old = focus_hwnd;
    int valid = hwnd == 0 || win_get(hwnd) != NULL;
    if (valid) focus_hwnd = hwnd;
    pthread_mutex_unlock(&user_mu);
    if (!valid || old == hwnd) return old;
    uint32_t r;
    if (old && !send_message(c, old, WM_KILLFOCUS, hwnd, 0, &r)) return old;
    if (hwnd) send_message(c, hwnd, WM_SETFOCUS, old, 0, &r);
    return old;
}

H(user_SetFocus) { return set_focus(c, a[0]); }

H(user_SetForegroundWindow) {
    activate_window(c, a[0]);
    return 1;
}

H(user_ShowWindow) {
    pthread_mutex_lock(&user_mu);
    Window *w = win_get(a[0]);
    if (!w) {
        pthread_mutex_unlock(&user_mu);
        return 0;
    }
    int was = w->visible, show = a[1] != 0;
    w->visible = show;
    int top = is_top_level_game_window(w);
    char title[128];
    snprintf(title, sizeof title, "%s", w->text);
    int ww = w->w, hh = w->h; /* the game's own window size (= client area for its popup window) */
    if (top && show && !main_hwnd) main_hwnd = a[0];
    int is_main = main_hwnd == a[0];
    pthread_mutex_unlock(&user_mu);
    if (was != show) {
        uint32_t r;
        if (!send_message(c, a[0], WM_SHOWWINDOW, (uint32_t)show, 0, &r)) return 0;
    }
    if (is_main && show) {
        /* Use the game's own window size for the screen, so input, GetClientRect
         * and the render target all share one coordinate space (MapleStory lays
         * its UI out at 800x600, not the 640x480 default). */
        if (ww >= 320 && hh >= 240) { screen_w = ww; screen_h = hh; }
        rt_video_open(title, SCREEN_W, SCREEN_H);
        activate_window(c, a[0]);
        if (c->eip == RT_NL_PENDING) return 0;
        pthread_mutex_lock(&user_mu);
        int need_focus = focus_hwnd == 0;
        pthread_mutex_unlock(&user_mu);
        if (need_focus) set_focus(c, a[0]);
    }
    return (uint32_t)was;
}

H(user_UpdateWindow) { UNUSED; return 1; }

H(user_GetClientRect) {
    UNUSED;
    pthread_mutex_lock(&user_mu);
    Window *w = win_get(a[0]);
    int ww = 0, hh = 0, ok = w != NULL;
    if (w) {
        ww = w->w;
        hh = w->h;
        /* DirectDraw sizes the main window to the display mode. */
        if (a[0] == main_hwnd) {
            ww = SCREEN_W;
            hh = SCREEN_H;
        }
    }
    pthread_mutex_unlock(&user_mu);
    st32(a[1], 0);
    st32(a[1] + 4, 0);
    st32(a[1] + 8, (uint32_t)ww);
    st32(a[1] + 12, (uint32_t)hh);
    return (uint32_t)ok;
}

H(user_ClientToScreen) {
    UNUSED;
    pthread_mutex_lock(&user_mu);
    Window *w = win_get(a[0]);
    int ok = w != NULL, x = 0, y = 0;
    if (w && (w->style & WS_CHILD)) {
        x = w->x;
        y = w->y;
    }
    pthread_mutex_unlock(&user_mu);
    st32(a[1], ld32(a[1]) + (uint32_t)x);
    st32(a[1] + 4, ld32(a[1] + 4) + (uint32_t)y);
    return (uint32_t)ok;
}

/* ---------------------------------------------------------------- timers */
H(user_SetTimer) {
    pthread_once(&user_once, user_init);
    uint32_t id = a[1];
    pthread_mutex_lock(&user_mu);
    Window *w = a[0] ? win_get(a[0]) : NULL;
    RtThread *owner = w ? w->owner : rt_thread_of(c);
    Timer *t = NULL;
    for (size_t i = 0; i < RT_COUNTOF(timers) && !t; i++)
        if (timers[i].used && timers[i].hwnd == a[0] && timers[i].id == id && a[0]) t = &timers[i];
    for (size_t i = 0; i < RT_COUNTOF(timers) && !t; i++)
        if (!timers[i].used) t = &timers[i];
    if (!t || (a[0] && !w)) {
        pthread_mutex_unlock(&user_mu);
        return 0;
    }
    if (!a[0]) id = next_timer_id++;
    t->used = 1;
    t->hwnd = a[0];
    t->id = id;
    t->proc = a[3];
    t->period_ms = a[2] < 10 ? 10 : a[2];
    t->due_ns = rt_mono_ns() + (uint64_t)t->period_ms * 1000000ull;
    t->owner = owner;
    queue_of(owner);
    pthread_cond_broadcast(&user_cv);
    pthread_mutex_unlock(&user_mu);
    return id ? id : 1;
}

H(user_KillTimer) {
    UNUSED;
    uint32_t r = 0;
    pthread_mutex_lock(&user_mu);
    for (size_t i = 0; i < RT_COUNTOF(timers); i++) {
        if (timers[i].used && timers[i].hwnd == a[0] && timers[i].id == a[1]) {
            timers[i].used = 0;
            r = 1;
        }
    }
    pthread_mutex_unlock(&user_mu);
    return r;
}

/* ------------------------------------------------------------ the queue */
static void write_msg(uint32_t p, const QMsg *m) {
    st32(p, m->hwnd);
    st32(p + 4, m->msg);
    st32(p + 8, m->wparam);
    st32(p + 12, m->lparam);
    st32(p + 16, m->time);
    st32(p + 20, (uint32_t)m->x);
    st32(p + 24, (uint32_t)m->y);
}

static uint32_t key_lparam(const RtInputEvent *ev) {
    uint32_t lp = 1u | ((uint32_t)(ev->scan & 0x7F) << 16);
    if (ev->scan & 0x80) lp |= 1u << 24;
    if (!ev->down) lp |= 0xC0000000u;
    else if (ev->repeat) lp |= 0x40000000u;
    return lp;
}

static uint32_t mouse_wparam(void) {
    uint32_t wp = 0;
    if (rt_mouse_buttons & 1) wp |= 0x01;
    if (rt_mouse_buttons & 2) wp |= 0x02;
    if (rt_mouse_buttons & 4) wp |= 0x10;
    if (rt_vk_state[0x10] & 0x80) wp |= 0x04;
    if (rt_vk_state[0x11] & 0x80) wp |= 0x08;
    return wp;
}

/* Turn pending host input into posted messages. Video thread only. */
static void pump_input(void) {
    RtInputEvent ev;
    while (rt_video_poll(&ev)) {
        pthread_mutex_lock(&user_mu);
        Window *mw = win_get(main_hwnd);
        if (!mw) {
            pthread_mutex_unlock(&user_mu);
            continue;
        }
        QMsg m = {main_hwnd, 0, 0, 0, now_ms(), ev.x, ev.y, NULL};
        uint32_t pos = ((uint32_t)ev.y << 16) | ((uint32_t)ev.x & 0xFFFF);
        Window *fw = win_get(focus_hwnd);
        switch (ev.type) {
        case RT_EV_MOUSE_MOVE:
            m.msg = WM_MOUSEMOVE;
            m.wparam = mouse_wparam();
            m.lparam = pos;
            break;
        case RT_EV_MOUSE_BUTTON: {
            int dbl = ev.down && ev.repeat > 1 && !(ev.repeat & 1) && (mw->cls->style & CS_DBLCLKS);
            if (ev.code == 1) m.msg = ev.down ? (dbl ? WM_LBUTTONDBLCLK : WM_LBUTTONDOWN) : WM_LBUTTONUP;
            else if (ev.code == 3) m.msg = ev.down ? (dbl ? WM_RBUTTONDBLCLK : WM_RBUTTONDOWN) : WM_RBUTTONUP;
            else m.msg = ev.down ? WM_MBUTTONDOWN : WM_MBUTTONUP;
            m.wparam = mouse_wparam();
            m.lparam = pos;
            break;
        }
        case RT_EV_KEY: {
            int sys = (rt_vk_state[0x12] & 0x80) || ev.code == 0x79; /* Alt held, or F10 */
            m.hwnd = fw ? focus_hwnd : main_hwnd;
            m.msg = ev.down ? (sys ? WM_SYSKEYDOWN : WM_KEYDOWN) : (sys ? WM_SYSKEYUP : WM_KEYUP);
            m.wparam = (uint32_t)ev.code;
            m.lparam = key_lparam(&ev);
            break;
        }
        case RT_EV_CHAR:
            m.hwnd = fw ? focus_hwnd : main_hwnd;
            m.msg = WM_CHAR;
            m.wparam = (uint32_t)ev.code;
            m.lparam = 1;
            break;
        case RT_EV_FOCUS:
            m.msg = WM_ACTIVATEAPP;
            m.wparam = (uint32_t)ev.down;
            break;
        case RT_EV_QUIT:
            m.msg = WM_SYSCOMMAND;
            m.wparam = SC_CLOSE;
            break;
        default:
            break;
        }
        if (m.msg) queue_push(win_get(m.hwnd)->owner, &m);
        pthread_mutex_unlock(&user_mu);
    }
}

static int msg_matches(const QMsg *m, uint32_t hwnd, uint32_t lo, uint32_t hi) {
    if (hwnd && m->hwnd != hwnd) {
        /* a filter on a window also accepts messages for its children */
        Window *w = win_get(m->hwnd);
        if (!w || w->parent != hwnd) return 0;
    }
    if ((lo || hi) && (m->msg < lo || m->msg > hi)) return 0;
    return 1;
}

/* Returns 1 with *out filled when a message is available. Messages sent from other
 * threads are delivered here, before anything is returned. Returns -1 when a
 * non-local return passed through a window procedure. */
static int peek(CPU *c, QMsg *out, uint32_t hwnd, uint32_t lo, uint32_t hi, int remove) {
    RtThread *th = rt_thread_of(c);
    if (rt_video_on_video_thread()) pump_input();
    pthread_mutex_lock(&user_mu);
    MsgQueue *q = queue_of(th);
    for (;;) {
        size_t i;
        for (i = 0; i < q->count; i++)
            if (q->buf[i].send) break;
        if (i == q->count) break;
        QMsg m = q->buf[i];
        memmove(q->buf + i, q->buf + i + 1, (q->count - i - 1) * sizeof *q->buf);
        q->count--;
        Window *w = win_get(m.hwnd);
        uint32_t proc = w ? w->wndproc : 0;
        pthread_mutex_unlock(&user_mu);
        uint32_t r = 0;
        int ok = call_proc(c, proc, m.hwnd, m.msg, m.wparam, m.lparam, &r);
        pthread_mutex_lock(&user_mu);
        m.send->result = r;
        m.send->done = 1;
        pthread_cond_broadcast(&user_cv);
        if (!ok) {
            pthread_mutex_unlock(&user_mu);
            return -1;
        }
    }
    for (size_t i = 0; i < q->count; i++) {
        if (!msg_matches(&q->buf[i], hwnd, lo, hi)) continue;
        *out = q->buf[i];
        if (remove) {
            memmove(q->buf + i, q->buf + i + 1, (q->count - i - 1) * sizeof *q->buf);
            q->count--;
        }
        pthread_mutex_unlock(&user_mu);
        return 1;
    }
    if (q->quit && q->count == 0 && !hwnd) {
        QMsg m = {0, WM_QUIT, q->quit_code, 0, now_ms(), 0, 0, NULL};
        *out = m;
        if (remove) q->quit = 0;
        pthread_mutex_unlock(&user_mu);
        return 1;
    }
    uint64_t now = rt_mono_ns();
    for (size_t i = 0; i < RT_COUNTOF(timers); i++) {
        Timer *t = &timers[i];
        if (!t->used || t->owner != th || t->due_ns > now) continue;
        QMsg m = {t->hwnd, WM_TIMER, t->id, t->proc, (uint32_t)(now / 1000000ull), 0, 0, NULL};
        if (!msg_matches(&m, hwnd, lo, hi)) continue;
        if (remove) t->due_ns = now + (uint64_t)t->period_ms * 1000000ull;
        *out = m;
        pthread_mutex_unlock(&user_mu);
        return 1;
    }
    pthread_mutex_unlock(&user_mu);
    return 0;
}

/* PeekMessageA(msg, hwnd, min, max, remove) */
H(user_PeekMessageA) {
    pthread_once(&user_once, user_init);
    QMsg m;
    int r = peek(c, &m, a[1], a[2], a[3], (a[4] & PM_REMOVE) != 0);
    if (r < 0) return 0;
    if (r == 0) {
        /* The game spins on PeekMessage between frames; give the processor a rest. */
        static __thread uint64_t last_idle;
        uint64_t now = rt_mono_ns();
        if (now - last_idle < 2000000ull) rt_sleep_ns(500000ull);
        last_idle = rt_mono_ns();
        return 0;
    }
    write_msg(a[0], &m);
    return 1;
}

/* GetMessageA(msg, hwnd, min, max) */
H(user_GetMessageA) {
    pthread_once(&user_once, user_init);
    RtThread *th = rt_thread_of(c);
    QMsg m;
    for (;;) {
        int r = peek(c, &m, a[1], a[2], a[3], 1);
        if (r < 0) return 0;
        if (r > 0) break;
        if (rt_video_on_video_thread()) {
            rt_sleep_ns(2000000ull); /* host input has no wake-up of its own */
            continue;
        }
        pthread_mutex_lock(&user_mu);
        MsgQueue *q = queue_of(th);
        if (q->count == 0 && !q->quit) {
            uint64_t due = UINT64_MAX;
            for (size_t i = 0; i < RT_COUNTOF(timers); i++)
                if (timers[i].used && timers[i].owner == th && timers[i].due_ns < due) due = timers[i].due_ns;
            if (due == UINT64_MAX) {
                pthread_cond_wait(&user_cv, &user_mu);
            } else {
                struct timespec ts = {(time_t)(due / 1000000000ull), (long)(due % 1000000000ull)};
                pthread_cond_timedwait(&user_cv, &user_mu, &ts);
            }
        }
        pthread_mutex_unlock(&user_mu);
    }
    write_msg(a[0], &m);
    return m.msg != WM_QUIT;
}

H(user_TranslateMessage) {
    uint32_t p = a[0];
    uint32_t msg = ld32(p + 4), vk = ld32(p + 8);
    if (msg != WM_KEYDOWN) return 0;
    /* Printable characters arrive from the host as text input; only the control
     * characters are derived from key presses here. */
    uint32_t ch = 0;
    if (vk == 0x0D) ch = 13;
    else if (vk == 0x08) ch = 8;
    else if (vk == 0x09) ch = 9;
    else if (vk == 0x1B) ch = 27;
    if (!ch) return 0;
    pthread_mutex_lock(&user_mu);
    Window *w = win_get(ld32(p));
    if (w) {
        QMsg m = {ld32(p), WM_CHAR, ch, ld32(p + 12), ld32(p + 16), 0, 0, NULL};
        MsgQueue *q = queue_of(w->owner);
        /* WM_CHAR goes to the head of the queue, as it follows its key press immediately. */
        queue_push(w->owner, &m);
        memmove(q->buf + 1, q->buf, (q->count - 1) * sizeof *q->buf);
        q->buf[0] = m;
    }
    pthread_mutex_unlock(&user_mu);
    (void)c;
    return 1;
}

H(user_DispatchMessageA) {
    uint32_t p = a[0];
    uint32_t hwnd = ld32(p), msg = ld32(p + 4), wp = ld32(p + 8), lp = ld32(p + 12), r = 0;
    if (msg == WM_TIMER && lp) {
        uint32_t args[4] = {hwnd, WM_TIMER, wp, now_ms()};
        rt_call_guest(c, lp, RT_STDCALL, 4, args, &r);
        return r;
    }
    if (!hwnd) return 0;
    pthread_mutex_lock(&user_mu);
    Window *w = win_get(hwnd);
    uint32_t proc = w ? w->wndproc : 0;
    pthread_mutex_unlock(&user_mu);
    /* --trace-input: key, character and button messages as the game's window procedure gets them */
    if (g_opt.trace_input && proc && ((msg >= 0x100 && msg <= 0x105) || (msg >= 0x200 && msg <= 0x206)))
        rt_log("[msg] %04x wparam %x at %d,%d -> window procedure %08x\n", msg, wp, (int)(lp & 0xFFFF), (int)(lp >> 16),
               proc);
    if (proc) call_proc(c, proc, hwnd, msg, wp, lp, &r);
    return r;
}

H(user_PostQuitMessage) {
    pthread_once(&user_once, user_init);
    if (getenv("RECOMP_TRACE_EXIT")) { rt_log("\nPostQuitMessage(%u) called:\n", a[0]); rt_backtrace(c); }
    pthread_mutex_lock(&user_mu);
    MsgQueue *q = queue_of(rt_thread_of(c));
    q->quit = 1;
    q->quit_code = a[0];
    pthread_cond_broadcast(&user_cv);
    pthread_mutex_unlock(&user_mu);
    return 0;
}

/* ------------------------------------------------------- cursor and keys */
/* Cursor handles: 0x0001xxxx a stock cursor (IDC_* - 32500), 0x0002xxxx a cursor
 * made from a module's resources (the video cursor id). */
#define STOCK_CURSOR(idc) (0x00010000u | ((idc) - 32500u))
static const struct { uint16_t idc; uint8_t sdl; } stock_cursors[] = {
    /* IDC_* -> SDL_SystemCursor */
    {32512, 0}, {32513, 1}, {32514, 2}, {32515, 3}, {32516, 0}, {32640, 9}, {32641, 0}, {32642, 5},
    {32643, 6}, {32644, 7}, {32645, 8}, {32646, 9}, {32648, 10}, {32649, 11}, {32650, 4}, {32651, 0},
};

/* A cursor from a module's RT_GROUP_CURSOR (12): the entry nearest 32x32 with the
 * most colours. Its RT_CURSOR (1) is the hot spot (two WORDs) and a DIB: a
 * BITMAPINFOHEADER of twice the height, the colour (XOR) bitmap bottom-up, then the
 * 1-bit AND mask. Returns the video cursor id, -1 if there is none. */
static int cursor_from_resource(uint32_t hmod, uint32_t id) {
    uint32_t gsz = 0, g = rt_resource(hmod, 12, id, &gsz);
    if (!g || gsz < 6) return -1;
    uint32_t n = ld16(g + 4), pick = 0;
    int best = -1;
    for (uint32_t i = 0; i < n && 6 + 14 * (i + 1) <= gsz; i++) {
        uint32_t e = g + 6 + 14 * i;
        int w = ld16(e), bpp = ld16(e + 6);
        int score = (w == 32 ? 1000 : 500 - abs(w - 32)) + bpp;
        if (score > best) best = score, pick = ld16(e + 12);
    }
    uint32_t sz = 0, d = best >= 0 ? rt_resource(hmod, 1, pick, &sz) : 0;
    if (!d || sz < 4 + 40) return -1;
    int hx = ld16(d), hy = ld16(d + 2);
    uint32_t bi = d + 4, hdr = ld32(bi);
    int w = (int32_t)ld32(bi + 4), h = (int32_t)ld32(bi + 8) / 2, bpp = ld16(bi + 14);
    uint32_t comp = ld32(bi + 16), used = ld32(bi + 32);
    if (w <= 0 || h <= 0 || w > 256 || h > 256 || (comp != 0 && !(comp == 3 && bpp == 32))) return -1;
    if (bpp != 1 && bpp != 4 && bpp != 8 && bpp != 24 && bpp != 32) return -1;
    uint32_t ncolors = bpp <= 8 ? (used ? used : 1u << bpp) : 0;
    uint32_t pal = bi + hdr + (comp == 3 ? 12 : 0);
    uint32_t xor_bits = pal + 4 * ncolors, xor_stride = (uint32_t)((w * bpp + 31) / 32) * 4;
    uint32_t and_bits = xor_bits + xor_stride * (uint32_t)h, and_stride = (uint32_t)((w + 31) / 32) * 4;
    if (and_bits + and_stride * (uint32_t)h > d + sz) return -1;
    uint32_t *px = malloc((size_t)w * (size_t)h * 4);
    if (!px) return -1;
    int has_alpha = 0;
    for (int y = 0; y < h; y++) {
        uint32_t row = xor_bits + xor_stride * (uint32_t)(h - 1 - y), mrow = and_bits + and_stride * (uint32_t)(h - 1 - y);
        for (int x = 0; x < w; x++) {
            uint32_t rgb;
            switch (bpp) {
            case 1: rgb = ld32(pal + 4 * ((ld8(row + (uint32_t)x / 8) >> (7 - x % 8)) & 1)); break;
            case 4: rgb = ld32(pal + 4 * ((ld8(row + (uint32_t)x / 2) >> (x % 2 ? 0 : 4)) & 15)); break;
            case 8: rgb = ld32(pal + 4 * ld8(row + (uint32_t)x)); break;
            case 24: rgb = ld8(row + 3u * (uint32_t)x) | ld8(row + 3u * (uint32_t)x + 1) << 8 | ld8(row + 3u * (uint32_t)x + 2) << 16; break;
            default: rgb = ld32(row + 4u * (uint32_t)x); break;
            }
            if (bpp == 32 && (rgb >> 24)) has_alpha = 1;
            int masked = (ld8(mrow + (uint32_t)x / 8) >> (7 - x % 8)) & 1;
            /* AND 1 with colour 0 is transparent; with a colour it inverts the screen,
             * shown here as opaque black. */
            uint32_t v = masked ? ((rgb & 0xFFFFFF) ? 0xFF000000u : 0) : (0xFF000000u | (rgb & 0xFFFFFF));
            px[y * w + x] = v;
        }
    }
    if (bpp == 32 && has_alpha) { /* the alpha channel decides, not the mask */
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++) px[y * w + x] = ld32(xor_bits + xor_stride * (uint32_t)(h - 1 - y) + 4u * (uint32_t)x);
    }
    int vid = rt_video_cursor_create(px, w, h, hx, hy);
    free(px);
    return vid;
}

static struct { uint32_t hmod, id; int vid; } res_cursors[256];
static int nres_cursors;

static uint32_t load_cursor(uint32_t hmod, uint32_t name) {
    if (name >= 0x10000) return STOCK_CURSOR(32512); /* by name: not used by the game */
    if (hmod == 0) {
        for (size_t i = 0; i < RT_COUNTOF(stock_cursors); i++)
            if (stock_cursors[i].idc == name) return STOCK_CURSOR(name);
        return 0;
    }
    pthread_mutex_lock(&user_mu);
    int vid = -1, found = 0;
    for (int i = 0; i < nres_cursors && !found; i++)
        if (res_cursors[i].hmod == hmod && res_cursors[i].id == name) vid = res_cursors[i].vid, found = 1;
    pthread_mutex_unlock(&user_mu);
    if (!found) {
        vid = cursor_from_resource(hmod, name);
        if (g_opt.verbose) rt_log("[user] cursor #%u of %#x -> %d\n", name, hmod, vid);
        pthread_mutex_lock(&user_mu);
        if (nres_cursors < (int)RT_COUNTOF(res_cursors)) res_cursors[nres_cursors++] = (typeof(res_cursors[0])){hmod, name, vid};
        pthread_mutex_unlock(&user_mu);
    }
    return vid >= 0 ? 0x00020000u | (uint32_t)vid : STOCK_CURSOR(32512);
}

H(user_LoadCursorA) {
    UNUSED;
    return load_cursor(a[0], a[1]);
}

static void show_cursor_handle(uint32_t h) {
    if ((h & 0xFFFF0000u) == 0x00020000u) {
        rt_video_set_cursor((int)(h & 0xFFFF));
    } else if ((h & 0xFFFF0000u) == 0x00010000u) {
        uint32_t idc = (h & 0xFFFF) + 32500u;
        for (size_t i = 0; i < RT_COUNTOF(stock_cursors); i++)
            if (stock_cursors[i].idc == idc) rt_video_set_cursor(RT_CURSOR_SYSTEM + stock_cursors[i].sdl);
    } else if (h == 0) {
        rt_video_set_cursor(RT_CURSOR_NONE);
    }
}

H(user_SetCursor) {
    UNUSED;
    uint32_t old = __atomic_exchange_n(&current_cursor, a[0], __ATOMIC_SEQ_CST);
    if (old != a[0]) show_cursor_handle(a[0]);
    return old;
}

H(user_ShowCursor) {
    UNUSED;
    pthread_mutex_lock(&user_mu);
    cursor_count += a[0] ? 1 : -1;
    int n = cursor_count;
    pthread_mutex_unlock(&user_mu);
    rt_video_show_cursor(n >= 0);
    return (uint32_t)n;
}

H(user_SetCursorPos) {
    UNUSED;
    rt_mouse_x = (int32_t)a[0];
    rt_mouse_y = (int32_t)a[1];
    rt_video_warp_mouse((int32_t)a[0], (int32_t)a[1]);
    return 1;
}

H(user_GetKeyState) {
    UNUSED;
    uint8_t s = rt_vk_state[a[0] & 0xFF];
    return (uint32_t)(int32_t)(int16_t)(((s & 0x80) ? 0xFF80 : 0) | (s & 1));
}

/* ------------------------------------------------------------ MessageBox */
H(user_MessageBoxA) {
    fflush(stdout);
    rt_log("\nrecomp: MessageBoxA from thread %x (type %#x)\n  caption: %s\n  text:    %s\n", c->thread_id, a[3],
           a[2] ? rt_gstr(a[2]) : "(default: Error)", a[1] ? rt_gstr(a[1]) : "");
    if (g_opt.verbose) rt_backtrace(c);
    rt_video_message_box(a[2] ? rt_gstr(a[2]) : "Error", a[1] ? rt_gstr(a[1]) : "");
    switch (a[3] & 0xF) {
    case 4: return 6; /* MB_YESNO: IDYES */
    case 3: return 6; /* MB_YESNOCANCEL */
    default: return 1; /* IDOK */
    }
}

/* -------------------------------------------------------------- wsprintf
 * int wsprintfA(char *out, const char *fmt, ...): cdecl, so the variable
 * arguments are read from the guest stack. */
/* wfmt: wsprintfW, where %s and %c are wide (%hs, %S narrow); text is UTF-8 here. */
static uint32_t format_guest(char *out, size_t cap, const char *fmt, uint32_t argp, int wfmt) {
    size_t o = 0;
#define PUT(ch) do { if (o + 1 < cap) out[o] = (char)(ch); o++; } while (0)
    while (*fmt) {
        if (*fmt != '%') {
            PUT(*fmt++);
            continue;
        }
        const char *start = fmt++;
        char spec[32], buf[64];
        size_t sn = 0;
        spec[sn++] = '%';
        while (*fmt && strchr("-+ #0", *fmt) && sn < 8) spec[sn++] = *fmt++;
        while (isdigit((unsigned char)*fmt) && sn < 16) spec[sn++] = *fmt++;
        if (*fmt == '.') {
            spec[sn++] = *fmt++;
            while (isdigit((unsigned char)*fmt) && sn < 24) spec[sn++] = *fmt++;
        }
        int wide = -1;
        while (*fmt == 'l' || *fmt == 'h' || *fmt == 'w' || *fmt == 'I') {
            if (*fmt == 'l' || *fmt == 'w') wide = 1;
            else if (*fmt == 'h') wide = 0;
            fmt++;
        }
        char conv = *fmt ? *fmt++ : 0;
        const char *s = buf;
        switch (conv) {
        case 'd': case 'i': case 'u': case 'x': case 'X':
            spec[sn++] = conv;
            spec[sn] = 0;
            snprintf(buf, sizeof buf, spec, ld32(argp));
            argp += 4;
            break;
        case 'c': case 'C': {
            int wch = wide == 1 || (wide < 0 && (conv == 'C') != wfmt);
            uint32_t ch = ld32(argp) & (wch ? 0xFFFF : 0xFF);
            argp += 4;
            if (ch < 0x80 || (!wch && !wfmt)) buf[0] = (char)ch, buf[1] = 0; /* a byte of the A text */
            else if (ch < 0x800) buf[0] = (char)(0xC0 | ch >> 6), buf[1] = (char)(0x80 | (ch & 63)), buf[2] = 0;
            else buf[0] = (char)(0xE0 | ch >> 12), buf[1] = (char)(0x80 | ((ch >> 6) & 63)), buf[2] = (char)(0x80 | (ch & 63)), buf[3] = 0;
            break;
        }
        case 's': case 'S': {
            uint32_t p = ld32(argp);
            argp += 4;
            int prec = -1, width = 0, left = 0;
            for (size_t k = 1; k < sn; k++) {
                if (spec[k] == '-') left = 1;
                else if (spec[k] == '.') { prec = atoi(spec + k + 1); break; }
                else if (isdigit((unsigned char)spec[k]) && spec[k] != '0' && !width) { width = atoi(spec + k); }
            }
            char tmp[1024];
            size_t n = 0;
            if (!p) {
                n = (size_t)snprintf(tmp, sizeof tmp, "(null)");
            } else if (wide == 1 || (wide < 0 && (conv == 'S') != wfmt)) {
                n = rt_get_wstr(p, tmp, sizeof tmp);
                if (n >= sizeof tmp) n = sizeof tmp - 1;
            } else {
                while (n + 1 < sizeof tmp && ld8(p + (uint32_t)n)) {
                    tmp[n] = (char)ld8(p + (uint32_t)n);
                    n++;
                }
            }
            if (prec >= 0 && n > (size_t)prec) n = (size_t)prec;
            if (!left) for (size_t k = n; k < (size_t)width; k++) PUT(' ');
            for (size_t k = 0; k < n; k++) PUT(tmp[k]);
            if (left) for (size_t k = n; k < (size_t)width; k++) PUT(' ');
            s = "";
            break;
        }
        case '%':
            buf[0] = '%';
            buf[1] = 0;
            break;
        default: /* unknown conversion: copied through, as wsprintf does */
            for (const char *q = start; q < fmt; q++) PUT(*q);
            s = "";
            break;
        }
        while (*s) PUT(*s++);
    }
#undef PUT
    if (cap) out[o < cap ? o : cap - 1] = 0;
    return (uint32_t)o;
}

H(user_wsprintfA) {
    char out[1025]; /* wsprintf never writes more than 1024 characters */
    uint32_t n = format_guest(out, sizeof out, rt_gstr(a[1]), ESP + 12, 0);
    if (n > 1024) n = 1024;
    memcpy(G(a[0]), out, n + 1);
    return n;
}
H(user_wsprintfW) {
    char fmt[2048], out[4097];
    rt_get_wstr(a[1], fmt, sizeof fmt);
    format_guest(out, sizeof out, fmt, ESP + 12, 1);
    return rt_put_wstr(a[0], 1025, out);
}

/* ------------------------------------------------------------ gdi32, imm32 */
H(user_GetDC) { UNUSED; return HDC_FAKE; }
H(user_ReleaseDC) { UNUSED; return 1; }

H(gdi_GetDeviceCaps) {
    UNUSED;
    switch (a[1]) {
    case 8: return SCREEN_W;  /* HORZRES */
    case 10: return SCREEN_H; /* VERTRES */
    case 12: return (uint32_t)screen_bpp; /* BITSPIXEL */
    case 14: return 1;        /* PLANES */
    case 88: case 90: return 96; /* LOGPIXELSX/Y */
    case 116: return 60;      /* VREFRESH */
    case 117: return SCREEN_H; /* DESKTOPVERTRES */
    case 118: return SCREEN_W; /* DESKTOPHORZRES */
    default: return 0;
    }
}

H(imm_ImmDisableIME) { UNUSED; return 1; }
H(imm_ImmGetContext) { UNUSED; return 0; }
H(imm_ImmGetOpenStatus) { UNUSED; return 0; }
H(imm_ImmGetCompositionStringA) { UNUSED; return 0; }
H(imm_ImmSetCompositionWindow) { UNUSED; return 0; }

/* ===================================================== Age of Mythology additions
 * W forms, rectangles, window geometry, display modes, painting and other
 * functions GunBound never called. */
#define WM_GETMINMAXINFO 0x0024
#define SM_CXSCREEN 0
#define SM_CYSCREEN 1

/* WNDCLASSEXA without cbSize is a WNDCLASSA. */
H(user_RegisterClassExA) {
    uint32_t args[1] = {a[0] + 4};
    return user_RegisterClassA(c, args);
}
/* The W forms copy the structure with the strings converted. */
static uint32_t register_w(CPU *c, uint32_t wc /* WNDCLASSW layout */) {
    uint32_t tmp = rt_heap_alloc(40 + 256, 1);
    memcpy(G(tmp), G(wc), 40);
    uint32_t name = ld32(wc + 36);
    if (name >= 0x10000) {
        rt_get_wstr(name, (char *)G(tmp + 40), 256);
        st32(tmp + 36, tmp + 40);
    }
    st32(tmp + 32, 0); /* menu name: no menus */
    uint32_t args[1] = {tmp};
    uint32_t r = user_RegisterClassA(c, args);
    rt_heap_free(tmp);
    return r;
}
H(user_RegisterClassW) { UNUSED; return register_w(c, a[0]); }
H(user_RegisterClassExW) { UNUSED; return register_w(c, a[0] + 4); }
H(user_UnregisterClass) { UNUSED; return 1; }
H(user_CreateWindowExW) { return rt_call_a(c, a, 12, "user32.dll", "CreateWindowExA", (1u << 1) | (1u << 2)); }
H(user_SetWindowTextW) { return rt_call_a(c, a, 2, "user32.dll", "SetWindowTextA", 2); }
H(user_MessageBoxW) { return rt_call_a(c, a, 4, "user32.dll", "MessageBoxA", (1u << 1) | (1u << 2)); }
H(user_GetWindowTextW) {
    uint32_t tmp = rt_heap_alloc(1024, 1);
    uint32_t args[3] = {a[0], tmp, 1024};
    user_GetWindowTextA(c, args);
    uint32_t n = a[2] ? rt_put_wstr(a[1], a[2], (const char *)G(tmp)) : 0;
    rt_heap_free(tmp);
    return n;
}
H(user_PostMessageA) {
    UNUSED;
    if (!a[0]) {
        /* A thread message for the calling thread. */
        pthread_mutex_lock(&user_mu);
        QMsg m = {0, a[1], a[2], a[3], now_ms(), rt_mouse_x, rt_mouse_y, NULL};
        queue_push(rt_thread_of(c), &m);
        pthread_mutex_unlock(&user_mu);
        return 1;
    }
    return (uint32_t)rt_post_message(a[0], a[1], a[2], a[3]);
}
H(user_LoadIcon) { UNUSED; return 0x00020001u; }
H(user_LoadCursorW) { return user_LoadCursorA(c, a); }

/* ------------------------------------------------------------- rectangles */
static void rect_get(uint32_t r, int32_t v[4]) {
    for (int i = 0; i < 4; i++) v[i] = (int32_t)ld32(r + 4 * (uint32_t)i);
}
static void rect_put(uint32_t r, const int32_t v[4]) {
    for (int i = 0; i < 4; i++) st32(r + 4 * (uint32_t)i, (uint32_t)v[i]);
}
H(user_SetRect) {
    UNUSED;
    int32_t v[4] = {(int32_t)a[1], (int32_t)a[2], (int32_t)a[3], (int32_t)a[4]};
    rect_put(a[0], v);
    return 1;
}
H(user_SetRectEmpty) { UNUSED; memset(G(a[0]), 0, 16); return 1; }
H(user_IsRectEmpty) {
    UNUSED;
    int32_t v[4];
    rect_get(a[0], v);
    return v[2] <= v[0] || v[3] <= v[1];
}
H(user_EqualRect) { UNUSED; return !memcmp(G(a[0]), G(a[1]), 16); }
H(user_IntersectRect) {
    UNUSED;
    int32_t p[4], q[4], r[4];
    rect_get(a[1], p);
    rect_get(a[2], q);
    r[0] = p[0] > q[0] ? p[0] : q[0];
    r[1] = p[1] > q[1] ? p[1] : q[1];
    r[2] = p[2] < q[2] ? p[2] : q[2];
    r[3] = p[3] < q[3] ? p[3] : q[3];
    if (r[2] <= r[0] || r[3] <= r[1]) {
        memset(G(a[0]), 0, 16);
        return 0;
    }
    rect_put(a[0], r);
    return 1;
}
H(user_PtInRect) {
    UNUSED;
    int32_t v[4], x = (int32_t)a[1], y = (int32_t)a[2];
    rect_get(a[0], v);
    int r = x >= v[0] && x < v[2] && y >= v[1] && y < v[3];
    if (getenv("RECOMP_TRACE_HIT")) rt_log("[hit] origin ~ %d,%d size %dx%d cursor %d,%d %s\n", rt_mouse_x - x, rt_mouse_y - y, v[2], v[3], rt_mouse_x, rt_mouse_y, r?"HIT":"");
    return r;
}
H(user_OffsetRect) {
    UNUSED;
    int32_t v[4];
    rect_get(a[0], v);
    v[0] += (int32_t)a[1], v[2] += (int32_t)a[1], v[1] += (int32_t)a[2], v[3] += (int32_t)a[2];
    rect_put(a[0], v);
    return 1;
}

/* ------------------------------------------------------- window geometry */
/* The main window always fills the guest screen, with no frame. */
H(user_AdjustWindowRect) { UNUSED; return 1; }
H(user_GetWindowRect) {
    UNUSED;
    pthread_mutex_lock(&user_mu);
    Window *w = win_get(a[0]);
    int32_t v[4] = {0, 0, SCREEN_W, SCREEN_H};
    if (w && a[0] != main_hwnd) {
        v[0] = w->x, v[1] = w->y, v[2] = w->x + w->w, v[3] = w->y + w->h;
    }
    pthread_mutex_unlock(&user_mu);
    if (a[0] == 0x00010000u) { /* the desktop */
        v[0] = v[1] = 0, v[2] = SCREEN_W, v[3] = SCREEN_H;
    } else if (!w) {
        return 0;
    }
    rect_put(a[1], v);
    return 1;
}
static void move_window(uint32_t hwnd, int32_t x, int32_t y, int32_t w_, int32_t h, int move, int size) {
    pthread_mutex_lock(&user_mu);
    Window *w = win_get(hwnd);
    if (w) {
        if (move) w->x = x, w->y = y;
        if (size) w->w = w_, w->h = h;
    }
    pthread_mutex_unlock(&user_mu);
}
/* SetWindowPos(hwnd, after, x, y, cx, cy, flags) */
H(user_SetWindowPos) {
    UNUSED;
    move_window(a[0], (int32_t)a[2], (int32_t)a[3], (int32_t)a[4], (int32_t)a[5], !(a[6] & 2), !(a[6] & 1));
    if (a[6] & 0x40) { /* SWP_SHOWWINDOW */
        uint32_t args[2] = {a[0], 5};
        user_ShowWindow(c, args);
    }
    return 1;
}
H(user_MoveWindow) {
    UNUSED;
    move_window(a[0], (int32_t)a[1], (int32_t)a[2], (int32_t)a[3], (int32_t)a[4], 1, 1);
    return 1;
}
H(user_IsWindow) {
    UNUSED;
    pthread_mutex_lock(&user_mu);
    int ok = win_get(a[0]) != NULL;
    pthread_mutex_unlock(&user_mu);
    return (uint32_t)ok;
}
H(user_IsWindowVisible) {
    UNUSED;
    pthread_mutex_lock(&user_mu);
    Window *w = win_get(a[0]);
    int v = w && w->visible;
    pthread_mutex_unlock(&user_mu);
    return (uint32_t)v;
}
H(user_IsIconic) { UNUSED; return 0; }
H(user_GetDesktopWindow) { UNUSED; return 0x00010000u; }
H(user_GetForegroundWindow) { UNUSED; return active_hwnd; }
H(user_GetActiveWindow) { UNUSED; return active_hwnd; }
H(user_GetFocus) { UNUSED; return focus_hwnd; }
H(user_GetTopWindow) { UNUSED; return main_hwnd; }
H(user_GetWindow) { UNUSED; return 0; }
H(user_GetParent) {
    UNUSED;
    pthread_mutex_lock(&user_mu);
    Window *w = win_get(a[0]);
    uint32_t p = w ? w->parent : 0;
    pthread_mutex_unlock(&user_mu);
    return p;
}
H(user_GetWindowThreadProcessId) {
    UNUSED;
    pthread_mutex_lock(&user_mu);
    Window *w = win_get(a[0]);
    uint32_t tid = w ? w->owner->tid : 0;
    pthread_mutex_unlock(&user_mu);
    if (a[1]) st32(a[1], 0x10); /* the process id GetCurrentProcessId reports */
    return tid;
}
H(user_EnumThreadWindows) { UNUSED; return 1; }
H(user_BringWindowToTop) { UNUSED; return 1; }
H(user_RedrawWindow) { UNUSED; return 1; }
H(user_InvalidateRect) { UNUSED; return 1; }
H(user_ValidateRect) { UNUSED; return 1; }
H(user_ScreenToClient) { UNUSED; return 1; } /* the main window sits at 0,0 */
H(user_GetCursorPos) {
    UNUSED;
    st32(a[0], (uint32_t)rt_mouse_x);
    st32(a[0] + 4, (uint32_t)rt_mouse_y);
    return 1;
}
H(user_ClipCursor) { UNUSED; return 1; }
H(user_SetCapture) { UNUSED; static uint32_t cap; uint32_t o = cap; cap = a[0]; return o; }
H(user_ReleaseCapture) { UNUSED; return 1; }
H(user_GetAsyncKeyState) {
    UNUSED;
    uint32_t vk = a[0] & 0xFF;
    int down;
    if (vk == 1) down = rt_mouse_buttons & 1;
    else if (vk == 2) down = rt_mouse_buttons & 2;
    else if (vk == 4) down = rt_mouse_buttons & 4;
    else down = rt_vk_state[vk] & 0x80;
    return down ? 0xFFFF8000u : 0;
}
/* MapVirtualKeyA(code, type): 0 VK->scan, 1 scan->VK, 2 VK->char. */
H(user_MapVirtualKeyA) {
    UNUSED;
    static const uint8_t scan_of_letter[26] = {0x1E, 0x30, 0x2E, 0x20, 0x12, 0x21, 0x22, 0x23, 0x17,
                                               0x24, 0x25, 0x26, 0x32, 0x31, 0x18, 0x19, 0x10, 0x13,
                                               0x1F, 0x14, 0x16, 0x2F, 0x11, 0x2D, 0x15, 0x2C};
    uint32_t k = a[0];
    switch (a[1]) {
    case 0:
        if (k >= 'A' && k <= 'Z') return scan_of_letter[k - 'A'];
        if (k >= '1' && k <= '9') return 2 + (k - '1');
        if (k == '0') return 0x0B;
        switch (k) {
        case 0x08: return 0x0E; case 0x09: return 0x0F; case 0x0D: return 0x1C; case 0x1B: return 0x01;
        case 0x20: return 0x39; case 0x10: return 0x2A; case 0x11: return 0x1D; case 0x12: return 0x38;
        default: return 0;
        }
    case 1:
        for (int i = 0; i < 26; i++)
            if (scan_of_letter[i] == k) return 'A' + (uint32_t)i;
        if (k >= 2 && k <= 10) return '1' + (k - 2);
        if (k == 0x0B) return '0';
        return 0;
    case 2:
        if ((k >= 'A' && k <= 'Z') || (k >= '0' && k <= '9') || k == ' ') return k;
        return 0;
    default: return 0;
    }
}
H(user_GetKeyboardLayout) { UNUSED; return 0x04090409u; }
H(user_GetKeyboardLayoutList) {
    UNUSED;
    if (a[0] && a[1]) st32(a[1], 0x04090409u);
    return 1;
}
H(user_GetCaretBlinkTime) { UNUSED; return 530; }
H(user_MessageBeep) { UNUSED; return 1; }
H(user_GetSystemMetrics) {
    UNUSED;
    switch (a[0]) {
    case SM_CXSCREEN: case 78: case 16: return (uint32_t)SCREEN_W; /* also SM_CXVIRTUALSCREEN, SM_CXFULLSCREEN */
    case SM_CYSCREEN: case 79: case 17: return (uint32_t)SCREEN_H;
    case 2: case 3: return 16;  /* SM_CXVSCROLL, SM_CYHSCROLL */
    case 4: return 19;          /* SM_CYCAPTION */
    case 5: case 6: return 1;   /* SM_CXBORDER */
    case 7: case 8: return 3;   /* SM_CXDLGFRAME */
    case 11: case 12: return 32; /* SM_CXICON */
    case 13: case 14: return 32; /* SM_CXCURSOR */
    case 19: return 1;          /* SM_MOUSEPRESENT */
    case 32: case 33: return 4; /* SM_CXFRAME */
    case 36: case 37: return 4; /* SM_CXDOUBLECLK */
    case 43: return 3;          /* SM_CMOUSEBUTTONS */
    case 80: return 1;          /* SM_CMONITORS */
    default: return 0;
    }
}
/* SystemParametersInfo(action, uiParam, pvParam, winIni) */
H(user_SystemParametersInfo) {
    UNUSED;
    switch (a[0]) {
    case 0x0030: /* SPI_GETWORKAREA */ {
        int32_t v[4] = {0, 0, SCREEN_W, SCREEN_H};
        rect_put(a[2], v);
        return 1;
    }
    case 0x000A: case 0x0016: case 0x0068: case 0x0003: /* mouse speed, keyboard delay ... */
        if (a[2]) st32(a[2], 0);
        return 1;
    default:
        if (a[2] && a[0] < 0x1000 && (a[0] & 1) == 0) st32(a[2], 0); /* a GET: report "off" */
        return 1;
    }
}

/* ---------------------------------------------------------- display modes */
static const struct { int w, h; } display_modes[] = {{640, 480}, {800, 600}, {1024, 768}, {1280, 1024}};

/* ChangeDisplaySettingsEx(device, DEVMODE*, hwnd, flags, param). DEVMODEA has
 * dmBitsPerPel at 104, dmPelsWidth at 108, dmPelsHeight at 112; DEVMODEW has
 * the same fields 64 bytes later (the two name arrays are WCHAR). */
static uint32_t change_display(uint32_t dm, int wide) {
    if (!dm) return 0; /* back to the registry mode */
    uint32_t o = wide ? 64 : 0;
    uint32_t fields = ld32(dm + o + 40);
    int w = (fields & 0x80000) ? (int)ld32(dm + o + 108) : screen_w;
    int h = (fields & 0x100000) ? (int)ld32(dm + o + 112) : screen_h;
    int bpp = (fields & 0x40000) ? (int)ld32(dm + o + 104) : screen_bpp;
    int ok = 0;
    for (size_t i = 0; i < RT_COUNTOF(display_modes); i++) ok |= display_modes[i].w == w && display_modes[i].h == h;
    if (!ok || (bpp != 16 && bpp != 32)) {
        rt_log("[user] ChangeDisplaySettings %dx%dx%d: not a mode the runtime offers\n", w, h, bpp);
        return (uint32_t)-2; /* DISP_CHANGE_BADMODE */
    }
    screen_w = w, screen_h = h, screen_bpp = bpp;
    if (g_opt.verbose) rt_log("[user] display mode %dx%dx%d\n", w, h, bpp);
    return 0; /* DISP_CHANGE_SUCCESSFUL */
}
void rt_user_set_display_mode(int w, int h, int bpp) {
    if (g_opt.verbose && (w != screen_w || h != screen_h)) rt_log("[user] display mode %dx%dx%d (Direct3D)\n", w, h, bpp);
    screen_w = w, screen_h = h, screen_bpp = bpp;
}

void rt_user_fullscreen_window(CPU *c, uint32_t hwnd, int w, int h, int bpp) {
    rt_user_set_display_mode(w, h, bpp);
    pthread_mutex_lock(&user_mu);
    Window *win = win_get(hwnd);
    int changed = win && (win->x || win->y || win->w != w || win->h != h);
    if (win) win->x = 0, win->y = 0, win->w = w, win->h = h;
    pthread_mutex_unlock(&user_mu);
    if (!changed) return;
    uint32_t r;
    if (!send_message(c, hwnd, WM_MOVE, 0, 0, &r)) return;
    send_message(c, hwnd, WM_SIZE, 0 /* SIZE_RESTORED */, (uint32_t)(h << 16 | (w & 0xFFFF)), &r);
}

H(user_ChangeDisplaySettingsA) { UNUSED; return change_display(a[0], 0); }
H(user_ChangeDisplaySettingsW) { UNUSED; return change_display(a[0], 1); }
H(user_ChangeDisplaySettingsExA) { UNUSED; return change_display(a[1], 0); }
H(user_ChangeDisplaySettingsExW) { UNUSED; return change_display(a[1], 1); }

/* EnumDisplaySettings(device, mode, DEVMODE*) with ENUM_CURRENT_SETTINGS = -1. */
static uint32_t enum_display(uint32_t mode, uint32_t dm, int wide) {
    uint32_t o = wide ? 64 : 0;
    int w, h, bpp;
    if (mode == 0xFFFFFFFFu || mode == 0xFFFFFFFEu) {
        w = screen_w, h = screen_h, bpp = screen_bpp;
    } else {
        uint32_t n = (uint32_t)RT_COUNTOF(display_modes);
        if (mode >= 2 * n) return 0;
        w = display_modes[mode % n].w, h = display_modes[mode % n].h, bpp = mode < n ? 16 : 32;
    }
    st16(dm + o + 36, (uint16_t)(o + 156)); /* dmSize */
    st32(dm + o + 40, 0x40000 | 0x80000 | 0x100000 | 0x400000);
    st32(dm + o + 104, (uint32_t)bpp);
    st32(dm + o + 108, (uint32_t)w);
    st32(dm + o + 112, (uint32_t)h);
    st32(dm + o + 120, 60); /* dmDisplayFrequency */
    return 1;
}
H(user_EnumDisplaySettingsA) { UNUSED; return enum_display(a[1], a[2], 0); }
H(user_EnumDisplaySettingsW) { UNUSED; return enum_display(a[1], a[2], 1); }
H(user_EnumDisplayDevicesW) {
    UNUSED;
    /* DISPLAY_DEVICEW: cb, DeviceName[32], DeviceString[128], StateFlags, DeviceID[128], DeviceKey[128] */
    if (a[1] != 0) return 0;
    uint32_t p = a[2];
    rt_put_wstr(p + 4, 32, "\\\\.\\DISPLAY1");
    rt_put_wstr(p + 68, 128, "Recompilation display");
    st32(p + 324, 0x5); /* ATTACHED_TO_DESKTOP | PRIMARY_DEVICE */
    return 1;
}

/* ------------------------------------------------------------- painting */
/* BeginPaint(hwnd, PAINTSTRUCT*): nothing is drawn through GDI windows. */
H(user_BeginPaint) {
    UNUSED;
    memset(G(a[1]), 0, 64);
    st32(a[1], HDC_FAKE);
    return HDC_FAKE;
}
H(user_EndPaint) { UNUSED; return 1; }
H(user_FillRect) { UNUSED; return 1; }

/* ----------------------------------------------- clipboard, menus, dialogs */
H(user_false) { UNUSED; return 0; }
H(user_true) { UNUSED; return 1; }

/* ------------------------------------------------------------- dialogs */
/* Skip a DLGTEMPLATE sz_Or_Ord field; copy a string one into out. */
static uint32_t dlg_field(uint32_t p, char *out, size_t cap) {
    if (out) out[0] = 0;
    uint16_t w = ld16(p);
    if (w == 0) return p + 2;
    if (w == 0xFFFF) {
        if (out) snprintf(out, cap, "#%u", ld16(p + 2));
        return p + 4;
    }
    if (out) rt_get_wstr(p, out, cap);
    return p + 2 * (rt_wlen(p) + 1);
}
/* DialogBoxParamA/W(hinst, templateName, parent, proc, param): a modal dialog
 * from a resource template. Not shown; the answer is IDOK. (MapleStory's real
 * interface is its own Direct3D window, not a dialog.) */
H(user_DialogBoxParam) {
    UNUSED;
    if (g_opt.verbose) rt_log("[user] DialogBoxParam template %#x answered IDOK\n", a[1]);
    return 1; /* IDOK */
}

/* DialogBoxIndirectParamW(hinst, template, parent, proc, param). Dialogs cannot be
 * shown: the template's title and texts are logged and the answer is IDOK. */
H(user_DialogBoxIndirectParamW) {
    UNUSED;
    uint32_t t = a[1];
    int ex = ld16(t) == 1 && ld16(t + 2) == 0xFFFF;
    uint32_t style = ex ? ld32(t + 12) : ld32(t);
    uint16_t n = ex ? ld16(t + 16) : ld16(t + 8);
    uint32_t p = t + (ex ? 26 : 18);
    char title[256], text[512];
    p = dlg_field(p, NULL, 0);           /* menu */
    p = dlg_field(p, NULL, 0);           /* class */
    p = dlg_field(p, title, sizeof title);
    if (style & 0x40) {                  /* DS_SETFONT */
        p += ex ? 6 : 2;
        p = dlg_field(p, NULL, 0);
    }
    rt_log("[user] dialog \"%s\" (%u controls) answered IDOK\n", title, n);
    for (uint16_t i = 0; i < n; i++) {
        p = (p + 3) & ~3u;
        p += ex ? 24 : 18;
        char cls[64];
        p = dlg_field(p, cls, sizeof cls);
        p = dlg_field(p, text, sizeof text);
        uint16_t extra = ld16(p);
        p += 2 + extra;
        if (text[0] && text[0] != '#') rt_log("[user]   %s: %s\n", cls, text);
    }
    return 1;
}

/* ---------------------------------------------------------- characters */
static uint32_t wcase(uint32_t p, int upper) {
    if (p < 0x10000) { /* a single character */
        if (upper && p >= 'a' && p <= 'z') return p - 32;
        if (!upper && p >= 'A' && p <= 'Z') return p + 32;
        return p;
    }
    for (uint32_t q = p; ld16(q); q += 2) {
        uint16_t ch = ld16(q);
        if (upper && ch >= 'a' && ch <= 'z') st16(q, (uint16_t)(ch - 32));
        if (!upper && ch >= 'A' && ch <= 'Z') st16(q, (uint16_t)(ch + 32));
    }
    return p;
}
H(user_CharUpperW) { UNUSED; return wcase(a[0], 1); }
H(user_CharLowerW) { UNUSED; return wcase(a[0], 0); }
H(user_CharNextA) { UNUSED; return ld8(a[0]) ? a[0] + 1 : a[0]; }

static const ImportDef defs[] = {
    {"user32.dll", "RegisterClassA", user_RegisterClassA, 1, 0},
    {"user32.dll", "RegisterClassExA", user_RegisterClassExA, 1, 0},
    {"user32.dll", "RegisterClassW", user_RegisterClassW, 1, 0},
    {"user32.dll", "RegisterClassExW", user_RegisterClassExW, 1, 0},
    {"user32.dll", "UnregisterClassA", user_UnregisterClass, 2, 0},
    {"user32.dll", "UnregisterClassW", user_UnregisterClass, 2, 0},
    {"user32.dll", "CreateWindowExW", user_CreateWindowExW, 12, 0},
    {"user32.dll", "DefWindowProcW", user_DefWindowProcA, 4, 0},
    {"user32.dll", "CallWindowProcW", user_CallWindowProcA, 5, 0},
    {"user32.dll", "SendMessageW", user_SendMessageA, 4, 0},
    {"user32.dll", "PostMessageA", user_PostMessageA, 4, 0},
    {"user32.dll", "PostMessageW", user_PostMessageA, 4, 0},
    {"user32.dll", "GetWindowLongW", user_GetWindowLongA, 2, 0},
    {"user32.dll", "SetWindowLongW", user_SetWindowLongA, 3, 0},
    {"user32.dll", "GetWindowTextW", user_GetWindowTextW, 3, 0},
    {"user32.dll", "SetWindowTextW", user_SetWindowTextW, 2, 0},
    {"user32.dll", "PeekMessageW", user_PeekMessageA, 5, 0},
    {"user32.dll", "GetMessageW", user_GetMessageA, 4, 0},
    {"user32.dll", "DispatchMessageW", user_DispatchMessageA, 1, 0},
    {"user32.dll", "MessageBoxW", user_MessageBoxW, 4, 0},
    {"user32.dll", "LoadCursorW", user_LoadCursorW, 2, 0},
    {"user32.dll", "LoadIconA", user_LoadIcon, 2, 0},
    {"user32.dll", "LoadIconW", user_LoadIcon, 2, 0},
    {"user32.dll", "DestroyCursor", user_true, 1, 0},
    {"user32.dll", "SetRect", user_SetRect, 5, 0},
    {"user32.dll", "SetRectEmpty", user_SetRectEmpty, 1, 0},
    {"user32.dll", "IsRectEmpty", user_IsRectEmpty, 1, 0},
    {"user32.dll", "EqualRect", user_EqualRect, 2, 0},
    {"user32.dll", "IntersectRect", user_IntersectRect, 3, 0},
    {"user32.dll", "PtInRect", user_PtInRect, 3, 0},
    {"user32.dll", "OffsetRect", user_OffsetRect, 3, 0},
    {"user32.dll", "AdjustWindowRect", user_AdjustWindowRect, 3, 0},
    {"user32.dll", "AdjustWindowRectEx", user_AdjustWindowRect, 4, 0},
    {"user32.dll", "GetWindowRect", user_GetWindowRect, 2, 0},
    {"user32.dll", "SetWindowPos", user_SetWindowPos, 7, 0},
    {"user32.dll", "MoveWindow", user_MoveWindow, 6, 0},
    {"user32.dll", "IsWindow", user_IsWindow, 1, 0},
    {"user32.dll", "IsWindowVisible", user_IsWindowVisible, 1, 0},
    {"user32.dll", "IsIconic", user_IsIconic, 1, 0},
    {"user32.dll", "GetDesktopWindow", user_GetDesktopWindow, 0, 0},
    {"user32.dll", "GetForegroundWindow", user_GetForegroundWindow, 0, 0},
    {"user32.dll", "GetActiveWindow", user_GetActiveWindow, 0, 0},
    {"user32.dll", "GetFocus", user_GetFocus, 0, 0},
    {"user32.dll", "GetTopWindow", user_GetTopWindow, 1, 0},
    {"user32.dll", "GetWindow", user_GetWindow, 2, 0},
    {"user32.dll", "GetParent", user_GetParent, 1, 0},
    {"user32.dll", "GetWindowThreadProcessId", user_GetWindowThreadProcessId, 2, 0},
    {"user32.dll", "EnumThreadWindows", user_EnumThreadWindows, 3, 0},
    {"user32.dll", "BringWindowToTop", user_BringWindowToTop, 1, 0},
    {"user32.dll", "RedrawWindow", user_RedrawWindow, 4, 0},
    {"user32.dll", "InvalidateRect", user_InvalidateRect, 3, 0},
    {"user32.dll", "ValidateRect", user_ValidateRect, 2, 0},
    {"user32.dll", "ScreenToClient", user_ScreenToClient, 2, 0},
    {"user32.dll", "GetCursorPos", user_GetCursorPos, 1, 0},
    {"user32.dll", "ClipCursor", user_ClipCursor, 1, 0},
    {"user32.dll", "SetCapture", user_SetCapture, 1, 0},
    {"user32.dll", "ReleaseCapture", user_ReleaseCapture, 0, 0},
    {"user32.dll", "GetAsyncKeyState", user_GetAsyncKeyState, 1, 0},
    {"user32.dll", "MapVirtualKeyA", user_MapVirtualKeyA, 2, 0},
    {"user32.dll", "MapVirtualKeyW", user_MapVirtualKeyA, 2, 0},
    {"user32.dll", "GetKeyboardLayout", user_GetKeyboardLayout, 1, 0},
    {"user32.dll", "GetKeyboardLayoutList", user_GetKeyboardLayoutList, 2, 0},
    {"user32.dll", "GetCaretBlinkTime", user_GetCaretBlinkTime, 0, 0},
    {"user32.dll", "MessageBeep", user_MessageBeep, 1, 0},
    {"user32.dll", "GetSystemMetrics", user_GetSystemMetrics, 1, 0},
    {"user32.dll", "SystemParametersInfoA", user_SystemParametersInfo, 4, 0},
    {"user32.dll", "SystemParametersInfoW", user_SystemParametersInfo, 4, 0},
    {"user32.dll", "ChangeDisplaySettingsA", user_ChangeDisplaySettingsA, 2, 0},
    {"user32.dll", "ChangeDisplaySettingsW", user_ChangeDisplaySettingsW, 2, 0},
    {"user32.dll", "ChangeDisplaySettingsExA", user_ChangeDisplaySettingsExA, 5, 0},
    {"user32.dll", "ChangeDisplaySettingsExW", user_ChangeDisplaySettingsExW, 5, 0},
    {"user32.dll", "EnumDisplaySettingsA", user_EnumDisplaySettingsA, 3, 0},
    {"user32.dll", "EnumDisplaySettingsW", user_EnumDisplaySettingsW, 3, 0},
    {"user32.dll", "EnumDisplayDevicesW", user_EnumDisplayDevicesW, 4, 0},
    {"user32.dll", "BeginPaint", user_BeginPaint, 2, 0},
    {"user32.dll", "EndPaint", user_EndPaint, 2, 0},
    {"user32.dll", "FillRect", user_FillRect, 3, 0},
    {"user32.dll", "OpenClipboard", user_false, 1, 0},
    {"user32.dll", "CloseClipboard", user_true, 0, 0},
    {"user32.dll", "EmptyClipboard", user_false, 0, 0},
    {"user32.dll", "SetClipboardData", user_false, 2, 0},
    {"user32.dll", "GetClipboardData", user_false, 1, 0},
    {"user32.dll", "IsClipboardFormatAvailable", user_false, 1, 0},
    {"user32.dll", "GetMenu", user_false, 1, 0},
    {"user32.dll", "DestroyMenu", user_true, 1, 0},
    {"user32.dll", "EndDialog", user_true, 2, 0},
    {"user32.dll", "FindWindowA", user_false, 2, 0},
    {"user32.dll", "FindWindowW", user_false, 2, 0},
    {"user32.dll", "DialogBoxParamA", user_DialogBoxParam, 5, 0},
    {"user32.dll", "DialogBoxParamW", user_DialogBoxParam, 5, 0},
    {"user32.dll", "DialogBoxIndirectParamW", user_DialogBoxIndirectParamW, 5, 0},
    {"user32.dll", "GetDlgItem", user_false, 2, 0},
    {"user32.dll", "GetScrollPos", user_false, 2, 0},
    {"user32.dll", "CharUpperW", user_CharUpperW, 1, 0},
    {"user32.dll", "CharLowerW", user_CharLowerW, 1, 0},
    {"user32.dll", "CharNextA", user_CharNextA, 1, 0},
    {"user32.dll", "CreateWindowExA", user_CreateWindowExA, 12, 0},
    {"user32.dll", "DestroyWindow", user_DestroyWindow, 1, 0},
    {"user32.dll", "DefWindowProcA", user_DefWindowProcA, 4, 0},
    {"user32.dll", "CallWindowProcA", user_CallWindowProcA, 5, 0},
    {"user32.dll", "SendMessageA", user_SendMessageA, 4, 0},
    {"user32.dll", "GetWindowLongA", user_GetWindowLongA, 2, 0},
    {"user32.dll", "SetWindowLongA", user_SetWindowLongA, 3, 0},
    {"user32.dll", "GetWindowTextA", user_GetWindowTextA, 3, 0},
    {"user32.dll", "SetWindowTextA", user_SetWindowTextA, 2, 0},
    {"user32.dll", "SetFocus", user_SetFocus, 1, 0},
    {"user32.dll", "SetForegroundWindow", user_SetForegroundWindow, 1, 0},
    {"user32.dll", "ShowWindow", user_ShowWindow, 2, 0},
    {"user32.dll", "UpdateWindow", user_UpdateWindow, 1, 0},
    {"user32.dll", "GetClientRect", user_GetClientRect, 2, 0},
    {"user32.dll", "ClientToScreen", user_ClientToScreen, 2, 0},
    {"user32.dll", "SetTimer", user_SetTimer, 4, 0},
    {"user32.dll", "KillTimer", user_KillTimer, 2, 0},
    {"user32.dll", "PeekMessageA", user_PeekMessageA, 5, 0},
    {"user32.dll", "GetMessageA", user_GetMessageA, 4, 0},
    {"user32.dll", "TranslateMessage", user_TranslateMessage, 1, 0},
    {"user32.dll", "DispatchMessageA", user_DispatchMessageA, 1, 0},
    {"user32.dll", "PostQuitMessage", user_PostQuitMessage, 1, 0},
    {"user32.dll", "LoadCursorA", user_LoadCursorA, 2, 0},
    {"user32.dll", "SetCursor", user_SetCursor, 1, 0},
    {"user32.dll", "ShowCursor", user_ShowCursor, 1, 0},
    {"user32.dll", "SetCursorPos", user_SetCursorPos, 2, 0},
    {"user32.dll", "GetKeyState", user_GetKeyState, 1, 0},
    {"user32.dll", "MessageBoxA", user_MessageBoxA, 4, 0},
    {"user32.dll", "wsprintfA", user_wsprintfA, 2, 1},
    {"user32.dll", "wsprintfW", user_wsprintfW, 2, 1},
    {"user32.dll", "GetDC", user_GetDC, 1, 0},
    {"user32.dll", "ReleaseDC", user_ReleaseDC, 2, 0},
    {"gdi32.dll", "GetDeviceCaps", gdi_GetDeviceCaps, 2, 0},
    {"imm32.dll", "ImmDisableIME", imm_ImmDisableIME, 1, 0},
    {"imm32.dll", "ImmGetContext", imm_ImmGetContext, 1, 0},
    {"imm32.dll", "ImmGetOpenStatus", imm_ImmGetOpenStatus, 1, 0},
    {"imm32.dll", "ImmGetCompositionStringA", imm_ImmGetCompositionStringA, 4, 0},
    {"imm32.dll", "ImmSetCompositionWindow", imm_ImmSetCompositionWindow, 2, 0},
};
RT_EXPORT_IMPORTS(rt_imports_user, defs);

uint32_t rt_main_window(void) { return main_hwnd; }

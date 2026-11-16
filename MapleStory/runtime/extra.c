/* MapleStory: Win32 functions the client resolves at run time with GetProcAddress
 * and calls through stored pointers. A slot left null (an unimplemented name)
 * faults when called, so these are provided here. Correctness-sensitive ones
 * (RECT maths, file mapping) are real; the rest are benign stubs that let the
 * client proceed, to be fleshed out when their effect is reached. */
#include <pthread.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "com.h"
#include "rt.h"

/* ------------------------------------------------------------- keyboard hook
 * MapleStory reads the login fields' keystrokes through a WH_KEYBOARD_LL hook
 * (also its anti-keylogger), not through the message queue or polling. Deliver
 * key events to the installed hook on the guest's main thread, from the per-frame
 * MsgWaitForMultipleObjects. */
static uint32_t ll_kbd_hook;
static pthread_mutex_t ll_mu = PTHREAD_MUTEX_INITIALIZER;
#define LL_Q 128
static struct { uint16_t vk, scan; uint8_t up; } ll_q[LL_Q];
static int ll_head, ll_tail;

void rt_ll_kbd_queue(int vk, int scan, int down) {
    if (getenv("RECOMP_TRACE_KEY")) rt_log("[llkbd] queue vk=%d scan=%d down=%d hook=%x\n", vk, scan, down, ll_kbd_hook);
    pthread_mutex_lock(&ll_mu);
    if (ll_kbd_hook) {
        int n = (ll_tail + 1) % LL_Q;
        if (n != ll_head) {
            ll_q[ll_tail].vk = (uint16_t)vk;
            ll_q[ll_tail].scan = (uint16_t)scan;
            ll_q[ll_tail].up = (uint8_t)!down;
            ll_tail = n;
        }
    }
    pthread_mutex_unlock(&ll_mu);
}

static int ll_flush_logged;
static void ll_kbd_flush(CPU *c) {
    if (getenv("RECOMP_TRACE_KEY") && !ll_flush_logged) { ll_flush_logged = 1; rt_log("[llkbd] flush called, hook=%x head=%d tail=%d\n", ll_kbd_hook, ll_head, ll_tail); }
    if (!ll_kbd_hook) return;
    for (;;) {
        pthread_mutex_lock(&ll_mu);
        if (ll_head == ll_tail) { pthread_mutex_unlock(&ll_mu); return; }
        uint16_t vk = ll_q[ll_head].vk, scan = ll_q[ll_head].scan;
        uint8_t up = ll_q[ll_head].up;
        ll_head = (ll_head + 1) % LL_Q;
        pthread_mutex_unlock(&ll_mu);
        uint32_t sp = ESP;
        uint32_t s = rt_stack_alloc(c, 32); /* KBDLLHOOKSTRUCT */
        st32(s + 0, vk);                    /* vkCode */
        st32(s + 4, scan);                  /* scanCode */
        st32(s + 8, up ? 0x80u : 0u);       /* flags: LLKHF_UP */
        st32(s + 12, 0);                    /* time */
        st32(s + 16, 0);                    /* dwExtraInfo */
        uint32_t args[3] = {0u /* HC_ACTION */, up ? 0x101u /* WM_KEYUP */ : 0x100u /* WM_KEYDOWN */, s};
        uint32_t r = 0;
        if (getenv("RECOMP_TRACE_KEY")) rt_log("[llkbd] hook %x vk=%u scan=%u up=%u\n", ll_kbd_hook, vk, scan, up);
        rt_call_guest(c, ll_kbd_hook, RT_STDCALL, 3, args, &r);
        if (getenv("RECOMP_TRACE_KEY")) rt_log("[llkbd] -> %u\n", r);
        ESP = sp;
    }
}

/* ------------------------------------------------------------------ user32: RECT */
/* A RECT is four int32: left, top, right, bottom. */
H(ex_CopyRect) {
    UNUSED;
    memmove(G(a[0]), G(a[1]), 16);
    return 1;
}
H(ex_InflateRect) {
    UNUSED;
    int32_t dx = (int32_t)a[1], dy = (int32_t)a[2];
    st32(a[0], (uint32_t)((int32_t)ld32(a[0]) - dx));
    st32(a[0] + 4, (uint32_t)((int32_t)ld32(a[0] + 4) - dy));
    st32(a[0] + 8, (uint32_t)((int32_t)ld32(a[0] + 8) + dx));
    st32(a[0] + 12, (uint32_t)((int32_t)ld32(a[0] + 12) + dy));
    return 1;
}
H(ex_UnionRect) {
    UNUSED; /* (dst, a, b): the bounding rectangle of a and b */
    int32_t al = (int32_t)ld32(a[1]), at = (int32_t)ld32(a[1] + 4), ar = (int32_t)ld32(a[1] + 8), ab = (int32_t)ld32(a[1] + 12);
    int32_t bl = (int32_t)ld32(a[2]), bt = (int32_t)ld32(a[2] + 4), br = (int32_t)ld32(a[2] + 8), bb = (int32_t)ld32(a[2] + 12);
    int ae = al >= ar || at >= ab, be = bl >= br || bt >= bb;
    if (ae && be) { memset(G(a[0]), 0, 16); return 0; }
    if (ae) { memmove(G(a[0]), G(a[2]), 16); return 1; }
    if (be) { memmove(G(a[0]), G(a[1]), 16); return 1; }
    st32(a[0], (uint32_t)(al < bl ? al : bl));
    st32(a[0] + 4, (uint32_t)(at < bt ? at : bt));
    st32(a[0] + 8, (uint32_t)(ar > br ? ar : br));
    st32(a[0] + 12, (uint32_t)(ab > bb ? ab : bb));
    return 1;
}
H(ex_GetDoubleClickTime) { UNUSED; return 500; }
H(ex_GetWindowTextLength) { UNUSED; return 0; }
H(ex_CharPrevA) {
    UNUSED; /* (start, current): the previous character (no DBCS handling) */
    return a[1] > a[0] ? a[1] - 1 : a[0];
}
H(ex_InSendMessage) { UNUSED; return 0; }
H(ex_SendDlgItemMessageA) { UNUSED; return 0; }
H(ex_DrawTextA) {
    UNUSED; /* (hdc, text, count, rect, format): no GDI text drawing; report a height */
    return 16;
}
/* Windows hooks: accepted but never fire (no WH_* hooks in a headless/one-window
 * client that matters at boot). CallNextHookEx passes through. */
H(ex_SetWindowsHookEx) {
    UNUSED; /* (idHook, lpfn, hmod, dwThreadId) */
    if (a[0] == 13 || a[0] == 2) ll_kbd_hook = a[1]; /* WH_KEYBOARD_LL / WH_KEYBOARD */
    if (getenv("RECOMP_TRACE_KEY")) rt_log("[llkbd] SetWindowsHookEx id=%u proc=%x -> stored=%x\n", a[0], a[1], ll_kbd_hook);
    return 0xC0DE0100u + a[0];
}
H(ex_UnhookWindowsHookEx) {
    UNUSED;
    if (a[0] == 0xC0DE0100u + 13 || a[0] == 0xC0DE0100u + 2) ll_kbd_hook = 0;
    return 1;
}
H(ex_CallNextHookEx) { UNUSED; return 0; }
H(ex_MsgWaitForMultipleObjects) {
    UNUSED; /* (nCount, pHandles, fWaitAll, dwMilliseconds, dwWakeMask) */
    ll_kbd_flush(c); /* let the keyboard hook see the keys (it blocks Win/Esc) */
    if (getenv("RECOMP_TRACE_FOCUS")) { static uint32_t pf=0xbad; uint32_t f=ld32(0x978364u); uint32_t fld=f?ld32(f+0x80u):0; if (fld!=pf){pf=fld; rt_log("[focus2] focusedField=%x\n", fld);} }
    if (getenv("RECOMP_TRACE_FOCUS")) { static uint32_t pf=0xbad; uint32_t uim=ld32(0x9784c0u); uint32_t f=uim?ld32(uim+0x80u):0; if (f!=pf){pf=f; rt_log("[focus] focusedField=%x\n", f);} }
    /* A signaled wait handle wins first. */
    if (a[0] > 0) {
        uint32_t r = rt_wait_handles(c, a[1], a[0], (int)a[2], 0);
        if (r != 0x102u) return r;
    }
    /* If input is queued and the caller wants input events (dwWakeMask != 0),
     * report WAIT_OBJECT_0 + nCount so the game pumps its message loop
     * (PeekMessage -> TranslateMessage -> DispatchMessage). This is how
     * MapleStory's main loop learns a keystroke or click is waiting. */
    if (a[4] && (rt_input_pending() || rt_messages_pending(c))) return a[0];
    return 0x102; /* WAIT_TIMEOUT */
}

/* ------------------------------------------------------------------ kernel32: file mapping */
/* CreateFileMappingA/MapViewOfFile live in file.c (they need the file layer to
 * back a view with the mapped file's bytes). OpenFileMapping reports absent
 * (there is one process, so no named mapping from another one exists). */
H(ex_OpenFileMappingA) {
    UNUSED;
    rt_set_last_error(c, 2); /* ERROR_FILE_NOT_FOUND: no existing mapping */
    return 0;
}
H(ex_UnmapViewOfFile) { UNUSED; return 1; }
H(ex_FlushViewOfFile) { UNUSED; return 1; }
H(ex_OpenProcess) { UNUSED; return 0xC0DE0200u; } /* a process handle */
H(ex_WinExec) { UNUSED; return 32; }               /* >31 = success, nothing launched */
H(ex_CompareFileTime) {
    UNUSED;
    uint64_t x = ld32(a[0]) | (uint64_t)ld32(a[0] + 4) << 32;
    uint64_t y = ld32(a[1]) | (uint64_t)ld32(a[1] + 4) << 32;
    return x < y ? (uint32_t)-1 : x > y ? 1 : 0;
}
H(ex_GetVolumeInformationA) {
    UNUSED; /* (root, nameBuf, nameLen, serial*, maxComp*, flags*, fsBuf, fsLen) */
    if (a[1] && a[2]) st8(a[1], 0);
    if (a[3]) st32(a[3], 0x12345678);
    if (a[4]) st32(a[4], 255);
    if (a[5]) st32(a[5], 0);
    if (a[6] && a[7]) { const char *fs = "NTFS"; snprintf((char *)G(a[6]), a[7], "%s", fs); }
    return 1;
}
H(ex_RemoveDirectoryA) { UNUSED; return 1; }

/* I/O completion ports: the client's networking uses them. Stubbed so creation
 * succeeds; the socket layer does not yet post completions (TODO for networking). */
H(ex_CreateIoCompletionPort) { UNUSED; return 0xC0DE0300u; }
H(ex_GetQueuedCompletionStatus) {
    UNUSED;
    rt_set_last_error(c, 258); /* WAIT_TIMEOUT */
    return 0;
}
H(ex_PostQueuedCompletionStatus) { UNUSED; return 1; }

/* ------------------------------------------------------------------ ole32, shell32, imm32, advapi32 */
H(ex_OleInitialize) { UNUSED; return 0; }   /* S_OK */
H(ex_OleUninitialize) { UNUSED; return 0; }
H(ex_SHGetSpecialFolderPathA) {
    UNUSED; /* (hwnd, path, csidl, create): a plausible folder */
    if (a[1]) snprintf((char *)G(a[1]), 260, "C:\\Nexon\\MapleStory");
    return 1;
}
H(ex_ImmCreateContext) { UNUSED; return 0xC0DE0400u; }
H(ex_ImmDestroyContext) { UNUSED; return 1; }
H(ex_ImmGetProperty) { UNUSED; return 0; }
H(ex_ImmGetCandidateWindow) { UNUSED; return 0; }
H(ex_ImmSetCandidateWindow) { UNUSED; return 0; }

static const ImportDef defs[] = {
    {"user32.dll", "CopyRect", ex_CopyRect, 2, 0},
    {"user32.dll", "InflateRect", ex_InflateRect, 3, 0},
    {"user32.dll", "UnionRect", ex_UnionRect, 3, 0},
    {"user32.dll", "GetDoubleClickTime", ex_GetDoubleClickTime, 0, 0},
    {"user32.dll", "GetWindowTextLengthA", ex_GetWindowTextLength, 1, 0},
    {"user32.dll", "GetWindowTextLengthW", ex_GetWindowTextLength, 1, 0},
    {"user32.dll", "CharPrevA", ex_CharPrevA, 2, 0},
    {"user32.dll", "InSendMessage", ex_InSendMessage, 0, 0},
    {"user32.dll", "SendDlgItemMessageA", ex_SendDlgItemMessageA, 5, 0},
    {"user32.dll", "DrawTextA", ex_DrawTextA, 5, 0},
    {"user32.dll", "SetWindowsHookExA", ex_SetWindowsHookEx, 4, 0},
    {"user32.dll", "UnhookWindowsHookEx", ex_UnhookWindowsHookEx, 1, 0},
    {"user32.dll", "CallNextHookEx", ex_CallNextHookEx, 4, 0},
    {"user32.dll", "MsgWaitForMultipleObjects", ex_MsgWaitForMultipleObjects, 5, 0},

    {"kernel32.dll", "OpenFileMappingA", ex_OpenFileMappingA, 3, 0},
    {"kernel32.dll", "UnmapViewOfFile", ex_UnmapViewOfFile, 1, 0},
    {"kernel32.dll", "FlushViewOfFile", ex_FlushViewOfFile, 2, 0},
    {"kernel32.dll", "OpenProcess", ex_OpenProcess, 3, 0},
    {"kernel32.dll", "WinExec", ex_WinExec, 2, 0},
    {"kernel32.dll", "CompareFileTime", ex_CompareFileTime, 2, 0},
    {"kernel32.dll", "GetVolumeInformationA", ex_GetVolumeInformationA, 8, 0},
    {"kernel32.dll", "RemoveDirectoryA", ex_RemoveDirectoryA, 1, 0},
    {"kernel32.dll", "CreateIoCompletionPort", ex_CreateIoCompletionPort, 4, 0},
    {"kernel32.dll", "GetQueuedCompletionStatus", ex_GetQueuedCompletionStatus, 5, 0},
    {"kernel32.dll", "PostQueuedCompletionStatus", ex_PostQueuedCompletionStatus, 4, 0},

    {"ole32.dll", "OleInitialize", ex_OleInitialize, 1, 0},
    {"ole32.dll", "OleUninitialize", ex_OleUninitialize, 0, 0},
    {"shell32.dll", "SHGetSpecialFolderPathA", ex_SHGetSpecialFolderPathA, 4, 0},
    {"imm32.dll", "ImmCreateContext", ex_ImmCreateContext, 0, 0},
    {"imm32.dll", "ImmDestroyContext", ex_ImmDestroyContext, 1, 0},
    {"imm32.dll", "ImmGetProperty", ex_ImmGetProperty, 2, 0},
    {"imm32.dll", "ImmGetCandidateWindow", ex_ImmGetCandidateWindow, 2, 0},
    {"imm32.dll", "ImmSetCandidateWindow", ex_ImmSetCandidateWindow, 2, 0},
};
RT_EXPORT_IMPORTS(rt_imports_extra, defs);

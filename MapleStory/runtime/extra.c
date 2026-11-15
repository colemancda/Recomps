/* MapleStory: Win32 functions the client resolves at run time with GetProcAddress
 * and calls through stored pointers. A slot left null (an unimplemented name)
 * faults when called, so these are provided here. Correctness-sensitive ones
 * (RECT maths, file mapping) are real; the rest are benign stubs that let the
 * client proceed, to be fleshed out when their effect is reached. */
#include <stdio.h>
#include <string.h>

#include "rt.h"

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
H(ex_SetWindowsHookEx) { UNUSED; return 0xC0DE0100u; }
H(ex_UnhookWindowsHookEx) { UNUSED; return 1; }
H(ex_CallNextHookEx) { UNUSED; return 0; }
H(ex_MsgWaitForMultipleObjects) { UNUSED; return 0x102; } /* WAIT_TIMEOUT */

/* ------------------------------------------------------------------ kernel32: file mapping */
/* Shared memory. The client uses a named mapping for single-instance detection
 * and small shared buffers. Backed by guest heap; names are not tracked across
 * processes (there is only one process), so OpenFileMapping reports absent. */
H(ex_CreateFileMappingA) {
    UNUSED; /* (hFile, sa, protect, sizeHigh, sizeLow, name) */
    uint32_t size = a[4] ? a[4] : 0x1000;
    uint32_t mem = rt_heap_alloc(size, 1);
    return mem ? mem : 0;
}
H(ex_OpenFileMappingA) {
    UNUSED;
    rt_set_last_error(c, 2); /* ERROR_FILE_NOT_FOUND: no existing mapping */
    return 0;
}
H(ex_MapViewOfFile) {
    UNUSED; /* (hMapping, access, offHigh, offLow, bytes): the handle is the memory */
    return a[0] + a[3];
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

    {"kernel32.dll", "CreateFileMappingA", ex_CreateFileMappingA, 6, 0},
    {"kernel32.dll", "OpenFileMappingA", ex_OpenFileMappingA, 3, 0},
    {"kernel32.dll", "MapViewOfFile", ex_MapViewOfFile, 5, 0},
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

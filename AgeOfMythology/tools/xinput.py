#!/usr/bin/env python3
"""Scripted X11 input for reference runs under Wine on Xvfb (no xdotool needed).

  DISPLAY=:99 tools/xinput.py STEP [STEP ...]

Steps:
  move X Y          move the pointer to root coordinates
  click X Y [B]     move there and click button B (default 1)
  key NAME          press and release a keysym (e.g. Return, a, Escape, alt+a)
  type TEXT         type plain ASCII text
  raise WINDOW_ID   raise and focus a window (hex id from xwininfo -root -tree)
  sleep SECONDS     wait

Uses libX11 and the XTEST extension (libXtst) through ctypes.
"""
import ctypes
import ctypes.util
import os
import sys
import time

x11 = ctypes.CDLL(ctypes.util.find_library("X11") or "libX11.so.6")
xtst = ctypes.CDLL(ctypes.util.find_library("Xtst") or "libXtst.so.6")
x11.XOpenDisplay.restype = ctypes.c_void_p
x11.XOpenDisplay.argtypes = [ctypes.c_char_p]
x11.XStringToKeysym.restype = ctypes.c_ulong
x11.XStringToKeysym.argtypes = [ctypes.c_char_p]
x11.XKeysymToKeycode.restype = ctypes.c_ubyte
x11.XKeysymToKeycode.argtypes = [ctypes.c_void_p, ctypes.c_ulong]
x11.XFlush.argtypes = [ctypes.c_void_p]
x11.XSync.argtypes = [ctypes.c_void_p, ctypes.c_int]
x11.XRaiseWindow.argtypes = [ctypes.c_void_p, ctypes.c_ulong]
x11.XSetInputFocus.argtypes = [ctypes.c_void_p, ctypes.c_ulong, ctypes.c_int, ctypes.c_ulong]
xtst.XTestFakeMotionEvent.argtypes = [ctypes.c_void_p, ctypes.c_int, ctypes.c_int, ctypes.c_int, ctypes.c_ulong]
xtst.XTestFakeButtonEvent.argtypes = [ctypes.c_void_p, ctypes.c_uint, ctypes.c_int, ctypes.c_ulong]
xtst.XTestFakeKeyEvent.argtypes = [ctypes.c_void_p, ctypes.c_uint, ctypes.c_int, ctypes.c_ulong]

MODS = {"alt": "Alt_L", "ctrl": "Control_L", "shift": "Shift_L"}


def main(argv):
    dpy = x11.XOpenDisplay(os.environ.get("DISPLAY", ":99").encode())
    if not dpy:
        sys.exit("cannot open display")

    def flush(pause=0.05):
        x11.XSync(dpy, 0)
        time.sleep(pause)

    def keycode(name):
        ks = x11.XStringToKeysym(name.encode())
        if not ks:
            sys.exit("unknown keysym %r" % name)
        return x11.XKeysymToKeycode(dpy, ks)

    def tap(combo):
        parts = combo.split("+")
        codes = [keycode(MODS.get(p, p)) for p in parts]
        for k in codes:
            xtst.XTestFakeKeyEvent(dpy, k, 1, 0)
            flush(0.02)
        for k in reversed(codes):
            xtst.XTestFakeKeyEvent(dpy, k, 0, 0)
            flush(0.02)

    i = 0
    while i < len(argv):
        op = argv[i]
        if op == "move":
            xtst.XTestFakeMotionEvent(dpy, -1, int(argv[i + 1]), int(argv[i + 2]), 0)
            flush()
            i += 3
        elif op == "click":
            x, y = int(argv[i + 1]), int(argv[i + 2])
            b = 1
            i += 3
            if i < len(argv) and argv[i].isdigit():
                b = int(argv[i])
                i += 1
            xtst.XTestFakeMotionEvent(dpy, -1, x, y, 0)
            flush(0.1)
            xtst.XTestFakeButtonEvent(dpy, b, 1, 0)
            flush(0.08)
            xtst.XTestFakeButtonEvent(dpy, b, 0, 0)
            flush()
        elif op == "key":
            tap(argv[i + 1])
            i += 2
        elif op == "type":
            for ch in argv[i + 1]:
                name = {" ": "space", ".": "period", "-": "minus", "_": "underscore"}.get(ch, ch)
                if ch.isupper():
                    tap("shift+" + ch.lower())
                else:
                    tap(name)
            i += 2
        elif op == "raise":
            w = int(argv[i + 1], 16)
            x11.XRaiseWindow(dpy, w)
            x11.XSetInputFocus(dpy, w, 1, 0)  # RevertToPointerRoot, CurrentTime
            flush(0.2)
            i += 2
        elif op == "sleep":
            time.sleep(float(argv[i + 1]))
            i += 2
        else:
            sys.exit("unknown step %r" % op)


if __name__ == "__main__":
    main(sys.argv[1:])

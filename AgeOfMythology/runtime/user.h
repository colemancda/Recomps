/* user32 services used by other parts of the runtime. */
#ifndef RECOMP_USER_H
#define RECOMP_USER_H

#include <stdint.h>

/* Post a message to a window's queue from any host thread. Returns 0 if the window is gone. */
int rt_post_message(uint32_t hwnd, uint32_t msg, uint32_t wparam, uint32_t lparam);
/* The display mode, as a full-screen Direct3D device sets it (d3d8.c). */
void rt_user_set_display_mode(int w, int h, int bpp);
/* A full-screen Direct3D device on window hwnd: the display takes its mode and the
 * window covers the screen, with WM_SIZE, as Windows does in CreateDevice/Reset. */
void rt_user_fullscreen_window(CPU *c, uint32_t hwnd, int w, int h, int bpp);
/* The game's main window (0 before it is shown). */
uint32_t rt_main_window(void);

#endif

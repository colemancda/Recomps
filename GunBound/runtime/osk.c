/* On-screen keyboard on a second screen.
 *
 * Handhelds with two screens (the Anbernic RG DS: two 640x480 panels, both with touch) leave
 * the lower one with nothing to do while the game fills the upper. This opens a second window
 * there with a touch keyboard, so that text (chat, room names) can be typed without a keyboard.
 * Taps become the same input a keyboard gives the game: rt_in_char for characters, rt_in_key for
 * Backspace, Enter, Esc and Tab.
 *
 * Detection: two connected internal panels (DSI, eDP, LVDS) in /sys/class/drm, or two SDL
 * displays. --second-screen on makes a window anyway (to try it on one screen), off never.
 *
 * Placement: the window's title contains "Bottom". ROCKNIX's sway configuration moves windows
 * with such a title (Secondary, Sub, Bottom, Screen 2, GamePad) to the second output and powers
 * it on, as it does for DraStic; when SDL lists two displays the window is also created there.
 *
 * Everything runs on the video thread, from video.c's event loop. */
#ifndef RT_NO_SDL

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL.h>

#include "pad.h"
#include "rt.h"
#include "video.h"

int rt_osk_mode;           /* 0 automatic, 1 always, -1 never (--second-screen) */
char rt_osk_dump_path[512]; /* where the kdump input command writes the keyboard window (--dump-osk) */

/* ------------------------------------------------------------------ font
 * 5x7, one byte per column, bit 0 at the top; the characters 32 to 126. */
static const uint8_t font5x7[95][5] = {
    {0x00, 0x00, 0x00, 0x00, 0x00}, {0x00, 0x00, 0x5F, 0x00, 0x00}, {0x00, 0x07, 0x00, 0x07, 0x00},
    {0x14, 0x7F, 0x14, 0x7F, 0x14}, {0x24, 0x2A, 0x7F, 0x2A, 0x12}, {0x23, 0x13, 0x08, 0x64, 0x62},
    {0x36, 0x49, 0x56, 0x20, 0x50}, {0x00, 0x08, 0x07, 0x03, 0x00}, {0x00, 0x1C, 0x22, 0x41, 0x00},
    {0x00, 0x41, 0x22, 0x1C, 0x00}, {0x2A, 0x1C, 0x7F, 0x1C, 0x2A}, {0x08, 0x08, 0x3E, 0x08, 0x08},
    {0x00, 0x80, 0x70, 0x30, 0x00}, {0x08, 0x08, 0x08, 0x08, 0x08}, {0x00, 0x00, 0x60, 0x60, 0x00},
    {0x20, 0x10, 0x08, 0x04, 0x02}, {0x3E, 0x51, 0x49, 0x45, 0x3E}, {0x00, 0x42, 0x7F, 0x40, 0x00},
    {0x72, 0x49, 0x49, 0x49, 0x46}, {0x21, 0x41, 0x49, 0x4D, 0x33}, {0x18, 0x14, 0x12, 0x7F, 0x10},
    {0x27, 0x45, 0x45, 0x45, 0x39}, {0x3C, 0x4A, 0x49, 0x49, 0x31}, {0x41, 0x21, 0x11, 0x09, 0x07},
    {0x36, 0x49, 0x49, 0x49, 0x36}, {0x46, 0x49, 0x49, 0x29, 0x1E}, {0x00, 0x00, 0x14, 0x00, 0x00},
    {0x00, 0x40, 0x34, 0x00, 0x00}, {0x00, 0x08, 0x14, 0x22, 0x41}, {0x14, 0x14, 0x14, 0x14, 0x14},
    {0x00, 0x41, 0x22, 0x14, 0x08}, {0x02, 0x01, 0x59, 0x09, 0x06}, {0x3E, 0x41, 0x5D, 0x59, 0x4E},
    {0x7C, 0x12, 0x11, 0x12, 0x7C}, {0x7F, 0x49, 0x49, 0x49, 0x36}, {0x3E, 0x41, 0x41, 0x41, 0x22},
    {0x7F, 0x41, 0x41, 0x41, 0x3E}, {0x7F, 0x49, 0x49, 0x49, 0x41}, {0x7F, 0x09, 0x09, 0x09, 0x01},
    {0x3E, 0x41, 0x41, 0x51, 0x73}, {0x7F, 0x08, 0x08, 0x08, 0x7F}, {0x00, 0x41, 0x7F, 0x41, 0x00},
    {0x20, 0x40, 0x41, 0x3F, 0x01}, {0x7F, 0x08, 0x14, 0x22, 0x41}, {0x7F, 0x40, 0x40, 0x40, 0x40},
    {0x7F, 0x02, 0x1C, 0x02, 0x7F}, {0x7F, 0x04, 0x08, 0x10, 0x7F}, {0x3E, 0x41, 0x41, 0x41, 0x3E},
    {0x7F, 0x09, 0x09, 0x09, 0x06}, {0x3E, 0x41, 0x51, 0x21, 0x5E}, {0x7F, 0x09, 0x19, 0x29, 0x46},
    {0x26, 0x49, 0x49, 0x49, 0x32}, {0x03, 0x01, 0x7F, 0x01, 0x03}, {0x3F, 0x40, 0x40, 0x40, 0x3F},
    {0x1F, 0x20, 0x40, 0x20, 0x1F}, {0x3F, 0x40, 0x38, 0x40, 0x3F}, {0x63, 0x14, 0x08, 0x14, 0x63},
    {0x03, 0x04, 0x78, 0x04, 0x03}, {0x61, 0x59, 0x49, 0x4D, 0x43}, {0x00, 0x7F, 0x41, 0x41, 0x41},
    {0x02, 0x04, 0x08, 0x10, 0x20}, {0x00, 0x41, 0x41, 0x41, 0x7F}, {0x04, 0x02, 0x01, 0x02, 0x04},
    {0x40, 0x40, 0x40, 0x40, 0x40}, {0x00, 0x03, 0x07, 0x08, 0x00}, {0x20, 0x54, 0x54, 0x78, 0x40},
    {0x7F, 0x28, 0x44, 0x44, 0x38}, {0x38, 0x44, 0x44, 0x44, 0x28}, {0x38, 0x44, 0x44, 0x28, 0x7F},
    {0x38, 0x54, 0x54, 0x54, 0x18}, {0x00, 0x08, 0x7E, 0x09, 0x02}, {0x18, 0xA4, 0xA4, 0x9C, 0x78},
    {0x7F, 0x08, 0x04, 0x04, 0x78}, {0x00, 0x44, 0x7D, 0x40, 0x00}, {0x20, 0x40, 0x40, 0x3D, 0x00},
    {0x7F, 0x10, 0x28, 0x44, 0x00}, {0x00, 0x41, 0x7F, 0x40, 0x00}, {0x7C, 0x04, 0x78, 0x04, 0x78},
    {0x7C, 0x08, 0x04, 0x04, 0x78}, {0x38, 0x44, 0x44, 0x44, 0x38}, {0xFC, 0x18, 0x24, 0x24, 0x18},
    {0x18, 0x24, 0x24, 0x18, 0xFC}, {0x7C, 0x08, 0x04, 0x04, 0x08}, {0x48, 0x54, 0x54, 0x54, 0x24},
    {0x04, 0x04, 0x3F, 0x44, 0x24}, {0x3C, 0x40, 0x40, 0x20, 0x7C}, {0x1C, 0x20, 0x40, 0x20, 0x1C},
    {0x3C, 0x40, 0x30, 0x40, 0x3C}, {0x44, 0x28, 0x10, 0x28, 0x44}, {0x4C, 0x90, 0x90, 0x90, 0x7C},
    {0x44, 0x64, 0x54, 0x4C, 0x44}, {0x00, 0x08, 0x36, 0x41, 0x00}, {0x00, 0x00, 0x77, 0x00, 0x00},
    {0x00, 0x41, 0x36, 0x08, 0x00}, {0x02, 0x01, 0x02, 0x04, 0x02},
};

/* ---------------------------------------------------------------- layout
 * Drawn on a 640x480 canvas, fitted into the window. */
#define CANVAS_W 640
#define CANVAS_H 480

enum { K_CHAR, K_SHIFT, K_BKSP, K_SYM, K_SPACE, K_ENTER, K_ESC, K_TAB };
enum { L_LOWER, L_UPPER, L_SYMBOLS };

typedef struct {
    int kind;
    int ch;          /* K_CHAR */
    char label[8];
    int x, y, w, h;  /* canvas pixels */
} Key;

static Key keys[64];
static int nkeys;
static int layer = L_LOWER;
static int caps;      /* shift pressed twice: stays upper */
static int shifted;   /* the next character is upper */

static const char *const rows[3][3] = {
    /* lower   */ {"1234567890", "qwertyuiop", "asdfghjkl'"},
    /* upper   */ {"!@#$%^&*()", "QWERTYUIOP", "ASDFGHJKL\""},
    /* symbols */ {"1234567890", "-_=+[]{}/\\", "@#$%&*()\";"},
};
static const char *const row4[3] = {"zxcvbnm", "ZXCVBNM", "!?:;<>|"};
static const char row5[3][2] = {{',', '.'}, {';', ':'}, {',', '.'}};

static void add_key(int kind, int ch, const char *label, int x, int y, int w, int h) {
    if (nkeys >= (int)(sizeof keys / sizeof keys[0])) return;
    Key *k = &keys[nkeys++];
    k->kind = kind;
    k->ch = ch;
    snprintf(k->label, sizeof k->label, "%s", label);
    k->x = x, k->y = y, k->w = w, k->h = h;
}

static void build_layout(void) {
    nkeys = 0;
    /* the top row: the preview field takes the left (drawn separately), Esc and Tab the right */
    add_key(K_ESC, 0, "Esc", 486, 6, 70, 52);
    add_key(K_TAB, 0, "Tab", 562, 6, 70, 52);
    const int y0 = 66, rh = 74, pitch = 80;
    for (int r = 0; r < 3; r++)
        for (int i = 0; i < 10; i++) {
            char l[2] = {rows[layer][r][i], 0};
            add_key(K_CHAR, rows[layer][r][i], l, i * 64, y0 + r * pitch, 64, rh);
        }
    int y = y0 + 3 * pitch;
    add_key(K_SHIFT, 0, "Shift", 0, y, 96, rh);
    for (int i = 0; i < 7; i++) {
        char l[2] = {row4[layer][i], 0};
        add_key(K_CHAR, row4[layer][i], l, 96 + i * 64, y, 64, rh);
    }
    add_key(K_BKSP, 0, "Bksp", 544, y, 96, rh);
    y = y0 + 4 * pitch;
    add_key(K_SYM, 0, layer == L_SYMBOLS ? "abc" : "?123", 0, y, 96, rh);
    char c1[2] = {row5[layer][0], 0}, c2[2] = {row5[layer][1], 0};
    add_key(K_CHAR, c1[0], c1, 96, y, 64, rh);
    add_key(K_SPACE, ' ', "Space", 160, y, 288, rh);
    add_key(K_CHAR, c2[0], c2, 448, y, 64, rh);
    add_key(K_ENTER, 0, "Enter", 512, y, 128, rh);
}

/* ------------------------------------------------------------------ state */
static SDL_Window *kwin;
static SDL_Renderer *krend;
static SDL_Window *game_win;
static int out_w, out_h;
static int off_x, off_y; /* where the canvas starts in the window, in canvas pixels (the fit's margin) */
static int dirty = 1;
static int pressed = -1;      /* key under the finger */
static unsigned press_at, repeat_at;
static char typed[128];       /* what has been typed, for the preview field */
static int ntyped;
static unsigned last_shift_tap;

int osk_active(void) { return kwin != NULL; }

/* ---------------------------------------------------------------- drawing */
static void fill(int x, int y, int w, int h, int r, int g, int b) {
    SDL_SetRenderDrawColor(krend, (Uint8)r, (Uint8)g, (Uint8)b, 255);
    SDL_Rect rc = {x + off_x, y + off_y, w, h};
    SDL_RenderFillRect(krend, &rc);
}

static int text_width(const char *s, int scale) {
    int n = (int)strlen(s);
    return n ? n * 6 * scale - scale : 0;
}

/* Draws text with its top left at x, y (canvas pixels, scaled by the caller's origin). */
static void draw_text(const char *s, int x, int y, int scale, int r, int g, int b) {
    SDL_SetRenderDrawColor(krend, (Uint8)r, (Uint8)g, (Uint8)b, 255);
    for (; *s; s++, x += 6 * scale) {
        int c = (unsigned char)*s;
        if (c < 32 || c > 126) continue;
        const uint8_t *col = font5x7[c - 32];
        for (int cx = 0; cx < 5; cx++)
            for (int cy = 0; cy < 8; cy++)
                if (col[cx] & (1 << cy)) {
                    SDL_Rect px = {off_x + x + cx * scale, off_y + y + cy * scale, scale, scale};
                    SDL_RenderFillRect(krend, &px);
                }
    }
}

static void draw_centered(const char *s, int x, int y, int w, int h, int r, int g, int b) {
    int scale = 4;
    while (scale > 1 && (text_width(s, scale) > w - 14 || 7 * scale > h - 10)) scale--;
    draw_text(s, x + (w - text_width(s, scale)) / 2, y + (h - 7 * scale) / 2, scale, r, g, b);
}

static void draw_all(void) {
    SDL_GetRendererOutputSize(krend, &out_w, &out_h);
    /* fit the canvas into the window, keeping its proportions */
    float s = (float)out_w / CANVAS_W < (float)out_h / CANVAS_H ? (float)out_w / CANVAS_W : (float)out_h / CANVAS_H;
    SDL_SetRenderDrawColor(krend, 22, 26, 38, 255);
    SDL_RenderClear(krend);
    SDL_RenderSetScale(krend, s, s);
    off_x = (int)(((float)out_w - CANVAS_W * s) / 2 / s);
    off_y = (int)(((float)out_h - CANVAS_H * s) / 2 / s);
    fill(0, 0, CANVAS_W, CANVAS_H, 22, 26, 38);

    /* the preview field: what has been typed (the last characters) */
    fill(8, 6, 470, 52, 12, 14, 22);
    char shown[40];
    int first = ntyped > 24 ? ntyped - 24 : 0;
    snprintf(shown, sizeof shown, "%.*s_", ntyped - first, typed + first);
    draw_text(shown, 16, 20, 3, 235, 235, 245);

    for (int i = 0; i < nkeys; i++) {
        const Key *k = &keys[i];
        int special = k->kind != K_CHAR && k->kind != K_SPACE;
        int lit = i == pressed || (k->kind == K_SHIFT && (shifted || caps));
        int r = special ? 44 : 62, g = special ? 52 : 70, b = special ? 78 : 98;
        if (lit) r = i == pressed ? 118 : 70, g = i == pressed ? 158 : 150, b = i == pressed ? 232 : 110;
        fill(k->x + 3, k->y + 3, k->w - 6, k->h - 6, r + 26, g + 26, b + 26);
        fill(k->x + 5, k->y + 5, k->w - 10, k->h - 10, r, g, b);
        draw_centered(k->label, k->x, k->y, k->w, k->h, 240, 240, 250);
    }
    SDL_RenderSetScale(krend, 1.0f, 1.0f);
}

void osk_present(void) {
    if (!kwin || !dirty) return;
    dirty = 0;
    draw_all();
    SDL_RenderPresent(krend);
}

/* Writes the keyboard window's contents (the kdump input command, for tests). */
void osk_dump(void) {
    if (!kwin) return;
    draw_all();
    const char *path = rt_osk_dump_path[0] ? rt_osk_dump_path : "osk.png";
    uint8_t *rgb = malloc((size_t)out_w * (size_t)out_h * 3);
    if (!rgb) return;
    if (SDL_RenderReadPixels(krend, NULL, SDL_PIXELFORMAT_RGB24, rgb, out_w * 3) == 0 && rt_write_png(path, rgb, out_w, out_h))
        rt_log("recomp: keyboard window written to %s (%dx%d)\n", path, out_w, out_h);
    else
        rt_log("recomp: could not write the keyboard window to %s (%s)\n", path, SDL_GetError());
    free(rgb);
    dirty = 1;
}

/* ------------------------------------------------------------------ input */
static void type_char(int ch) {
    if (g_opt.verbose) rt_log("[osk] char %d '%c'\n", ch, ch >= 32 && ch < 127 ? ch : '?');
    rt_in_char(ch);
    if (ntyped < (int)sizeof typed - 1) typed[ntyped++] = (char)ch;
    typed[ntyped] = 0;
}

static void type_key(int vk, int dik) {
    if (g_opt.verbose) rt_log("[osk] key vk %#x\n", vk);
    rt_in_key(vk, dik, 1, 0);
    rt_in_key(vk, dik, 0, 0);
}

static void set_layer(int l) {
    layer = l;
    build_layout();
}

static void press_key(int i) {
    const Key *k = &keys[i];
    switch (k->kind) {
    case K_CHAR:
    case K_SPACE:
        type_char(k->ch);
        if (shifted && !caps) {
            shifted = 0;
            if (layer == L_UPPER) set_layer(L_LOWER);
        }
        break;
    case K_SHIFT: {
        unsigned now = SDL_GetTicks();
        if (layer == L_SYMBOLS) set_layer(L_LOWER);
        if (layer == L_UPPER && !caps && shifted && now - last_shift_tap < 450) caps = 1; /* a second tap locks */
        else if (layer == L_UPPER) shifted = caps = 0, set_layer(L_LOWER);
        else shifted = 1, caps = 0, set_layer(L_UPPER);
        last_shift_tap = now;
        break;
    }
    case K_SYM:
        shifted = caps = 0;
        set_layer(layer == L_SYMBOLS ? L_LOWER : L_SYMBOLS);
        break;
    case K_BKSP:
        type_key(0x08, 0x0E);
        if (ntyped > 0) typed[--ntyped] = 0;
        break;
    case K_ENTER:
        type_key(0x0D, 0x1C);
        ntyped = 0;
        typed[0] = 0;
        break;
    case K_ESC: type_key(0x1B, 0x01); break;
    case K_TAB: type_key(0x09, 0x0F); break;
    }
    dirty = 1;
}

/* A touch or click at canvas coordinates. */
static void tap_down(int cx, int cy) {
    for (int i = 0; i < nkeys; i++) {
        const Key *k = &keys[i];
        if (cx >= k->x && cx < k->x + k->w && cy >= k->y && cy < k->y + k->h) {
            pressed = i;
            press_at = SDL_GetTicks();
            repeat_at = press_at + 450;
            press_key(i);
            dirty = 1;
            return;
        }
    }
}

static void tap_up(void) {
    if (pressed >= 0) dirty = 1;
    pressed = -1;
}

/* Window-normalised position (0..1) to canvas pixels, undoing the fit. */
static void to_canvas(float nx, float ny, int *cx, int *cy) {
    SDL_GetRendererOutputSize(krend, &out_w, &out_h);
    float s = (float)out_w / CANVAS_W < (float)out_h / CANVAS_H ? (float)out_w / CANVAS_W : (float)out_h / CANVAS_H;
    float px = nx * (float)out_w, py = ny * (float)out_h;
    *cx = (int)((px - ((float)out_w - CANVAS_W * s) / 2) / s);
    *cy = (int)((py - ((float)out_h - CANVAS_H * s) / 2) / s);
}

/* The input script's ktap: x, y in thousandths of the window, a press and release. */
void osk_tap(int x1000, int y1000) {
    if (!kwin) return;
    int cx, cy;
    to_canvas((float)x1000 / 1000.0f, (float)y1000 / 1000.0f, &cx, &cy);
    tap_down(cx, cy);
    tap_up();
}

/* Backspace held down repeats. Once per pass of the video thread's event loop. */
void osk_tick(void) {
    if (!kwin || pressed < 0 || keys[pressed].kind != K_BKSP) return;
    unsigned now = SDL_GetTicks();
    if (now >= repeat_at) {
        repeat_at = now + 70;
        type_key(0x08, 0x0E);
        if (ntyped > 0) typed[--ntyped] = 0;
        dirty = 1;
    }
}

/* An event of the keyboard window is consumed here (1); others are the game's (0). */
int osk_handle_event(const SDL_Event *e) {
    if (!kwin) return 0;
    Uint32 id = SDL_GetWindowID(kwin);
    switch (e->type) {
    case SDL_WINDOWEVENT:
        if (e->window.windowID != id) return 0;
        if (e->window.event == SDL_WINDOWEVENT_EXPOSED || e->window.event == SDL_WINDOWEVENT_SIZE_CHANGED ||
            e->window.event == SDL_WINDOWEVENT_RESIZED)
            dirty = 1;
        return 1; /* its close button does not end the game */
    case SDL_FINGERDOWN:
    case SDL_FINGERUP:
        if (e->tfinger.windowID != id) return 0;
        if (e->type == SDL_FINGERDOWN) {
            int cx, cy;
            to_canvas(e->tfinger.x, e->tfinger.y, &cx, &cy);
            tap_down(cx, cy);
        } else tap_up();
        return 1;
    case SDL_FINGERMOTION:
        return e->tfinger.windowID == id;
    case SDL_MOUSEBUTTONDOWN:
    case SDL_MOUSEBUTTONUP:
        if (e->button.windowID != id) return 0;
        if (e->button.which == SDL_TOUCH_MOUSEID) return 1;
        if (e->type == SDL_MOUSEBUTTONDOWN) {
            int ww = 0, wh = 0, cx, cy;
            SDL_GetWindowSize(kwin, &ww, &wh);
            to_canvas((float)e->button.x / (float)(ww ? ww : 1), (float)e->button.y / (float)(wh ? wh : 1), &cx, &cy);
            tap_down(cx, cy);
        } else tap_up();
        return 1;
    case SDL_MOUSEMOTION:
        return e->motion.windowID == id;
    case SDL_MOUSEWHEEL:
        return e->wheel.windowID == id;
    case SDL_KEYDOWN:
    case SDL_KEYUP:
        return e->key.windowID == id;
    case SDL_TEXTINPUT:
        return e->text.windowID == id;
    default:
        return 0;
    }
}

/* -------------------------------------------------------------- detection */
/* Connected internal panels: the connectors of a dual-screen handheld's two displays. */
static int internal_panels(void) {
    DIR *d = opendir("/sys/class/drm");
    if (!d) return 0;
    int n = 0;
    struct dirent *de;
    while ((de = readdir(d))) {
        if (!strstr(de->d_name, "-DSI-") && !strstr(de->d_name, "-eDP-") && !strstr(de->d_name, "-LVDS-")) continue;
        char path[300], st[32] = "";
        snprintf(path, sizeof path, "/sys/class/drm/%s/status", de->d_name);
        FILE *f = fopen(path, "r");
        if (!f) continue;
        if (fgets(st, sizeof st, f) && !strncmp(st, "connected", 9)) n++;
        fclose(f);
    }
    closedir(d);
    return n;
}

void osk_open(SDL_Window *game) {
    if (kwin || rt_osk_mode < 0) return;
    int displays = SDL_GetNumVideoDisplays(), panels = internal_panels();
    int dual = displays >= 2 || panels >= 2;
    rt_log("recomp: screens: %d SDL display%s, %d internal panel%s: %s\n", displays, displays == 1 ? "" : "s", panels,
           panels == 1 ? "" : "s", dual ? "dual screen" : "single screen");
    if (!dual && rt_osk_mode == 0) return;
    game_win = game;
    int gd = game ? SDL_GetWindowDisplayIndex(game) : 0;
    if (gd < 0) gd = 0;
    int kd = -1;
    for (int i = 0; i < displays; i++)
        if (i != gd) {
            kd = i;
            break;
        }
    int x = SDL_WINDOWPOS_UNDEFINED, y = SDL_WINDOWPOS_UNDEFINED, w = CANVAS_W, h = CANVAS_H;
    Uint32 flags = SDL_WINDOW_SHOWN;
    if (dual) {
        flags |= SDL_WINDOW_FULLSCREEN_DESKTOP;
        if (kd >= 0) {
            x = SDL_WINDOWPOS_CENTERED_DISPLAY(kd);
            y = SDL_WINDOWPOS_CENTERED_DISPLAY(kd);
            SDL_Rect b;
            if (SDL_GetDisplayBounds(kd, &b) == 0) w = b.w, h = b.h;
        }
    }
    /* "Bottom" is what ROCKNIX's sway rule looks for to put a window on the second screen */
    kwin = SDL_CreateWindow("GunBound Keyboard (Bottom)", x, y, w, h, flags);
    if (kwin) krend = SDL_CreateRenderer(kwin, -1, SDL_RENDERER_ACCELERATED);
    if (kwin && !krend) krend = SDL_CreateRenderer(kwin, -1, SDL_RENDERER_SOFTWARE);
    if (!kwin || !krend) {
        rt_log("recomp: cannot create the keyboard window (%s)\n", SDL_GetError());
        if (kwin) SDL_DestroyWindow(kwin);
        kwin = NULL, krend = NULL;
        return;
    }
    build_layout();
    SDL_GetRendererOutputSize(krend, &out_w, &out_h);
    SDL_RendererInfo info;
    memset(&info, 0, sizeof info);
    SDL_GetRendererInfo(krend, &info);
    rt_log("recomp: on-screen keyboard window %dx%d on display %d (renderer %s)\n", out_w, out_h, kd >= 0 ? kd : gd,
           info.name ? info.name : "?");
    dirty = 1;
    if (game) SDL_RaiseWindow(game); /* the game keeps the focus it had */
}

#endif /* RT_NO_SDL */

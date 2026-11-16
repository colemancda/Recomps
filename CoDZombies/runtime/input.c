/* Input: the pointer (touches), delivered to the game's callbacks from
 * s3eDeviceYield as the loader did, and answered when the game polls it.
 *
 * Callbacks (device 6 in the loader's registry, s3ePointerRegister 0x2fe2c),
 * each called as fn(event, userdata):
 *
 *   0 S3E_POINTER_BUTTON_EVENT       { int32 button (0), uint32 pressed, int32 x, int32 y }
 *   1 S3E_POINTER_MOTION_EVENT       { int32 x, int32 y }
 *   2 S3E_POINTER_TOUCH_EVENT        { uint32 touchID, uint32 pressed, int32 x, int32 y }
 *   3 S3E_POINTER_TOUCH_MOTION_EVENT { uint32 touchID, int32 x, int32 y }
 *
 * Touch ids reach the game from 1: it keeps 0 for "no touch" (the sticks
 * take a touch with id 0 for nothing and the game pauses).
 *
 * The game registers 0 (0x16966c) and 1 (0x1696e8) at start; it asks
 * s3ePointerGetInt(S3E_POINTER_MULTI_TOUCH_AVAILABLE) for 2 and 3. Touch
 * events go to 2 and 3 when they are registered; touch 0 also drives the
 * single pointer (events 0 and 1 and the polled state).
 *
 * Polling, in alphabetical stub order (0x2bdbe4-0x2bdc70):
 *
 *   0x2fdb8 GetInt    0: available 1, 1: hide cursor 0, 2: type 2 (touch),
 *                     3: stylus type 2 (finger), 4: multi-touch available 1
 *   0x2f838 GetState(button): up 0, down 1, pressed 2, released 4 (bits;
 *                     5 for a button above 4)
 *   0x2f8c0 GetX, 0x2f874 GetY, 0x2fe2c Register (s3e.c), 0x2fdec SetInt,
 *   0x2fdfc UnRegister, 0x2f7a8 Update (latches the state once a frame)
 *
 * In screen coordinates (SCREEN_W x SCREEN_H).
 *
 * A platform layer calls input_touch() as touches happen. On the host,
 * RT_TAPS scripts taps by frame number, which keeps runs repeatable:
 *
 *   RT_TAPS="300:240,200;900:100,50"   press at frame 300 at (240, 200),
 *                                      release RT_TAP_HOLD (3) frames later
 *   RT_TAPS="3000:400,250-460,250:60"  a drag: press at (400, 250), move
 *                                      to (460, 250) over 60 frames, release
 */
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#include "rt.h"

#define TOUCHES 10
typedef struct { uint8_t kind, pressed, id; int32_t x, y; } Event;   /* kind: 0 press/release, 1 motion */
#define QUEUE 256
static Event queue[QUEUE];
static unsigned head, tail;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;   /* touches come from the UI thread */

void input_touch(int id, int kind, int pressed, int32_t x, int32_t y) {
    if (id < 0 || id >= TOUCHES) return;
    pthread_mutex_lock(&lock);
    if (tail - head < QUEUE) queue[tail++ % QUEUE] = (Event){(uint8_t)kind, (uint8_t)pressed, (uint8_t)id, x, y};
    pthread_mutex_unlock(&lock);
}
void input_pointer(int kind, int pressed, int32_t x, int32_t y) { input_touch(0, kind, pressed, x, y); }

/* ------------------------------------------------------- scripted taps */
typedef struct { uint32_t frame, frames; int32_t x, y, x2, y2; } Tap;   /* frames 0: a tap */
static Tap *taps;
static int ntaps, next_tap = -1;

static void parse_taps(void) {
    next_tap = 0;
    const char *s = getenv("RT_TAPS");
    if (!s) return;
    while (*s) {
        unsigned f, d = 0;
        int x, y, x2, y2, n = 0;
        if (sscanf(s, "%u:%d,%d-%d,%d:%u%n", &f, &x, &y, &x2, &y2, &d, &n) == 6 && n) {
            /* a drag */
        } else if (n = 0, d = 0, sscanf(s, "%u:%d,%d%n", &f, &x, &y, &n) == 3 && n) {
            x2 = x;
            y2 = y;
        } else {
            rt_log("RT_TAPS: can't read \"%s\"", s);
            break;
        }
        taps = realloc(taps, sizeof(Tap) * (size_t)(ntaps + 1));
        taps[ntaps++] = (Tap){f, d, x, y, x2, y2};
        s += n;
        while (*s == ';' || *s == ' ') s++;
    }
    rt_log("RT_TAPS: %d taps", ntaps);
}

static uint32_t cur_frame;
static uint32_t press_frame[TOUCHES];
static uint32_t tap_hold = 3;   /* RT_TAP_HOLD: frames from press to release */

/* Called at each eglSwapBuffers with the number of the frame just finished. */
void input_frame(uint32_t frame) {
    cur_frame = frame;
    if (next_tap < 0) {
        parse_taps();
        const char *h = getenv("RT_TAP_HOLD");
        if (h) tap_hold = (uint32_t)atoi(h);
    }
    for (int k = 0; k < ntaps; k++) {
        Tap *t = &taps[k];
        if (t->frame == frame) {
            rt_log("%s at (%d, %d), frame %u", t->frames ? "drag" : "tap", t->x, t->y, frame);
            input_touch(0, 1, 0, t->x, t->y);
            input_touch(0, 0, 1, t->x, t->y);
        }
        if (t->frames && frame > t->frame && frame <= t->frame + t->frames) {
            uint32_t i = frame - t->frame;
            input_touch(0, 1, 0, t->x + (t->x2 - t->x) * (int32_t)i / (int32_t)t->frames,
                        t->y + (t->y2 - t->y) * (int32_t)i / (int32_t)t->frames);
        }
        if (t->frame + (t->frames ? t->frames : tap_hold) == frame) input_touch(0, 0, 0, t->x2, t->y2);
    }
}

/* -------------------------------------------------------- polled state */
static int32_t ptr_x, ptr_y;
static int ptr_down, ptr_was_down;   /* touch 0 now, and at the last s3ePointerUpdate */
static uint32_t ptr_state;           /* latched by s3ePointerUpdate */

static void s3ePointerUpdate(CPU *c) {
    uint32_t s = ptr_down ? 1 : 0;
    if (ptr_down && !ptr_was_down) s |= 2;
    if (!ptr_down && ptr_was_down) s |= 4;
    ptr_state = s;
    ptr_was_down = ptr_down;
    ret32(c, 0);
}
static void s3ePointerGetState(CPU *c) {
    uint32_t b = arg(c, 0);
    ret32(c, b > 4 ? 5 : b == 0 ? ptr_state : 0);
}
static void s3ePointerGetX(CPU *c) { ret32(c, (uint32_t)ptr_x); }
static void s3ePointerGetY(CPU *c) { ret32(c, (uint32_t)ptr_y); }
static void s3ePointerGetInt(CPU *c) {
    static const int32_t v[5] = {1, 0, 2, 2, 1};
    uint32_t p = arg(c, 0);
    ret32(c, p < 5 ? (uint32_t)v[p] : (uint32_t)-1);
}
static void s3ePointerSetInt(CPU *c) { ret32(c, 0); }
static void s3ePointerUnRegister(CPU *c) { ret32(c, 0); }   /* callbacks stay; the game doesn't drop them */

/* ------------------------------------------------------------- delivery */
static uint32_t event_buf;   /* guest memory for the event structs */

/* From s3eDeviceYield: hand the queued events to the game. A release waits
 * HOLD_FRAMES frames after its press: the game looks at the button once a
 * frame, and doesn't take a quick tap (both in one frame, or in the next) as a
 * press. */
#define HOLD_FRAMES 2
void input_deliver(CPU *c) {
    if (head == tail) return;
    if (!event_buf) event_buf = guest_alloc(16);
    int touch_cbs = callbacks_registered(6, 2) || callbacks_registered(6, 3);
    for (;;) {
        pthread_mutex_lock(&lock);
        int any = head != tail;
        Event e = any ? queue[head % QUEUE] : (Event){0};
        if (any && e.kind == 0 && !e.pressed && cur_frame - press_frame[e.id] < HOLD_FRAMES) any = 0;   /* later */
        if (any) head++;
        pthread_mutex_unlock(&lock);
        if (!any) break;
        if (e.kind == 0 && e.pressed) press_frame[e.id] = cur_frame;
        if (getenv("RT_LOG_TOUCH"))
            rt_log("deliver touch %d kind %d pressed %d at %d,%d, frame %u", e.id, e.kind, e.pressed, e.x, e.y, cur_frame);
        if (touch_cbs) {
            st32(event_buf, e.id + 1u);   /* the game takes touch id 0 for "no touch" */
            if (e.kind == 0) {
                st32(event_buf + 4, e.pressed);
                st32(event_buf + 8, (uint32_t)e.x);
                st32(event_buf + 12, (uint32_t)e.y);
            } else {
                st32(event_buf + 4, (uint32_t)e.x);
                st32(event_buf + 8, (uint32_t)e.y);
            }
            callbacks_fire(c, 6, e.kind ? 3 : 2, event_buf);
        }
        if (e.id != 0) continue;
        ptr_x = e.x;
        ptr_y = e.y;
        if (e.kind == 0) {
            ptr_down = e.pressed;
            st32(event_buf, 0);   /* S3E_POINTER_BUTTON_SELECT */
            st32(event_buf + 4, e.pressed);
            st32(event_buf + 8, (uint32_t)e.x);
            st32(event_buf + 12, (uint32_t)e.y);
        } else {
            st32(event_buf, (uint32_t)e.x);
            st32(event_buf + 4, (uint32_t)e.y);
        }
        callbacks_fire(c, 6, e.kind, event_buf);
    }
}

const ImportImpl g_input_impls[] = {
    {0x0002fdb8, "s3ePointerGetInt", s3ePointerGetInt},
    {0x0002f838, "s3ePointerGetState", s3ePointerGetState},
    {0x0002f8c0, "s3ePointerGetX", s3ePointerGetX},
    {0x0002f874, "s3ePointerGetY", s3ePointerGetY},
    {0x0002fdec, "s3ePointerSetInt", s3ePointerSetInt},
    {0x0002fdfc, "s3ePointerUnRegister", s3ePointerUnRegister},
    {0x0002f7a8, "s3ePointerUpdate", s3ePointerUpdate},
};
const uint32_t g_input_impl_count = sizeof g_input_impls / sizeof g_input_impls[0];

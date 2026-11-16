/* Input: pointer (touch) events, delivered to the game's callbacks from
 * s3eDeviceYield as the loader did.
 *
 * The game registers two s3ePointer callbacks (device 6 in the loader's
 * registry, through 0x2fe2c): id 0, S3E_POINTER_BUTTON_EVENT, at 0x16966c and
 * id 1, S3E_POINTER_MOTION_EVENT, at 0x1696e8. Each is called as
 * fn(event, userdata) with
 *
 *   button: { int32 button (0, the only one it handles), uint32 pressed, int32 x, int32 y }
 *   motion: { int32 x, int32 y }
 *
 * in screen coordinates (SCREEN_W x SCREEN_H). It doesn't poll the pointer.
 *
 * A platform layer calls input_pointer() as touches happen. On the host,
 * RT_TAPS scripts them by frame number, which keeps runs repeatable:
 *
 *   RT_TAPS="300:240,200;900:100,50"   press at frame 300 at (240, 200),
 *                                      release three frames later, and so on
 */
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#include "rt.h"

typedef struct { uint8_t kind, pressed; int32_t x, y; } Event;   /* kind: 0 button, 1 motion */
#define QUEUE 256
static Event queue[QUEUE];
static unsigned head, tail;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;   /* touches come from the UI thread */

void input_pointer(int kind, int pressed, int32_t x, int32_t y) {
    pthread_mutex_lock(&lock);
    if (tail - head < QUEUE) queue[tail++ % QUEUE] = (Event){(uint8_t)kind, (uint8_t)pressed, x, y};
    pthread_mutex_unlock(&lock);
}

/* ------------------------------------------------------- scripted taps */
typedef struct { uint32_t frame; int32_t x, y; } Tap;
static Tap *taps;
static int ntaps, next_tap = -1;

static void parse_taps(void) {
    next_tap = 0;
    const char *s = getenv("RT_TAPS");
    if (!s) return;
    while (*s) {
        unsigned f;
        int x, y, n = 0;
        if (sscanf(s, "%u:%d,%d%n", &f, &x, &y, &n) != 3 || n == 0) {
            rt_log("RT_TAPS: can't read \"%s\"", s);
            break;
        }
        taps = realloc(taps, sizeof(Tap) * (size_t)(ntaps + 1));
        taps[ntaps++] = (Tap){f, x, y};
        s += n;
        while (*s == ';' || *s == ' ') s++;
    }
    rt_log("RT_TAPS: %d taps", ntaps);
}

static uint32_t cur_frame, press_frame = 0xffffffffu;

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
        if (taps[k].frame == frame) {
            rt_log("tap at (%d, %d), frame %u", taps[k].x, taps[k].y, frame);
            input_pointer(1, 0, taps[k].x, taps[k].y);
            input_pointer(0, 1, taps[k].x, taps[k].y);
        }
        if (taps[k].frame + tap_hold == frame) {
            input_pointer(0, 0, taps[k].x, taps[k].y);
        }
    }
}

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
    for (;;) {
        pthread_mutex_lock(&lock);
        int any = head != tail;
        Event e = any ? queue[head % QUEUE] : (Event){0};
        if (any && e.kind == 0 && !e.pressed && cur_frame - press_frame < HOLD_FRAMES) any = 0;   /* later */
        if (any) head++;
        pthread_mutex_unlock(&lock);
        if (!any) break;
        if (e.kind == 0 && e.pressed) press_frame = cur_frame;
        if (getenv("RT_LOG_TOUCH")) rt_log("deliver kind %d pressed %d at %d,%d, frame %u", e.kind, e.pressed, e.x, e.y, cur_frame);
        if (e.kind == 0) {
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

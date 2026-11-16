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
#include <stdlib.h>
#include <string.h>

#include "rt.h"

typedef struct { uint8_t kind, pressed; int32_t x, y; } Event;   /* kind: 0 button, 1 motion */
#define QUEUE 256
static Event queue[QUEUE];
static unsigned head, tail;

void input_pointer(int kind, int pressed, int32_t x, int32_t y) {
    if (tail - head >= QUEUE) return;   /* full: drop */
    queue[tail++ % QUEUE] = (Event){(uint8_t)kind, (uint8_t)pressed, x, y};
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

/* Called at each eglSwapBuffers with the number of the frame just finished. */
void input_frame(uint32_t frame) {
    if (next_tap < 0) parse_taps();
    for (int k = 0; k < ntaps; k++) {
        if (taps[k].frame == frame) {
            rt_log("tap at (%d, %d), frame %u", taps[k].x, taps[k].y, frame);
            input_pointer(1, 0, taps[k].x, taps[k].y);
            input_pointer(0, 1, taps[k].x, taps[k].y);
        } else if (taps[k].frame + 3 == frame) {
            input_pointer(0, 0, taps[k].x, taps[k].y);
        }
    }
}

/* ------------------------------------------------------------- delivery */
static uint32_t event_buf;   /* guest memory for the event structs */

/* From s3eDeviceYield: hand every queued event to the game. */
void input_deliver(CPU *c) {
    if (head == tail) return;
    if (!event_buf) event_buf = guest_alloc(16);
    while (head != tail) {
        Event e = queue[head++ % QUEUE];
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

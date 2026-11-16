/* OpenGL ES imports, by name. None are wired up yet: the headless host build has
 * no GL context, so every GL call is reported as missing. */
#include <string.h>

#include "rt.h"

ImportFn gl_lookup(const char *name) {
    (void)name;
    return NULL;
}

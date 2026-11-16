/* Runs the recompiled game: map guest memory, load the image, call the start routine.
 *
 * codz_run(app, save) does it for any platform: app is the directory with the
 * game's files (the original executable as "kiwi" and its .dz archives), save
 * a writable directory. The iOS app (ios/app.m) calls it on a thread of its
 * own; on the Mac, main() is the command line:
 *
 *   codz [path/to/kiwi.app]
 *
 * Saves and other files the game writes go to $CODZ_SAVE (default: save/ in
 * the directory it was started from).
 *
 * Environment: RT_LOG_IMPORTS=1 logs every import call; RT_MISSING=zero makes
 * unimplemented imports return 0 instead of stopping.
 */
#include <TargetConditionals.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "rt.h"
#include "funcs.h"

#define START_ROUTINE 0x00065f00u   /* the s3e image's entry; the loader's only pointer into it */

int codz_run(const char *app_dir, const char *save_dir) {
    static char app[PATH_MAX], save[PATH_MAX];
    if (!realpath(app_dir, app)) { perror(app_dir); return 1; }
    mkdir(save_dir, 0755);
    if (!realpath(save_dir, save)) { perror(save_dir); return 1; }
    g_app_dir = app;
    g_save_dir = save;
    /* RT_EXTRA_FUNCS is written after the chdir below: make it absolute first */
    const char *extra = getenv("RT_EXTRA_FUNCS");
    if (extra && extra[0] != '/') {
        static char abs_extra[PATH_MAX];
        char cwd[PATH_MAX];
        if (getcwd(cwd, sizeof cwd)) {
            snprintf(abs_extra, sizeof abs_extra, "%s/%s", cwd, extra);
            setenv("RT_EXTRA_FUNCS", abs_extra, 1);
        }
    }
    if (chdir(g_app_dir) != 0) { perror(g_app_dir); return 1; }

    mem_init();
    load_image("kiwi");
    config_init();
    imports_init();

    static CPU cpu;
    CPU *c = &cpu;
    c->r[13] = GUEST_STACK_TOP;
    c->r[14] = GUEST_SENTINEL;
    c->excl_addr = 1;
    rt_log("calling the start routine %08x", START_ROUTINE);
    f_00065f00(c);
    rt_log("start routine returned to %08x, r0 = %08x", c->r[15], c->r[0]);
    return (int)c->r[0];
}

#if !TARGET_OS_IPHONE
int main(int argc, char **argv) {
    const char *s = getenv("CODZ_SAVE");
    return codz_run(argc > 1 ? argv[1] : "orig/Payload/kiwi.app", s ? s : "save");
}
#endif

/* Guest paths to host paths.
 *
 * s3e paths are relative to the app's data, optionally with a drive: rom://
 * (read-only app data) or ram:// (writable). Reads look in the save directory
 * first, so files the game wrote shadow the bundle's; writes go to the save
 * directory.
 */
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#include "rt.h"

const char *g_app_dir;
const char *g_save_dir;

static const char *strip_drive(const char *p, int *ram) {
    *ram = 0;
    if (!strncasecmp(p, "rom://", 6)) p += 6;
    else if (!strncasecmp(p, "ram://", 6)) { p += 6; *ram = 1; }
    while (*p == '/' || (p[0] == '.' && p[1] == '/')) p += *p == '/' ? 1 : 2;
    return p;
}

const char *host_path(const char *guest, char *buf, size_t size, int for_write) {
    if (!guest) return NULL;
    int ram;
    const char *rel = strip_drive(guest, &ram);
    if (strstr(rel, "..")) return NULL;
    char tmp[1024];
    snprintf(tmp, sizeof tmp, "%s", rel);
    for (char *p = tmp; *p; p++) if (*p == '\\') *p = '/';
    snprintf(buf, size, "%s/%s", g_save_dir, tmp);
    if (for_write || ram) return buf;
    struct stat st;
    if (stat(buf, &st) == 0) return buf;
    snprintf(buf, size, "%s/%s", g_app_dir, tmp);
    return buf;
}

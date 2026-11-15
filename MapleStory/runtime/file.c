/* kernel32 file access.
 *
 * The folder of the EXE's guest path is the game directory (--gamedir). Names are matched
 * without regard to case, component by component. Everything the guest writes
 * goes to the save directory (--savedir, by default the game directory itself):
 * when the two differ the save directory is an overlay, looked at first and the
 * only place files are created, changed or deleted, so the game files can stay
 * read-only. Paths outside it live under <savedir>/drive_c.
 */
#define _GNU_SOURCE
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#include "rt.h"

#define INVALID_HANDLE 0xFFFFFFFFu
#define H_STDIN 0x00000010u
#define H_STDOUT 0x00000014u
#define H_STDERR 0x00000018u

#define ERROR_FILE_NOT_FOUND 2u
#define ERROR_PATH_NOT_FOUND 3u
#define ERROR_ACCESS_DENIED 5u
#define ERROR_INVALID_HANDLE 6u
#define ERROR_FILE_EXISTS 80u
#define ERROR_INVALID_PARAMETER 87u
#define ERROR_ALREADY_EXISTS 183u

typedef struct RtFile {
    int fd;
    char path[768];
} RtFile;

static uint32_t errno_to_win(int e) {
    switch (e) {
    case ENOENT: return ERROR_FILE_NOT_FOUND;
    case ENOTDIR: return ERROR_PATH_NOT_FOUND;
    case EACCES: case EPERM: case EROFS: case EISDIR: return ERROR_ACCESS_DENIED;
    case EEXIST: return ERROR_FILE_EXISTS;
    default: return ERROR_ACCESS_DENIED;
    }
}

/* Append component `name` to the host directory `dir` (dir is modified), matching
 * an existing entry case-insensitively. Returns 1 if the entry exists. */
static int descend(char *dir, size_t cap, const char *name) {
    size_t n = strlen(dir);
    struct stat st;
    snprintf(dir + n, cap - n, "/%s", name);
    if (lstat(dir, &st) == 0 || stat(dir, &st) == 0) return 1;
    dir[n] = 0;
    DIR *d = opendir(dir);
    int found = 0;
    if (d) {
        struct dirent *e;
        while ((e = readdir(d)) != NULL) {
            if (!strcasecmp(e->d_name, name)) {
                snprintf(dir + n, cap - n, "/%s", e->d_name);
                found = 1;
                break;
            }
        }
        closedir(d);
    }
    if (!found) snprintf(dir + n, cap - n, "/%s", name);
    return found;
}

/* Resolve `comps` under host directory `root`. Returns 1 if the whole path exists. */
static int walk(const char *root, char comps[][256], int ncomp, char *out, size_t cap) {
    snprintf(out, cap, "%s", root);
    int exists = 1;
    for (int i = 0; i < ncomp; i++) {
        if (exists) exists = descend(out, cap, comps[i]);
        else {
            size_t n = strlen(out);
            snprintf(out + n, cap - n, "/%s", comps[i]);
        }
    }
    return exists;
}

static int overlay_active(void) { return strcmp(g_opt.gamedir, g_opt.savedir) != 0; }

static void mkdirs_for(const char *path) {
    char tmp[768];
    snprintf(tmp, sizeof tmp, "%s", path);
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = 0;
            mkdir(tmp, 0777);
            *p = '/';
        }
    }
}

/* Translate a guest path. `write`: the caller will create or modify the file.
 * Returns 1 if the resulting host path exists. *in_game is set when the file
 * exists only in the (read-only) game directory; for a write the result is then
 * the overlay path to create and game_path (768 bytes, optional) the original. */
/* Components of the guest's game directory: the folder of the EXE's guest path
 * without the drive, e.g. {"Program Files", "Microsoft Games", "Age of Mythology"}. */
static int game_dir_comps(char gc[][256], int max) {
    const char *p = g_image.guest_path;
    if (p[0] && p[1] == ':') p += 2;
    const char *end = strrchr(p, '\\');
    int n = 0;
    while (end && p < end && n < max) {
        while (*p == '\\') p++;
        size_t len = strcspn(p, "\\");
        if (!len || p >= end) break;
        snprintf(gc[n++], 256, "%.*s", (int)(len > 255 ? 255 : len), p);
        p += len;
    }
    return n;
}

static int resolve2(const char *guest, int write, char *out, size_t cap, int *in_game, char *game_path) {
    char comps[32][256];
    int n = 0;
    char gc[8][256];
    int ngc = game_dir_comps(gc, 8);
    const char *p = guest;
    int absolute = 0;
    if (in_game) *in_game = 0;
    /* "\\C:\dir" and "/C:/dir" (what URL-to-path conversions leave) name C:\dir */
    {
        const char *q = p;
        while (*q == '\\' || *q == '/') q++;
        if (q != p && ((q[0] | 0x20) >= 'a' && (q[0] | 0x20) <= 'z') && q[1] == ':') p = q;
    }
    if (((p[0] | 0x20) >= 'a' && (p[0] | 0x20) <= 'z') && p[1] == ':') {
        p += 2;
        absolute = *p == '\\' || *p == '/';
    } else if (*p == '\\' || *p == '/') {
        absolute = 1;
    }
    if (!absolute) /* the current directory is the game's */
        for (int i = 0; i < ngc; i++) snprintf(comps[n++], 256, "%s", gc[i]);
    while (*p) {
        while (*p == '\\' || *p == '/') p++;
        size_t len = strcspn(p, "\\/");
        if (!len) break;
        if (len == 1 && p[0] == '.') {
        } else if (len == 2 && p[0] == '.' && p[1] == '.') {
            if (n > 0) n--;
        } else if (n < 32) {
            snprintf(comps[n++], 256, "%.*s", (int)(len > 255 ? 255 : len), p);
        }
        p += len;
    }
    int in_dir = n >= ngc;
    for (int i = 0; in_dir && i < ngc; i++) in_dir = !strcasecmp(comps[i], gc[i]);
    if (in_dir) {
        char (*rel)[256] = comps + ngc;
        int nrel = n - ngc;
        int exists = walk(g_opt.savedir, rel, nrel, out, cap);
        if (!overlay_active() || exists) return exists;
        char game[768];
        int in_g = walk(g_opt.gamedir, rel, nrel, game, sizeof game);
        if (write) {
            /* Create it in the overlay, under the directories the game tree has. */
            if (nrel > 0) {
                char parent[768];
                if (nrel == 1 || walk(g_opt.gamedir, rel, nrel - 1, parent, sizeof parent)) mkdirs_for(out);
            }
            if (in_game) *in_game = in_g;
            if (in_g && game_path) snprintf(game_path, 768, "%s", game);
            return 0;
        }
        if (in_g) {
            snprintf(out, cap, "%s", game);
            if (in_game) *in_game = 1;
        }
        return in_g;
    }
    char root[640];
    snprintf(root, sizeof root, "%.600s/drive_c", g_opt.savedir);
    int exists = walk(root, comps, n, out, cap);
    if (write && !exists) mkdirs_for(out);
    return exists;
}

static int resolve(const char *guest, int write, char *out, size_t cap, int *in_game) {
    return resolve2(guest, write, out, cap, in_game, NULL);
}

/* Host path of a guest path, for reading. */
int rt_resolve_path(const char *guest, char *out, size_t cap) { return resolve(guest, 0, out, cap, NULL); }

static int copy_file(const char *from, const char *to) {
    int in = open(from, O_RDONLY | O_CLOEXEC);
    if (in < 0) return 0;
    int out = open(to, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0666);
    if (out < 0) {
        close(in);
        return 0;
    }
    char buf[65536];
    ssize_t r;
    int ok = 1;
    while ((r = read(in, buf, sizeof buf)) > 0)
        if (write(out, buf, (size_t)r) != r) ok = 0;
    close(in);
    close(out);
    return ok && r == 0;
}

static void file_close(void *obj) {
    RtFile *f = obj;
    close(f->fd);
    free(f);
}

/* CreateFileA(name, access, share, security, disposition, flags, template) */
H(k32_CreateFileA) {
    const char *name = rt_gstr(a[0]);
    /* The game opens its archives with OPEN_ALWAYS and read access: the disposition
     * alone does not make an open a write. */
    int want_write = (a[1] & 0x50000000u) != 0 || a[4] == 2 || a[4] == 5;
    char host[768], game[768];
    int in_game = 0;
    int exists = resolve(name, 0, host, sizeof host, &in_game);
    if ((!exists && (a[4] == 1 || a[4] == 2 || a[4] == 4)) || (exists && in_game && want_write)) {
        /* It will be created, or it exists only in the game directory and is opened for
         * writing: the file to work on is the one in the save directory. A file about to
         * be truncated needs no copy of its old contents. */
        int was_in_game = exists && in_game;
        resolve2(name, 1, host, sizeof host, &in_game, game);
        if (was_in_game && (a[4] == 3 || a[4] == 4)) {
            exists = copy_file(game, host);
            rt_log("recomp: \"%s\" is opened for writing: copied to the save directory\n", name);
        }
        want_write = 1;
    }
    int flags = O_CLOEXEC;
    if ((a[1] & 0xC0000000u) == 0xC0000000u || (a[1] & 0x10000000u)) flags |= O_RDWR;
    else if (a[1] & 0x40000000u) flags |= O_WRONLY;
    else flags |= O_RDONLY;
    uint32_t err = 0;
    switch (a[4]) {
    case 1: /* CREATE_NEW */
        if (exists) {
            rt_set_last_error(c, ERROR_FILE_EXISTS);
            return INVALID_HANDLE;
        }
        flags |= O_CREAT | O_EXCL;
        break;
    case 2: /* CREATE_ALWAYS */
        flags |= O_CREAT | O_TRUNC;
        if (exists) err = ERROR_ALREADY_EXISTS;
        break;
    case 3: /* OPEN_EXISTING */
        break;
    case 4: /* OPEN_ALWAYS */
        flags |= O_CREAT;
        if (exists) err = ERROR_ALREADY_EXISTS;
        break;
    case 5: /* TRUNCATE_EXISTING */
        if (!exists) {
            rt_set_last_error(c, ERROR_FILE_NOT_FOUND);
            return INVALID_HANDLE;
        }
        flags |= O_CREAT | O_TRUNC;
        break;
    default:
        rt_set_last_error(c, ERROR_INVALID_PARAMETER);
        return INVALID_HANDLE;
    }
    int fd = open(host, flags, 0666);
    struct stat st;
    if (fd >= 0 && fstat(fd, &st) == 0 && S_ISDIR(st.st_mode)) {
        close(fd);
        fd = -1;
        errno = EACCES;
    }
    if (fd < 0) {
        int e = errno;
        if (g_opt.verbose || g_trace_imports) rt_log("[file] open \"%s\" -> %s: %s\n", name, host, strerror(e));
        rt_set_last_error(c, errno_to_win(e));
        return INVALID_HANDLE;
    }
    RtFile *f = calloc(1, sizeof *f);
    f->fd = fd;
    snprintf(f->path, sizeof f->path, "%s", host);
    uint32_t h = rt_handle_alloc(RT_H_FILE, f, file_close);
    if (g_opt.verbose || g_trace_imports)
        rt_log("[file] open \"%s\" -> %s (handle %x%s)\n", name, host, h, want_write ? ", writable" : "");
    rt_set_last_error(c, err);
    return h;
}

/* ReadFile(handle, buffer, count, &read, overlapped) */
H(k32_ReadFile) {
    if (a[3]) st32(a[3], 0);
    RtFile *f = rt_handle_get(a[0], RT_H_FILE);
    int fd = f ? f->fd : a[0] == H_STDIN ? 0 : -1;
    if (fd < 0) {
        rt_set_last_error(c, ERROR_INVALID_HANDLE);
        return 0;
    }
    if (!f) return 1; /* standard input: always at end of file */
    uint32_t done = 0;
    while (done < a[2]) {
        ssize_t r = read(fd, G(a[1] + done), a[2] - done);
        if (r < 0) {
            if (errno == EINTR) continue;
            rt_set_last_error(c, errno_to_win(errno));
            return 0;
        }
        if (r == 0) break;
        done += (uint32_t)r;
    }
    if (a[3]) st32(a[3], done);
    return 1;
}

/* WriteFile(handle, buffer, count, &written, overlapped) */
H(k32_WriteFile) {
    if (a[3]) st32(a[3], 0);
    RtFile *f = rt_handle_get(a[0], RT_H_FILE);
    if (!f) {
        if (a[0] == H_STDOUT || a[0] == H_STDERR) {
            /* The guest's console output goes to the host's standard error, marked as such. */
            rt_log("[guest %s] %.*s%s", a[0] == H_STDOUT ? "stdout" : "stderr", (int)a[2], (const char *)G(a[1]),
                   a[2] && ld8(a[1] + a[2] - 1) == '\n' ? "" : "\n");
            if (a[3]) st32(a[3], a[2]);
            return 1;
        }
        rt_set_last_error(c, ERROR_INVALID_HANDLE);
        return 0;
    }
    uint32_t done = 0;
    while (done < a[2]) {
        ssize_t r = write(f->fd, G(a[1] + done), a[2] - done);
        if (r < 0) {
            if (errno == EINTR) continue;
            rt_set_last_error(c, errno_to_win(errno));
            return 0;
        }
        done += (uint32_t)r;
    }
    if (a[3]) st32(a[3], done);
    return 1;
}

/* SetFilePointer(handle, distanceLow, &distanceHigh, method) */
H(k32_SetFilePointer) {
    RtFile *f = rt_handle_get(a[0], RT_H_FILE);
    if (!f) {
        rt_set_last_error(c, ERROR_INVALID_HANDLE);
        return 0xFFFFFFFFu;
    }
    int64_t off = (int32_t)a[1];
    if (a[2]) off = (int64_t)(((uint64_t)ld32(a[2]) << 32) | a[1]);
    int whence = a[3] == 0 ? SEEK_SET : a[3] == 1 ? SEEK_CUR : SEEK_END;
    off_t r = lseek(f->fd, (off_t)off, whence);
    if (r < 0) {
        rt_set_last_error(c, 131); /* ERROR_NEGATIVE_SEEK */
        return 0xFFFFFFFFu;
    }
    if (a[2]) st32(a[2], (uint32_t)((uint64_t)r >> 32));
    rt_set_last_error(c, 0);
    return (uint32_t)r;
}

H(k32_GetFileSize) {
    RtFile *f = rt_handle_get(a[0], RT_H_FILE);
    struct stat st;
    if (!f || fstat(f->fd, &st) != 0) {
        rt_set_last_error(c, ERROR_INVALID_HANDLE);
        return 0xFFFFFFFFu;
    }
    if (a[1]) st32(a[1], (uint32_t)((uint64_t)st.st_size >> 32));
    rt_set_last_error(c, 0);
    return (uint32_t)st.st_size;
}

H(k32_SetEndOfFile) {
    RtFile *f = rt_handle_get(a[0], RT_H_FILE);
    if (!f) {
        rt_set_last_error(c, ERROR_INVALID_HANDLE);
        return 0;
    }
    off_t pos = lseek(f->fd, 0, SEEK_CUR);
    return pos >= 0 && ftruncate(f->fd, pos) == 0;
}

H(k32_FlushFileBuffers) { UNUSED; return 1; }

H(k32_GetFileType) {
    UNUSED;
    if (a[0] == H_STDIN || a[0] == H_STDOUT || a[0] == H_STDERR) return 2; /* FILE_TYPE_CHAR */
    if (rt_handle_get(a[0], RT_H_FILE)) return 1;                          /* FILE_TYPE_DISK */
    return 0;
}

H(k32_DeleteFileA) {
    char host[768];
    int in_game = 0;
    if (!resolve(rt_gstr(a[0]), 0, host, sizeof host, &in_game)) {
        rt_set_last_error(c, ERROR_FILE_NOT_FOUND);
        return 0;
    }
    if (in_game) {
        rt_log("recomp: DeleteFileA(\"%s\"): the file is in the read-only game directory; not deleted\n",
               rt_gstr(a[0]));
        rt_set_last_error(c, ERROR_ACCESS_DENIED);
        return 0;
    }
    if (unlink(host) != 0) {
        rt_set_last_error(c, errno_to_win(errno));
        return 0;
    }
    return 1;
}

H(k32_MoveFileA) {
    char from[768], to[768];
    int in_game = 0;
    if (!resolve(rt_gstr(a[0]), 0, from, sizeof from, &in_game)) {
        rt_set_last_error(c, ERROR_FILE_NOT_FOUND);
        return 0;
    }
    if (resolve(rt_gstr(a[1]), 1, to, sizeof to, NULL)) {
        rt_set_last_error(c, ERROR_ALREADY_EXISTS);
        return 0;
    }
    int ok = in_game ? copy_file(from, to) : rename(from, to) == 0;
    if (in_game) rt_log("recomp: MoveFileA(\"%s\"): the source is in the read-only game directory; copied\n", rt_gstr(a[0]));
    if (!ok) rt_set_last_error(c, errno_to_win(errno));
    return (uint32_t)ok;
}

H(k32_CreateDirectoryA) {
    char host[768];
    if (resolve(rt_gstr(a[0]), 1, host, sizeof host, NULL)) {
        rt_set_last_error(c, ERROR_ALREADY_EXISTS);
        return 0;
    }
    if (mkdir(host, 0777) != 0) {
        rt_set_last_error(c, errno == EEXIST ? ERROR_ALREADY_EXISTS : ERROR_PATH_NOT_FOUND);
        return 0;
    }
    return 1;
}

static uint32_t attributes_of(const struct stat *st) {
    if (S_ISDIR(st->st_mode)) return 0x10; /* FILE_ATTRIBUTE_DIRECTORY */
    return 0x20;                           /* FILE_ATTRIBUTE_ARCHIVE */
}

H(k32_GetFileAttributesA) {
    char host[768];
    struct stat st;
    /* Wildcards and the other characters Windows forbids in names: ERROR_INVALID_NAME.
     * MSLU's loader tells NT from Windows 9x by this answer for "???.???". */
    const char *name = rt_gstr(a[0]);
    if (strpbrk(name[0] && name[1] == ':' ? name + 2 : name, "*?<>|\"")) {
        rt_set_last_error(c, 123);
        return 0xFFFFFFFFu;
    }
    if (!resolve(rt_gstr(a[0]), 0, host, sizeof host, NULL) || stat(host, &st) != 0) {
        rt_set_last_error(c, ERROR_FILE_NOT_FOUND);
        return 0xFFFFFFFFu;
    }
    return attributes_of(&st);
}

H(k32_SetFileAttributesA) {
    char host[768];
    if (!resolve(rt_gstr(a[0]), 0, host, sizeof host, NULL)) {
        rt_set_last_error(c, ERROR_FILE_NOT_FOUND);
        return 0;
    }
    return 1;
}

static uint64_t filetime_of(time_t t) { return ((uint64_t)t + 11644473600ull) * 10000000ull; }

static int wild_match(const char *pat, const char *s) {
    for (; *pat; pat++, s++) {
        if (*pat == '*') {
            while (pat[1] == '*') pat++;
            if (!pat[1]) return 1;
            for (; *s; s++)
                if (wild_match(pat + 1, s)) return 1;
            return wild_match(pat + 1, s);
        }
        if (!*s) return 0;
        if (*pat != '?' && tolower((unsigned char)*pat) != tolower((unsigned char)*s)) return 0;
    }
    return *s == 0;
}

static void fill_find_data(uint32_t p, const char *name, const struct stat *st) {
    memset(G(p), 0, 320);
    st32(p, attributes_of(st));
    st64(p + 4, filetime_of(st->st_ctime));
    st64(p + 12, filetime_of(st->st_atime));
    st64(p + 20, filetime_of(st->st_mtime));
    st32(p + 28, (uint32_t)((uint64_t)st->st_size >> 32));
    st32(p + 32, (uint32_t)st->st_size);
    snprintf((char *)G(p + 44), 260, "%s", name);
}

/* ------------------------------------------------------------ directory search
 * FindFirstFile collects every match at once: the overlay's entries first, then
 * the game tree's that the overlay does not shadow. FindNextFile walks the list. */
typedef struct FindEntry {
    char name[260];
    char host[768];
} FindEntry;
typedef struct FindState {
    FindEntry *e;
    int n, cap, pos;
} FindState;

static void find_free(void *p) {
    FindState *f = p;
    free(f->e);
    free(f);
}

static void find_collect(FindState *f, const char *dir, const char *pat) {
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        if (!wild_match(pat, de->d_name)) continue;
        int dup = 0;
        for (int i = 0; i < f->n && !dup; i++) dup = !strcasecmp(f->e[i].name, de->d_name);
        if (dup) continue;
        if (f->n == f->cap) {
            f->cap = f->cap ? 2 * f->cap : 64;
            f->e = realloc(f->e, (size_t)f->cap * sizeof *f->e);
        }
        snprintf(f->e[f->n].name, 260, "%s", de->d_name);
        snprintf(f->e[f->n].host, 768, "%.500s/%.260s", dir, de->d_name);
        f->n++;
    }
    closedir(d);
}

/* Fill WIN32_FIND_DATAA (wide = 0, 320 bytes) or WIN32_FIND_DATAW (wide = 1, 592 bytes). */
static int find_fill(FindState *f, uint32_t out, int wide) {
    while (f->pos < f->n) {
        FindEntry *e = &f->e[f->pos++];
        struct stat st;
        if (stat(e->host, &st) != 0) continue;
        if (!wide) {
            fill_find_data(out, e->name, &st);
            return 1;
        }
        memset(G(out), 0, 592);
        st32(out, attributes_of(&st));
        st64(out + 4, filetime_of(st.st_ctime));
        st64(out + 12, filetime_of(st.st_atime));
        st64(out + 20, filetime_of(st.st_mtime));
        st32(out + 28, (uint32_t)((uint64_t)st.st_size >> 32));
        st32(out + 32, (uint32_t)st.st_size);
        rt_put_wstr(out + 44, 260, e->name);
        return 1;
    }
    return 0;
}

static uint32_t find_first(CPU *c, const char *g, uint32_t out, int wide) {
    char dirpart[600], pat[260];
    const char *slash = strrchr(g, '\\');
    const char *fs = strrchr(g, '/');
    if (fs > slash) slash = fs;
    if (slash) {
        snprintf(dirpart, sizeof dirpart, "%.*s", (int)(slash - g + 1), g);
        snprintf(pat, sizeof pat, "%s", slash + 1);
    } else {
        snprintf(dirpart, sizeof dirpart, ".");
        snprintf(pat, sizeof pat, "%s", g);
    }
    if (!strcmp(pat, "*.*")) snprintf(pat, sizeof pat, "*");
    FindState *f = calloc(1, sizeof *f);
    char host[768];
    int in_game = 0;
    if (resolve(dirpart, 0, host, sizeof host, &in_game)) find_collect(f, host, pat);
    if (overlay_active()) {
        /* the directory may exist in both trees */
        size_t n = strlen(g_opt.savedir);
        if (!strncmp(host, g_opt.savedir, n)) {
            /* the same directory in the game tree, names matched without regard to case */
            char comps[32][256], both[768];
            int nc = 0;
            for (const char *p = host + n; *p && nc < 32;) {
                while (*p == '/') p++;
                size_t len = strcspn(p, "/");
                if (!len) break;
                snprintf(comps[nc++], 256, "%.*s", (int)(len > 255 ? 255 : len), p);
                p += len;
            }
            if (walk(g_opt.gamedir, comps, nc, both, sizeof both)) find_collect(f, both, pat);
        }
    }
    int found = find_fill(f, out, wide);
    if (g_opt.verbose || g_trace_imports) rt_log("[file] find \"%s\" -> %d entries\n", g, f->n);
    if (!found) {
        find_free(f);
        rt_set_last_error(c, ERROR_FILE_NOT_FOUND);
        return INVALID_HANDLE;
    }
    return rt_handle_alloc(RT_H_FIND, f, find_free);
}

H(k32_FindFirstFileA) { return find_first(c, rt_gstr(a[0]), a[1], 0); }
H(k32_FindFirstFileW) {
    char g[600];
    rt_get_wstr(a[0], g, sizeof g);
    return find_first(c, g, a[1], 1);
}
static uint32_t find_next(CPU *c, const uint32_t *a, int wide) {
    FindState *f = rt_handle_get(a[0], RT_H_FIND);
    if (!f) {
        rt_set_last_error(c, 6);
        return 0;
    }
    if (find_fill(f, a[1], wide)) return 1;
    rt_set_last_error(c, 18); /* ERROR_NO_MORE_FILES */
    return 0;
}
H(k32_FindNextFileA) { return find_next(c, a, 0); }
H(k32_FindNextFileW) { return find_next(c, a, 1); }
H(k32_FindClose) {
    UNUSED;
    return rt_handle_get(a[0], RT_H_FIND) ? (uint32_t)rt_handle_close(a[0]) : 0;
}

H(k32_GetSystemDirectoryA) {
    UNUSED;
    static const char dir[] = "C:\\WINDOWS\\system32";
    if (a[1] < sizeof dir) return sizeof dir;
    memcpy(G(a[0]), dir, sizeof dir);
    return sizeof dir - 1;
}

/* ------------------------------------------------------------ resources
 * FindResource walks the three-level resource directory of the image:
 * type, name, language. Names and types are integers or strings. */
/* ---------------------------------------------------------------- resources
 * A module handle is the module's base: 0 or the EXE's base for the EXE, a
 * lifted DLL, or a resource-only DLL mapped as data. Names and types are
 * integer ids or strings; the strings arrive here as host UTF-8. */
static RtImage *rsrc_image(uint32_t hmod) {
    if (!hmod || hmod == g_image.base) return &g_image;
    RtImage *im = rt_image_at(hmod);
    return im && im->rsrc_rva ? im : NULL;
}

/* Entry of directory `dir` for an id (s == NULL) or a name; returns its offset word, 0 if absent. */
static uint32_t rsrc_lookup(uint32_t base, uint32_t dir, uint32_t id, const char *s) {
    uint32_t nnamed = ld16(dir + 12), nid = ld16(dir + 14);
    for (uint32_t i = 0; i < nnamed + nid; i++) {
        uint32_t e = dir + 16 + 8 * i;
        uint32_t name = ld32(e), off = ld32(e + 4);
        if (s) {
            if (!(name & 0x80000000u)) continue;
            uint32_t ws = base + (name & 0x7FFFFFFF);
            uint32_t len = ld16(ws);
            if (strlen(s) != len) continue;
            uint32_t k;
            for (k = 0; k < len; k++)
                if (toupper((unsigned char)s[k]) != toupper(ld16(ws + 2 + 2 * k) & 0xFF)) break;
            if (k == len) return off;
        } else if (!(name & 0x80000000u) && name == id) {
            return off;
        }
    }
    return 0;
}

/* IMAGE_RESOURCE_DATA_ENTRY of (type, name) in the module, first language; 0 if absent. */
static uint32_t rsrc_find(RtImage *im, uint32_t type, const char *type_s, uint32_t name, const char *name_s) {
    if (!im || !im->rsrc_rva) return 0;
    uint32_t base = im->base + im->rsrc_rva;
    uint32_t off = rsrc_lookup(base, base, type, type_s);
    if (!(off & 0x80000000u)) return 0;
    off = rsrc_lookup(base, base + (off & 0x7FFFFFFF), name, name_s);
    if (off & 0x80000000u) {
        uint32_t dir = base + (off & 0x7FFFFFFF);
        off = ld16(dir + 12) + ld16(dir + 14) ? ld32(dir + 16 + 4) : 0;
    }
    if (!off || (off & 0x80000000u)) return 0;
    return base + off;
}

/* Data of resource (type, id) of a module (0 = the EXE) in guest memory, its size in
 * *size; 0 if absent. */
uint32_t rt_resource(uint32_t hmod, uint32_t type, uint32_t id, uint32_t *size) {
    RtImage *im = rsrc_image(hmod);
    uint32_t e = rsrc_find(im, type, NULL, id, NULL);
    if (!e) return 0;
    if (size) *size = ld32(e + 4);
    return im->base + ld32(e);
}

/* A resource name or type argument: an integer id, "#123", or a string (A or W). */
static const char *rsrc_key(uint32_t v, int wide, uint32_t *id, char *buf, size_t cap) {
    if (v < 0x10000) {
        *id = v;
        return NULL;
    }
    if (wide) rt_get_wstr(v, buf, cap);
    else snprintf(buf, cap, "%s", rt_gstr(v));
    if (buf[0] == '#') {
        *id = (uint32_t)atoi(buf + 1);
        return NULL;
    }
    *id = 0;
    return buf;
}

static uint32_t find_resource(CPU *c, uint32_t hmod, uint32_t name, uint32_t type, int wide) {
    char nb[128], tb[128];
    uint32_t nid, tid;
    const char *ns = rsrc_key(name, wide, &nid, nb, sizeof nb), *ts = rsrc_key(type, wide, &tid, tb, sizeof tb);
    uint32_t r = rsrc_find(rsrc_image(hmod), tid, ts, nid, ns);
    if (!r) rt_set_last_error(c, 1814); /* ERROR_RESOURCE_NAME_NOT_FOUND */
    return r;
}

H(k32_FindResourceA) { return find_resource(c, a[0], a[1], a[2], 0); }
H(k32_FindResourceW) { return find_resource(c, a[0], a[1], a[2], 1); }
H(k32_FindResourceExA) { return find_resource(c, a[0], a[2], a[1], 0); }
H(k32_FindResourceExW) { return find_resource(c, a[0], a[2], a[1], 1); }
H(k32_LoadResource) {
    UNUSED;
    RtImage *im = rsrc_image(a[0]);
    return a[1] && im ? im->base + ld32(a[1]) : 0;
}
H(k32_SizeofResource) { UNUSED; return a[1] ? ld32(a[1] + 4) : 0; }
H(k32_FreeResource) { UNUSED; return 0; }

/* LoadString(hinst, id, buf, cch). String tables: RT_STRING (6) block id/16 + 1
 * holds 16 counted UTF-16 strings. LoadStringW with cch 0 stores a pointer to
 * the (unterminated) resource text in *buf and returns its length. */
static uint32_t load_string(CPU *c, const uint32_t *a, int wide) {
    RtImage *im = rsrc_image(a[0]);
    uint32_t e = rsrc_find(im, 6, NULL, (a[1] >> 4) + 1, NULL);
    if (!e) {
        rt_set_last_error(c, 1814);
        return 0;
    }
    uint32_t p = im->base + ld32(e);
    for (uint32_t i = 0; i < (a[1] & 15); i++) p += 2 + 2 * ld16(p);
    uint32_t len = ld16(p), str = p + 2;
    if (wide && a[3] == 0) {
        st32(a[2], str);
        return len;
    }
    if (!a[3]) return 0;
    uint32_t n = len < a[3] - 1 ? len : a[3] - 1;
    if (wide) {
        memcpy(G(a[2]), G(str), 2 * n);
        st16(a[2] + 2 * n, 0);
    } else {
        for (uint32_t i = 0; i < n; i++) {
            uint16_t ch = ld16(str + 2 * i);
            st8(a[2] + i, ch < 256 ? (uint8_t)ch : '?');
        }
        st8(a[2] + n, 0);
    }
    return n;
}
H(user_LoadStringW) { return load_string(c, a, 1); }
H(user_LoadStringA) { return load_string(c, a, 0); }

/* GetFileTime(h, creation*, access*, write*) */
H(k32_GetFileTime) {
    RtFile *f = rt_handle_get(a[0], RT_H_FILE);
    struct stat st;
    if (!f || fstat(f->fd, &st) != 0) {
        rt_set_last_error(c, 6);
        return 0;
    }
    if (a[1]) st64(a[1], filetime_of(st.st_ctime));
    if (a[2]) st64(a[2], filetime_of(st.st_atime));
    if (a[3]) st64(a[3], filetime_of(st.st_mtime));
    return 1;
}

static const ImportDef defs[] = {
    {"kernel32.dll", "CreateFileA", k32_CreateFileA, 7, 0},
    {"kernel32.dll", "ReadFile", k32_ReadFile, 5, 0},
    {"kernel32.dll", "WriteFile", k32_WriteFile, 5, 0},
    {"kernel32.dll", "SetFilePointer", k32_SetFilePointer, 4, 0},
    {"kernel32.dll", "GetFileSize", k32_GetFileSize, 2, 0},
    {"kernel32.dll", "SetEndOfFile", k32_SetEndOfFile, 1, 0},
    {"kernel32.dll", "FlushFileBuffers", k32_FlushFileBuffers, 1, 0},
    {"kernel32.dll", "GetFileType", k32_GetFileType, 1, 0},
    {"kernel32.dll", "DeleteFileA", k32_DeleteFileA, 1, 0},
    {"kernel32.dll", "MoveFileA", k32_MoveFileA, 2, 0},
    {"kernel32.dll", "CreateDirectoryA", k32_CreateDirectoryA, 2, 0},
    {"kernel32.dll", "GetFileAttributesA", k32_GetFileAttributesA, 1, 0},
    {"kernel32.dll", "SetFileAttributesA", k32_SetFileAttributesA, 2, 0},
    {"kernel32.dll", "FindFirstFileA", k32_FindFirstFileA, 2, 0},
    {"kernel32.dll", "FindFirstFileW", k32_FindFirstFileW, 2, 0},
    {"kernel32.dll", "FindNextFileA", k32_FindNextFileA, 2, 0},
    {"kernel32.dll", "FindNextFileW", k32_FindNextFileW, 2, 0},
    {"kernel32.dll", "FindClose", k32_FindClose, 1, 0},
    {"kernel32.dll", "GetSystemDirectoryA", k32_GetSystemDirectoryA, 2, 0},
    {"kernel32.dll", "FindResourceA", k32_FindResourceA, 3, 0},
    {"kernel32.dll", "FindResourceW", k32_FindResourceW, 3, 0},
    {"kernel32.dll", "FindResourceExW", k32_FindResourceExW, 4, 0},
    {"kernel32.dll", "FreeResource", k32_FreeResource, 1, 0},
    {"kernel32.dll", "GetFileTime", k32_GetFileTime, 4, 0},
    {"user32.dll", "LoadStringW", user_LoadStringW, 4, 0},
    {"user32.dll", "LoadStringA", user_LoadStringA, 4, 0},
    {"kernel32.dll", "FindResourceExA", k32_FindResourceExA, 4, 0},
    {"kernel32.dll", "LoadResource", k32_LoadResource, 2, 0},
    {"kernel32.dll", "SizeofResource", k32_SizeofResource, 2, 0},
};
RT_EXPORT_IMPORTS(rt_imports_file, defs);

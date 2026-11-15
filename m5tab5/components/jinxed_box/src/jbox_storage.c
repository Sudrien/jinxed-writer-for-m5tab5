/*
 * jbox_storage.c -- writes on the card kept to Jinxed's directory, for a
 * boxed editor (u-jinxed-0003, 0004).
 *
 * feckless_box_calls answers every storage call Jinxed's editor makes
 * (u-feckless-0047, 0050), and its box keeps the app out of the host's
 * memory but not out of the card's files: a boxed app may write anywhere
 * on the card. Jinxed's editor writes under JINXED_BOX_ROOT and nowhere
 * else, so a write named outside it is a bug, and this refuses it.
 *
 * jinxed_box_calls() is feckless_box_calls_v1()'s table, the same names
 * in the same order, with the calls that can change the card by a path
 * wrapped. A wrapper copies the path out of the box, checks the copy, and
 * hands the call on to feckless's own handler unchanged, or refuses it:
 *
 *   remove, unlink, mkdir, rmdir, utime, storage_mark_hidden  the path
 *   rename                                       both paths
 *   fopen, storage_io_open                        the path, if the mode
 *                                                 writes (w, a or +)
 *   f_open, f_chmod                               refused if they write:
 *                                                 FatFs's paths are its
 *                                                 drive's, not /sd's, and
 *                                                 the editor does not
 *                                                 call FatFs
 *
 * Reads are not contained: opening for reading, stat, opendir and the rest
 * go anywhere on the card, as feckless lets them.
 *
 * A path is allowed if it is JINXED_BOX_ROOT or below it with no ".."
 * segment; a refusal sets the app's errno (EACCES, or EFAULT for a path
 * not terminated in the box) where feckless's own refusals do
 * (u-feckless-0050), and returns what feckless returns for a failure.
 *
 * The check is made on a copy, but feckless's handler reads the path from
 * the box again. A second thread of the app writing the string between
 * the two could slip a path past the check. That stops an editor's
 * mistakes, not a hostile app; the box's own guarantee, the host's memory,
 * does not depend on it.
 *
 * SPDX-License-Identifier: MIT
 */
#include "jinxed_box.h"

#include <errno.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "feckless_box_calls.h"
#include "ff.h"

typedef feckless_umode_window_t win_t;
#define H(name) static bool name(win_t *w, const uint32_t a[7], uint32_t *ret)

#define MODE_MAX 8                          /* feckless's own limit on a mode */
/* FatFs's mode bits that write or create: FA_WRITE, FA_CREATE_NEW,
 * FA_CREATE_ALWAYS, FA_OPEN_ALWAYS (and so FA_OPEN_APPEND). */
#define FA_WRITES 0x1Eu
#if defined(FA_WRITE) && defined(FA_OPEN_ALWAYS)
_Static_assert((FA_WRITE | FA_CREATE_NEW | FA_CREATE_ALWAYS | FA_OPEN_ALWAYS) == FA_WRITES, "FatFs's mode bits");
#endif

/* The calls wrapped, and feckless's handler for each, found by name. */
enum { G_REMOVE, G_UNLINK, G_MKDIR, G_RMDIR, G_UTIME, G_MARK_HIDDEN, G_RENAME,
       G_FOPEN, G_IO_OPEN, G_F_OPEN, G_F_CHMOD, G_N };
static feckless_umode_call_fn s_next[G_N];

/* The app's errno: the word at tp, if tp is in the box. */
static void set_errno(win_t *w, int e)
{
    const uint32_t tp = w->ctx.x[4];
    if (feckless_umode_range_ok(w, (uintptr_t)tp, sizeof(int))) {
        *(int *)(uintptr_t)tp = e;
    }
}

/* A copy of the app's string at p, terminated within max (counting the
 * NUL) and the box; false otherwise. */
static bool copy_str(const win_t *w, uint32_t p, char *out, uint32_t max)
{
    const int n = feckless_umode_strlen(w, (uintptr_t)p, max);
    if (n < 0) {
        return false;
    }
    memcpy(out, (const void *)(uintptr_t)p, (size_t)n);
    out[n] = '\0';
    return true;
}

/* JINXED_BOX_ROOT itself or below it, and no ".." segment anywhere. */
static bool path_allowed(const char *p)
{
    const size_t r = sizeof JINXED_BOX_ROOT - 1;
    if (strncmp(p, JINXED_BOX_ROOT, r) != 0 || (p[r] != '\0' && p[r] != '/')) {
        return false;
    }
    for (const char *s = p; *s;) {
        while (*s == '/') s++;
        const char *e = s;
        while (*e && *e != '/') e++;
        if (e - s == 2 && s[0] == '.' && s[1] == '.') {
            return false;
        }
        s = e;
    }
    return true;
}

/* Is the app's path at p one it may write? If not, its errno is set. */
static bool may_write(win_t *w, uint32_t p)
{
    char path[JINXED_BOX_PATH_MAX];
    if (!copy_str(w, p, path, sizeof path)) {
        set_errno(w, EFAULT);
        return false;
    }
    if (!path_allowed(path)) {
        set_errno(w, EACCES);
        return false;
    }
    return true;
}

/* Does the app's stdio mode at p write? A mode that is not a string in
 * the box counts as writing, so its path is checked. */
static bool mode_writes(const win_t *w, uint32_t p)
{
    char m[MODE_MAX];
    if (!copy_str(w, p, m, sizeof m)) {
        return true;
    }
    return m[0] != 'r' || strchr(m, '+') != NULL;
}

/* path in a0; -1 refused */
#define BY_PATH(name, g)                                \
    H(name)                                             \
    {                                                   \
        if (!may_write(w, a[0])) {                      \
            *ret = (uint32_t)-1;                        \
            return true;                                \
        }                                               \
        return s_next[g](w, a, ret);                    \
    }
BY_PATH(g_remove, G_REMOVE)
BY_PATH(g_unlink, G_UNLINK)
BY_PATH(g_mkdir, G_MKDIR)
BY_PATH(g_rmdir, G_RMDIR)
BY_PATH(g_utime, G_UTIME)

/* void; nothing is marked */
H(g_storage_mark_hidden)
{
    if (!may_write(w, a[0])) {
        *ret = 0;
        return true;
    }
    return s_next[G_MARK_HIDDEN](w, a, ret);
}

H(g_rename)
{
    if (!may_write(w, a[0]) || !may_write(w, a[1])) {
        *ret = (uint32_t)-1;
        return true;
    }
    return s_next[G_RENAME](w, a, ret);
}

/* (path, mode); NULL refused */
H(g_fopen)
{
    if (mode_writes(w, a[1]) && !may_write(w, a[0])) {
        *ret = 0;
        return true;
    }
    return s_next[G_FOPEN](w, a, ret);
}
H(g_storage_io_open)
{
    if (mode_writes(w, a[1]) && !may_write(w, a[0])) {
        *ret = 0;
        return true;
    }
    return s_next[G_IO_OPEN](w, a, ret);
}

/* (FIL *, path, mode) */
H(g_f_open)
{
    if (a[2] & FA_WRITES) {
        *ret = FR_DENIED;
        return true;
    }
    return s_next[G_F_OPEN](w, a, ret);
}
/* (path, attr, mask): an attribute is a write */
H(g_f_chmod)
{
    (void)w;
    (void)a;
    *ret = FR_DENIED;
    return true;
}

static const struct {
    const char *name;
    feckless_umode_call_fn fn;
} s_guard[G_N] = {
    [G_REMOVE] = { "remove", g_remove },
    [G_UNLINK] = { "unlink", g_unlink },
    [G_MKDIR] = { "mkdir", g_mkdir },
    [G_RMDIR] = { "rmdir", g_rmdir },
    [G_UTIME] = { "utime", g_utime },
    [G_MARK_HIDDEN] = { "storage_mark_hidden", g_storage_mark_hidden },
    [G_RENAME] = { "rename", g_rename },
    [G_FOPEN] = { "fopen", g_fopen },
    [G_IO_OPEN] = { "storage_io_open", g_storage_io_open },
    [G_F_OPEN] = { "f_open", g_f_open },
    [G_F_CHMOD] = { "f_chmod", g_f_chmod },
};

static feckless_umode_call_t *s_table;
static uint32_t s_n;

const feckless_umode_call_t *jinxed_box_calls(uint32_t *n)
{
    if (!s_table) {
        uint32_t fn = 0;
        const feckless_umode_call_t *f = feckless_box_calls_v1(&fn);
        feckless_umode_call_t *t = heap_caps_malloc(fn * sizeof *t, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        if (!t) {
            *n = 0;
            return NULL;
        }
        memcpy(t, f, fn * sizeof *t);
        uint32_t found = 0;
        for (uint32_t i = 0; i < fn; i++) {
            for (int g = 0; g < G_N; g++) {
                if (t[i].fn && strcmp(t[i].name, s_guard[g].name) == 0) {
                    s_next[g] = t[i].fn;
                    t[i].fn = s_guard[g].fn;
                    found |= 1u << g;
                }
            }
        }
        /* Every wrapped name must be feckless's: one it no longer
         * answers (renamed, say) would be a wrapper with nothing to call,
         * and the call under its new name unguarded. No table, so the
         * load fails rather than run uncontained. */
        if (found != (1u << G_N) - 1) {
            free(t);
            *n = 0;
            return NULL;
        }
        s_table = t;
        s_n = fn;
    }
    *n = s_n;
    return s_table;
}

/*
 * jbox_storage.c -- the card, for a boxed editor (u-jinxed-0003).
 *
 * As feckless_box_calls' box_storage.c does for reading (u-feckless-0007,
 * 0015), and in its manner:
 *
 * - FILE is opaque to the app, so it gets a number for each (TOKEN_FILE +
 *   slot), and fileno() a number of its own (FD_BASE + slot) that only
 *   fsync() takes. The real FILE is the host's.
 * - Every pointer is checked against the box for its whole length before
 *   it is read or written.
 * - Reads and writes go through one internal, DMA-capable staging buffer
 *   in 8 KB slices, copied by the CPU: the box is in PSRAM with the
 *   framebuffer the panel scans out, and a card DMA there underruns the
 *   panel (u-feckless-0015). A write handed the box's buffer would DMA out
 *   of PSRAM the same way. Without the buffer they go direct.
 *
 * And, beyond it:
 *
 * - Paths and modes are copied out of the box before they are checked, so
 *   another thread of the app cannot change one between the check and the
 *   use. A path must be JINXED_BOX_ROOT or below it, with no ".." segment;
 *   anything else is EACCES. The box can write, so it is kept to the one
 *   directory the editor owns.
 * - A call that fails sets the app's errno -- the word at its thread's tp,
 *   which feckless_box_libc's __errno() returns (u-feckless-0023, 0042) --
 *   as doclog.c reads it after mkdir() and rename().
 * - The brief holds are counted per volume: the app releases only what it
 *   took, and jinxed_box_end() gives back the rest.
 *
 * struct stat is copied as the host has it. The host and the app's ELF
 * are built by the one toolchain against the one picolibc, so the layout
 * is the same; a host test on glibc is the host's layout on both sides.
 *
 * SPDX-License-Identifier: MIT
 */
#include "jinxed_box.h"

#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "esp_heap_caps.h"
#include "esp_random.h"
#include "feckless_box_calls.h"
#include "storage.h"

typedef feckless_umode_window_t win_t;
#define H(name) static bool name(win_t *w, const uint32_t a[7], uint32_t *ret)
#define FN(n)   { #n, c_##n, NULL, 0, 0 }
/* Touches no state of this file, may be called from any of the app's
 * threads at once (FECKLESS_UMODE_CALL_UNLOCKED). */
#define FNU(n)  { #n, c_##n, NULL, 0, FECKLESS_UMODE_CALL_UNLOCKED }

#define TOKEN_FILE  0x4a464c00u             /* "JFL\0" + slot: a FILE * */
#define FD_BASE     0x4a46u                 /* + slot: what fileno() gives */
#define SLICE       (8 * 1024)
#define MODE_MAX    4

static FILE *s_file[JINXED_BOX_FILES];
static int s_brief[STORAGE_COUNT];          /* the app's brief holds, not released */
static uint8_t *s_stage;

static uint8_t *stage(void)
{
    if (!s_stage) {
        s_stage = heap_caps_aligned_alloc(64, SLICE, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA | MALLOC_CAP_8BIT);
    }
    return s_stage;
}

static bool ok_ptr(const win_t *w, uint32_t p, uint32_t len)
{
    return feckless_umode_range_ok(w, (uintptr_t)p, len);
}

/* The app's errno: the word at tp, if tp is in the box. */
static void set_errno(win_t *w, int e)
{
    const uint32_t tp = w->ctx.x[4];
    if (ok_ptr(w, tp, sizeof(int))) {
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

/* The app's path at p, copied into out and allowed; on false, errno is
 * set (EFAULT for a bad pointer or an unterminated string, EACCES for a
 * path outside the root). */
static bool take_path(win_t *w, uint32_t p, char out[JINXED_BOX_PATH_MAX])
{
    if (!copy_str(w, p, out, JINXED_BOX_PATH_MAX)) {
        set_errno(w, EFAULT);
        return false;
    }
    if (!path_allowed(out)) {
        set_errno(w, EACCES);
        return false;
    }
    return true;
}

/* r, w or a, then at most + and b. */
static bool mode_allowed(const char *m)
{
    if (m[0] != 'r' && m[0] != 'w' && m[0] != 'a') {
        return false;
    }
    for (const char *s = m + 1; *s; s++) {
        if (*s != '+' && *s != 'b') {
            return false;
        }
    }
    return true;
}

static FILE *file_of(uint32_t t)
{
    const uint32_t i = t - TOKEN_FILE;
    return (t >= TOKEN_FILE && i < JINXED_BOX_FILES) ? s_file[i] : NULL;
}

/* ---- files ----------------------------------------------------------- */

H(c_fopen)
{
    char path[JINXED_BOX_PATH_MAX];
    char mode[MODE_MAX + 1];
    *ret = 0;
    if (!take_path(w, a[0], path)) {
        return true;
    }
    if (!copy_str(w, a[1], mode, sizeof mode) || !mode_allowed(mode)) {
        set_errno(w, EINVAL);
        return true;
    }
    int i = 0;
    while (i < JINXED_BOX_FILES && s_file[i]) i++;
    if (i == JINXED_BOX_FILES) {
        set_errno(w, EMFILE);
        return true;
    }
    s_file[i] = fopen(path, mode);
    if (!s_file[i]) {
        set_errno(w, errno);
        return true;
    }
    *ret = TOKEN_FILE + (uint32_t)i;
    return true;
}

H(c_fclose)
{
    FILE *f = file_of(a[0]);
    if (!f) {
        set_errno(w, EBADF);
        *ret = (uint32_t)EOF;
        return true;
    }
    s_file[a[0] - TOKEN_FILE] = NULL;
    const int r = fclose(f);
    if (r != 0) {
        set_errno(w, errno);
    }
    *ret = (uint32_t)r;
    return true;
}

H(c_fread)
{
    FILE *f = file_of(a[3]);
    const uint64_t len = (uint64_t)a[1] * a[2];
    *ret = 0;
    if (!f || len == 0 || len > UINT32_MAX || !ok_ptr(w, a[0], (uint32_t)len)) {
        return true;
    }
    uint8_t *const st = stage();
    if (!st) {
        *ret = (uint32_t)fread((void *)(uintptr_t)a[0], a[1], a[2], f);
        return true;
    }
    uint32_t done = 0;
    while (done < (uint32_t)len) {
        const uint32_t want = (uint32_t)len - done > SLICE ? SLICE : (uint32_t)len - done;
        const size_t g = fread(st, 1, want, f);
        memcpy((uint8_t *)(uintptr_t)a[0] + done, st, g);
        done += (uint32_t)g;
        if (g < want) {
            break;
        }
    }
    *ret = done / a[1];
    return true;
}

H(c_fwrite)
{
    FILE *f = file_of(a[3]);
    const uint64_t len = (uint64_t)a[1] * a[2];
    *ret = 0;
    if (!f || len == 0 || len > UINT32_MAX || !ok_ptr(w, a[0], (uint32_t)len)) {
        return true;
    }
    uint8_t *const st = stage();
    if (!st) {
        *ret = (uint32_t)fwrite((const void *)(uintptr_t)a[0], a[1], a[2], f);
        return true;
    }
    uint32_t done = 0;
    while (done < (uint32_t)len) {
        const uint32_t want = (uint32_t)len - done > SLICE ? SLICE : (uint32_t)len - done;
        memcpy(st, (const uint8_t *)(uintptr_t)a[0] + done, want);
        const size_t g = fwrite(st, 1, want, f);
        done += (uint32_t)g;
        if (g < want) {
            break;
        }
    }
    *ret = done / a[1];
    return true;
}

H(c_fgets)
{
    FILE *f = file_of(a[2]);
    const int n = (int)a[1];
    *ret = 0;
    if (!f || n <= 0 || !ok_ptr(w, a[0], (uint32_t)n)) {
        return true;
    }
    *ret = fgets((char *)(uintptr_t)a[0], n, f) ? a[0] : 0;
    return true;
}

/* long is 32 bits in the app. */
H(c_fseek)
{
    FILE *f = file_of(a[0]);
    if (!f) {
        set_errno(w, EBADF);
        *ret = (uint32_t)-1;
        return true;
    }
    const int r = fseek(f, (long)(int32_t)a[1], (int)a[2]);
    if (r != 0) {
        set_errno(w, errno);
    }
    *ret = (uint32_t)r;
    return true;
}

H(c_ftell)
{
    FILE *f = file_of(a[0]);
    long p = f ? ftell(f) : -1;
    if (p > INT32_MAX) {
        p = -1;                             /* does not fit the app's long */
        set_errno(w, EOVERFLOW);
    } else if (!f) {
        set_errno(w, EBADF);
    }
    *ret = (uint32_t)(int32_t)p;
    return true;
}

/* fflush(NULL) flushes every file the app has open, as C's does. */
H(c_fflush)
{
    int r = 0;
    if (a[0] == 0) {
        for (int i = 0; i < JINXED_BOX_FILES; i++) {
            if (s_file[i] && fflush(s_file[i]) != 0) r = EOF;
        }
    } else {
        FILE *f = file_of(a[0]);
        if (!f) {
            set_errno(w, EBADF);
            *ret = (uint32_t)EOF;
            return true;
        }
        r = fflush(f);
    }
    if (r != 0) {
        set_errno(w, errno);
    }
    *ret = (uint32_t)r;
    return true;
}

H(c_ferror)
{
    (void)w;
    FILE *f = file_of(a[0]);
    *ret = f ? (uint32_t)(ferror(f) != 0) : 1u;
    return true;
}

H(c_fileno)
{
    FILE *f = file_of(a[0]);
    if (!f) {
        set_errno(w, EBADF);
        *ret = (uint32_t)-1;
        return true;
    }
    *ret = FD_BASE + (a[0] - TOKEN_FILE);
    return true;
}

/* Only a number fileno() gave. */
H(c_fsync)
{
    const uint32_t i = a[0] - FD_BASE;
    FILE *f = (a[0] >= FD_BASE && i < JINXED_BOX_FILES) ? s_file[i] : NULL;
    if (!f) {
        set_errno(w, EBADF);
        *ret = (uint32_t)-1;
        return true;
    }
    const int r = fsync(fileno(f));
    if (r != 0) {
        set_errno(w, errno);
    }
    *ret = (uint32_t)r;
    return true;
}

/* ---- names ----------------------------------------------------------- */

H(c_stat)
{
    char path[JINXED_BOX_PATH_MAX];
    *ret = (uint32_t)-1;
    if (!ok_ptr(w, a[1], sizeof(struct stat))) {
        set_errno(w, EFAULT);
        return true;
    }
    if (!take_path(w, a[0], path)) {
        return true;
    }
    struct stat st;
    if (stat(path, &st) != 0) {
        set_errno(w, errno);
        return true;
    }
    memcpy((void *)(uintptr_t)a[1], &st, sizeof st);
    *ret = 0;
    return true;
}

H(c_mkdir)
{
    char path[JINXED_BOX_PATH_MAX];
    *ret = (uint32_t)-1;
    if (!take_path(w, a[0], path)) {
        return true;
    }
    if (mkdir(path, (mode_t)a[1]) != 0) {
        set_errno(w, errno);
        return true;
    }
    *ret = 0;
    return true;
}

H(c_rename)
{
    char from[JINXED_BOX_PATH_MAX], to[JINXED_BOX_PATH_MAX];
    *ret = (uint32_t)-1;
    if (!take_path(w, a[0], from) || !take_path(w, a[1], to)) {
        return true;
    }
    if (rename(from, to) != 0) {
        set_errno(w, errno);
        return true;
    }
    *ret = 0;
    return true;
}

H(c_unlink)
{
    char path[JINXED_BOX_PATH_MAX];
    *ret = (uint32_t)-1;
    if (!take_path(w, a[0], path)) {
        return true;
    }
    if (unlink(path) != 0) {
        set_errno(w, errno);
        return true;
    }
    *ret = 0;
    return true;
}

/* ---- the storage handler --------------------------------------------- */

H(c_storage_hold_brief)
{
    (void)w;
    *ret = 0;
    if (a[0] < STORAGE_COUNT) {
        storage_hold_brief((storage_id_t)a[0]);
        s_brief[a[0]]++;
    }
    return true;
}

/* Only a hold the app took. */
H(c_storage_release_brief)
{
    (void)w;
    *ret = 0;
    if (a[0] < STORAGE_COUNT && s_brief[a[0]] > 0) {
        s_brief[a[0]]--;
        storage_release_brief((storage_id_t)a[0]);
    }
    return true;
}

H(c_storage_mark_hidden)
{
    char path[JINXED_BOX_PATH_MAX];
    *ret = 0;
    if (take_path(w, a[0], path)) {
        storage_mark_hidden(path);
    }
    return true;
}

H(c_esp_random)
{
    (void)w;
    (void)a;
    *ret = esp_random();
    return true;
}

static const feckless_umode_call_t s_own[] = {
    FN(fopen), FN(fclose), FN(fread), FN(fwrite), FN(fgets), FN(fseek), FN(ftell),
    FN(fflush), FN(ferror), FN(fileno), FN(fsync),
    FN(stat), FN(mkdir), FN(rename), FN(unlink),
    FN(storage_hold_brief), FN(storage_release_brief), FN(storage_mark_hidden),
    FNU(esp_random),
};
#define N_OWN (sizeof s_own / sizeof s_own[0])

static feckless_umode_call_t *s_table;
static uint32_t s_n;

static bool own(const char *name)
{
    for (size_t i = 0; i < N_OWN; i++) {
        if (strcmp(s_own[i].name, name) == 0) return true;
    }
    return false;
}

const feckless_umode_call_t *jinxed_box_calls(uint32_t *n)
{
    if (!s_table) {
        uint32_t fn = 0;
        const feckless_umode_call_t *f = feckless_box_calls_v1(&fn);
        feckless_umode_call_t *t = heap_caps_malloc((N_OWN + fn) * sizeof *t, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        if (!t) {
            *n = 0;
            return NULL;
        }
        uint32_t k = 0;
        for (size_t i = 0; i < N_OWN; i++) t[k++] = s_own[i];
        for (uint32_t i = 0; i < fn; i++) {
            if (!own(f[i].name)) t[k++] = f[i];
        }
        s_table = t;
        s_n = k;
    }
    *n = s_n;
    return s_table;
}

void jinxed_box_end(void)
{
    for (int i = 0; i < JINXED_BOX_FILES; i++) {
        if (s_file[i]) {
            fclose(s_file[i]);
            s_file[i] = NULL;
        }
    }
    for (int v = 0; v < STORAGE_COUNT; v++) {
        while (s_brief[v] > 0) {
            s_brief[v]--;
            storage_release_brief((storage_id_t)v);
        }
    }
}

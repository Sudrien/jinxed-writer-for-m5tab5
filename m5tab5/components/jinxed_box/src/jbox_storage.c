/*
 * jbox_storage.c -- writes on the card kept to Jinxed's directory, for a
 * boxed editor (u-jinxed-0003, 0004, 0012).
 *
 * feckless_box_calls answers every storage call Jinxed's editor makes
 * (u-feckless-0047, 0050). A box keeps the app out of the host's memory,
 * not out of the card's files, so a write named outside JINXED_BOX_ROOT
 * is the editor's bug, and Jinxed refuses it.
 *
 * Since u-feckless-0055 that is one filter. feckless copies every path out
 * of the box into the host before it uses it, and calls the launcher's
 * filter on that copy for each call that would write: fopen and
 * storage_io_open in a writing mode, remove, unlink, rename (each end),
 * mkdir, rmdir, utime, storage_mark_hidden, and FatFs's f_open with a
 * write or create bit and f_chmod. What the filter allowed is what the
 * call does, whatever another thread of the app writes into the box
 * meanwhile. 0004's wrappers checked a copy and then passed feckless the
 * app's pointer; that race is gone with them.
 *
 * A path is allowed if it is JINXED_BOX_ROOT or below it, with no ".."
 * segment. FatFs's paths are its drive's ("0:/..."), never "/sd/...", so
 * every FatFs write is refused, as 0004 refused them; the editor does not
 * call FatFs. Reads are not filtered.
 *
 * SPDX-License-Identifier: MIT
 */
#include "jinxed_box.h"

#include <stdbool.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "feckless_box_calls.h"

bool jinxed_box_may_write(const char *p)
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

static feckless_umode_call_t *s_table;
static uint32_t s_n;

const feckless_umode_call_t *jinxed_box_calls(uint32_t *n)
{
    /* Each app's: feckless_box_calls_end() forgets both. */
    feckless_box_calls_write_filter(jinxed_box_may_write);
    static const char *const ns[] = { "jinxed" };   /* files.c's, display.c's: not the mail account's */
    feckless_box_calls_nvs(ns, 1);
    if (!s_table) {
        uint32_t fn = 0, on = 0;
        const feckless_umode_call_t *f = feckless_box_calls_v1(&fn);
        const feckless_umode_call_t *o = jinxed_box_own_calls(&on);
        feckless_umode_call_t *t = heap_caps_malloc((fn + on) * sizeof *t, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        if (!t) {
            *n = 0;
            return NULL;
        }
        memcpy(t, f, fn * sizeof *t);
        memcpy(t + fn, o, on * sizeof *t);
        s_table = t;
        s_n = fn + on;
    }
    *n = s_n;
    return s_table;
}

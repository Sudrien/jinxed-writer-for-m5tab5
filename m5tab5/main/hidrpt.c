/*
 * hidrpt.c -- see hidrpt.h.
 *
 * A short item is one prefix byte -- bSize in bits 0-1 (0, 1, 2 or 4
 * data bytes), bType in 2-3 (main, global, local), bTag in 4-7 -- and
 * its data, little-endian. 0xFE starts a long item: a size byte, a tag
 * byte, then that many bytes, skipped. The Usage Page is global and
 * carries over; a Usage of four bytes carries its own page in the high
 * half. Usage Minimum and Maximum make a range on the current page.
 *
 * SPDX-License-Identifier: MIT
 */
#include "hidrpt.h"

#include <string.h>

#define TYPE_GLOBAL     (1)
#define TYPE_LOCAL      (2)
#define TAG_USAGE_PAGE  (0x0)       /* global */
#define TAG_USAGE       (0x0)       /* local */
#define TAG_USAGE_MIN   (0x1)
#define TAG_USAGE_MAX   (0x2)

#define PAGE_KEYBOARD   (0x0007)
#define PAGE_CONSUMER   (0x000C)
#define PAGE_APPLE      (0x00FF)
#define APPLE_FN        (0x0003)
#define CONSUMER_GLOBE  (0x029D)

static void usage(hidrpt_t *r, uint32_t page, uint32_t u)
{
    if (page == PAGE_APPLE && u == APPLE_FN) r->fn = true;
    if (page == PAGE_CONSUMER && u == CONSUMER_GLOBE) r->globe = true;
    if (page >= 0xFF00 && page <= 0xFFFF) r->vendor = true;
    if (page == PAGE_KEYBOARD && u <= 0xFFFF && u > r->keys_max) r->keys_max = (uint16_t)u;
}

void hidrpt_scan(const uint8_t *d, size_t n, hidrpt_t *r)
{
    memset(r, 0, sizeof(*r));
    uint32_t page = 0, umin = 0;
    bool have_min = false;
    size_t i = 0;
    while (i < n) {
        const uint8_t p = d[i++];
        if (p == 0xFE) {                        /* long item */
            if (n - i < 2) return;
            const size_t len = d[i];
            i += 2;
            if (n - i < len) return;
            i += len;
            continue;
        }
        static const uint8_t k_size[4] = { 0, 1, 2, 4 };
        const size_t sz = k_size[p & 3];
        if (n - i < sz) return;
        uint32_t v = 0;
        for (size_t b = 0; b < sz; b++) v |= (uint32_t)d[i + b] << (8 * b);
        i += sz;
        r->items++;

        const int type = (p >> 2) & 3, tag = p >> 4;
        if (type == TYPE_GLOBAL && tag == TAG_USAGE_PAGE) {
            page = v & 0xFFFF;
            if (page >= 0xFF00) r->vendor = true;
        } else if (type == TYPE_LOCAL && tag == TAG_USAGE) {
            if (sz == 4) usage(r, v >> 16, v & 0xFFFF);
            else         usage(r, page, v);
        } else if (type == TYPE_LOCAL && tag == TAG_USAGE_MIN) {
            umin = sz == 4 ? v & 0xFFFF : v;
            have_min = true;
        } else if (type == TYPE_LOCAL && tag == TAG_USAGE_MAX) {
            const uint32_t umax = sz == 4 ? v & 0xFFFF : v;
            const uint32_t pg = sz == 4 ? v >> 16 : page;
            if (have_min && umin <= umax) {
                if (pg == PAGE_CONSUMER && umin <= CONSUMER_GLOBE && CONSUMER_GLOBE <= umax) r->globe = true;
                if (pg == PAGE_APPLE && umin <= APPLE_FN && APPLE_FN <= umax) r->fn = true;
                usage(r, pg, umax);             /* keys_max, and the vendor note */
            }
            have_min = false;
        } else if (type == 0) {                 /* a main item ends the locals */
            have_min = false;
        }
    }
    r->ok = true;
}

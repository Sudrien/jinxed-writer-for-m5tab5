/*
 * hidrpt.h -- what a HID report descriptor says a device has (0025).
 *
 * Only enough of HID 1.11 section 6.2.2 to answer one question: does
 * this keyboard have keys only Apple's have? Two are looked for:
 *
 *   fn     Apple's vendor page 0x00FF, usage 0x0003 -- the Fn key as
 *          Apple's keyboards report it (Linux's hid-apple reads it there)
 *   globe  Consumer page 0x000C, usage 0x029D, AC Next Keyboard Layout
 *          Select -- the globe key on Apple's keyboards since 2020, also
 *          on some third-party "for Mac" ones
 *
 * Pure C, no IDF, so ./test/run.sh runs it on the host.
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    bool     fn;            /* 0x00FF:0x0003 */
    bool     globe;         /* 0x000C:0x029D, as a usage or in a range */
    bool     vendor;        /* any usage page 0xFF00-0xFFFF: noted, not Apple's */
    uint16_t keys_max;      /* highest keyboard-page (0x07) usage named */
    int      items;         /* short items read */
    bool     ok;            /* read to the end without running over */
} hidrpt_t;

void hidrpt_scan(const uint8_t *d, size_t n, hidrpt_t *out);

/* Either Apple-only key. */
static inline bool hidrpt_apple(const hidrpt_t *r) { return r->fn || r->globe; }

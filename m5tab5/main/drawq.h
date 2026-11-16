/*
 * drawq.h -- when the editor redraws (u-jinxed-0021).
 *
 * A redraw is tens of milliseconds (a whole-screen blit at 60 or more on
 * a board) and a key a fraction of one, so drawing after every key let a
 * fast typist fill kbd.c's queue and lose presses. Instead a key that
 * changes the screen owes a redraw, and the loop looks at once for the
 * next key; the redraw is paid when the loop finds no key waiting, or
 * once `max` ticks have passed since it was owed, so a stream of keys
 * still shows as it is typed.
 *
 * Header-only, so the timing is tested on the host
 * (test/drawq_host_test.c) against kbd.c's queue rule (kbdq.h).
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    bool     owed;
    uint32_t since;     /* ticks, when it was first owed */
} drawq_t;

/* The screen changed. */
static inline void drawq_dirty(drawq_t *d, uint32_t now)
{
    if (!d->owed) {
        d->owed = true;
        d->since = now;
    }
}

/* How long the loop may wait for a key: not at all while a redraw is
 * owed. */
static inline uint32_t drawq_wait(const drawq_t *d, uint32_t wait)
{
    return d->owed ? 0 : wait;
}

/* After a pass of the loop that did (got) or did not find a key: true if
 * the redraw is due now, and it is then no longer owed. */
static inline bool drawq_due(drawq_t *d, bool got, uint32_t now, uint32_t max)
{
    if (!d->owed || (got && now - d->since < max)) return false;
    d->owed = false;
    return true;
}

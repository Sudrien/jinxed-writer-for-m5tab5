/*
 * kbdq.h -- which key events go into kbd.c's queue (u-jinxed-0020).
 *
 * The editor repeats a held key until it sees that key's release. A
 * release the queue had no room for was dropped, like any other event,
 * and the key then repeated for good (a board did it: the editor busy
 * redrawing, the queue full, a space's release dropped, spaces for 50 s).
 *
 * So a press is admitted only while more than KBD_RELEASE_ROOM places
 * are free, and a release only when its press was admitted. The room
 * is then always enough: a release is admitted only for a key whose
 * press is in the queue or already taken, which is a key the keyboard
 * holds down now, and a boot report holds at most six of those per
 * keyboard. A release whose press was dropped is dropped too: the
 * editor never saw that key go down, and the release would take room
 * that a held key's needs.
 *
 * Header-only, so the policy is tested on the host (test/kbdq_host_test.c)
 * apart from the USB stack.
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#define KBD_REPORT_KEYS     (6)     /* a boot report's key array */

/* What each keyboard's presses were: one bit a usage, set when a press
 * went into the queue, cleared when its release did. Zeroed with the
 * keyboard's context. */
typedef struct {
    uint8_t bit[32];
} kbdq_sent_t;

static inline bool kbdq_was_sent(const kbdq_sent_t *s, uint8_t usage)
{
    return (s->bit[usage >> 3] >> (usage & 7)) & 1;
}

/*
 * Whether an event may go in, with `spaces` places free in the queue and
 * `room` of them kept for releases (KBD_REPORT_KEYS for each keyboard
 * that can be open). True for a press with more than `room` free, and
 * for the release of a key whose press went in. Records nothing: call
 * kbdq_sent() once the event is actually in.
 */
static inline bool kbdq_admit(const kbdq_sent_t *s, uint8_t usage, bool down,
                              unsigned spaces, unsigned room)
{
    if (down) return spaces > room;
    return spaces > 0 && kbdq_was_sent(s, usage);
}

/* The event went in. */
static inline void kbdq_sent(kbdq_sent_t *s, uint8_t usage, bool down)
{
    if (down) s->bit[usage >> 3] |= (uint8_t)(1u << (usage & 7));
    else s->bit[usage >> 3] &= (uint8_t)~(1u << (usage & 7));
}

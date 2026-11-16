/*
 * kbdq_host_test.c -- kbd.c's queue policy (main/kbdq.h, u-jinxed-0020).
 *
 * The queue is modelled as kbd.c sizes it (64 + 6 per keyboard), filled
 * by up to four keyboards whose reports hold at most six keys each, as a
 * boot report does, and drained by an editor that is sometimes quick,
 * sometimes slow and sometimes stalled for a long while (a redraw, a
 * page). Two things must hold:
 *
 *   - a key the editor saw go down, it sees come up: once every key is
 *     let go and the queue drained, the editor holds nothing; and it
 *     never sees a release for a key it does not hold;
 *   - a release kbdq admits always finds a place (kbd.c's "no room for
 *     key's release" never happens).
 *
 * With `old` as the first argument the policy is kbd.c's before 0020
 * (anything that fits goes in), which must fail the first.
 *
 * SPDX-License-Identifier: MIT
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "kbdq.h"

#define KBDS        4
#define ROOM        (KBD_REPORT_KEYS * KBDS)
#define QLEN        (64 + ROOM)

typedef struct { uint8_t kbd, usage; bool down; } ev_t;

static ev_t q[QLEN];
static unsigned q_head, q_n;
static bool old_policy;
static unsigned long admitted, dropped_press, dropped_release, no_room, ghost_release;

static kbdq_sent_t sent[KBDS];
static uint8_t held[KBDS][KBD_REPORT_KEYS];     /* what each keyboard holds now */
static int n_held[KBDS];
static bool editor_down[KBDS][256];             /* what the editor thinks is down */

static void post(int k, uint8_t usage, bool down)
{
    const unsigned spaces = QLEN - q_n;
    bool ok;
    if (old_policy) ok = spaces > 0;
    else ok = kbdq_admit(&sent[k], usage, down, spaces, ROOM);
    if (!ok) {
        if (down) dropped_press++;
        else if (!old_policy && kbdq_was_sent(&sent[k], usage)) no_room++;
        else dropped_release++;
        return;
    }
    if (q_n == QLEN) { no_room++; return; }      /* the xQueueSend that fails */
    q[(q_head + q_n++) % QLEN] = (ev_t){ (uint8_t)k, usage, down };
    if (!old_policy) kbdq_sent(&sent[k], usage, down);
    admitted++;
}

static bool take(void)
{
    if (!q_n) return false;
    const ev_t e = q[q_head];
    q_head = (q_head + 1) % QLEN;
    q_n--;
    if (e.down) {
        editor_down[e.kbd][e.usage] = true;
    } else {
        if (!editor_down[e.kbd][e.usage]) ghost_release++;
        editor_down[e.kbd][e.usage] = false;
    }
    return true;
}

static void press(int k, uint8_t usage)
{
    for (int i = 0; i < n_held[k]; i++) if (held[k][i] == usage) return;
    if (n_held[k] == KBD_REPORT_KEYS) return;
    held[k][n_held[k]++] = usage;
    post(k, usage, true);
}

static void release(int k, int i)
{
    const uint8_t usage = held[k][i];
    held[k][i] = held[k][--n_held[k]];
    post(k, usage, false);
}

static int still_down(void)
{
    int n = 0;
    for (int k = 0; k < KBDS; k++) for (int u = 0; u < 256; u++) n += editor_down[k][u];
    return n;
}

static int run(unsigned seed, int kbds, long steps)
{
    srand(seed);
    memset(sent, 0, sizeof sent);
    memset(n_held, 0, sizeof n_held);
    memset(editor_down, 0, sizeof editor_down);
    q_head = q_n = 0;
    int stall = 0;
    for (long s = 0; s < steps; s++) {
        const int k = rand() % kbds;
        if (n_held[k] && (rand() % 3 == 0 || n_held[k] == KBD_REPORT_KEYS)) release(k, rand() % n_held[k]);
        else press(k, (uint8_t)(0x04 + rand() % 60));
        /* the editor: quick, slow, or stalled for a while */
        if (stall > 0) { stall--; continue; }
        const int r = rand() % 100;
        if (r < 3) stall = 50 + rand() % 400;
        else if (r < 60) take();
        else if (r < 80) { take(); take(); }
    }
    /* every key let go -- with the editor still stalled -- then drained */
    for (int k = 0; k < kbds; k++) while (n_held[k]) release(k, 0);
    while (take()) {}
    return still_down();
}

int main(int argc, char **argv)
{
    old_policy = argc > 1 && strcmp(argv[1], "old") == 0;
    int fails = 0, stuck_total = 0;
    for (unsigned seed = 1; seed <= 400; seed++) {
        const int kbds = 1 + (int)(seed % KBDS);
        const int stuck = run(seed, kbds, 20000);
        stuck_total += stuck;
        if (stuck) fails++;
    }
    /* The one the board showed: the editor stalls, the queue fills with
     * presses, a key held through it is let go. */
    memset(sent, 0, sizeof sent);
    memset(n_held, 0, sizeof n_held);
    memset(editor_down, 0, sizeof editor_down);
    q_head = q_n = 0;
    press(0, 0x2C);                          /* space down, and taken */
    take();
    for (int i = 0; i < 200; i++) { press(0, 0x15); release(0, n_held[0] - 1); }   /* r, typed hard */
    release(0, 0);                           /* space up, the queue full */
    while (take()) {}
    const bool space_stuck = editor_down[0][0x2C];

    printf("kbdq %s: %d of 400 runs left %d keys down in the editor; space %s; "
           "%lu admitted, %lu presses and %lu releases dropped, %lu with no room, %lu ghost releases\n",
           old_policy ? "old" : "new", fails, stuck_total, space_stuck ? "STUCK" : "released",
           admitted, dropped_press, dropped_release, no_room, ghost_release);
    const bool ok = fails == 0 && !space_stuck && no_room == 0 && ghost_release == 0;
    if (old_policy) {
        /* The old policy is expected to fail; succeed only if it does. */
        printf(ok ? "kbdq old: FAIL -- the old policy should lose a release and did not\n"
                  : "kbdq old: loses releases, as it did on the board\n");
        return ok ? 1 : 0;
    }
    printf(ok ? "kbdq: ok\n" : "kbdq: FAIL\n");
    return ok ? 0 : 1;
}

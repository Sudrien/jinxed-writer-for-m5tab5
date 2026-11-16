/*
 * drawq_host_test.c -- the editor's redraws against a fast typist
 * (main/drawq.h, u-jinxed-0021).
 *
 * The editor's loop is modelled a millisecond at a time: wait for a key
 * (30 ms, the touch poll, or none while a redraw is owed), handle it,
 * do the loop's other work, and redraw when due. A redraw costs what
 * the board's call stats showed one costing in the box: a 60 ms blit, two
 * 10 ms fills and about 15 rows of text, 100 ms in all. The typist
 * presses a key every 1000/rate ms and holds it 60 ms; presses and
 * releases go into an 88-place queue by kbd.c's rule (kbdq.h), with the
 * keys arriving while the editor is busy, as on the board.
 *
 * The old loop (a redraw after every key that changes the screen) must
 * drop presses at a brisk typing rate, since 100 ms a redraw is 10 keys
 * a second. The new one must drop none at up to 20 keys a second, and
 * show every key within DRAW_OWED_MS plus a redraw or two of its press.
 *
 * SPDX-License-Identifier: MIT
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "drawq.h"
#include "kbdq.h"

#define ROOM        (KBD_REPORT_KEYS * 4)
#define QLEN        (64 + ROOM)
#define REDRAW_MS   100
#define KEY_MS      1           /* a key's handling, its host calls in a box */
#define PASS_MS     1           /* the loop's other work: ticks, battery, idle */
#define POLL_MS     30
#define HOLD_MS     60
#define DRAW_OWED_MS 150        /* editor.c's */
#define RUN_MS      60000

typedef struct { uint8_t usage; bool down; uint32_t at; } ev_t;

static ev_t q[QLEN];
static unsigned qh, qn;
static kbdq_sent_t sent;
static uint32_t now;

/* The typist's next events, in time order. */
static uint32_t period, next_press, n_pressed;
static struct { uint8_t usage; uint32_t up_at; bool live; } holding[8];
static unsigned long pressed, dropped;

static void arrive(uint8_t usage, bool down, uint32_t at)
{
    if (!kbdq_admit(&sent, usage, down, QLEN - qn, ROOM)) { if (down) dropped++; return; }
    q[(qh + qn++) % QLEN] = (ev_t){ usage, down, at };
    kbdq_sent(&sent, usage, down);
}

/* Everything the typist did up to `now`. */
static void typist(void)
{
    for (;;) {
        uint32_t t = UINT32_MAX;
        int which = -1;
        for (int i = 0; i < 8; i++) if (holding[i].live && holding[i].up_at < t) { t = holding[i].up_at; which = i; }
        if (next_press < t && next_press < RUN_MS) { t = next_press; which = -2; }
        if (t > now || which == -1) return;
        if (which == -2) {
            const uint8_t usage = (uint8_t)(0x04 + n_pressed % 26);
            n_pressed++;
            pressed++;
            arrive(usage, true, t);
            for (int i = 0; i < 8; i++) if (!holding[i].live) {
                holding[i].live = true; holding[i].usage = usage; holding[i].up_at = t + HOLD_MS; break;
            }
            next_press += period;
        } else {
            holding[which].live = false;
            arrive(holding[which].usage, false, t);
        }
    }
}

static void spend(uint32_t ms) { now += ms; typist(); }

/* Taken and waiting for a redraw: when each key was pressed. */
static uint32_t undrawn[4096];
static unsigned n_undrawn;
static uint32_t worst_ms;
static unsigned long redraws, latency_sum, drawn;

static void run(bool batched, int rate, unsigned long *out_dropped, uint32_t *out_worst, double *out_mean,
                unsigned long *out_redraws)
{
    memset(&sent, 0, sizeof sent);
    memset(holding, 0, sizeof holding);
    qh = qn = 0;
    now = 0;
    period = 1000u / (uint32_t)rate;
    next_press = 500;
    n_pressed = pressed = dropped = 0;
    n_undrawn = 0;
    worst_ms = 0;
    redraws = latency_sum = drawn = 0;
    drawq_t d = { 0 };

    while (now < RUN_MS + 2000) {
        /* kbd_get(wait) */
        uint32_t wait = batched ? drawq_wait(&d, POLL_MS) : POLL_MS;
        bool got = false, dirty = false;
        for (;;) {
            typist();
            if (qn) {
                const ev_t e = q[qh];
                qh = (qh + 1) % QLEN;
                qn--;
                got = true;
                spend(KEY_MS);
                if (e.down) { dirty = true; if (n_undrawn < 4096) undrawn[n_undrawn++] = e.at; }
                break;
            }
            if (!wait) break;
            wait--;
            spend(1);
        }
        spend(PASS_MS);
        bool draw_now;
        if (batched) {
            if (dirty) drawq_dirty(&d, now);
            draw_now = drawq_due(&d, got, now, DRAW_OWED_MS);
        } else {
            draw_now = dirty;
        }
        if (draw_now) {
            spend(REDRAW_MS);
            redraws++;
            for (unsigned i = 0; i < n_undrawn; i++) {
                const uint32_t lat = now - undrawn[i];
                if (lat > worst_ms) worst_ms = lat;
                latency_sum += lat;
                drawn++;
            }
            n_undrawn = 0;
        }
    }
    *out_dropped = dropped;
    *out_worst = worst_ms;
    *out_mean = drawn ? (double)latency_sum / (double)drawn : 0.0;
    *out_redraws = redraws;
}

/* The rule itself, a step at a time. */
static int unit(void)
{
    int f = 0;
#define U(c) do { if (!(c)) { printf("FAIL drawq unit: %s\n", #c); f++; } } while (0)
    drawq_t d = { 0 };
    U(drawq_wait(&d, 30) == 30);                 /* nothing owed: wait as asked */
    U(!drawq_due(&d, false, 0, 150));            /* nothing owed: nothing due */
    drawq_dirty(&d, 1000);
    U(drawq_wait(&d, 30) == 0);                  /* owed: look, do not wait */
    U(!drawq_due(&d, true, 1100, 150));          /* a key came: later */
    drawq_dirty(&d, 1120);                       /* owed since 1000 still */
    U(!drawq_due(&d, true, 1149, 150));
    U(drawq_due(&d, true, 1150, 150));           /* keys still coming, but 150 ticks: now */
    U(!d.owed && drawq_wait(&d, 30) == 30);
    drawq_dirty(&d, 2000);
    U(drawq_due(&d, false, 2001, 150));          /* no key waiting: now */
    U(!drawq_due(&d, false, 2002, 150));         /* and once */
    drawq_dirty(&d, UINT32_MAX - 10);            /* the tick count wrapping */
    U(!drawq_due(&d, true, 100, 150));
    U(drawq_due(&d, true, 140, 150));
#undef U
    return f;
}

int main(void)
{
    static const int rates[] = { 5, 8, 10, 12, 15, 20 };
    int fails = unit();
    bool old_drops = false;
    for (size_t i = 0; i < sizeof rates / sizeof rates[0]; i++) {
        unsigned long od, nd, orr, nr;
        uint32_t ow, nw;
        double om, nm;
        run(false, rates[i], &od, &ow, &om, &orr);
        run(true, rates[i], &nd, &nw, &nm, &nr);
        printf("drawq: %2d keys/s for %d s: old %4lu dropped, worst %5u ms, mean %5.0f ms, %4lu redraws; "
               "new %4lu dropped, worst %4u ms, mean %4.0f ms, %4lu redraws\n",
               rates[i], RUN_MS / 1000, od, (unsigned)ow, om, orr, nd, (unsigned)nw, nm, nr);
        if (od) old_drops = true;
        if (nd) { printf("FAIL: the new loop dropped presses at %d keys/s\n", rates[i]); fails++; }
        if (nw > DRAW_OWED_MS + 2 * REDRAW_MS + 2 * POLL_MS) {
            printf("FAIL: a key waited %u ms to be shown at %d keys/s\n", (unsigned)nw, rates[i]);
            fails++;
        }
    }
    if (!old_drops) { printf("FAIL: the old loop should drop presses at these rates (is the model right?)\n"); fails++; }
    printf(fails ? "drawq: FAIL\n" : "drawq: ok\n");
    return fails ? 1 : 0;
}

/*
 * power.h -- soft power-off with the RX8130's alarm armed, and the dark
 * wake it brings. Spec sections 2 and 3.
 *
 * Sleep is a full power-down: PWROFF_PULSE (expander 0x44, P4) turns the
 * board off and only the power button or the RTC's alarm turns it on.
 * The alarm is armed only when there is something to send -- the open
 * note has seqs past the last one sent, and email is configured -- so a
 * wake never costs a boot for nothing.
 *
 * Until the log's checkpoint records come (spec 4 and 6), "sent" is
 * kept in NVS: the note's name and the last seq a send covered
 * (`jinxed`, `sentname` / `sentseq`). A swapped card or a stale value
 * costs at worst one duplicate send, never a missed one: a name or seq
 * that does not match counts as unsent.
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* The send interval, and the most backoff doubles it to (spec 3). */
#define POWER_INTERVAL_S        (3600)
#define POWER_BACKOFF_MAX_S     (7200)

/*
 * 0054: the idle steps, Defeatist's (its screendim.h and powerdown.h):
 * a row of five on the settings page, Never and four times, each kept
 * in NVS (`jinxed`, `idledim` / `idleoff` / `idlepwr`). Off is never
 * sooner than the dim (screenoff_effective()), and power-off never
 * sooner than off. The defaults are a writer's, not the player's:
 * dim at 30 s, dark at 2 min, off at 30 min.
 */
#define POWER_DIM       (0)
#define POWER_DARK      (1)
#define POWER_OFF       (2)
#define POWER_KINDS     (3)
#define POWER_STEPS     (5)     /* 0 Never, then 1..4 */

int         power_step(int kind);
void        power_set_step(int kind, int step);
const char *power_step_label(int kind, int step);
/* The wait for `kind` as it applies, clamped behind the one before it;
 * 0 for never. */
int         power_idle_seconds(int kind);

/* Was this boot the alarm's? AF cleared and AIE off either way (spec 2,
 * rule 5). Once, straight after rtc8130_init(). */
bool power_woke(void);

/* After a send of `name` through `seq` succeeded. */
void power_note_sent(const char *name, uint32_t seq);

/*
 * The idle power-off (spec 5): the log flushed and exported already, by
 * the caller. Keeps `name` for the dark wake, arms the alarm a send
 * interval on when `seq` is unsent and email is configured, and powers
 * off. Returns only if the board is still running after the pulses.
 */
void power_off_idle(const char *name, uint32_t seq);

/*
 * A scheduled wake: the screen dark, the display, touch and keyboard
 * never started. After nvsh_init(), storage_init() and sender_init().
 * Opens the note kept at power-off, sends it if anything is unsent,
 * and powers off -- the alarm off on success or with nothing to do, at
 * the backed-off interval on failure. Does not return.
 */
void power_dark_wake(void);

/* The last dark wake's result, once: true and the line in `line` the
 * first time the editor asks after it, false after. */
bool power_wake_line(char *line, size_t size);

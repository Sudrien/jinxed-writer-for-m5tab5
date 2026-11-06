/*
 * settings.h -- the gear, and the full-window settings page behind it.
 * Spec section 7.
 *
 * The page scrolls: drag it, or Up/Down, PageUp/PageDown, GUI+Up/Down.
 * Escape, GUI+comma or the Back button close it. Sections so far:
 *
 *   Email (SMTP)   the outgoing account, feckless_mail's mailcfg in NVS;
 *                  Edit opens mailform full-window, typed on the USB
 *                  keyboard or tapped; Forget clears it (tap twice);
 *                  Send test email (or GUI+T) sends one through the
 *                  send task, its progress and result beside the button
 *   Libraries      every library this build was made from, its version
 *                  and licence, from feckless_libversions' table
 *
 * Runs on the calling task (the editor's) until closed, reading the key
 * queue and the touch panel itself.
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <stdbool.h>

/* The gear, drawn in the status line's right-hand corner. */
void settings_draw_gear(void);

/* How much of the status line's right end the gear takes (0022: it
 * grows with the footer's text). */
int  settings_gear_w(void);

/* Whether a tap at (x, y), in screen coordinates, is on the gear.
 * Generous: the whole bottom-right corner. */
bool settings_gear_hit(int x, int y);

/* The page, until it is closed. Leaves the screen for the caller to
 * redraw, and swallows the press that closed it. */
void settings_page(void);

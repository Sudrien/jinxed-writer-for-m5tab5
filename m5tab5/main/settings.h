/*
 * settings.h -- the gear, and the full-window settings page behind it.
 * Spec section 7.
 *
 * The page scrolls: drag it, or Up/Down, PageUp/PageDown, Home/End,
 * and Command+Up/Down on an Apple keyboard. Escape, Cmd+comma or the
 * Back button close it (Cmd: Command on an Apple keyboard, Ctrl on any
 * other; kbd.h). Sections so far:
 *
 *   Email (SMTP)   the outgoing account, feckless_mail's mailcfg in NVS;
 *                  Edit opens mailform full-window, typed on the USB
 *                  keyboard or tapped; Forget clears it (tap twice);
 *                  Send test email (or Cmd+T) sends one through the
 *                  send task, its progress and result beside the button;
 *                  Send notes now (or Cmd+Enter) sends today's text (0038)
 *   Sleep          dim, screen off and power off after, Defeatist's
 *                  steps (0054, power.h)
 *   Attribution    the name the user's edits are credited to in the log
 *                  (0035), "user" until one is set; Change name, or
 *                  Cmd+N, opens a one-line field
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

/* 0035: the name the user's edits are credited to, kept in NVS
 * (`jinxed`, `author`); NULL while it is the default, "user". */
const char *settings_author(void);

/* 0038: what Send notes now (and Cmd+Enter here) does -- the editor's
 * send of today's text. False when the send could not start. */
void settings_on_send(bool (*send)(void));

/* The page, until it is closed. Leaves the screen for the caller to
 * redraw, and swallows the press that closed it. */
void settings_page(void);

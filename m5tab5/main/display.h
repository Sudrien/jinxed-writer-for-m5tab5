/*
 * display.h -- the panel, dim, whichever way up.
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <stdint.h>

#include "esp_err.h"
#include "gfx.h"

/* The theme: dark, low contrast (spec section 5). */
#define COL_BG      RGB(0x10, 0x10, 0x12)
#define COL_TEXT    RGB(0x9A, 0x9A, 0x90)
#define COL_DIM     RGB(0x50, 0x50, 0x4C)
#define COL_CURSOR  RGB(0x70, 0x70, 0x68)

/* The settings page and its form: the same dark, a step lighter for
 * things to press. */
#define COL_HEAD    RGB(0x1C, 0x1C, 0x20)
#define COL_RULE    RGB(0x2C, 0x2C, 0x30)
#define COL_BTN     RGB(0x30, 0x30, 0x36)
#define COL_FIELD   RGB(0x1A, 0x1A, 0x1E)
#define COL_EDGE    RGB(0x40, 0x40, 0x46)
#define COL_GO      RGB(0x2E, 0x50, 0x3A)
#define COL_BAD     RGB(0xA0, 0x58, 0x50)

/*
 * Which way up, in GFX_ROT_* quarter turns: chosen on the settings page
 * (0019) and kept in NVS, so it holds from the first frame of the next
 * boot. The glass is 720x1280 portrait; a writerdeck is held landscape,
 * GFX_ROT_90 out of the box, and GFX_ROT_270 is the same turned round.
 */
#define DISPLAY_ROTATION_DEFAULT    GFX_ROT_90

int  display_rotation(void);

/* Turn the picture, and the touch with it, and keep the angle. A touch
 * still down is swallowed, so the finger that chose the angle does not
 * land on whatever is under it now. The caller redraws everything. */
void display_set_rotation(int quarter_turns);

/* Backlight, percent. Low on purpose. */
#define DISPLAY_BACKLIGHT   (30)

/* Panel, gfx, rotation, a cleared screen, then the backlight. After
 * tab5io_init(). */
esp_err_t display_init(void);

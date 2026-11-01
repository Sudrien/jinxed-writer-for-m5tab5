/*
 * display.h -- the panel, landscape, dim.
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

/*
 * Which way up. The glass is 720x1280 portrait; a writerdeck is held
 * landscape. GFX_ROT_90 or GFX_ROT_270 -- which is the right one for
 * the keyboard in front of the USB-A port has not been checked on a
 * board yet; flip it here if the text is upside down.
 */
#define DISPLAY_ROTATION    GFX_ROT_90

/* Backlight, percent. Low on purpose. */
#define DISPLAY_BACKLIGHT   (30)

/* Panel, gfx, rotation, a cleared screen, then the backlight. After
 * tab5io_init(). */
esp_err_t display_init(void);

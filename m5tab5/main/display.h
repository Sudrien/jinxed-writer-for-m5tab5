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

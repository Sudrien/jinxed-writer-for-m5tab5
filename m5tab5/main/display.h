/*
 * display.h -- the panel, dim, whichever way up.
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <stdint.h>

#include "esp_err.h"
#include "gfx.h"

/*
 * The theme (0021): Dark, Light, Green console or Orange console, chosen
 * on the settings page and kept in NVS like the rotation. Dark is spec
 * section 5's dim, low-contrast default. COL_* read the current one, so
 * a change shows on the next redraw.
 */
typedef struct {
    uint16_t bg, text, dim, cursor;     /* the editor */
    uint16_t head, rule, btn, field, edge, go, bad;     /* pages, forms */
} display_theme_t;

#define DISPLAY_THEME_DARK      (0)
#define DISPLAY_THEME_LIGHT     (1)
#define DISPLAY_THEME_GREEN     (2)
#define DISPLAY_THEME_ORANGE    (3)
#define DISPLAY_THEMES          (4)

extern const display_theme_t *display_col;

int  display_theme(void);
/* Choose and keep a theme. The caller redraws everything. */
void display_set_theme(int theme);

#define COL_BG      (display_col->bg)
#define COL_TEXT    (display_col->text)
#define COL_DIM     (display_col->dim)
#define COL_CURSOR  (display_col->cursor)
#define COL_HEAD    (display_col->head)
#define COL_RULE    (display_col->rule)
#define COL_BTN     (display_col->btn)
#define COL_FIELD   (display_col->field)
#define COL_EDGE    (display_col->edge)
#define COL_GO      (display_col->go)
#define COL_BAD     (display_col->bad)

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

/*
 * The text size (0022): the editor's glyphs, ark12 scaled 2 to 5 times,
 * 3 out of the box. Chosen with Ctrl+= / Ctrl+- (Command on an Apple
 * keyboard) in the editor or on the settings page, and kept in NVS. The status line, the gear and the
 * spelling view's small print go one step smaller, never under 2, and
 * no larger than the status line fits across the screen. The settings
 * page itself stays as it is.
 */
#define DISPLAY_SCALE_MIN       (2)
#define DISPLAY_SCALE_MAX       (5)
#define DISPLAY_SCALE_DEFAULT   (3)

int  display_text_scale(void);
int  display_foot_scale(void);
/* Clamped to MIN..MAX and kept. The caller re-measures and redraws. */
void display_set_text_scale(int scale);

/* Backlight, percent. Low on purpose. */
#define DISPLAY_BACKLIGHT   (30)

/* Panel, gfx, rotation, a cleared screen, then the backlight. After
 * tab5io_init(). */
esp_err_t display_init(void);

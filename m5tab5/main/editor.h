/*
 * editor.h -- the text, the cursor, and the screen.
 *
 * In memory only, for now: nothing here reaches the SD card, and a
 * power-off loses the lot. The operation log and the history dotfiles
 * (spec section 4) come later and will sit under this.
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include "esp_err.h"

/* Allocate the buffer and draw the empty page. After display_init(). */
esp_err_t editor_init(void);

/* Read keys and edit, for ever. Runs on the calling task. */
void editor_run(void);

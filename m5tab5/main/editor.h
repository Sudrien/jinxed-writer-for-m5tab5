/*
 * editor.h -- the text, the cursor, and the screen.
 *
 * The text lives in RAM and is flushed to the operation log on the SD
 * card (doclog.h) whenever typing pauses. Export, checkpoints and the
 * idle power-off flush come later.
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include "esp_err.h"

/* Allocate the buffers, recover today's document from the card if one
 * is in, and draw it. After display_init(), storage_init() and
 * rtc8130_init(). */
esp_err_t editor_init(void);

/* Read keys and edit, for ever. Runs on the calling task. */
void editor_run(void);

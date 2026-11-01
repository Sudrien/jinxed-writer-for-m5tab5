/*
 * main.c -- jinxed writer.
 *
 * SPDX-License-Identifier: MIT
 */
#include "esp_log.h"
#include "tab5io.h"

#include "display.h"

static const char *TAG = "jinxed";

void app_main(void)
{
    /* First thing: the bus, and the expanders holding the panel's
     * reset line. Spec section 1. */
    ESP_ERROR_CHECK(tab5io_init());
    ESP_ERROR_CHECK(display_init());

    gfx_draw_text(24, 24, "jinxed writer", 3, gfx_w() - 48, COL_DIM);
    gfx_blit(0, gfx_h());
    ESP_LOGI(TAG, "display %dx%d", gfx_w(), gfx_h());
}

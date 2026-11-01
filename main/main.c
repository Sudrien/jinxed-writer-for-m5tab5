/*
 * main.c -- jinxed writer.
 *
 * SPDX-License-Identifier: MIT
 */
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "tab5io.h"
#include "usbhost.h"

#include "display.h"
#include "kbd.h"

static const char *TAG = "jinxed";

void app_main(void)
{
    /* First thing: the bus, and the expanders holding the panel's
     * reset line and USB5V_EN. Spec section 1. */
    ESP_ERROR_CHECK(tab5io_init());
    ESP_ERROR_CHECK(display_init());

    /* The port's owner, then the keyboard client, then power: a
     * keyboard already plugged in enumerates the moment VBUS arrives,
     * and has to find the client registered. */
    ESP_ERROR_CHECK(usbhost_init(tab5io_exp2()));
    ESP_ERROR_CHECK(kbd_init());
    usbhost_start();

    gfx_draw_text(24, 24, "jinxed writer", 3, gfx_w() - 48, COL_DIM);
    gfx_blit(0, gfx_h());
    ESP_LOGI(TAG, "display %dx%d", gfx_w(), gfx_h());

    /* Until the editor exists: every key, to the log. */
    for (;;) {
        kbd_event_t ev;
        if (!kbd_get(&ev, portMAX_DELAY)) continue;
        const char c = kbd_ascii(ev.usage, ev.mods, false);
        ESP_LOGI(TAG, "%s 0x%02X mods 0x%02X '%c'", ev.down ? "down" : "up  ",
                 ev.usage, ev.mods, (c >= 0x20 && c < 0x7F) ? c : ' ');
    }
}

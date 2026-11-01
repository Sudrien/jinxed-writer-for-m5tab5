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
#include "editor.h"
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

    ESP_ERROR_CHECK(editor_init());
    ESP_LOGI(TAG, "ready, %dx%d", gfx_w(), gfx_h());
    editor_run();
}

/*
 * main.c -- jinxed writer.
 *
 * SPDX-License-Identifier: MIT
 */
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "rtc8130.h"
#include "storage.h"
#include "storage_io.h"
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
    /* Timestamps for the log. Absent or VLF is not fatal: records are
     * marked untrusted instead (spec section 4). */
    if (!rtc8130_init(tab5io_bus())) ESP_LOGW(TAG, "no RTC; log times untrusted");
    ESP_ERROR_CHECK(display_init());

    /* The port's owner, then the keyboard client, then power: a
     * keyboard already plugged in enumerates the moment VBUS arrives,
     * and has to find the client registered. */
    ESP_ERROR_CHECK(usbhost_init(tab5io_exp2()));
    /* The card registers a mass-storage class on the same port, so it
     * too goes in before the port starts. */
    static const storage_usb_t usb = {
        .register_class = usbhost_register_class,
        .set_power      = usbhost_set_power,
        .powered        = usbhost_powered,
    };
    storage_io_init();
    ESP_ERROR_CHECK(storage_init(&usb));
    ESP_ERROR_CHECK(kbd_init());
    usbhost_start();

    ESP_ERROR_CHECK(editor_init());
    ESP_LOGI(TAG, "ready, %dx%d", gfx_w(), gfx_h());
    editor_run();
}

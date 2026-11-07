/*
 * main.c -- jinxed writer.
 *
 * SPDX-License-Identifier: MIT
 */
#include <sys/time.h>
#include <time.h>

#include "esp_app_desc.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "lcd.h"
#include "nvs_handler.h"
#include "touch.h"
#include "rtc8130.h"
#include "storage.h"
#include "storage_io.h"
#include "tab5io.h"
#include "usbhost.h"

#include "display.h"
#include "sender.h"
#include "editor.h"
#include "kbd.h"

static const char *TAG = "jinxed";

/*
 * 0048: the system clock from the RX8130, before the card is mounted.
 * FatFs stamps every file it writes from the system clock, which starts
 * at the epoch, so without this every note the Tab5 wrote was dated
 * 1980-01-01 -- FAT's first day -- in the notes page and on a computer.
 * Avoidant does the same at boot (its clock.c). Nothing is written
 * back: the RTC is never set here (spec 4). A time the chip cannot vouch
 * for (VLF), or one before 2026, is left alone, and files stay dated as
 * before rather than wrongly.
 */
#define CLOCK_FLOOR     1767225600LL    /* 2026-01-01 00:00 UTC */

static void clock_from_rtc(void)
{
    int64_t epoch;
    if (!rtc8130_read(&epoch) || epoch < CLOCK_FLOOR) {
        ESP_LOGW(TAG, "RTC time not trusted; files keep FAT's default date");
        return;
    }
    const struct timeval tv = { .tv_sec = (time_t)epoch };
    if (settimeofday(&tv, NULL) == 0) ESP_LOGI(TAG, "system clock from the RTC: %lld", (long long)epoch);
}

void app_main(void)
{
    /* Where this program starts in the log -- above is the ROM, the
     * bootloader and IDF's startup -- and what it was built from, so a
     * log can be matched to a tree. A warning, so it survives the info
     * level turned down; first, so a failure in the first init is still
     * below it. Defeatist's and Avoidant's line, in this program's name. */
    {
        const esp_app_desc_t *d = esp_app_get_description();
        ESP_LOGW(TAG, "=== Jinxed Writer === %s, IDF %s, built %s %s",
                 d ? d->version : "?", d ? d->idf_ver : "?",
                 d ? d->date : "?", d ? d->time : "?");
    }

    /* First thing: the bus, and the expanders holding the panel's
     * reset line and USB5V_EN. Spec section 1. */
    ESP_ERROR_CHECK(tab5io_init());
    /* Timestamps for the log. Absent or VLF is not fatal: records are
     * marked untrusted instead (spec section 4). */
    if (!rtc8130_init(tab5io_bus())) ESP_LOGW(TAG, "no RTC; log times untrusted");
    clock_from_rtc();
    /* NVS: the email account (feckless_mail), later the Wi-Fi list. */
    ESP_ERROR_CHECK(nvsh_init());
    ESP_ERROR_CHECK(display_init());
    /* Touch after the panel (TP_RST rides on its reset write), turned
     * with the picture. Not fatal: the keyboard does everything. */
    if (touch_init(tab5io_bus(), LCD_H_RES, LCD_V_RES) == ESP_OK) {
        touch_set_rotation(display_rotation());
    } else {
        ESP_LOGW(TAG, "no touch panel");
    }

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
    /* The network and the send task: the Ethernet adapter's class goes
     * on the port too, and the radio worker after storage (wifi.h: they
     * share the SDMMC host). Nothing is powered until a send asks. Not
     * fatal: the device is a writer first. */
    if (sender_init() != ESP_OK) ESP_LOGW(TAG, "no send task; email is off");
    ESP_ERROR_CHECK(kbd_init());
    usbhost_start();

    ESP_ERROR_CHECK(editor_init());
    ESP_LOGI(TAG, "ready, %dx%d", gfx_w(), gfx_h());
    editor_run();
}

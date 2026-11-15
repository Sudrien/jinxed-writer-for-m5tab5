/*
 * host.c -- Jinxed's .bin with its editor in a box (u-jinxed-0018).
 *
 * The board is brought up as the writer's main.c brings it up, line for
 * line to editor_init(), and the dark wake is the same. Then, where main.c
 * runs the editor, this loads the editor's ELF (../elf, embedded) into a
 * feckless box with jinxed_box_calls() -- feckless_box_calls_v1()'s table,
 * writes held to /sd/jinxed, NVS to "jinxed", and Jinxed's own calls
 * answered with the functions below -- and runs it on core 1, the USB
 * host, the card and the panel's interrupts on core 0 (u-jinxed-0011).
 *
 * The keyboard, the sender, power, settings and the display stay here
 * (UMODE.md, 0013): the email account and Wi-Fi never enter the box.
 *
 * SPDX-License-Identifier: MIT
 */
#include <stdbool.h>
#include <stdint.h>
#include <sys/time.h>
#include <time.h>

#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lcd.h"
#include "nvs_handler.h"
#include "touch.h"
#include "rtc8130.h"
#include "storage.h"
#include "storage_io.h"
#include "tab5io.h"
#include "usbhost.h"

#include "feckless_box_calls.h"
#include "feckless_umode_idf.h"
#include "feckless_umode_load.h"
#include "jinxed_box.h"

#include "display.h"
#include "sender.h"
#include "kbd.h"
#include "power.h"
#include "settings.h"

static const char *TAG = "jinxed";

extern const uint8_t elf_start[] asm("_binary_jinxed_editor_app_elf_start");
extern const uint8_t elf_end[]   asm("_binary_jinxed_editor_app_elf_end");

/* The box: the editor's buffers (TEXT_MAX six times over, the undo arena,
 * about 4.5 MB) and the dictionary's sections, which spell.c reads into
 * it, with room to spare. What is left of PSRAM is the host's: the panel's
 * shadow buffer, and a send's copy of the text and its log
 * (SENDER_LOG_MAX), which the sender allocates when it first sends. */
#define BOX_MAX     (16u << 20)
#define HOST_KEEP   (8u << 20)
#define BOX_STACK   (64u * 1024u)

/* As main.c's (0048). */
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

/* The functions Jinxed's own calls answer with: the writer's, by name. */
static const jinxed_host_t k_host = {
    .kbd_get = kbd_get, .kbd_present = kbd_present, .kbd_ascii = kbd_ascii,
    .sender_fetch_dict = sender_fetch_dict, .sender_fetched = sender_fetched,
    .sender_sent = sender_sent, .sender_gen = sender_gen, .sender_status = sender_status,
    .sender_send_doc = sender_send_doc,
    .power_idle_seconds = power_idle_seconds, .power_note_sent = power_note_sent,
    .power_off_idle = power_off_idle, .power_wake_line = power_wake_line,
    .settings_draw_gear = settings_draw_gear, .settings_gear_w = settings_gear_w,
    .settings_gear_hit = settings_gear_hit, .settings_author = settings_author,
    .settings_on_send = settings_on_send, .settings_page = settings_page,
    .settings_close = settings_close,
    .display_text_scale = display_text_scale, .display_foot_scale = display_foot_scale,
    .display_set_text_scale = display_set_text_scale, .display_markdown = display_markdown,
    .display_col = (const void *const *)&display_col, .display_col_size = sizeof(display_theme_t),
};

/* The editor, boxed, until it ends -- which the writer's never does, so
 * any end is logged, and said on the screen by app_main(). */
static void run_editor(void)
{
    feckless_umode_guard_unroute();
    jinxed_box_host(&k_host);
    size_t box = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM);
    box = box > HOST_KEEP ? box - HOST_KEEP : 0;
    if (box > BOX_MAX) box = BOX_MAX;
    box &= ~(size_t)63;
    uint32_t n = 0;
    const feckless_umode_call_t *calls = jinxed_box_calls(&n);
    if (!calls) {
        ESP_LOGE(TAG, "no table for the box");
        return;
    }
    feckless_umode_app_t *app = NULL;
    const esp_err_t e = feckless_umode_load(elf_start, (size_t)(elf_end - elf_start), calls, n,
                                            (uint32_t)box, BOX_STACK, &app);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "the editor would not load in a box of %u KB: %s",
                 (unsigned)(box / 1024), esp_err_to_name(e));
        return;
    }
    ESP_LOGI(TAG, "the editor, boxed, in %u KB, %u calls", (unsigned)(box / 1024), (unsigned)n);
    const feckless_umode_end_t end = feckless_umode_app_run_on(app, 1);
    if (end.reason == FECKLESS_UMODE_END_FAULT) {
        uint32_t va = 0;
        const bool in_elf = feckless_umode_app_elf_vaddr(app, end.pc, &va);
        ESP_LOGE(TAG, "the editor FAULTED: %s at 0x%08x (0x%08x), ELF %s0x%08x",
                 feckless_umode_cause_name(end.mcause), (unsigned)end.pc, (unsigned)end.mtval,
                 in_elf ? "" : "not in the image ", (unsigned)va);
    } else {
        ESP_LOGE(TAG, "the editor ended: reason %d", (int)end.reason);
    }
    feckless_umode_unload(app);
    feckless_box_calls_end();
}

void app_main(void)
{
    {
        const esp_app_desc_t *d = esp_app_get_description();
        ESP_LOGW(TAG, "=== Jinxed Writer (boxed) === %s, IDF %s, built %s %s",
                 d ? d->version : "?", d ? d->idf_ver : "?",
                 d ? d->date : "?", d ? d->time : "?");
    }

    /* From here to editor_init(), main.c's, and its comments say why. */
    ESP_ERROR_CHECK(tab5io_init());
    if (!rtc8130_init(tab5io_bus())) ESP_LOGW(TAG, "no RTC; log times untrusted");
    const bool dark = power_woke();
    clock_from_rtc();
    ESP_ERROR_CHECK(nvsh_init());
    if (dark) {
        ESP_ERROR_CHECK(usbhost_init(tab5io_exp2()));
        static const storage_usb_t dusb = {
            .register_class = usbhost_register_class,
            .set_power      = usbhost_set_power,
            .powered        = usbhost_powered,
        };
        storage_io_init();
        ESP_ERROR_CHECK(storage_init(&dusb));
        if (sender_init() != ESP_OK) ESP_LOGW(TAG, "no send task");
        power_dark_wake();
    }
    ESP_ERROR_CHECK(display_init());
    if (touch_init(tab5io_bus(), LCD_H_RES, LCD_V_RES) == ESP_OK) {
        touch_set_rotation(display_rotation());
    } else {
        ESP_LOGW(TAG, "no touch panel");
    }
    ESP_ERROR_CHECK(usbhost_init(tab5io_exp2()));
    static const storage_usb_t usb = {
        .register_class = usbhost_register_class,
        .set_power      = usbhost_set_power,
        .powered        = usbhost_powered,
    };
    storage_io_init();
    ESP_ERROR_CHECK(storage_init(&usb));
    if (sender_init() != ESP_OK) ESP_LOGW(TAG, "no send task; email is off");
    ESP_ERROR_CHECK(kbd_init());
    usbhost_start();

    ESP_LOGI(TAG, "ready, %dx%d", gfx_w(), gfx_h());
    run_editor();
    /* Stopped, and said so, not restarted: a fault at start would loop,
     * and the log above is what to read. The text is on the card as the
     * last append left it. Power off with the button. */
    gfx_fill_rect(0, 0, gfx_w(), gfx_h(), COL_BG);
    gfx_draw_text(40, 40, "The boxed editor stopped. The serial log says why.", 3, gfx_w() - 40, COL_BAD);
    gfx_blit(0, gfx_h());
    for (;;) vTaskDelay(portMAX_DELAY);
}

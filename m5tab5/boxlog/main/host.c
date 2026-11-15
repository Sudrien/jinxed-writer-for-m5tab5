/*
 * host.c -- boxlog's .bin (u-jinxed-0005): the probe natively, then the
 * same probe boxed, then the two results compared. See ../README.md.
 *
 * The box is loaded with jinxed_box_calls() -- feckless_box_calls_v1()'s
 * table with writes held to /sd/jinxed (u-jinxed-0004) -- which is what
 * Jinxed's own .bin will give its editor, and runs on core 1, the storage
 * handler and the USB host on core 0, as the writer would have them. No
 * panel: nothing here draws, so nothing scans PSRAM while the card DMAs.
 *
 * SPDX-License-Identifier: MIT
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "boxlog.h"
#include "feckless_box_calls.h"
#include "feckless_umode_idf.h"
#include "feckless_umode_load.h"
#include "jinxed_box.h"
#include "storage.h"
#include "storage_io.h"
#include "tab5io.h"
#include "usbhost.h"

static const char *TAG = "boxlog";

extern const uint8_t elf_start[] asm("_binary_jinxed_boxlog_probe_app_elf_start");
extern const uint8_t elf_end[]   asm("_binary_jinxed_boxlog_probe_app_elf_end");

#define BOX_MAX   (8u << 20)        /* the probe's three 256 KB texts and doclog's two buffers, many times over */
#define BOX_STACK (64u * 1024u)

/* The two result files, line by line. */
static void compare(void)
{
    FILE *h = fopen(BOXLOG_HOST_RESULT, "r"), *b = fopen(BOXLOG_BOX_RESULT, "r");
    if (!h || !b) {
        ESP_LOGE(TAG, "COMPARE: no %s", h ? BOXLOG_BOX_RESULT : BOXLOG_HOST_RESULT);
        if (h) fclose(h);
        if (b) fclose(b);
        return;
    }
    static char lh[512], lb[512];
    unsigned n = 0;
    for (;;) {
        const char *gh = fgets(lh, sizeof lh, h), *gb = fgets(lb, sizeof lb, b);
        if (!gh && !gb) {
            ESP_LOGI(TAG, "SAME: the box and the host agree, %u lines", n);
            break;
        }
        n++;
        if (!gh || !gb || strcmp(lh, lb) != 0) {
            lh[strcspn(lh, "\n")] = '\0';
            lb[strcspn(lb, "\n")] = '\0';
            ESP_LOGE(TAG, "DIFFER at line %u", n);
            ESP_LOGE(TAG, "  host: %s", gh ? lh : "(ended)");
            ESP_LOGE(TAG, "  box:  %s", gb ? lb : "(ended)");
            break;
        }
    }
    fclose(h);
    fclose(b);
}

static void run(void)
{
    /* The card, as main.c brings it up, without the keyboard's class. */
    ESP_ERROR_CHECK(tab5io_init());
    ESP_ERROR_CHECK(usbhost_init(tab5io_exp2()));
    static const storage_usb_t usb = {
        .register_class = usbhost_register_class,
        .set_power      = usbhost_set_power,
        .powered        = usbhost_powered,
    };
    storage_io_init();
    ESP_ERROR_CHECK(storage_init(&usb));
    usbhost_start();
    for (int i = 0; i < 100 && !storage_present(STORAGE_SD); i++) vTaskDelay(pdMS_TO_TICKS(100));
    if (!storage_present(STORAGE_SD)) {
        ESP_LOGE(TAG, "no card");
        return;
    }

    /* 1. Natively. */
    ESP_LOGI(TAG, "the host's run");
    if (boxlog_run(BOXLOG_HOST_RESULT) != 0) return;

    /* 2. Boxed: as Lothesome's standalone.c loads its analyzer, with
     * Jinxed's table. */
    feckless_umode_guard_unroute();
    size_t box = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM);
    box = box > (1u << 20) ? box - (1u << 20) : 0;
    if (box > BOX_MAX) box = BOX_MAX;
    box &= ~(size_t)63;
    uint32_t n = 0;
    const feckless_umode_call_t *calls = jinxed_box_calls(&n);
    if (!calls) {
        ESP_LOGE(TAG, "no table: feckless_box_calls lacks a call jinxed_box guards");
        return;
    }
    feckless_umode_app_t *app = NULL;
    const esp_err_t e = feckless_umode_load(elf_start, (size_t)(elf_end - elf_start), calls, n,
                                            (uint32_t)box, BOX_STACK, &app);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "the probe would not load in a box: %s", esp_err_to_name(e));
        return;
    }
    ESP_LOGI(TAG, "the box's run, in %u KB, %u calls", (unsigned)(box / 1024), (unsigned)n);
    const int64_t t0 = esp_timer_get_time();
    const feckless_umode_end_t end = feckless_umode_app_run_on(app, 1);
    const unsigned ms = (unsigned)((esp_timer_get_time() - t0) / 1000);
    if (end.reason == FECKLESS_UMODE_END_FAULT) {
        uint32_t va = 0;
        const bool in_elf = feckless_umode_app_elf_vaddr(app, end.pc, &va);
        ESP_LOGE(TAG, "the probe FAULTED after %u ms: %s at 0x%08x (0x%08x), ELF %s0x%08x", ms,
                 feckless_umode_cause_name(end.mcause), (unsigned)end.pc, (unsigned)end.mtval,
                 in_elf ? "" : "not in the image ", (unsigned)va);
    } else {
        ESP_LOGI(TAG, "the probe ended after %u ms: reason %d", ms, (int)end.reason);
    }
    feckless_umode_unload(app);
    feckless_box_calls_end();

    /* 3. */
    compare();
}

/* u-jinxed-0011: the run between two marker lines, so the part of the log
 * to copy is plain: from the first to the second, whichever way run()
 * ends. Everything before is the ROM's and the bootloader's. */
void app_main(void)
{
    ESP_LOGI(TAG, "======================== boxlog: copy from here ========================");
    run();
    ESP_LOGI(TAG, "======================== boxlog: copy to here ==========================");
}

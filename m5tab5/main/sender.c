/*
 * sender.c -- the send task. See sender.h.
 *
 * The task's stack is in PSRAM and its control block in internal RAM
 * (DRAM_ATTR): internal RAM is the radio's DMA pool, and a 16 KB stack
 * there is 16 KB the C6's transport cannot have. A FreeRTOS control block
 * cannot live in PSRAM, so that one small part stays internal. Made once
 * and never deleted, so neither is ever freed.
 *
 * SPDX-License-Identifier: MIT
 */
#include "sender.h"

#include <stdio.h>
#include <string.h>

#include "esp_attr.h"
#include "esp_err.h"
#include "sdkconfig.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "ethernet.h"
#include "feckless_net.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mailcfg.h"
#include "mailsend.h"
#include "rtc8130.h"
#include "tab5io.h"
#include "usbhost.h"
#include "wifi.h"

static const char *TAG = "sender";

#if !CONFIG_FREERTOS_TASK_CREATE_ALLOW_EXT_MEM
#error "sender.c puts its task stack in PSRAM and needs CONFIG_FREERTOS_TASK_CREATE_ALLOW_EXT_MEM=y"
#endif

/* Above MAILSEND_MIN_STACK for our own frames and the logging. */
#define SEND_STACK      (MAILSEND_MIN_STACK + 4096)
/* Below the editor (4) and the keyboard (5): a send waits for typing,
 * never the other way round. */
#define SEND_PRIO       (2)
#define POLL_MS         (250)

/* The saved networks' namespace: Avoidant's (and Defeatist's). */
#define NET_NS          "defeatist"

static TaskHandle_t       s_task;
static DRAM_ATTR StaticTask_t s_tcb;
static volatile bool      s_want_radio;
static volatile bool      s_busy;

static portMUX_TYPE       s_lock = portMUX_INITIALIZER_UNLOCKED;
static char               s_line[160];
static volatile uint32_t  s_gen;

/* The account's copy for one send, password and all: a static, not the
 * task's stack (it is several hundred bytes), wiped after use. */
static mailcfg_t          s_cfg;

static void set_line(const char *s)
{
    taskENTER_CRITICAL(&s_lock);
    strlcpy(s_line, s, sizeof s_line);
    s_gen++;
    taskEXIT_CRITICAL(&s_lock);
}

bool sender_status(char *line, size_t size)
{
    taskENTER_CRITICAL(&s_lock);
    strlcpy(line, s_line, size);
    taskEXIT_CRITICAL(&s_lock);
    return s_busy;
}

uint32_t sender_gen(void) { return s_gen; }

/* ---- the radio ---------------------------------------------------------- */

static bool radio_wanted(void) { return s_want_radio; }

static void radio(bool on)
{
    s_want_radio = on;
    wifi_request_apply();
}

static bool online(void) { return wifi_connected() || net_online(); }

/* ---- one send ----------------------------------------------------------- */

static void test_send(void)
{
    const int64_t t0 = esp_timer_get_time();
    const bool configured = mailcfg_load(NULL, &s_cfg);
    char dom[MAILCFG_ADDR_MAX + 2];
    mailcfg_domain(s_cfg.to, dom, sizeof dom);

    /* Spec 6's order: no network first (it suppresses the email message),
     * then no account. */
    set_line("Looking for a network...");
    radio(true);
    while (!online() && esp_timer_get_time() - t0 < (int64_t)MAILSEND_TIMEOUT_MS * 1000) {
        vTaskDelay(pdMS_TO_TICKS(POLL_MS));
    }
    char line[sizeof s_line];
    if (!online()) {
        if (configured) snprintf(line, sizeof line, "no wifi, did not send to %s", dom);
        else            snprintf(line, sizeof line, "no wifi");
        set_line(line);
    } else if (!configured) {
        set_line("no email configured");
    } else {
        set_line("Sending...");
        int64_t now = 0;
        if (!rtc8130_read(&now)) now = 0;
        const int64_t left_ms = (int64_t)MAILSEND_TIMEOUT_MS - (esp_timer_get_time() - t0) / 1000;
        mailsend_result_t r;
        mailsend(&s_cfg, "Jinxed Writer: test",
                 "This is a test message from Jinxed Writer.\n"
                 "\n"
                 "If it arrived, the email account on the settings page works,\n"
                 "and notes will be able to reach this address.\n",
                 now, left_ms > 1000 ? (uint32_t)left_ms : 1000, &r);
        if (r.err == MAILSEND_OK) snprintf(line, sizeof line, "sent to %s", dom);
        else                      snprintf(line, sizeof line, "not sent: %s", r.why);
        set_line(line);
    }
    mailcfg_wipe(&s_cfg);
    radio(false);
    ESP_LOGI(TAG, "test send done in %lld ms", (long long)((esp_timer_get_time() - t0) / 1000));
}

static void send_task(void *arg)
{
    (void)arg;
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        test_send();
        s_busy = false;
        s_gen++;
    }
}

/* ---- API ---------------------------------------------------------------- */

esp_err_t sender_init(void)
{
    /* The network's half that belongs to the board's owner: saved
     * networks, the Ethernet adapter's USB class and configuration
     * choice, the radio's worker. Before usbhost_start(). */
    const feckless_net_start_t net = {
        .exp2                  = tab5io_exp2(),
        .usb_register_class    = usbhost_register_class,
        .usb_set_config_select = usbhost_set_config_select,
        .nvs_ns                = NET_NS,
    };
    esp_err_t err = feckless_net_start(&net);
    if (err != ESP_OK) ESP_LOGW(TAG, "network start: %s", esp_err_to_name(err));

    /* The app's policy: the radio only while a send wants it; no NTP (the
     * RTC is set by hand, spec 4). */
    static const feckless_net_hooks_t hooks = { .wifi_enabled = radio_wanted };
    feckless_net_set_hooks(&hooks);

    StackType_t *stack = heap_caps_malloc(SEND_STACK, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!stack) return ESP_ERR_NO_MEM;
    s_task = xTaskCreateStatic(send_task, "send", SEND_STACK, NULL, SEND_PRIO, stack, &s_tcb);
    return s_task ? ESP_OK : ESP_FAIL;
}

bool sender_test(void)
{
    if (!s_task || s_busy) return false;
    s_busy = true;
    set_line("Starting...");
    xTaskNotifyGive(s_task);
    return true;
}

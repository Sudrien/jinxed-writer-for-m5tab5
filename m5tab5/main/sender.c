/*
 * sender.c -- the send task. See sender.h.
 *
 * The task's stack is in INTERNAL RAM, though internal RAM is the radio's
 * DMA pool. 0005 put it in PSRAM, and the first test send asserted in
 * spi_flash's cache_utils.c (esp_task_stack_is_sane_cache_disabled):
 * mailcfg_load() reads NVS, a flash read disables the cache, and a stack
 * in PSRAM cannot be touched while the cache is off. NVS is not the only
 * way this task reaches flash -- Mbed TLS 4's PSA key storage is NVS too
 * -- so moving the one read elsewhere would leave the next one. 16 KB,
 * made once, never freed.
 *
 * SPDX-License-Identifier: MIT
 */
#include "sender.h"

#include <stdio.h>
#include <string.h>

#include "esp_attr.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "ethernet.h"
#include "feckless_fetch.h"
#include "feckless_net.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mailcfg.h"
#include "mailsend.h"
#include "rtc8130.h"
#include "spell.h"
#include "storage.h"
#include "tab5io.h"
#include "usbhost.h"
#include "wifi.h"

static const char *TAG = "sender";


/* Above MAILSEND_MIN_STACK for our own frames and the logging. */
#define SEND_STACK      (MAILSEND_MIN_STACK + 4096)
/* Below the editor (4) and the keyboard (5): a send waits for typing,
 * never the other way round. */
#define SEND_PRIO       (2)
#define POLL_MS         (250)

/* The saved networks' namespace: Avoidant's (and Defeatist's). */
#define NET_NS          "defeatist"

/* The dictionary download (spec 5), through feckless_fetch (feckless
 * 9003): its connections, resume and writer, our radio and our volume.
 * Up to ten minutes; then it is paused, its part file kept, and the next
 * D carries on from it. A first connect that fails is tried again, as
 * 0015 did: feckless_fetch retries a connection that breaks part way,
 * not one that never opened. */
#define FETCH_CONNS     (3)
#define FETCH_MAX_MS    (10 * 60 * 1000)
#define FETCH_TRIES     (3)
#define FETCH_RETRY_MS  (2000)

typedef enum { JOB_TEST, JOB_DICT } job_t;

static TaskHandle_t       s_task;
static volatile job_t     s_job;
static volatile bool      s_fetched;
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

/* ---- the dictionary ------------------------------------------------------ */

/* VERIFY, on feckless_fetch's task: the part file is whole -- is it a
 * dictionary, header and sections adding up to its length? The volume is
 * held, by fetch_dict(). Refused, the part file goes. */
static bool fetch_event(feckless_fetch_ev_t *ev)
{
    if (ev->kind == FECKLESS_FETCH_EV_VERIFY) return spell_file_ok(ev->part, (long)ev->size);
    return true;
}

/* One download, start to end: runs until feckless_fetch stops. */
static feckless_fetch_state_t fetch_once(const feckless_fetch_opts_t *o, int64_t t0,
                                         char *why, size_t why_size)
{
    if (!feckless_fetch_start(o)) {
        feckless_fetch_why(why, why_size);
        return FECKLESS_FETCH_FAILED;
    }
    set_line("Downloading the dictionary...");
    uint32_t seen = 0;
    bool paused = false;
    feckless_fetch_state_t st;
    int64_t got = 0, total = -1;
    uint32_t gen = 0;
    while ((st = feckless_fetch_state(NULL, &got, &total, NULL, 0, &gen)) == FECKLESS_FETCH_RUNNING) {
        if (gen != seen) {
            seen = gen;
            char line[64];
            if (total > 0) snprintf(line, sizeof line, "Downloading the dictionary: %d%%", (int)(got * 100 / total));
            else           snprintf(line, sizeof line, "Downloading the dictionary: %d KB", (int)(got / 1024));
            set_line(line);
        }
        if (!paused && esp_timer_get_time() - t0 > (int64_t)FETCH_MAX_MS * 1000) {
            feckless_fetch_pause();             /* kept: the next D carries on */
            paused = true;
        }
        vTaskDelay(pdMS_TO_TICKS(POLL_MS));
    }
    feckless_fetch_why(why, why_size);
    return st;
}

static void fetch_dict(void)
{
    const int64_t t0 = esp_timer_get_time();
    static const char dict[] = SPELL_DIR "/" SPELL_DICT;
    s_fetched = false;

    set_line("Looking for a network...");
    radio(true);
    while (!online() && esp_timer_get_time() - t0 < (int64_t)MAILSEND_TIMEOUT_MS * 1000) {
        vTaskDelay(pdMS_TO_TICKS(POLL_MS));
    }
    char why[64] = "";
    feckless_fetch_state_t st = FECKLESS_FETCH_FAILED;
    if (!online()) {
        snprintf(why, sizeof why, "no wifi");
    } else {
        /*
         * The volume is held for the whole download, not leased per piece
         * as spec 1 has it: feckless_fetch keeps the part file open
         * throughout, and an unmount with a FILE open is a use-after-free
         * inside FatFs (storage.h). Held, a pulled card is marked absent
         * and its unmount waits for the release; the writes fail and the
         * download stops, its part file kept for a card put back.
         */
        storage_hold_brief(STORAGE_SD);
        const feckless_fetch_opts_t o = {
            .url        = SPELL_DICT_URL,
            .path       = dict,
            .conns      = FETCH_CONNS,
            .https_only = true,
            .cb         = fetch_event,
        };
        for (int tries = 0; tries < FETCH_TRIES; tries++) {
            if (!storage_present(STORAGE_SD)) {
                snprintf(why, sizeof why, "no card");
                break;
            }
            st = fetch_once(&o, t0, why, sizeof why);
            if (st != FECKLESS_FETCH_FAILED || strcmp(why, "could not connect") != 0) break;
            if (tries + 1 < FETCH_TRIES) {
                ESP_LOGW(TAG, "could not connect; try %d of %d in %d ms", tries + 2, FETCH_TRIES, FETCH_RETRY_MS);
                set_line("Downloading the dictionary: connecting again...");
                vTaskDelay(pdMS_TO_TICKS(FETCH_RETRY_MS));
            }
        }
        storage_release_brief(STORAGE_SD);
    }
    radio(false);

    if (st == FECKLESS_FETCH_DONE) {
        s_fetched = true;
        set_line("Dictionary downloaded.");
    } else if (st == FECKLESS_FETCH_PAUSED) {
        set_line("Dictionary download paused after ten minutes; D carries on.");
    } else {
        char line[sizeof s_line];
        snprintf(line, sizeof line, "Dictionary not downloaded: %s.", why[0] ? why : "it failed");
        set_line(line);
    }
    ESP_LOGI(TAG, "dictionary download ended in %lld ms (%d)",
             (long long)((esp_timer_get_time() - t0) / 1000), (int)st);
}

static void send_task(void *arg)
{
    (void)arg;
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        if (s_job == JOB_DICT) fetch_dict();
        else                   test_send();
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

    /* Internal: this task reads flash (see the top of the file). */
    StackType_t *stack = heap_caps_malloc(SEND_STACK, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!stack) return ESP_ERR_NO_MEM;
    s_task = xTaskCreateStatic(send_task, "send", SEND_STACK, NULL, SEND_PRIO, stack, &s_tcb);
    return s_task ? ESP_OK : ESP_FAIL;
}

bool sender_test(void)
{
    if (!s_task || s_busy) return false;
    s_busy = true;
    s_job = JOB_TEST;
    set_line("Starting...");
    xTaskNotifyGive(s_task);
    return true;
}

bool sender_fetch_dict(void)
{
    if (!s_task || s_busy) return false;
    s_busy = true;
    s_job = JOB_DICT;
    set_line("Starting...");
    xTaskNotifyGive(s_task);
    return true;
}

bool sender_fetched(void) { return s_fetched; }

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
#include "doclog.h"
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

typedef enum { JOB_TEST, JOB_DICT, JOB_DOC, JOB_NTP } job_t;

/* 0038: a document's copy for its send, PSRAM, made once: the editor's
 * whole text, the footer and a NUL. */
#define DOC_MAX         (256 * 1024 + DOCLOG_FOOT_MAX + 1)
static char              *s_doc;
static uint32_t           s_doc_len;
static char               s_subject[96];
/* 0041: the files: the .md, the log. */
static char              *s_log;        /* SENDER_LOG_MAX, PSRAM, made at the first send */
static size_t             s_log_len;
static bool               s_log_big;
static char               s_md_name[40], s_log_name[44];

static TaskHandle_t       s_task;
static volatile job_t     s_job;
static volatile bool      s_fetched;
static volatile bool      s_sent;       /* 0051: the last send went */
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

/* ---- 0049: the time, by NTP ------------------------------------------------
 *
 * SNTP starts by itself once the station has an address (wifi.c), now
 * that ntp_enabled says yes; wifi_stop() ends it. Its reply has already
 * set the system clock when ntp_reply() runs; this writes the RTC to
 * match, so the next boot starts right too. A reply before 2026 is not
 * believed and the RTC is left alone, as Avoidant's clock.c does.
 */
#define CLOCK_FLOOR     1767225600LL    /* 2026-01-01 00:00 UTC */
#define NTP_JOIN_MS     (20 * 1000)     /* at boot: a network, or give up */
#define NTP_WAIT_MS     (8 * 1000)      /* then a reply, or give up */

static volatile uint32_t s_ntp_gen;     /* bumped per believed reply */

static bool ntp_on(void) { return true; }

static bool ntp_reply(int64_t epoch, int64_t boot_us)
{
    (void)boot_us;
    if (epoch < CLOCK_FLOOR) {
        ESP_LOGW(TAG, "NTP reply %lld is before 2026; the RTC left alone", (long long)epoch);
        return false;
    }
    if (rtc8130_write(epoch)) ESP_LOGI(TAG, "NTP to the RTC: %lld", (long long)epoch);
    else ESP_LOGW(TAG, "NTP reply not written: no RTC");
    s_ntp_gen++;
    return true;
}

/* With the radio up and online: wait, up to `ms`, for a reply after
 * `gen`. True if one came. */
static bool ntp_wait(uint32_t gen, uint32_t ms)
{
    const int64_t t0 = esp_timer_get_time();
    while (s_ntp_gen == gen && esp_timer_get_time() - t0 < (int64_t)ms * 1000)
        vTaskDelay(pdMS_TO_TICKS(POLL_MS));
    return s_ntp_gen != gen;
}

/* At boot, once: the radio up for the time alone, then down. Silent --
 * the editor shows no line for it -- and quick to give up. */
static void ntp_boot(void)
{
    const int64_t t0 = esp_timer_get_time();
    const uint32_t gen = s_ntp_gen;
    radio(true);
    while (!online() && esp_timer_get_time() - t0 < (int64_t)NTP_JOIN_MS * 1000)
        vTaskDelay(pdMS_TO_TICKS(POLL_MS));
    const bool got = online() && ntp_wait(gen, NTP_WAIT_MS);
    radio(false);
    ESP_LOGI(TAG, "boot NTP: %s in %lld ms", got ? "set" : online() ? "no reply" : "no network",
             (long long)((esp_timer_get_time() - t0) / 1000));
}

/* ---- one send ----------------------------------------------------------- */

/*
 * One message, start to end: the radio, a network, the account, the
 * send, each with spec 6's line. 0038 lifted this out of test_send()
 * unchanged but for the subject and body, so the test and a document go
 * the same way.
 */
static void send_one(const char *subject, const char *body, const mailsend_attach_t *att, size_t n_att,
                     const char *note)
{
    const int64_t t0 = esp_timer_get_time();
    const uint32_t ntp_gen = s_ntp_gen;     /* 0049 */
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
        mailsend_with(&s_cfg, subject, body, att, n_att, now, left_ms > 1000 ? (uint32_t)left_ms : 1000, &r);
        s_sent = r.err == MAILSEND_OK;
        if (r.err == MAILSEND_OK) snprintf(line, sizeof line, "sent to %s%s", dom, note ? note : "");
        else                      snprintf(line, sizeof line, "not sent: %s", r.why);
        set_line(line);
    }
    mailcfg_wipe(&s_cfg);
    /* 0049: SNTP started at the join; give it a moment if it has not
     * answered yet, so a send also sets the clock. Usually it already has. */
    if (online() && s_ntp_gen == ntp_gen) ntp_wait(ntp_gen, NTP_WAIT_MS / 2);
    radio(false);
    ESP_LOGI(TAG, "send done in %lld ms", (long long)((esp_timer_get_time() - t0) / 1000));
}

static void test_send(void)
{
    send_one("Jinxed Writer: test",
             "This is a test message from Jinxed Writer.\n"
             "\n"
             "If it arrived, the email account on the settings page works,\n"
             "and notes will be able to reach this address.\n", NULL, 0, NULL);
}

/* 0041: the text inline, and the .md and the log attached. */
static void doc_send(void)
{
    mailsend_attach_t a[2] = {
        { .name = s_md_name, .type = "text/markdown", .data = s_doc, .len = s_doc_len },
        { .name = s_log_name, .data = s_log, .len = s_log_len },
    };
    send_one(s_subject, s_doc, a, s_log_len ? 2 : 1,
             s_log_big ? "; the log was too big to attach" : NULL);
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
        set_line("Dictionary paused at ten minutes; D carries on.");
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
        if (s_job == JOB_DICT)     fetch_dict();
        else if (s_job == JOB_DOC) doc_send();
        else if (s_job == JOB_NTP) ntp_boot();
        else                       test_send();
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

    /* The app's policy: the radio only while a send wants it. 0049: and
     * NTP whenever it is up, the reply written to the RTC. */
    static const feckless_net_hooks_t hooks = {
        .wifi_enabled = radio_wanted,
        .ntp_enabled  = ntp_on,
        .ntp_reply    = ntp_reply,
    };
    feckless_net_set_hooks(&hooks);

    /* Internal: this task reads flash (see the top of the file). */
    StackType_t *stack = heap_caps_malloc(SEND_STACK, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!stack) return ESP_ERR_NO_MEM;
    s_task = xTaskCreateStatic(send_task, "send", SEND_STACK, NULL, SEND_PRIO, stack, &s_tcb);
    if (!s_task) return ESP_FAIL;
    /* 0049: the time at boot. Busy while it runs, so a send asked for in
     * the first seconds says so and is asked again. */
    s_busy = true;
    s_job = JOB_NTP;
    xTaskNotifyGive(s_task);
    return ESP_OK;
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

bool sender_send_doc(const char *name, const char *range, const char *text, uint32_t len,
                     const char *foot, size_t (*log)(void *buf, size_t cap, bool *too_big))
{
    if (!s_task || s_busy) return false;
    if (!s_doc) s_doc = heap_caps_malloc(DOC_MAX, MALLOC_CAP_SPIRAM);
    if (!s_log) s_log = heap_caps_malloc(SENDER_LOG_MAX, MALLOC_CAP_SPIRAM);   /* none: no log attached */
    const size_t fl = foot ? strlen(foot) : 0;
    if (!s_doc || (size_t)len + fl + 1 > DOC_MAX) return false;
    s_busy = true;
    s_sent = false;
    memcpy(s_doc, text, len);
    memcpy(s_doc + len, foot ? foot : "", fl + 1);
    s_doc_len = len + (uint32_t)fl;
    s_log_big = false;
    s_log_len = s_log && log ? log(s_log, SENDER_LOG_MAX, &s_log_big) : 0;
    snprintf(s_md_name, sizeof s_md_name, "%.32s", name);
    snprintf(s_log_name, sizeof s_log_name, "%.32s.h0", name);
    snprintf(s_subject, sizeof s_subject, "Jinxed Writer: %.32s, %.40s", name, range);
    s_job = JOB_DOC;
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

bool sender_sent(void) { return s_sent; }

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

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "esp_attr.h"
#include "esp_crt_bundle.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "ethernet.h"
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

/* The dictionary fetch. Received into PSRAM and written in pieces this
 * big, the card leased for each piece, never across the fetch (spec 1). */
#define FETCH_PIECE     (64 * 1024)
#define FETCH_HOPS      (5)         /* raw.githubusercontent.com answers directly;
                                     * a redirect is still followed, should it move */
/* The radio's wait is a send's (spec 3); the transfer gets this long,
 * and stalls for this long, before it is given up. */
#define FETCH_MAX_MS    (10 * 60 * 1000)
#define FETCH_STALL_MS  (30 * 1000)
#define FETCH_MAX_LEN   (64L * 1024 * 1024)
/* Connecting: each try waits this long for TCP and the TLS handshake,
 * and a failed one is tried again this many times in all. On the board a
 * handshake has lost its certificate burst to esp_hosted (an SDIO read
 * of 3584 bytes against 1536-byte staging slots, dropped whole), and 15
 * s was not enough for TCP to recover it; a fresh connection is a fresh
 * chance at the burst. */
#define FETCH_CONNECT_MS (30 * 1000)
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

/* Every directory down to `dir`. A level that exists already, or the
 * mount point, fails harmlessly; the fopen() after says what matters. */
static void make_dirs(const char *dir)
{
    char p[sizeof(SPELL_DIR)];
    for (size_t i = 1; dir[i - 1] != '\0' && i < sizeof(p); i++) {
        if (dir[i] != '/' && dir[i] != '\0') continue;
        memcpy(p, dir, i);
        p[i] = '\0';
        if (mkdir(p, 0777) != 0 && errno != EEXIST) ESP_LOGD(TAG, "mkdir %s: errno %d", p, errno);
    }
}

/* Append (or, first, write) one piece to the part file. */
static bool put_piece(const char *path, const char *buf, size_t n, bool first)
{
    storage_hold_brief(STORAGE_SD);
    bool ok = false;
    if (storage_present(STORAGE_SD)) {
        if (first) make_dirs(SPELL_DIR);
        FILE *f = fopen(path, first ? "wb" : "ab");
        if (f) {
            ok = fwrite(buf, 1, n, f) == n;
            ok = (fflush(f) == 0) && ok;
            ok = (fsync(fileno(f)) == 0) && ok;
            ok = (fclose(f) == 0) && ok;
        }
    }
    storage_release_brief(STORAGE_SD);
    return ok;
}

/* Check the part file and put it where spell_load() looks. */
static bool settle(const char *part, const char *dict, long size)
{
    storage_hold_brief(STORAGE_SD);
    bool ok = storage_present(STORAGE_SD) && spell_file_ok(part, size);
    if (ok) {
        /* FatFs cannot rename over a file: unlink, then rename. */
        unlink(dict);
        ok = rename(part, dict) == 0;
    } else {
        unlink(part);
    }
    storage_release_brief(STORAGE_SD);
    return ok;
}

/* GET the dictionary into `part`, following redirects. Its length, or -1
 * with `why` set. */
static long fetch(esp_http_client_handle_t c, const char *part, char *buf, int64_t t0,
                  char *why, size_t why_size)
{
    int status = 0;
    int64_t len = 0;
    for (int hop = 0, tries = 0; hop < FETCH_HOPS; ) {
        const esp_err_t err = esp_http_client_open(c, 0);
        if (err != ESP_OK) {
            esp_http_client_close(c);
            if (++tries >= FETCH_TRIES) {
                snprintf(why, why_size, "could not connect");
                return -1;
            }
            ESP_LOGW(TAG, "connect failed (%s), try %d of %d in %d ms",
                     esp_err_to_name(err), tries + 1, FETCH_TRIES, FETCH_RETRY_MS);
            set_line("Downloading the dictionary: connecting again...");
            vTaskDelay(pdMS_TO_TICKS(FETCH_RETRY_MS));
            continue;
        }
        hop++;
        len = esp_http_client_fetch_headers(c);
        status = esp_http_client_get_status_code(c);
        if (status < 300 || status > 308) break;
        if (esp_http_client_set_redirection(c) != ESP_OK) break;
        int left;
        esp_http_client_flush_response(c, &left);
        esp_http_client_close(c);
    }
    if (status >= 300 && status <= 308) {
        snprintf(why, why_size, "too many redirects");
        return -1;
    }
    if (status != 200) {
        snprintf(why, why_size, "the server said %d", status);
        return -1;
    }
    if (len <= 0 || len > FETCH_MAX_LEN) {
        snprintf(why, why_size, "no usable length");
        return -1;
    }

    long got = 0;
    size_t fill = 0;
    int shown = -1;
    int64_t last = esp_timer_get_time();
    while (got < len) {
        const int64_t now = esp_timer_get_time();
        if (now - t0 > (int64_t)FETCH_MAX_MS * 1000 || now - last > (int64_t)FETCH_STALL_MS * 1000) {
            snprintf(why, why_size, "too slow");
            return -1;
        }
        const int r = esp_http_client_read(c, buf + fill, (int)(FETCH_PIECE - fill));
        if (r < 0) {
            snprintf(why, why_size, "the connection broke");
            return -1;
        }
        if (r == 0) {
            if (esp_http_client_is_complete_data_received(c)) break;
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
        last = esp_timer_get_time();
        fill += (size_t)r;
        if (fill == FETCH_PIECE || got + (long)fill >= len) {
            if (!put_piece(part, buf, fill, got == 0)) {
                snprintf(why, why_size, "could not write the card");
                return -1;
            }
            got += (long)fill;
            fill = 0;
            const int pc = (int)(got * 100 / len);
            if (pc != shown) {
                char line[64];
                snprintf(line, sizeof line, "Downloading the dictionary: %d%%", pc);
                set_line(line);
                shown = pc;
            }
        }
    }
    if (got != len) {
        snprintf(why, why_size, "cut short");
        return -1;
    }
    return got;
}

static void fetch_dict(void)
{
    const int64_t t0 = esp_timer_get_time();
    static const char part[] = SPELL_DIR "/" SPELL_DICT ".part";
    static const char dict[] = SPELL_DIR "/" SPELL_DICT;
    s_fetched = false;

    set_line("Looking for a network...");
    radio(true);
    while (!online() && esp_timer_get_time() - t0 < (int64_t)MAILSEND_TIMEOUT_MS * 1000) {
        vTaskDelay(pdMS_TO_TICKS(POLL_MS));
    }
    char why[48] = "";
    char *buf = NULL;
    esp_http_client_handle_t c = NULL;
    long size = -1;
    if (!online()) {
        snprintf(why, sizeof why, "no wifi");
    } else if (!(buf = heap_caps_malloc(FETCH_PIECE, MALLOC_CAP_SPIRAM))) {
        snprintf(why, sizeof why, "out of memory");
    } else {
        set_line("Downloading the dictionary...");
        const esp_http_client_config_t cfg = {
            .url               = SPELL_DICT_URL,
            .crt_bundle_attach = esp_crt_bundle_attach,
            .timeout_ms        = FETCH_CONNECT_MS,
            /* The asset host's redirect is a long signed URL, in the
             * Location header in and the request line out. */
            .buffer_size       = 4096,
            .buffer_size_tx    = 4096,
        };
        c = esp_http_client_init(&cfg);
        if (!c) snprintf(why, sizeof why, "out of memory");
        else    size = fetch(c, part, buf, t0, why, sizeof why);
    }
    if (c) {
        esp_http_client_close(c);
        esp_http_client_cleanup(c);
    }
    heap_caps_free(buf);
    radio(false);

    if (size <= 0) {
        /* Nothing half-written is left on the card. */
        storage_hold_brief(STORAGE_SD);
        if (storage_present(STORAGE_SD)) unlink(part);
        storage_release_brief(STORAGE_SD);
    }
    if (size > 0 && settle(part, dict, size)) {
        s_fetched = true;
        set_line("Dictionary downloaded.");
    } else {
        if (size > 0) snprintf(why, sizeof why, "the file is not a dictionary");
        char line[sizeof s_line];
        snprintf(line, sizeof line, "Dictionary not downloaded: %s.", why);
        set_line(line);
    }
    ESP_LOGI(TAG, "dictionary fetch done in %lld ms, %ld bytes",
             (long long)((esp_timer_get_time() - t0) / 1000), size);
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

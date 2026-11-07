/*
 * power.c -- power-off, the alarm, and the dark wake. See power.h.
 *
 * SPDX-License-Identifier: MIT
 */
#include "power.h"

#include <stdio.h>
#include <string.h>

#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lcd.h"
#include "mailcfg.h"
#include "nvs_handler.h"
#include "rtc8130.h"
#include "storage.h"
#include "tab5io.h"

#include "doclog.h"
#include "sender.h"
#include "settings.h"

static const char *TAG = "power";

#define NVS_NS          "jinxed"
#define NVS_WAKE_NAME   "wname"     /* the note open at power-off */
#define NVS_SENT_NAME   "sentname"
#define NVS_SENT_SEQ    "sentseq"
#define NVS_INTERVAL    "wakeint"   /* seconds, backed off on failure */
#define NVS_WAKE_LINE   "wline"     /* the last dark wake's result */

#define PWROFF_BIT      (1u << 4)   /* expander 0x44 P4, PWROFF_PULSE */
#define PULSE_MS        (500)
#define PULSE_TRIES     (3)

/* A dark wake's own cap, over the send's MAILSEND_TIMEOUT_MS: the card,
 * the boot's NTP job ahead of us, then the send. Past it, off anyway. */
#define DARK_CAP_MS     (120 * 1000)
#define DARK_TEXT_MAX   (256 * 1024)    /* the editor's TEXT_MAX */

#define NAME_MAX        (64)

/* ---- the off switch ----------------------------------------------------- */

/* Read-modify-write one bit of a 0x44 register, as usbhost.c does. */
static esp_err_t exp2_bit(uint8_t reg, bool set)
{
    i2c_master_dev_handle_t d = tab5io_exp2();
    if (!d) return ESP_ERR_INVALID_STATE;
    uint8_t val = 0;
    esp_err_t err = i2c_master_transmit_receive(d, &reg, 1, &val, 1, 100);
    if (err != ESP_OK) return err;
    val = set ? (uint8_t)(val | PWROFF_BIT) : (uint8_t)(val & (uint8_t)~PWROFF_BIT);
    const uint8_t b[2] = { reg, val };
    return i2c_master_transmit(d, b, sizeof b, 100);
}

/* Spec 2, rule 6: output, out of high-Z, high for ~500 ms, low; again if
 * the board is still running a second later. The alarm is already set
 * as it should be. */
static void pulse_off(void)
{
    lcd_backlight_set(0);
    for (int i = 0; i < PULSE_TRIES; i++) {
        ESP_LOGW(TAG, "power off (pulse %d)", i + 1);
        exp2_bit(PI4IOE_REG_IO_DIR, true);
        exp2_bit(PI4IOE_REG_OUT_HIGH_Z, false);
        exp2_bit(PI4IOE_REG_OUT_SET, true);
        vTaskDelay(pdMS_TO_TICKS(PULSE_MS));
        exp2_bit(PI4IOE_REG_OUT_SET, false);
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
    ESP_LOGE(TAG, "still running after %d power-off pulses", PULSE_TRIES);
}

/* ---- what has been sent ------------------------------------------------- */

static uint32_t interval(void)
{
    uint32_t s = POWER_INTERVAL_S;
    nvsh_get_u32(NVS_NS, NVS_INTERVAL, &s);
    if (s < POWER_INTERVAL_S || s > POWER_BACKOFF_MAX_S) s = POWER_INTERVAL_S;
    return s;
}

static void set_interval(uint32_t s)
{
    if (s != interval()) nvsh_set_u32(NVS_NS, NVS_INTERVAL, s);
}

static bool unsent(const char *name, uint32_t seq)
{
    if (!seq) return false;                 /* nothing written yet */
    char sent[NAME_MAX];
    size_t n = sizeof sent;
    uint32_t s = 0;
    if (nvsh_get_str(NVS_NS, NVS_SENT_NAME, sent, &n) != ESP_OK || strcmp(sent, name)) return true;
    if (nvsh_get_u32(NVS_NS, NVS_SENT_SEQ, &s) != ESP_OK) return true;
    return seq != s;        /* fewer is another card's log: send that too */
}

void power_note_sent(const char *name, uint32_t seq)
{
    if (!name || !seq) return;
    nvsh_set_str(NVS_NS, NVS_SENT_NAME, name);
    nvsh_set_u32(NVS_NS, NVS_SENT_SEQ, seq);
    set_interval(POWER_INTERVAL_S);
}

/* Several hundred bytes: off the stack (CLAUDE.md). */
static mailcfg_t s_cfg;

static bool mail_ready(void)
{
    const bool ok = mailcfg_load(NULL, &s_cfg);
    mailcfg_wipe(&s_cfg);
    return ok;
}

/* ---- the calls ---------------------------------------------------------- */

bool power_woke(void)
{
    return rtc8130_alarm_take();
}

void power_off_idle(const char *name, uint32_t seq)
{
    if (name && name[0]) nvsh_set_str(NVS_NS, NVS_WAKE_NAME, name);
    const bool arm = name && unsent(name, seq) && mail_ready();
    /* A boot is a send point (spec 3): the interval runs from here. */
    if (arm && rtc8130_alarm_arm(interval())) {
        ESP_LOGI(TAG, "%s unsent through seq %lu; waking in %lu s", name,
                 (unsigned long)seq, (unsigned long)interval());
    } else {
        rtc8130_alarm_off();
    }
    pulse_off();
}

static void dark_line(const char *s)
{
    ESP_LOGI(TAG, "dark wake: %s", s);
    nvsh_set_str(NVS_NS, NVS_WAKE_LINE, s);
}

static void off_quiet(void)
{
    rtc8130_alarm_off();
    pulse_off();
    for (;;) vTaskDelay(portMAX_DELAY);
}

static void off_backoff(void)
{
    uint32_t next = interval() * 2;
    if (next > POWER_BACKOFF_MAX_S) next = POWER_BACKOFF_MAX_S;
    set_interval(next);
    if (!rtc8130_alarm_arm(next)) rtc8130_alarm_off();
    pulse_off();
    for (;;) vTaskDelay(portMAX_DELAY);
}

static bool past(int64_t t0)
{
    return esp_timer_get_time() - t0 > (int64_t)DARK_CAP_MS * 1000;
}

void power_dark_wake(void)
{
    const int64_t t0 = esp_timer_get_time();
    char name[NAME_MAX];
    size_t n = sizeof name;
    if (nvsh_get_str(NVS_NS, NVS_WAKE_NAME, name, &n) != ESP_OK || !name[0]) {
        dark_line("woke with no note to send");
        off_quiet();
    }
    if (!mail_ready()) off_quiet();         /* spec 2, rule 4: should not have armed */

    while (!storage_present(STORAGE_SD) && esp_timer_get_time() - t0 < 5 * 1000 * 1000) {
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    char *text = heap_caps_malloc(DARK_TEXT_MAX, MALLOC_CAP_SPIRAM);
    int64_t now = 0;
    const bool now_ok = rtc8130_read(&now);
    uint32_t len = 0;
    if (!storage_present(STORAGE_SD) || !text || doclog_init(DARK_TEXT_MAX) != ESP_OK ||
        doclog_open(name, text, &len, now, now_ok) != ESP_OK) {
        /* Nothing on the board can change while it is off; trying again
         * would find the same. The next power-on looks again. */
        dark_line("scheduled send: no card");
        off_quiet();
    }
    const uint32_t seq = doclog_seq();
    if (!unsent(name, seq)) off_quiet();    /* the card decides (spec 3) */

    /* The boot's NTP job holds the send task for a moment. */
    char line[160];
    while (sender_status(line, sizeof line) && !past(t0)) vTaskDelay(pdMS_TO_TICKS(200));
    char range[48], foot[DOCLOG_FOOT_MAX];
    snprintf(range, sizeof range, "seq 1-%lu, scheduled", (unsigned long)seq);
    const esp_app_desc_t *d = esp_app_get_description();
    doclog_footer(foot, settings_author(), now, now_ok, d ? d->version : NULL);
    if (past(t0) || !sender_send_doc(name, range, text, len, foot, doclog_copy_log)) {
        dark_line("scheduled send did not start");
        off_backoff();
    }
    vTaskDelay(pdMS_TO_TICKS(50));
    while (sender_status(line, sizeof line) && !past(t0)) vTaskDelay(pdMS_TO_TICKS(200));
    doclog_close();
    if (sender_sent()) {
        power_note_sent(name, seq);
        char shown[sizeof line + 16];
        snprintf(shown, sizeof shown, "scheduled: %s", line);
        dark_line(shown);
        off_quiet();
    }
    char shown[sizeof line + 16];
    snprintf(shown, sizeof shown, "scheduled: %s", past(t0) ? "timed out" : line);
    dark_line(shown);
    off_backoff();
}

bool power_wake_line(char *line, size_t size)
{
    size_t n = size;
    if (!size || nvsh_get_str(NVS_NS, NVS_WAKE_LINE, line, &n) != ESP_OK || !line[0]) return false;
    nvsh_erase_key(NVS_NS, NVS_WAKE_LINE);
    return true;
}

/*
 * shim.c -- the boxed editor's side of the boundary (u-jinxed-0018).
 *
 * The ELF is the writer's editor.c, files.c, doclog.c, md.c, spell.c and
 * fname.c, unchanged, with this file. Their calls into the host's modules
 * are Jinxed's own calls (../../components/jinxed_box, u-jinxed-0013),
 * and most cross under the writer's own names. Four cannot, and are made
 * here from the own calls that stand in for them:
 *
 *   settings_author()       a host pointer; copied out into ours.
 *   settings_on_send(),     the page calls the editor's send from inside
 *   settings_page()         its loop, and the send is ours. The page comes
 *                           back with 1 for "send now"; we send, and open
 *                           it again. It reopens at its top after a send,
 *                           where the writer's stays put.
 *   sender_send_doc()       takes doclog_copy_log, which the host cannot
 *                           run. The host reads the log itself, from the
 *                           path and end doclog gives (u-jinxed-0017).
 *   display_col             not a call: COL_* reads it. Ours points at a
 *                           copy of the host's theme (u-jinxed-0016),
 *                           fetched at start and after the page.
 *
 * And app_main, which the writer's main.c is on the host: the board is the
 * .bin's (host.c), already up when this runs.
 *
 * SPDX-License-Identifier: MIT
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_log.h"

#include "feckless_abi.h"
#include "feckless_abi_nvs.h"
#include "feckless_abi_storage.h"
#include "feckless_box.h"

#include "display.h"
#include "doclog.h"
#include "editor.h"
#include "files.h"
#include "sender.h"
#include "settings.h"

static const char *TAG = "jinxed_box";

/* Jinxed's own calls that do not have the writer's names
 * (jbox_calls.c, jinxed_calls.txt). */
extern const uint32_t jinxed_calls_v1;
uint32_t jinxed_settings_author(char *buf, uint32_t size);
bool     jinxed_send_doc(const char *name, const char *range, const char *text, uint32_t len,
                         const char *foot, const char *log_path, uint32_t log_len);
int      jinxed_settings_page(void);
int      jinxed_display_col(display_theme_t *out, uint32_t size);

/* ---- the theme --------------------------------------------------------- */

static display_theme_t s_col;
const display_theme_t *display_col = &s_col;

/* The host's theme into ours. If the two builds disagree on its layout
 * the host refuses, and the colours stay as they were: logged, since at
 * the start that is a page of one colour. */
static void col_fetch(void)
{
    if (!jinxed_display_col(&s_col, sizeof s_col)) {
        ESP_LOGE(TAG, "no theme from the host (display_theme_t is %u bytes here)", (unsigned)sizeof s_col);
    }
}

/* ---- settings ---------------------------------------------------------- */

static char s_author[DOCLOG_AUTHOR_MAX + 1];

/* NULL for none, as the writer's. Asked each time: the page may change it. */
const char *settings_author(void)
{
    return jinxed_settings_author(s_author, sizeof s_author) ? s_author : NULL;
}

static bool (*s_send)(void);
void settings_on_send(bool (*send)(void)) { s_send = send; }

void settings_page(void)
{
    while (jinxed_settings_page()) {
        if (s_send) s_send();
    }
    /* What the page may have changed that this side keeps a copy of. The
     * text size, markdown and the rotation are asked of the host each
     * time; the theme and the mode are held here. */
    col_fetch();
    files_forget_mode();
}

/* ---- the sender -------------------------------------------------------- */

/* `log` is doclog_copy_log, or NULL for none; either way it is not called
 * here. The host reads the first `end` bytes of the twin, held to
 * /sd/jinxed, before this returns, and the editor appends only on this
 * thread, so those are the bytes doclog_copy_log() would have read. */
bool sender_send_doc(const char *name, const char *range, const char *text, uint32_t len,
                     const char *foot, size_t (*log)(void *buf, size_t cap, bool *too_big))
{
    long end = 0;
    const char *path = log ? doclog_log_path(&end) : NULL;
    return jinxed_send_doc(name, range, text, len, foot, path, path ? (uint32_t)end : 0);
}

/* ---- the entry --------------------------------------------------------- */

void app_main(void)
{
    feckless_abi_require();
    feckless_abi_require_storage();
    feckless_abi_require_nvs();
    feckless_abi_require_box();
    __asm__ volatile("" :: "r"(&jinxed_calls_v1));      /* the own calls' version */

    col_fetch();
    if (editor_init() != ESP_OK) {
        ESP_LOGE(TAG, "editor_init failed");
        return;
    }
    editor_run();
}

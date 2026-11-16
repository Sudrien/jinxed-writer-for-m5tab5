/*
 * shim_host_test.c -- the boxed editor's side of the boundary
 * (main/shim.c, u-jinxed-0018), on the host (u-jinxed-0027).
 *
 * shim.c is compiled as it ships. Jinxed's own calls under it are
 * stand-ins that record what they were given and answer as the host's
 * handlers would (components/jinxed_box/src/jbox_calls.c), so what is
 * checked is the shim's half: what it asks the host for, when, and what
 * it does with the answers. The four things it makes:
 *
 *   settings_page()     the page comes back 1 for "send now": the editor's
 *                       send runs, the page opens again; at 0 it is done,
 *                       and the theme is fetched again and the notes' mode
 *                       forgotten -- the two things the page may change
 *                       that the box keeps a copy of.
 *   sender_send_doc()   with a log function, doclog's path and end go to
 *                       the host, which reads the log itself; without, no
 *                       log; the strings and the text as given.
 *   settings_author()   the host's name copied out, or NULL for none.
 *   display_col         the box's own copy of the host's theme, by value.
 *
 * Built and run by ./run.sh, with FECKLESS=<feckless-libs> for the
 * headers. The editor itself (editor_init, editor_run) is not linked:
 * app_main is not run.
 *
 * SPDX-License-Identifier: MIT
 */
#include <stdio.h>
#include <string.h>

#include "../main/shim.c"

static int checks, failures;
#define CHECK(c, ...) do { checks++; if (!(c)) { failures++; printf("FAIL %s:%d: ", __FILE__, __LINE__); \
                           printf(__VA_ARGS__); printf("\n"); } } while (0)

/* What app_main refers to, so the ELF imports them; here, data. */
const uint32_t jinxed_calls_v1 = 1;
const uint32_t feckless_abi_v1 = 1, feckless_abi_box_v1 = 1, feckless_abi_nvs_v1 = 1, feckless_abi_storage_v2 = 1;
const void *const feckless_abi_table = NULL;
esp_err_t editor_init(void) { return ESP_OK; }
void editor_run(void) {}
void esp_log_write(int level, const char *tag, const char *fmt, ...) { (void)level; (void)tag; (void)fmt; }

/* ---- the host, as Jinxed's own calls answer ---------------------------- */

static display_theme_t h_theme;
static int h_col_calls;
int jinxed_display_col(display_theme_t *out, uint32_t size)
{
    h_col_calls++;
    if (size != sizeof h_theme) return 0;
    *out = h_theme;
    return 1;
}

static const char *h_author;
uint32_t jinxed_settings_author(char *buf, uint32_t size)
{
    if (!h_author || !size) return 0;
    snprintf(buf, size, "%s", h_author);
    return (uint32_t)strlen(buf);
}

static int h_page_presses;          /* how many times the page says "send now" before it is left */
static int h_page_opens;
int jinxed_settings_page(void)
{
    h_page_opens++;
    if (h_page_presses > 0) { h_page_presses--; return 1; }
    return 0;
}

static char h_name[64], h_range[40], h_foot[160], h_log[256];
static const char *h_text;
static uint32_t h_len, h_log_len;
static bool h_foot_null, h_log_null;
bool jinxed_send_doc(const char *name, const char *range, const char *text, uint32_t len,
                     const char *foot, const char *log_path, uint32_t log_len)
{
    snprintf(h_name, sizeof h_name, "%s", name);
    snprintf(h_range, sizeof h_range, "%s", range);
    h_foot_null = foot == NULL;
    snprintf(h_foot, sizeof h_foot, "%s", foot ? foot : "");
    h_log_null = log_path == NULL;
    snprintf(h_log, sizeof h_log, "%s", log_path ? log_path : "");
    h_text = text;
    h_len = len;
    h_log_len = log_len;
    return true;
}

/* ---- the box's own modules the shim calls ------------------------------ */

static const char *d_path;
static long d_end;
const char *doclog_log_path(long *end) { *end = d_path ? d_end : 0; return d_path; }
size_t doclog_copy_log(void *buf, size_t cap, bool *too_big) { (void)buf; (void)cap; *too_big = false; return 0; }

static int f_forgets;
void files_forget_mode(void) { f_forgets++; }

static int e_sends;
static bool editor_send(void) { e_sends++; return true; }

int main(void)
{
    /* the theme, by value */
    memset(&h_theme, 0, sizeof h_theme);
    h_theme.bg = 0x1111; h_theme.text = 0x2222; h_theme.bad = 0x3333;
    col_fetch();
    CHECK(display_col == &s_col && display_col->bg == 0x1111 && display_col->bad == 0x3333,
          "display_col is the box's copy of the host's theme");
    h_theme.bg = 0x4444;
    CHECK(display_col->bg == 0x1111, "and a copy: the host's change is not seen until fetched");

    /* the settings page: left at once */
    settings_on_send(editor_send);
    h_page_presses = 0; h_page_opens = 0; e_sends = 0; f_forgets = 0; h_col_calls = 0;
    settings_page();
    CHECK(h_page_opens == 1 && e_sends == 0, "left at once: opened once, nothing sent (%d, %d)", h_page_opens, e_sends);
    CHECK(h_col_calls == 1 && display_col->bg == 0x4444, "the theme fetched again after the page");
    CHECK(f_forgets == 1, "the notes' mode forgotten after the page");

    /* "send now" twice, then left: sent twice, the page opened three times */
    h_page_presses = 2; h_page_opens = 0; e_sends = 0; f_forgets = 0; h_col_calls = 0;
    settings_page();
    CHECK(h_page_opens == 3 && e_sends == 2, "send now twice: opened %d times, sent %d", h_page_opens, e_sends);
    CHECK(h_col_calls == 1 && f_forgets == 1, "fetched and forgotten once, when it is left");

    /* no send registered: "send now" sends nothing, the page still reopens */
    settings_on_send(NULL);
    h_page_presses = 1; h_page_opens = 0; e_sends = 0;
    settings_page();
    CHECK(h_page_opens == 2 && e_sends == 0, "no send: reopened, nothing sent");

    /* the author */
    h_author = NULL;
    CHECK(settings_author() == NULL, "no author: NULL, as the writer's");
    h_author = "Ada";
    const char *a = settings_author();
    CHECK(a && !strcmp(a, "Ada") && a == s_author, "the author copied into the box's buffer");
    h_author = "A name a good deal longer than thirty-two bytes, truncated";
    a = settings_author();
    CHECK(a && strlen(a) == DOCLOG_AUTHOR_MAX, "a long name truncated to DOCLOG_AUTHOR_MAX (%zu)", a ? strlen(a) : 0);

    /* the send: with the log */
    static const char text[] = "# some words\n";
    d_path = "/sd/jinxed/.notes.md.h0";
    d_end = 1234;
    CHECK(sender_send_doc("notes.md", "seq 1-9", text, 13, "-- Ada", doclog_copy_log),
          "the send goes");
    CHECK(!strcmp(h_name, "notes.md") && !strcmp(h_range, "seq 1-9") && !strcmp(h_foot, "-- Ada") && !h_foot_null,
          "the strings as given");
    CHECK(h_text == text && h_len == 13, "the text as given (the host copies it)");
    CHECK(!h_log_null && !strcmp(h_log, "/sd/jinxed/.notes.md.h0") && h_log_len == 1234,
          "the log: doclog's path and end (%s, %u)", h_log, (unsigned)h_log_len);

    /* without a log function, no log; without a foot, none */
    sender_send_doc("notes.md", "not yet saved", text, 13, NULL, NULL);
    CHECK(h_log_null && h_log_len == 0 && h_foot_null, "no log function: no log; no foot: NULL");

    /* a log function, but no log open */
    d_path = NULL;
    sender_send_doc("notes.md", "seq 1-1", text, 13, "", doclog_copy_log);
    CHECK(h_log_null && h_log_len == 0, "no log open: no log");

    printf("shim: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}

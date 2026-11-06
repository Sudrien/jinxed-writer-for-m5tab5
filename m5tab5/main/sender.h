/*
 * sender.h -- the send task: Wi-Fi up, one email, Wi-Fi down. Spec
 * section 6.
 *
 * One lower-priority task, made once and kept, so TLS and SMTP never run
 * on the editor's task and typing never waits on them. For now it sends
 * one thing, the settings page's test message; the background send of
 * the log's changes will be the second. It also fetches the spelling
 * dictionary (spell.h) when the card has none: the same radio, the same
 * TLS, one job at a time.
 *
 * Wi-Fi is up only for a send: the radio's policy hook says "on" while a
 * send wants it and "off" otherwise, and feckless_network_handler's worker
 * powers the C6 and joins the saved networks -- Avoidant's, in NVS under
 * "defeatist", so networks saved there work here (spec 6: credentials
 * kept the way the saved networks are). A USB Ethernet adapter with an
 * address counts as a network too.
 *
 * The whole attempt -- radio, join, send -- is capped at
 * MAILSEND_TIMEOUT_MS (spec section 3).
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

/* The network's start, with the saved networks and Avoidant's namespace,
 * and the task. After nvsh_init(), usbhost_init() and storage_init(),
 * before usbhost_start(). Nothing is powered until a send asks. */
esp_err_t sender_init(void);

/* Start a test send. False if one is already running. */
bool sender_test(void);

/*
 * 0038: start sending `text` (len bytes, no NUL needed), then `foot`,
 * as one message: subject "Jinxed Writer: <name>" and `range`. Both are
 * copied before this returns, so the caller's text may change at once.
 * False if a send or a fetch is running, or there is no room for it.
 *
 * 0041: the text is inline, as before, and also attached as `name`
 * (text/markdown, footer and all); and `log` -- called here, on the
 * caller's task, into a buffer of SENDER_LOG_MAX -- is attached as
 * `name`.h0 when it gives bytes. A log over the cap is left off and
 * the result line says so.
 */
#define SENDER_LOG_MAX  (4 * 1024 * 1024)
bool sender_send_doc(const char *name, const char *range, const char *text, uint32_t len,
                     const char *foot, size_t (*log)(void *buf, size_t cap, bool *too_big));

/* Start fetching the spelling dictionary from SPELL_DICT_URL to
 * SPELL_DIR/SPELL_DICT. False if a send or a fetch is already running.
 * Progress and the outcome are the status line; sender_fetched() says
 * whether the last fetch put a dictionary on the card. */
bool sender_fetch_dict(void);
bool sender_fetched(void);

/* Whether a send is running, and a line for the settings page: what it
 * is doing or how the last one ended, "" before the first. Domain only,
 * never the whole address (spec section 6). */
bool sender_status(char *line, size_t size);

/* Bumped whenever the status line changes, so a page can redraw on it. */
uint32_t sender_gen(void);

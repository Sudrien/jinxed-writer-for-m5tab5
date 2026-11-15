/*
 * jinxed_box.h -- what Jinxed's own .bin loads a boxed editor with
 * (u-jinxed-0003, 0004, 0012, 0013, UMODE.md).
 *
 * feckless_box_calls_v1()'s table, every name of it, with the editor's
 * writes held to JINXED_BOX_ROOT by feckless's write filter
 * (u-feckless-0055), which sees the host's copy of each path the app would
 * write; reads go anywhere, as feckless has them. Then Jinxed's own calls
 * (src/jbox_calls.c, jinxed_calls.txt): the editor's keyboard, sender,
 * power, settings and display settings, which stay on the host. NVS, for
 * files.c's list of notes, in the namespace "jinxed" only.
 *
 *   jinxed_box_host(&host);                                       // once, main's functions
 *   uint32_t n;
 *   const feckless_umode_call_t *calls = jinxed_box_calls(&n);   // for each load
 *   feckless_umode_load(image, size, calls, n, box_size, stack, &app);
 *   ...
 *   feckless_umode_unload(app);
 *   feckless_box_calls_end();      // forgets the filter too
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "feckless_umode_calls.h"
#include "kbd.h"                    /* the writer's main/: kbd_event_t */

#ifdef __cplusplus
extern "C" {
#endif

#ifndef JINXED_BOX_ROOT
#define JINXED_BOX_ROOT "/sd/jinxed"
#endif

/* Installs the filter and the NVS namespace, and returns feckless's table
 * with Jinxed's own calls after it, built once; NULL and *n 0 if it could
 * not be allocated. Call it before each feckless_umode_load(), since
 * feckless_box_calls_end() forgets the filter and the namespace with the
 * app. */
const feckless_umode_call_t *jinxed_box_calls(uint32_t *n);

/* u-jinxed-0013: the host's functions the own calls answer with, given by
 * the writer's main (this component cannot require main). Each has the
 * signature of the writer's function of that name; one left NULL makes
 * its call answer 0. Copied; NULL clears them. */
typedef struct {
    bool (*kbd_get)(kbd_event_t *ev, uint32_t ticks);
    bool (*kbd_present)(void);
    char (*kbd_ascii)(uint8_t usage, uint8_t mods, bool caps);
    bool (*sender_fetch_dict)(void);
    bool (*sender_fetched)(void);
    bool (*sender_sent)(void);
    uint32_t (*sender_gen)(void);
    bool (*sender_status)(char *line, size_t size);
    bool (*sender_send_doc)(const char *name, const char *range, const char *text, uint32_t len,
                            const char *foot, size_t (*log)(void *buf, size_t cap, bool *too_big));
    int  (*power_idle_seconds)(int kind);
    void (*power_note_sent)(const char *name, uint32_t seq);
    void (*power_off_idle)(const char *name, uint32_t seq);
    bool (*power_wake_line)(char *line, size_t size);
    void (*settings_draw_gear)(void);
    int  (*settings_gear_w)(void);
    bool (*settings_gear_hit)(int x, int y);
    const char *(*settings_author)(void);
    void (*settings_on_send)(bool (*send)(void));
    void (*settings_page)(void);
    void (*settings_close)(void);
    int  (*display_text_scale)(void);
    int  (*display_foot_scale)(void);
    void (*display_set_text_scale)(int scale);
    bool (*display_markdown)(void);
} jinxed_host_t;
void jinxed_box_host(const jinxed_host_t *h);

/* Jinxed's own calls alone, jinxed_calls_v1 first (jinxed_calls.txt). */
const feckless_umode_call_t *jinxed_box_own_calls(uint32_t *n);

/* The longest path the own calls take from the app (jinxed_send_doc's
 * log). */
#define JINXED_BOX_PATH_MAX 256

/* The filter: true if path is JINXED_BOX_ROOT or below it, with no ".."
 * segment. */
bool jinxed_box_may_write(const char *path);

#ifdef __cplusplus
}
#endif

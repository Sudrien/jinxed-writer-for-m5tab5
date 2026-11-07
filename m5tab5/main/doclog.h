/*
 * doclog.h -- the document as an operation log, mirrored on the SD card.
 * Spec section 4.
 *
 * The log is the document. Each record is one changeset: a subset of
 * Etherpad's Easysync with no attribute pool and no follow/merge -- old
 * length, new length, a list of keep / delete / insert ops and a char
 * bank holding the inserted bytes. Replaying every changeset from the
 * empty text gives the text. Authorship is kept, per changeset rather
 * than per character: the record's source byte names the user or the
 * spelling check (0011), where Easysync would put an `author` attribute
 * from its pool on each op.
 *
 * Two twins, `<dir>/.<name>.h0` and `.h1`, each a complete log. Every
 * append goes to h0 and is fsynced and closed, and only then to h1, so a
 * power loss tears the tail of at most one. Recovery takes the longer
 * valid prefix and copies its tail onto the other.
 *
 * FILE LAYOUT (little-endian)
 *
 *   header, 64 bytes
 *     0   magic "JINXLOG\0"
 *     8   u32 format version (1)
 *     12  u32 doc id (random at creation)
 *     16  u8  highlight mode (0 none, 1 markdown+math, 2 TeX)
 *     20  i64 creation time (RTC epoch)
 *     28  u32 last export hash (0: never exported)
 *     60  u32 CRC-32C of bytes 0..59
 *
 *   records, back to back
 *     0   u32 payload length
 *     4   u8  type (1 changeset, 2 author)
 *     5   u8  flags (bit 0: time untrusted -- the RTC reported VLF)
 *     6   u8  source: who made it, a doclog_src_t (0 in logs written
 *             before there was one, which is "the user")
 *     7   u8  author (0035): 0 the default, "user"; otherwise the id of
 *             an author record earlier in this log. Only a user's
 *             changeset carries one; the spelling check's are 0
 *     8   u32 seq, monotonic from 1 over changesets; an author record
 *             repeats the seq before it
 *     12  i64 time (RTC epoch, advisory)
 *     20  payload
 *     ..  u32 CRC-32C over bytes 0..19 and the payload, seeded with the
 *         previous record's CRC (the header's for the first)
 *
 *   then zeros: the file is grown in DOCLOG_GROW steps of zeros ahead of
 *   the records, and recovery stops at the first length/CRC that fails,
 *   which a zero length always does.
 *
 *   changeset payload
 *     u32 old_len, u32 new_len, u16 n_ops, then n_ops x { u8 op, u32 n }
 *     with op '=' keep, '-' delete, '+' insert, then the char bank. Text
 *     after the last op is kept implicitly, as in Easysync.
 *
 *   author payload (0035), Easysync's pool entry for one author
 *     u8 id (the next, from 1), u8 len (1..DOCLOG_AUTHOR_MAX), the name
 *     Written once per name per log, just before the first changeset
 *     that names it, and only for a name other than the default: a log
 *     with no name set has no author records. Firmware before 0035 stops
 *     reading at an author record and writes over it and everything
 *     after it, so a log with one must not be opened by that firmware.
 *
 * Not thread-safe: the editor is the only caller (spec section 6, "the
 * editor is the only log writer").
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

/* Where the logs live, and the twins' suffixes. */
#ifndef DOCLOG_DIR
#define DOCLOG_DIR      "/sd/jinxed"
#endif
#define DOCLOG_SUFFIX0  ".h0"
#define DOCLOG_SUFFIX1  ".h1"

/* Who made a changeset: the user typing, or a correction taken from
 * the spelling check. Record byte 6. A reader that predates it ignores
 * it; every record it wrote says 0, the user. */
typedef enum {
    DOCLOG_SRC_USER     = 0,
    DOCLOG_SRC_SYMSPELL = 1,
} doclog_src_t;

/*
 * 0041: the open log's first twin (h0) into `buf`, for an email -- its
 * header and records, without the room the file has grown past them
 * (0042): its length, or 0 when there is no log open, no card, it will
 * not read, or it is longer than `cap` (*too_big then true). On the caller's task,
 * which is the one that appends, so the read never meets a write.
 */
size_t doclog_copy_log(void *buf, size_t cap, bool *too_big);

/* 0038: the export's footer (0037) on its own, for an email's body:
 * at most DOCLOG_FOOT_MAX bytes, NUL-terminated, into `f`. Its length. */
#define DOCLOG_FOOT_MAX     (160)
uint32_t doclog_footer(char *f, const char *author, int64_t now, bool now_ok, const char *build);

/* 0035: an author's name, bytes, and how many one log can name. */
#define DOCLOG_AUTHOR_MAX   (32)
#define DOCLOG_AUTHORS_MAX  (32)

/* Allocate buffers for texts of up to `cap` bytes. Once. */
esp_err_t doclog_init(uint32_t cap);

/*
 * Open (recovering or creating) the log for `name` in DOCLOG_DIR, and
 * replay it into `text` (cap bytes), setting *len. A document with no log
 * but an export file `DOCLOG_DIR/name` is imported as one insert.
 * `now`/`now_ok` timestamp a creation or import.
 */
esp_err_t doclog_open(const char *name, char *text, uint32_t *len,
                      int64_t now, bool now_ok);

bool doclog_is_open(void);

/*
 * Append the changeset taking `old` to `neu` to both twins, made by
 * `src` -- and, for DOCLOG_SRC_USER, credited to `author` (0035): NULL
 * or "" is the default, "user"; any other name is recorded in the log
 * the first time it is used. `old` must be the text the log replays to (the last appended or
 * opened text). No-op when they are equal. On failure nothing is assumed
 * written; call again with the same `old` later.
 */
esp_err_t doclog_append(const char *old, uint32_t old_len,
                        const char *neu, uint32_t new_len,
                        int64_t now, bool now_ok, doclog_src_t src, const char *author);

/*
 * 0036: write `text` -- the text the log replays to -- as the export,
 * DOCLOG_DIR/name: to name.tmp, synced, then in place of the old one.
 * What was last exported is recorded in a hidden sidecar, `.name.x`
 * (the header cannot hold it: its CRC seeds the record chain). If the
 * export on the card is not what was last written there -- edited on a
 * computer -- it is left alone and the text goes to `name.conflict.ext`
 * instead: ESP_ERR_INVALID_STATE. An import records its file as
 * exported, so it is not a conflict with itself; an export already on
 * the card beside a log with no sidecar (an import before 0036) is.
 *
 * 0037: the export ends in a footer the log does not hold -- a rule,
 * `author: <name>` when `author` is set, `exported: YYYY-MM-DD HH:MM`
 * from `now` when `now_ok`, and `Jinxed Writer <build>`. An import drops
 * a footer of this shape. The sidecar also keeps the text's own hash, so
 * an export with the text unchanged writes nothing, though its time
 * would differ.
 */
esp_err_t doclog_export(const char *text, uint32_t len, const char *author,
                        int64_t now, bool now_ok, const char *build);

/* 0035: the id the open log credits `name` as: 0 for NULL or "", -1 if
 * it has not named it. */
int doclog_author_id(const char *name);

/*
 * After the card came and went: is the log on the card still the one
 * this module last wrote? ESP_OK yes; ESP_ERR_NOT_FOUND no log there;
 * ESP_ERR_INVALID_STATE some other log (a different card or doc).
 * Repairs a torn twin on the way, as open does.
 */
esp_err_t doclog_verify(void);

/*
 * 0044: a document's files, as one -- the export, both twins, the
 * sidecar and the conflict file (name.conflict.ext) -- for file mode.
 * Never on the open log's document (doclog_close() first):
 * ESP_ERR_INVALID_STATE.
 *
 * doclog_exists(): whether the export or either twin is there.
 *
 * doclog_rename(): every file of `from` that is there to `to`'s name.
 * ESP_ERR_INVALID_STATE if `to` has any file already, ESP_ERR_NOT_FOUND
 * if `from` has none. The twins and the sidecar go before the export, so
 * a power cut part way leaves the text under both names -- the export
 * under `from` imports as a document of its own -- and never under none.
 *
 * doclog_trash(): every file of `name` into DOCLOG_DIR/trash, made if it
 * is not there (spec 4: delete moves, never erases). A name already in
 * the trash keeps its file; this one gets "-2", "-3"... before its
 * extension, each of its files the same. Nothing is erased.
 */
bool doclog_exists(const char *name);
esp_err_t doclog_rename(const char *from, const char *to);
esp_err_t doclog_trash(const char *name);
#define DOCLOG_TRASH        "trash"

/* Forget the open log. Files are untouched. */
void doclog_close(void);

/* The last seq written or recovered. */
uint32_t doclog_seq(void);

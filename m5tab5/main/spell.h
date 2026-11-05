/*
 * spell.h -- spelling, checked on demand. Spec section 5.
 *
 * SymSpell (Wolf Garbe's symmetric-delete algorithm): every dictionary
 * word's first SPELL_PREFIX letters, with up to SPELL_MAX_DIST of them
 * deleted, index the word. A misspelling's own deletes are looked up in
 * that index, and what they find is ranked by Damerau (OSA) distance and
 * then by how common the word is.
 *
 * The dictionary is one file on the card, SPELL_DIR/SPELL_DICT, built on
 * a computer by tools/spelldict.c from a "word frequency" list. Its word
 * list (about 1.7 MB for 86,000 words) is read into PSRAM by
 * spell_load() and stays there; the delete index, tens of megabytes, stays
 * on the card and is read a few bytes at a time by spell_suggest(), which
 * leases the volume for the one call. Checking whether a word is known
 * touches only RAM.
 *
 * The user's own words are SPELL_DIR/SPELL_USER, one per line, appended
 * to by spell_add(). "Ignore all" lasts until power-off.
 *
 * ASCII only, as the editor is. Not thread-safe: the editor's task only.
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifndef SPELL_DIR
#define SPELL_DIR       "/sd/jinxed/spell"
#endif
#define SPELL_DICT      "english.sym"
#define SPELL_USER      "words.txt"

/* Where the board fetches the dictionary when the card has none: a
 * release asset on this repository, english.sym as tools/spelldict.c
 * builds it from symspell-c99's dictionaries/dictionary.txt (commit
 * a764b99). A new dictionary is a new tag, so a board never fetches a
 * file other than the one its firmware names. */
#define SPELL_DICT_TAG  "dict-en-1"
#define SPELL_DICT_URL  "https://github.com/Sudrien/jinxed-writer-for-m5tab5/releases/download/" \
                        SPELL_DICT_TAG "/" SPELL_DICT

/* Fixed in the file format: a dictionary built with others is refused. */
#define SPELL_MAX_DIST  (2)
#define SPELL_PREFIX    (7)

/* The longest word checked or suggested. Longer runs of letters are
 * left alone. */
#define SPELL_WORD_MAX  (40)
#define SPELL_SUGG_MAX  (9)

typedef char spell_word_t[SPELL_WORD_MAX + 1];

/* Whether `path` holds a whole dictionary of exactly `size` bytes: its
 * header good and its sections adding up to the size. Touches nothing
 * else, so any task may call it; the caller holds the volume. */
bool spell_file_ok(const char *path, long size);

/* Read the dictionary and the user's words from the card. True when a
 * dictionary is loaded, now or from before (a second call is cheap).
 * False with no card or no valid file; try again later. */
bool spell_load(void);
bool spell_loaded(void);

/*
 * The next word to check in text[from, len): letters, with apostrophes
 * between letters. Skipped, as not prose: single letters, words in
 * capitals (acronyms), words with a capital inside (identifiers), words
 * over SPELL_WORD_MAX, and any word in a run of non-space characters
 * that holds a digit, / \ @ _ ` < > or = (paths, addresses, code, TeX).
 * The word is [*a, *b). False when there is none.
 */
bool spell_next_word(const char *text, uint32_t len, uint32_t from,
                     uint32_t *a, uint32_t *b);

/* Whether w[0, n) is spelt right: in the dictionary, the user's words
 * or the ignored words, ignoring case. A contraction is right when its
 * stem is (don't, isn't, we'll, John's). */
bool spell_known(const char *w, size_t n);

/* Up to `max` suggestions for w[0, n), best first, lower case. Reads
 * the card. The count, 0 when there are none or the card cannot be read. */
int spell_suggest(const char *w, size_t n, spell_word_t *out, int max);

/* Make `sugg` take the case of w[0, n): Capitalised, or left lower. */
void spell_case_like(const char *w, size_t n, char *sugg);

/* Accept w[0, n) until power-off. */
void spell_ignore(const char *w, size_t n);

/* Accept w[0, n) for good: appended to the user's words on the card.
 * False when the card could not be written (it is still accepted until
 * power-off). */
bool spell_add(const char *w, size_t n);

#ifdef SPELL_HOST
/* tools/spelldict.c: build `out` from a word-frequency list. 0 on
 * success. Host builds only. */
int spell_build(const char *list_path, const char *out_path);
/* Card reads spell_suggest() has made, for the host test. */
extern unsigned spell_reads;
/* Forget everything loaded, as a power-off would. */
void spell_reset(void);
#endif

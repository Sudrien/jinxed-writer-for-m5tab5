/*
 * fname.h -- a note's name as typed, made a name the card will take
 * (0044). Plain C, no I/O: test/fname_host_test.c runs it on the host.
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>

/* Why a name was refused, for the page to say; NULL when it was taken. */
typedef const char *fname_why_t;

/*
 * `typed` into `out` (cap `n`): spaces at either end dropped, ".md" put
 * on when it ends in none of .md, .txt or .tex (any case). Letters,
 * digits, space and - _ . ( ) ' , only -- what FAT, a Mac and Windows
 * all take, and nothing a shell or a mail header has to quote -- and it
 * may not start with a dot (that is a log's).
 * Returns NULL when `out` holds the name, or why it does not.
 */
fname_why_t fname_fix(const char *typed, char *out, size_t n);

/* Whether `name` ends in .md, .txt or .tex, any case: a note. */
bool fname_is_note(const char *name);

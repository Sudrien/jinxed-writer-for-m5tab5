/*
 * md.h -- the markdown of one line, as a style for each byte (0031).
 *
 * Inline only, a line at a time -- no style crosses a newline, as spec 5
 * has it for $math$ -- and only what Jinxed draws:
 *
 *   **bold** __bold__  *italic* _italic_  ***both***  ~~struck~~
 *   `code` (any run of backticks, closed by one as long)
 *   # Heading .. ###### Heading, at the start of the line
 *   \* and the like: a backslash before ASCII punctuation makes it plain
 *
 * Delimiters follow CommonMark's flanking rules, so `2 * 3 * 4` stays
 * plain, and an underscore inside a word (snake_case_name) never opens
 * or closes anything. Strikethrough is GFM's double tilde. The markers
 * are kept and marked MD_MARK, to be drawn dim: nothing is hidden, so a
 * byte on screen is a byte in the file.
 *
 * Pure C, no IDF: ./test/run.sh runs it on the host.
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#define MD_MARK     (0x01)  /* a marker: drawn dim */
#define MD_BOLD     (0x02)
#define MD_ITALIC   (0x04)
#define MD_STRIKE   (0x08)
#define MD_CODE     (0x10)  /* inside `code`, its backticks too */
#define MD_HEAD     (0x20)  /* a heading's text */

/* st[0..n) for the line s[0..n), which holds no newline. */
void md_line(const char *s, size_t n, uint8_t *st);

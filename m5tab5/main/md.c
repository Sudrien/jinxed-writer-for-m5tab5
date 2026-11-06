/*
 * md.c -- see md.h.
 *
 * Three passes over the line: code spans first (nothing inside them is
 * markdown), then the heading, then emphasis -- delimiter runs of *, _
 * and ~ matched by a small version of CommonMark's "process emphasis":
 * a closer takes the nearest opener of its own character, two at a time
 * when both have two (bold) and one otherwise (italic); tildes only in
 * twos. What is left over is plain text.
 *
 * SPDX-License-Identifier: MIT
 */
#include "md.h"

#include <stdbool.h>
#include <string.h>

#define MD_LIT      (0x80)  /* escaped or coded: no delimiter here (internal) */
#define OPENERS_MAX (64)

static bool is_space(int c) { return c == ' ' || c == '\t' || c < 0; }

static bool is_punct(int c)
{
    return (c >= '!' && c <= '/') || (c >= ':' && c <= '@') ||
           (c >= '[' && c <= '`') || (c >= '{' && c <= '~');
}

static void code_spans(const char *s, size_t n, uint8_t *st)
{
    size_t i = 0;
    while (i < n) {
        if (s[i] == '\\' && i + 1 < n && is_punct((unsigned char)s[i + 1])) {
            st[i] |= MD_MARK;
            st[i + 1] |= MD_LIT;
            i += 2;
            continue;
        }
        if (s[i] != '`') { i++; continue; }
        size_t k = 0;
        while (i + k < n && s[i + k] == '`') k++;
        /* A closing run exactly as long. */
        size_t j = i + k, close = n;
        while (j < n) {
            if (s[j] != '`') { j++; continue; }
            size_t m = 0;
            while (j + m < n && s[j + m] == '`') m++;
            if (m == k) { close = j; break; }
            j += m;
        }
        if (close == n) {                       /* unmatched: literal */
            for (size_t q = i; q < i + k; q++) st[q] |= MD_LIT;
            i += k;
            continue;
        }
        for (size_t q = i; q < close + k; q++) st[q] |= MD_CODE | MD_LIT;
        for (size_t q = 0; q < k; q++) { st[i + q] |= MD_MARK; st[close + q] |= MD_MARK; }
        i = close + k;
    }
}

static void heading(const char *s, size_t n, uint8_t *st)
{
    size_t i = 0;
    while (i < n && i < 3 && s[i] == ' ') i++;      /* up to three spaces in */
    size_t h = 0;
    while (i + h < n && s[i + h] == '#') h++;
    if (h < 1 || h > 6 || (i + h < n && s[i + h] != ' ' && s[i + h] != '\t')) return;
    if (st[i] & MD_LIT) return;                     /* \# */
    for (size_t q = i; q < i + h; q++) st[q] |= MD_MARK;
    for (size_t q = i; q < n; q++) st[q] |= MD_HEAD;
}

typedef struct { size_t pos, len; char c; } run_t;

static void emphasis(const char *s, size_t n, uint8_t *st)
{
    run_t open[OPENERS_MAX];
    int top = 0;
    size_t i = 0;
    while (i < n) {
        const char c = s[i];
        if ((c != '*' && c != '_' && c != '~') || (st[i] & MD_LIT)) { i++; continue; }
        size_t len = 0;
        while (i + len < n && s[i + len] == c && !(st[i + len] & MD_LIT)) len++;
        const int prev = i > 0 ? (unsigned char)s[i - 1] : -1;
        const int next = i + len < n ? (unsigned char)s[i + len] : -1;
        const bool left  = !is_space(next) && (!is_punct(next) || is_space(prev) || is_punct(prev));
        const bool right = !is_space(prev) && (!is_punct(prev) || is_space(next) || is_punct(next));
        bool can_open = left, can_close = right;
        if (c == '_') {                         /* never inside a word */
            can_open  = left  && (!right || is_punct(prev));
            can_close = right && (!left  || is_punct(next));
        }
        run_t r = { i, len, c };
        i += len;
        if (c == '~' && len != 2) continue;     /* GFM: ~~ only, here */

        while (can_close && r.len > 0) {
            int o = top - 1;
            while (o >= 0 && open[o].c != c) o--;
            if (o < 0) break;
            const size_t use = c == '~' ? 2 : (open[o].len >= 2 && r.len >= 2) ? 2 : 1;
            const uint8_t style = c == '~' ? MD_STRIKE : use == 2 ? MD_BOLD : MD_ITALIC;
            const size_t a = open[o].pos + open[o].len;     /* first byte inside */
            for (size_t q = a - use; q < a; q++) st[q] |= MD_MARK;
            for (size_t q = r.pos; q < r.pos + use; q++) st[q] |= MD_MARK;
            for (size_t q = a; q < r.pos; q++) st[q] |= style;
            open[o].len -= use;
            r.pos += use;
            r.len -= use;
            top = open[o].len ? o + 1 : o;      /* openers above it are dropped */
        }
        if (can_open && r.len > 0 && top < OPENERS_MAX) open[top++] = r;
    }
}

void md_line(const char *s, size_t n, uint8_t *st)
{
    memset(st, 0, n);
    code_spans(s, n, st);
    heading(s, n, st);
    emphasis(s, n, st);
    for (size_t i = 0; i < n; i++) st[i] &= (uint8_t)~MD_LIT;
}

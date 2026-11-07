/*
 * fname.c -- see fname.h (0044).
 *
 * SPDX-License-Identifier: MIT
 */
#include "fname.h"

#include <stdio.h>
#include <string.h>
#include <strings.h>

static bool ends(const char *s, size_t n, const char *ext)
{
    const size_t e = strlen(ext);
    return n > e && !strncasecmp(s + n - e, ext, e);
}

bool fname_is_note(const char *name)
{
    const size_t n = name ? strlen(name) : 0;
    return ends(name, n, ".md") || ends(name, n, ".txt") || ends(name, n, ".tex");
}

static bool allowed(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
           c == ' ' || c == '-' || c == '_' || c == '.' || c == '(' || c == ')' ||
           c == '\'' || c == ',';
}

fname_why_t fname_fix(const char *typed, char *out, size_t n)
{
    if (!out || n == 0) return "no room";
    out[0] = '\0';
    if (!typed) return "Type a name.";
    while (*typed == ' ') typed++;
    size_t len = strlen(typed);
    while (len && typed[len - 1] == ' ') len--;
    if (!len) return "Type a name.";
    if (typed[0] == '.') return "A name cannot start with a dot.";
    for (size_t i = 0; i < len; i++)
        if (!allowed(typed[i])) return "Letters, digits, spaces and - _ . ( ) ' , only.";
    const bool note = ends(typed, len, ".md") || ends(typed, len, ".txt") || ends(typed, len, ".tex");
    const int w = snprintf(out, n, "%.*s%s", (int)len, typed, note ? "" : ".md");
    if (w < 0 || (size_t)w >= n) {
        out[0] = '\0';
        return "That name is too long.";
    }
    return NULL;
}

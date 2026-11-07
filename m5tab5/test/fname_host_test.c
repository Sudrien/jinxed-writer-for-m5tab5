/*
 * fname_host_test.c -- fname.c (0044) on the host: what a typed name
 * becomes, and what is refused.
 *
 * SPDX-License-Identifier: MIT
 */
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "fname.h"

static const char *fix(const char *t, size_t n)
{
    static char out[64];
    return fname_fix(t, out, n) ? NULL : out;
}

int main(void)
{
    assert(!strcmp(fix("ideas", 64), "ideas.md"));
    assert(!strcmp(fix("  ideas  ", 64), "ideas.md"));
    assert(!strcmp(fix("Draft (2).TXT", 64), "Draft (2).TXT"));
    assert(!strcmp(fix("paper.tex", 64), "paper.tex"));
    assert(!strcmp(fix("v1.2 notes", 64), "v1.2 notes.md"));       /* a dot inside is not an extension */
    assert(!strcmp(fix("it's, mine", 64), "it's, mine.md"));
    assert(!fix("", 64) && !fix("   ", 64) && !fix(NULL, 64));
    assert(!fix(".h0", 64) && !fix(" .hidden", 64));
    assert(!fix("a/b", 64) && !fix("a\\b", 64) && !fix("a:b", 64) && !fix("a*b", 64) &&
           !fix("a\"b", 64) && !fix("caf\xc3\xa9", 64));
    assert(!strcmp(fix("abcdef", 10), "abcdef.md"));                /* 9 and the NUL */
    assert(!fix("abcdefg", 10));
    char out[8] = "x";
    assert(fname_fix("abcdefgh", out, sizeof out) && out[0] == '\0');   /* refused: out emptied */
    assert(fname_is_note("a.md") && fname_is_note("A.TeX") && fname_is_note("b.txt"));
    assert(!fname_is_note("a.mdx") && !fname_is_note(".md") && !fname_is_note("md") && !fname_is_note(NULL));
    puts("fname: ok");
    return 0;
}

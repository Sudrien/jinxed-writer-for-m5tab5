/*
 * md_host_test.c -- md_line() against lines a writer types. Each case is
 * the line and a picture of its styles, one letter a byte:
 *   . plain  m marker  b bold  i italic  x bold+italic  s struck
 *   c code   h heading (with b/i/m on top shown as B/I/M)
 * SPDX-License-Identifier: MIT
 */
#include <stdio.h>
#include <string.h>

#include "md.h"

static int fails;

static char pic(uint8_t f)
{
    if (f & MD_CODE) return (f & MD_MARK) ? 'm' : 'c';
    if (f & MD_MARK) return (f & MD_HEAD) ? 'M' : 'm';
    if ((f & (MD_BOLD | MD_ITALIC)) == (MD_BOLD | MD_ITALIC)) return 'x';
    if (f & MD_BOLD) return (f & MD_HEAD) ? 'B' : 'b';
    if (f & MD_ITALIC) return (f & MD_HEAD) ? 'I' : 'i';
    if (f & MD_STRIKE) return 's';
    if (f & MD_HEAD) return 'h';
    return '.';
}

static void t(const char *line, const char *want)
{
    const size_t n = strlen(line);
    uint8_t st[256];
    char got[257];
    md_line(line, n, st);
    for (size_t i = 0; i < n; i++) got[i] = pic(st[i]);
    got[n] = '\0';
    if (strcmp(got, want)) {
        fails++;
        printf("FAIL  %s\n  want %s\n  got  %s\n", line, want, got);
    }
}

int main(void)
{
    t("plain text",                 "..........");
    t("**bold** x",                 "mmbbbbmm..");
    t("__bold__ x",                 "mmbbbbmm..");
    t("an *it* b",                  "...miim..");
    t("an _it_ b",                  "...miim..");
    t("***both***",                 "mmmxxxxmmm");
    t("~~gone~~ ok",                "mmssssmm...");
    t("~one~",                      ".....");
    t("snake_case_name",            "...............");
    t("2 * 3 * 4",                  ".........");
    t("a ** b",                     "......");
    t("**unclosed",                 "..........");
    t("`co*de*` *x*",               "mccccccm.mim");
    t("``a ` b``",                  "mmcccccmm");
    t("`open",                      ".....");
    t("\\*not\\*",                  "m....m.");
    t("# Title",                    "Mhhhhhh");
    t("### *Sub*",                  "MMMhMIIIM");
    t("#nope",                      ".....");
    t("####### seven",              ".............");
    t("\\# not",                    "m.....");
    t("**a *b* c**",                "mmbbmxmbbmm");
    t("*a **b** c*",                "miimmxmmiim");
    t("x**y**z",                    ".mmbmm.");
    t("x__y__z",                    ".......");
    t("\xc3\xa9 *\xc3\xbc* x",          "...miim..");   /* UTF-8 inside */
    /* 0032: what the editor widens. */
    static const struct { const char *s; bool head; } hs[] = {
        { "# T", true }, { "###### T", true }, { "   # T", true }, { "#", true },
        { "    # T", false }, { "####### T", false }, { "#T", false },
        { "\\# T", false }, { "", false }, { "plain", false },
    };
    for (size_t i = 0; i < sizeof hs / sizeof hs[0]; i++) {
        if (md_is_heading(hs[i].s, strlen(hs[i].s)) != hs[i].head) {
            fails++;
            printf("FAIL  md_is_heading(\"%s\") should be %d\n", hs[i].s, hs[i].head);
        }
    }
    printf("md: %s\n", fails ? "FAILED" : "all passed");
    return fails != 0;
}

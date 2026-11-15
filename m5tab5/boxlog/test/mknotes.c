/* SPDX-License-Identifier: MIT
 *
 * boxlog's host test (u-jinxed-0005): notes for the probe to find, made by
 * doclog itself in sd/jinxed -- a short one, a long one with an author,
 * one whose second twin is torn, one with no second twin -- and a file
 * that only looks like a twin's neighbour. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "doclog.h"

static char a[65536], b[65536];

static void note(const char *name, int edits, const char *author)
{
    uint32_t len = 0;
    if (doclog_open(name, a, &len, 1767229200, true) != ESP_OK) { printf("mknotes: open %s\n", name); exit(1); }
    for (int i = 0; i < edits; i++) {
        uint32_t n = len;
        memcpy(b, a, len);
        n += (uint32_t)snprintf(b + n, sizeof b - n, "line %d of %s, *some* `words`\n", i, name);
        if (doclog_append(a, len, b, n, 1767229200 + i, true, DOCLOG_SRC_USER, author) != ESP_OK) { printf("mknotes: append\n"); exit(1); }
        memcpy(a, b, n);
        len = n;
    }
    doclog_close();
}

int main(void)
{
    if (doclog_init(65536) != ESP_OK) return 1;
    note("2026-11-14.md", 3, NULL);
    note("2026-11-15.md", 200, "someone");
    note("torn.md", 20, NULL);
    note("single.md", 5, NULL);
    /* torn: h1 cut short mid-record; single: no h1 at all */
    if (truncate("sd/jinxed/.torn.md.h1", 64 + 30) != 0) return 1;
    if (unlink("sd/jinxed/.single.md.h1") != 0) return 1;
    FILE *f = fopen("sd/jinxed/.notes.txt", "w");
    if (!f) return 1;
    fputs("not a log\n", f);
    fclose(f);
    return 0;
}

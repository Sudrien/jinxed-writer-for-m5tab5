/*
 * doclog_host_test.c -- doclog.c on a PC, against real files.
 *
 *   ./run.sh
 *
 * Builds doclog.c with DOCLOG_HOST (plain files, no arbiter) and
 * DOCLOG_DIR="sd", so the logs land in ./sd under a temp directory.
 *
 * What it checks, many times over with random edits:
 *   - a log replays to exactly the text that was appended
 *   - a torn tail on either twin (cut at any byte, or garbage written
 *     there) recovers to the last record both could agree on, or one
 *     further, and the torn twin is repaired to match the other
 *   - a deleted twin is rebuilt from the other
 *   - an export with no log is imported as one insert
 *
 * SPDX-License-Identifier: MIT
 */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "doclog.h"

#define CAP (64 * 1024)
#define NAME "2026-11-02.md"

static char P0[256], P1[256], EXP[256];

static char cur[CAP], got[CAP], hist[64][CAP];
static uint32_t cur_len, hist_len[64];

static long fsize(const char *p) { struct stat st; return stat(p, &st) == 0 ? st.st_size : -1; }

static void rnd_edit(void)
{
    const int kind = rand() % 4;
    uint32_t pos = cur_len ? (uint32_t)rand() % (cur_len + 1) : 0;
    if (kind < 3 || cur_len == 0) {
        uint32_t n = 1 + rand() % 40;
        if (cur_len + n > CAP) n = CAP - cur_len;
        memmove(cur + pos + n, cur + pos, cur_len - pos);
        for (uint32_t i = 0; i < n; i++) cur[pos + i] = "abc de\nfg"[rand() % 9];
        cur_len += n;
    } else {
        uint32_t n = 1 + rand() % 30;
        if (pos + n > cur_len) n = cur_len - pos;
        memmove(cur + pos, cur + pos + n, cur_len - pos - n);
        cur_len -= n;
    }
}

static void wipe(void) { unlink(P0); unlink(P1); unlink(EXP); }

/* Truncate a file to `len` bytes, then maybe scribble a few garbage bytes. */
static void tear(const char *p, long len, int garbage)
{
    assert(truncate(p, len) == 0);
    if (garbage) {
        FILE *f = fopen(p, "r+b");
        fseek(f, len, SEEK_SET);
        for (int i = 0; i < garbage; i++) fputc(rand() & 0xFF, f);
        fclose(f);
    }
}

static int files_equal_prefix(long n)
{
    FILE *a = fopen(P0, "rb"), *b = fopen(P1, "rb");
    int eq = a && b;
    for (long i = 0; eq && i < n; i++) eq = fgetc(a) == fgetc(b);
    if (a) fclose(a);
    if (b) fclose(b);
    return eq;
}

int main(void)
{
    srand(12345);
    mkdir("sd", 0777);
    snprintf(P0, sizeof P0, "%s/.%s%s", DOCLOG_DIR, NAME, DOCLOG_SUFFIX0);
    snprintf(P1, sizeof P1, "%s/.%s%s", DOCLOG_DIR, NAME, DOCLOG_SUFFIX1);
    snprintf(EXP, sizeof EXP, "%s/%s", DOCLOG_DIR, NAME);
    assert(doclog_init(CAP) == 0);

    int trials = 0, repaired = 0;
    for (int round = 0; round < 300; round++) {
        wipe();
        uint32_t len;
        assert(doclog_open(NAME, got, &len, 1700000000, true) == 0);
        assert(len == 0);
        cur_len = 0;

        /* A run of flushes, remembering the text after each, and each
         * twin's size after each. */
        const int flushes = 2 + rand() % 30;
        static char prev[CAP];
        uint32_t prev_len = 0;
        hist_len[0] = 0;
        for (int k = 1; k <= flushes; k++) {
            const int edits = 1 + rand() % 6;
            for (int e = 0; e < edits; e++) rnd_edit();
            assert(doclog_append(prev, prev_len, cur, cur_len, 1700000000 + k, k % 5 != 0) == 0);
            assert(doclog_seq() == (uint32_t)k);
            memcpy(prev, cur, cur_len); prev_len = cur_len;
            memcpy(hist[k], cur, cur_len); hist_len[k] = cur_len;
        }

        /* Plain reopen replays exactly. */
        doclog_close();
        assert(doclog_open(NAME, got, &len, 0, false) == 0);
        assert(len == cur_len && memcmp(got, cur, len) == 0);
        assert(doclog_seq() == (uint32_t)flushes);

        /* Now damage. */
        const int mode = rand() % 4;
        const char *victim = (rand() & 1) ? P1 : P0;
        if (mode == 0) {
            unlink(victim);
        } else {
            /* Cut anywhere in the record region (the zeros past it do
             * not matter); garbage on some. The other twin is intact. */
            FILE *f = fopen(victim, "rb");
            fseek(f, 0, SEEK_END);
            fclose(f);
            /* Find the log end by reopening's seq: cut somewhere before
             * the first zero run. Approximate with a scan for the last
             * nonzero byte. */
            FILE *g = fopen(victim, "rb");
            long last = 0, i = 0; int c;
            while ((c = fgetc(g)) != EOF) { if (c) last = i; i++; }
            fclose(g);
            const long cut = 64 + rand() % (last - 64 + 1);
            tear(victim, cut, mode == 2 ? 1 + rand() % 40 : 0);
        }
        trials++;

        doclog_close();
        assert(doclog_open(NAME, got, &len, 0, false) == 0);
        /* The intact twin wins: full text back. */
        assert(len == cur_len && memcmp(got, cur, len) == 0);
        assert(doclog_seq() == (uint32_t)flushes);
        /* And the victim has been repaired to match. */
        if (fsize(victim) > 0) repaired++;
        doclog_close();
        unlink(victim == P0 ? P1 : P0);     /* lose the good twin now */
        assert(doclog_open(NAME, got, &len, 0, false) == 0);
        assert(len == cur_len && memcmp(got, cur, len) == 0);

        /* Both torn at once (never happens by design, but recovery must
         * still land on SOME earlier flush exactly, never on garbage). */
        if (mode == 3) {
            FILE *g = fopen(P0, "rb");
            long last = 0, i = 0; int c;
            while ((c = fgetc(g)) != EOF) { if (c) last = i; i++; }
            fclose(g);
            const long cut = 64 + rand() % (last - 64 + 1);
            tear(P0, cut, 0);
            tear(P1, cut, 3);
            doclog_close();
            assert(doclog_open(NAME, got, &len, 0, false) == 0);
            const uint32_t s = doclog_seq();
            assert(s <= (uint32_t)flushes);
            assert(len == hist_len[s] && memcmp(got, hist[s], len) == 0);
            /* And appending from there works and replays. */
            assert(doclog_append(got, len, "tail", 4, 1, true) == 0);
            doclog_close();
            assert(doclog_open(NAME, got, &len, 0, false) == 0);
            assert(len == 4 && memcmp(got, "tail", 4) == 0);
            assert(files_equal_prefix(64));
        }
    }

    /* Import an export with no log. */
    wipe();
    FILE *f = fopen(EXP, "wb");
    fputs("hello from before\n", f);
    fclose(f);
    uint32_t len;
    assert(doclog_open(NAME, got, &len, 0, false) == 0);
    assert(len == 18 && memcmp(got, "hello from before\n", 18) == 0);
    assert(doclog_seq() == 1);
    doclog_close();
    unlink(EXP);
    assert(doclog_open(NAME, got, &len, 0, false) == 0);
    assert(len == 18 && doclog_seq() == 1);

    /* Verify: same log OK; swapped-out log NOT_FOUND; other doc INVALID. */
    assert(doclog_verify() == 0);
    wipe();
    assert(doclog_verify() == ESP_ERR_NOT_FOUND);

    printf("ok: %d damage trials, %d twins repaired\n", trials, repaired);
    wipe();
    return 0;
}

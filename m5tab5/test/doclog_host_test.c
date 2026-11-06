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
#include <stdint.h>
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

/* 0037: an export with no author and no time, and the file it makes. */
static int ex(const char *t) { return doclog_export(t, (uint32_t)strlen(t), NULL, 0, false, "T"); }
static const char *full(const char *t)
{
    static char b[4][256];
    static int i;
    char *r = b[i++ % 4];
    snprintf(r, 256, "%s\n\n---\n\nJinxed Writer T\n", t);
    return r;
}
static void put_file(const char *p, const char *t) { FILE *f = fopen(p, "wb"); fputs(t, f); fclose(f); }
static int file_is(const char *p, const char *t)
{
    static char b[512];
    FILE *f = fopen(p, "rb");
    if (!f) return 0;
    const size_t n = fread(b, 1, sizeof b, f);
    fclose(f);
    return n == strlen(t) && !memcmp(b, t, n);
}

/* 0036: the sidecar, written as doclog.c writes it, to fake a cut. */
static uint32_t crc(const char *p, size_t n)
{
    uint32_t c = ~0u;
    while (n--) { c ^= (unsigned char)*p++; for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0x82F63B78u & (0u - (c & 1))); }
    return ~c;
}
static void put4(unsigned char *b, uint32_t v) { b[0] = v; b[1] = v >> 8; b[2] = v >> 16; b[3] = v >> 24; }
static void side_put(const char *path, uint32_t last, uint32_t pending)
{
    unsigned char b[16];
    put4(b, 0x3158474Au); put4(b + 4, last); put4(b + 8, pending);
    put4(b + 12, crc((const char *)b, 12));
    FILE *f = fopen(path, "wb"); fwrite(b, 1, 16, f); fclose(f);
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

/* Which source the k-th flush is made by, and a walk of a twin's raw
 * records checking each one's source byte came out as written. */
static doclog_src_t src_of(int k) { return k % 3 == 0 ? DOCLOG_SRC_SYMSPELL : DOCLOG_SRC_USER; }

/* 0035: who the k-th flush credits -- Ada first (id 1), Bob second (2),
 * the default otherwise; a spelling flush is never anyone's. */
static const char *author_of(int k) { return k % 4 == 1 ? "Ada" : k % 4 == 2 ? "Bob" : NULL; }
static unsigned id_of(int k)
{
    if (src_of(k) != DOCLOG_SRC_USER || !author_of(k)) return 0;
    return author_of(k)[0] == 'A' ? 1 : 2;
}

static void check_sources(const char *path, int flushes)
{
    FILE *f = fopen(path, "rb");
    assert(f);
    long off = 64;
    int names = 0;
    for (int k = 1; k <= flushes; k++) {
        unsigned char h[20];
        assert(fseek(f, off, SEEK_SET) == 0 && fread(h, 1, 20, f) == 20);
        const unsigned long n = h[0] | h[1] << 8 | h[2] << 16 | (unsigned long)h[3] << 24;
        off += 20 + (long)n + 4;
        if (h[4] == 2) {                /* an author record: Ada, then Bob */
            unsigned char a[2 + 3];
            assert(n == 5 && fseek(f, off - 4 - 5, SEEK_SET) == 0 && fread(a, 1, 5, f) == 5);
            assert(a[0] == ++names && a[1] == 3 && !memcmp(a + 2, names == 1 ? "Ada" : "Bob", 3));
            k--;
            continue;
        }
        assert(h[4] == 1 && h[6] == (unsigned char)src_of(k) && h[7] == id_of(k));
    }
    assert(names == (flushes >= 2 ? 2 : 1));
    fclose(f);
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
            assert(doclog_append(prev, prev_len, cur, cur_len, 1700000000 + k, k % 5 != 0,
                                 src_of(k), author_of(k)) == 0);
            assert(doclog_seq() == (uint32_t)k);
            memcpy(prev, cur, cur_len); prev_len = cur_len;
            memcpy(hist[k], cur, cur_len); hist_len[k] = cur_len;
        }

        /* Plain reopen replays exactly. */
        doclog_close();
        assert(doclog_open(NAME, got, &len, 0, false) == 0);
        assert(len == cur_len && memcmp(got, cur, len) == 0);
        assert(doclog_seq() == (uint32_t)flushes);
        check_sources(P0, flushes);
        check_sources(P1, flushes);
        assert(doclog_author_id("Ada") == 1 && doclog_author_id(NULL) == 0);
        assert(doclog_author_id("Bob") == (flushes >= 2 ? 2 : -1));

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
            assert(doclog_append(got, len, "tail", 4, 1, true, DOCLOG_SRC_USER, "Ada") == 0);
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

    /* 0036, 0037: export, with its footer. */
    {
        char CONF[256], SIDE[256], TMP[sizeof EXP + 4];
        snprintf(CONF, sizeof CONF, "%s/2026-11-02.conflict.md", DOCLOG_DIR);
        snprintf(SIDE, sizeof SIDE, "%s/.%s.x", DOCLOG_DIR, NAME);
        snprintf(TMP, sizeof TMP, "%s.tmp", EXP);
        wipe(); unlink(CONF); unlink(SIDE);
        assert(doclog_open(NAME, got, &len, 0, false) == 0 && len == 0);
        /* First export: the file appears, the footer after it, no temp. */
        assert(doclog_append("", 0, "one", 3, 1, true, DOCLOG_SRC_USER, NULL) == 0);
        assert(ex("one") == 0);
        assert(file_is(EXP, full("one")) && fsize(TMP) < 0 && fsize(SIDE) == 20);
        /* Again, over its own file. */
        assert(ex("one two") == 0 && file_is(EXP, full("one two")));
        /* Edited outside: left alone, ours goes beside it, every time. */
        put_file(EXP, "theirs");
        assert(ex("one two three") == ESP_ERR_INVALID_STATE);
        assert(file_is(EXP, "theirs") && file_is(CONF, full("one two three")));
        assert(ex("one two three!") == ESP_ERR_INVALID_STATE && file_is(EXP, "theirs"));
        /* They move theirs away: ours is written normally again. */
        unlink(EXP);
        assert(ex("four") == 0 && file_is(EXP, full("four")));
        /* A power cut mid-export, a 0036 sidecar saying last "four",
         * pending "five". Either one on the card is ours. */
        side_put(SIDE, crc(full("four"), strlen(full("four"))), crc(full("five"), strlen(full("five"))));
        put_file(EXP, full("five"));            /* renamed in, not recorded */
        assert(ex("six") == 0 && file_is(EXP, full("six")));
        side_put(SIDE, crc(full("four"), strlen(full("four"))), crc(full("five"), strlen(full("five"))));
        put_file(EXP, full("four"));            /* cut before the rename */
        assert(ex("six") == 0 && file_is(EXP, full("six")));
        side_put(SIDE, crc(full("four"), strlen(full("four"))), crc(full("five"), strlen(full("five"))));
        put_file(EXP, "fiv");                   /* neither: theirs */
        assert(ex("six") == ESP_ERR_INVALID_STATE && file_is(EXP, "fiv"));
        unlink(CONF);
        /* A missing or torn sidecar beside an existing file: theirs. */
        unlink(EXP);
        assert(ex("seven") == 0);
        assert(truncate(SIDE, 7) == 0);
        assert(ex("eight") == ESP_ERR_INVALID_STATE);
        /* ...unless the file already holds it: then it is just recorded. */
        assert(ex("seven") == 0 && fsize(SIDE) == 20);
        assert(ex("nine") == 0 && file_is(EXP, full("nine")));
        unlink(CONF);

        /* 0037: the author when set, and the time when trusted. */
        assert(doclog_export("ten", 3, "Ada", 1762437780, true, "v0.0.37") == 0);   /* 2025-11-06 14:03 UTC */
        assert(file_is(EXP, "ten\n\n---\n\nauthor: Ada  \nexported: 2025-11-06 14:03  \nJinxed Writer v0.0.37\n"));
        /* The same text later: not rewritten for the time alone. */
        assert(doclog_export("ten", 3, "Ada", 1762437780 + 3600, true, "v0.0.37") == 0);
        assert(file_is(EXP, "ten\n\n---\n\nauthor: Ada  \nexported: 2025-11-06 14:03  \nJinxed Writer v0.0.37\n"));
        /* A changed text is. */
        assert(doclog_export("ten!", 4, "Ada", 1762437780 + 3600, true, "v0.0.37") == 0);
        assert(file_is(EXP, "ten!\n\n---\n\nauthor: Ada  \nexported: 2025-11-06 15:03  \nJinxed Writer v0.0.37\n"));

        /* An import drops a footer of ours, and only ours: the text comes
         * back without it, and typing on it is no conflict. */
        static const struct { const char *file; const char *text; } imp[] = {
            { "imported\n\n\n---\n\nauthor: Ada  \nJinxed Writer v1\n", "imported\n" },
            { "\n\n---\n\nexported: 2025-11-06 14:03  \nJinxed Writer v1\n", "" },
            { "a\n\n---\n\nJinxed Writer v1\n", "a" },
            { "a\n\n---\n\nauthor: Ada\nJinxed Writer v1\n", NULL },     /* no hard break */
            { "a\n\n---\n\nJinxed Writer v1", NULL },                     /* no last newline */
            { "a\n\n---\n\nnotes: x  \nJinxed Writer v1\n", NULL },     /* not our key */
            { "imported\n", NULL },
        };
        for (size_t i = 0; i < sizeof imp / sizeof imp[0]; i++) {
            wipe(); unlink(CONF); unlink(SIDE);
            put_file(EXP, imp[i].file);
            doclog_close();
            const char *want = imp[i].text ? imp[i].text : imp[i].file;
            assert(doclog_open(NAME, got, &len, 0, false) == 0);
            assert(len == strlen(want) && !memcmp(got, want, len));
            char more[128];
            snprintf(more, sizeof more, "%smore", want);
            assert(ex(more) == 0 && file_is(EXP, full(more)) && fsize(CONF) < 0);
        }
        unlink(CONF); unlink(SIDE);
        /* 0041: the log, whole, for an email; and too big for its cap. */
        {
            static char lb[1 << 16], fb[1 << 16];
            bool big = true;
            assert(doclog_append(got, len, "copied", 6, 1, true, DOCLOG_SRC_USER, NULL) == 0);
            const size_t n = doclog_copy_log(lb, sizeof lb, &big);
            FILE *h = fopen(P0, "rb");
            const size_t m = fread(fb, 1, sizeof fb, h);
            fclose(h);
            assert(!big && n > 64 && n == m && !memcmp(lb, fb, n));
            assert(doclog_copy_log(lb, n - 1, &big) == 0 && big);
            doclog_close();
            assert(doclog_copy_log(lb, sizeof lb, &big) == 0 && !big);
        }
        puts("export: ok");
    }

    printf("ok: %d damage trials, %d twins repaired\n", trials, repaired);
    wipe();
    return 0;
}

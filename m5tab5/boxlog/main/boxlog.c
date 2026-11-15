/*
 * boxlog.c -- the probe (u-jinxed-0005): see boxlog.h.
 *
 * The same source in the .bin (natively), in the ELF (boxed) and in
 * test/ (on a PC, BOXLOG_HOST). Nothing here may depend on which: the
 * result file is compared byte for byte.
 *
 * SPDX-License-Identifier: MIT
 */
#include "boxlog.h"

#include <dirent.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "doclog.h"
#include "md.h"

#ifdef BOXLOG_HOST
#include <time.h>
#define LOG(...) do { printf("boxlog: " __VA_ARGS__); putchar('\n'); } while (0)
#define BIG(n)   malloc(n)
static int64_t now_us(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (int64_t)t.tv_sec * 1000000 + t.tv_nsec / 1000;
}
#else
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
static const char *TAG = "boxlog";
#define LOG(...) ESP_LOGI(TAG, __VA_ARGS__)
#define BIG(n)   heap_caps_malloc((n), MALLOC_CAP_SPIRAM)
static int64_t now_us(void) { return esp_timer_get_time(); }
#endif

#ifdef BOXLOG_ELF
#define WHERE "box"
#else
#define WHERE "host"
#endif

#define CAP          (256u * 1024u)     /* the longest text, as the editor's own */
#define SYNTH_EDITS  400u               /* each an append: two writes, two fsyncs */
#define LARGE_PASTES 50u                /* u-jinxed-0009: the large note, in pastes of */
#define LARGE_PASTE  4096u              /* this many bytes: 200 KB */
#define NOTES_MAX    32
#define COPY_CHUNK   (32u * 1024u)
#define TIME0        1767229200LL       /* 2026-01-01 01:00 UTC: every 'now' from here */

static char *s_a, *s_b, *s_c;           /* the text, the next text, the replay */
static char s_names[NOTES_MAX][256];

static uint32_t fnv(uint32_t h, const void *p, size_t n)
{
    const uint8_t *b = p;
    for (size_t i = 0; i < n; i++) h = (h ^ b[i]) * 16777619u;
    return h;
}
#define FNV0 2166136261u

static uint32_t s_rng;
static uint32_t rnd(void)
{
    s_rng = s_rng * 1103515245u + 12345u;
    return s_rng >> 8;
}

static unsigned ms_since(int64_t t0)
{
    return (unsigned)((now_us() - t0 + 500) / 1000);
}

/* dir "/" pre name suf into out, or false if it would not fit. Lengths
 * checked here rather than by snprintf, whose truncation warning cannot
 * see that a name from s_names is under 256 bytes. */
static bool join(char *out, size_t cap, const char *dir, const char *pre, const char *name, const char *suf)
{
    const size_t a = strlen(dir), p = strlen(pre), b = strlen(name), c = strlen(suf);
    if (a + 1 + p + b + c + 1 > cap) return false;
    memcpy(out, dir, a);
    out[a] = '/';
    memcpy(out + a + 1, pre, p);
    memcpy(out + a + 1 + p, name, b);
    memcpy(out + a + 1 + p + b, suf, c + 1);
    return true;
}

/* Every file in DOCLOG_DIR removed, so each run starts from nothing. */
static void clean_dir(void)
{
    DIR *d = opendir(DOCLOG_DIR);
    if (!d) return;
    struct dirent *e;
    char p[300];
    while ((e = readdir(d)) != NULL) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        if (join(p, sizeof p, DOCLOG_DIR, "", e->d_name, "")) unlink(p);
    }
    closedir(d);
}

/* The file's FNV hash and length; false if it will not open. */
static bool hash_file(const char *path, uint32_t *h, uint32_t *len)
{
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    *h = FNV0;
    *len = 0;
    size_t n;
    while ((n = fread(s_c, 1, COPY_CHUNK, f)) > 0) {
        *h = fnv(*h, s_c, n);
        *len += (uint32_t)n;
    }
    fclose(f);
    return true;
}

/* 1: the synthetic note. */
static void synthetic(FILE *out)
{
    uint32_t len = 0, seq = 0;
    if (doclog_open("synthetic.md", s_a, &len, TIME0, true) != ESP_OK || len != 0) {
        fprintf(out, "synthetic open failed\n");
        return;
    }
    s_rng = 0x6a696e78u;                            /* "jinx" */
    static const char alpha[] = "abcdefghijklmnopqrstuvwxyz      **__`#~\n.,";
    int64_t t_append = 0, worst = 0;
    unsigned failed = 0;
    for (uint32_t i = 0; i < SYNTH_EDITS; i++) {
        uint32_t nlen;
        const uint32_t pos = len ? rnd() % (len + 1) : 0;
        if (len == 0 || rnd() % 10 < 7) {
            const uint32_t k = 1 + rnd() % 24;
            if (len + k >= CAP) continue;
            memcpy(s_b, s_a, pos);
            for (uint32_t j = 0; j < k; j++) s_b[pos + j] = alpha[rnd() % (sizeof alpha - 1)];
            memcpy(s_b + pos + k, s_a + pos, len - pos);
            nlen = len + k;
        } else {
            uint32_t k = 1 + rnd() % 12;
            if (k > len - pos) k = len - pos;
            memcpy(s_b, s_a, pos);
            memcpy(s_b + pos, s_a + pos + k, len - pos - k);
            nlen = len - k;
        }
        const doclog_src_t src = (i % 40 == 39) ? DOCLOG_SRC_SYMSPELL : DOCLOG_SRC_USER;
        const char *author = i < SYNTH_EDITS / 2 ? NULL : "boxlog";
        const int64_t t0 = now_us();
        const esp_err_t e = doclog_append(s_a, len, s_b, nlen, TIME0 + i, true, src, author);
        const int64_t dt = now_us() - t0;
        t_append += dt;
        if (dt > worst) worst = dt;
        if (e != ESP_OK) {
            failed++;
            continue;                               /* s_a is still what the log has */
        }
        char *t = s_a;
        s_a = s_b;
        s_b = t;
        len = nlen;
    }
    seq = doclog_seq();
    doclog_close();
    LOG(WHERE ": %u appends in %u ms, %u us each, worst %u us", (unsigned)SYNTH_EDITS,
        (unsigned)(t_append / 1000), (unsigned)(t_append / SYNTH_EDITS), (unsigned)worst);

    /* Replayed from the card. */
    uint32_t rlen = 0;
    int64_t t0 = now_us();
    const esp_err_t e = doclog_open("synthetic.md", s_c, &rlen, TIME0, true);
    LOG(WHERE ": synthetic replayed in %u ms, %u bytes", ms_since(t0), (unsigned)rlen);
    const bool same = e == ESP_OK && rlen == len && memcmp(s_c, s_a, len) == 0 && doclog_seq() == seq;

    /* Exported, and the export read back. */
    uint32_t xh = 0, xlen = 0;
    const esp_err_t xe = doclog_export(s_a, len, "boxlog", TIME0 + SYNTH_EDITS, true, "boxlog");
    const bool xok = xe == ESP_OK && hash_file(DOCLOG_DIR "/synthetic.md", &xh, &xlen);
    doclog_close();

    /* Its markdown, a line at a time, into s_b. */
    t0 = now_us();
    uint32_t mh = FNV0, lines = 0;
    for (uint32_t i = 0; i < len;) {
        uint32_t j = i;
        while (j < len && s_a[j] != '\n') j++;
        md_line(s_a + i, j - i, (uint8_t *)s_b);
        mh = fnv(mh, s_b, j - i);
        mh = fnv(mh, md_is_heading(s_a + i, j - i) ? "H" : "-", 1);
        lines++;
        i = j + 1;
    }
    LOG(WHERE ": markdown of %u lines in %u ms", (unsigned)lines, ms_since(t0));

    fprintf(out, "synthetic edits %u failed %u seq %u len %u text %08x replay %s export %s %u %08x md %u %08x\n",
            (unsigned)SYNTH_EDITS, failed, (unsigned)seq, (unsigned)len, (unsigned)fnv(FNV0, s_a, len),
            same ? "same" : "DIFFERS", xok ? "ok" : "FAILED", (unsigned)xlen, (unsigned)xh,
            (unsigned)lines, (unsigned)mh);
}

/* u-jinxed-0009: one generated line of markdown into p, at most room
 * bytes with its newline; its length. Headings, emphasis, code, lists,
 * plain words: what md.c styles. */
static uint32_t gen_line(char *p, uint32_t room)
{
    static const char *const words[] = {
        "the", "log", "*emphasis*", "**strong**", "`code`", "~~gone~~", "words",
        "box", "_under_", "keystroke", "twin", "replay", "[a link](x)", "and",
    };
    static const char *const starts[] = { "", "", "", "# ", "## ", "- ", "> ", "1. " };
    uint32_t n = 0;
    const char *st = starts[rnd() % 8];
    const uint32_t ls = (uint32_t)strlen(st);
    if (ls + 1 >= room) return 0;
    memcpy(p, st, ls);
    n = ls;
    const uint32_t nw = 3 + rnd() % 12;
    for (uint32_t i = 0; i < nw; i++) {
        const char *w = words[rnd() % (sizeof words / sizeof *words)];
        const uint32_t lw = (uint32_t)strlen(w);
        if (n + lw + 2 >= room) break;
        if (n > ls) p[n++] = ' ';
        memcpy(p + n, w, lw);
        n += lw;
    }
    p[n++] = '\n';
    return n;
}

/* u-jinxed-0009: a large note, LARGE_PASTES pastes of LARGE_PASTE bytes
 * of generated markdown, each one append; replayed; and its markdown
 * styled whole. Styling makes no host call, so in a box its time is one
 * window, the longest the probe makes; replaying 200 KB is the other
 * figure the 0008 run, with a 2 KB note, could not give. */
static void large(FILE *out)
{
    uint32_t len = 0;
    if (doclog_open("large.md", s_a, &len, TIME0, true) != ESP_OK || len != 0) {
        fprintf(out, "large open failed\n");
        return;
    }
    s_rng = 0x6c617267u;                            /* "larg" */
    int64_t t_append = 0, worst = 0;
    unsigned failed = 0;
    for (uint32_t i = 0; i < LARGE_PASTES; i++) {
        memcpy(s_b, s_a, len);
        uint32_t k = 0;
        while (k < LARGE_PASTE) {
            const uint32_t g = gen_line(s_b + len + k, LARGE_PASTE - k);
            if (g == 0) break;
            k += g;
        }
        while (k < LARGE_PASTE) s_b[len + k++] = '\n';  /* exactly a paste's length */
        const int64_t t0 = now_us();
        const esp_err_t e = doclog_append(s_a, len, s_b, len + k, TIME0 + i, true, DOCLOG_SRC_USER, NULL);
        const int64_t dt = now_us() - t0;
        t_append += dt;
        if (dt > worst) worst = dt;
        if (e != ESP_OK) {
            failed++;
            continue;
        }
        char *t = s_a;
        s_a = s_b;
        s_b = t;
        len += k;
    }
    const uint32_t seq = doclog_seq();
    doclog_close();
    LOG(WHERE ": large: %u pastes of %u bytes in %u ms, worst %u ms", (unsigned)LARGE_PASTES,
        (unsigned)LARGE_PASTE, (unsigned)(t_append / 1000), (unsigned)(worst / 1000));

    uint32_t rlen = 0;
    int64_t t0 = now_us();
    const esp_err_t e = doclog_open("large.md", s_c, &rlen, TIME0, true);
    LOG(WHERE ": large: replayed in %u ms, %u bytes", ms_since(t0), (unsigned)rlen);
    const bool same = e == ESP_OK && rlen == len && memcmp(s_c, s_a, len) == 0 && doclog_seq() == seq;
    doclog_close();

    t0 = now_us();
    uint32_t mh = FNV0, lines = 0;
    for (uint32_t i = 0; i < len;) {
        uint32_t j = i;
        while (j < len && s_a[j] != '\n') j++;
        md_line(s_a + i, j - i, (uint8_t *)s_b);
        mh = fnv(mh, s_b, j - i);
        lines++;
        i = j + 1;
    }
    LOG(WHERE ": large: markdown of %u lines in %u ms, one window", (unsigned)lines, ms_since(t0));

    fprintf(out, "large pastes %u failed %u seq %u len %u text %08x replay %s md %u %08x\n",
            (unsigned)LARGE_PASTES, failed, (unsigned)seq, (unsigned)len,
            (unsigned)fnv(FNV0, s_a, len), same ? "same" : "DIFFERS", (unsigned)lines, (unsigned)mh);
}

/* src to dst, whole; false if src will not open or a write fails. */
static bool copy_file(const char *src, const char *dst)
{
    FILE *in = fopen(src, "rb");
    if (!in) return false;
    FILE *o = fopen(dst, "wb");
    bool ok = o != NULL;
    size_t n;
    while (ok && (n = fread(s_c, 1, COPY_CHUNK, in)) > 0) ok = fwrite(s_c, 1, n, o) == n;
    fclose(in);
    if (o && fclose(o) != 0) ok = false;
    return ok;
}

/* The notes in BOXLOG_SRC with a log (".<name>.h0"), sorted, so both
 * runs take them in one order whatever readdir gives. */
static int name_cmp(const void *a, const void *b) { return strcmp(a, b); }
static int list_notes(void)
{
    DIR *d = opendir(BOXLOG_SRC);
    if (!d) return 0;
    int n = 0;
    struct dirent *e;
    while ((e = readdir(d)) != NULL && n < NOTES_MAX) {
        const char *s = e->d_name;
        const size_t l = strlen(s);
        if (l < 5 || s[0] != '.' || strcmp(s + l - 3, ".h0") != 0 || l - 4 >= sizeof s_names[0]) continue;
        memcpy(s_names[n], s + 1, l - 4);
        s_names[n][l - 4] = '\0';
        n++;
    }
    closedir(d);
    qsort(s_names, (size_t)n, sizeof s_names[0], name_cmp);
    return n;
}

/* 2: the writer's own notes, copied and opened. */
static void notes(FILE *out)
{
    const int n = list_notes();
    char src[300], dst[300];
    for (int i = 0; i < n; i++) {
        const char *name = s_names[i];
        bool ok = true;
        for (int t = 0; t < 2; t++) {
            const char *suf = t ? DOCLOG_SUFFIX1 : DOCLOG_SUFFIX0;
            struct stat st;
            if (!join(src, sizeof src, BOXLOG_SRC, ".", name, suf) ||
                !join(dst, sizeof dst, DOCLOG_DIR, ".", name, suf)) {
                ok = false;
            } else if (stat(src, &st) == 0 && !copy_file(src, dst)) {
                ok = false;
            }
        }
        if (!ok) {
            fprintf(out, "note %s copy failed\n", name);
            continue;
        }
        uint32_t len = 0;
        const int64_t t0 = now_us();
        const esp_err_t e = doclog_open(name, s_a, &len, TIME0, true);
        const unsigned ms = ms_since(t0);
        if (e != ESP_OK) {
            fprintf(out, "note %s open error %d\n", name, (int)e);
        } else {
            fprintf(out, "note %s seq %u len %u text %08x\n", name, (unsigned)doclog_seq(),
                    (unsigned)len, (unsigned)fnv(FNV0, s_a, len));
            LOG(WHERE ": %s: seq %u, %u bytes, opened in %u ms", name, (unsigned)doclog_seq(),
                (unsigned)len, ms);
        }
        doclog_close();
    }
    fprintf(out, "notes %d\n", n);
}

int boxlog_run(const char *result)
{
    static bool inited;
    if (!inited) {
        s_a = BIG(CAP);
        s_b = BIG(CAP);
        s_c = BIG(CAP);
        if (!s_a || !s_b || !s_c || doclog_init(CAP) != ESP_OK) {
            LOG(WHERE ": no memory");
            return -1;
        }
        inited = true;
    }
    if ((mkdir(BOXLOG_ROOT, 0777) != 0 && errno != EEXIST) ||
        (mkdir(DOCLOG_DIR, 0777) != 0 && errno != EEXIST)) {
        LOG(WHERE ": no directory %s (errno %d)", DOCLOG_DIR, errno);
        return -1;
    }
    clean_dir();
    FILE *out = fopen(result, "w");
    if (!out) {
        LOG(WHERE ": no result file %s (errno %d)", result, errno);
        return -1;
    }
    const int64_t t0 = now_us();
    synthetic(out);
    large(out);
    notes(out);
    fclose(out);
    LOG(WHERE ": done in %u ms, %s", ms_since(t0), result);
    return 0;
}

#ifdef BOXLOG_ELF
#include "feckless_abi.h"
#include "feckless_abi_storage.h"
#include "feckless_box.h"

/* The boxed app: the probe, its result beside the host's. */
void app_main(void)
{
    feckless_abi_require();
    feckless_abi_require_storage();
    feckless_abi_require_box();
    boxlog_run(BOXLOG_BOX_RESULT);
}
#endif

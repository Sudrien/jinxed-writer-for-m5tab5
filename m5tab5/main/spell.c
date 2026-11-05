/*
 * spell.c -- spelling, checked on demand. See spell.h.
 *
 * Written for this board rather than linking symspell-c99, whose
 * approach it follows: that library callocs two 128 MB arenas when a
 * dictionary is created, and at distance 2 holds about 45 MB resident.
 * The Tab5 has 32 MB of PSRAM. Here the index is built once on a
 * computer and stays on the card.
 *
 * The file, little-endian, every offset from the start:
 *
 *   header     64 bytes, below
 *   words      the words, a-z, each NUL-terminated, in strcmp order;
 *              a word's id is its place in this list
 *   freqs      u32 per word
 *   dir        65537 u32: entries [dir[k], dir[k+1]) have hash >> 16 == k
 *   entries    n_keys + 1 of { u32 hash, u32 first posting }, by hash;
 *              the last is a sentinel whose first posting is n_post
 *   postings   u32 word ids; entry i's are [first(i), first(i + 1))
 *
 * A key is the FNV-1a hash of a delete. Two deletes with one hash share
 * a key, which only adds candidates, and every candidate is measured
 * against the misspelling before it is offered, so a collision costs a
 * comparison and never a wrong answer.
 *
 * words, freqs and dir are read into PSRAM by spell_load(). A lookup
 * reads, for each of a misspelling's deletes (29 at most), one bucket of
 * entries and one run of postings.
 *
 * SPDX-License-Identifier: MIT
 */
#include "spell.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef SPELL_HOST
#define ESP_LOGI(t, ...) do { (void)(t); } while (0)
#define ESP_LOGW(t, ...) do { fprintf(stderr, "W %s: ", t); fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); } while (0)
#define VOL_HOLD()      do { } while (0)
#define VOL_RELEASE()   do { } while (0)
#define VOL_PRESENT()   (true)
#define BIG_ALLOC(n)    malloc(n)
#define BIG_FREE(p)     free(p)
unsigned spell_reads;
#define COUNT_READ()    (spell_reads++)
#else
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "storage.h"
#define VOL_HOLD()      storage_hold_brief(STORAGE_SD)
#define VOL_RELEASE()   storage_release_brief(STORAGE_SD)
#define VOL_PRESENT()   storage_present(STORAGE_SD)
#define BIG_ALLOC(n)    heap_caps_malloc((n), MALLOC_CAP_SPIRAM)
#define BIG_FREE(p)     heap_caps_free(p)
#define COUNT_READ()    do { } while (0)
#endif

static const char *TAG = "spell";

#define MAGIC           "JINXSYM"   /* 7 + NUL = 8 bytes */
#define VERSION         (1)
#define HDR_LEN         (64)
#define DIR_N           (65536)
#define DEL_MAX         (32)        /* 1 + 7 + 21 at prefix 7, distance 2 */
#define USER_MAX        (1024)
#define BUCKET_CHUNK    (64)        /* entries read at a time */
#define POST_CHUNK      (256)       /* postings read at a time */
#define PATH_LEN        (sizeof(SPELL_DIR) + 16)

/* The header, as read. */
typedef struct {
    uint32_t build_id;      /* hash of the words: which build this is */
    uint32_t n_words, n_keys, n_post;
    uint32_t words_off, words_len, freq_off, dir_off, ent_off, post_off, file_len;
} hdr_t;

static bool      s_loaded;
static hdr_t     s_hdr;
static char     *s_words;       /* words_len bytes */
static uint32_t *s_woff;        /* n_words: where each word starts */
static uint32_t *s_freq;        /* n_words */
static uint32_t *s_dir;         /* DIR_N + 1 */
static uint8_t  *s_seen;        /* n_words: lookup stamps */
static uint8_t   s_stamp;

/* The user's words and the ignored ones, lower case. */
static spell_word_t *s_user;
static int       s_n_user;

/* ---- small things ------------------------------------------------------ */

static uint32_t get32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static uint32_t fnv(const char *s, size_t n)
{
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < n; i++) {
        h ^= (uint8_t)s[i];
        h *= 16777619u;
    }
    return h;
}

static char lower(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c; }
static bool is_upper(char c) { return c >= 'A' && c <= 'Z'; }
static bool is_alpha(char c) { return (c >= 'a' && c <= 'z') || is_upper(c); }

/* w[0, n) lower-cased into out, which holds SPELL_WORD_MAX + 1. */
static bool lower_into(const char *w, size_t n, char *out)
{
    if (n == 0 || n > SPELL_WORD_MAX) return false;
    for (size_t i = 0; i < n; i++) out[i] = lower(w[i]);
    out[n] = '\0';
    return true;
}

/*
 * The deletes of w's first SPELL_PREFIX letters, distance 0 to `maxd`
 * (at most 2), each distinct, into del. The count. The same function
 * builds the index and searches it, so the two cannot disagree.
 */
static int deletes(const char *w, size_t n, int maxd, char del[DEL_MAX][SPELL_PREFIX + 1])
{
    if (n > SPELL_PREFIX) n = SPELL_PREFIX;
    int nd = 0;
    memcpy(del[nd], w, n);
    del[nd][n] = '\0';
    nd++;
    int from = 0;
    for (int d = 1; d <= maxd; d++) {
        const int to = nd;
        for (int k = from; k < to; k++) {
            const size_t len = strlen(del[k]);
            if (len <= 1) continue;     /* nothing left worth indexing */
            for (size_t i = 0; i < len; i++) {
                char t[SPELL_PREFIX + 1];
                memcpy(t, del[k], i);
                memcpy(t + i, del[k] + i + 1, len - i);    /* and the NUL */
                bool dup = false;
                for (int j = 0; j < nd && !dup; j++) dup = strcmp(del[j], t) == 0;
                if (!dup && nd < DEL_MAX) strcpy(del[nd++], t);
            }
        }
        from = to;
    }
    return nd;
}

/* Optimal string alignment distance between a and b, or max + 1 when it
 * is over max. */
static int osa(const char *a, int la, const char *b, int lb, int max)
{
    if (la - lb > max || lb - la > max) return max + 1;
    int r0[SPELL_WORD_MAX + 2], r1[SPELL_WORD_MAX + 2], r2[SPELL_WORD_MAX + 2];
    int *pp = r0, *p = r1, *c = r2;     /* two rows back, one back, this */
    for (int j = 0; j <= lb; j++) p[j] = j;
    for (int i = 1; i <= la; i++) {
        c[0] = i;
        int best = c[0];
        for (int j = 1; j <= lb; j++) {
            const int cost = a[i - 1] != b[j - 1];
            int v = p[j - 1] + cost;
            if (p[j] + 1 < v)     v = p[j] + 1;
            if (c[j - 1] + 1 < v) v = c[j - 1] + 1;
            if (i > 1 && j > 1 && a[i - 1] == b[j - 2] && a[i - 2] == b[j - 1] &&
                pp[j - 2] + 1 < v) {
                v = pp[j - 2] + 1;
            }
            c[j] = v;
            if (v < best) best = v;
        }
        if (best > max) return max + 1;
        int *t = pp; pp = p; p = c; c = t;
    }
    return p[lb] > max ? max + 1 : p[lb];
}

/* ---- the word list ----------------------------------------------------- */

static const char *word(uint32_t id) { return s_words + s_woff[id]; }

static bool in_dict(const char *lw)
{
    if (!s_loaded) return false;
    uint32_t lo = 0, hi = s_hdr.n_words;
    while (lo < hi) {
        const uint32_t mid = lo + (hi - lo) / 2;
        const int c = strcmp(word(mid), lw);
        if (c == 0) return true;
        if (c < 0) lo = mid + 1; else hi = mid;
    }
    return false;
}

static bool in_user(const char *lw)
{
    for (int i = 0; i < s_n_user; i++) {
        if (strcmp(s_user[i], lw) == 0) return true;
    }
    return false;
}

static void user_put(const char *lw)
{
    if (!s_user) {
        s_user = BIG_ALLOC(USER_MAX * sizeof(spell_word_t));
        if (!s_user) return;
    }
    if (s_n_user >= USER_MAX || in_user(lw)) return;
    strcpy(s_user[s_n_user++], lw);
}

static bool known_lower(const char *lw)
{
    return lw[1] == '\0' || in_dict(lw) || in_user(lw);
}

/* ---- loading ----------------------------------------------------------- */

static void unload(void)
{
    BIG_FREE(s_words); BIG_FREE(s_woff); BIG_FREE(s_freq);
    BIG_FREE(s_dir);   BIG_FREE(s_seen);
    s_words = NULL; s_woff = NULL; s_freq = NULL; s_dir = NULL; s_seen = NULL;
    s_loaded = false;
}

static bool read_hdr(FILE *f, hdr_t *h)
{
    uint8_t b[HDR_LEN];
    COUNT_READ();
    if (fseek(f, 0, SEEK_SET) != 0 || fread(b, 1, HDR_LEN, f) != HDR_LEN) return false;
    if (memcmp(b, MAGIC, 8) != 0 || get32(b + 8) != VERSION) return false;
    if (b[28] != SPELL_MAX_DIST || b[29] != SPELL_PREFIX) return false;
    if (get32(b + 60) != fnv((const char *)b, 60)) return false;
    h->build_id  = get32(b + 12);
    h->n_words   = get32(b + 16);
    h->n_keys    = get32(b + 20);
    h->n_post    = get32(b + 24);
    h->words_off = get32(b + 32);
    h->words_len = get32(b + 36);
    h->freq_off  = get32(b + 40);
    h->dir_off   = get32(b + 44);
    h->ent_off   = get32(b + 48);
    h->post_off  = get32(b + 52);
    h->file_len  = get32(b + 56);
    /* The sections in order, each the size its count says. */
    return h->n_words > 0 && h->words_off == HDR_LEN &&
           h->freq_off  == h->words_off + h->words_len &&
           h->dir_off   == h->freq_off + 4 * h->n_words &&
           h->ent_off   == h->dir_off + 4 * (DIR_N + 1) &&
           h->post_off  == h->ent_off + 8 * (h->n_keys + 1) &&
           h->file_len  == h->post_off + 4 * h->n_post;
}

/* A whole section into a fresh buffer. */
static void *read_section(FILE *f, uint32_t off, uint32_t len)
{
    uint8_t *p = BIG_ALLOC(len ? len : 1);
    if (!p) return NULL;
    COUNT_READ();
    if (fseek(f, (long)off, SEEK_SET) != 0 || fread(p, 1, len, f) != len) {
        BIG_FREE(p);
        return NULL;
    }
    return p;
}

static void to_u32(uint32_t *v, uint32_t n)
{
    for (uint32_t i = 0; i < n; i++) v[i] = get32((const uint8_t *)&v[i]);
}

static void load_user(void)
{
    char path[PATH_LEN + 16];
    snprintf(path, sizeof(path), "%s/%s", SPELL_DIR, SPELL_USER);
    FILE *f = fopen(path, "r");
    if (!f) return;
    char line[128];
    while (fgets(line, sizeof(line), f)) {
        size_t n = strcspn(line, "\r\n \t");
        spell_word_t lw;
        bool ok = lower_into(line, n, lw);
        for (size_t i = 0; ok && i < n; i++) ok = is_alpha(lw[i]) || lw[i] == '\'';
        if (ok) user_put(lw);
    }
    fclose(f);
}

static bool load_file(FILE *f)
{
    hdr_t h;
    if (!read_hdr(f, &h)) return false;
    s_hdr = h;
    s_words = read_section(f, h.words_off, h.words_len);
    s_freq  = read_section(f, h.freq_off, 4 * h.n_words);
    s_dir   = read_section(f, h.dir_off, 4 * (DIR_N + 1));
    s_woff  = BIG_ALLOC(4 * h.n_words);
    s_seen  = BIG_ALLOC(h.n_words);
    if (!s_words || !s_freq || !s_dir || !s_woff || !s_seen) return false;
    to_u32(s_freq, h.n_words);
    to_u32(s_dir, DIR_N + 1);
    if (s_words[h.words_len - 1] != '\0' || s_dir[DIR_N] != h.n_keys) return false;

    /* Where each word starts; there must be exactly n_words of them. */
    uint32_t id = 0, at = 0;
    for (uint32_t i = 0; i < h.words_len; i++) {
        if (s_words[i] != '\0') continue;
        if (id == h.n_words) return false;
        s_woff[id++] = at;
        at = i + 1;
    }
    if (id != h.n_words) return false;
    for (uint32_t k = 0; k < DIR_N; k++) {
        if (s_dir[k] > s_dir[k + 1]) return false;
    }
    memset(s_seen, 0, h.n_words);
    s_stamp = 0;
    return true;
}

bool spell_load(void)
{
    if (s_loaded) return true;
    char path[PATH_LEN + 16];
    snprintf(path, sizeof(path), "%s/%s", SPELL_DIR, SPELL_DICT);

    VOL_HOLD();
    bool ok = false;
    if (VOL_PRESENT()) {
        FILE *f = fopen(path, "rb");
        if (f) {
            ok = load_file(f);
            fclose(f);
            if (ok) load_user();
        }
    }
    VOL_RELEASE();

    if (!ok) {
        unload();
        ESP_LOGW(TAG, "no dictionary at %s", path);
        return false;
    }
    s_loaded = true;
    ESP_LOGI(TAG, "%lu words, %lu keys, %d of the user's",
             (unsigned long)s_hdr.n_words, (unsigned long)s_hdr.n_keys, s_n_user);
    return true;
}

bool spell_loaded(void) { return s_loaded; }

/* ---- checking ---------------------------------------------------------- */

static bool not_prose(char c)
{
    return (c >= '0' && c <= '9') || c == '/' || c == '\\' || c == '@' || c == '_' ||
           c == '`' || c == '<' || c == '>' || c == '=';
}

static bool space(char c) { return c == ' ' || c == '\n' || c == '\t'; }

/* Whether text[k] is part of a word: a letter, or an apostrophe
 * between two. */
static bool in_word(const char *text, uint32_t len, uint32_t k)
{
    return is_alpha(text[k]) ||
           (text[k] == '\'' && k > 0 && k + 1 < len && is_alpha(text[k - 1]) && is_alpha(text[k + 1]));
}

bool spell_next_word(const char *text, uint32_t len, uint32_t from,
                     uint32_t *a, uint32_t *b)
{
    uint32_t i = from;
    /* Starting inside a word means starting at its beginning; starting
     * just after one does not. */
    if (i < len && in_word(text, len, i)) {
        while (i > 0 && in_word(text, len, i - 1)) i--;
    }
    while (i < len) {
        if (!is_alpha(text[i])) { i++; continue; }
        uint32_t s = i, e = i;
        while (e < len && in_word(text, len, e)) e++;
        i = e;

        /* The run of non-space around it. */
        uint32_t cs = s, ce = e;
        while (cs > 0 && !space(text[cs - 1])) cs--;
        while (ce < len && !space(text[ce])) ce++;
        bool skip = false;
        for (uint32_t k = cs; k < ce && !skip; k++) skip = not_prose(text[k]);

        const uint32_t n = e - s;
        bool inner_cap = false;
        for (uint32_t k = s + 1; k < e; k++) inner_cap |= is_upper(text[k]);
        if (n < 2 || n > SPELL_WORD_MAX) skip = true;
        if (inner_cap) skip = true;         /* WORDS, iPhone, camelCase */
        if (!skip) { *a = s; *b = e; return true; }
    }
    return false;
}

/* The stem a contraction is judged by: "don" of "don't", "is" of
 * "isn't", "we" of "we'll". Lower-cased word in, its stem written over
 * it; false when the ending is not one English uses. */
static bool stem(char *lw, bool *also_without_n)
{
    char *ap = strchr(lw, '\'');
    *also_without_n = false;
    if (!ap) return true;
    const char *end = ap + 1;
    static const char *const ends[] = { "s", "d", "ll", "re", "ve", "m", "t" };
    bool ok = false;
    for (size_t i = 0; i < sizeof(ends) / sizeof(ends[0]) && !ok; i++) ok = strcmp(end, ends[i]) == 0;
    if (!ok) return false;
    *also_without_n = strcmp(end, "t") == 0 && ap > lw + 1 && ap[-1] == 'n';
    *ap = '\0';
    return true;
}

bool spell_known(const char *w, size_t n)
{
    spell_word_t lw;
    if (!lower_into(w, n, lw)) return true;     /* not ours to judge */
    if (known_lower(lw)) return true;           /* o'clock, in the user's words */
    bool without_n;
    if (!strchr(lw, '\'') || !stem(lw, &without_n)) return false;
    if (known_lower(lw)) return true;
    if (without_n) {
        lw[strlen(lw) - 1] = '\0';
        return known_lower(lw);
    }
    return false;
}

void spell_ignore(const char *w, size_t n)
{
    spell_word_t lw;
    if (lower_into(w, n, lw)) user_put(lw);
}

bool spell_add(const char *w, size_t n)
{
    spell_word_t lw;
    if (!lower_into(w, n, lw)) return false;
    user_put(lw);

    char path[PATH_LEN + 16];
    snprintf(path, sizeof(path), "%s/%s", SPELL_DIR, SPELL_USER);
    bool ok = false;
    VOL_HOLD();
    if (VOL_PRESENT()) {
        FILE *f = fopen(path, "a");
        if (f) {
            ok = fprintf(f, "%s\n", lw) > 0;
            if (fclose(f) != 0) ok = false;
        }
    }
    VOL_RELEASE();
    if (!ok) ESP_LOGW(TAG, "cannot append to %s", path);
    return ok;
}

void spell_case_like(const char *w, size_t n, char *sugg)
{
    if (n > 0 && is_upper(w[0]) && sugg[0] >= 'a' && sugg[0] <= 'z') sugg[0] -= 'a' - 'A';
}

/* ---- suggesting -------------------------------------------------------- */

typedef struct {
    uint32_t id;
    int      dist;
} cand_t;

/* Offer word `id` at distance d, keeping the best `max` in order:
 * nearer first, then more common. */
static void offer(cand_t *best, int *nb, int max, uint32_t id, int d)
{
    int at = *nb;
    while (at > 0 && (best[at - 1].dist > d ||
                      (best[at - 1].dist == d && s_freq[best[at - 1].id] < s_freq[id]))) {
        at--;
    }
    if (at >= max) return;
    const int keep = (*nb < max) ? *nb : max - 1;
    memmove(best + at + 1, best + at, (size_t)(keep - at) * sizeof(*best));
    best[at].id = id;
    best[at].dist = d;
    if (*nb < max) (*nb)++;
}

/* Read n records of `size` bytes from off. */
static bool read_at(FILE *f, uint32_t off, void *buf, size_t size, size_t n)
{
    COUNT_READ();
    return fseek(f, (long)off, SEEK_SET) == 0 && fread(buf, size, n, f) == n;
}

/* The postings of the key with hash h, or an empty range. */
static bool find_key(FILE *f, uint32_t h, uint32_t *p0, uint32_t *p1)
{
    *p0 = *p1 = 0;
    uint32_t lo = s_dir[h >> 16];
    const uint32_t hi = s_dir[(h >> 16) + 1];
    uint8_t e[(BUCKET_CHUNK + 1) * 8];
    while (lo < hi) {
        uint32_t n = hi - lo;
        if (n > BUCKET_CHUNK) n = BUCKET_CHUNK;
        /* One past, for the end of the last one's postings. */
        if (!read_at(f, s_hdr.ent_off + 8 * lo, e, 8, n + 1)) return false;
        for (uint32_t i = 0; i < n; i++) {
            const uint32_t eh = get32(e + 8 * i);
            if (eh == h) {
                *p0 = get32(e + 8 * i + 4);
                *p1 = get32(e + 8 * i + 12);
                return *p0 <= *p1 && *p1 <= s_hdr.n_post;
            }
            if (eh > h) return true;
        }
        lo += n;
    }
    return true;
}

static int suggest_lower(FILE *f, const char *lw, int ln, cand_t *best, int max)
{
    /* At three letters or fewer, two edits make anything of anything. */
    const int maxd = ln <= 3 ? 1 : SPELL_MAX_DIST;
    char del[DEL_MAX][SPELL_PREFIX + 1];
    const int nd = deletes(lw, (size_t)ln, maxd, del);

    if (++s_stamp == 0) {
        memset(s_seen, 0, s_hdr.n_words);
        s_stamp = 1;
    }
    int nb = 0;
    uint8_t pb[POST_CHUNK * 4];
    for (int k = 0; k < nd; k++) {
        uint32_t p0, p1;
        if (!find_key(f, fnv(del[k], strlen(del[k])), &p0, &p1)) return -1;
        while (p0 < p1) {
            uint32_t n = p1 - p0;
            if (n > POST_CHUNK) n = POST_CHUNK;
            if (!read_at(f, s_hdr.post_off + 4 * p0, pb, 4, n)) return -1;
            for (uint32_t i = 0; i < n; i++) {
                const uint32_t id = get32(pb + 4 * i);
                if (id >= s_hdr.n_words || s_seen[id] == s_stamp) continue;
                s_seen[id] = s_stamp;
                const char *cw = word(id);
                const int d = osa(lw, ln, cw, (int)strlen(cw), maxd);
                if (d <= maxd && d > 0) offer(best, &nb, max, id, d);
            }
            p0 += n;
        }
    }
    return nb;
}

int spell_suggest(const char *w, size_t n, spell_word_t *out, int max)
{
    if (!s_loaded || max <= 0) return 0;
    if (max > SPELL_SUGG_MAX) max = SPELL_SUGG_MAX;
    spell_word_t lw;
    if (!lower_into(w, n, lw)) return 0;

    /* A contraction: suggest for the stem, and put the ending back. */
    char tail[SPELL_WORD_MAX + 1] = "";
    char *ap = strchr(lw, '\'');
    if (ap) {
        strcpy(tail, ap);
        *ap = '\0';
    }
    for (const char *c = lw; *c; c++) {
        if (*c < 'a' || *c > 'z') return 0;
    }
    const int ln = (int)strlen(lw);
    if (ln == 0) return 0;

    char path[PATH_LEN + 16];
    snprintf(path, sizeof(path), "%s/%s", SPELL_DIR, SPELL_DICT);
    cand_t best[SPELL_SUGG_MAX];
    int nb = -1;
    VOL_HOLD();
    if (VOL_PRESENT()) {
        FILE *f = fopen(path, "rb");
        if (f) {
            /* The same build as the word list in RAM, or the ids mean
             * nothing: a card swapped since spell_load(). */
            hdr_t h;
            if (read_hdr(f, &h) && h.build_id == s_hdr.build_id && h.n_keys == s_hdr.n_keys) {
                nb = suggest_lower(f, lw, ln, best, max);
            } else {
                ESP_LOGW(TAG, "the dictionary on the card has changed; reloading next time");
                unload();
            }
            fclose(f);
        }
    }
    VOL_RELEASE();

    int no = 0;
    for (int i = 0; i < nb; i++) {
        const char *cw = word(best[i].id);
        if (strlen(cw) + strlen(tail) > SPELL_WORD_MAX) continue;
        strcpy(out[no], cw);
        strcat(out[no], tail);
        no++;
    }
    return no;
}

/* ---- building (host) --------------------------------------------------- */

#ifdef SPELL_HOST

void spell_reset(void)
{
    unload();
    free(s_user);
    s_user = NULL;
    s_n_user = 0;
}

typedef struct { char *w; uint32_t f; } bw_t;
typedef struct { uint32_t h, id; } pair_t;

static int cmp_bw(const void *a, const void *b)
{
    return strcmp(((const bw_t *)a)->w, ((const bw_t *)b)->w);
}

static int cmp_pair(const void *a, const void *b)
{
    const pair_t *x = a, *y = b;
    if (x->h != y->h) return x->h < y->h ? -1 : 1;
    return (x->id > y->id) - (x->id < y->id);
}

static void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

static bool w32(FILE *f, uint32_t v)
{
    uint8_t b[4];
    put32(b, v);
    return fwrite(b, 1, 4, f) == 4;
}

int spell_build(const char *list_path, const char *out_path)
{
    FILE *in = fopen(list_path, "r");
    if (!in) { perror(list_path); return 1; }

    size_t nw = 0, cap = 1024;
    bw_t *ws = malloc(cap * sizeof(*ws));
    char line[512];
    while (ws && fgets(line, sizeof(line), in)) {
        char wd[SPELL_WORD_MAX + 2];
        unsigned long long fr = 1;
        if (line[0] == '#') continue;
        const int got = sscanf(line, "%41s %llu", wd, &fr);
        if (got < 1) continue;
        const size_t n = strlen(wd);
        bool ok = n >= 2 && n <= SPELL_WORD_MAX;
        for (size_t i = 0; ok && i < n; i++) {
            wd[i] = lower(wd[i]);
            ok = wd[i] >= 'a' && wd[i] <= 'z';
        }
        if (!ok) continue;
        if (nw == cap) {
            cap *= 2;
            bw_t *g = realloc(ws, cap * sizeof(*ws));
            if (!g) { free(ws); ws = NULL; break; }
            ws = g;
        }
        ws[nw].w = strdup(wd);
        ws[nw].f = fr > 0xFFFFFFFFull ? 0xFFFFFFFFu : (uint32_t)fr;
        if (!ws[nw].w) break;
        nw++;
    }
    fclose(in);
    if (!ws || nw == 0) { fprintf(stderr, "no words in %s\n", list_path); return 1; }

    /* Sorted, and one of each: a word listed twice keeps its higher count. */
    qsort(ws, nw, sizeof(*ws), cmp_bw);
    size_t u = 0;
    for (size_t i = 0; i < nw; i++) {
        if (u > 0 && strcmp(ws[u - 1].w, ws[i].w) == 0) {
            if (ws[i].f > ws[u - 1].f) ws[u - 1].f = ws[i].f;
            free(ws[i].w);
        } else {
            ws[u++] = ws[i];
        }
    }
    nw = u;

    size_t np = 0, pcap = nw * 16;
    pair_t *ps = malloc(pcap * sizeof(*ps));
    uint32_t words_len = 0;
    for (size_t i = 0; ps && i < nw; i++) {
        char del[DEL_MAX][SPELL_PREFIX + 1];
        const int nd = deletes(ws[i].w, strlen(ws[i].w), SPELL_MAX_DIST, del);
        for (int k = 0; k < nd; k++) {
            if (np == pcap) {
                pcap *= 2;
                pair_t *g = realloc(ps, pcap * sizeof(*ps));
                if (!g) { free(ps); ps = NULL; break; }
                ps = g;
            }
            ps[np].h = fnv(del[k], strlen(del[k]));
            ps[np].id = (uint32_t)i;
            np++;
        }
        words_len += (uint32_t)strlen(ws[i].w) + 1;
    }
    if (!ps) { fprintf(stderr, "out of memory\n"); return 1; }
    qsort(ps, np, sizeof(*ps), cmp_pair);
    /* Two deletes of one word may share a hash: one posting each. */
    u = 0;
    for (size_t i = 0; i < np; i++) {
        if (u > 0 && ps[u - 1].h == ps[i].h && ps[u - 1].id == ps[i].id) continue;
        ps[u++] = ps[i];
    }
    np = u;
    uint32_t nk = 0;
    for (size_t i = 0; i < np; i++) {
        if (i == 0 || ps[i].h != ps[i - 1].h) nk++;
    }

    uint32_t build_id = 2166136261u;
    for (size_t i = 0; i < nw; i++) {
        build_id ^= fnv(ws[i].w, strlen(ws[i].w) + 1);
        build_id *= 16777619u;
    }

    uint8_t hb[HDR_LEN] = { 0 };
    memcpy(hb, MAGIC, 8);
    const uint32_t words_off = HDR_LEN;
    const uint32_t freq_off  = words_off + words_len;
    const uint32_t dir_off   = freq_off + 4 * (uint32_t)nw;
    const uint32_t ent_off   = dir_off + 4 * (DIR_N + 1);
    const uint32_t post_off  = ent_off + 8 * (nk + 1);
    const uint32_t file_len  = post_off + 4 * (uint32_t)np;
    put32(hb + 8, VERSION);
    put32(hb + 12, build_id);
    put32(hb + 16, (uint32_t)nw);
    put32(hb + 20, nk);
    put32(hb + 24, (uint32_t)np);
    hb[28] = SPELL_MAX_DIST;
    hb[29] = SPELL_PREFIX;
    put32(hb + 32, words_off);
    put32(hb + 36, words_len);
    put32(hb + 40, freq_off);
    put32(hb + 44, dir_off);
    put32(hb + 48, ent_off);
    put32(hb + 52, post_off);
    put32(hb + 56, file_len);
    put32(hb + 60, fnv((const char *)hb, 60));

    FILE *out = fopen(out_path, "wb");
    if (!out) { perror(out_path); return 1; }
    bool ok = fwrite(hb, 1, HDR_LEN, out) == HDR_LEN;
    for (size_t i = 0; ok && i < nw; i++) ok = fwrite(ws[i].w, 1, strlen(ws[i].w) + 1, out) > 0;
    for (size_t i = 0; ok && i < nw; i++) ok = w32(out, ws[i].f);
    /* dir: the first entry of each top-16-bit bucket. */
    size_t e = 0;
    uint32_t ki = 0;
    for (uint32_t k = 0; ok && k <= DIR_N; k++) {
        while (e < np && (ps[e].h >> 16) < k) {
            const uint32_t h = ps[e].h;
            while (e < np && ps[e].h == h) e++;
            ki++;
        }
        ok = w32(out, ki);
    }
    /* entries, then the sentinel */
    for (size_t i = 0; ok && i < np; i++) {
        if (i == 0 || ps[i].h != ps[i - 1].h) ok = w32(out, ps[i].h) && w32(out, (uint32_t)i);
    }
    if (ok) ok = w32(out, 0xFFFFFFFFu) && w32(out, (uint32_t)np);
    for (size_t i = 0; ok && i < np; i++) ok = w32(out, ps[i].id);
    if (fclose(out) != 0) ok = false;
    if (!ok) { fprintf(stderr, "cannot write %s\n", out_path); return 1; }

    printf("%zu words, %u keys, %zu postings, %u bytes (%u in RAM on the board)\n",
           nw, nk, np, file_len, words_len + 9 * (uint32_t)nw + 4 * (DIR_N + 1));
    for (size_t i = 0; i < nw; i++) free(ws[i].w);
    free(ws);
    free(ps);
    return 0;
}

#endif /* SPELL_HOST */

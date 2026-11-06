/*
 * doclog.c -- the document as an operation log. See doclog.h.
 *
 * Every operation opens the file, does its work and closes it: the
 * volume is leased per operation and never held open (spec section 1),
 * so a card pulled between flushes costs nothing but the next flush.
 *
 * Preallocation is by writing zeros rather than f_expand(): IDF's FatFs
 * is built without FF_USE_EXPAND, and the VFS has no way to it. Zeros
 * written and fsynced ahead of the records do the same job -- the
 * cluster chain and the directory size are settled before a record
 * lands in them, so a power loss mid-append can tear the record but not
 * the allocation under it.
 *
 * SPDX-License-Identifier: MIT
 */
#include "doclog.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#ifdef DOCLOG_HOST
/* Host test build: plain files, no arbiter. */
#include <stdarg.h>
#define ESP_LOGI(t, ...) do { (void)(t); } while (0)
#define ESP_LOGW(t, ...) do { fprintf(stderr, "W %s: ", t); fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); } while (0)
#define VOL_HOLD()      do { } while (0)
#define VOL_RELEASE()   do { } while (0)
#define VOL_PRESENT()   (true)
#define MARK_HIDDEN(p)  do { (void)(p); } while (0)
#define BIG_ALLOC(n)    malloc(n)
#define DOC_RANDOM()    ((uint32_t)rand())
#else
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_random.h"
#include "storage.h"
#define VOL_HOLD()      storage_hold_brief(STORAGE_SD)
#define VOL_RELEASE()   storage_release_brief(STORAGE_SD)
#define VOL_PRESENT()   storage_present(STORAGE_SD)
#define MARK_HIDDEN(p)  storage_mark_hidden(p)
#define BIG_ALLOC(n)    heap_caps_malloc((n), MALLOC_CAP_SPIRAM)
#define DOC_RANDOM()    esp_random()
#endif

static const char *TAG = "doclog";

#define MAGIC           "JINXLOG"   /* 7 + NUL = 8 bytes */
#define VERSION         (1)
#define HDR_LEN         (64)
#define REC_HDR         (20)
#define REC_CRC         (4)
#define TYPE_CHANGESET  (1)
#define TYPE_AUTHOR     (2)         /* 0035 */
#define AUTHOR_REC_MAX  (REC_HDR + 2 + DOCLOG_AUTHOR_MAX + REC_CRC)
#define FLAG_UNTRUSTED  (0x01)
#define CS_HDR          (10)        /* old_len, new_len, n_ops */
#define CS_OP           (5)
#define CS_MAX_OPS      (8)

#define DOCLOG_GROW     (64 * 1024)
#define IO_CHUNK        (16 * 1024)
#define PATH_MAX_LEN    (128)

typedef struct {
    uint32_t doc_id;
    uint8_t  hl;
    int64_t  created;
    uint32_t export_hash;
    uint32_t crc;           /* the chain's seed */
} hdr_t;

/* What a scan found. */
typedef struct {
    bool     valid;         /* header good */
    hdr_t    hdr;
    long     end;           /* offset after the last good record */
    long     size;          /* file size */
    uint32_t seq;
    uint32_t crc;           /* last good record's, or the header's */
} scan_t;

static uint32_t  s_cap;
static uint8_t  *s_rec;         /* one record, header to CRC */
static uint32_t  s_rec_max;
static char     *s_scratch;     /* replay output */

static bool      s_open;
static char      s_path[2][PATH_MAX_LEN];
/* 0036: the export, its temp and conflict files, and the sidecar
 * holding what the last export wrote. */
static char      s_exp[PATH_MAX_LEN], s_tmp[PATH_MAX_LEN + 4];
static char      s_conf[PATH_MAX_LEN + 16], s_side[PATH_MAX_LEN + 4];
static hdr_t     s_hdr;
static long      s_end;         /* append offset, same in both twins */
static uint32_t  s_seq;
static uint32_t  s_crc;

/* 0035: the open log's authors, id 1 up; id 0 is "user" and never
 * written. Rebuilt by every replay. */
static char      s_names[DOCLOG_AUTHORS_MAX][DOCLOG_AUTHOR_MAX + 1];
static int       s_n_names;
static uint8_t   s_arec[AUTHOR_REC_MAX];

/* ---- bytes ------------------------------------------------------------ */

static void put32(uint8_t *p, uint32_t v) { p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24; }
static void put16(uint8_t *p, uint16_t v) { p[0] = v; p[1] = v >> 8; }
static void put64(uint8_t *p, int64_t v)  { put32(p, (uint32_t)v); put32(p + 4, (uint32_t)((uint64_t)v >> 32)); }
static uint32_t get32(const uint8_t *p)   { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static uint16_t get16(const uint8_t *p)   { return (uint16_t)(p[0] | p[1] << 8); }
static int64_t  get64(const uint8_t *p)   { return (int64_t)((uint64_t)get32(p) | (uint64_t)get32(p + 4) << 32); }

/* CRC-32C (Castagnoli), reflected, as LevelDB uses. Chained: pass the
 * previous CRC as `crc` to continue from it. */
static uint32_t s_crc_tab[256];

static void crc_init(void)
{
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;
        for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0x82F63B78u & (0u - (c & 1)));
        s_crc_tab[i] = c;
    }
}

static uint32_t crc32c(uint32_t crc, const uint8_t *p, size_t n)
{
    crc = ~crc;
    while (n--) crc = s_crc_tab[(crc ^ *p++) & 0xFF] ^ (crc >> 8);
    return ~crc;
}

/* ---- files ------------------------------------------------------------ */

static bool sync_close(FILE *f)
{
    bool ok = fflush(f) == 0;
    ok = ok && fsync(fileno(f)) == 0;
    return (fclose(f) == 0) && ok;
}

static long file_size(FILE *f)
{
    if (fseek(f, 0, SEEK_END) != 0) return -1;
    return ftell(f);
}

static void encode_hdr(uint8_t *b, hdr_t *h)
{
    memset(b, 0, HDR_LEN);
    memcpy(b, MAGIC, 8);
    put32(b + 8, VERSION);
    put32(b + 12, h->doc_id);
    b[16] = h->hl;
    put64(b + 20, h->created);
    put32(b + 28, h->export_hash);
    h->crc = crc32c(0, b, 60);
    put32(b + 60, h->crc);
}

static bool decode_hdr(const uint8_t *b, hdr_t *h)
{
    if (memcmp(b, MAGIC, 8) != 0 || get32(b + 8) != VERSION) return false;
    if (crc32c(0, b, 60) != get32(b + 60)) return false;
    h->doc_id      = get32(b + 12);
    h->hl          = b[16];
    h->created     = get64(b + 20);
    h->export_hash = get32(b + 28);
    h->crc         = get32(b + 60);
    return true;
}

/* ---- changesets ------------------------------------------------------- */

/*
 * Apply a changeset payload to src (src_len) into s_scratch. Checks the
 * old length against the base and the new length against the result, so
 * an intact record replayed onto the wrong text is caught (spec 4).
 */
static bool cs_apply(const uint8_t *p, uint32_t n, const char *src, uint32_t src_len,
                     uint32_t *out_len)
{
    if (n < CS_HDR) return false;
    const uint32_t old_len = get32(p), new_len = get32(p + 4);
    const uint16_t ops = get16(p + 8);
    if (old_len != src_len || new_len > s_cap) return false;
    if ((uint64_t)CS_HDR + (uint64_t)ops * CS_OP > n) return false;

    const uint8_t *bank = p + CS_HDR + ops * CS_OP;
    uint32_t bank_left = n - (uint32_t)(bank - p);
    uint32_t si = 0, oi = 0;
    for (uint16_t i = 0; i < ops; i++) {
        const uint8_t op = p[CS_HDR + i * CS_OP];
        const uint32_t c = get32(p + CS_HDR + i * CS_OP + 1);
        switch (op) {
        case '=':
            if (c > src_len - si || c > new_len - oi) return false;
            memcpy(s_scratch + oi, src + si, c); si += c; oi += c;
            break;
        case '-':
            if (c > src_len - si) return false;
            si += c;
            break;
        case '+':
            if (c > bank_left || c > new_len - oi) return false;
            memcpy(s_scratch + oi, bank, c); bank += c; bank_left -= c; oi += c;
            break;
        default:
            return false;
        }
    }
    if (bank_left != 0) return false;
    const uint32_t rest = src_len - si;     /* implicit keep */
    if (rest > new_len - oi) return false;
    memcpy(s_scratch + oi, src + si, rest);
    oi += rest;
    if (oi != new_len) return false;
    *out_len = oi;
    return true;
}

/*
 * The changeset from old to neu as keep-prefix, delete, insert: the
 * common prefix and suffix are kept and the middle is replaced. One
 * flush is one typing burst, so this is the burst's net edit. Writes
 * the payload into `p`, returns its length.
 */
static uint32_t cs_make(uint8_t *p, const char *old, uint32_t ol,
                        const char *neu, uint32_t nl)
{
    uint32_t pre = 0;
    while (pre < ol && pre < nl && old[pre] == neu[pre]) pre++;
    uint32_t suf = 0;
    while (suf < ol - pre && suf < nl - pre && old[ol - 1 - suf] == neu[nl - 1 - suf]) suf++;
    const uint32_t del = ol - pre - suf, ins = nl - pre - suf;

    put32(p, ol);
    put32(p + 4, nl);
    uint16_t ops = 0;
    uint8_t *o = p + CS_HDR;
    if (pre) { o[0] = '='; put32(o + 1, pre); o += CS_OP; ops++; }
    if (del) { o[0] = '-'; put32(o + 1, del); o += CS_OP; ops++; }
    if (ins) { o[0] = '+'; put32(o + 1, ins); o += CS_OP; ops++; }
    put16(p + 8, ops);
    memcpy(o, neu + pre, ins);
    return (uint32_t)(o - p) + ins;
}

/* ---- scanning --------------------------------------------------------- */

/*
 * Walk a twin's records from the start. With `text`, replay each into it
 * and stop at the first that does not apply. Stops at the first bad
 * length, type or CRC; that is the end of the valid prefix.
 */
static void scan(const char *path, char *text, uint32_t *len, scan_t *out)
{
    memset(out, 0, sizeof(*out));
    if (text) *len = 0;
    FILE *f = fopen(path, "rb");
    if (!f) return;
    out->size = file_size(f);
    rewind(f);

    uint8_t hb[HDR_LEN];
    if (fread(hb, 1, HDR_LEN, f) != HDR_LEN || !decode_hdr(hb, &out->hdr)) {
        fclose(f);
        return;
    }
    out->valid = true;
    out->end = HDR_LEN;
    out->crc = out->hdr.crc;
    if (text) s_n_names = 0;

    for (;;) {
        if (fread(s_rec, 1, REC_HDR, f) != REC_HDR) break;
        const uint32_t n = get32(s_rec);
        if (n == 0 || n > s_rec_max - REC_HDR - REC_CRC) break;
        const uint8_t type = s_rec[4];
        if (type != TYPE_CHANGESET && type != TYPE_AUTHOR) break;
        if (fread(s_rec + REC_HDR, 1, n + REC_CRC, f) != n + REC_CRC) break;
        const uint32_t c = crc32c(out->crc, s_rec, REC_HDR + n);
        if (c != get32(s_rec + REC_HDR + n)) break;
        const uint32_t seq = get32(s_rec + 8);
        if (type == TYPE_AUTHOR) {
            /* 0035: names an author; takes no seq of its own. Its id is
             * the next one, and the name fits. */
            const uint8_t *a = s_rec + REC_HDR;
            if (seq != out->seq || n < 3 || n != 2u + a[1] || a[1] > DOCLOG_AUTHOR_MAX) break;
            if (text) {
                if (a[0] != s_n_names + 1 || s_n_names >= DOCLOG_AUTHORS_MAX) break;
                memcpy(s_names[s_n_names], a + 2, a[1]);
                s_names[s_n_names][a[1]] = '\0';
                s_n_names++;
            }
            out->crc = c;
            out->end += REC_HDR + n + REC_CRC;
            continue;
        }
        if (seq != out->seq + 1) break;
        if (text) {
            uint32_t nl;
            if (!cs_apply(s_rec + REC_HDR, n, text, *len, &nl)) {
                ESP_LOGW(TAG, "%s: seq %lu does not apply; stopping there", path,
                         (unsigned long)seq);
                break;
            }
            memcpy(text, s_scratch, nl);
            *len = nl;
        }
        out->seq = seq;
        out->crc = c;
        out->end += REC_HDR + n + REC_CRC;
    }
    fclose(f);
}

/* ---- writing ---------------------------------------------------------- */

/* Write `n` bytes at `off`, growing the file with zeros first so the
 * bytes land inside space that is already allocated. */
static bool write_at(const char *path, long off, const uint8_t *p, size_t n)
{
    FILE *f = fopen(path, "r+b");
    if (!f) return false;
    long sz = file_size(f);
    bool ok = sz >= 0;
    if (ok && (long)(off + n) > sz) {
        static const uint8_t zeros[1024];
        long want = off + (long)n + DOCLOG_GROW;
        want -= want % DOCLOG_GROW;
        while (ok && sz < want) {
            const size_t k = (size_t)(want - sz) < sizeof(zeros) ? (size_t)(want - sz) : sizeof(zeros);
            ok = fwrite(zeros, 1, k, f) == k;
            sz += (long)k;
        }
        /* Allocation settled on the card before the record goes in. */
        ok = ok && fflush(f) == 0 && fsync(fileno(f)) == 0;
    }
    ok = ok && fseek(f, off, SEEK_SET) == 0 && fwrite(p, 1, n, f) == n;
    return sync_close(f) && ok;
}

/* A fresh twin: header and nothing else. */
static bool create_file(const char *path, hdr_t *h)
{
    uint8_t hb[HDR_LEN];
    encode_hdr(hb, h);
    FILE *f = fopen(path, "wb");
    if (!f) return false;
    bool ok = fwrite(hb, 1, HDR_LEN, f) == HDR_LEN;
    ok = sync_close(f) && ok;
    if (ok) MARK_HIDDEN(path);
    return ok;
}

/* Make dst's bytes [from, end) equal src's. A torn tail is repaired by
 * this, a missing twin by copying from 0 onto a fresh file. */
static bool copy_tail(const char *src, const char *dst, long from, long end)
{
    if (from >= end) return true;
    FILE *in = fopen(src, "rb");
    if (!in) return false;
    bool ok = fseek(in, from, SEEK_SET) == 0;
    long off = from;
    while (ok && off < end) {
        const size_t k = (size_t)(end - off) < IO_CHUNK ? (size_t)(end - off) : IO_CHUNK;
        ok = fread(s_scratch, 1, k, in) == k && write_at(dst, off, (const uint8_t *)s_scratch, k);
        off += (long)k;
    }
    fclose(in);
    return ok;
}

static bool append_record(const uint8_t *rec, size_t n)
{
    /* h0, synced and closed, before h1 is touched (spec section 4). */
    if (!write_at(s_path[0], s_end, rec, n)) return false;
    if (!write_at(s_path[1], s_end, rec, n)) {
        /* h0 has it, h1 may be torn: recovery repairs h1 from h0. The
         * record exists, so the state moves on with it. */
        ESP_LOGW(TAG, "h1 write failed after h0; recovery will repair it");
    }
    return true;
}

/* ---- the two twins together ------------------------------------------- */

/*
 * Scan both, choose the longer valid one, replay it into text (when
 * given), and bring the other up to it. Sets the module state from the
 * chosen twin. False when neither has a header.
 */
static bool recover(char *text, uint32_t *len)
{
    scan_t a, b;
    scan(s_path[0], NULL, NULL, &a);
    scan(s_path[1], NULL, NULL, &b);
    if (!a.valid && !b.valid) return false;

    int win;
    if (!a.valid)      win = 1;
    else if (!b.valid) win = 0;
    else               win = (b.end > a.end) ? 1 : 0;
    const int lose = 1 - win;
    scan_t *w = win ? &b : &a, *l = win ? &a : &b;

    if (text) {
        scan_t r;
        scan(s_path[win], text, len, &r);   /* may stop earlier */
        *w = r;
    }

    /* The loser shares the winner's prefix only if it is the same doc;
     * otherwise it is rebuilt whole. */
    long from = l->end;
    if (!l->valid || l->hdr.doc_id != w->hdr.doc_id || l->hdr.crc != w->hdr.crc) {
        if (create_file(s_path[lose], &w->hdr)) from = HDR_LEN;
        else from = w->end;     /* cannot write it; leave it */
        if (l->valid) ESP_LOGW(TAG, "twins disagree on doc id; rebuilding %s", s_path[lose]);
    }
    if (from > w->end) from = w->end;
    if (from < w->end) {
        ESP_LOGW(TAG, "repairing %s from %ld to %ld", s_path[lose], from, w->end);
        if (!copy_tail(s_path[win], s_path[lose], from, w->end)) {
            ESP_LOGW(TAG, "repair failed; the next recovery will try again");
        }
    }

    s_hdr = w->hdr;
    s_end = w->end;
    s_seq = w->seq;
    s_crc = w->crc;
    return true;
}

/* 0035: any record into `r`, its payload already at r + REC_HDR. A
 * changeset takes the next seq; an author record repeats the last. */
static bool encode_record(uint8_t *r, uint8_t type, uint32_t payload, int64_t now, bool now_ok,
                          doclog_src_t src, uint8_t author, size_t *n)
{
    put32(r, payload);
    r[4] = type;
    r[5] = now_ok ? 0 : FLAG_UNTRUSTED;
    r[6] = (uint8_t)src;
    r[7] = author;
    put32(r + 8, type == TYPE_CHANGESET ? s_seq + 1 : s_seq);
    put64(r + 12, now_ok ? now : 0);
    const uint32_t c = crc32c(s_crc, r, REC_HDR + payload);
    put32(r + REC_HDR + payload, c);
    *n = REC_HDR + payload + REC_CRC;
    return true;
}

static esp_err_t append_cs(uint32_t payload, int64_t now, bool now_ok, doclog_src_t src, uint8_t author)
{
    size_t n;
    encode_record(s_rec, TYPE_CHANGESET, payload, now, now_ok, src, author, &n);
    const uint32_t c = get32(s_rec + REC_HDR + payload);
    if (!append_record(s_rec, n)) return ESP_FAIL;
    s_end += (long)n;
    s_seq++;
    s_crc = c;
    return ESP_OK;
}

/* ---- API -------------------------------------------------------------- */

esp_err_t doclog_init(uint32_t cap)
{
    crc_init();
    s_cap = cap;
    /* Worst record: a whole text deleted and a whole text inserted. */
    s_rec_max = REC_HDR + CS_HDR + CS_MAX_OPS * CS_OP + cap + REC_CRC;
    s_rec = BIG_ALLOC(s_rec_max);
    s_scratch = BIG_ALLOC(cap > IO_CHUNK ? cap : IO_CHUNK);
    return (s_rec && s_scratch) ? ESP_OK : ESP_ERR_NO_MEM;
}

static uint8_t hl_from_name(const char *name)
{
    const char *dot = strrchr(name, '.');
    if (dot && strcmp(dot, ".md") == 0)  return 1;
    if (dot && strcmp(dot, ".tex") == 0) return 2;
    return 0;
}

static bool side_write(uint32_t last, uint32_t pending, uint32_t body);
static bool file_hash(const char *path, uint32_t *h);
static uint32_t footer_strip(const char *t, uint32_t n);

/* A document with no log: import the export if there is one (spec 4,
 * "importing existing files"), as one insert stamped with its mtime. */
static esp_err_t create_log(const char *name, char *text, uint32_t *len,
                            int64_t now, bool now_ok)
{
    char exp[PATH_MAX_LEN];
    snprintf(exp, sizeof(exp), "%s/%s", DOCLOG_DIR, name);
    uint32_t n = 0;
    int64_t when = now;
    bool when_ok = now_ok;
    bool imported = false;
    struct stat st;
    if (stat(exp, &st) == 0) {
        FILE *f = fopen(exp, "rb");
        if (f) {
            n = (uint32_t)fread(text, 1, s_cap, f);
            fclose(f);
            n = footer_strip(text, n);  /* 0037: ours, not the text */
            imported = true;
            when = (int64_t)st.st_mtime;
            when_ok = true;
            ESP_LOGI(TAG, "importing %s, %lu bytes", exp, (unsigned long)n);
        }
    }

    s_hdr = (hdr_t){ .doc_id = DOC_RANDOM(), .hl = hl_from_name(name),
                     .created = now_ok ? now : 0, .export_hash = 0 };
    if (!create_file(s_path[0], &s_hdr) || !create_file(s_path[1], &s_hdr)) {
        return ESP_FAIL;
    }
    s_end = HDR_LEN;
    s_seq = 0;
    s_crc = s_hdr.crc;
    s_n_names = 0;
    s_open = true;
    *len = 0;
    if (n) {
        const uint32_t p = cs_make(s_rec + REC_HDR, "", 0, text, n);
        if (append_cs(p, when, when_ok, DOCLOG_SRC_USER, 0) != ESP_OK) { s_open = false; return ESP_FAIL; }
        *len = n;
    }
    /* 0036: the file is ours now, as the log's first text -- even when
     * that text is empty (0037: a file of nothing but a footer). */
    uint32_t h;
    if (imported && file_hash(exp, &h)) side_write(h, h, crc32c(0, (const uint8_t *)text, n));
    return ESP_OK;
}

esp_err_t doclog_open(const char *name, char *text, uint32_t *len,
                      int64_t now, bool now_ok)
{
    s_open = false;
    *len = 0;
    if (!VOL_PRESENT()) return ESP_ERR_INVALID_STATE;
    if (snprintf(s_path[0], PATH_MAX_LEN, "%s/.%s%s", DOCLOG_DIR, name, DOCLOG_SUFFIX0) >= PATH_MAX_LEN ||
        snprintf(s_path[1], PATH_MAX_LEN, "%s/.%s%s", DOCLOG_DIR, name, DOCLOG_SUFFIX1) >= PATH_MAX_LEN ||
        snprintf(s_exp, sizeof s_exp, "%s/%s", DOCLOG_DIR, name) >= (int)sizeof s_exp) {
        return ESP_ERR_INVALID_ARG;
    }
    /* 0036: name.md.tmp, .name.md.x, and name.conflict.md -- the
     * conflict's extension kept, so it opens as what it is. */
    snprintf(s_tmp, sizeof s_tmp, "%s.tmp", s_exp);
    snprintf(s_side, sizeof s_side, "%s/.%s.x", DOCLOG_DIR, name);
    const char *dot = strrchr(name, '.');
    const int stem = dot ? (int)(dot - name) : (int)strlen(name);
    snprintf(s_conf, sizeof s_conf, "%s/%.*s.conflict%s", DOCLOG_DIR, stem, name, dot ? dot : "");

    VOL_HOLD();
    esp_err_t err = ESP_OK;
    if (mkdir(DOCLOG_DIR, 0777) != 0 && errno != EEXIST) {
        ESP_LOGW(TAG, "mkdir %s: errno %d", DOCLOG_DIR, errno);
    }
    if (recover(text, len)) {
        s_open = true;
        ESP_LOGI(TAG, "%s: seq %lu, %lu bytes", name, (unsigned long)s_seq, (unsigned long)*len);
    } else {
        err = create_log(name, text, len, now, now_ok);
    }
    VOL_RELEASE();
    return err;
}

bool doclog_is_open(void) { return s_open; }
uint32_t doclog_seq(void) { return s_seq; }
void doclog_close(void)   { s_open = false; }

/* ---- export (0036) ---------------------------------------------------- */

#define SIDE_MAGIC      (0x3158474Au)   /* "JGX1", 0036: 16 bytes */
#define SIDE_MAGIC2     (0x3258474Au)   /* "JGX2", 0037: 20, with the body's hash */
#define NO_BODY         (0xFFFFFFFFu)   /* a JGX1 sidecar: not known */

/* ---- the footer (0037) --------------------------------------------------
 *
 * After the text, in the export only -- never in the log:
 *
 *   <text>\n\n---\n\nauthor: <name>  \nexported: YYYY-MM-DD HH:MM  \nJinxed Writer <version>\n
 *
 * author only when a name is set, exported only when the RTC's time is
 * trusted. The rule makes it a footer, and the two trailing spaces put
 * each line on its own in rendered markdown. An import that ends in one
 * drops it, so footers never pile up in the text.
 */
#define FOOT_SEP        "\n\n---\n\n"
#define FOOT_BRAND      "Jinxed Writer "

static bool foot_line(const char *a, const char *b, const char *key)
{
    const size_t k = strlen(key);
    if ((size_t)(b - a) < k + 3 || memcmp(a, key, k) != 0 || b[-1] != ' ' || b[-2] != ' ') return false;
    for (const char *p = a; p < b; p++) if (*p < ' ' || *p > '~') return false;
    return true;
}

/* The text without a footer of ours at its end; n when there is none. */
static uint32_t footer_strip(const char *t, uint32_t n)
{
    const uint32_t sl = (uint32_t)strlen(FOOT_SEP);
    if (n < sl + strlen(FOOT_BRAND) + 1 || t[n - 1] != '\n') return n;
    const uint32_t lo = n > DOCLOG_FOOT_MAX + sl ? n - DOCLOG_FOOT_MAX - sl : 0;
    for (uint32_t s = n - sl + 1; s-- > lo; ) {
        if (memcmp(t + s, FOOT_SEP, sl) != 0) continue;
        const char *p = t + s + sl, *end = t + n;
        bool ok = true;
        while (ok && p < end) {
            const char *e = memchr(p, '\n', (size_t)(end - p));
            if (!e) { ok = false; break; }
            if (e + 1 == end) {         /* the last line: the brand */
                ok = (size_t)(e - p) > strlen(FOOT_BRAND) && !memcmp(p, FOOT_BRAND, strlen(FOOT_BRAND));
                for (const char *q = p; ok && q < e; q++) ok = *q >= ' ' && *q <= '~';
            } else {
                ok = foot_line(p, e, "author: ") || foot_line(p, e, "exported: ");
            }
            p = e + 1;
        }
        return ok ? s : n;
    }
    return n;
}

static uint32_t footer_make(char *f, const char *author, int64_t now, bool now_ok, const char *build);

uint32_t doclog_footer(char *f, const char *author, int64_t now, bool now_ok, const char *build)
{
    return footer_make(f, author, now, now_ok, build);
}

static uint32_t footer_make(char *f, const char *author, int64_t now, bool now_ok, const char *build)
{
    int k = snprintf(f, DOCLOG_FOOT_MAX, "%s", FOOT_SEP);
    if (author && author[0]) k += snprintf(f + k, DOCLOG_FOOT_MAX - (size_t)k, "author: %.40s  \n", author);
    if (now_ok) {
        const time_t tt = (time_t)now;
        struct tm tm;
        gmtime_r(&tt, &tm);
        k += snprintf(f + k, DOCLOG_FOOT_MAX - (size_t)k, "exported: %04d-%02d-%02d %02d:%02d  \n",
                      (tm.tm_year + 1900) % 10000, (tm.tm_mon + 1) % 100, tm.tm_mday % 100,
                      tm.tm_hour % 100, tm.tm_min % 100);
    }
    k += snprintf(f + k, DOCLOG_FOOT_MAX - (size_t)k, "%s%.40s\n", FOOT_BRAND, build && build[0] ? build : "?");
    return k < DOCLOG_FOOT_MAX ? (uint32_t)k : DOCLOG_FOOT_MAX - 1;
}

/* CRC-32C of a whole file, read to its end: a file the user supplies
 * may be any size (CLAUDE.md), and nothing here needs its length. */
static bool file_hash(const char *path, uint32_t *h)
{
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    uint32_t c = 0;
    size_t k;
    while ((k = fread(s_scratch, 1, IO_CHUNK, f)) > 0) c = crc32c(c, (const uint8_t *)s_scratch, k);
    const bool ok = !ferror(f);
    fclose(f);
    *h = c;
    return ok;
}

/* The sidecar: magic, the hash of the export as last written, the hash
 * of one being put in place now, and a CRC of the three. Either hash
 * found on the card is ours, so a power cut between writing the new
 * file and recording it is not taken for someone else's edit. */
static bool side_write(uint32_t last, uint32_t pending, uint32_t body)
{
    uint8_t b[20];
    put32(b, SIDE_MAGIC2);
    put32(b + 4, last);
    put32(b + 8, pending);
    put32(b + 12, body);
    put32(b + 16, crc32c(0, b, 16));
    FILE *f = fopen(s_side, "wb");
    if (!f) return false;
    const bool ok = fwrite(b, 1, sizeof b, f) == sizeof b;
    const bool done = sync_close(f) && ok;
    if (done) MARK_HIDDEN(s_side);
    return done;
}

/* A JGX1 sidecar (0036) reads with its body unknown. */
static bool side_read(uint32_t *last, uint32_t *pending, uint32_t *body)
{
    uint8_t b[20];
    FILE *f = fopen(s_side, "rb");
    if (!f) return false;
    const size_t n = fread(b, 1, sizeof b, f);
    fclose(f);
    if (n == 16 && get32(b) == SIDE_MAGIC && get32(b + 12) == crc32c(0, b, 12)) {
        *body = NO_BODY;
    } else if (n == 20 && get32(b) == SIDE_MAGIC2 && get32(b + 16) == crc32c(0, b, 16)) {
        *body = get32(b + 12);
    } else {
        return false;
    }
    *last = get32(b + 4);
    *pending = get32(b + 8);
    return true;
}

/* Write `text` and then `foot` to `path`, synced. */
static bool write_whole(const char *path, const char *text, uint32_t len, const char *foot, uint32_t flen)
{
    FILE *f = fopen(path, "wb");
    if (!f) return false;
    bool ok = fwrite(text, 1, len, f) == len;
    ok = ok && fwrite(foot, 1, flen, f) == flen;
    return sync_close(f) && ok;
}

esp_err_t doclog_export(const char *text, uint32_t len, const char *author,
                        int64_t now, bool now_ok, const char *build)
{
    if (!s_open) return ESP_ERR_INVALID_STATE;
    if (!VOL_PRESENT()) return ESP_ERR_INVALID_STATE;
    VOL_HOLD();
    esp_err_t err = ESP_OK;
    static char foot[DOCLOG_FOOT_MAX];
    const uint32_t flen = footer_make(foot, author, now, now_ok, build);
    const uint32_t body = crc32c(0, (const uint8_t *)text, len);
    const uint32_t neu = crc32c(body, (const uint8_t *)foot, flen);
    uint32_t have = 0, last = 0, pending = 0, side_body = NO_BODY;
    struct stat st;
    const bool exists = stat(s_exp, &st) == 0;
    const bool side = side_read(&last, &pending, &side_body);
    if (exists && !file_hash(s_exp, &have)) {
        err = ESP_FAIL;
    } else if (exists && have == neu) {
        /* Already this text: only the record may be behind. */
        if (!side || last != neu || pending != neu || side_body != body) side_write(neu, neu, body);
    } else if (exists && side && have == last && side_body == body) {
        /* 0037: ours, and the text is what it holds: only the footer's
         * time would change. Not worth a write. */
    } else if (exists && (!side || (have != last && have != pending))) {
        /* Not what we last wrote: someone else's. Leave it; put ours
         * beside it (spec 4, "external edits"). */
        ESP_LOGW(TAG, "%s changed outside; writing %s", s_exp, s_conf);
        err = write_whole(s_conf, text, len, foot, flen) ? ESP_ERR_INVALID_STATE : ESP_FAIL;
    } else if (!write_whole(s_tmp, text, len, foot, flen) || !side_write(side ? last : neu, neu, NO_BODY)) {
        unlink(s_tmp);
        err = ESP_FAIL;
    } else {
        /* FatFs will not rename over a file: unlink, then rename. The
         * log has everything, so the gap costs nothing. */
        if (exists) unlink(s_exp);
        if (rename(s_tmp, s_exp) != 0) {
            ESP_LOGW(TAG, "rename %s: errno %d", s_tmp, errno);
            err = ESP_FAIL;
        } else {
            side_write(neu, neu, body);
        }
    }
    VOL_RELEASE();
    return err;
}

int doclog_author_id(const char *name)
{
    if (!name || !name[0]) return 0;
    for (int i = 0; i < s_n_names; i++) {
        if (strcmp(s_names[i], name) == 0) return i + 1;
    }
    return -1;
}

/* 0035: the id `name` is credited as, writing its author record first
 * if this log has not named it yet. 0 for the default, for a name too
 * long, when the table is full, or when the record cannot be written --
 * the edit is then the user's, unnamed, rather than lost. */
static uint8_t author_for(const char *name, int64_t now, bool now_ok)
{
    const int known = doclog_author_id(name);
    if (known >= 0) return (uint8_t)known;
    const size_t len = strlen(name);
    if (len > DOCLOG_AUTHOR_MAX || s_n_names >= DOCLOG_AUTHORS_MAX) {
        ESP_LOGW(TAG, "author not recorded (%s); the edit is the user's",
                 len > DOCLOG_AUTHOR_MAX ? "name too long" : "too many names");
        return 0;
    }
    uint8_t *a = s_arec + REC_HDR;
    a[0] = (uint8_t)(s_n_names + 1);
    a[1] = (uint8_t)len;
    memcpy(a + 2, name, len);
    size_t n;
    encode_record(s_arec, TYPE_AUTHOR, 2 + (uint32_t)len, now, now_ok, DOCLOG_SRC_USER, 0, &n);
    if (!append_record(s_arec, n)) return 0;
    s_end += (long)n;
    s_crc = get32(s_arec + n - REC_CRC);
    memcpy(s_names[s_n_names], name, len);
    s_names[s_n_names][len] = '\0';
    return (uint8_t)++s_n_names;
}

esp_err_t doclog_append(const char *old, uint32_t old_len,
                        const char *neu, uint32_t new_len,
                        int64_t now, bool now_ok, doclog_src_t src, const char *author)
{
    if (!s_open) return ESP_ERR_INVALID_STATE;
    if (old_len == new_len && memcmp(old, neu, old_len) == 0) return ESP_OK;
    if (!VOL_PRESENT()) return ESP_ERR_INVALID_STATE;
    VOL_HOLD();
    const uint8_t id = src == DOCLOG_SRC_USER && author ? author_for(author, now, now_ok) : 0;
    const uint32_t p = cs_make(s_rec + REC_HDR, old, old_len, neu, new_len);
    const esp_err_t err = append_cs(p, now, now_ok, src, id);
    VOL_RELEASE();
    return err;
}

esp_err_t doclog_verify(void)
{
    if (!VOL_PRESENT()) return ESP_ERR_INVALID_STATE;
    const hdr_t h = s_hdr;
    const long end = s_end;
    const uint32_t seq = s_seq, crc = s_crc;

    VOL_HOLD();
    const bool any = recover(NULL, NULL);
    VOL_RELEASE();
    if (!any) { s_open = false; return ESP_ERR_NOT_FOUND; }
    if (s_hdr.doc_id == h.doc_id && s_hdr.crc == h.crc &&
        s_end == end && s_seq == seq && s_crc == crc) {
        return ESP_OK;
    }
    /* Someone else's log: put our state back and let the caller decide. */
    s_hdr = h; s_end = end; s_seq = seq; s_crc = crc;
    s_open = false;
    return ESP_ERR_INVALID_STATE;
}

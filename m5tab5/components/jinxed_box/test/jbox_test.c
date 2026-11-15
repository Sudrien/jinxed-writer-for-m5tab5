/* SPDX-License-Identifier: MIT
 *
 * Host test for jbox_storage.c (u-jinxed-0003): the table jinxed_box_calls()
 * builds over feckless_box_calls_v1(), and doclog.c's own sequences -- the
 * directory made, a twin created, grown with zeros and written at an offset,
 * fsynced through fileno(), read back, an export written to .tmp and renamed
 * over the old one, the hidden mark -- through feckless_umode's ext hook,
 * over real files under ./jbox_root. Then every refusal.
 *
 *   make -C components/jinxed_box/test FECKLESS=<feckless-libs at umode>
 */
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include "feckless_box_calls.h"
#include "jinxed_box.h"
#include "storage.h"

static int failed;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); failed++; } } while (0)

/* The stand-ins this component needs beyond feckless_box_calls' own. */
static int brief[STORAGE_COUNT], briefs_taken;
static char hidden[256];
void storage_hold_brief(storage_id_t id) { brief[id]++; briefs_taken++; }
void storage_release_brief(storage_id_t id) { brief[id]--; }
void storage_mark_hidden(const char *path) { snprintf(hidden, sizeof hidden, "%s", path); }
uint32_t esp_random(void) { return 0x5eed1234u; }

#define MEM (256 * 1024)
static uint8_t *mem;
static feckless_box_t box;
static feckless_umode_window_t w;
static feckless_umode_calls_t tab;
static const feckless_umode_call_t *calls_tab;
static uint32_t ncalls;
static int *app_errno;

static int nr_index(const char *name)
{
    for (uint32_t i = 0; i < ncalls; i++)
        if (!strcmp(calls_tab[i].name, name)) return (int)i;
    return -1;
}
static uint32_t call(const char *name, uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3)
{
    const int i = nr_index(name);
    if (i < 0) { printf("FAIL no call %s\n", name); failed++; return 0; }
    w.ctx.x[10] = a0; w.ctx.x[11] = a1; w.ctx.x[12] = a2; w.ctx.x[13] = a3;
    uint32_t ret = 0xdeadbeef;
    CHECK(feckless_umode_calls_ext(&w, FECKLESS_SYS_EXT_BASE + (uint32_t)i, &w.ctx.x[10], &ret));
    return ret;
}
static uint32_t P(const void *p) { return (uint32_t)(uintptr_t)p; }
static void *box_alloc(uint32_t n) { void *p = (void *)(uintptr_t)feckless_box_alloc(&box, n); memset(p, 0, n); return p; }
static char *put_str(const char *s) { char *p = box_alloc((uint32_t)strlen(s) + 1); strcpy(p, s); return p; }
#define M1 ((uint32_t)-1)

static void test_table(const char *answers)
{
    uint32_t fn = 0;
    const feckless_umode_call_t *f = feckless_box_calls_v1(&fn);
    uint32_t n2 = 0;
    CHECK(jinxed_box_calls(&n2) == calls_tab && n2 == ncalls);     /* built once */
    /* No name twice: the loader resolves the first, so a duplicate would
     * only waste a stub, but it would mean an override that did not take. */
    for (uint32_t i = 0; i < ncalls; i++)
        for (uint32_t j = i + 1; j < ncalls; j++)
            if (!strcmp(calls_tab[i].name, calls_tab[j].name)) { printf("FAIL twice: %s\n", calls_tab[i].name); failed++; }
    /* Every feckless name is still answered, by feckless or by Jinxed. */
    for (uint32_t i = 0; i < fn; i++) CHECK(nr_index(f[i].name) >= 0);
    /* The feckless entries kept are feckless's own, unchanged. */
    for (uint32_t i = 0; i < fn; i++) {
        const int k = nr_index(f[i].name);
        if (k >= 0 && !strcmp(f[i].name, "gfx_draw_text")) CHECK(calls_tab[k].fn == f[i].fn);
        if (k >= 0 && !strcmp(f[i].name, "fopen")) CHECK(calls_tab[k].fn != f[i].fn);   /* overridden */
        if (k >= 0 && !strcmp(f[i].name, "feckless_abi_storage_v2")) CHECK(calls_tab[k].data == f[i].data);
    }
    /* answers.txt is exactly the names this table answers itself. */
    FILE *a = fopen(answers, "r");
    CHECK(a != NULL);
    int listed = 0;
    char line[128];
    while (a && fgets(line, sizeof line, a)) {
        line[strcspn(line, "\r\n")] = '\0';
        if (!line[0] || line[0] == '#') continue;
        listed++;
        const int k = nr_index(line);
        int fk = -1;
        for (uint32_t i = 0; i < fn; i++) if (!strcmp(f[i].name, line)) fk = (int)i;
        if (k < 0) { printf("FAIL answers.txt names %s, which the table lacks\n", line); failed++; }
        else if (fk >= 0 && calls_tab[k].fn == f[fk].fn) { printf("FAIL %s is feckless's, not Jinxed's\n", line); failed++; }
    }
    if (a) fclose(a);
    /* Every entry not feckless's is listed. */
    int own = 0;
    for (uint32_t i = 0; i < ncalls; i++) {
        int fk = -1;
        for (uint32_t j = 0; j < fn; j++) if (!strcmp(f[j].name, calls_tab[i].name)) fk = (int)j;
        if (fk < 0 || calls_tab[i].fn != f[fk].fn) own++;
    }
    CHECK(own == listed && own == 19);
}

/* doclog.c: open_dir, create_twin, grow and write_at, sync_close, replay,
 * the export's tmp-and-rename, and the hidden mark. */
static void test_doclog(void)
{
    char *dir = put_str("jbox_root");
    char *twin = put_str("jbox_root/.2026-11-15.md.h0");
    char *tmp = put_str("jbox_root/2026-11-15.md.tmp");
    char *exp = put_str("jbox_root/2026-11-15.md");
    char *wb = put_str("wb"), *rpb = put_str("r+b"), *rb = put_str("rb");
    struct stat *st = box_alloc(sizeof *st);

    CHECK(call("mkdir", P(dir), 0777, 0, 0) == 0);
    *app_errno = 0;
    CHECK(call("mkdir", P(dir), 0777, 0, 0) == M1 && *app_errno == EEXIST);   /* doclog reads errno */

    uint8_t *hdr = box_alloc(64);
    memcpy(hdr, "JINXLOG", 8);
    const uint32_t f = call("fopen", P(twin), P(wb), 0, 0);
    CHECK((f & 0xffffff00u) == 0x4a464c00u);
    CHECK(call("fwrite", P(hdr), 1, 64, f) == 64);
    CHECK(call("ferror", f, 0, 0, 0) == 0);
    CHECK(call("fflush", f, 0, 0, 0) == 0);
    const uint32_t fd = call("fileno", f, 0, 0, 0);
    CHECK(fd != M1 && call("fsync", fd, 0, 0, 0) == 0);
    CHECK(call("fclose", f, 0, 0, 0) == 0);
    CHECK(call("storage_mark_hidden", P(twin), 0, 0, 0) == 0 && !strcmp(hidden, twin));

    /* Grown with zeros, then 20000 bytes at 64: across two slice edges. */
    uint8_t *rec = box_alloc(20000);
    uint32_t x = 1;                                               /* not periodic in a slice */
    for (int i = 0; i < 20000; i++) rec[i] = (uint8_t)((x = x * 1103515245u + 12345u) >> 16);
    const uint32_t g = call("fopen", P(twin), P(rpb), 0, 0);
    CHECK(g != 0);
    CHECK(call("fseek", g, 0, SEEK_END, 0) == 0 && call("ftell", g, 0, 0, 0) == 64);
    uint8_t *zeros = box_alloc(4096);
    CHECK(call("fwrite", P(zeros), 1, 4096, g) == 4096);
    CHECK(call("fseek", g, 64, SEEK_SET, 0) == 0);
    CHECK(call("fwrite", P(rec), 4, 5000, g) == 5000);            /* whole items */
    CHECK(call("ftell", g, 0, 0, 0) == 20064);
    CHECK(call("fflush", 0, 0, 0, 0) == 0);                      /* fflush(NULL) */
    CHECK(call("fsync", call("fileno", g, 0, 0, 0), 0, 0, 0) == 0);
    CHECK(call("fclose", g, 0, 0, 0) == 0);

    /* Replayed: read back, byte for byte, through the slices. */
    uint8_t *back = box_alloc(20000);
    const uint32_t r = call("fopen", P(twin), P(rb), 0, 0);
    CHECK(call("fread", P(back), 1, 64, r) == 64 && !memcmp(back, "JINXLOG", 8));
    CHECK(call("fread", P(back), 1, 20000, r) == 20000 && !memcmp(back, rec, 20000));
    CHECK(call("fread", P(back), 1, 100, r) == 0);                /* at the end */
    CHECK(call("fclose", r, 0, 0, 0) == 0);
    CHECK(call("stat", P(twin), P(st), 0, 0) == 0 && st->st_size == 20064);

    /* The export: written to .tmp, the old one unlinked, renamed in. */
    char *text = put_str("# a heading\n\nsome words\n");
    const uint32_t e = call("fopen", P(exp), P(wb), 0, 0);
    CHECK(call("fwrite", P(text), 1, 5, e) == 5 && call("fclose", e, 0, 0, 0) == 0);
    const uint32_t t = call("fopen", P(tmp), P(wb), 0, 0);
    CHECK(call("fwrite", P(text), 1, (uint32_t)strlen(text), t) == strlen(text));
    CHECK(call("fclose", t, 0, 0, 0) == 0);
    CHECK(call("unlink", P(exp), 0, 0, 0) == 0);
    CHECK(call("rename", P(tmp), P(exp), 0, 0) == 0);
    CHECK(call("stat", P(exp), P(st), 0, 0) == 0 && st->st_size == (off_t)strlen(text));
    *app_errno = 0;
    CHECK(call("stat", P(tmp), P(st), 0, 0) == M1 && *app_errno == ENOENT);
    char *fg = box_alloc(32);
    const uint32_t l = call("fopen", P(exp), P(rb), 0, 0);
    CHECK(call("fgets", P(fg), 32, l, 0) == P(fg) && !strcmp(fg, "# a heading\n"));
    CHECK(call("fclose", l, 0, 0, 0) == 0);

    /* The id doclog gives a new log. */
    CHECK(call("esp_random", 0, 0, 0, 0) == 0x5eed1234u);
}

static void test_refusals(void)
{
    char *wb = put_str("wb");
    struct stat *st = box_alloc(sizeof *st);
    static const char *const outside[] = {
        "/sd/other", "jbox_rootx/a", "jbox_roo", "jbox_root/../x", "jbox_root/a/..",
        "jbox_root//..//x", "../jbox_root/a", "/jbox_root/a", "",
    };
    for (size_t i = 0; i < sizeof outside / sizeof *outside; i++) {
        char *p = put_str(outside[i]);
        *app_errno = 0;
        CHECK(call("fopen", P(p), P(wb), 0, 0) == 0 && *app_errno == EACCES);
        *app_errno = 0;
        CHECK(call("mkdir", P(p), 0777, 0, 0) == M1 && *app_errno == EACCES);
        CHECK(call("unlink", P(p), 0, 0, 0) == M1);
        CHECK(call("stat", P(p), P(st), 0, 0) == M1);
    }
    CHECK(access("x", F_OK) != 0 && access("jbox_rootx", F_OK) != 0);   /* nothing made */

    /* .. as part of a longer name is a name. */
    char *dots = put_str("jbox_root/..notes.md");
    const uint32_t d = call("fopen", P(dots), P(wb), 0, 0);
    CHECK(d != 0 && call("fclose", d, 0, 0, 0) == 0);

    /* rename: either end outside */
    char *in = put_str("jbox_root/..notes.md"), *out = put_str("/tmp/escaped");
    *app_errno = 0;
    CHECK(call("rename", P(in), P(out), 0, 0) == M1 && *app_errno == EACCES);
    CHECK(call("rename", P(out), P(in), 0, 0) == M1);
    CHECK(access("jbox_root/..notes.md", F_OK) == 0);

    /* pointers: a host path, an unterminated one, a path too long */
    *app_errno = 0;
    CHECK(call("fopen", P("jbox_root/a"), P(wb), 0, 0) == 0 && *app_errno == EFAULT);
    char *edge = (char *)mem + MEM - 4;
    memcpy(edge, "jbox", 4);                                       /* runs off the box */
    CHECK(call("fopen", P(edge), P(wb), 0, 0) == 0);
    char *lng = box_alloc(400);
    memcpy(lng, "jbox_root/", 10);
    memset(lng + 10, 'a', 300);
    *app_errno = 0;
    CHECK(call("fopen", P(lng), P(wb), 0, 0) == 0 && *app_errno == EFAULT);

    /* modes */
    char *p = put_str("jbox_root/m");
    static const char *const modes[] = { "x", "r w", "rw", "wbx", "+r", "rb+++", "" };
    for (size_t i = 0; i < sizeof modes / sizeof *modes; i++) {
        char *m = put_str(modes[i]);
        *app_errno = 0;
        CHECK(call("fopen", P(p), P(m), 0, 0) == 0 && *app_errno == EINVAL);
    }

    /* a stat buffer past the box, a write from outside it, an overflow */
    CHECK(call("stat", P(p), P(mem + MEM - 8), 0, 0) == M1);
    const uint32_t f = call("fopen", P(p), P(wb), 0, 0);
    CHECK(f != 0);
    CHECK(call("fwrite", P(&box), 1, 16, f) == 0);
    CHECK(call("fwrite", P(mem), 0x10000, 0x10001, f) == 0);
    CHECK(call("fwrite", P(mem + MEM - 8), 1, 16, f) == 0);
    CHECK(call("fread", P(mem + MEM - 8), 1, 16, f) == 0);
    CHECK(call("fgets", P(mem + MEM - 8), 16, f, 0) == 0);

    /* a token is not an fd, nor an fd a token, nor feckless's FILE ours */
    *app_errno = 0;
    CHECK(call("fsync", f, 0, 0, 0) == M1 && *app_errno == EBADF);
    const uint32_t fd = call("fileno", f, 0, 0, 0);
    CHECK(call("fclose", fd, 0, 0, 0) == (uint32_t)EOF);
    CHECK(call("fwrite", P(wb), 1, 2, 0x46494c00u) == 0);       /* feckless's FILE token */
    CHECK(call("ftell", 0x4a464c00u + JINXED_BOX_FILES, 0, 0, 0) == M1);
    CHECK(call("fclose", f, 0, 0, 0) == 0);
    CHECK(call("fclose", f, 0, 0, 0) == (uint32_t)EOF);           /* twice */
    CHECK(call("fsync", fd, 0, 0, 0) == M1);                      /* closed */

    /* eight open at once, the ninth refused */
    uint32_t h[JINXED_BOX_FILES];
    for (int i = 0; i < JINXED_BOX_FILES; i++) CHECK((h[i] = call("fopen", P(p), P(wb), 0, 0)) != 0);
    *app_errno = 0;
    CHECK(call("fopen", P(p), P(wb), 0, 0) == 0 && *app_errno == EMFILE);
    for (int i = 0; i < JINXED_BOX_FILES; i++) CHECK(call("fclose", h[i], 0, 0, 0) == 0);

    /* errno with tp outside the box is not written */
    const uint32_t tp = w.ctx.x[4];
    w.ctx.x[4] = P(&box);
    CHECK(call("fopen", P(out), P(wb), 0, 0) == 0);
    w.ctx.x[4] = tp;
}

static void test_holds_and_end(void)
{
    char *p = put_str("jbox_root/left-open");
    char *wb = put_str("wb");
    CHECK(call("storage_release_brief", STORAGE_SD, 0, 0, 0) == 0 && brief[STORAGE_SD] == 0);   /* never the host's */
    CHECK(call("storage_hold_brief", STORAGE_SD, 0, 0, 0) == 0 && brief[STORAGE_SD] == 1);
    CHECK(call("storage_hold_brief", STORAGE_SD, 0, 0, 0) == 0 && brief[STORAGE_SD] == 2);
    CHECK(call("storage_release_brief", STORAGE_SD, 0, 0, 0) == 0 && brief[STORAGE_SD] == 1);
    CHECK(call("storage_hold_brief", STORAGE_COUNT, 0, 0, 0) == 0);
    CHECK(call("storage_hold_brief", 0xffffffffu, 0, 0, 0) == 0);
    CHECK(briefs_taken == 2);
    storage_hold_brief(STORAGE_SD);                               /* the host's own */
    const uint32_t f = call("fopen", P(p), P(wb), 0, 0);
    CHECK(f != 0 && call("fwrite", P(p), 1, 4, f) == 4);
    jinxed_box_end();
    CHECK(brief[STORAGE_SD] == 1);                                /* the host's stands */
    CHECK(call("ftell", f, 0, 0, 0) == M1);                       /* closed */
    struct stat s;
    CHECK(stat("jbox_root/left-open", &s) == 0 && s.st_size == 4);   /* flushed by the close */
    jinxed_box_end();                                             /* twice is nothing */
    CHECK(brief[STORAGE_SD] == 1);
}

int main(int argc, char **argv)
{
    if (argc < 2) { printf("usage: jbox_test <answers.txt>\n"); return 2; }
    mem = mmap(NULL, MEM, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_32BIT, -1, 0);
    if (mem == MAP_FAILED) { printf("no memory below 4 GB\n"); return 1; }
    feckless_box_init(&box, (uintptr_t)mem, MEM, 4096);
    calls_tab = jinxed_box_calls(&ncalls);
    CHECK(calls_tab != NULL);
    tab = (feckless_umode_calls_t){ calls_tab, ncalls, &box };
    w.lo = (uintptr_t)mem;
    w.hi = (uintptr_t)mem + MEM;
    w.ext_arg = &tab;
    app_errno = box_alloc(sizeof *app_errno);                     /* the thread's errno block */
    w.ctx.x[4] = P(app_errno);
    test_table(argv[1]);
    test_doclog();
    test_refusals();
    test_holds_and_end();
    feckless_box_calls_end();
    free((void *)calls_tab);
    printf(failed ? "%d FAILED\n" : "jinxed_box: all passed\n", failed);
    return failed != 0;
}

/* SPDX-License-Identifier: MIT
 *
 * Host test for jbox_storage.c (u-jinxed-0004, 0012): Jinxed's write
 * filter, installed in feckless_box_calls (u-feckless-0055). doclog.c's
 * sequence under ./jbox_root goes through; every write named outside it is
 * refused by feckless on the filter's word, with nothing made; every read
 * is let through. Over real files, through feckless_umode's ext hook,
 * against feckless_box_calls' own stand-ins. feckless's own test covers the
 * copy and the race.
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
#include <utime.h>

#include "feckless_box_calls.h"
#include "ff.h"
#include "jinxed_box.h"

/* feckless_box_calls' stand-ins record these. */
extern uintptr_t box_lo, box_hi;
extern int fat_open, io_open;
extern BYTE chmod_attr;
extern char hidden_path[300];

static int failed;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); failed++; } } while (0)

#define MEM (256 * 1024)
#define M1 ((uint32_t)-1)
static uint8_t *mem;
static feckless_box_t box;
static feckless_umode_window_t w;
static feckless_umode_calls_t tab;
static const feckless_umode_call_t *calls_tab;
static uint32_t ncalls;
static int *eno;

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
static bool exists(const char *p) { return access(p, F_OK) == 0; }

/* u-jinxed-0012: feckless's own table, the filter installed with it. */
static void test_table(void)
{
    uint32_t fn = 0, n2 = 0;
    const feckless_umode_call_t *f = feckless_box_calls_v1(&fn);
    CHECK(calls_tab == f && ncalls == fn);                         /* feckless's, as it is */
    CHECK(jinxed_box_calls(&n2) == f && n2 == fn);
}

/* The filter alone. */
static void test_filter_rule(void)
{
    static const char *const yes[] = {
        "jbox_root", "jbox_root/", "jbox_root/a.md", "jbox_root//a", "jbox_root/..notes.md",
        "jbox_root/a/b/.c.h0", "jbox_root/.../x",
    };
    static const char *const no[] = {
        "", "/", "jbox_roo", "jbox_rootx", "jbox_rootx/a", "/jbox_root/a", "../jbox_root/a",
        "jbox_root/..", "jbox_root/../x", "jbox_root/a/..", "jbox_root//..//x", "0:/jinxed/a",
        "elsewhere/new",
    };
    for (size_t i = 0; i < sizeof yes / sizeof *yes; i++) {
        if (!jinxed_box_may_write(yes[i])) { printf("FAIL refused %s\n", yes[i]); failed++; }
    }
    for (size_t i = 0; i < sizeof no / sizeof *no; i++) {
        if (jinxed_box_may_write(no[i])) { printf("FAIL allowed %s\n", no[i]); failed++; }
    }
}

/* doclog.c, inside the root: every call reaches feckless. */
static void test_inside(void)
{
    char *dir = put_str("jbox_root");
    char *twin = put_str("jbox_root/.note.md.h0");
    char *tmp = put_str("jbox_root/note.md.tmp");
    char *exp = put_str("jbox_root/note.md");
    char *wb = put_str("wb"), *rpb = put_str("r+b"), *ab = put_str("ab"), *rb = put_str("rb");
    struct stat *st = box_alloc(sizeof *st);

    CHECK(call("mkdir", P(dir), 0777, 0, 0) == 0 && exists("jbox_root"));
    *eno = 0;
    CHECK(call("mkdir", P(dir), 0777, 0, 0) == M1 && *eno == EEXIST);       /* feckless's errno */

    uint8_t *rec = box_alloc(9000);
    uint32_t x = 1;
    for (int i = 0; i < 9000; i++) rec[i] = (uint8_t)((x = x * 1103515245u + 12345u) >> 16);
    const uint32_t f = call("fopen", P(twin), P(wb), 0, 0);
    CHECK(f != 0);
    CHECK(call("fwrite", P(rec), 1, 9000, f) == 9000 && call("ferror", f, 0, 0, 0) == 0);
    CHECK(call("fflush", f, 0, 0, 0) == 0 && call("fsync", call("fileno", f, 0, 0, 0), 0, 0, 0) == 0);
    CHECK(call("fclose", f, 0, 0, 0) == 0);
    hidden_path[0] = '\0';
    call("storage_mark_hidden", P(twin), 0, 0, 0);
    CHECK(!strcmp(hidden_path, "jbox_root/.note.md.h0"));
    const uint32_t g = call("fopen", P(twin), P(rpb), 0, 0);
    CHECK(g != 0 && call("fseek", g, 100, SEEK_SET, 0) == 0 && call("fwrite", P(rec), 1, 10, g) == 10);
    CHECK(call("fclose", g, 0, 0, 0) == 0);
    const uint32_t h = call("fopen", P(twin), P(ab), 0, 0);
    CHECK(h != 0 && call("fwrite", P(rec), 1, 6, h) == 6 && call("fclose", h, 0, 0, 0) == 0);
    CHECK(call("stat", P(twin), P(st), 0, 0) == 0 && st->st_size == 9006);

    uint32_t e = call("fopen", P(exp), P(wb), 0, 0);
    CHECK(call("fwrite", P(rec), 1, 3, e) == 3 && call("fclose", e, 0, 0, 0) == 0);
    e = call("fopen", P(tmp), P(wb), 0, 0);
    CHECK(call("fwrite", P(rec), 1, 7, e) == 7 && call("fclose", e, 0, 0, 0) == 0);
    CHECK(call("unlink", P(exp), 0, 0, 0) == 0);
    CHECK(call("rename", P(tmp), P(exp), 0, 0) == 0);
    CHECK(call("stat", P(exp), P(st), 0, 0) == 0 && st->st_size == 7);
    struct utimbuf *ut = box_alloc(sizeof *ut);
    ut->actime = ut->modtime = 1000000000;
    CHECK(call("utime", P(exp), P(ut), 0, 0) == 0);
    CHECK(call("stat", P(exp), P(st), 0, 0) == 0 && st->st_mtime == 1000000000);
    const uint32_t r = call("fopen", P(exp), P(rb), 0, 0);
    CHECK(r != 0 && call("fclose", r, 0, 0, 0) == 0);

    char *trash = put_str("jbox_root/trash");
    CHECK(call("mkdir", P(trash), 0777, 0, 0) == 0);
    char *in_trash = put_str("jbox_root/trash/note.md");
    CHECK(call("rename", P(exp), P(in_trash), 0, 0) == 0 && exists("jbox_root/trash/note.md"));
    CHECK(call("remove", P(in_trash), 0, 0, 0) == 0);
    CHECK(call("rmdir", P(trash), 0, 0, 0) == 0 && !exists("jbox_root/trash"));
    /* ".." inside a longer name is a name */
    char *dots = put_str("jbox_root/..notes.md");
    const uint32_t d = call("fopen", P(dots), P(wb), 0, 0);
    CHECK(d != 0 && call("fclose", d, 0, 0, 0) == 0);
    /* storage_io's files, for writing, inside */
    char *io = put_str("jbox_root/io.bin");
    const uint32_t s = call("storage_io_open", P(io), P(wb), 0, 0);
    CHECK(s != 0 && io_open == 1 && call("storage_io_close", s, 0, 0, 0) == 0 && io_open == 0);
}

/* Outside the root: every write refused, nothing made; reads let through. */
static void test_outside(void)
{
    if (mkdir("elsewhere", 0700) != 0) {}
    FILE *o = fopen("elsewhere/kept.txt", "w");
    fputs("kept\n", o);
    fclose(o);

    static const char *const outside[] = {
        "elsewhere/new", "jbox_rootx/a", "jbox_roo", "jbox_root/../elsewhere/new",
        "jbox_root/a/..", "jbox_root//..//elsewhere/new", "../jbox_root/a", "/jbox_root/a", "",
    };
    static const char *const writes[] = { "w", "wb", "a", "ab", "r+", "r+b", "rb+", "w+", "x" };
    struct stat *st = box_alloc(sizeof *st);
    struct utimbuf *ut = box_alloc(sizeof *ut);
    char *kept = put_str("elsewhere/kept.txt");
    char *inside = put_str("jbox_root/note.md");
    for (size_t i = 0; i < sizeof outside / sizeof *outside; i++) {
        char *p = put_str(outside[i]);
        for (size_t m = 0; m < sizeof writes / sizeof *writes; m++) {
            char *mode = put_str(writes[m]);
            *eno = 0;
            CHECK(call("fopen", P(p), P(mode), 0, 0) == 0 && *eno == EACCES);
            *eno = 0;
            CHECK(call("storage_io_open", P(p), P(mode), 0, 0) == 0 && *eno == EACCES);
        }
        *eno = 0;
        CHECK(call("mkdir", P(p), 0777, 0, 0) == M1 && *eno == EACCES);
        CHECK(call("rmdir", P(p), 0, 0, 0) == M1);
        CHECK(call("remove", P(p), 0, 0, 0) == M1);
        CHECK(call("unlink", P(p), 0, 0, 0) == M1);
        CHECK(call("utime", P(p), P(ut), 0, 0) == M1);
        *eno = 0;
        CHECK(call("rename", P(inside), P(p), 0, 0) == M1 && *eno == EACCES);
        CHECK(call("rename", P(p), P(inside), 0, 0) == M1);
        hidden_path[0] = '\0';
        call("storage_mark_hidden", P(p), 0, 0, 0);
        CHECK(hidden_path[0] == '\0');                              /* never reached storage */
    }
    CHECK(!exists("elsewhere/new") && !exists("jbox_rootx") && !exists("jbox_roo"));
    CHECK(io_open == 0);

    /* existing files outside: not removed, renamed, touched or truncated */
    CHECK(call("remove", P(kept), 0, 0, 0) == M1 && call("unlink", P(kept), 0, 0, 0) == M1);
    CHECK(call("rename", P(kept), P(inside), 0, 0) == M1);
    CHECK(call("fopen", P(kept), P(put_str("w")), 0, 0) == 0);
    struct stat hs;
    CHECK(stat("elsewhere/kept.txt", &hs) == 0 && hs.st_size == 5);

    /* reads anywhere */
    const uint32_t r = call("fopen", P(kept), P(put_str("r")), 0, 0);
    CHECK(r != 0);
    char *line = box_alloc(16);
    CHECK(call("fgets", P(line), 16, r, 0) == P(line) && !strcmp(line, "kept\n"));
    CHECK(call("fclose", r, 0, 0, 0) == 0);
    const uint32_t r2 = call("storage_io_open", P(kept), P(put_str("rb")), 0, 0);
    CHECK(r2 != 0 && call("storage_io_close", r2, 0, 0, 0) == 0);
    CHECK(call("stat", P(kept), P(st), 0, 0) == 0 && st->st_size == 5);
    const uint32_t dd = call("opendir", P(put_str("elsewhere")), 0, 0, 0);
    CHECK(dd != 0 && call("readdir", dd, 0, 0, 0) != 0 && call("closedir", dd, 0, 0, 0) == 0);

    /* FatFs: reading passes, any writing or creating mode is refused */
    FIL *fp = box_alloc(sizeof(FIL));
    CHECK(call("f_open", P(fp), P(kept), FA_READ, 0) == FR_OK && fat_open == 1);
    CHECK(call("f_close", P(fp), 0, 0, 0) == FR_OK && fat_open == 0);
    for (uint32_t bit = 0x02; bit <= 0x10; bit <<= 1) {
        CHECK(call("f_open", P(fp), P(kept), FA_READ | bit, 0) == FR_DENIED);
        /* On the board FatFs's paths are "0:/...", which the filter never
         * allows; here the stand-in takes this project's paths, so a
         * drive-form one stands for them. */
        CHECK(call("f_open", P(fp), P(put_str("0:/jinxed/note.md")), bit, 0) == FR_DENIED);
    }
    CHECK(call("f_open", P(fp), P(kept), 0x30, 0) == FR_DENIED);     /* FA_OPEN_APPEND */
    CHECK(fat_open == 0);
    chmod_attr = 0;
    CHECK(call("f_chmod", P(kept), 0x02, 0x02, 0) == FR_DENIED && chmod_attr == 0);

    /* paths that are not strings in the box: EFAULT, and nothing done */
    *eno = 0;
    CHECK(call("mkdir", P("jbox_root/host"), 0777, 0, 0) == M1 && *eno == EFAULT);
    char *edge = (char *)mem + MEM - 4;
    memcpy(edge, "jbox", 4);
    *eno = 0;
    CHECK(call("unlink", P(edge), 0, 0, 0) == M1 && *eno == EFAULT);
    char *lng = box_alloc(400);
    memcpy(lng, "jbox_root/", 10);
    memset(lng + 10, 'a', 300);
    *eno = 0;
    /* u-jinxed-0012: feckless takes paths to 1024 bytes, and this one is
     * under the root, so the filter allows it; the file system refuses a
     * 300-byte name, and the app gets the host's errno for that. */
    CHECK(call("fopen", P(lng), P(put_str("wb")), 0, 0) == 0 && *eno == ENAMETOOLONG);
    /* a mode that is not a string counts as writing: the path decides */
    *eno = 0;
    CHECK(call("fopen", P(put_str("elsewhere/new")), P(edge), 0, 0) == 0 && *eno == EFAULT);  /* feckless refuses the mode first */
    CHECK(!exists("elsewhere/new"));

    /* errno with tp outside the box is not written */
    int *outside_tp = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_32BIT, -1, 0);
    CHECK(outside_tp != MAP_FAILED);
    if (outside_tp != MAP_FAILED) {
        const uint32_t tp = w.ctx.x[4];
        *outside_tp = 77;
        w.ctx.x[4] = P(outside_tp);
        CHECK(call("unlink", P(kept), 0, 0, 0) == M1 && *outside_tp == 77);
        w.ctx.x[4] = tp;
        munmap(outside_tp, 4096);
    }
}

int main(void)
{
    mem = mmap(NULL, MEM, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_32BIT, -1, 0);
    if (mem == MAP_FAILED) { printf("no memory below 4 GB\n"); return 1; }
    feckless_box_init(&box, (uintptr_t)mem, MEM, 4096);
    calls_tab = jinxed_box_calls(&ncalls);
    CHECK(calls_tab != NULL);
    if (!calls_tab) return 1;
    tab = (feckless_umode_calls_t){ calls_tab, ncalls, &box };
    w.lo = (uintptr_t)mem;
    w.hi = (uintptr_t)mem + MEM;
    w.ext_arg = &tab;
    box_lo = w.lo;
    box_hi = w.hi;
    eno = box_alloc(sizeof *eno);                                   /* the thread's errno block */
    w.ctx.x[4] = P(eno);
    test_table();
    test_filter_rule();
    test_inside();
    test_outside();

    /* The app's end forgets the filter: a write outside goes through, and
     * jinxed_box_calls() puts it back for the next load. */
    feckless_box_calls_end();
    const uint32_t f = call("fopen", P(put_str("elsewhere/after")), P(put_str("w")), 0, 0);
    CHECK(f != 0 && call("fclose", f, 0, 0, 0) == 0 && exists("elsewhere/after"));
    uint32_t n3 = 0;
    jinxed_box_calls(&n3);
    *eno = 0;
    CHECK(call("fopen", P(put_str("elsewhere/again")), P(put_str("w")), 0, 0) == 0 && *eno == EACCES);
    feckless_box_calls_end();
    printf(failed ? "%d FAILED\n" : "jinxed_box: all passed\n", failed);
    return failed != 0;
}

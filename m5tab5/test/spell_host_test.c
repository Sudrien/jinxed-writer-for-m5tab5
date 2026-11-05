/*
 * spell_host_test.c -- spell.c on the host, against a small dictionary
 * built by spell_build() the way tools/spelldict.c builds one. Run by
 * run.sh in a scratch directory with SPELL_DIR "sd/spell".
 *
 * Given a path to a full word list (symspell-c99's dictionary.txt), it
 * also builds that and prints the file size, the card reads a lookup
 * takes and a few suggestions, for reading by eye.
 *
 * SPDX-License-Identifier: MIT
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "spell.h"

static int s_fail;

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); s_fail++; } } while (0)

static bool known(const char *w) { return spell_known(w, strlen(w)); }

static int sugg(const char *w, spell_word_t *out)
{
    return spell_suggest(w, strlen(w), out, SPELL_SUGG_MAX);
}

static const char *LIST =
    "# a comment\n"
    "the 1000000\n"
    "they 300000\n"
    "then 200000\n"
    "tea 5000\n"
    "receive 9000\n"
    "recipe 4000\n"
    "spelling 3000\n"
    "spell 6000\n"
    "is 800000\n"
    "do 700000\n"
    "don 2000\n"
    "we 600000\n"
    "can 500000\n"
    "would 400000\n"
    "international 7000\n"
    "internationally 900\n"
    "word 50000\n"
    "words 40000\n"
    "world 60000\n"
    "Hello 20000\n"
    "x1y 5\n"           /* not letters: dropped */
    "a 999999\n"        /* one letter: dropped */
    "the 5\n";          /* listed twice: the higher count stays */

static void write_file(const char *p, const char *s)
{
    FILE *f = fopen(p, "w");
    fputs(s, f);
    fclose(f);
}

static void small(void)
{
    mkdir("sd", 0777);
    mkdir("sd/spell", 0777);
    write_file("list.txt", LIST);

    /* Nothing on the card yet. */
    CHECK(!spell_load());
    CHECK(!spell_loaded());
    CHECK(spell_build("list.txt", "sd/spell/" SPELL_DICT) == 0);
    CHECK(spell_load());

    CHECK(known("the"));
    CHECK(known("The"));
    CHECK(known("HELLO"));
    CHECK(!known("teh"));
    CHECK(known("a"));              /* one letter: never judged */
    CHECK(known("don't"));
    CHECK(known("isn't"));          /* is + n't */
    CHECK(known("we'll"));
    CHECK(known("can't"));
    CHECK(!known("teh's"));
    CHECK(!known("the'x"));

    spell_word_t out[SPELL_SUGG_MAX];
    int n = sugg("teh", out);
    CHECK(n >= 1 && strcmp(out[0], "the") == 0);
    n = sugg("recieve", out);
    CHECK(n >= 1 && strcmp(out[0], "receive") == 0);
    n = sugg("speling", out);
    CHECK(n >= 1 && strcmp(out[0], "spelling") == 0);
    n = sugg("wrod", out);          /* a transposition is one edit */
    CHECK(n >= 1 && strcmp(out[0], "word") == 0);
    n = sugg("internatoinal", out); /* past the prefix */
    CHECK(n >= 1 && strcmp(out[0], "international") == 0);
    n = sugg("zzzzzz", out);
    CHECK(n == 0);
    n = sugg("tehy's", out);        /* the stem, the ending put back */
    CHECK(n >= 2);
    for (int i = 0; i < n; i++) CHECK(strstr(out[i], "'s") == out[i] + strlen(out[i]) - 2);
    CHECK(n >= 2 && (strcmp(out[0], "they's") == 0 || strcmp(out[1], "they's") == 0));

    /* Ranking: distance, then how common. "thn" is one from then, the. */
    n = sugg("thn", out);
    CHECK(n >= 2 && strcmp(out[0], "the") == 0 && strcmp(out[1], "then") == 0);

    char c[SPELL_WORD_MAX + 1] = "the";
    spell_case_like("Teh", 3, c);
    CHECK(strcmp(c, "The") == 0);
    strcpy(c, "the");
    spell_case_like("teh", 3, c);
    CHECK(strcmp(c, "the") == 0);

    /* Ignore all lasts the session; add lasts. */
    CHECK(!known("jinxed"));
    spell_ignore("Jinxed", 6);
    CHECK(known("jinxed"));
    CHECK(!known("feckless"));
    CHECK(spell_add("Feckless", 8));
    CHECK(known("feckless"));
    spell_reset();
    CHECK(spell_load());
    CHECK(!known("jinxed"));
    CHECK(known("feckless"));

    /* A different dictionary on the card: no suggestions from a word
     * list that does not match it, and a reload next time. */
    write_file("list2.txt", "other 5\nwords 4\n");
    CHECK(spell_build("list2.txt", "sd/spell/" SPELL_DICT) == 0);
    n = sugg("teh", out);
    CHECK(n == 0);
    CHECK(!spell_loaded());
    CHECK(spell_load());
    CHECK(known("other") && !known("the"));

    /* A torn file is refused. */
    CHECK(spell_build("list.txt", "sd/spell/" SPELL_DICT) == 0);
    CHECK(truncate("sd/spell/" SPELL_DICT, 1000) == 0);
    spell_reset();
    CHECK(!spell_load());
}

static bool next(const char *t, uint32_t from, const char *want, uint32_t *at)
{
    uint32_t a, b;
    if (!spell_next_word(t, (uint32_t)strlen(t), from, &a, &b)) return want == NULL;
    if (!want) return false;
    *at = b;
    return b - a == strlen(want) && memcmp(t + a, want, b - a) == 0;
}

static void words(void)
{
    const char *t = "I don't see x2 NASA's iPhone at /usr/bin, me@ex.com `code` "
                    "\\section here 'quoted' end";
    uint32_t at = 0;
    CHECK(next(t, at, "don't", &at));
    CHECK(next(t, at, "see", &at));
    CHECK(next(t, at, "at", &at));      /* past x2, NASA's, iPhone */
    CHECK(next(t, at, "here", &at));    /* past the path, address, code, TeX */
    CHECK(next(t, at, "quoted", &at));  /* apostrophes at the ends are quotes */
    CHECK(next(t, at, "end", &at));
    CHECK(next(t, at, NULL, &at));
    /* From inside a word, the whole word. */
    CHECK(next("hello world", 8, "world", &at));
    CHECK(next("don't", 4, "don't", &at));
}

static void full(const char *path)
{
    spell_reset();
    if (spell_build(path, "sd/spell/" SPELL_DICT) != 0 || !spell_load()) {
        s_fail++;
        return;
    }
    struct stat st;
    stat("sd/spell/" SPELL_DICT, &st);
    printf("full dictionary: %lld bytes on the card\n", (long long)st.st_size);
    static const char *const probe[] = { "teh", "recieve", "speling", "definately",
                                         "accomodate", "seperate", "wierd", "writting",
                                         "becuase", "untill", "occured", "thier" };
    unsigned worst = 0;
    for (size_t i = 0; i < sizeof(probe) / sizeof(probe[0]); i++) {
        spell_word_t out[SPELL_SUGG_MAX];
        spell_reads = 0;
        const int n = sugg(probe[i], out);
        if (spell_reads > worst) worst = spell_reads;
        printf("  %-12s %2u reads:", probe[i], spell_reads);
        for (int k = 0; k < n && k < 4; k++) printf(" %s", out[k]);
        printf("\n");
    }
    printf("  the most reads for one word: %u\n", worst);
}

int main(int argc, char **argv)
{
    small();
    words();
    if (argc > 1) full(argv[1]);
    spell_reset();
    if (s_fail) {
        fprintf(stderr, "%d failed\n", s_fail);
        return 1;
    }
    printf("spell: all passed\n");
    return 0;
}

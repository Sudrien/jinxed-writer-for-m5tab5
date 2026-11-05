/*
 * spelldict.c -- build the spelling dictionary for the card, on a
 * computer. See main/spell.h.
 *
 *   cc -O2 -DSPELL_HOST -Imain -o spelldict tools/spelldict.c main/spell.c
 *   ./spelldict dictionary.txt english.sym
 *
 * The input is one word per line, optionally followed by how common it
 * is ("the 226129150"); lines starting '#' are skipped, and words that
 * are not all letters, or are one letter long, are dropped. The 86,060
 * word list in symspell-c99's dictionaries/dictionary.txt
 * (github.com/sumanpokhrel-11/symspell-c99) is that format. Copy the
 * output to jinxed/spell/english.sym on the card.
 *
 * SPDX-License-Identifier: MIT
 */
#include <stdio.h>

#include "spell.h"

int main(int argc, char **argv)
{
    if (argc != 3) {
        fprintf(stderr, "usage: %s word-list.txt english.sym\n", argv[0]);
        return 2;
    }
    return spell_build(argv[1], argv[2]);
}

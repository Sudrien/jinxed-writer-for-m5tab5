# Credits

Jinxed Writer's own code is under its LICENSE. It also rests on work
by others, credited here.

## Spelling (m5tab5/main/spell.c, m5tab5/tools/spelldict.c)

- **SymSpell**, the symmetric-delete spelling algorithm, by Wolf Garbe
  ([wolfgarbe/SymSpell](https://github.com/wolfgarbe/SymSpell), MIT
  licence, copyright 2018 Wolf Garbe). `spell.c` implements the
  algorithm as Garbe published it; it contains none of his code.
- **symspell-c99**, by Suman Pokhrel
  ([sumanpokhrel-11/symspell-c99](https://github.com/sumanpokhrel-11/symspell-c99),
  MIT licence, copyright 2025 Suman Pokhrel). `spell.c` follows its
  C port's approach (prefix 7, distance 2, Damerau-OSA, ranking by
  distance then frequency); it contains none of its code. Its
  misspelling test sets were used to measure the checker, and are not
  part of this repository.

## The dictionary

The dictionary the board downloads, `english.sym`, is not in this
repository. It lives in
[jinxed-writer-spellcheck-dictionaries](https://github.com/Sudrien/jinxed-writer-spellcheck-dictionaries),
with the notices of everything it is made from: symspell-c99's word
list, Wolf Garbe's frequency dictionary, SCOWL (Kevin Atkinson and the
authors it credits), Google Books Ngram data (Google, CC BY 3.0) and
English Wikipedia's word frequencies. See that repository's README and
`licenses/`.

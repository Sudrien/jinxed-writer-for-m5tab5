# Jinxed Writer

A writerdeck for the M5Stack Tab5: a USB keyboard, a dim screen, every
word kept on the SD card, and nothing else. Standalone ESP-IDF 6.1
firmware on the feckless-libs-for-m5tab5 components.

Not ready for human consumption yet. `writerdeck_spec.md` is the design;
`CLAUDE.md` is how patches are made.

## What works today

- **Writing.** One file a day, `jinxed/YYYY-MM-DD.md` on the card, opened
  at the end. The keys are in the spec, section 5:
  Word's on a PC keyboard (Ctrl, or the Windows key), the Mac's on an
  Apple one (Command and Option). Below, Ctrl is Command on an Apple
  keyboard.
- **Nothing lost.** The document is a log of every edit, written twice
  (`.YYYY-MM-DD.md.h0` and `.h1`) and recovered from whichever twin is
  longer after a power cut; the `.md` is regenerated from it. Each edit
  says whether you made it or the spelling check did.
- **Spelling, on demand.** F7 or Ctrl+; walks the document from the cursor:
  1-9 or Enter take a suggestion, E types the word yourself, A adds it to
  `jinxed/spell/words.txt`, I ignores it until power-off. No marks while
  you type.
- **The dictionary** (`jinxed/spell/english.sym`, 15 MB) is downloaded on
  the Tab5 itself when it is missing: press D in the spelling view. It
  comes from
  [jinxed-writer-spellcheck-dictionaries](https://github.com/Sudrien/jinxed-writer-spellcheck-dictionaries)
  over Wi-Fi, resumes where it stopped, and is checked before it is used.
- **Settings** (the gear, or Ctrl+,): the email account, a test email,
  which way up the screen is, the colours, the text size (also Ctrl+=
  and Ctrl+- while writing), all kept across power-offs; and the library
  versions and licences.

## Not yet

Scheduled and background email sends, sleep with the RTC alarm, idle dim
and power-off, file mode, syntax highlighting, setting the clock. The spec describes each.

## Building

From `m5tab5/`: `idf.py build flash`. After a change to
`sdkconfig.defaults`, `rm sdkconfig` first. Host tests for the log and the
spelling check: `./test/run.sh`.

## Credit

SymSpell, its C port and the dictionary's sources: `CREDITS.md`. MIT
licence: `LICENSE`.

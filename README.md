# Jinxed Writer

A writerdeck for the M5Stack Tab5: a USB keyboard, a dim screen, every
word kept on the SD card, and nothing else. Standalone ESP-IDF 6.1
firmware on the feckless-libs-for-m5tab5 components.

Not ready for human consumption yet. `writerdeck_spec.md` is the design;
`CLAUDE.md` is how patches are made.

## What works today

- **Writing.** One file a day, `jinxed/YYYY-MM-DD.md` on the card, opened
  at the end. The keys are in the spec, section 5:
  Windows' on a PC keyboard (Ctrl; the Windows key does nothing), the
  Mac's on an Apple one (Command and Option). Shift with any movement
  selects; Cmd+A, and Cmd+C / X / V with a clipboard kept until power-off.
  Cmd+Z undoes and Cmd+Shift+Z (or Ctrl+Y) redoes; Tab and Shift+Tab
  indent; Cmd+F finds, F3 (or Cmd+G) again.
- **Nothing lost.** The document is a log of every edit, written twice
  (`.YYYY-MM-DD.md.h0` and `.h1`) and recovered from whichever twin is
  longer after a power cut; the `.md` is regenerated from it a few
  seconds after typing stops, or on Ctrl+S (Command+S). Edited on a
  computer meanwhile, it is left alone and `YYYY-MM-DD.conflict.md` written
  beside it. The export ends in a footer: who wrote it, if you set a name
  in Settings, when it was exported, and the Jinxed build. Each edit
  says whether you made it or the spelling check did.
- **Spelling, on demand.** F7 (or Command+; on an Apple keyboard) walks the document from the cursor:
  1-9 or Enter take a suggestion, E types the word yourself, A adds it to
  `jinxed/spell/words.txt`, I ignores it until power-off. No marks while
  you type.
- **The dictionary** (`jinxed/spell/english.sym`, 15 MB) is downloaded on
  the Tab5 itself when it is missing: press D in the spelling view. It
  comes from
  [jinxed-writer-spellcheck-dictionaries](https://github.com/Sudrien/jinxed-writer-spellcheck-dictionaries)
  over Wi-Fi, resumes where it stopped, and is checked before it is used.
- **Notes** (Ctrl+O, Command+O): every note in `jinxed/`, newest first, to open,
  make, rename or put in `jinxed/trash/`. Settings, Mode: Daily opens
  today's at power-on, File the one last opened.
- **Markdown**, drawn as you type: bold, italic, strikethrough, `code`
  and headings, the markers kept and dim. Off in Settings, Highlighting.
- **F1** shows the keys for the keyboard plugged in, **F11** a bare page
  (no word count, no gear), **F12** page stats.
- **Settings** (the gear, Ctrl+, or Command+,): the email account, a test email,
  the name your edits are credited to, which way up the screen is, the colours, the text size (also Ctrl+=
  and Ctrl+-, or Command, while writing), all kept across power-offs; and the library
  versions and licences.

- **Email now:** Ctrl+Enter (Command+Enter), or Send notes now in
  Settings, mails today's text to the account set up there: in the body,
  and attached as the day's `.md` with its log (`.md.h0`) beside it.

## Not yet

Scheduled and background email sends, sleep with the RTC alarm, idle dim
and power-off, the highlight-mode picker and TeX, setting the clock. The spec describes each.

## Building

From `m5tab5/`: `idf.py build flash`. After a change to
`sdkconfig.defaults`, `rm sdkconfig` first. Host tests for the log and the
spelling check: `./test/run.sh`.

## Credit

SymSpell, its C port and the dictionary's sources: `CREDITS.md`. MIT
licence: `LICENSE`.

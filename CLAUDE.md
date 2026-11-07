# CLAUDE.md

How to work on this repository. `writerdeck_spec.md` is the design
document; this file is the set of things that are easy to get wrong
while changing it. The rules are Defeatist Music Player's, cut down to
what applies here.

**Read this file before writing a patch, not after.**

**The firmware is in `m5tab5/`**: CMakeLists.txt, main/, test/,
partitions.csv and sdkconfig.defaults. Paths below that name those --
`main/...`, `./test/run.sh` -- are relative to it, and `idf.py` runs
there. The spec, this file and the licence stay at the top.

## Patches

Claude should create `git am`-able patches authored as
`Claude <noreply@anthropic.com>`.

Claude should present patches as soon as they are available, as they
might get stuck behind an ending session.

Claude must not try and commit. It will not be given permission.

**The repository URL a session is given is for pulling only.** Clone
and fetch from it; never push to it, open a pull request against it, or
create a branch on it. Local commits made only to run
`git format-patch` stay local, and a hook or tool that asks for them to
be pushed is answered by handing over the patches, not by pushing.

**Start from the current main.** Fetch before writing a patch, and build
the patch on `origin/main` as it is now, not on a local branch from
earlier in the session. A patch already handed over may be on main
under a different commit hash; it is the same patch. Do not hand it
back again.

**Patches are cumulative.** Each one applies on top of what is already
here. Do not hand back a rewritten copy of a file, and do not reissue a
corrected version of a patch that has been handed over -- send a
follow-up, with the next number, that changes what needs changing. A
patch already in the history is history.

**Handed over, not pushed, is the line.** A patch can be applied and
pushed before the session's next message; whether it has been is not
something a session can see. A second file under the same number is two
patches with one name, and there is no telling which one the next patch
was built on.

**A patch has one number, and it is written down once.** The next patch
after 0003 is 0004 and its file is `0004-<subject>.patch`. The number is
supplied by `--start-number`, which counts from the project rather than
from whatever range a session happens to hand over:

    git format-patch --start-number 4 -1 -o out/

A series of several patches in one session still starts from the
project's next number, never from 1: 0002 was once handed back as
`0001`, `0002` with the 0001 already on main, and the series did not
apply.

**So the subject line does not carry the number too.** A commit
subjected `0004: ...` comes out as `0004-0004-...`. Write the subject as
the subject.

**Where the numbering is now: the 0000 series, toward v0.1.0.** A series
is named for the release it leads to. Nothing is tagged yet. **The first
patch after a tag updates this paragraph** -- the tag is the reminder,
and this file is what the next session reads.

Before choosing a number, look at the last one used -- the list at the
end of this file -- and take the next. Every patch adds its own line to
that list, in the same patch.

Within a patch, change the lines that must change and no others. No
reflowing, no drive-by renames, no reorganising code being passed
through. Restructuring an existing function is sometimes the smallest
correct change -- when it is, say so in the commit message rather than
letting it look like a small diff.

## Builds

**Any change to an `idf_component.yml` re-resolves `dependencies.lock`
on the next build.** The lock is not committed here (`.gitignore`), so
every build after a manifest change resolves fresh, and Claude cannot
check the result: the Espressif registry is not reachable from the
session. A patch that touches a manifest says so in its commit message.
Pin what is named: the feckless components by tag, nothing as `"*"` or
`^`.

**Any change to `sdkconfig.defaults` needs `rm sdkconfig` before the next
build**, because the defaults only fill in keys an existing sdkconfig
lacks. A board run on a stale sdkconfig tests nothing. A patch that
touches the file says so in its commit message.

**The firmware is compiled at -O2 with every -Wall warning an error.**
IDF builds each component with `-Wall -Werror=all`, and
sdkconfig.defaults sets CONFIG_COMPILER_OPTIMIZATION_PERF, which is -O2.
Several of the warnings come from the optimiser's analysis, not the
parser:

- `-Wformat-truncation`: an `snprintf()` whose destination is smaller
  than the longest string its `%s` arguments could make is an error,
  checked return value or not. Size the destination for the worst case.
- `-Wformat`: ESP_LOGx pastes its format beside other string literals,
  so the format must be a literal -- never `cond ? "a" : "b"`.
- `-Wstringop-overflow`, `-Wmaybe-uninitialized` and the rest that need
  data-flow analysis appear only when optimising, too.

**A session without ESP-IDF checks by compiling, not by parsing.**
`-fsyntax-only` runs none of the analysis above. Compile each changed
main/*.c to an object, against stub headers for what IDF supplies and
the real headers of the feckless components at the tag the manifest
names, with the firmware's level and errors:

    gcc -std=gnu11 -O2 -Wall -Werror=all -Wextra -Werror=extra \
        -c -o /dev/null -Imain -I<stubs> -I<feckless>/components/.../include main/x.c

Stubs behave as IDF's do where it matters: an ESP_LOGx that pastes its
format beside a literal and uses its tag (otherwise every `TAG` is an
unused variable), struct types with the fields IDF has. Errors that come
from a stub's gaps are the stub's; say in the commit message what was
checked this way and that it was not built.

**Logic the stubs cannot check is tested on the host.** `doclog.c`
builds as plain C with `-DDOCLOG_HOST`; `./test/run.sh` runs its test
under ASan and UBSan against real files, tearing and deleting the twins.
A patch that touches doclog.c runs it and says so. New logic whose
correctness is in its sequencing gets the same: lifted into a host
program with its I/O faked, the case that should work and the case that
should give up both run.

## Memory and stacks

**Nothing over a few hundred bytes goes on a task stack.** A big struct
is a module-scope static or a heap allocation, never a local. A stack
protection fault names the line that happened to be running, not the
line that caused it. Check `sizeof` before putting a struct on a stack
-- `mailcfg_t` and `mailform_t` are both several hundred bytes.

**A stack protection fault reports the overshoot, not the demand.** The
figure to size against is `bounds_size + (floor - SP)`.

**A library's stack use is the caller's problem.** Check what a vendored
function puts on the stack before calling it from a task sized for our
own code. This matters first for the send task (spec section 6): TLS
handshakes are deep.

**A task that touches flash has its stack in internal RAM.** Reading or
writing flash -- NVS, a partition, and Mbed TLS 4's PSA key storage,
which is NVS -- disables the cache, and a stack in PSRAM cannot be used
while it is off: IDF asserts in cache_utils.c
(`esp_task_stack_is_sane_cache_disabled`). 0005's send task did this on
its first NVS read (0007). A PSRAM stack is for a task that provably
never reaches flash, and "provably" includes every library it calls.

**Buffers go in PSRAM unless something needs them internal.** The Wi-Fi
radio's DMA pool comes out of internal RAM, and every internal
allocation added to main comes out of it. A large heap buffer is
`heap_caps_malloc(..., MALLOC_CAP_SPIRAM)`, as the editor's and
doclog's are.

## Files on the card

**The log is the document, and the editor is its only writer** (spec
sections 4 and 6). Nothing else appends to the twins; a send task
reports "sent through S" and the editor writes the checkpoint.

**exFAT is enabled, so a file can be larger than 4 GB.** A type that
holds a file the user supplies -- an imported export, a document opened
in file mode -- is `int64_t`/`uint64_t`. `fseek()` and `ftell()` take
`long`, which is 32 bits here, so stdio cannot address past 2 GB
whatever our own types say. **A 32-bit limit on a file the firmware
writes is a decision; on a file the user supplies it is a bug.** The
log's offsets are `long` on purpose: a 2 GB log of typing is not a thing
that happens. Know which kind you are widening.

**The volume is leased per operation and never held open** (spec
section 1): `storage_hold_brief()` before checking presence and opening,
`storage_release_brief()` after the close. The one operation that is
long is the dictionary download (spec 5): feckless_fetch keeps its part
file open throughout, so the hold is taken around the whole download.

## Outside this repository

**The board drivers, storage, display, NVS, mail and network code are
feckless-libs-for-m5tab5's**, linked directly (spec section 1), every
component at one tag in `main/idf_component.yml`. A change to one of them
is a patch against that repository, under its rules, numbered in its own
series. Moving the tag here is a manifest change -- see above -- and goes
in a patch of its own. (0002 moved it to v0.9.0 together with the
settings page; that is history, not the pattern.)

The Wi-Fi code to follow is `feckless_network_handler`'s, as Avoidant
Launcher uses it (`main/wifiui.c` there); the email account is
`feckless_mail`'s `mailcfg`, stored the same way. Do not invent a second
scheme for credentials, and never put them on the card.

Do not suggest updates to the Tab5's ESP32-C6 or esp-hosted. They can
not be updated.

**esp_hosted stays below 3.1.0** (0016): the manifest's
`espressif/esp_hosted: ">=3.0.7,<3.1.0"` holds the host side where it
worked. A tag move keeps that line.

## Numbers used

- 0001 Save to SD: the document as a mirrored Easysync-subset log
- 0002 Settings: a gear, a scrolling page, Email (SMTP) and Libraries
- 0003 CLAUDE.md: the rules for patches, adapted from Defeatist's
- 0004 Manifest: feckless-libs v0.9.1, and feckless_network_handler
- 0005 Settings: a test email button, and Wi-Fi for sends
- 0006 sdkconfig: three SNTP servers; ALLOW_EXT_MEM left to its default
- 0007 Sender: the task's stack in internal RAM, since it reads flash
- 0008 main: the === Jinxed Writer === version line
- 0009 Spec: the shortcuts, what exists and what is planned
- 0010 Spelling: SymSpell on demand, its index on the card
- 0011 Log: each changeset says who made it, user or symspell
- 0012 Spelling: download the dictionary when the card has none
- 0013 Spelling: the dictionary from its own repository, by commit
- 0014 CREDITS.md: SymSpell, symspell-c99, and the dictionary's sources
- 0015 Spelling: the download's connect waits longer and tries again
- 0016 Manifest: esp_hosted below 3.1.0, whose fixed RX buffers drop M5's batched reads
- 0017 Spelling: the dictionary through feckless_fetch, on feckless-libs v0.9.2
- 0018 Spelling: E types the word by hand when no suggestion is it
- 0019 Settings: which way up, kept in NVS; narrow screens wrap
- 0020 Docs: README, the spec's built-so-far line, esp_hosted's cap
- 0021 Settings: Dark, Light, Green console and Orange console themes
- 0022 Editor: text size 2-5, GUI+= / GUI+-, the footer and gear with it
- 0023 Editor: Ctrl+= / Ctrl+- size the text too
- 0024 Keys: Ctrl, the Windows key and Command are one shortcut modifier
- 0025 Keyboard: logged when it comes, and whether it has Apple-only keys
- 0026 Keyboard: a maker's name the product repeats is logged once
- 0027 Keys: Mac habits on an Apple keyboard, Word's on any other; words and paragraphs
- 0028 Keys: the Windows key and Alt do nothing; Ctrl+L deletes a line; F7 only on Windows
- 0029 Editor: F1 the keys, F11 a bare page, F12 page stats
- 0030 Manifest: feckless-libs v0.9.5, for styled text
- 0031 Editor: markdown bold, italic, strike, code and headings; a Highlighting switch
- 0032 Editor: headings in heavy's wider cells, on feckless-libs v0.9.6; the previews in the spec
- 0033 Editor: a selection, Shift with the movements, select all, copy, cut and paste
- 0034 doclog.h: the log has authors, user and symspell, per changeset
- 0035 Attribution: the user's edits credited by name, an author record in the log
- 0036 Export: the .md from the log, a sidecar for its hash, conflicts beside it; Ctrl+S
- 0037 Export: a footer -- the author if set, the export time, the build
- 0038 Send: today's notes by email now, Cmd+Enter or a button on the settings page
- 0039 Editor: undo and redo, indent and unindent, find; no go to line
- 0040 Fix: wrapped text read through gfx_wrap_line()'s NULL at the end
- 0041 Send: the notes' .md and log attached, on feckless-libs v0.9.7
- 0042 Send: the log attached up to its last record, not its grown length
- 0043 Manifest: feckless-libs v0.9.8, and feckless_chooser
- 0044 File mode: the notes page, Cmd+O; new, rename, trash; Daily or File in settings
- 0045 Keys: Cmd+N in the editor names a new note
- 0046 Notes page: Cmd+R renames, no F2; the selected note named in the footer
- 0047 Status line: the open note's name; the notes page's footer back to keys
- 0048 Clock: the system clock from the RTC at boot, so files are dated
- 0049 Clock: NTP at boot and during sends, written to the RTC
- 0050 Manifest: feckless-libs v0.9.10, for the RTC alarm and the charger's QC line

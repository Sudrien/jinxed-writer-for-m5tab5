# The `umode` branch

Work toward running Jinxed's editor in a feckless box: RISC-V user mode,
in one block of PSRAM that the PMP grants it and nothing else, every call
out of it an `ecall` the host checks (feckless-libs' `umode` branch,
`u-feckless-0001` to `0044`; Avoidant's, `u-avoidant-0001` to `0003`).
Numbered `u-jinxed-NNNN` (CLAUDE.md), applied to the `umode` branch.
Nothing here changes the firmware main builds.

Each patch adds a section below, headed with its number: what it does,
how it was checked, and once a board has run it, what the log said.

## u-jinxed-0001 -- opens the branch: what is known, what is not

Documentation only; no code, nothing built.

**Where the box is** (feckless-libs at 27229f8, Avoidant at 77601e2,
Lothesome's and Defeatist's `umode` branches; read, not run). Spurious
and Lothesome run boxed, from Avoidant and from their own standalone
.bins, which link the same kernel (`feckless_umode`): the .bin embeds
the app's ELF, loads it with `feckless_umode_load()` against
`feckless_box_calls_v1()`'s table, and runs it with
`feckless_umode_app_run()` (Lothesome's `main/standalone.c`, its
u-lothesome-0001). An ELF is boxed if it imports `feckless_abi_box_v1`,
which `feckless_abi_box_elf()` makes it do. `feckless_box_libc` links the
pure part of libc, float and double maths and 64-bit division into the
ELF. `feckless_box_calls` answers about 140 names, written for what
Spurious, Lothesome and Defeatist's boxwin call.

**Where Jinxed is.** Unlike those apps, Jinxed has never been an ELF. It is
standalone firmware that links the feckless components directly (spec
section 1: "Do not depend on the ELF-launcher ABI"), on v0.9.10. The
umode branch is cut from v0.10.2. Between the two, no header Jinxed
includes changes except for comments (IDF 5.x support was removed;
picolibc is IDF 6's libc). So moving the pins is a manifest change and
not a code change. That was found by diffing the headers, not by a
build.

**The shape it would take.** The box is for the code that holds
the user's text and parses what is on the card. The trusted side keeps
what needs the hardware or secrets:

- *Boxed, the ELF:* `editor.c`, `doclog.c`, `md.c`, `spell.c`, `fname.c`,
  `files.c`'s notes page, and `display.c`'s drawing.
- *Host, the .bin:* `kbd.c` and `hidrpt.c` (raw `usb_host_*`, a client
  task, a queue), `sender.c` (Wi-Fi, SMTP, the dictionary download, a
  static task woken by notifications), `settings.c`'s email form (the
  credentials stay in NVS and never enter the box), `power.c` (the RTC
  alarm, power-off, the battery), and `main.c` as the loader, as
  Lothesome's `standalone.c` is.

The editor would run on core 1 (u-feckless-0031), with USB host and
Wi-Fi on core 0. That way a window that holds interrupts off cannot stall
the keyboard.

**What stands between the editor and a box**, in the order it would bite.
These counts come from matching names against `main/`'s source text, not
from a link:

- *Storage writes.* The box reads only: `f_open`/`f_read`/`f_lseek`,
  `fopen`/`fread`/`fgets`/`fseek`, `opendir`/`readdir`. The log is
  appended on every keystroke and exported on a timer. So the editor needs
  `fwrite` (9 sites), `fprintf` (8), `fflush` and `fsync` (2 each),
  `rename` (3), `unlink` (2), `mkdir` (2) and `stat` (3), plus
  `storage_hold_brief`/`storage_release_brief` (3 each) and
  `storage_list.h` for the chooser. All of them are in the ABI's storage
  section already; none has a box handler. This is the first feckless
  request.
- *Styled text.* `gfx_draw_text_style()` and `gfx_text_w_style()` (the
  markdown highlighting, 0031/0032) are not in the box's gfx calls.
- *Calls of Jinxed's own.* The editor calls into the host's modules:
  `kbd_next()` for keys, `sender_*` to start a send and read its result,
  `power_*` for idle and wake, `settings_*`, `battery_*`, `brightness_*`,
  `rtc8130_*`. `feckless_umode_load()` takes any table, so the .bin could
  append its own handlers to `feckless_box_calls_v1()`'s. But the ELF's
  build check (`feckless_abi_box_elf()`'s abicheck) accepts only names in
  `exports.txt` and `feckless_abi/sections/`. Either feckless-libs gives
  an app a way to declare a section of its own, or every one of these
  becomes a feckless ABI name. That is feckless-libs' decision, and it
  is the second request.
- *Waiting.* The box has no queue, semaphore or task notification. The
  editor's loop blocks on `kbd_next(ev, ticks)`. If the host answers that
  as a call that waits, unlocked like the audio reads, no lock is needed
  inside the box. The editor's own code takes no lock and has no critical
  section (`kbd.c` and `sender.c` do, and they stay on the host).
- *Windows hold interrupts off.* `CONFIG_FECKLESS_UMODE_PREEMPTIBLE`
  defaults off: on a Tab5 it stopped core 0's tick (u-feckless-0016,
  0017). Two of the editor's stretches compute without calling out. One
  is a spelling lookup against the 15 MB index. The other is regenerating
  the export from a long log. Both would hold core 1's interrupts for
  their length. That is tolerable with the host on core 0, but it should
  be measured before anything else is built on it.
- *Odds and ends* with no box handler: `esp_random` (doclog's changeset
  ids), `esp_app_get_description` (the footer and the version line),
  `settimeofday` (host side, after NTP). `heap_caps_malloc(...,
  MALLOC_CAP_SPIRAM)` works: in a box it is the box's heap whatever the
  caps say.
- *The box's size.* The editor's buffers and the spelling index live in
  PSRAM today (CLAUDE.md, "Buffers go in PSRAM"). In a box they come out
  of the one block, at most 16 MB in Lothesome's loader. The index
  is read from the card in pieces, so this should fit, but it is not
  measured.

**Intended order**, to be revised by what the board says:

1. `u-jinxed-0002`: the feckless pins move from `v0.9.10` to `umode`,
   and `feckless_umode`, `feckless_box_calls` and `feckless_box_libc` are
   added. Following Lothesome, the branch pins feckless-libs' branch
   rather than a tag, and main's manifest stays on its tag. Nothing is
   boxed yet. A board run should match main's.
2. `CONFIG_BOOTLOADER_REGION_PROTECTION_ENABLE=n` and a second build
   target, `m5tab5/elf/`, which builds the boxed modules as an ELF for
   the .bin to embed. It starts with `doclog.c` and `md.c`, which need
   nothing but storage, behind a test entry that replays a log. This is
   the measurement: how long their windows are, and whether a log
   written in a box replays byte for byte as main's does.
3. The feckless requests above, as `u-feckless` patches against that
   repository: storage writes, styled text, and a section for an app's
   own calls.
4. The editor in the box, a module at a time, with the host's calls
   behind one table.

Releases stay on main until a boxed editor matches the trusted one: the
same twins, the same export, and no key lost while a window runs.

*Not checked:* anything above on a board, or built with ESP-IDF.

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

1. `u-jinxed-0002`: the feckless pins move from `v0.9.10` to `umode`.
   Following Lothesome, the branch pins feckless-libs' branch rather
   than a tag, and main's manifest stays on its tag. Nothing is boxed
   yet. A board run should match main's. (0002 left the three box
   components out; they come with the patch that requires them.)
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

## u-jinxed-0002 -- feckless-libs' umode branch in place of v0.9.10

`m5tab5/main/idf_component.yml`: all twelve feckless pins move from
`v0.9.10` to `umode`, feckless-libs' branch at 27229f8 when this was
written: v0.10.2 with u-feckless-0001 to 0044. The comment says why.
The esp_hosted cap (0016) and the IDF floor (6.1) are unchanged.
`feckless_umode`, `feckless_box_calls` and `feckless_box_libc` are not
added yet. With `MINIMAL_BUILD` nothing would build them until main
requires them, and they come in with the patch that does.

What the move brings to this firmware, from the diff of v0.9.10 against
27229f8 outside the three box components:

- IDF 5.x support removed throughout (feckless-libs 4f2ce99). This
  firmware was already 6.1 only.
- `usb_host_msc` and `usb_host_uac` require `espressif/usb` without the
  version rule, and `usb_host_msc` always builds its `esp_blockdev`
  diskio.
- The storage handler's mount poll is quiet. While the slot is empty, the
  SD driver's per-attempt errors and SD_HOST's INFO line are turned off
  around each attempt and then put back (u-feckless-0018, 0019). The boot
  mount still logs.
- gfx's blit timing (u-feckless-0032, 0033), behind a Kconfig option
  that defaults off.
- No header this firmware includes changes, apart from comments
  (`wifi.h`, `feckless_cert.h`).

**This patch touches the manifest:** the next build re-resolves
`dependencies.lock` (not committed here), fetching the branch's head at
that moment. A later push to feckless-libs' umode branch changes what a
fresh resolve gets.

*Checked:* the diffs above, read. *Not checked:* an ESP-IDF build, since
the registry cannot be reached from the session; a board.

*What to look for on a Tab5:* the Libraries section of the settings page
lists the feckless components at 0.10.2. Everything else should be as
on main, with no `send_op_cond` errors repeating while the card is out.

## u-jinxed-0003 -- the card for a boxed editor, from Jinxed's own .bin

0001 counted storage writes as the first request to feckless-libs. They
need no feckless change. `feckless_umode_load()` takes any table, and
every name doclog.c and spell.c write with is already in the ABI's
storage section (or `exports.txt`, for `esp_random`). Only the box's
answers lack them. So Jinxed's .bin answers them itself:
`m5tab5/components/jinxed_box`.

- `jinxed_box_calls()`: Jinxed's own calls, then `feckless_box_calls_v1()`'s
  without the names Jinxed answers. That way each name appears once and
  nothing depends on which entry the loader takes first.
- Its calls (`answers.txt`): `fopen`, `fclose`, `fread`, `fwrite`, `fgets`,
  `fseek`, `ftell`, `fflush`, `ferror`, `fileno`, `fsync`, `stat`, `mkdir`,
  `rename`, `unlink`, `storage_hold_brief`, `storage_release_brief`,
  `storage_mark_hidden`, `esp_random`. It answers every FILE call, not
  only the writes, so one table owns every FILE the app holds. A
  feckless FILE token handed to a Jinxed call is refused.
- In feckless' manner (u-feckless-0007, 0015): FILE and the fd that
  `fileno()` gives are numbers. Every pointer is checked for its whole
  length. Reads *and* writes go through one internal staging buffer in
  8 KB slices, because a write that DMAs out of the box's PSRAM disturbs
  the panel just as a read into it does.
- Beyond it: paths and modes are copied out of the box before they are
  checked. A path must be `JINXED_BOX_ROOT` (`/sd/jinxed`) or below it,
  with no `..` segment; anything else gets `EACCES`. A failed call sets the
  app's errno at its `tp`, which doclog reads after `mkdir()` and
  `rename()`. Brief holds are counted per volume, so the app releases
  only its own, and `jinxed_box_end()` closes what it left open and gives
  back the rest.
- Not restricted: what stays feckless's, which only reads. `opendir`,
  `readdir` and FatFs's `f_open` can still read the whole card.

Not yet answered, for the editor's modules: `fprintf` (spell.c's word
list, one `"%s\n"`). It belongs in the ELF as `vsnprintf` into `fwrite`,
not on the host: a host `fprintf` would walk a format whose `%s` is the
app's pointer. That comes with the ELF's build.

Nothing requires the component yet, so under `MINIMAL_BUILD` the
firmware does not build it: no manifest or sdkconfig change.

*Checked:* `make -C m5tab5/components/jinxed_box/test FECKLESS=<feckless-libs
at 27229f8>` on the host under ASan and UBSan, against feckless's own
`box_calls.c`, `box_storage.c`, umode's table, box and ext hook, and
feckless_box_calls' stand-ins, over real files:
- the table: no name twice, every feckless name still answered, the
  kept entries feckless's own, `answers.txt` exactly Jinxed's 19;
- doclog's sequences: mkdir and EEXIST through errno; a twin created,
  fsynced through `fileno`, grown with zeros, 20000 bytes written at 64
  across two slice edges; the bytes read back; `stat`'s size; the export
  written to .tmp, the old one unlinked, renamed in; `fgets`; the hidden
  mark;
- every refusal: nine paths outside the root (`jbox_rootx/a`,
  `jbox_root/a/..`, `jbox_root//..//x`, `""` and others) for fopen, mkdir,
  unlink and stat, with nothing made; `..notes.md` allowed as a name;
  rename with either end outside; a host pointer, a string off the
  box's end, a 310-byte path; seven bad modes; buffers off the box's end
  and an overflowing count; token and fd mixed up, feckless's FILE
  token, a closed file twice; the ninth open; errno when tp is not in
  the box; holds the app did not take, and `jinxed_box_end()` twice.
- Eight changes are each caught: no `..` check, prefix only, the
  segment walk skipped, a write slice from the wrong offset, a read slice
  corrupted, releasing a hold the app did not take, the table
  unfiltered, mkdir's errno unset. The slice change was not caught at
  first: the test's pattern repeated every 256 bytes, a divisor of the
  slice. The second try at the filter change failed to compile under
  `-Werror` and so tested nothing.
- `jbox_storage.c` compiles at `-O2 -Wall -Werror=all -Wextra -Werror=extra`
  against the stand-ins.

*Not checked:* an ESP-IDF build; picolibc's `struct stat` and a 32-bit
`long` (no RISC-V or 32-bit toolchain in the session); a board.

## u-jinxed-0004 -- feckless's storage calls, with writes held to /sd/jinxed

feckless-libs' umode branch now answers every storage call the editor
makes. u-feckless-0047 (Defeatist's) added the writes, `stat`, `rename`,
`mkdir` and the brief holds. u-feckless-0050 added `ferror`, `feof`,
`clearerr`, `fileno`, `fsync` and `unlink`, and sets the app's errno on
failure. u-feckless-0048 put `fprintf` in the app. Styled text is
u-feckless-0051. So 0003's own FILE table goes, and every storage call is
feckless's.

What feckless does not do is keep a boxed app's writes to one directory.
Its box guards the host's memory, not the card's files. That stays
Jinxed's, for now, as a guard in front of feckless's handlers.
`jinxed_box_calls()` is `feckless_box_calls_v1()`'s table, the same names
in the same order, with these wrapped:

- by path: `remove`, `unlink`, `mkdir`, `rmdir`, `utime`,
  `storage_mark_hidden`; `rename` by both paths;
- by path when the mode writes (`w`, `a`, `+`, or a mode that is not a
  string in the box): `fopen`, `storage_io_open`;
- refused when they would write: `f_open` with any of FatFs's write or
  create bits, and `f_chmod`. FatFs's paths are drive-form, not `/sd`,
  and the editor does not call FatFs.

A wrapper copies the path out of the box, checks the copy (0003's rule:
`JINXED_BOX_ROOT` or below, no `..` segment), and either passes the call,
arguments unchanged, to feckless's handler, or returns feckless's failure
value with the app's errno set to `EACCES` (`EFAULT` for a path that is
not a string in the box). Reads stay uncontained, as feckless has them.
If feckless's table lacks any wrapped name, `jinxed_box_calls()` returns
NULL, so the load fails rather than runs unguarded. A path-writing call
that feckless adds later is not guarded until it is added here.

feckless's handler reads the path from the box again after the check, so
a second thread of the app rewriting the string in between could get a
path past it. That makes this a guard against the editor's mistakes, not
against a hostile app. The box's own guarantee does not rest on it.

Removed with 0003's table: `answers.txt` (Jinxed answers no name of its
own now, so feckless's `boxcalls_names.txt` is the whole list),
`jinxed_box_end()` (feckless's `feckless_box_calls_end()` closes and
releases everything), and the test's own stand-in headers, which
collided with feckless's after u-feckless-0045 to 0048. The test now
uses feckless's stand-ins unchanged, and its source list follows
feckless's test Makefile.

*Checked:* `make -C m5tab5/components/jinxed_box/test FECKLESS=<feckless-libs
at 6a6983c>` on the host under ASan and UBSan, over real files:
- the table: same names, order, data and flags as feckless's; the
  eleven wrapped, every other entry feckless's own;
- doclog's sequence inside the root, through to feckless: mkdir and
  `EEXIST`, a twin written, fsynced through fileno, opened `r+b` and
  `ab`; the export unlinked and renamed in; utime, the trash, the hidden
  mark; a `storage_io_open` for writing;
- outside it: nine paths times nine writing modes for `fopen` and
  `storage_io_open`, plus mkdir, rmdir, remove, unlink, utime, rename
  either way, and the hidden mark. Each is refused with `EACCES`,
  nothing is made, and an existing file is neither removed, renamed
  nor truncated;
- reads outside let through (fopen `r`, `storage_io_open` `rb`, stat,
  opendir, `f_open` `FA_READ`); every FatFs write bit refused, inside
  too; `f_chmod` refused;
- `EFAULT` for host, unterminated and over-long paths; errno through a
  `tp` outside the box not written.
- Twelve changes are each caught: no `..` check, prefix only, `r+` not
  a write, only `w` writing, an unterminated mode counted as a read
  (caught only by errno after a first version of the test missed it,
  since feckless refuses that open too), rename checking one end,
  `f_open` refusing `FA_WRITE` only, `f_chmod` passed on, no `EACCES`,
  an unchecked `tp`, a guard name not in feckless's table, mkdir left
  unwrapped.
- `jbox_storage.c` compiles at `-O2 -Wall -Werror=all -Wextra
  -Werror=extra` against feckless's stand-ins. Its check of the FatFs
  mode bits against `ff.h` is a `_Static_assert` that only the real
  `ff.h` exercises; the stand-in defines `FA_READ` alone.

*Not checked:* an ESP-IDF build; a board.

## u-jinxed-0005 -- boxlog: the log, replayed in a box and out of one

Step 2 of 0001's order, the measurement, as a project of its own:
`m5tab5/boxlog/` (README there). The writer's firmware is unchanged: a box
needs `CONFIG_BOOTLOADER_REGION_PROTECTION_ENABLE=n`, which boxlog sets in
its own sdkconfig and the writer does not.

- Two builds of one `main/`, as Lothesome's umode branch has. The .bin is
  `host.c` and the ELF it embeds is `elf/`. `doclog.c` and `md.c` are the
  writer's own files, unchanged, compiled into both. Each build has its
  own `DOCLOG_DIR`, `/sd/jinxed/boxlog/host` or `.../box`.
- `boxlog.c`, the probe, is the same file in both. It appends 400
  generated edits to a synthetic note as the editor logs keystrokes
  (some under a second author, some as the spelling check's), closes the
  note, replays it from the card and compares it with what was appended,
  exports it, and styles its markdown. Then it copies every real note's
  twins from `/sd/jinxed` into its own directory and opens each one,
  which recovers and replays it. It writes one result file per run, with
  no times in it.
- `host.c` brings up the card as `main.c` does, without the keyboard,
  and runs the probe natively. It then loads the ELF with
  `jinxed_box_calls()` and runs it on core 1, logging the cause, pc and
  ELF address on a fault. Finally it compares `host.txt` with `box.txt`
  and logs `SAME` or `DIFFER at line N` with both lines.
- The manifest names feckless-libs' `umode` branch: the drivers, the
  storage handler, `feckless_abi`, `feckless_abi_storage`,
  `feckless_box_libc`, `feckless_umode`, `feckless_box_calls`, and what
  those require by name. `sdkconfig.defaults` adds the FatFs settings
  that `feckless_abi_storage` checks: fast seek, dynamic buffers,
  per-file cache, no lock, 4096-byte sectors. The writer's configuration
  lacks fast seek and does not need it. It also turns call statistics
  on (u-feckless-0029).

What the ELF imports was checked by name first. doclog.c, md.c and
spell.c, compiled at -O2 and listed with `nm -u`, need only names
feckless_box_calls answers (`rewind`, `fileno`, `fsync`, `unlink`,
`ferror`, `stat`, `rename`, `mkdir`, the brief holds, the hidden mark,
`esp_random`), or names feckless_box_libc links into the app (`fprintf`,
`snprintf`, `gmtime_r`, the string functions, `heap_caps_*`, `__errno`).
That spell.c, which writes its word list with `fprintf`, needs nothing
more means the editor's storage is now fully answered. On a board,
`boxcheck` decides.

*Checked:*
- `make -C m5tab5/boxlog/test` on a PC, under ASan and UBSan. The probe
  is built twice, as the .bin and the ELF build it, over notes that
  doclog itself made: a short one, a long one with an author, one with a
  torn second twin, one with no second twin, and a file that is not a
  log. The two result files are identical, and identical again on a
  repeat run. The synthetic note replays as appended, all four notes
  open, and the source notes are byte-for-byte unchanged.
- Three changes are each caught: a run that writes to a source note, a
  replay comparison off by a byte, and the notes taken in a different
  order by one side. Two of them failed to compile the first time, one
  under `-Werror=misleading-indentation`, and were redone. The test's own
  first version failed a correct run: its grep for failures matched the
  result line's `failed 0`, and it now looks for the failure words
  themselves.
- `boxlog.c` compiles at `-O2 -Wall -Werror=all -Wextra -Werror=extra`
  as firmware, as the ELF and on the host. The first try failed
  `-Wformat-truncation`, the trap CLAUDE.md describes, because GCC could
  not see that a note's name is under 256 bytes. Paths are now joined
  with an explicit length check rather than `snprintf`. `host.c` passes
  `-fsyntax-only` with the same flags, against feckless-libs' own headers
  for the storage handler, drivers, umode and box calls, and stand-ins
  for IDF's.

*Not checked:* an ESP-IDF build of either side, so neither the
ExternalProject embedding nor boxcheck has run; a board. The README's
log is a made-up example of the format.

## u-jinxed-0006 -- boxlog's .bin requires lwip, for elf_loader

The first build of boxlog stopped in elf_loader's `esp_elf_symbol.c`:
`sys/socket.h` was missing. feckless_umode requires elf_loader, which
includes `<sys/socket.h>` and `<arpa/inet.h>` without requiring lwip,
where they live. boxlog is trimmed to `COMPONENTS main` and what main
requires. 0005 put lwip in the ELF build's list, after Defeatist's boxwin
(u-defeatist-0003), but not in the .bin's, which links elf_loader through
feckless_umode. ninja, which builds the .bin, reported the failure. The
ELF side builds with make inside it, and its list already had lwip.

`m5tab5/boxlog/main/CMakeLists.txt`: lwip added to the .bin's
`PRIV_REQUIRES`, with the reason. Nothing in boxlog opens a socket.

*Checked:* the board's build log, as reported. *Not checked:* a build
after the change. If the ELF's build stops next, its error will come
from make, under `build/elf`.

## u-jinxed-0007 -- boxlog's .bin no longer links an app's libc

With 0006, the .bin's link failed with `stdin`, `stdout`, `stderr`,
`strerror`, `getc`, `fgetc`, `putc` and `fputc` each defined twice: by
`feckless_box_libc` (`box_stdio.c`, `box_string.c`, u-feckless-0048) and
by IDF's `esp_libc` and picolibc. The .bin should not link
`feckless_box_libc` at all, since it is the part of libc a boxed app
links into its own ELF. It did so because the component manager makes
every dependency in a component's manifest a requirement of that
component. `main/idf_component.yml` named `feckless_box_libc`, so main
required it in both builds, not only in the ELF's, where `REQUIRES` names
it.

- `m5tab5/boxlog/elf/elfdeps`, a component with no code. Its manifest
  holds the ELF's own dependencies, `feckless_box_libc` and
  `feckless_abi_storage`, and it requires both. Only `elf/` adds it
  (`EXTRA_COMPONENT_DIRS`), and the ELF's main requires it.
- `main/idf_component.yml` drops those two. What it keeps is what both
  builds need.
- `elf/` keeps a lock of its own, `elf/dependencies.lock`, instead of
  sharing the .bin's. One lock shared by two sets of manifests would be
  solved again by each build in turn. Both resolve the umode branch, so
  their commits match unless the branch moves between the two
  configures. The README says to compare them.

Lothesome's umode branch lists `feckless_box_libc` in its main manifest,
the same way, beside its standalone .bin. Its board runs predate
u-feckless-0048, which added the colliding definitions, so its .bin
would likely fail the same way now. That is not checked here.

*Checked:* the board's link log, as reported; that
`feckless_abi_box_elf()` finds `feckless_box_libc` by component name in
the ELF's build (its `project_include.cmake`), which a requirement from
elfdeps satisfies. *Not checked:* a build after the change. It assumes
the manager reads elfdeps' manifest as it reads main's, which ESP-IDF
does for any component in a build.

## u-jinxed-0008 -- boxlog: the ELF-only component named after its directory

0007's ELF configure stopped: `Failed to resolve component
'jinxed_boxlog_elfdeps' required by component 'main': unknown name`.
ESP-IDF names a component after its directory, and 0007 put it in
`elf/elfdeps`, so its name was `elfdeps`. main's `REQUIRES` named
`jinxed_boxlog_elfdeps`, a name nothing in the build had. The directory
is now `elf/jinxed_boxlog_elfdeps`, a name less likely to meet another
component's than `elfdeps`. `elf/CMakeLists.txt` adds it by that path,
and the comments in main's CMakeLists and manifest, and the README,
follow. 0007's section above keeps its old path, as written then.

The error came from the ELF's build, which means the .bin had configured
with 0007. Its link is not reached until the ELF is built and embedded,
so whether 0007 cured the duplicate libc is still to be seen.

*Checked:* the configure log, as reported; that nothing in boxlog still
names `elfdeps` alone. *Not checked:* a build after the change.

## u-jinxed-0009 -- boxlog's first board run, and a large note

### The board's run of u-jinxed-0008 (54b487a)

A Tab5 with an 8 GB SDHC card, holding one note with a log
(`deletable2.md`, 82 bytes):

    boxlog: host: 400 appends in 9507 ms, 23769 us each, worst 733005 us
    boxlog: host: synthetic replayed in 97 ms, 2325 bytes
    boxlog: host: markdown of 55 lines in 0 ms
    boxlog: host: deletable2.md: seq 1, 82 bytes, opened in 16 ms
    feckless_umode: app in its box at 0x48145000, 8192 KB, 259 calls; 52 KB used by the load
    feckless_umode: app_main on core 1
    feckless_umode: host calls, last 5.0 s: 942/s, 994.3 ms/s in the host
    feckless_umode:   fsync     80/s  6087.98 us each (2.94 waiting for the lock)  484.6 ms/s
    feckless_umode:   fopen     80/s  3669.98 us each (3.01 waiting for the lock)  293.6 ms/s
    feckless_umode:   fwrite   105/s  1261.05 us each (3.17 waiting for the lock)  132.4 ms/s
    feckless_umode:   fseek    158/s   247.61 us each (3.08 waiting for the lock)   39.1 ms/s
    feckless_umode:   fflush    80/s   358.47 us each (3.00 waiting for the lock)   28.6 ms/s
    boxlog: box: 400 appends in 9175 ms, 22939 us each, worst 684034 us
    boxlog: box: synthetic replayed in 134 ms, 2325 bytes
    boxlog: box: markdown of 55 lines in 0 ms
    boxlog: box: deletable2.md: seq 1, 82 bytes, opened in 17 ms
    boxlog: the probe ended after 9902 ms: reason 0
    boxlog: SAME: the box and the host agree, 3 lines

- **The box computes what the host does.** doclog's appends, replay and
  export, and md's styling, gave the host's results through jinxed_box's
  guard and feckless's handlers, with no fault and no refusal. This is
  the first boxed run of any of the editor's code.
- **An append costs the same in a box**: 22.9 ms there against the
  host's 23.8 ms, within the card's variation. It is all card time. A
  keystroke's append is, per twin, an fopen (3.7 ms), fseeks, an fwrite,
  an fflush and an fsync (6.1 ms). The trap into the host and the 3 us
  wait for the app's lock do not show beside that. The box spent 997 ms of
  every second in host calls.
- **The worst append**, 733 ms on the host and 684 ms in the box, is the
  card stalling, since it happens on both sides. The writer on main
  waits as long. That is a matter for main, and is not the box's.
- **Replay** of the 2 KB synthetic note took 97 ms on the host and 134
  ms in the box. That is mostly opening and reading two twins, with a
  37 ms difference in the box that a note this small cannot explain.
  The large note below gives a figure worth comparing.
- **Windows were not measured.** The longest stretch without a host call
  is computation over a large document, and this card had none.
  Markdown took 0 ms over 55 lines.
- elf_loader logged `Too much padding before segment[2], padding: 1071`
  at load. The load succeeded and the run was correct. It is noted here,
  not acted on.

### The large note

`boxlog.c`'s `large()`, after the synthetic note: 50 pastes of 4 KB of
generated markdown (headings, emphasis, code, strike-through, lists,
quotes, links), each one append, making 200 KB. The note is then closed,
replayed and compared, and its 3391 lines are styled whole, timed. md.c
makes no host call, so in a box that time is a single window. A result
line, `large pastes ... replay same md ...`, joins the file. The host
test requires it: 50 pastes, seq 50, 204800 bytes, `replay same`.

*Checked:* `make -C m5tab5/boxlog/test`, all passing, the two sides'
results identical. On a PC the large note's markdown takes 7 ms, and the
P4 will be slower. `boxlog.c` at `-O2 -Wall -Werror=all -Wextra
-Werror=extra`, firmware and ELF. *Not checked:* on a board.

*What to look for:* `large: markdown of 3391 lines in N ms, one window`
on the box's side. Under 5 ms there is nothing to do. At 20 ms or more, a
keyboard interrupt sharing core 1 would wait that long, and the editor
should style a screen's lines at a time rather than the document whole.
Also compare `large: replayed in N ms` between the host and the box.

## u-jinxed-0010 -- the large note on a board, and the editor's own passes

### The board's run of u-jinxed-0009 (be58350)

    boxlog: host: large: 50 pastes of 4096 bytes in 5546 ms, worst 659 ms
    boxlog: host: large: replayed in 789 ms, 204800 bytes
    boxlog: host: large: markdown of 3391 lines in 31 ms, one window
    boxlog: box: large: 50 pastes of 4096 bytes in 4767 ms, worst 684 ms
    boxlog: box: large: replayed in 914 ms, 204800 bytes
    boxlog: box: large: markdown of 3391 lines in 32 ms, one window
    boxlog: SAME: the box and the host agree, 4 lines

(Synthetic note and appends as in 0009's run: 22.6 ms an append on the
host and 23.6 ms in the box, with the worst at 561 and 765 ms.)

- **SAME again**, now over a 200 KB note: 50 appends of 4 KB records,
  a replay, and 3391 lines styled.
- **Computation costs the same in the box:** 32 ms styling against the
  host's 31. The box adds nothing to code that makes no call, as it
  should. The P4 styles markdown at about 0.16 ms a KB.
- **The 32 ms is not a window the editor makes.** editor.c styles the
  logical line of each drawn row (`md_for()`, 0031), and its gfx calls
  between rows end a window. A normal paragraph costs a fraction of a
  millisecond. Only one very long logical line approaches the
  whole-document figure, which is measured below.
- **Replay: 914 ms in the box against 789**, 16 % more. It reads as it
  goes (fread at 482 a second in the box's statistics), so it is not
  one window. The difference is the box's per-call cost over the many
  small reads of the twins. It is paid when a note is opened, not on a
  keystroke.
- **4 KB appends** took the same time on both sides, all card time:
  fwrite 5.6 ms each in the statistics.
- elf_loader's padding note again (`padding: 2951`), and again harmless.

### The editor's own passes

Reading editor.c for where md is called found what does make a
per-keystroke window. draw() runs `layout()` (editor.c:436), which wraps
the whole text in fixed-width cells, and `word_count()` (editor.c:460),
which scans every byte for the status line. Neither makes a host call,
so in a box each is one window per keystroke, and both grow with the
document. `pos_line()`, the walk from the top to find the cursor's line,
is a third.

`boxlog.c` now times copies of them (`line_from`, `head_line`,
`layout`, `word_count`, `pos_line`, from editor.c at be58350, with the
statics as parameters). They are static and tangled with the display,
so the probe cannot call the real ones. The copied `line_from` body
was diffed against editor.c's and matches, apart from a comment. The
copies run over the large note and then over the same note with every
newline made a space: one 200 KB paragraph, the worst case for
`layout()` and for `md_line()`, which is also timed on it. 45 columns,
42 for a heading: 720 px portrait at scale 2, the narrowest the editor
lays out, so the most lines. The line table's hash, the line count,
the word count and the cursor's line go into the result file, so SAME
covers them too.

On a PC: the one-paragraph layout takes 0.4 to 0.7 ms, word_count 0.9
to 1.4 ms and md_line 2 to 3 ms. Scaled by 0009's run (the P4 styled 4
to 5 times slower than the PC's 7 ms), expect a few milliseconds for
each on the board. That would be well under 20 ms even at the editor's
256 KB limit. The board decides.

*Checked:* `make -C m5tab5/boxlog/test`, both sides identical, the new
lines required. `-O2 -Werror=all -Werror=extra`, firmware and ELF.
*Not checked:* on a board.

*What to look for:* `box: large: layout ... word_count ... pos_line ...
one window each` and the `one-paragraph` lines, against the host's.
Under 5 ms each: the editor's draw loop can be boxed as it is. Over 20
ms: layout() should rewrap only from the edited line down, and
word_count() should be kept as a running total. Those are main's changes
as much as the box's, since the trusted editor pays the same time,
only with interrupts on.

## u-jinxed-0011 -- the editor's passes on a board; markers in boxlog's log

### The board's run of u-jinxed-0010 (dae16d7)

    boxlog: host: large: layout 8366 us (6375 lines), word_count 6371 us, pos_line 160 us: one window each
    boxlog: host: one-paragraph: layout 6297 us (4806 lines), word_count 6401 us, pos_line 114 us: one window each
    boxlog: host: one-paragraph: markdown of one 204800-byte line in 25 ms, one window
    boxlog: box: large: replayed in 916 ms, 204800 bytes
    boxlog: box: large: markdown of 3391 lines in 32 ms, one window
    boxlog: box: large: layout 8344 us (6375 lines), word_count 6359 us, pos_line 161 us: one window each
    boxlog: box: one-paragraph: layout 6280 us (4806 lines), word_count 6391 us, pos_line 121 us: one window each
    boxlog: box: one-paragraph: markdown of one 204800-byte line in 27 ms, one window
    boxlog: SAME: the box and the host agree, 7 lines

(Appends, the synthetic note and the large note's pastes and replay as in
0010's run.)

- **SAME, 7 lines**: the line table, word count and cursor line of the
  copied passes agree too.
- **The editor's per-keystroke windows, at 200 KB, in the box:**
  `layout()` 8.3 ms, `word_count()` 6.4 ms, `pos_line()` 0.16 ms. Each is
  a separate window. Linearly, at the editor's 256 KB limit (TEXT_MAX),
  that is about 10.4, 8.0 and 0.2 ms. All are under 20 ms, so **the
  editor's draw loop can be boxed as it is**, on core 1, with core 0
  keeping the USB keyboard's and the panel's interrupts. They are over
  5 ms, so it would not do on the panel's core. The host's figures are
  the same, as for all computation so far.
- **`word_count()` is slow for what it does**, about 31 ns a byte. Its
  byte-at-a-time loop over PSRAM is memory-bound. A running total, kept
  by the edit functions, would remove it from every keystroke. That
  would be worth doing on main for the trusted editor too, and is not
  needed for the box.
- **`md_line()` on one 200 KB paragraph: 27 ms**, or about 34 ms at
  256 KB, the one window over 20 ms. The editor styles the logical line
  of each drawn row, so only a document that is a single paragraph that
  long reaches it, and only once per draw. If that matters, md_for()
  could style from the row's start and not from the line's. That is
  noted, not done.
- One-paragraph layout is faster than the large note's (6.3 against
  8.3 ms), because `head_line()` runs once per logical line, and the one
  paragraph has one.

So, of 0001's list, the windows are settled for doclog, md and the draw
passes. What remains before step 4 (the editor in the box) is what the
box cannot answer at all: the editor's calls into the host's own modules
(the keyboard, the sender, power, settings) and the waiting on keys.

### Markers

boxlog's `app_main()` now logs a `=== boxlog: copy from here ===` line
first and a `=== boxlog: copy to here ===` line last, around `run()`
(the old body), so they bracket every way it can end. The README says
so.

*Checked:* `host.c` `-fsyntax-only` at `-O2 -Wall -Werror=all -Wextra
-Werror=extra` against feckless-libs' headers. *Not checked:* on a
board.

## u-jinxed-0012 -- jinxed_box: feckless's write filter in place of the wrappers

feckless-libs' umode branch now copies every path out of the box before
it uses it and calls a launcher's write filter on that copy
(u-feckless-0055, written for this). 0004's eleven wrappers checked
their own copy and then passed feckless the app's pointer, so another
thread of the app could change the path between the two. They are gone:

- `jbox_storage.c` is now `jinxed_box_may_write()`, 0003's rule
  (`JINXED_BOX_ROOT` or below, no `..` segment) as a public function, and
  `jinxed_box_calls()`, which installs it with
  `feckless_box_calls_write_filter()` and returns
  `feckless_box_calls_v1()`'s table as it is. Call it before each load,
  because `feckless_box_calls_end()` forgets the filter with the app.
  boxlog's `host.c` already does both.
- feckless filters the same calls 0004 wrapped. FatFs's `f_open` (when
  writing) and `f_chmod` were refused outright in 0004. They are now
  refused because their paths are drive-form (`0:/...`) and never under
  `/sd/jinxed`. The effect is the same.
- The 256-byte path limit goes with the wrappers. feckless takes paths
  to 1024 bytes, so a path that long under the root reaches the file
  system, which refuses it with its own errno (`ENAMETOOLONG`), not
  `EFAULT`.
- The component drops `fatfs` and `heap` from its requirements.

The test follows. The table is feckless's own pointer. The rule is
checked alone: seven allowed paths and thirteen refused, including
`0:/jinxed/a`. The behaviour through feckless is as before, with a
drive-form path standing for FatFs's. After `feckless_box_calls_end()`
an outside write goes through, and `jinxed_box_calls()` puts the filter
back. An unterminated mode is now feckless's `EFAULT`, since feckless
reads the mode before it filters. The source list follows feckless's
test Makefile again (box_codec.c and box_nvs.c, u-feckless-0052, 0053),
as 0004 said it would need to.

*Checked:* `make -C m5tab5/components/jinxed_box/test FECKLESS=<feckless-libs
at 4832315>`, all passing. Three changes are each caught: the filter not
installed (214 failures), no `..` check, prefix only.
`jbox_storage.c` compiles at `-O2 -Wall -Werror=all -Wextra
-Werror=extra`. The race is feckless's to test, and its `test_filter`
does. *Not checked:* an ESP-IDF build; a board.

## u-jinxed-0013 -- the boundary, host side: Jinxed's own calls

The split, as chosen: the keyboard, the sender, power and settings stay
on the host, with the settings page as one blocking host call, so the
email account and Wi-Fi never enter the box. The editor, `files.c` (with
feckless's chooser in the ELF) and NVS namespace `jinxed` go in the box.
One change from that choice: **`display.c` stays on the host too.** The
settings page sets the rotation through it (`display_set_rotation()`,
settings.c), and the page is the host's. The editor's four calls into it
(`display_text_scale`, `display_foot_scale`, `display_set_text_scale`,
`display_markdown`) become own calls with the rest.

What the box already answers: battery, the RTC, touch, the backlight,
the I2C bus, `vTaskDelay`, ticks, `esp_timer_get_time`, NVS
(u-feckless-0045 to 0053). `esp_app_get_description` is linked into the
ELF (feckless_abi_elf()). What the editor calls into Jinxed's own host
modules, by name from editor.c and files.c, is 22 calls.

`m5tab5/components/jinxed_box`:
- `jinxed_calls.txt`: `@symbol jinxed_calls_v1` and the 22 names, the
  file for the ELF's `OWN_CALLS` (u-feckless-0054).
- `src/jbox_calls.c`: the handlers. Most keep the writer's names and
  signatures, so editor.c will call them unchanged:
  - `kbd_get` (unlocked, it waits), `kbd_present`, `kbd_ascii`;
  - the sender's `fetch_dict`, `fetched`, `sent`, `gen` and `status`;
  - power's `idle_seconds`, `note_sent`, `off_idle` (unlocked, it may
    not return) and `wake_line`;
  - settings' `draw_gear`, `gear_w` and `gear_hit`;
  - the four display calls.

  Every pointer is checked for its length, every string the host reads
  is copied into it first, and results go only into the room given. A
  host string is truncated to fit and always terminated.
- Three calls cannot cross as they are, and have names of their own. A
  shim in the ELF (the next patch) turns them back into the editor's:
  - `jinxed_settings_author(buf, size)`: `settings_author()` returns a
    host pointer.
  - `jinxed_send_doc(name, range, text, len, foot, log_path, log_len)`:
    `sender_send_doc()` takes a function to copy the log, which would be
    the box's to run. The host reads the log itself, the first
    `log_len` bytes of the twin named, as `doclog_copy_log()` does,
    with the path held to `/sd/jinxed` and a log past the sender's
    buffer marked too big. The sender copies text and log before it
    returns, so the host keeps nothing of the box's.
  - `jinxed_settings_page()`: the page calls the editor's send from
    inside its loop, and the editor is in the box. So the host registers
    its own send callback, which records the request and calls
    `settings_close()`. The page returns 1 for "send now" and 0 when
    left, and the shim sends and opens the page again. The page then
    reopens at its top after a send; that is the visible difference.
- `settings.c` and `settings.h`: `settings_close()`, a flag checked after
  each of the page's two `s_send()` calls, and reset when the page
  opens. The writer's own page is unchanged, since nothing there sets
  the flag.
- The host's functions come in through `jinxed_box_host(&host)`, a
  `jinxed_host_t` of pointers given by the writer's main. A component
  cannot require main, and the test gives stand-ins. A call whose
  function is NULL answers 0. The writer's `main/` is on the component's
  include path for `kbd.h`'s `kbd_event_t`. No header there has a
  feckless header's name.
- `jinxed_box_calls()` returns feckless's table with the own calls after
  it, built once. For each load it installs the write filter (0012) and
  NVS namespace `jinxed` (`feckless_box_calls_nvs()`): files.c's and
  display.c's, not the mail account's.

*Checked:* `make -C m5tab5/components/jinxed_box/test FECKLESS=<feckless-libs
at 4832315>`, all passing. A new `test_own_calls` runs every own call
against stand-ins:
- an event copied out, and refused outside the box or at its end, with
  nothing written when no key comes;
- status lines fitted and terminated;
- `jinxed_send_doc`'s strings handed over as host copies and the text as
  the box's, with the log read from the card. Too long is too big, a
  path outside the root attaches none, and bad strings, text past the
  box or a 99-byte name refuse the send;
- the author truncated, and absent;
- the page left (0) and "send now" (1, closed), not remembered after;
- the display calls passed through;
- NVS: `jinxed` written, `mail` refused;
- with no host functions, every own call answers 0.

The table is feckless's entries unchanged and then the own calls, no
own name is a feckless name, `jinxed_calls_v1` is data and version 1,
and `jinxed_calls.txt` lists exactly the own calls.

Eight changes are each caught: `kbd_get` unchecked (ASan), the send's
strings handed over from the box, the log path anywhere, a host string
unterminated, no NVS namespace, a log past the host's buffer, and
`jinxed_calls.txt` naming a call that is not one. Three of them failed
to compile under `-Werror` the first time and were redone. One survives
because it changes nothing (the request flag is cleared at the page's
start too). The test's stand-in for the sender kept the handler's stack
copies and read them after the call, and ASan caught it. It now copies
during the call, as the sender does. Both files compile at `-O2 -Wall
-Werror=all -Wextra -Werror=extra`.

*Not checked:* settings.c compiled (its headers are the writer's whole
board); an ESP-IDF build; a board. The ELF's side, the shim and the
editor built boxed, is next.

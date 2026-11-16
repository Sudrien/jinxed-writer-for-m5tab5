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

## u-jinxed-0014 -- boxlog's manifest names feckless_nvs_handler

boxlog's configure, with 0013 and feckless-libs at 4832315, stopped:
`Failed to resolve component 'feckless_nvs_handler' required by component
'feckless_box_calls'`. Since u-feckless-0053, feckless_box_calls answers
NVS in a box and requires the handler by name. boxlog's manifest names
the components that feckless's require (0005), and now names this one
too. It requires only IDF's `nvs_flash`. The audio codec
feckless_box_calls can answer (u-feckless-0052) is an optional
requirement, and without it that table is empty. The writer's own
manifest already names the handler.

*Checked:* the configure log, as reported; feckless_box_calls' and
feckless_nvs_handler's requirements at 4832315. *Not checked:* a build
after the change.

## u-jinxed-0015 -- a power-off that does not happen is said, not silent

This is main's code, unchanged on this branch, and the fix applies to
main as it is. It is here because a board running the branch showed it.

**What a board did.** It woke for the RTC alarm, a dark wake, with no
note to send, with USB attached for the monitor. It logged
`power: dark wake: woke with no note to send`, then
`E ledc: ... LEDC is not initialized` and `E tab5_lcd: lcd_backlight_set(175): duty`,
then `W power: power off (try 1)`, and then nothing until its power
button was pressed by hand. The serial line dropped during the pulse.

**Why.** `pulse_off()` toggles the PMS150's line. On battery that cuts
the rail and never returns. On external power the rail stays up, and
after three tries `pulse_off()` returns with an error on the serial
line, the only report. The dark-wake paths `off_quiet()` and
`off_backoff()` then ended in `for (;;) vTaskDelay(portMAX_DELAY)`: no
panel (a dark wake never lights it), no editor, no keyboard. That is a
board that looks off and is not, and stays that way until its button.
The editor's idle power-off (`power_down()`) had the same gap in a
milder form: still here, it stayed dark and said nothing. The LEDC error
is `pulse_off()`'s `lcd_backlight_set(0)` on a dark wake, where the
panel and its LEDC were never set up.

**Now:**
- `power.c`, the dark wake: `not_off()` takes the place of the endless
  wait. It adds `did not power off (external power?)` to the wake line,
  after the line already set (`woke with no note to send; did not power
  off ...`), kept within the editor's 160-byte status line. It logs the
  restart and restarts. The alarm is already what it should be, either
  off or the backoff's, so the board boots normally, and the editor
  shows the wake line on its status line for 10 s, as it shows a
  scheduled send's result (0051).
- `power.c`: `pulse_off()` leaves the backlight alone on a dark wake
  (`s_dark`, set by `power_dark_wake()`), so there is no LEDC error.
- `editor.c`, the idle power-off: when `power_off_idle()` comes back, the
  screen is lit again and the status line says `did not power off
  (external power?)` for 10 s. The idle count starts over as before, so
  on USB the editor tries again after the power-off time and says so
  again.

Not changed: three tries, a second apart, before giving up. Whether a
board on external power should not try at all (`battery_external()`)
is a question for main.

*Checked:* `power.c`, `editor.c` and `settings.c` compiled at `-O2 -Wall
-Werror=all -Wextra -Werror=extra` against feckless-libs' own headers at
4832315 and stand-ins for ESP-IDF's, the first time any of them was
compiled in this work. editor.c was clean before the change too.
*Not checked:* a board. To see it: with USB attached, let the alarm wake
it with nothing to send, or set the power-off time to its shortest.
Either should come back lit with the line, not stay dark.

## u-jinxed-0016 -- the theme's colours for the box: jinxed_display_col

0013's list of the editor's calls into the host was made by name, and
missed one that is not a call. `COL_BG`, `COL_TEXT` and the rest
(display.h) read `display_col->...`, and `display_col` is a variable in
display.c pointing into its theme table. display.c stays on the host
(0013), so a boxed editor would import `display_col` as data. A data
entry in the table is copied at load, and would be a host pointer: the
first `COL_*` would fault. Compiling editor.c, files.c, doclog.c, md.c,
spell.c and fname.c against feckless-libs' headers and listing their
imports finds it beside the five 0013 already gives the shim
(`settings_author`, `settings_on_send`, `settings_page`,
`sender_send_doc`, and the log for the send). Nothing else is
unanswered.

- `jinxed_display_col(buf, size)`, a 23rd own call: the theme
  `display_col` points at now, copied whole into the app's
  `display_theme_t`. The shim (0018) keeps its own `display_theme_t`
  and points its own `display_col` at it, and asks again at start and
  after the settings page, where the theme is chosen. The ELF reads its
  colours from its own memory, as it draws.
- `size` must equal the host's `sizeof (display_theme_t)`. An ELF built
  against another layout gets nothing, and keeps the colours it had,
  rather than colours read from the wrong fields.
- `jinxed_host_t` gains `display_col` (the writer's `&display_col`) and
  `display_col_size`. It is the variable, not a function, since that is
  what display.h has. It is read at each call, so a theme chosen on the
  page is the one copied next.

*Checked:* `make -C m5tab5/components/jinxed_box/test FECKLESS=<feckless-libs
at bf2e864>`, all passing. The test copies the theme, copies the other
after the host's pointer moves, and refuses a size two bytes short or
over (nothing written), a buffer that is not the box's, and one that
runs past its end. With no host functions it answers 0. The list check
counts the new name. Three changes are each caught: no size check (three
failures), no pointer check (ASan), the name left out of
`jinxed_calls.txt`. `jbox_calls.c` compiles at `-O2 -Wall -Werror=all
-Wextra -Werror=extra`. *Not checked:* an ESP-IDF build; a board.

## u-jinxed-0017 -- doclog: the log's path and end, for a boxed send

The editor sends with `sender_send_doc(..., doclog_copy_log)`, and the
sender calls `doclog_copy_log()` on its own task to copy the log. In a
box that function is the box's, so the host cannot call it.
`jinxed_send_doc` (0013) has the host read the log itself instead,
given the twin's path and how many bytes to read. Until now only doclog
knew those: `s_path[0]` and `s_end`, both static.

- `doclog_log_path(&end)`: the open log's first twin, and the end of its
  last record, which are exactly what `doclog_copy_log()` reads. NULL
  and 0 when no log is open. The ELF's `sender_send_doc` (0018) passes
  them to `jinxed_send_doc`, and the host's reading of them is 0013's
  `log_from_card()`, held to `/sd/jinxed`.
- The writer does not call it. Main's doclog.c gains the function and
  nothing else changes.

One difference from the writer: `doclog_copy_log()` reads when the
sender gets to it, while the path and end are taken when the editor asks
for the send. The sender copies the log before `sender_send_doc()`
returns (0013), and the editor appends only on its own task, which is
blocked in that call. So the two read the same bytes.

*Checked:* `./test/run.sh`, all passing. The export test now also asks
for the path and end beside its `doclog_copy_log()` and finds the same
file and the same length, and NULL and 0 after the close. Two changes
are each caught: the end left as it was after a close, and the path
returned with no log open. doclog.c compiles at `-O2 -Wall -Werror=all
-Wextra -Werror=extra` against feckless-libs' headers at bf2e864 and
stand-ins for ESP-IDF's. *Not checked:* an ESP-IDF build; a board.

## u-jinxed-0018 -- the editor in a box: m5tab5/boxed, Jinxed's own .bin

This is step 4 of 0001's order: the whole editor in the box, with the
host's calls behind one table (0013, 0016). It is a project of its own,
`m5tab5/boxed`, laid out as boxlog is. The writer's firmware does not
build it and is unchanged by it, apart from one function in files.c.

**The ELF** (`boxed/elf`, `boxed/main` built with `JINXED_BOXED_ELF`):
the writer's `editor.c`, `files.c`, `doclog.c`, `md.c`, `spell.c` and
`fname.c` unchanged, feckless_storage_handler's `storage_list.c`, which a
boxed app links into itself since it calls the app's filter
(u-feckless-0047), feckless_chooser as an `ELF_COMPONENTS` entry, and
`boxed/main/shim.c`. `feckless_abi_box_elf(jinxed_editor OWN_CALLS
.../jinxed_calls.txt)` builds it, so both checks take the 23 own calls
as answered.

`shim.c` is the boundary's ELF side: the four things 0013 and 0016 left
for it.
- `settings_author()` copies the name out through
  `jinxed_settings_author`, into a buffer of `DOCLOG_AUTHOR_MAX + 1`,
  which is also the size of the host's.
- `settings_on_send()` and `settings_page()`: the page loops on
  `jinxed_settings_page`, and each 1 is a "send now", which the shim
  sends and then reopens the page. When the page is left, the shim
  fetches the theme again and forgets the mode (below).
- `sender_send_doc()` passes `doclog_log_path()` (0017) to
  `jinxed_send_doc` when it is given a log function, and nothing when it
  is given none.
- `display_col` points at the shim's own `display_theme_t`, filled by
  `jinxed_display_col` (0016) before `editor_init()`. If the host
  refuses (a layout mismatch), the shim logs it.
- `app_main()` requires the core ABI, storage, NVS and the box, refers to
  `jinxed_calls_v1`, and runs `editor_init()` and `editor_run()`.

**The .bin** (`boxed/main/host.c`): main.c's bring-up line for line, the
dark wake included, then `jinxed_box_host()` with the writer's functions
by name and `display_col`'s address, the load, and
`feckless_umode_app_run_on(app, 1)`. The rest of the writer's `main/`
except `editor.c` is linked: `doclog.c` for the dark wake's send,
`spell.c` for the sender's check of a download, `files.c` for the
settings page's mode, and `fname.c` and `md.c` with them. The box is at
most 16 MB, and the host keeps 8 MB of PSRAM's largest block. That is
room for the gfx shadow buffer and for a send's copy of the text and
its 4 MB log, which the sender allocates the first time it sends. The
editor's own buffers are about 4.5 MB. Host calls run on
`box_main`'s 16 KB internal stack (u-feckless-0031). Internal is what
the settings page needs, since it writes NVS (CLAUDE.md: a task that
touches flash). If the editor ends, which the writer's never does, the
host logs why, puts one line on the screen, and waits. It does not
restart, because a fault at start would loop.

**Two copies of state, and the one that went stale.** files.c runs on
both sides: in the box for the notes page, and on the host for the
settings page's Daily/File buttons. Both read and write NVS `jinxed`
`mode`, but each caches it in `s_mode`. If the page changed the mode,
the box's copy would keep the old one until a reboot.
`files_forget_mode()` (files.c, files.h, the only change to the writer's
code) clears the cache, and the shim calls it after the page. The theme
is the other copy, refetched at the same point. The text size,
markdown and the rotation are asked of the host at each use, and spell
and doclog state are the box's alone.

**Manifests**, after boxlog's 0007 lesson that a manifest dependency is a
requirement of the component in both builds: `boxed/main/idf_component.yml`
names what both builds need (the ELF's headers, feckless_abi, and the
by-name requirements). `jinxed_boxed_bindeps`, which only the .bin's
build has, names feckless_umode, feckless_box_calls, mail, the keyboard,
the library table, the network and the esp_hosted cap.
`elf/jinxed_boxed_elfdeps` names feckless_box_libc and the storage and
NVS sections. All are on feckless-libs' `umode` branch. Neither build
has been run, so the resolution is unchecked (CLAUDE.md: a manifest
change).

**stpcpy.** Compiled by the host's GCC at -O2, editor.c's two `strcat()`s
for the F1 legend became `stpcpy()`. That name is neither in the app
(`inapp.txt`) nor answered by the box, so boxcheck would refuse the ELF
on it. Whether riscv32-esp-elf's GCC does the same is not known. The
ELF's sources are compiled with `-fno-builtin-stpcpy`, which stopped it
on the host. Adding stpcpy to feckless_box_libc's in-app list would be
the general fix, and that would be feckless-libs' change.

*Checked:* the ELF's sources (shim.c, the six from main/,
`storage_list.c` and feckless_chooser's source) compiled at -O2 against
feckless-libs' headers at bf2e864 and stand-ins for ESP-IDF's. shim.c and
the six compile with `-Wall -Werror=all -Wextra -Werror=extra`. Their
undefined symbols, less what they define, were compared with
`boxcalls_names.txt`, `inapp.txt` and `jinxed_calls.txt`, as boxcheck
compares them. What is left is `esp_app_get_description` (linked in by
`feckless_abi_elf()`), `stpcpy` (above), and glibc's own
`__errno_location`, `__ctype_tolower_loc` and GOT, which picolibc has
inside the app. Every own call is imported, `jinxed_calls_v1` included.
host.c compiles with the same flags. `./test/run.sh` and the jinxed_box
test pass.
*Not checked:* an ESP-IDF build of either side. That means the CMake,
`ELF_COMPONENTS`, `COMPONENT_DIR` in the ELF's component, both checks
for real, the link, and the manifests' resolution. Also not checked:
the writer's kbd.c, settings.c, sender.c and power.c compiled here (they
are unchanged; this patch's stand-ins lack their IDF headers), and
anything on a board.

**What a first board run should show:** `the editor, boxed, in N KB`;
the editor drawing with the theme; typing that appends (`.h0`/`.h1`
growing); the settings page opening, changing the theme, and coming back
in it; Send notes now sending with the log attached; the notes page
listing and opening; and no key lost while typing fast. The call
statistics will say what a keystroke costs in host calls.

## u-jinxed-0019 -- boxed: the chooser compiled into the ELF's main

The first ESP-IDF build of 0018's ELF passed feckless_abi's check (92
symbols, all in the core list, box, nvs, storage and `jinxed_calls_v1`)
and then failed boxcheck:

    imports names the box does not answer ...:
      storage_list_find  storage_list_read  storage_list_sort
      storage_list_volumes  storage_list_free

**Why.** 0018 compiled `storage_list.c` into main's archive and linked
feckless_chooser as an `ELF_COMPONENTS` library after it. Only the
chooser calls `storage_list_*`, files.c does not. So when the linker
scanned main's archive, nothing yet needed `storage_list.o` and it was
left out. By the time the chooser's library asked for those names, that
archive had been passed. A shared object may keep undefined names, so
the link went through with five imports the box does not answer, and
boxcheck stopped it, as it is there to do. 0018's check compared loose
objects, which says what is defined but not what an archive link pulls
in.

**Now** `feckless_chooser.c` is compiled into main beside
`storage_list.c`, found by its component's `COMPONENT_DIR` as
`storage_list.c` is, and `ELF_COMPONENTS` no longer names the chooser.
Within one archive, ld rescans until nothing more is pulled: files.o
pulls in the chooser, and the chooser pulls in `storage_list.o`. main
still requires feckless_chooser, for its header. The chooser's
CMakeLists sets no definitions or private includes that its source
would miss here.

*Checked, on the host,* with the same sources compiled `-fPIC` at -O2
against the same stand-ins: archived as 0018 links them (main's archive,
then the chooser's) and linked `-shared`, the result imports exactly
these five `storage_list_*` names. With the chooser in main's archive,
it imports none of them and no `chooser_*`. *Not checked:* the ESP-IDF
build after the change; a board.

## u-jinxed-0020 -- a key's release is never the event dropped

This is main's code, unchanged on this branch, and the fix applies to
main as it is (kbd.c is the same file there). A board running the boxed
editor showed it.

**What a board did.** It ran `m5tab5/boxed` with feckless-libs'
`umode` branch, and the editor redrew at about 60 ms a blit plus two
10 ms fills a frame. At 62.7 s, typing outran the redraws, and kbd.c
logged `queue full; key ... dropped` 50 times in 1.3 s, among them
0x2C, space, four times. One of the dropped events was space's release.
The editor repeats a held key until it sees that key's release
(`held`, editor.c), so it typed spaces at its redraw rate, 11 frames a
second for about 50 s by the call stats, until space was pressed again.
The unboxed writer's queue had filled the same way before (u-jinxed-0018's
first log). There the dropped keys happened to include no held key's
release.

**Why.** kbd.c's `post()` queued every event with `xQueueSend(..., 0)`
and dropped whatever did not fit, presses and releases alike. A dropped
press loses a character. A dropped release loses the end of a key.

**Now** (`main/kbdq.h`, header-only, and kbd.c's `post()`):
- A press goes in only while more than `RELEASE_ROOM` places are free:
  six (a boot report's key array) for each of `KBD_MAX` keyboards, 24.
  `QUEUE_LEN` is 64 + 24, so presses keep the 64 places they had.
- A release goes in only if its press did. Each keyboard's context keeps
  a bit for each usage, set when the press goes in and cleared when the
  release does. The context is zeroed when the keyboard opens, as
  before.
- So the room is always enough. A release is admitted only for a key
  the editor has seen go down, or will, and that is a key the keyboard
  holds now, of which a boot report has at most six. A release whose
  press was dropped is dropped too. The editor never saw that key down,
  and the release would take a held key's room.
- The unplug path (`reap()`) posts releases for the keys held, as
  before, through the same rule.
- Logged: a dropped press as before (`queue full; key ... dropped`).
  "No room for a release" is an error that the rule makes impossible.

Dropped presses still lose characters when typing outruns the redraws.
This patch makes the overflow harmless, not rare. The editor drawing
once for all the keys waiting, instead of once a key, would make it
rare, and is a separate change.

*Checked:* `./test/run.sh` now builds `test/kbdq_host_test.c`. It models
the queue as kbd.c sizes it, with one to four keyboards whose reports
hold at most six keys, and an editor that drains quickly, slowly or not
at all for up to 450 events. Over 400 seeded runs of 20,000 events
(nearly a million admitted, 3.4 million presses dropped), the editor
never holds a key once everything is let go and drained, never sees a
release for a key it does not hold, and no admitted release lacks room.
The board's case also passes: space held, 200 hard-typed keys into a
stalled queue, space let go. With `old`, the same test runs kbd.c's
policy before this patch: all 400 runs leave keys down (19615 in all),
and space sticks, as on the board. Two changes are each caught: presses
allowed into the releases' room, and releases admitted without their
press. kbd.c compiles at `-O2 -Wall -Werror=all -Wextra -Werror=extra`,
before and after, against esp-usb's own `usb/usb_host.h` (GitHub, main)
and stand-ins for the rest of ESP-IDF. *Not checked:* an ESP-IDF build;
a board.

## u-jinxed-0021 -- one redraw for all the keys waiting

This is main's code too: editor.c's loop, unchanged on this branch until
now, and the fix applies to main as it is.

**What a board did** (the boxed build with 0020, 9e19f23). It typed in
bursts, and at 73.6 s kbd.c logged `queue full; key ... dropped` 13 times
in 1.3 s. Every one was a press, and no key stuck: kbd_get went back to
its idle 30 ms after, which is 0020 working. But each dropped press is a
character lost. The call stats say why. While typing, the editor drew 11
frames a second at about 90 ms each: a 60 ms `gfx_blit`, two 10 ms
`gfx_fill_rect`s and about 15 rows of `gfx_draw_text` at 0.6 ms. It drew
one frame per key that changed the screen. A key's own handling, host
calls included, is about a millisecond. So above about 10 keys a second
the queue only grew.

**Now** (`main/drawq.h`, header-only, and editor.c's loop):
- A key that changes the screen owes a redraw (`drawq_dirty()`). It
  does not draw.
- While one is owed, the loop's wait for a key is 0 (`drawq_wait()`), so
  it goes straight to the next key already waiting.
- The redraw is paid (`drawq_due()`) when a pass of the loop finds no
  key, or once `DRAW_OWED_MS` (150) has passed since it was first owed,
  so a stream of keys faster than the redraws still shows as it is
  typed. The tick arithmetic is unsigned, so it survives the tick count
  wrapping.
- A held key's repeat is the editor's own timer, not the queue, so it
  finds no key waiting and draws each repeat as before. The pages and
  sheets a key opens either paint the whole screen or call `redraw()`
  themselves (find's bar does, with the blit held), so an owed redraw
  never shows stale text behind them. The key handler lays the text out
  itself before it reads the line table (`layout()` at its top), so
  several keys between redraws see the same layout one key would.

*Checked:* `./test/run.sh` now builds `test/drawq_host_test.c`.
- Unit checks of the three calls: nothing owed means a full wait and
  nothing due; owed means no wait; a key keeps it owed until 150 ticks
  from the first, not the last; no key means due once; the tick count
  wrapping.
- A simulation of the loop, a millisecond at a time. Its costs are the
  board's: a 100 ms redraw, 1 ms a key, 1 ms of the loop's other work,
  and the 30 ms touch poll. A typist presses a key every 1000/rate ms
  and holds it 60 ms, into an 88-place queue by kbd.c's rule (kbdq.h,
  0020).
  - Drawing after every key, as before: nothing dropped at 5 and 8 keys
    a second, but at 10 the keys are shown up to 2.5 s late. At 12 it
    drops 113 presses a minute, then 298 at 15 and 586 at 20, shown
    3.4 s late. That is the board's behaviour.
  - With drawq: none dropped at any of those rates, and every key shown
    within 103 ms at 5 and 8 keys a second and within 213 ms at 20,
    with fewer redraws.

Four changes are each caught: no 150-tick cap, waiting while a redraw is
owed, the owed time restarted by each key, and always due (the old
loop). editor.c compiles at `-O2 -Wall -Werror=all -Wextra
-Werror=extra` against feckless-libs' headers and stand-ins for
ESP-IDF's. *Not checked:* an ESP-IDF build; a board. What a board should
show: no `queue full` at a brisk pace, and the call stats with fewer
`gfx_blit`s than keys while typing fast.

## u-jinxed-0022 -- boxed: blit statistics on

`m5tab5/boxed/sdkconfig.defaults` sets `CONFIG_FECKLESS_GFX_BLIT_STATS=y`.
feckless_graphics_handler then logs, every 100 blits, the average blit and
its parts (u-feckless-0032):
- the lock;
- the turn into the scratch, called "filter", which is the CPU's gather
  or, with u-feckless-0057, the PPA's;
- `draw_bitmap`;
- the wait for the transfer;
- the gaps between bands.

The boxed editor's whole-screen blit was 84 ms on its first runs and
60 ms on later ones (0023), with nothing yet saying which part, or
whether the PPA was turning. A measurement, like the call statistics
beside it. It goes off with them once the editor's drawing is settled.

**`rm sdkconfig` in `m5tab5/boxed` before the next build** (CLAUDE.md:
the defaults fill only keys an existing sdkconfig lacks, and an existing
one already holds this key, off). The ELF's build copies the .bin's
sdkconfig, so one removal does both.

*Checked:* the option's name and meaning against feckless-libs' Kconfig
and gfx.c at bf2e864 plus u-feckless-0057. *Not checked:* a build; a
board.

## u-jinxed-0023 -- the board runs of u-jinxed-0018 to 0021

Documentation only. The runs since 0018, in order, from the logs as
reported, with the build each was (its version line) and what it
settled. Times are from boot.

### The writer, unboxed, on this branch (c07d382, 0018)

Built from `m5tab5/`, not `m5tab5/boxed`, so nothing was boxed. The
keyboard (04b4:0510) arrived at 99.7 s. From 116.5 s, `kbd: queue full;
key ... dropped` appeared ten times, among them three Enters. The cause
was the same as the boxed runs below: a redraw a key, slower than the
typing. Read at the time as possibly a stall, it was not.

### The first boxed run (3611083, built 19:48, 0019)

    feckless_umode: app in its box at 0x4845b000, 16384 KB, 297 calls; 97 KB used by the load
    jinxed: the editor, boxed, in 16384 KB, 297 calls
    feckless_umode: app_main on core 1
    editor: 57 columns, 13 rows, scale 3
    doclog: deletable2.md: seq 4, 186 bytes

- **It loaded and ran for 827 s with no fault.** The box is 16 MB, with
  297 calls: feckless's and Jinxed's 23. The editor read the note from
  the card. Wi-Fi came up, NTP set the RTC, and the radio went down, all
  on the host while the box ran.
- **It idled as designed.** `kbd_get` 32 times a second at 30.32 ms
  each, the touch poll's timeout. `touch_get` 0.55 ms. About 3 ms a call
  waiting for the app's lock. The dark fade came at 117 to 122 s
  (`vTaskDelay` 8/s, `lcd_backlight_set_counts`, one blit), on its 2 min
  default.
- **No key reached the editor in this run.** No blit came between boot
  and the fade, so nothing was typed.
- A whole-screen blit took 85.5 ms, and 124 ms during the fade.

### Typing in the box (the same build, later)

    gfx_blit         5/s 83760.72 us each   418.8 ms/s
    kbd_get         19/s 15528.69 us each   295.0 ms/s
    touch_get       19/s  8019.76 us each   152.4 ms/s
    gfx_fill_rect   10/s  9895.28 us each    99.0 ms/s
    gfx_draw_text   75/s   252.79 us each    19.0 ms/s
    fsync, fopen    on the card as typed

- **Typing works in the box.** Keys arrive, the editor draws, and the
  log is appended and fsynced.
- **A frame was about 140 ms of host time:** an 84 ms blit, two 10 ms
  fills, the rows' text, and `touch_get` at 8 ms a poll while drawing,
  against 0.55 ms idle (u-feckless-0032 noted the same). That is about
  7 frames a second, one a key. The box's own cost per call stays a few
  microseconds.
- The blit is feckless's `gather_rotated()`, a CPU turn of every pixel
  at 90 degrees. That led to u-feckless-0057, the PPA.

### A stuck space (3611083, built 20:46)

The same Jinxed commit, rebuilt. Only feckless-libs' `umode` branch can
have moved, presumably to u-feckless-0057. `gfx_blit` was now 60.5 ms.
Whether the PPA turned is not in the log; 0022 will say.
- At 62.7 s, kbd.c dropped 50 events in 1.3 s, 0x2C (space) four times
  among them.
- From about 57 s to 111 s, the editor drew 11 frames a second with
  `kbd_get` at 30 us, which is a held key repeating. Space's release had
  been dropped, so the editor never saw it come up. It typed spaces until
  space was pressed again.
- This led to **0020**: a key's release is never the event dropped.

### 0020 on a board (9e19f23, built 20:56)

- At 73.6 to 74.9 s, `queue full; key ... dropped` 13 times, every one a
  press. Then `kbd_get` went back to its idle 30 ms. **No key stuck.**
- Thirteen characters were still lost. The frame was about 90 ms (a
  60 ms blit, two 10 ms fills, about 15 rows of text at 0.6 ms), one a
  key. This led to **0021**: one redraw for all the keys waiting.

### 0021 on a board (08a94f1, built 21:02)

    gfx_blit        10/s 60420.61 us each   580.0 ms/s
    gfx_fill_rect   19/s 10032.37 us each   194.6 ms/s
    gfx_draw_text  144/s   720.57 us each   104.1 ms/s
    touch_get       59/s   616.01 us each    36.6 ms/s
    kbd_get         59/s   601.48 us each    35.7 ms/s
    display_markdown 2222/s  6.57 us each    14.6 ms/s

- From about 7 s to 22 s of typing: **no `queue full` at all.** There
  were 56 to 60 key events a second against 8 to 10 frames, so a frame
  took several keys, and `kbd_get` came back in 0.6 to 3.8 ms on average,
  usually with a key already waiting. The append, fsync and the export
  (rename, unlink, `storage_mark_hidden`) followed when typing stopped.
- **The frame is now the cost:** about 100 ms, so 10 frames a second,
  and a key shows within about 200 ms. Of that, 60 ms is the blit, 20 ms
  the fills, and the rest the rows' text. `display_markdown` is asked
  about 220 times a frame, an own call each time at 6.6 us. That is
  harmless, and a candidate for caching.
- This led to **0022** (where the blit's 60 ms goes) and **0024** (only
  the rows that changed).

### Not settled by these runs

The settings page in the box (theme, Send notes now), the notes page
(Cmd+O), the spell view, and a send with its log. None were exercised in
the logs reported.

# boxlog

Jinxed's log (`main/doclog.c`) and markdown styling (`main/md.c`), run
natively and then in a feckless box, with the two results compared
(u-jinxed-0005). This is the first measurement on the way to a boxed
editor (`UMODE.md`). It is not part of the writer: its firmware does not
build this, and nothing in `main/` includes it.

## What it answers

1. **Does the box compute what the host does?** The same probe
   (`main/boxlog.c`) runs twice: once in the .bin, and once in the ELF it
   embeds, which is boxed through `jinxed_box_calls()`, the table the
   writer's own .bin would use. Each run writes a result file. The .bin
   then compares the two files line by line.
2. **What does an append cost in a box?** Every keystroke the writer
   logs is a changeset written to both twins and fsynced. In a box, every
   write, fflush and fsync is a host call.
3. **How long are its windows?** Replaying a log and styling a document
   are computation between host calls. While that runs, the box holds
   interrupts off on core 1.

## Build

ESP-IDF 6.1, from `m5tab5/boxlog`:

    idf.py set-target esp32p4
    idf.py build flash monitor

This is a project of its own. Its manifest names feckless-libs' `umode`
branch, and the first configure writes its own `dependencies.lock`. The
ELF's build writes `elf/dependencies.lock`, since its manifests differ:
`feckless_box_libc` and `feckless_abi_storage` are named only in
`elf/jinxed_boxlog_elfdeps`, so that the .bin does not link an app's libc
(u-jinxed-0007). Both locks resolve the same branch. After the first
build, their feckless-libs commits should match. Its sdkconfig is the writer's
`../sdkconfig.defaults`, plus `sdkconfig.defaults` here: region
protection off, the ELF in PSRAM, FatFs configured as the storage
section requires, and call statistics on. The ELF build ends with
feckless' `boxcheck`. If boxcheck lists a name, the box does not answer
something that doclog, md or the probe imports.

## Run

Use a card that has the writer's notes on it, in `/sd/jinxed`. The probe
reads every note there that has a log (`.<name>.h0`, and `.h1` when
present), and writes only below `/sd/jinxed/boxlog`:

- `host/` and `box/` hold each run's copies of the twins, and the synthetic
  note's log and export. Each run empties its own directory first.
- `host.txt` and `box.txt` are the results.

The originals are opened only for reading. In the box, `jinxed_box` would
refuse any write outside `/sd/jinxed` anyway.

## The log

    boxlog: the host's run
    boxlog: host: 400 appends in 9507 ms, 23769 us each, worst 733005 us
    boxlog: host: synthetic replayed in 97 ms, 2325 bytes
    boxlog: host: markdown of 55 lines in 0 ms
    boxlog: host: deletable2.md: seq 1, 82 bytes, opened in 16 ms
    boxlog: host: done in 10666 ms, /sd/jinxed/boxlog/host.txt
    boxlog: the box's run, in 8192 KB, 259 calls
    feckless_umode: host calls, last 5.0 s: 942/s, 994.3 ms/s in the host
    feckless_umode:   fsync     80/s  6087.98 us each (2.94 waiting for the lock)  484.6 ms/s
    ...
    boxlog: box: 400 appends in 9175 ms, 22939 us each, worst 684034 us
    boxlog: box: synthetic replayed in 134 ms, 2325 bytes
    ...
    boxlog: the probe ended after 9902 ms: reason 0
    boxlog: SAME: the box and the host agree, 3 lines

(A board's run of u-jinxed-0008, trimmed and with the call statistics
narrowed. It predates the large note, which adds three lines a run:
`large: N pastes ... in N ms`, `large: replayed in N ms`, and
`large: markdown of N lines in N ms, one window`.)

- **SAME** is the result that matters. **DIFFER at line N** prints both
  lines: the box replayed, exported or styled something differently from
  the host. That is a bug in the box, or in a call's answer, and that
  line names the step.
- **appends**: the box's time against the host's is the cost of a
  keystroke's log write going through host calls. The worst one is an
  upper bound on how long a keystroke could wait.
- **replayed** and **opened**, the box's against the host's: replay is
  almost all computation, so the box's figure for the largest note is
  close to its longest window. Over about 20 ms, the keyboard's USB
  interrupts would wait that long if they shared the core. That is why
  the editor runs on core 1.
- **markdown**: computation with no host call at all, so one window.
  The large note's (u-jinxed-0009, 200 KB) is the longest window the
  probe makes. Its replay is the other figure to compare between the two
  sides.
- Call statistics come every 5 s while the box runs (u-feckless-0029):
  the busiest host calls, and what each costs.
- **FAULTED** gives the cause, the pc, and the ELF address for
  `riscv32-esp-elf-addr2line -e build/elf/jinxed_boxlog_probe.app.elf`.

`host.txt`'s lines:

    synthetic edits 400 failed 0 seq 399 len 2325 text af29a797 replay same export ok 2399 a2b4e086 md 55 d6a16e1a
    note 2026-11-15.md seq 812 len 31873 text 1c0ffee5
    notes 1

`replay same` means the log, closed and reopened, gave back exactly what
was appended. `text`, `export` and `md` are FNV-1a hashes. Times are not
in the file, so that it can be compared.

## Host test

    make -C test

The probe, built twice as the .bin and the ELF build it (each with its
own `DOCLOG_DIR`), over notes that doclog itself made: a short note, a
long one with an author, one whose second twin is torn, one with no
second twin, and a file that is not a log. It passes when:

- the two result files are identical, and identical again on a second
  run;
- the synthetic note replays as it was appended and its export opens;
- all four notes open;
- the source notes are byte-for-byte unchanged.

It runs under ASan and UBSan.

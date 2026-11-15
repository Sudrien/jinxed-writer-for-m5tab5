# boxed

The writer with its editor in a feckless box (u-jinxed-0018, `UMODE.md`):
the editor runs in RISC-V user mode in one block of PSRAM, and every call
it makes out of that block is checked by the host first. Nothing in the
writer's own firmware builds this, and the writer is unchanged by it. The
writer's releases stay on main until this matches it.

## What is where

- **The ELF** (`elf/`, from `main/` here and the writer's `../main`): the
  writer's `editor.c`, `files.c`, `doclog.c`, `md.c`, `spell.c` and
  `fname.c`, unchanged, plus `main/shim.c`. These hold the text and parse
  what is on the card. Built by `feckless_abi_box_elf()` with Jinxed's
  own calls (`../components/jinxed_box/jinxed_calls.txt`); its two checks
  fail the build on any import the box would not answer.
- **The .bin** (`main/host.c` and the rest of the writer's `../main`):
  the board, the keyboard, the sender (Wi-Fi, SMTP, the dictionary
  download), power, and the settings page with the email account. The
  credentials and the network never enter the box. It loads the ELF
  with `jinxed_box_calls()`. That table holds feckless's calls, with
  writes kept to `/sd/jinxed` and NVS kept to the `jinxed` namespace,
  plus the editor's calls into the host's modules. It runs the ELF on
  core 1.

## Build

ESP-IDF 6.1, from `m5tab5/boxed`:

    idf.py set-target esp32p4
    idf.py build flash monitor

This is a project of its own, like `../boxlog`. Its manifests name
feckless-libs' `umode` branch, and the first configure writes
`dependencies.lock`. The ELF's build writes `elf/dependencies.lock`.
After the first build, the feckless-libs commits in the two locks should
match. What only one side links is named in that side's own component:
`jinxed_boxed_bindeps` for the .bin (mail, the network, the box's host
side) and `elf/jinxed_boxed_elfdeps` for the ELF (an app's libc, the
storage and NVS sections). That way neither side resolves the other's.
Its sdkconfig is the writer's `../sdkconfig.defaults` plus
`sdkconfig.defaults` here, which turns region protection off, puts the
ELF in PSRAM, configures FatFs as the storage section requires, and
turns on call statistics.

It flashes to the same partitions as the writer, and reads and writes
the same `/sd/jinxed` and NVS. Going between the two is a reflash, and
the notes carry over.

## Differences from the writer

- **The settings page reopens at its top after Send notes now**, since
  the send is the box's and the page is the host's (0013).
- **A boxed editor that stops** (a fault, or `app_main` returning)
  leaves a line on the screen and the reason in the serial log, and the
  board waits there. The text is on the card as the last append left
  it.
- **Every keystroke's file writes are host calls.** They cost about 5 %
  more than the writer's (boxlog, 0009 and 0010).

## The log

`=== Jinxed Writer (boxed) ===` is the .bin's first line, as the
writer's is. `the editor, boxed, in N KB, M calls` says the load went.
Call statistics follow every 5 s.

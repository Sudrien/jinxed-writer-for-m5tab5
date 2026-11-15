/*
 * boxlog.h -- the log, replayed in a box and out of one (u-jinxed-0005).
 *
 * boxlog_run() is the probe: the same file compiled into the .bin, where
 * it runs natively, and into the ELF the .bin boxes. Each run works in a
 * directory of its own, DOCLOG_DIR (BOXLOG_ROOT/host or BOXLOG_ROOT/box,
 * set by main/CMakeLists.txt), so the two never meet, and writes what it
 * found to a result file whose every line is the same in both runs if the
 * box computes what the host does. Times go to the log, not the file.
 *
 *   1. A synthetic note: SYNTH_EDITS edits from a fixed generator,
 *      appended one at a time as the editor appends keystrokes, the
 *      second author and the spelling check's changesets among them;
 *      closed, replayed from the card and compared with what was
 *      appended; exported; its markdown styled a line at a time.
 *   2. Every note in BOXLOG_SRC (the writer's own directory) that has a
 *      log: its twins copied into DOCLOG_DIR -- the originals are only
 *      read -- and opened there, which recovers and replays them.
 *
 * Nothing outside BOXLOG_ROOT is written, which in a box jinxed_box's
 * guard holds it to anyway (BOXLOG_ROOT is below /sd/jinxed).
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

#ifndef BOXLOG_SRC
#define BOXLOG_SRC  "/sd/jinxed"
#endif
#ifndef BOXLOG_ROOT
#define BOXLOG_ROOT BOXLOG_SRC "/boxlog"
#endif

#define BOXLOG_HOST_RESULT  BOXLOG_ROOT "/host.txt"
#define BOXLOG_BOX_RESULT   BOXLOG_ROOT "/box.txt"

/* The probe; result is the file its lines go to. 0, or -1 if it could not
 * start (no memory, no directory, no result file); a step that fails says
 * so in its line and the rest still run. */
int boxlog_run(const char *result);

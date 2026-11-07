/*
 * files.h -- file mode (0044, spec section 5): which mode the device
 * runs in, and the page that lists the notes and opens, makes, renames
 * and deletes them.
 *
 * The page is feckless_chooser over DOCLOG_DIR: the notes (.md, .txt,
 * .tex) newest first, with sizes and dates; the logs are dotfiles and
 * never shown. Cmd+O in the editor opens it, in either mode. Its keys
 * follow the keyboard, as the editor's do:
 *
 *   Up/Down, PageUp/PageDown, Home/End, a letter   move
 *   Enter, or a tap                                open
 *   Cmd+N                                          a new note, named
 *   F2, or Cmd+R                                   rename
 *   Delete, or Cmd+Backspace (Finder's)            to the trash, asked first
 *   Esc, or Cmd+O                                  back to the note
 *
 * Cmd+N in the editor (0045) goes straight to the new note's name, over
 * the list; Esc there goes back to the note, not to the list.
 *
 * The editor closes its log before the page and opens one after, so the
 * page may rename or trash any note, the open one too.
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>

/* The mode, kept in NVS (`jinxed`, `mode`): daily out of the box. */
#define FILES_DAILY     (0)
#define FILES_FILE      (1)
int  files_mode(void);
void files_set_mode(int mode);

/* The longest name the page will make, extension included. */
#define FILES_NAME_MAX  (48)

/* File mode's note when there is no other. */
#define FILES_DEFAULT   "notes.md"

/* File mode's note at power-on: the one last opened, kept in NVS
 * (`jinxed`, `file`). False when there is none. */
bool files_last(char *name, size_t n);
void files_set_last(const char *name);

/* What the page did. */
typedef enum {
    FILES_STAY = 0,         /* back to the note in `cur` -- renamed, or "" if it went to the trash */
    FILES_OPEN,             /* open the note now in `cur`: chosen, or new */
} files_result_t;

/*
 * The page, until a note is opened or it is closed. 0045: `new_note`
 * asks for a new note's name first, and leaves on Esc. `cur` is the open
 * note's name on the way in (FILES_NAME_MAX + 1 bytes or more, cap
 * `n`), and on the way out the note to open or go back to. Leaves the
 * screen for the caller to redraw.
 */
files_result_t files_page(char *cur, size_t n, bool apple, bool new_note);

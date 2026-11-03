# Writerdeck for the M5Stack Tab5: software spec

A discreet capture device for getting notes down where a phone shouldn't come out. Standalone ESP-IDF 6.1 firmware for the Tab5 (ESP32-P4, **chip rev v1.3**), a USB keyboard on the USB-A host, text on the SD card, and notes emailed out on a schedule.

Priorities, in order: **never lose a word**, **instant to a cursor**, **discreet**, **battery life**.

## 1. Hard constraints

- ESP-IDF 6.1. `sdkconfig.defaults` must contain `CONFIG_ESP32P4_SELECTS_REV_LESS_V3=y` and `CONFIG_ESP32P4_REV_MIN_100=y`. Delete `sdkconfig` after changing them.
- Standalone firmware, linking components from `Sudrien/feckless-libs-for-m5tab5` directly. Do not depend on the ELF-launcher ABI. Name each component at the same tag in `main/idf_component.yml`:
  - `feckless_drivers`: USB HID host, RX8130 RTC, battery, touch, `tab5io`
  - `feckless_graphics_handler`: panel, backlight, gfx (text scaling works)
  - `feckless_storage_handler`: SD arbiter (lease the volume per operation; never hold it open)
  - `feckless_network_handler`: Wi-Fi via the C6, saved networks in NVS
  - `feckless_nvs_handler`, `feckless_libversions`, plus the by-name deps each one lists (`usb_host_uac`, `usb_host_msc`, `fatfs` for exFAT)
- Call `tab5io_init()` first thing in `app_main()`.
- **No RTC/LP/HP memory survives any reset** on this board. Do not rely on `RTC_DATA_ATTR`, `RTC_NOINIT_ATTR`, or `__NOINIT_ATTR`.
- The reset reason is useless. Every restart reports `WDT` (7), or `USB` (11) after a flash. Deep sleep does not work as a wake model on this board.

## 2. Power model (measured on hardware)

**Sleep = soft power-off with an RX8130 alarm armed.** A full power-down that the RTC can undo.

Measured results:
| Fact | Evidence |
|---|---|
| Pulsing `PWROFF_PULSE` (expander 0x44, P4) powers the board off, with or without USB-C | serial port disappeared; no reboot |
| The RX8130 **alarm** powers the board back on, unattended | boot 13: AF set, booted 3 s after the alarm time |
| The RX8130 **timer** fires (TF sets) but does **not** power the board on | boots 9→10 and 13→, board stayed off until the button was pressed |
| Alarm registers: minute (0x17), hour (0x18), weekday/day (0x19 = `0x80` to ignore), `EXT` WADA=0, `FLAG` AF=0x08, `CTRL0` AIE=0x08 | verified by readback |

**Rules**
1. **Use the alarm only.** Keep `CTRL0` TIE/UIE off and `EXT` TE off at all times, so no stray flags accumulate.
2. Before every power-off that arms the alarm, in this order: clear AF, write the next alarm time, set AIE, then pulse off. A stale alarm time or flag caused confusing boots in testing.
3. The alarm has **minute resolution**. That is fine for hourly sends. The next alarm time is computed **relative to the RTC reading** (see section 3), so it stays correct even if the RTC's absolute time is wrong.
4. **Arm the alarm only if there is something to send:** the log is dirty *and* email is configured. Otherwise power off with AIE off. Nothing can change while the board is off (only the power button starts it), so a wake with nothing to send would cost a full boot for no reason.
5. **On every boot that reaches the editor, clear AF and disable AIE** before doing anything else. Otherwise an alarm that comes due while the user is typing leaves AF set, and a later crash or WDT reset (the reset reason is useless, section 1) would be mistaken for a scheduled wake and power the board off mid-sentence.
6. Power-off pulse that worked in testing: configure P4 of expander 0x44 as output, take it out of high-Z, drive it high for ~500 ms, then low. Retry if the board is still running after a second.
7. The power button powers the board on. A USB keyboard cannot.

## 3. Boot flow and the send schedule

```
power on
  tab5io_init()
  rtc8130 init; read FLAG register
  if AF set:                        # scheduled wake, screen stays dark
      clear AF, disable AIE
      mount SD (lease); recover the log (section 4)
      dirty = log has ops after its last checkpoint     # the card decides
      if !dirty:  power off, no alarm
      else:       bring up Wi-Fi, send (section 6),
                  on success: append a checkpoint, power off, no alarm
                  on failure: arm the alarm at backoff, power off
      store the result (section 6) in NVS
  else:                             # user power-on
      clear AF, disable AIE
      init display, SD, USB HID
      recover the log; reconcile NVS against it
      open the editor (section 5); if dirty, schedule a background send
```

- The scheduled path never initializes the display, touch, or USB host.
- **The interval is not wall-clock time.** It runs from the last boot or send attempt, measured on the RTC (or on uptime while the editor is up). There is no NTP and no top-of-the-hour alignment. At power-off the next alarm is `RTC now + interval`, rounded to the minute. Every boot, scheduled or user, is a point where "has anything changed since the last checkpoint" is evaluated.
- **Time budget:** give the whole send path a hard cap (suggest 45 s). On timeout, count it as a failure and apply backoff.
- **Backoff:** normal interval is 1 h (configurable). On failure, double up to a **2 h maximum**. Reset to normal on success. On a dark wake the failure is silent; its result is kept for the next editor open (section 6).
- **The card is the source of truth.** `dirty` and `last_sent_seq` in NVS are caches. `dirty` lets an empty wake skip mounting the SD card; `last_sent_seq` mirrors the log's last checkpoint. At every editor boot, recompute both from the log and let the log win. A swapped card or stale NVS must never cause a missed send or a silent duplicate.
- `dirty` is written to NVS only on the clean-to-dirty transition. `last_sent_seq` is written only after a successful send. No per-keystroke NVS writes.

## 4. Storage and history

**The log is the document.** The `.md` or `.tex` file is an *export*, regenerated from the log. If the export is missing or stale, nothing is lost.

- **Two mirrored log dotfiles** next to the export (e.g. `.2026-11-01.md.h0`, `.h1`). Each is a complete log. Every flushed burst is appended to `h0` and `f_sync`ed, then appended to `h1` and `f_sync`ed. Never write both at once, so a power loss can tear the tail of at most one file.
- **Recovery** at every boot: scan both from the start. Take the longest valid prefix of each, use the longer one, and copy its missing tail onto the shorter. A damaged or missing file is rebuilt from its twin. If both are lost but the export survives, rebuild a log from the export as a single insert (history is gone, text is not).
- **Record framing** (LevelDB/SQLite-WAL style): a header, then records of `length, type, seq, time, payload, CRC-32C`, where the CRC chains from the previous record so a stale record from an earlier use of the file cannot validate. Recovery stops at the first bad length or CRC.
- **Preallocate** the log (`f_expand`) and ignore the FAT directory size. Recovery scans records until the CRC fails or it reaches zeros. This is robust to a power loss between a cluster-chain extension and the directory-entry update, which FAT does not make atomic.
- **Ops:** timestamped inserts and deletes as `(pos, bytes)`, coalesced by typing burst and flushed when typing pauses. Each record also carries the document length before and after (as Etherpad's changesets do), so replay can detect an intact record applied against the wrong base. This is the full creation history and supports replay. Lengths are in bytes (UTF-8).
- **Checkpoint = a successful send.** A checkpoint record `{seq S, time, hash of the text at S}` is appended to both mirrors after, and only after, a send of everything through `S` succeeds. It may carry the full text to speed replay. A checkpoint names a seq, not the end of the log, so typing may continue past `S` while a send runs. Ops before a checkpoint are never pruned automatically.
- Log header stores: format version, doc id, highlight mode, creation time, last export hash.
- Sequence numbers are monotonic per document and are what email "sent up to" refers to. They are the authoritative order; timestamps are advisory.
- **Export** replays the log to text, writes `name.ext.tmp`, `f_sync`s, then replaces the old file (FatFs cannot rename over an existing file, so unlink then rename; the brief gap is harmless because the log has everything). Export runs on a typing pause, at idle power-off, before an email, at midnight rollover, and on demand. The export extension is the target format (`.md`, `.tex`, otherwise `.txt`); highlight mode lives in the header.
- **External edits:** the hash of the last export is stored in the log. If the export file differs from it at export time, do not overwrite it. Write `name.conflict.ext` beside it and leave the original alone. Importing external edits as ops is a possible later feature.
- **Importing existing files:** a document with no log gets one created with the file as a single insert op, timestamped with the file's modification time.
- **Delete moves** the export and both log dotfiles into a trash folder. It never erases.
- Time comes from the RX8130, which is never synced. If the chip reports VLF (lost time), mark the affected log entries' timestamps as untrusted rather than inventing a clock. `seq` is unaffected. The settings page must let the user set the clock by hand, since the daily filename and midnight rollover depend on it.

## 5. Modes and the editor

**Daily mode:** one file per date, `YYYY-MM-DD.md`. Opening lands the cursor at the end of today's file, with an auto-inserted time marker if there has been a gap. Midnight rollover: keep appending to the previous day until a typing pause, so a thought is never split. **File mode:** basic management only: new, open (recent first), rename, delete (to trash), show size and last-modified.

Which mode the device runs in is chosen on the **settings page** (section 7) and stored in NVS. There is no boot-time hotkey.

**Editor**
- Text is drawn with gfx and **scaled up from the 12px pixel font** to something comfortable on 1280x720. Scale is a setting.
- Dim, dark, low-contrast theme. No sounds, no notifications. Auto-dim, then blank, after an idle timeout. The status line shows the word count, plus one deliberate exception: the transient send result (section 6).
- Idle timeout ends in the power-off sequence from section 2. Before it, flush the log and run an export.
- **Highlighting has three modes:** none, markdown with `$math$`, TeX. Default for a new note is **none**. The mode comes from the document's extension when the log is created (`.md`, `.tex`, otherwise none), is stored in the log header, and is changeable from the settings page. **There are no hotkeys for switching modes yet.** Leave a clear hook for adding one later.
- **Highlighter:** line-based tokenizer on visible lines only, with small carried state for open `$$` blocks and code fences, and a bounded look-back for multi-line constructs. Muted palette: 2-3 dim tones (structure, math/commands, comments).
- **Math rules (Pandoc style):** an opening `$` must be followed by a non-space; a closing `$` must be preceded by a non-space and not followed by a digit; `\$` is a literal dollar. Inline `$...$` never crosses a line. An unmatched `$` is plain text. Backtick code spans and fences win over math.

**Shortcuts.** The modifier for app shortcuts is the **GUI key** (Windows key on PC keyboards, Command on Mac keyboards). Both send the same HID modifier, so they are handled identically. **Ctrl** and **Alt** (Option on a Mac keyboard: the same HID modifier) are the word-level modifiers. A Mac keyboard needs no separate bindings, only the Mac habits below. Do not assign any mode-switch hotkey.

*In the editor, today:*

| Keys | Does |
|---|---|
| Arrows | Move; Up/Down keep their column |
| Home / End | Line start / end |
| PageUp / PageDown | Move one screenful |
| Backspace / Delete | Delete before / after the cursor |
| Tab | Insert four spaces |
| Caps Lock | Toggle caps (shown in the status line) |
| GUI+Left / Right | Line start / end |
| GUI+Up / Down | Document start / end |
| GUI+Backspace | Delete the whole line |
| GUI+, | Open the settings page |

Held keys repeat. Ctrl and Alt chords type nothing.

*On the settings page, today:* Up/Down scroll; PageUp/PageDown scroll a screen; Home/End and GUI+Up/Down jump to the top and bottom; Enter opens the email form; GUI+T sends a test email; Esc or GUI+, closes the page. In the email form: Tab / Shift+Tab or Up / Down move between fields, Enter saves, Esc cancels.

*Planned, not yet implemented*, modelled on MS Word and on the Mac text-field habits. Each row is "Word / Mac":

| Does | Keys |
|---|---|
| Previous / next word | Ctrl+Left / Right, and Alt+Left / Right |
| Previous / next paragraph | Ctrl+Up / Down, and Alt+Up / Down |
| Delete previous / next word | Ctrl+Backspace / Delete, and Alt+Backspace / Delete |
| Extend the selection | Shift with any movement above (by character, word, line, document) |
| Select all, copy, cut, paste | GUI+A / C / X / V |
| Undo / redo | GUI+Z / GUI+Shift+Z; Ctrl+Y also redoes |
| Save now (flush the log, then export) | GUI+S |
| Find, next, previous | GUI+F, then F3 / Shift+F3 |
| Go to line | GUI+G |
| Text scale up / down | GUI+= / GUI+- (Word: Ctrl+] / Ctrl+[) |
| Send now (file mode's manual send) | GUI+Enter |
| Remove up to four leading spaces | Shift+Tab |

*Decisions recorded.*
- For letter shortcuts, **Ctrl is accepted wherever GUI is**, so a PC keyboard's Word habits (Ctrl+Z, Ctrl+S, Ctrl+F) work. For the arrow keys the two differ on purpose: GUI means the whole line or document, as on a Mac; Ctrl and Alt mean a word or paragraph, as in Word.
- **Undo never removes anything from the log** (section 4). It appends a new changeset that reverses the last one, so every word typed stays in the history.
- **Selection comes first.** Copy, cut, paste, the Shift movements and Word-style delete-word all rest on the editor having a selection, and a RAM clipboard. Until then those rows stay planned.
- **Not carried over from Word:** bold, italic, underline, alignment, styles, page breaks, tables, spell check, print. The text is plain.
- Emacs-style Ctrl+A / Ctrl+E (line start / end on a Mac) are optional and not planned; Ctrl+A would collide with select-all on a PC keyboard.

## 6. Email

- SMTP over TLS, linked directly (mbedtls / esp-tls). Send only. Wi-Fi is up only for the duration of a send, then dropped.
- **What is sent (daily mode):** the net difference between the text at the previous checkpoint and the text now, so text typed and then deleted within the interval is never sent. Plus a full copy on manual send. No changes since the last checkpoint means no attempt, no Wi-Fi, and no message.
- **File mode:** manual send only (a hotkey or button), attaching the current export. Optionally attach the log.
- Put the sequence range in the subject line so a duplicate after a mid-send reset is obvious. Write the checkpoint only after the send succeeds. If power fails in between, the next attempt duplicates the send, which is acceptable; a missed send is not.
- **Credentials** (host, port, user, app password, recipient) are stored in **encrypted NVS, the same way the saved Wi-Fi networks are**. Follow how `feckless_network_handler` does it; do not invent a second scheme. Never store credentials on the SD card.

**Background send from the editor.** When the editor boots dirty, and then once per interval of uptime while dirty, it attempts a send without touching the screen except for the status line.
- The send runs in its own lower-priority task, so TLS and SMTP never stall typing. The 45 s cap and the backoff apply.
- The editor already holds the text in RAM. At send time it copies the text and the current seq `S` under a brief lock and keeps the previous checkpoint's text in RAM, so the diff needs no SD read.
- **The editor is the only log writer.** The send task reports "sent through S" and the editor appends the checkpoint to both mirrors.

**Status messages** (the only regular interaction the user sees), in the status line with the word count, domain only so the full address is never shown, cleared after about 10 s or on the next keypress. Per attempt, in this order:
1. No saved network in range: `no wifi, did not send to @example.com` (just `no wifi` if email is not configured). Repeats on each attempt, spaced by backoff. This **suppresses** the email message.
2. Wi-Fi available, email not configured: `no email configured`, shown **once per boot**. After it is shown, no further attempts are made while email stays unconfigured. (Until it has been shown, an unconfigured device keeps checking for Wi-Fi at the backoff interval, so the radio use is bounded.)
3. Send succeeded: `sent to @example.com`.

A dark wake cannot show a message. Its outcome (a result code and time, a few bytes) is stored in NVS and shown for a few seconds when the editor next opens. A dark wake never counts as having shown the once-per-boot email message. If email is not configured, the alarm is not armed at all (section 2, rule 4).

## 7. Settings page

Reached from a **gear icon** (touch) and also from the keyboard. At minimum:
- Mode select: **Daily** or **File**
- Daily-mode flush interval (default 1 h; 2 h backoff cap is fixed)
- Highlight mode for the current document (none / markdown+math / TeX)
- Text scale
- Library versions: render the table generated by `feckless_libversions` (`libversions.h` / `libversions.tsv`) with versions and licences, as the library provides it. Drawing it is the app's job.
- Set the clock (the RTC is never synced; see section 4)
- Email and Wi-Fi setup entry points

Touch is for this page, the file picker, and scrolling. The keyboard does everything else.

## 8. Verify first (not yet confirmed on hardware)

1. **Battery-only alarm wake.** All alarm tests ran with USB-C plugged in. Confirm the alarm wakes the board with USB-C unplugged, battery in.
2. **Charging.** `CHG_EN` is P7 on expander 0x44 and needs the charger configuration code sent to enable charging. `tab5io_init()` handles the basic enable. The same code appears to drive the power-button LED green when power is present. **Open item: the exact code is to be supplied from the project owner's notes.** Check whether the pack charges while the board is soft-off.
3. **SMTP/TLS from the standalone build,** including the time from Wi-Fi up to message sent, against the 45 s cap in section 3, and that a background send does not stall typing.
4. **NVS encryption** is present and working in this build for the credentials, as it is for the Wi-Fi list.
5. **SD behavior when the card is removed** mid-session: the editor keeps buffering and the log catches up when a card returns. Also verify mirror recovery by cutting power mid-append at random points and checking that one twin always survives and the other is repaired.
6. **USB HID keyboard layouts:** at least US ANSI, with Windows-key and Command-key shortcuts both working.

## 9. Out of scope for now

On-device TeX typesetting, PDF export, note-mode hotkeys, a Wi-Fi AP or web UI, receiving email.

## 10. Test projects already written (for reference)

- `reset_probe`: checks which memory survives which reset. Result: none does.
- `rtc_wake_probe`: arms an RX8130 timer or alarm, then powers off via `PWROFF_PULSE`. Result: alarm works; timer does not wake the board. Its register definitions are a working reference for the alarm code.


## Sort this and elaborate
- SymSpell (maybe) as a manually triggered spellcheck. No wavy red lines as you type, instead there's a dedicated half screen format. Dictionary is probably large enough to put on the sd card. https://github.com/sumanpokhrel-11/symspell-c99
- Color for syntax highlighting and footer, no low contrast grays. Highlight background can be lower contrast.
- Dark mode, light mode
- configurable font scale (current size is smallest, this is a retina display)
- fake bold and italic & strikethrough for font, markdown basics
- can apple keybords be detected form usb descriptor? tells which shortcut set to use

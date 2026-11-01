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
2. Before every power-off, in this order: clear AF, write the next alarm time, set AIE, then pulse off. A stale alarm time or flag caused confusing boots in testing.
3. The alarm has **minute resolution**. That is fine for hourly sends.
4. Power-off pulse that worked in testing: configure P4 of expander 0x44 as output, take it out of high-Z, drive it high for ~500 ms, then low. Retry if the board is still running after a second.
5. The power button powers the board on. A USB keyboard cannot.

## 3. Boot flow

```
power on
  tab5io_init()
  rtc8130 init; read FLAG register
  if AF set:                        # scheduled wake, screen stays dark
      clear AF
      read NVS: dirty, last_sent_seq, retry state
      if !dirty:  set next alarm; power off
      else:       mount SD (lease), read log from last_sent_seq,
                  bring up Wi-Fi, send, update NVS on success,
                  set next alarm (normal interval, or backoff on failure); power off
  else:                             # user power-on
      init display, SD, USB HID
      open the editor (see section 5)
```

- The scheduled path never initializes the display, touch, or USB host.
- **Time budget:** give the whole send path a hard cap (suggest 45 s). On timeout, count it as a failure and apply backoff.
- **Backoff:** normal interval is 1 h (configurable). On failure, double up to a **2 h maximum**. Reset to normal on success. Failures are silent.
- **Reconcile at every editor boot:** compare NVS `last_sent_seq` against the log on the SD card. The card is the source of truth. A swapped card or stale NVS must never cause a missed send or a silent duplicate.
- `dirty` is written to NVS only on the clean-to-dirty transition. `last_sent_seq` is written only after a successful send. No per-keystroke NVS writes.

## 4. Storage and history

- One document plus **two history dotfiles** next to it (e.g. `.2026-11-01.md.h0`, `.h1`). They are written **alternately**, each with a checksum, so a power loss mid-write always leaves one valid copy.
- Content is an **append-only operation log** (timestamped inserts/deletes), coalesced by typing burst, with periodic checkpoints. It is the full creation history, and it supports replay. A burst is flushed to the log when typing pauses.
- Log header stores: format version, doc id, highlight mode, creation time, last checkpoint.
- Sequence numbers are monotonic per document and are what email "sent up to" refers to.
- **Delete moves** the doc and both dotfiles into a trash folder. It never erases.
- Time comes from the RX8130. If the chip reports VLF (lost time), mark the affected log entries as untrusted time rather than inventing a clock.

## 5. Modes and the editor

**Daily mode:** one file per date, `YYYY-MM-DD.md`. Opening lands the cursor at the end of today's file, with an auto-inserted time marker if there has been a gap. Midnight rollover: keep appending to the previous day until a typing pause, so a thought is never split. **File mode:** basic management only: new, open (recent first), rename, delete (to trash), show size and last-modified.

Which mode the device runs in is chosen on the **settings page** (section 7) and stored in NVS. There is no boot-time hotkey.

**Editor**
- Text is drawn with gfx and **scaled up from the 12px pixel font** to something comfortable on 1280x720. Scale is a setting.
- Dim, dark, low-contrast theme. No sounds, no notifications. Auto-dim, then blank, after an idle timeout. Word count is the only status display.
- Idle timeout ends in the power-off sequence from section 2.
- **Highlighting has three modes:** none, markdown with `$math$`, TeX. Default for a new note is **none**. The mode comes from the file extension (`.md`, `.tex`, otherwise none), is stored in the log header, and is changeable from the settings page. **There are no hotkeys for switching modes yet.** Leave a clear hook for adding one later.
- **Highlighter:** line-based tokenizer on visible lines only, with small carried state for open `$$` blocks and code fences, and a bounded look-back for multi-line constructs. Muted palette: 2-3 dim tones (structure, math/commands, comments).
- **Math rules (Pandoc style):** an opening `$` must be followed by a non-space; a closing `$` must be preceded by a non-space and not followed by a digit; `\$` is a literal dollar. Inline `$...$` never crosses a line. An unmatched `$` is plain text. Backtick code spans and fences win over math.

**Shortcuts:** the modifier for app shortcuts is the **GUI key** (Windows key on PC keyboards, Command on Mac keyboards). Both send the same HID modifier, so handle them identically. Initial set (proposal, to be confirmed): GUI+Left/Right (line start/end), GUI+Up/Down (doc start/end), GUI+Backspace (delete line), GUI+A/C/X/V/Z/Shift+Z. Do not assign any mode-switch hotkey.

## 6. Email

- SMTP over TLS, linked directly (mbedtls / esp-tls). Send only. Wi-Fi is up only for the duration of a send, then dropped.
- **Daily mode scheduled flush:** send only the new text since `last_sent_seq` as a diff, plus a full copy at end of day or on manual send. No changes since the last send means no Wi-Fi.
- **File mode:** manual send only (a hotkey or button), attaching the current file. Optionally attach the history file.
- Put the sequence range in the subject line so a duplicate after a mid-send reset is obvious.
- **Credentials** (host, port, user, app password, recipient) are stored in **encrypted NVS, the same way the saved Wi-Fi networks are**. Follow how `feckless_network_handler` does it; do not invent a second scheme. Never store credentials on the SD card.
- If no saved network is in range, fail silently and back off.

## 7. Settings page

Reached from a **gear icon** (touch) and also from the keyboard. At minimum:
- Mode select: **Daily** or **File**
- Daily-mode flush interval (default 1 h; 2 h backoff cap is fixed)
- Highlight mode for the current document (none / markdown+math / TeX)
- Text scale
- Library versions: render the table generated by `feckless_libversions` (`libversions.h` / `libversions.tsv`) with versions and licences, as the library provides it. Drawing it is the app's job.
- Email and Wi-Fi setup entry points

Touch is for this page, the file picker, and scrolling. The keyboard does everything else.

## 8. Verify first (not yet confirmed on hardware)

1. **Battery-only alarm wake.** All alarm tests ran with USB-C plugged in. Confirm the alarm wakes the board with USB-C unplugged, battery in.
2. **Charging.** `CHG_EN` is P7 on expander 0x44 and needs the charger configuration code sent to enable charging. `tab5io_init()` handles the basic enable. The same code appears to drive the power-button LED green when power is present. **Open item: the exact code is to be supplied from the project owner's notes.** Check whether the pack charges while the board is soft-off.
3. **SMTP/TLS from the standalone build,** including the time from Wi-Fi up to message sent, against the 45 s cap in section 3.
4. **NVS encryption** is present and working in this build for the credentials, as it is for the Wi-Fi list.
5. **SD behavior when the card is removed** mid-session: the editor keeps buffering and the log catches up when a card returns.
6. **USB HID keyboard layouts:** at least US ANSI, with Windows-key and Command-key shortcuts both working.

## 9. Out of scope for now

On-device TeX typesetting, PDF export, note-mode hotkeys, a Wi-Fi AP or web UI, receiving email.

## 10. Test projects already written (for reference)

- `reset_probe`: checks which memory survives which reset. Result: none does.
- `rtc_wake_probe`: arms an RX8130 timer or alarm, then powers off via `PWROFF_PULSE`. Result: alarm works; timer does not wake the board. Its register definitions are a working reference for the alarm code.

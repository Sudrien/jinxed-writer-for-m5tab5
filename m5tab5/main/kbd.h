/*
 * kbd.h -- a USB keyboard on the USB-A port, as key events.
 *
 * feckless_drivers' hid.c is a remote-control driver: it maps three
 * keyboard keys onto media buttons and has nothing to say about the
 * rest. A writer needs every key, so this is a host client of its own,
 * registered with feckless_drivers' usbhost.c the same way hid.c is.
 * hid_init() is not called -- the two would race to claim the same
 * interface.
 *
 * Boot protocol only: an interface with class HID, subclass 1 (boot),
 * protocol 1 (keyboard) is claimed and put into boot protocol, so every
 * report is the fixed 8 bytes of HID 1.11 Appendix B whatever the
 * report descriptor says. Every keyboard is required to support it.
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Byte 0 of a boot report. */
#define KBD_MOD_LCTRL   (0x01)
#define KBD_MOD_LSHIFT  (0x02)
#define KBD_MOD_LALT    (0x04)
#define KBD_MOD_LGUI    (0x08)
#define KBD_MOD_RCTRL   (0x10)
#define KBD_MOD_RSHIFT  (0x20)
#define KBD_MOD_RALT    (0x40)
#define KBD_MOD_RGUI    (0x80)

#define KBD_MOD_CTRL    (KBD_MOD_LCTRL  | KBD_MOD_RCTRL)
#define KBD_MOD_SHIFT   (KBD_MOD_LSHIFT | KBD_MOD_RSHIFT)
#define KBD_MOD_ALT     (KBD_MOD_LALT   | KBD_MOD_RALT)
/* Windows key and Command send the same usage. Spec section 5. */
#define KBD_MOD_GUI     (KBD_MOD_LGUI   | KBD_MOD_RGUI)

typedef struct {
    uint8_t usage;      /* HID keyboard-page usage, 0x04 'a' and up */
    uint8_t mods;       /* KBD_MOD_* held when this happened */
    bool    down;       /* press, or release */
} kbd_event_t;

/*
 * Register with the USB host. Before usbhost_start(), so a keyboard
 * already in the port at power-on is seen. Events arrive on a queue
 * that kbd_get() reads.
 */
esp_err_t kbd_init(void);

/* The next event, waiting up to `ticks`. False on timeout. */
bool kbd_get(kbd_event_t *ev, uint32_t ticks);

/* Whether a keyboard is open right now. */
bool kbd_present(void);

/*
 * US ANSI: the character a usage types with these modifiers and caps
 * lock, or 0 for a key that types nothing (arrows, F-keys, modifiers).
 * Enter is '\n' and Tab '\t'; Backspace and Delete are 0 -- they are
 * editing, not typing.
 */
char kbd_ascii(uint8_t usage, uint8_t mods, bool caps);

/* Usages the editor acts on. HID Usage Tables, keyboard page. */
#define KBD_A           (0x04)
#define KBD_D           (0x07)
#define KBD_I           (0x0C)
#define KBD_1           (0x1E)      /* to KBD_1 + 8, '9' */
#define KBD_ENTER       (0x28)
#define KBD_ESCAPE      (0x29)
#define KBD_BACKSPACE   (0x2A)
#define KBD_TAB         (0x2B)
#define KBD_T           (0x17)
#define KBD_SPACE       (0x2C)
#define KBD_SEMICOLON   (0x33)
#define KBD_COMMA       (0x36)
#define KBD_CAPSLOCK    (0x39)
#define KBD_F7          (0x40)
#define KBD_HOME        (0x4A)
#define KBD_PAGEUP      (0x4B)
#define KBD_DELETE      (0x4C)
#define KBD_END         (0x4D)
#define KBD_PAGEDOWN    (0x4E)
#define KBD_RIGHT       (0x4F)
#define KBD_LEFT        (0x50)
#define KBD_DOWN        (0x51)
#define KBD_UP          (0x52)
#define KBD_KP_ENTER    (0x58)

#ifdef __cplusplus
}
#endif

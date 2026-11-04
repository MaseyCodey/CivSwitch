#ifndef HID_PAD_H
#define HID_PAD_H

#include <stdbool.h>
#include <stdint.h>

#include "usb_descriptors.h"

/* Starts the HID action worker. Call after tinyusb_driver_install(). */
void hid_pad_init(void);

/* TinyUSB mount callbacks call this. */
void hid_pad_set_mounted(bool mounted);

bool hid_pad_mounted(void);

/* True while a queued action is running. */
bool hid_pad_busy(void);

/* Ask the worker to stop after the current step. */
void hid_pad_stop(void);

/* Console region for the macro: true = JP-only console (needs the IME
 * Alt+Space before every line), false = international console. */
void hid_pad_set_region(bool jp);
bool hid_pad_region_jp(void);

/* Queue one run of the compiled macro (macro_script.h). */
void hid_pad_bootstrap(void);

/* Queue `count` presses of `buttons`, optionally with a hat direction. */
void hid_pad_press(uint16_t buttons, uint8_t hat, uint16_t hold_ms, uint16_t count);

/* Queue typing of printable ASCII (US layout), optional trailing Enter. */
void hid_pad_type(const char *text, uint16_t hold_ms, bool enter);

/* Send the neutral report (keyboard released, pad centred). */
void hid_pad_neutral(void);

#endif /* HID_PAD_H */

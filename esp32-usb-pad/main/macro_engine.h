#ifndef MACRO_ENGINE_H
#define MACRO_ENGINE_H

#include <stdint.h>
#include "class/hid/hid.h"
#include "usb_descriptors.h"

#ifndef LANG_JP
#define LANG_JP 1
#endif

typedef enum {
    MACRO_OP_WAIT = 0,
    MACRO_OP_KEYS,
    MACRO_OP_PAD,
    MACRO_OP_BOTH,
    MACRO_OP_KEY_TIMES,
    MACRO_OP_END,
} MacroOp;

typedef struct {
    uint8_t op;
    uint16_t durationMs;
    uint16_t count;
    KeyboardReport keyboard;
    SwitchUsbReport gamepad;
} MacroStep;

#define MACRO_KBD_RELEASED {0, 0, {0, 0, 0, 0, 0, 0}}
#define MACRO_PAD_NEUTRAL \
    {0, SWITCH_USB_HAT_NOTHING, SWITCH_USB_JOYSTICK_MID, \
     SWITCH_USB_JOYSTICK_MID, SWITCH_USB_JOYSTICK_MID, \
     SWITCH_USB_JOYSTICK_MID, 0}

#define WAIT(ms) {MACRO_OP_WAIT, (ms), 1, MACRO_KBD_RELEASED, MACRO_PAD_NEUTRAL}
#define KEYS(ms, mod, k1, k2, k3, k4, k5, k6) \
    {MACRO_OP_KEYS, (ms), 1, {(uint8_t)(mod), 0, {(k1), (k2), (k3), (k4), (k5), (k6)}}, MACRO_PAD_NEUTRAL}
#define PAD(ms, buttons, hat, lx, ly, rx, ry) \
    {MACRO_OP_PAD, (ms), 1, MACRO_KBD_RELEASED, \
     {(uint16_t)(buttons), (hat), (lx), (ly), (rx), (ry), 0}}
#define PAD_BTN(ms, buttons) \
    PAD((ms), (buttons), SWITCH_USB_HAT_NOTHING, SWITCH_USB_JOYSTICK_MID, \
        SWITCH_USB_JOYSTICK_MID, SWITCH_USB_JOYSTICK_MID, SWITCH_USB_JOYSTICK_MID)
#define BOTH(ms, mod, k1, k2, k3, k4, k5, k6, buttons, hat, lx, ly, rx, ry) \
    {MACRO_OP_BOTH, (ms), 1, {(uint8_t)(mod), 0, {(k1), (k2), (k3), (k4), (k5), (k6)}}, \
     {(uint16_t)(buttons), (hat), (lx), (ly), (rx), (ry), 0}}
#define KEY_TIMES(n, ms, mod, k) \
    {MACRO_OP_KEY_TIMES, (ms), (n), {(uint8_t)(mod), 0, {(k), 0, 0, 0, 0, 0}}, MACRO_PAD_NEUTRAL}
#define END() {MACRO_OP_END, 0, 1, MACRO_KBD_RELEASED, MACRO_PAD_NEUTRAL}

#endif

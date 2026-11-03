#include <stdint.h>
#include <string.h>
#include <stdbool.h>
#include <stddef.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "tinyusb.h"
#include "tinyusb_default_config.h"
#include "class/hid/hid_device.h"

#include "usb_descriptors.h"
#include "macro_engine.h"
#include "macro_script.h"

_Static_assert(sizeof(switch_usb_device_descriptor) == 18, "device descriptor length");
_Static_assert(sizeof(switch_usb_configuration_descriptor) == 0x49, "config descriptor length");
_Static_assert(sizeof(switch_usb_report_descriptor) == 0x56, "gamepad report descriptor length");
_Static_assert(sizeof(keyboard_usb_report_descriptor) == 0x3f, "keyboard report descriptor length");

static const char *TAG = "usbpad";

// String descriptors must be an array of char pointers.
static const char *usb_string_descriptors[] = {
    (const char *)switch_usb_string_language,
    (const char *)switch_usb_string_manufacturer,
    (const char *)switch_usb_string_product,
    (const char *)switch_usb_string_version,
};

uint8_t const *tud_hid_descriptor_report_cb(uint8_t instance) {
    return instance == 0 ? switch_usb_report_descriptor
                         : keyboard_usb_report_descriptor;
}

uint16_t tud_hid_get_report_cb(uint8_t instance, uint8_t report_id,
                               hid_report_type_t report_type, uint8_t *buffer,
                               uint16_t reqlen) {
    (void)instance;
    (void)report_id;
    (void)report_type;
    (void)buffer;
    (void)reqlen;
    return 0;
}

void tud_hid_set_report_cb(uint8_t instance, uint8_t report_id,
                           hid_report_type_t report_type, uint8_t const *buffer,
                           uint16_t bufsize) {
    (void)instance;
    (void)report_id;
    (void)report_type;
    (void)buffer;
    (void)bufsize;
}

static void pad_report(uint16_t buttons, uint8_t hat, uint8_t lx, uint8_t ly,
                       uint8_t rx, uint8_t ry) {
    uint8_t report[8] = {
        (uint8_t)(buttons & 0xff), (uint8_t)(buttons >> 8), hat, lx, ly, rx, ry, 0,
    };
    tud_hid_n_report(0, 0, report, sizeof(report));
}

static void pad_neutral(void) {
    pad_report(0, SWITCH_USB_HAT_NOTHING, SWITCH_USB_JOYSTICK_MID,
               SWITCH_USB_JOYSTICK_MID, SWITCH_USB_JOYSTICK_MID,
               SWITCH_USB_JOYSTICK_MID);
}

static void send_keyboard(const KeyboardReport *report) {
    tud_hid_n_report(1, 0, (const uint8_t *)report, sizeof(*report));
}

static void send_gamepad(const SwitchUsbReport *report) {
    tud_hid_n_report(0, 0, (const uint8_t *)report, sizeof(*report));
}

static void send_neutral(void) {
    KeyboardReport k = MACRO_KBD_RELEASED;
    SwitchUsbReport p = MACRO_PAD_NEUTRAL;
    send_keyboard(&k);
    send_gamepad(&p);
}

static void apply_step(const MacroStep *step) {
    switch (step->op) {
        case MACRO_OP_WAIT:
            send_neutral();
            break;
        case MACRO_OP_KEYS:
            send_keyboard(&step->keyboard);
            break;
        case MACRO_OP_PAD:
            send_gamepad(&step->gamepad);
            break;
        case MACRO_OP_BOTH:
            send_keyboard(&step->keyboard);
            send_gamepad(&step->gamepad);
            break;
        case MACRO_OP_KEY_TIMES:
            for (uint16_t i = 0; i < step->count; i++) {
                send_keyboard(&step->keyboard);
                vTaskDelay(pdMS_TO_TICKS(step->durationMs));
                KeyboardReport released = MACRO_KBD_RELEASED;
                send_keyboard(&released);
                vTaskDelay(pdMS_TO_TICKS(step->durationMs));
            }
            break;
        default:
            break;
    }
}

#ifndef MACRO_REPEAT
#define MACRO_REPEAT 1
#endif
#ifndef MACRO_REPEAT_DELAY_MS
#define MACRO_REPEAT_DELAY_MS 5000
#endif

static void macro_task(void *arg) {
    (void)arg;
    while (!tud_mounted()) vTaskDelay(pdMS_TO_TICKS(100));
    ESP_LOGI(TAG, "USB mounted; macro starts in 2s");
    vTaskDelay(pdMS_TO_TICKS(2000));

    for (;;) {
        send_neutral();
        for (size_t i = 0; i < sizeof(MACRO_SCRIPT) / sizeof(MACRO_SCRIPT[0]); i++) {
            if (MACRO_SCRIPT[i].op == MACRO_OP_END) break;
            apply_step(&MACRO_SCRIPT[i]);
            vTaskDelay(pdMS_TO_TICKS(MACRO_SCRIPT[i].durationMs));
        }
        send_neutral();
        ESP_LOGI(TAG, "macro done; next round in %d ms", MACRO_REPEAT_DELAY_MS);
#if !MACRO_REPEAT
        break;
#else
        vTaskDelay(pdMS_TO_TICKS(MACRO_REPEAT_DELAY_MS));
#endif
    }
}

void app_main(void) {
    tinyusb_config_t cfg = TINYUSB_DEFAULT_CONFIG();
    cfg.descriptor.device = (const tusb_desc_device_t *)switch_usb_device_descriptor;
    cfg.descriptor.full_speed_config = switch_usb_configuration_descriptor;
    cfg.descriptor.string = usb_string_descriptors;
    cfg.descriptor.string_count =
        sizeof(usb_string_descriptors) / sizeof(usb_string_descriptors[0]);
    ESP_ERROR_CHECK(tinyusb_driver_install(&cfg));

    xTaskCreate(macro_task, "macro", 8192, NULL, 5, NULL);
}

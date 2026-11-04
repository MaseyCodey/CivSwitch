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
#include "hid_pad.h"
#include "esp_ota_ops.h"
#include "net_api.h"

_Static_assert(sizeof(switch_usb_device_descriptor) == 18, "device descriptor length");
_Static_assert(sizeof(switch_usb_configuration_descriptor) == 0x49, "config descriptor length");
_Static_assert(sizeof(switch_usb_report_descriptor) == 0x56, "gamepad report descriptor length");
_Static_assert(sizeof(keyboard_usb_report_descriptor) == 0x3f, "keyboard report descriptor length");

static const char *TAG = "netpad";

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

/* esp_tinyusb already defines the TinyUSB mount hooks; use its event callback. */
static void usb_event(tinyusb_event_t *event, void *arg) {
    (void)arg;
    hid_pad_set_mounted(event->id == TINYUSB_EVENT_ATTACHED);
}

void app_main(void) {
    tinyusb_config_t cfg = TINYUSB_DEFAULT_CONFIG();
    cfg.event_cb = usb_event;
    cfg.descriptor.device = (const tusb_desc_device_t *)switch_usb_device_descriptor;
    cfg.descriptor.full_speed_config = switch_usb_configuration_descriptor;
    cfg.descriptor.string = usb_string_descriptors;
    cfg.descriptor.string_count =
        sizeof(usb_string_descriptors) / sizeof(usb_string_descriptors[0]);
#if NETPAD_SKIP_USB
    ESP_LOGW(TAG, "NETPAD_SKIP_USB: TinyUSB disabled for console debugging");
#else
    ESP_ERROR_CHECK(tinyusb_driver_install(&cfg));
#endif

    hid_pad_init();
    ESP_LOGI(TAG, "USB HID ready");

    ESP_ERROR_CHECK(net_api_start());

#if CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE
    /* Reached only if everything above started; a panic earlier triggers rollback. */
    esp_ota_mark_app_valid_cancel_rollback();
#endif
}

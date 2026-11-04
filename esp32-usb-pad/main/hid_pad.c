#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stdlib.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "class/hid/hid_device.h"

#include "usb_descriptors.h"
#include "macro_engine.h"
#include "macro_script.h"
#include "hid_pad.h"

static const char *TAG = "hidpad";

#ifndef PAD_AUTO_MACRO
#define PAD_AUTO_MACRO 0
#endif
#ifndef PAD_AUTO_REPEAT
#define PAD_AUTO_REPEAT 0
#endif
#ifndef PAD_AUTO_DELAY_MS
#define PAD_AUTO_DELAY_MS 5000
#endif

#define PAD_TYPE_HOLD_MS 30
#define PAD_TYPE_GAP_MS 30

typedef enum {
    JOB_BOOTSTRAP = 0,
    JOB_PRESS,
    JOB_TYPE,
} JobKind;

typedef struct {
    JobKind kind;
    uint16_t buttons;
    uint8_t hat;
    uint16_t hold_ms;
    uint16_t count;
    bool enter;
    char *text; /* owned by the worker */
} PadJob;

static QueueHandle_t s_queue;
static volatile bool s_region_jp = true;   /* JP-only console by default */
static volatile bool s_mounted;
static volatile bool s_busy;
static volatile bool s_stop;

/* ------------------------------------------------------------------ reports */

static void send_keyboard(const KeyboardReport *report) {
    tud_hid_n_report(1, 0, (const uint8_t *)report, sizeof(*report));
}

static void send_gamepad(const SwitchUsbReport *report) {
    tud_hid_n_report(0, 0, (const uint8_t *)report, sizeof(*report));
}

void hid_pad_neutral(void) {
    KeyboardReport k = MACRO_KBD_RELEASED;
    SwitchUsbReport p = MACRO_PAD_NEUTRAL;
    send_keyboard(&k);
    send_gamepad(&p);
}

static void pad_report(uint16_t buttons, uint8_t hat) {
    SwitchUsbReport r = MACRO_PAD_NEUTRAL;
    r.buttons = buttons;
    r.hat = hat;
    send_gamepad(&r);
}

void hid_pad_set_region(bool jp) { s_region_jp = jp; }
bool hid_pad_region_jp(void) { return s_region_jp; }

bool hid_pad_mounted(void) { return s_mounted; }
bool hid_pad_busy(void) { return s_busy; }
void hid_pad_stop(void) { s_stop = true; }

void hid_pad_set_mounted(bool mounted) {
    s_mounted = mounted;
    if (mounted) {
        ESP_LOGI(TAG, "USB mounted");
    } else {
        ESP_LOGW(TAG, "USB unmounted");
    }
}

/* --------------------------------------------------------------- macro run */

static void run_key_times(const MacroStep *step) {
    for (uint16_t i = 0; i < step->count && !s_stop; i++) {
        send_keyboard(&step->keyboard);
        vTaskDelay(pdMS_TO_TICKS(step->durationMs));
        KeyboardReport released = MACRO_KBD_RELEASED;
        send_keyboard(&released);
        vTaskDelay(pdMS_TO_TICKS(step->durationMs));
    }
}

static void run_bootstrap(void) {
    ESP_LOGI(TAG, "bootstrap start");
    hid_pad_neutral();
    for (size_t i = 0; i < sizeof(MACRO_SCRIPT) / sizeof(MACRO_SCRIPT[0]); i++) {
        if (s_stop) break;
        const MacroStep *step = &MACRO_SCRIPT[i];
        if (step->op == MACRO_OP_END) break;
        switch (step->op) {
            case MACRO_OP_WAIT:
                hid_pad_neutral();
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
                run_key_times(step);
                break;
            case MACRO_OP_IME:
                if (!s_region_jp) {
                    /* International console: no IME, skip the step + its wait. */
                    continue;
                }
                {
                    KeyboardReport ime = MACRO_KBD_RELEASED;
                    ime.modifier = KEYBOARD_MODIFIER_LEFTALT;
                    ime.keycode[0] = HID_KEY_SPACE;
                    send_keyboard(&ime);
                }
                break;
            default:
                break;
        }
        if (s_stop) break;
        vTaskDelay(pdMS_TO_TICKS(step->durationMs));
    }
    hid_pad_neutral();
    ESP_LOGI(TAG, "bootstrap done%s", s_stop ? " (stopped)" : "");
}

/* ------------------------------------------------------------------ typing */

typedef struct {
    char ch;
    uint8_t key;
    bool shift;
} AsciiKey;

static const AsciiKey ASCII_KEYS[] = {
    {' ', HID_KEY_SPACE, false},         {'\t', HID_KEY_TAB, false},
    {'1', HID_KEY_1, false},             {'2', HID_KEY_2, false},
    {'3', HID_KEY_3, false},             {'4', HID_KEY_4, false},
    {'5', HID_KEY_5, false},             {'6', HID_KEY_6, false},
    {'7', HID_KEY_7, false},             {'8', HID_KEY_8, false},
    {'9', HID_KEY_9, false},             {'0', HID_KEY_0, false},
    {'!', HID_KEY_1, true},              {'@', HID_KEY_2, true},
    {'#', HID_KEY_3, true},              {'$', HID_KEY_4, true},
    {'%', HID_KEY_5, true},              {'^', HID_KEY_6, true},
    {'&', HID_KEY_7, true},              {'*', HID_KEY_8, true},
    {'(', HID_KEY_9, true},              {')', HID_KEY_0, true},
    {'-', HID_KEY_MINUS, false},         {'_', HID_KEY_MINUS, true},
    {'=', HID_KEY_EQUAL, false},         {'+', HID_KEY_EQUAL, true},
    {'[', HID_KEY_BRACKET_LEFT, false},  {'{', HID_KEY_BRACKET_LEFT, true},
    {']', HID_KEY_BRACKET_RIGHT, false}, {'}', HID_KEY_BRACKET_RIGHT, true},
    {'\\', HID_KEY_BACKSLASH, false},    {'|', HID_KEY_BACKSLASH, true},
    {';', HID_KEY_SEMICOLON, false},     {':', HID_KEY_SEMICOLON, true},
    {'\'', HID_KEY_APOSTROPHE, false},   {'"', HID_KEY_APOSTROPHE, true},
    {'`', HID_KEY_GRAVE, false},         {'~', HID_KEY_GRAVE, true},
    {',', HID_KEY_COMMA, false},         {'<', HID_KEY_COMMA, true},
    {'.', HID_KEY_PERIOD, false},        {'>', HID_KEY_PERIOD, true},
    {'/', HID_KEY_SLASH, false},         {'?', HID_KEY_SLASH, true},
};

static bool lookup_key(char c, uint8_t *key, bool *shift) {
    if (c >= 'a' && c <= 'z') {
        *key = (uint8_t)(HID_KEY_A + (c - 'a'));
        *shift = false;
        return true;
    }
    if (c >= 'A' && c <= 'Z') {
        *key = (uint8_t)(HID_KEY_A + (c - 'A'));
        *shift = true;
        return true;
    }
    for (size_t i = 0; i < sizeof(ASCII_KEYS) / sizeof(ASCII_KEYS[0]); i++) {
        if (ASCII_KEYS[i].ch == c) {
            *key = ASCII_KEYS[i].key;
            *shift = ASCII_KEYS[i].shift;
            return true;
        }
    }
    return false;
}

static void tap_key(uint8_t key, bool shift, uint16_t hold_ms) {
    KeyboardReport r = MACRO_KBD_RELEASED;
    if (shift) r.modifier = KEYBOARD_MODIFIER_LEFTSHIFT;
    r.keycode[0] = key;
    send_keyboard(&r);
    vTaskDelay(pdMS_TO_TICKS(hold_ms));
    KeyboardReport rel = MACRO_KBD_RELEASED;
    send_keyboard(&rel);
    vTaskDelay(pdMS_TO_TICKS(PAD_TYPE_GAP_MS));
}

static void run_type(PadJob *job) {
    if (!job->text) return;
    ESP_LOGI(TAG, "type %u chars", (unsigned)strlen(job->text));
    for (const char *p = job->text; *p && !s_stop; p++) {
        if (*p == '\n') {
            tap_key(HID_KEY_ENTER, false, job->hold_ms);
            continue;
        }
        uint8_t key;
        bool shift;
        if (lookup_key(*p, &key, &shift)) {
            tap_key(key, shift, job->hold_ms);
        }
    }
    if (job->enter && !s_stop) {
        tap_key(HID_KEY_ENTER, false, job->hold_ms);
    }
}

/* ------------------------------------------------------------------ worker */

static void run_press(uint16_t buttons, uint8_t hat, uint16_t hold_ms, uint16_t count) {
    for (uint16_t i = 0; i < count && !s_stop; i++) {
        pad_report(buttons, hat);
        vTaskDelay(pdMS_TO_TICKS(hold_ms ? hold_ms : 60));
        hid_pad_neutral();
        if (i + 1 < count) vTaskDelay(pdMS_TO_TICKS(60));
    }
}

static void action_task(void *arg) {
    (void)arg;
    PadJob job;
    for (;;) {
        if (xQueueReceive(s_queue, &job, portMAX_DELAY) != pdTRUE) continue;
        while (!s_mounted) vTaskDelay(pdMS_TO_TICKS(100));
        s_stop = false;
        s_busy = true;
        switch (job.kind) {
            case JOB_BOOTSTRAP:
                run_bootstrap();
                break;
            case JOB_PRESS:
                run_press(job.buttons, job.hat, job.hold_ms,
                          job.count ? job.count : 1);
                break;
            case JOB_TYPE:
                run_type(&job);
                break;
            default:
                break;
        }
        s_busy = false;
        s_stop = false;
        free(job.text);
        job.text = NULL;
    }
}

/* --------------------------------------------------------------------- api */

static bool enqueue_job(PadJob *job) {
    if (!s_queue) return false;
    if (xQueueSend(s_queue, job, 0) != pdTRUE) {
        ESP_LOGW(TAG, "queue full, dropping job");
        return false;
    }
    return true;
}

void hid_pad_bootstrap(void) {
    PadJob j = {.kind = JOB_BOOTSTRAP};
    enqueue_job(&j);
}

void hid_pad_press(uint16_t buttons, uint8_t hat, uint16_t hold_ms, uint16_t count) {
    PadJob j = {.kind = JOB_PRESS, .buttons = buttons, .hat = hat,
                .hold_ms = hold_ms, .count = count};
    enqueue_job(&j);
}

void hid_pad_type(const char *text, uint16_t hold_ms, bool enter) {
    PadJob j = {.kind = JOB_TYPE, .hold_ms = hold_ms ? hold_ms : PAD_TYPE_HOLD_MS,
                .enter = enter, .text = NULL};
    if (text) {
        size_t n = strlen(text);
        if (n > 4096) n = 4096;
        j.text = malloc(n + 1);
        if (!j.text) return;
        memcpy(j.text, text, n);
        j.text[n] = '\0';
    }
    if (!enqueue_job(&j)) free(j.text);
}

void hid_pad_init(void) {
    s_queue = xQueueCreate(8, sizeof(PadJob));
    configASSERT(s_queue);
    xTaskCreate(action_task, "hidpad", 8192, NULL, 5, NULL);

#if PAD_AUTO_MACRO
    while (!s_mounted) vTaskDelay(pdMS_TO_TICKS(200));
    vTaskDelay(pdMS_TO_TICKS(2000));
    for (;;) {
        hid_pad_bootstrap();
        vTaskDelay(pdMS_TO_TICKS(500));
        while (hid_pad_busy()) vTaskDelay(pdMS_TO_TICKS(100));
#if !PAD_AUTO_REPEAT
        break;
#else
        vTaskDelay(pdMS_TO_TICKS(PAD_AUTO_DELAY_MS));
#endif
    }
#endif
}

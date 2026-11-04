/*
 * Wi-Fi + command interface for the USB HID pad.
 *
 * Two transports share one line-based command parser:
 *
 *   - HTTP  : GET /            -> control page
 *             POST /cmd        -> body is one or more command lines
 *             GET  /status     -> JSON status
 *   - TCP   : port 9060, one command per line, reply ends with "END"
 *
 * Commands (see README for the full list):
 *   STATUS | STOP | BOOTSTRAP | PRESS <btns> [hold_ms] [count]
 *   TYPE <enter0|1> <text> | WIFI <ssid> <pass> | REBOOT | HELP
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <stdbool.h>
#include <strings.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"

#include "esp_log.h"
#include "esp_err.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_http_server.h"
#include "esp_ota_ops.h"
#include "nvs.h"
#include "nvs_flash.h"

#include "lwip/sockets.h"
#include "lwip/inet.h"

#include "hid_pad.h"
#include "net_api.h"

static const char *TAG = "netapi";

#define NVS_NS      "netpad"
#define AP_SSID     "ESPNETPAD"
#define AP_PASS     "netpad12345"
#define HTTP_PORT   80
#define TCP_PORT    9060
#define CMD_MAX     4096

#ifndef NETPAD_WIFI_SSID
#define NETPAD_WIFI_SSID ""
#endif
#ifndef NETPAD_WIFI_PASS
#define NETPAD_WIFI_PASS ""
#endif

static esp_netif_t *s_sta_netif;
static bool s_have_creds;

static bool s_servers_started;
static char s_ip[16] = "0.0.0.0";
static char s_ssid[33];
static char s_lang[4] = "en";     /* UI language: en | zh */
static char s_region[8] = "jp";   /* macro region: jp | intl */

/* ------------------------------------------------------------------ output */

typedef struct {
    char *buf;
    size_t len;
    size_t cap;
} Sink;

static void sink_printf(Sink *s, const char *fmt, ...) {
    if (!s || s->len + 1 >= s->cap) return;
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(s->buf + s->len, s->cap - s->len, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    s->len += (size_t)n;
    if (s->len >= s->cap) s->len = s->cap - 1;
}

/* ----------------------------------------------------------------- parsing */

static bool button_mask(const char *name, uint16_t *mask) {
    static const struct { const char *n; uint16_t m; } table[] = {
        {"a", SWITCH_USB_MASK_A},           {"b", SWITCH_USB_MASK_B},
        {"x", SWITCH_USB_MASK_X},           {"y", SWITCH_USB_MASK_Y},
        {"l", SWITCH_USB_MASK_L},           {"r", SWITCH_USB_MASK_R},
        {"zl", SWITCH_USB_MASK_ZL},         {"zr", SWITCH_USB_MASK_ZR},
        {"minus", SWITCH_USB_MASK_MINUS},   {"plus", SWITCH_USB_MASK_PLUS},
        {"l3", SWITCH_USB_MASK_L3},         {"r3", SWITCH_USB_MASK_R3},
        {"home", SWITCH_USB_MASK_HOME},     {"capture", SWITCH_USB_MASK_CAPTURE},
    };
    for (size_t i = 0; i < sizeof(table) / sizeof(table[0]); i++) {
        if (strcasecmp(name, table[i].n) == 0) {
            *mask = table[i].m;
            return true;
        }
    }
    return false;
}

/* Returns true if `name` is a direction; sets dx/dy. */
static bool direction(const char *name, int *dx, int *dy) {
    if (strcasecmp(name, "up") == 0)    { *dx = 0;  *dy = 1;  return true; }
    if (strcasecmp(name, "down") == 0)  { *dx = 0;  *dy = -1; return true; }
    if (strcasecmp(name, "left") == 0)  { *dx = -1; *dy = 0;  return true; }
    if (strcasecmp(name, "right") == 0) { *dx = 1;  *dy = 0;  return true; }
    return false;
}

static uint8_t hat_from_xy(int dx, int dy) {
    if (dx == 0 && dy == 0) return SWITCH_USB_HAT_NOTHING;
    if (dx == 0 && dy > 0) return SWITCH_USB_HAT_UP;
    if (dx > 0 && dy > 0) return SWITCH_USB_HAT_UPRIGHT;
    if (dx > 0 && dy == 0) return SWITCH_USB_HAT_RIGHT;
    if (dx > 0 && dy < 0) return SWITCH_USB_HAT_DOWNRIGHT;
    if (dx == 0 && dy < 0) return SWITCH_USB_HAT_DOWN;
    if (dx < 0 && dy < 0) return SWITCH_USB_HAT_DOWNLEFT;
    if (dx < 0 && dy == 0) return SWITCH_USB_HAT_LEFT;
    return SWITCH_USB_HAT_UPLEFT;
}

/* "a+b+up" -> buttons mask + hat */
static bool parse_button_spec(char *spec, uint16_t *buttons, uint8_t *hat,
                              Sink *out) {
    uint16_t mask = 0;
    int dx = 0, dy = 0;
    char *save = NULL;
    for (char *tok = strtok_r(spec, "+", &save); tok;
         tok = strtok_r(NULL, "+", &save)) {
        uint16_t m;
        if (button_mask(tok, &m)) {
            mask |= m;
            continue;
        }
        int tx, ty;
        if (direction(tok, &tx, &ty)) {
            dx += tx;
            dy += ty;
            continue;
        }
        sink_printf(out, "ERR unknown button '%s'\n", tok);
        return false;
    }
    *buttons = mask;
    *hat = hat_from_xy(dx, dy);
    return true;
}

static void decode_escapes(char *s) {
    char *w = s;
    for (char *r = s; *r; r++) {
        if (*r == '\\' && r[1]) {
            r++;
            switch (*r) {
                case 'n': *w++ = '\n'; break;
                case 't': *w++ = '\t'; break;
                case '\\': *w++ = '\\'; break;
                default: *w++ = *r; break;
            }
        } else {
            *w++ = *r;
        }
    }
    *w = '\0';
}

/* ------------------------------------------------------------- wifi config */

static int sta_rssi(void) {
    wifi_ap_record_t ap;
    if (s_have_creds && esp_wifi_sta_get_ap_info(&ap) == ESP_OK) return ap.rssi;
    return -127;
}

static void cfg_get_str(const char *key, char *out, size_t cap, const char *def) {
    out[0] = '\0';
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        size_t n = cap;
        if (nvs_get_str(h, key, out, &n) != ESP_OK) out[0] = '\0';
        nvs_close(h);
    }
    if (!out[0]) snprintf(out, cap, "%s", def);
}

static esp_err_t cfg_set_str(const char *key, const char *val) {
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    err = nvs_set_str(h, key, val);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err;
}

static void wifi_load(char *ssid, size_t ssid_len, char *pass, size_t pass_len) {
    ssid[0] = '\0';
    pass[0] = '\0';
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        size_t n = ssid_len;
        if (nvs_get_str(h, "ssid", ssid, &n) != ESP_OK) ssid[0] = '\0';
        n = pass_len;
        if (nvs_get_str(h, "pass", pass, &n) != ESP_OK) pass[0] = '\0';
        nvs_close(h);
    }
    if (ssid[0] == '\0' && NETPAD_WIFI_SSID[0]) {
        snprintf(ssid, ssid_len, "%s", NETPAD_WIFI_SSID);
        snprintf(pass, pass_len, "%s", NETPAD_WIFI_PASS);
    }
}

static esp_err_t wifi_save(const char *ssid, const char *pass) {
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    err = nvs_set_str(h, "ssid", ssid);
    if (err == ESP_OK) err = nvs_set_str(h, "pass", pass);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err;
}

/* -------------------------------------------------------------- dispatcher */

static void exec_cmd(char *line, Sink *out) {
    while (*line == ' ' || *line == '\t') line++;
    size_t n = strlen(line);
    while (n && (line[n - 1] == '\r' || line[n - 1] == '\n' ||
                 line[n - 1] == ' ')) {
        line[--n] = '\0';
    }
    if (n == 0 || line[0] == '#') return;

    char *cmd = line;
    char *rest = line;
    while (*rest && *rest != ' ') rest++;
    if (*rest) *rest++ = '\0';
    while (*rest == ' ') rest++;

    if (strcasecmp(cmd, "HELP") == 0) {
        sink_printf(out,
            "OK commands:\n"
            "  STATUS\n"
            "  STOP\n"
            "  BOOTSTRAP [jp|intl]       run the Civ7 macro (saves region)\n"
            "  SET LANG en|zh            UI language\n"
            "  SET REGION jp|intl        macro region\n"
            "  PRESS <btns> [hold] [n]   btns: a+b+up, hold_ms, count\n"
            "  TYPE <enter 0|1> <text>   type ASCII, \\n = newline\n"
            "  WIFI <ssid> <pass>        save creds and reboot\n"
            "  REBOOT\n"
            "END\n");
    } else if (strcasecmp(cmd, "STATUS") == 0) {
        sink_printf(out,
            "OK mounted=%d busy=%d ip=%s ap=192.168.4.1 ssid=%s rssi=%d "
            "uptime=%llds slot=%s lang=%s region=%s\nEND\n",
            hid_pad_mounted(), hid_pad_busy(), s_ip, s_ssid, sta_rssi(),
            (long long)(esp_timer_get_time() / 1000000),
            esp_ota_get_running_partition()->label, s_lang, s_region);
    } else if (strcasecmp(cmd, "STOP") == 0) {
        hid_pad_stop();
        sink_printf(out, "OK stopping\nEND\n");
    } else if (strcasecmp(cmd, "BOOTSTRAP") == 0) {
        if (rest[0]) {
            bool jp;
            if (strcasecmp(rest, "jp") == 0) {
                jp = true;
            } else if (strcasecmp(rest, "intl") == 0) {
                jp = false;
            } else {
                sink_printf(out, "ERR usage: BOOTSTRAP [jp|intl]\nEND\n");
                return;
            }
            snprintf(s_region, sizeof(s_region), "%s", jp ? "jp" : "intl");
            hid_pad_set_region(jp);
            cfg_set_str("region", s_region);
        }
        if (!hid_pad_mounted()) {
            sink_printf(out, "ERR usb not mounted\nEND\n");
        } else {
            hid_pad_bootstrap();
            sink_printf(out, "OK bootstrap queued region=%s\nEND\n", s_region);
        }
    } else if (strcasecmp(cmd, "SET") == 0) {
        char key[16] = {0};
        char val[16] = {0};
        if (sscanf(rest, "%15s %15s", key, val) != 2) {
            sink_printf(out, "ERR usage: SET LANG en|zh / SET REGION jp|intl\nEND\n");
            return;
        }
        if (strcasecmp(key, "LANG") == 0) {
            if (strcasecmp(val, "en") && strcasecmp(val, "zh")) {
                sink_printf(out, "ERR lang must be en|zh\nEND\n");
                return;
            }
            snprintf(s_lang, sizeof(s_lang), "%s",
                     strcasecmp(val, "zh") == 0 ? "zh" : "en");
            cfg_set_str("lang", s_lang);
            sink_printf(out, "OK lang=%s\nEND\n", s_lang);
        } else if (strcasecmp(key, "REGION") == 0) {
            bool jp;
            if (strcasecmp(val, "jp") == 0) {
                jp = true;
            } else if (strcasecmp(val, "intl") == 0) {
                jp = false;
            } else {
                sink_printf(out, "ERR region must be jp|intl\nEND\n");
                return;
            }
            snprintf(s_region, sizeof(s_region), "%s", jp ? "jp" : "intl");
            hid_pad_set_region(jp);
            cfg_set_str("region", s_region);
            sink_printf(out, "OK region=%s\nEND\n", s_region);
        } else {
            sink_printf(out, "ERR unknown setting '%s'\nEND\n", key);
        }
    } else if (strcasecmp(cmd, "PRESS") == 0 || strcasecmp(cmd, "PAD") == 0) {
        char spec[128];
        unsigned hold = 60, count = 1;
        int got = sscanf(rest, "%127s %u %u", spec, &hold, &count);
        if (got < 1) {
            sink_printf(out, "ERR usage: PRESS <btns> [hold_ms] [count]\nEND\n");
            return;
        }
        uint16_t buttons;
        uint8_t hat;
        if (!parse_button_spec(spec, &buttons, &hat, out)) {
            sink_printf(out, "END\n");
            return;
        }
        if (!hid_pad_mounted()) {
            sink_printf(out, "ERR usb not mounted\nEND\n");
            return;
        }
        hid_pad_press(buttons, hat, (uint16_t)hold, (uint16_t)count);
        sink_printf(out, "OK press mask=0x%04x hat=%u hold=%u count=%u\nEND\n",
                    buttons, hat, hold, count);
    } else if (strcasecmp(cmd, "TYPE") == 0) {
        if (rest[0] != '0' && rest[0] != '1') {
            sink_printf(out, "ERR usage: TYPE <enter 0|1> <text>\nEND\n");
            return;
        }
        bool enter = rest[0] == '1';
        char *text = rest + 1;
        while (*text == ' ') text++;
        decode_escapes(text);
        if (!hid_pad_mounted()) {
            sink_printf(out, "ERR usb not mounted\nEND\n");
            return;
        }
        hid_pad_type(text, 30, enter);
        sink_printf(out, "OK type %u chars enter=%d\nEND\n",
                    (unsigned)strlen(text), enter);
    } else if (strcasecmp(cmd, "WIFI") == 0) {
        char ssid[33] = {0};
        char pass[65] = {0};
        if (sscanf(rest, "%32s %64s", ssid, pass) != 2) {
            sink_printf(out, "ERR usage: WIFI <ssid> <pass>\nEND\n");
            return;
        }
        if (wifi_save(ssid, pass) != ESP_OK) {
            sink_printf(out, "ERR nvs write failed\nEND\n");
            return;
        }
        sink_printf(out, "OK saved; rebooting\nEND\n");
    } else if (strcasecmp(cmd, "REBOOT") == 0) {
        sink_printf(out, "OK rebooting\nEND\n");
    } else {
        sink_printf(out, "ERR unknown command '%s' (try HELP)\nEND\n", cmd);
    }
}

/* -------------------------------------------------------------- http layer */

/* Embedded with EMBED_TXTFILES (see main/CMakeLists.txt). */
extern const uint8_t index_html_start[] asm("_binary_index_html_start");
extern const uint8_t index_html_end[] asm("_binary_index_html_end");

static esp_err_t root_get(httpd_req_t *req) {
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, (const char *)index_html_start,
                           index_html_end - index_html_start);
}

static esp_err_t status_get(httpd_req_t *req) {
    char buf[320];
    snprintf(buf, sizeof(buf),
             "{\"mounted\":%s,\"busy\":%s,\"ip\":\"%s\","
             "\"ap\":\"192.168.4.1\",\"ssid\":\"%s\",\"rssi\":%d,"
             "\"uptime\":%lld,\"slot\":\"%s\",\"lang\":\"%s\","
             "\"region\":\"%s\"}",
             hid_pad_mounted() ? "true" : "false",
             hid_pad_busy() ? "true" : "false", s_ip, s_ssid, sta_rssi(),
             (long long)(esp_timer_get_time() / 1000000),
             esp_ota_get_running_partition()->label, s_lang, s_region);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, buf, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t cmd_post(httpd_req_t *req) {
    static char body[CMD_MAX];
    int total = req->content_len;
    if (total <= 0 || total >= CMD_MAX) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad length");
    }
    int got = 0;
    while (got < total) {
        int n = httpd_req_recv(req, body + got, total - got);
        if (n <= 0) return ESP_FAIL;
        got += n;
    }
    body[got] = '\0';

    static char resp[CMD_MAX];
    Sink sink = {resp, 0, sizeof(resp)};
    resp[0] = '\0';
    char *save = NULL;
    for (char *line = strtok_r(body, "\n", &save); line;
         line = strtok_r(NULL, "\n", &save)) {
        exec_cmd(line, &sink);
    }
    if (sink.len == 0) sink_printf(&sink, "OK\nEND\n");

    if (strstr(resp, "OK rebooting") || strstr(resp, "OK saved; rebooting")) {
        httpd_resp_set_type(req, "text/plain");
        httpd_resp_send(req, resp, sink.len);
        vTaskDelay(pdMS_TO_TICKS(300));
        esp_restart();
    }
    httpd_resp_set_type(req, "text/plain");
    return httpd_resp_send(req, resp, sink.len);
}


typedef struct {
    char ssid[33];
    int8_t rssi;
    uint8_t auth;
} ScanEntry;

static int cmp_rssi(const void *a, const void *b) {
    return ((const ScanEntry *)b)->rssi - ((const ScanEntry *)a)->rssi;
}

static size_t json_escape(char *dst, size_t cap, const char *src) {
    size_t o = 0;
    for (const unsigned char *p = (const unsigned char *)src; *p; p++) {
        const char *rep = NULL;
        char tmp[8];
        if (*p == '"' || *p == '\\') {
            tmp[0] = '\\'; tmp[1] = (char)*p; tmp[2] = 0; rep = tmp;
        } else if (*p < 0x20) {
            continue;
        }
        if (rep) {
            size_t n = strlen(rep);
            if (o + n >= cap) break;
            memcpy(dst + o, rep, n);
            o += n;
        } else {
            if (o + 1 >= cap) break;
            dst[o++] = (char)*p;
        }
    }
    dst[o] = 0;
    return o;
}

static esp_err_t wifi_scan_get(httpd_req_t *req) {
    static char buf[4096];
    static ScanEntry list[24];
    Sink sink = {buf, 0, sizeof(buf)};
    buf[0] = 0;

    wifi_scan_config_t cfg = {0};
    cfg.show_hidden = 0;
    int n = 0;
    if (esp_wifi_scan_start(&cfg, true) == ESP_OK) {
        uint16_t num = 0;
        esp_wifi_scan_get_ap_num(&num);
        if (num > 64) num = 64;
        if (num) {
            wifi_ap_record_t *recs = calloc(num, sizeof(*recs));
            if (recs) {
                esp_wifi_scan_get_ap_records(&num, recs);
                for (uint16_t i = 0; i < num && n < 24; i++) {
                    if (!recs[i].ssid[0]) continue;
                    int found = -1;
                    for (int k = 0; k < n; k++) {
                        if (strcmp(list[k].ssid, (char *)recs[i].ssid) == 0) {
                            found = k;
                            break;
                        }
                    }
                    if (found >= 0) {
                        if (recs[i].rssi > list[found].rssi) list[found].rssi = recs[i].rssi;
                        continue;
                    }
                    memcpy(list[n].ssid, recs[i].ssid, 32);
                    list[n].ssid[32] = 0;
                    list[n].rssi = recs[i].rssi;
                    list[n].auth = recs[i].authmode != WIFI_AUTH_OPEN;
                    n++;
                }
                free(recs);
            }
        }
    }
    qsort(list, n, sizeof(list[0]), cmp_rssi);

    sink_printf(&sink, "{\"nets\":[");
    for (int i = 0; i < n; i++) {
        char esc[80];
        json_escape(esc, sizeof(esc), list[i].ssid);
        sink_printf(&sink, "%s{\"ssid\":\"%s\",\"rssi\":%d,\"auth\":%d}",
                    i ? "," : "", esc, list[i].rssi, list[i].auth);
    }
    sink_printf(&sink, "]}");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, buf, sink.len);
}


static esp_err_t ota_post(httpd_req_t *req) {
    if (req->content_len <= 0) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "empty body");
    }
    const esp_partition_t *part = esp_ota_get_next_update_partition(NULL);
    if (!part) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                                   "no OTA partition");
    }
    ESP_LOGI(TAG, "OTA -> %s (%d bytes)", part->label, req->content_len);

    esp_ota_handle_t handle = 0;
    esp_err_t err = esp_ota_begin(part, OTA_WITH_SEQUENTIAL_WRITES, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ota_begin: %s", esp_err_to_name(err));
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                                   esp_err_to_name(err));
    }

    static char buf[2048];
    int remaining = req->content_len;
    int written = 0;
    while (remaining > 0) {
        int to_read = remaining > (int)sizeof(buf) ? (int)sizeof(buf) : remaining;
        int n = httpd_req_recv(req, buf, to_read);
        if (n == HTTPD_SOCK_ERR_TIMEOUT) continue;
        if (n <= 0) {
            esp_ota_abort(handle);
            ESP_LOGE(TAG, "recv failed at %d", written);
            return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                                       "recv failed");
        }
        err = esp_ota_write(handle, buf, n);
        if (err != ESP_OK) {
            esp_ota_abort(handle);
            ESP_LOGE(TAG, "ota_write: %s", esp_err_to_name(err));
            return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                                       esp_err_to_name(err));
        }
        written += n;
        remaining -= n;
    }

    err = esp_ota_end(handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ota_end: %s", esp_err_to_name(err));
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                                   esp_err_to_name(err));
    }
    err = esp_ota_set_boot_partition(part);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "set_boot: %s", esp_err_to_name(err));
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                                   esp_err_to_name(err));
    }

    ESP_LOGI(TAG, "OTA done: %d bytes -> %s, rebooting", written, part->label);
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_send(req, "OK rebooting", HTTPD_RESP_USE_STRLEN);
    vTaskDelay(pdMS_TO_TICKS(600));
    esp_restart();
    return ESP_OK;
}

static void start_http(void) {
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.server_port = HTTP_PORT;
    cfg.stack_size = 8192;
    cfg.max_uri_handlers = 10;
    cfg.lru_purge_enable = true;
    httpd_handle_t server = NULL;
    if (httpd_start(&server, &cfg) != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start failed");
        return;
    }
    httpd_uri_t root = {.uri = "/", .method = HTTP_GET, .handler = root_get};
    httpd_uri_t status = {.uri = "/status", .method = HTTP_GET,
                          .handler = status_get};
    httpd_uri_t cmd = {.uri = "/cmd", .method = HTTP_POST, .handler = cmd_post};
    httpd_uri_t scan = {.uri = "/wifi/scan", .method = HTTP_GET,
                        .handler = wifi_scan_get};
    httpd_uri_t ota = {.uri = "/ota", .method = HTTP_POST, .handler = ota_post};
    httpd_register_uri_handler(server, &root);
    httpd_register_uri_handler(server, &status);
    httpd_register_uri_handler(server, &scan);
    httpd_register_uri_handler(server, &ota);
    httpd_register_uri_handler(server, &cmd);
    ESP_LOGI(TAG, "http on :%d", HTTP_PORT);
}

/* --------------------------------------------------------------- tcp layer */

static void tcp_client_task(void *arg) {
    int fd = (int)(intptr_t)arg;
    char line[CMD_MAX];
    size_t used = 0;
    char resp[CMD_MAX];
    for (;;) {
        int n = recv(fd, line + used, sizeof(line) - used - 1, 0);
        if (n <= 0) break;
        used += (size_t)n;
        line[used] = '\0';
        char *start = line;
        char *nl;
        while ((nl = strchr(start, '\n')) != NULL) {
            *nl = '\0';
            Sink sink = {resp, 0, sizeof(resp)};
            resp[0] = '\0';
            exec_cmd(start, &sink);
            if (sink.len == 0) sink_printf(&sink, "OK\nEND\n");
            send(fd, resp, sink.len, 0);
            start = nl + 1;
            if (strstr(resp, "OK rebooting") || strstr(resp, "OK saved; rebooting")) {
                vTaskDelay(pdMS_TO_TICKS(300));
                esp_restart();
            }
        }
        used = strlen(start);
        if (used && start != line) memmove(line, start, used + 1);
        if (used >= sizeof(line) - 1) {
            const char *err = "ERR line too long\nEND\n";
            send(fd, err, strlen(err), 0);
            used = 0;
        }
    }
    close(fd);
    vTaskDelete(NULL);
}

static void tcp_server_task(void *arg) {
    (void)arg;
    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) {
        ESP_LOGE(TAG, "socket failed");
        vTaskDelete(NULL);
        return;
    }
    int yes = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(TCP_PORT);
    if (bind(s, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        ESP_LOGE(TAG, "bind %d failed", TCP_PORT);
        close(s);
        vTaskDelete(NULL);
        return;
    }
    listen(s, 4);
    ESP_LOGI(TAG, "tcp on :%d", TCP_PORT);
    for (;;) {
        struct sockaddr_in peer;
        socklen_t plen = sizeof(peer);
        int c = accept(s, (struct sockaddr *)&peer, &plen);
        if (c < 0) {
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }
        xTaskCreate(tcp_client_task, "netcli", 12288, (void *)(intptr_t)c, 4, NULL);
    }
}

static void start_servers(void) {
    if (s_servers_started) return;
    s_servers_started = true;
    start_http();
    xTaskCreate(tcp_server_task, "nettcp", 4096, NULL, 4, NULL);
}

/* -------------------------------------------------------------------- wifi */

static void on_got_ip(esp_netif_t *netif) {
    esp_netif_ip_info_t ip;
    if (esp_netif_get_ip_info(netif, &ip) == ESP_OK) {
        snprintf(s_ip, sizeof(s_ip), IPSTR, IP2STR(&ip.ip));
        ESP_LOGI(TAG, "ip %s", s_ip);
    }
    wifi_ap_record_t ap;
    if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
        snprintf(s_ssid, sizeof(s_ssid), "%s", (char *)ap.ssid);
    }
    start_servers();
}

static void wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data) {
    if (base == WIFI_EVENT) {
        if (id == WIFI_EVENT_STA_START) {
            if (s_have_creds) esp_wifi_connect();
        } else if (id == WIFI_EVENT_STA_DISCONNECTED) {
            if (s_have_creds) {
                ESP_LOGW(TAG, "sta disconnected; retrying");
                vTaskDelay(pdMS_TO_TICKS(1000));
                esp_wifi_connect();
            }
        } else if (id == WIFI_EVENT_AP_STACONNECTED) {
            ESP_LOGI(TAG, "client joined setup AP");
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        on_got_ip(s_sta_netif);
    }
}

static void copy_str(char *dst, size_t cap, const char *src) {
    if (!dst || cap == 0) return;
    if (!src) src = "";
    size_t n = strlen(src);
    if (n >= cap) n = cap - 1;
    memcpy(dst, src, n);
    dst[n] = '\0';
}

static void wifi_start(const char *ssid, const char *pass) {
    esp_netif_create_default_wifi_ap();
    s_sta_netif = esp_netif_create_default_wifi_sta();

    wifi_config_t ap = {0};
    copy_str((char *)ap.ap.ssid, sizeof(ap.ap.ssid), AP_SSID);
    ap.ap.ssid_len = strlen(AP_SSID);
    copy_str((char *)ap.ap.password, sizeof(ap.ap.password), AP_PASS);
    ap.ap.max_connection = 4;
    ap.ap.authmode = WIFI_AUTH_WPA2_PSK;

    wifi_config_t sta = {0};
    copy_str((char *)sta.sta.ssid, sizeof(sta.sta.ssid), ssid);
    copy_str((char *)sta.sta.password, sizeof(sta.sta.password), pass);
    sta.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap));
    if (ssid && ssid[0]) {
        s_have_creds = true;
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta));
    }
    ESP_ERROR_CHECK(esp_wifi_start());
    esp_wifi_set_ps(WIFI_PS_NONE);

    snprintf(s_ip, sizeof(s_ip), "192.168.4.1");
    if (ssid && ssid[0]) {
        snprintf(s_ssid, sizeof(s_ssid), "%s", ssid);
        ESP_LOGI(TAG, "setup AP %s up at http://192.168.4.1, joining '%s'",
                 AP_SSID, ssid);
    } else {
        snprintf(s_ssid, sizeof(s_ssid), "(unset)");
        ESP_LOGW(TAG, "no Wi-Fi creds: join %s / %s then open http://192.168.4.1",
                 AP_SSID, AP_PASS);
    }
    start_servers();
}

esp_err_t net_api_start(void) {
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    if (err != ESP_OK) return err;

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                               &wifi_event, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                               &wifi_event, NULL));

    cfg_get_str("lang", s_lang, sizeof(s_lang), "en");
    cfg_get_str("region", s_region, sizeof(s_region), "jp");
    hid_pad_set_region(strcasecmp(s_region, "intl") != 0);

    char ssid[33], pass[65];
    wifi_load(ssid, sizeof(ssid), pass, sizeof(pass));
    wifi_start(ssid, pass);
    return ESP_OK;
}

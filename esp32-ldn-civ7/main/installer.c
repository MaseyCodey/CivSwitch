/*
 * One-shot installer: after the LDN injection brought the Remote JS Loader up,
 * join the home Wi-Fi and push the installer (template + apply JS) from the
 * `storage` flash partition to the console's loader on :9026.
 *
 * Storage layout:
 *   0x000000  u32 LE length + base64(save_template.bin)
 *   0x180000  u32 LE length + installer apply JS
 */
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_partition.h"
#include "nvs.h"
#include "lwip/sockets.h"
#include "lwip/inet.h"

#include "installer.h"
#include "civ7.h"

static const char *TAG = "install";

#define LOADER_PORT 9026
#define CHUNK 131072
#define TPL_OFF 0x000000
#define APPLY_OFF 0x180000

static const esp_partition_t *s_part;
static char s_ip[32];
static char s_loader_ip[32];

/* --------------------------------------------------------------- wifi sta */

static bool sta_connect(void) {
    char ssid[33] = {0}, pass[65] = {0};
    nvs_handle_t h;
    if (nvs_open("netpad", NVS_READONLY, &h) == ESP_OK) {
        size_t n = sizeof(ssid);
        if (nvs_get_str(h, "ssid", ssid, &n) != ESP_OK) ssid[0] = 0;
        n = sizeof(pass);
        if (nvs_get_str(h, "pass", pass, &n) != ESP_OK) pass[0] = 0;
        nvs_close(h);
    }
    if (!ssid[0]) {
        ESP_LOGE(TAG, "no Wi-Fi credentials in NVS (netpad)");
        return false;
    }
    ESP_LOGI(TAG, "joining '%s'", ssid);
    esp_netif_t *netif = esp_netif_create_default_wifi_sta();
    wifi_config_t cfg = {0};
    memcpy(cfg.sta.ssid, ssid, strlen(ssid));
    memcpy(cfg.sta.password, pass, strlen(pass));
    cfg.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    esp_wifi_set_mode(WIFI_MODE_STA);
    esp_wifi_set_config(WIFI_IF_STA, &cfg);
    esp_wifi_start();
    esp_wifi_connect();
    for (int i = 0; i < 150; i++) {
        esp_netif_ip_info_t ip;
        if (esp_netif_get_ip_info(netif, &ip) == ESP_OK && ip.ip.addr != 0) {
            snprintf(s_ip, sizeof(s_ip), IPSTR, IP2STR(&ip.ip));
            ESP_LOGI(TAG, "ip %s", s_ip);
            return true;
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    ESP_LOGE(TAG, "no DHCP address");
    return false;
}

/* ---------------------------------------------------------- loader socket */

static int connect_to(const char *ip) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    struct sockaddr_in a = {0};
    a.sin_family = AF_INET;
    a.sin_port = htons(LOADER_PORT);
    a.sin_addr.s_addr = inet_addr(ip);
    struct timeval tv = {.tv_sec = 2, .tv_usec = 0};
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    if (connect(fd, (struct sockaddr *)&a, sizeof(a)) != 0) {
        close(fd);
        return -1;
    }
    tv.tv_sec = 20;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    return fd;
}

static int find_loader(void) {
    char buf[32];
    /* the console keeps its DHCP address in practice; try it first */
    const char *first = "10.2.0.186";
    int fd = connect_to(first);
    if (fd >= 0) { snprintf(s_loader_ip, sizeof(s_loader_ip), "%s", first); ESP_LOGI(TAG, "loader at %s", first); return fd; }
    ESP_LOGI(TAG, "probing /24 for :9026 ...");
    for (int i = 1; i < 255; i++) {
        snprintf(buf, sizeof(buf), "10.2.0.%d", i);
        fd = connect_to(buf);
        if (fd >= 0) { snprintf(s_loader_ip, sizeof(s_loader_ip), "%s", buf); ESP_LOGI(TAG, "loader at %s", buf); return fd; }
        vTaskDelay(pdMS_TO_TICKS(5));
    }
    ESP_LOGE(TAG, "no loader found");
    return -1;
}

/* Send one JS command, read the log until the loader closes. */
static int run_js(int fd, const char *prefix, uint32_t blob_off, size_t blob_len,
                  const char *suffix, char *out, size_t out_cap) {
    static uint8_t buf[2048];
    if (prefix) send(fd, prefix, strlen(prefix), 0);
    size_t left = blob_len, off = blob_off;
    while (left) {
        size_t n = left > sizeof(buf) ? sizeof(buf) : left;
        esp_err_t r = esp_partition_read(s_part, off, buf, n);
        if (r != ESP_OK) { close(fd); return -1; }
        size_t sent = 0;
        while (sent < n) {
            int w = send(fd, buf + sent, n - sent, 0);
            if (w <= 0) { close(fd); return -1; }
            sent += w;
        }
        off += n; left -= n;
    }
    if (suffix) send(fd, suffix, strlen(suffix), 0);
    shutdown(fd, SHUT_WR);
    size_t used = 0;
    for (;;) {
        int n = recv(fd, buf, sizeof(buf) - 1, 0);
        if (n <= 0) break;
        if (out && used + n < out_cap - 1) {
            memcpy(out + used, buf, n);
            used += n;
            out[used] = 0;
        }
    }
    close(fd);
    return 0;
}

static uint32_t blob_len(uint32_t off) {
    uint32_t v = 0;
    if (esp_partition_read(s_part, off, &v, 4) != ESP_OK) return 0;
    return v;
}

/* --------------------------------------------------------------- install */

/* Fully offline: stream the installer over the LDN link to the UDP receiver
 * that core_udp.js binds on :9027.
 *
 * Board -> console: A<tpl>:<js>:<board ip>, T<i>:<b64>, J<i>:<b64>, G
 * Console -> board: D (all received), t<i>/j<i> (resend that chunk)
 *
 * One fast pass plus a NAK repair window: the console starts the installer as
 * soon as the last chunk lands instead of waiting for blind retransmissions. */
#define UDP_CHUNK 1100
#define UDP_PORT 9027

static uint32_t s_tpl_len, s_js_len;
static int s_need_t, s_need_j;
static QueueHandle_t s_req_q;
static volatile bool s_done;

static void send_chunk(bool js, int i) {
    if (js ? (i < 0 || i >= s_need_j) : (i < 0 || i >= s_need_t)) return;
    uint32_t base = js ? APPLY_OFF : TPL_OFF;
    uint32_t total = js ? s_js_len : s_tpl_len;
    uint32_t off = base + 4 + (uint32_t)i * UDP_CHUNK;
    size_t len = total - (uint32_t)i * UDP_CHUNK;
    if (len > UDP_CHUNK) len = UDP_CHUNK;

    static uint8_t buf[16 + UDP_CHUNK];
    int pfx = snprintf((char *)buf, 16, js ? "J%d:" : "T%d:", i);
    if (esp_partition_read(s_part, off, buf + pfx, len) != ESP_OK) return;
    int r = -1;
    for (int k = 0; k < 20 && r != 0; k++) {
        r = civ7_udp_send(UDP_PORT, buf, pfx + len);
        if (r != 0) vTaskDelay(pdMS_TO_TICKS(1));
    }
}

/* Called from the Wi-Fi rx callback for UDP datagrams sent to :9027. */
void installer_udp_rx(const uint8_t *data, size_t len) {
    if (len == 1 && data[0] == 'D') { s_done = true; return; }
    if (len < 2 || (data[0] != 't' && data[0] != 'j')) return;
    char tmp[12];
    size_t n = len - 1;
    if (n > sizeof(tmp) - 1) n = sizeof(tmp) - 1;
    memcpy(tmp, data + 1, n);
    tmp[n] = 0;
    int idx = atoi(tmp);
    if (idx < 0 || !s_req_q) return;
    uint32_t v = ((data[0] == 'j') ? (1u << 16) : 0u) | ((uint32_t)idx & 0xffff);
    xQueueSend(s_req_q, &v, 0);
}

void installer_udp_run(void) {
    s_part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, "storage");
    if (!s_part) { ESP_LOGE(TAG, "no storage partition"); return; }
    s_tpl_len = blob_len(TPL_OFF);
    s_js_len = blob_len(APPLY_OFF);
    ESP_LOGI(TAG, "UDP installer: tpl %u, js %u", s_tpl_len, s_js_len);
    if (!s_tpl_len || !s_js_len) { ESP_LOGE(TAG, "blobs missing"); return; }

    vTaskDelay(pdMS_TO_TICKS(800));   /* let the chat payload start the receiver */

    s_need_t = (s_tpl_len + UDP_CHUNK - 1) / UDP_CHUNK;
    s_need_j = (s_js_len + UDP_CHUNK - 1) / UDP_CHUNK;
    s_done = false;
    s_req_q = xQueueCreate(512, sizeof(uint32_t));

    uint8_t ip[4] = {0};
    civ7_get_local_ip(ip);
    char hdr[64];
    int hl = snprintf(hdr, sizeof(hdr), "A%d:%d:%u.%u.%u.%u",
                      s_need_t, s_need_j, ip[0], ip[1], ip[2], ip[3]);
    for (int k = 0; k < 3; k++) {
        civ7_udp_send(UDP_PORT, (const uint8_t *)hdr, hl);
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    ESP_LOGI(TAG, "header %s (tpl %d chunks, js %d)", hdr, s_need_t, s_need_j);

    /* One fast pass: the receiver NAKs whatever the radio dropped. */
    for (int i = 0; i < s_need_j && !s_done; i++) {
        send_chunk(true, i);
        vTaskDelay(pdMS_TO_TICKS(1));
    }
    for (int i = 0; i < s_need_t && !s_done; i++) {
        send_chunk(false, i);
        vTaskDelay(pdMS_TO_TICKS(1));
    }
    ESP_LOGI(TAG, "pass done, waiting for NAKs");

    /* Repair window: poll G, drain the receiver's missing-chunk requests. */
    int rounds = 0;
    while (!s_done && rounds < 60) {
        civ7_udp_send(UDP_PORT, (const uint8_t *)"G", 1);
        vTaskDelay(pdMS_TO_TICKS(150));
        uint32_t v;
        while (xQueueReceive(s_req_q, &v, 0) == pdTRUE) {
            send_chunk((v >> 16) != 0, (int)(v & 0xffff));
            vTaskDelay(pdMS_TO_TICKS(2));
        }
        rounds++;
    }
    ESP_LOGI(TAG, "UDP installer %s after %d repair rounds",
             s_done ? "acknowledged" : "timed out", rounds);
}

void installer_run(void) {
    s_part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, "storage");
    if (!s_part) { ESP_LOGE(TAG, "no storage partition"); return; }
    uint32_t tpl_len = blob_len(TPL_OFF);
    uint32_t apply_len = blob_len(APPLY_OFF);
    ESP_LOGI(TAG, "storage: template %u, apply %u", tpl_len, apply_len);
    if (!tpl_len || !apply_len) { ESP_LOGE(TAG, "blobs missing"); return; }

    if (!sta_connect()) return;

    int fd = find_loader();
    if (fd < 0) return;

    ESP_LOGI(TAG, "reset loader state");
    run_js(fd, "log('STARTING INSTALLER');window.__tpl=null;'reset'", 0, 0, NULL, NULL, 0);

    int n = (tpl_len + CHUNK - 1) / CHUNK;
    for (int i = 0; i < n; i++) {
        uint32_t off = TPL_OFF + 4 + (uint32_t)i * CHUNK;
        size_t len = tpl_len - (uint32_t)i * CHUNK;
        if (len > CHUNK) len = CHUNK;
        char prefix[64], suffix[64], out[256] = {0};
        snprintf(prefix, sizeof(prefix), "(window.__tpl=window.__tpl||[]).push('");
        snprintf(suffix, sizeof(suffix), "');'chunk %d/%d'", i + 1, n);
        bool ok = false;
        for (int attempt = 1; attempt <= 3 && !ok; attempt++) {
            fd = s_loader_ip[0] ? connect_to(s_loader_ip) : find_loader();
            if (fd < 0) fd = find_loader();
            if (fd < 0) { vTaskDelay(pdMS_TO_TICKS(300)); continue; }
            /* the loader address may differ from our own ip: use the discovered one */
            run_js(fd, prefix, off, len, suffix, out, sizeof(out));
            if (strstr(out, "Executed successfully")) ok = true;
            else ESP_LOGW(TAG, "chunk %d/%d attempt %d: %s", i + 1, n, attempt, out);
            vTaskDelay(pdMS_TO_TICKS(200));
        }
        ESP_LOGI(TAG, "chunk %d/%d %s", i + 1, n, ok ? "ok" : "FAILED");
        if (!ok) return;
    }

    ESP_LOGI(TAG, "sending apply payload");
    fd = find_loader();
    if (fd < 0) return;
    char out[2048] = {0};
    run_js(fd, NULL, APPLY_OFF + 4, apply_len, NULL, out, sizeof(out));
    ESP_LOGI(TAG, "apply result: %s", out);
}

/*
 * Civ VII LDN game protocol, ported from CivSwitchLDN's ldn/{protocol,transport,lobby}.py.
 * Runs over UDP (port 62056) inside the LDN network we already joined.
 */
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stdlib.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "esp_private/wifi.h"

#include "civ7.h"
#include "chat_payload.h"

static const char *TAG = "civ7";

#define CIV7_PORT 62056
#define TX_RING 64

static uint8_t s_local_ip[4], s_host_ip[4], s_host_mac[6], s_sta_mac[6];
static uint64_t s_next_seq = 1;      /* FireCONN uses sequence 0 */
static uint32_t s_counter;           /* message counter = packets queued before */
static volatile bool s_welcome;
static uint32_t s_slot = 1;
static volatile bool s_done;

static uint8_t *s_tx[TX_RING];
static uint16_t s_tx_len[TX_RING];
static uint64_t s_tx_seq[TX_RING];

static void ip_reversed(const uint8_t ip[4], uint8_t out[4]) {
    out[0] = ip[3]; out[1] = ip[2]; out[2] = ip[1]; out[3] = ip[0];
}

/* ------------------------------------------------------------------ UDP/IP */

static uint32_t sum16(const uint8_t *d, size_t n) {
    uint32_t sum = 0;
    size_t i = 0;
    for (; i + 1 < n; i += 2) sum += ((uint32_t)d[i] << 8) | d[i + 1];
    if (i < n) sum += (uint32_t)d[i] << 8;
    return sum;
}

static uint16_t ip_checksum(const uint8_t *d, size_t n) {
    uint32_t sum = sum16(d, n);
    while (sum >> 16) sum = (sum & 0xffff) + (sum >> 16);
    return (uint16_t)~sum;
}

static int udp_send_port(uint16_t dport, const uint8_t *payload, size_t len) {
    static uint8_t frame[1600];
    size_t udp_len = 8 + len;
    size_t ip_len = 20 + udp_len;
    if (14 + ip_len > sizeof(frame)) return -1;

    memcpy(frame, s_host_mac, 6);
    memcpy(frame + 6, s_sta_mac, 6);
    frame[12] = 0x08; frame[13] = 0x00;

    uint8_t *ip = frame + 14;
    memset(ip, 0, 20);
    ip[0] = 0x45;
    ip[2] = (ip_len >> 8) & 0xff; ip[3] = ip_len & 0xff;
    ip[8] = 64; ip[9] = 17;
    memcpy(ip + 12, s_local_ip, 4);
    memcpy(ip + 16, s_host_ip, 4);
    uint16_t ck = ip_checksum(ip, 20);
    ip[10] = ck >> 8; ip[11] = ck & 0xff;

    uint8_t *udp = ip + 20;
    udp[0] = dport >> 8; udp[1] = dport & 0xff;
    udp[2] = dport >> 8; udp[3] = dport & 0xff;
    udp[4] = (udp_len >> 8) & 0xff; udp[5] = udp_len & 0xff;
    udp[6] = 0; udp[7] = 0;
    memcpy(udp + 8, payload, len);

    /* UDP checksum over pseudo header + UDP */
    uint32_t sum = sum16(s_local_ip, 4) + sum16(s_host_ip, 4);
    sum += 17;
    sum += (uint32_t)udp_len;
    sum += sum16(udp, udp_len);
    while (sum >> 16) sum = (sum & 0xffff) + (sum >> 16);
    uint16_t uck = (uint16_t)~sum;
    if (uck == 0) uck = 0xffff;
    udp[6] = uck >> 8; udp[7] = uck & 0xff;

    int r = esp_wifi_internal_tx(WIFI_IF_STA, frame, 14 + ip_len);
    static int logged;
    if (logged < 1) {
        logged++;
        ESP_LOGI(TAG, "udp tx len=%u -> %d", (unsigned)(14+ip_len), r);
        ESP_LOG_BUFFER_HEX_LEVEL("udpframe", frame, (14+ip_len) < 240 ? (14+ip_len) : 240, ESP_LOG_INFO);
    }
    return r;
}

static int udp_send(const uint8_t *payload, size_t len) {
    return udp_send_port(CIV7_PORT, payload, len);
}

int civ7_udp_send(uint16_t port, const uint8_t *data, size_t len) {
    return udp_send_port(port, data, len);
}

void civ7_get_local_ip(uint8_t out[4]) {
    memcpy(out, s_local_ip, 4);
}

/* ------------------------------------------------------------- transport */

static void store(uint64_t seq, const uint8_t *pkt, size_t len) {
    int i = (int)(seq % TX_RING);
    free(s_tx[i]);
    s_tx[i] = malloc(len);
    if (!s_tx[i]) return;
    memcpy(s_tx[i], pkt, len);
    s_tx_len[i] = len;
    s_tx_seq[i] = seq;
}

static void resend(uint64_t seq) {
    int i = (int)(seq % TX_RING);
    if (s_tx[i] && s_tx_seq[i] == seq) udp_send(s_tx[i], s_tx_len[i]);
}

/* packet = reversed(dst ip) + kind + body */
static uint64_t transport_send(const uint8_t *body, size_t len, bool initial) {
    uint8_t *pkt = malloc(5 + len);
    if (!pkt) return 0;
    ip_reversed(s_host_ip, pkt);
    pkt[4] = 'F';
    memcpy(pkt + 5, body, len);
    uint64_t seq = initial ? 0 : s_next_seq;
    if (!initial) s_next_seq++;
    udp_send(pkt, 5 + len);
    store(seq, pkt, 5 + len);
    free(pkt);
    return seq;
}

static void send_control(char kind, uint64_t seq) {
    uint8_t pkt[13];
    ip_reversed(s_host_ip, pkt);
    pkt[4] = (uint8_t)kind;
    for (int i = 0; i < 8; i++) pkt[5 + i] = (seq >> (8 * i)) & 0xff;
    udp_send(pkt, sizeof(pkt));
}

static void queue_msg(const uint8_t *payload, size_t len) {
    uint8_t *body = malloc(8 + len);
    if (!body) return;
    uint64_t seq = s_next_seq;
    for (int i = 0; i < 8; i++) body[i] = (seq >> (8 * i)) & 0xff;
    memcpy(body + 8, payload, len);
    transport_send(body, 8 + len, false);
    free(body);
}

/* ------------------------------------------------------------ rx (wifi cb) */

void civ7_rx_udp(const uint8_t *data, uint16_t len) {
    if (len < 5) return;
    uint8_t want[4];
    ip_reversed(s_local_ip, want);
    if (memcmp(data, want, 4) != 0) return;      /* destination prefix */
    char kind = (char)data[4];
    const uint8_t *body = data + 5;
    size_t blen = len - 5;

    if (kind == 'S') {
        if (blen == 1 && body[0] == 'P') send_control('S', 0); /* pong: 'S','p' */
        return;
    }
    if (kind == 'R' && blen >= 8) {
        uint64_t seq = 0;
        for (int i = 0; i < 8; i++) seq |= (uint64_t)body[i] << (8 * i);
        resend(seq);
        return;
    }
    if (kind == 'V' && blen >= 8) {
        if (s_next_seq > 0) resend(s_next_seq - 1);
        return;
    }
    if (kind != 'F' || blen < 8) return;

    uint64_t seq = 0;
    for (int i = 0; i < 8; i++) seq |= (uint64_t)body[i] << (8 * i);
    const uint8_t *msg = body + 8;
    size_t mlen = blen - 8;
    (void)seq;

    static const uint8_t REPLY[8] = {0x59,0x4c,0x50,0x52,0x65,0x72,0x69,0x46};
    if (mlen >= 8 && memcmp(msg, REPLY, 8) == 0) {
        s_welcome = true;
        if (mlen >= 44) {
            s_slot = msg[40] | (msg[41] << 8) | (msg[42] << 16) | ((uint32_t)msg[43] << 24);
        }
        ESP_LOGI(TAG, "FireRPLY slot=%u", s_slot);
        return;
    }
    if (mlen >= 28) {
        uint32_t type = msg[0] | (msg[1] << 8) | (msg[2] << 16) | ((uint32_t)msg[3] << 24);
        if (type == 0x4B0) {
            s_welcome = true;
            if (mlen >= 44) {
                s_slot = msg[40] | (msg[41] << 8) | (msg[42] << 16) | ((uint32_t)msg[43] << 24);
            }
            ESP_LOGI(TAG, "NetWelcome slot=%u", s_slot);
        }
    }
}

/* --------------------------------------------------------------- messages */

static void put_u32le(uint8_t *p, uint32_t v) { p[0]=v; p[1]=v>>8; p[2]=v>>16; p[3]=v>>24; }

static size_t message_header(uint8_t *out, uint32_t kind, uint32_t counter) {
    uint32_t h[7] = { kind, 1, 1, 0xFFFFFFFF, 0xFFFFFFFF, 1, counter };
    for (int i = 0; i < 7; i++) put_u32le(out + 4 * i, h[i]);
    return 28;
}

static void send_net_register(void) {
    uint8_t b[28 + 4 + 5];
    size_t n = message_header(b, 0x4B8, s_counter++);
    put_u32le(b + n, 5); n += 4;
    memcpy(b + n, "1.0.0", 5); n += 5;
    queue_msg(b, n);
}

static void send_net_app_register(void) {
    uint8_t b[28 + 4 + 1];
    size_t n = message_header(b, 0x4B9, s_counter++);
    put_u32le(b + n, s_slot); n += 4;
    b[n++] = 0;
    queue_msg(b, n);
}

static void send_net_snapshot(void) {
    uint8_t b[28 + 4 + 1];
    size_t n = message_header(b, 0x4E9, s_counter++);
    put_u32le(b + n, 0); n += 4;
    b[n++] = 1;
    queue_msg(b, n);
}

static void send_net_chat(const char *text) {
    size_t tlen = strlen(text);
    uint8_t *b = malloc(28 + 4 + 4 + tlen + 4 + 4);
    if (!b) return;
    size_t n = message_header(b, 0x3F3, s_counter++);
    put_u32le(b + n, s_slot); n += 4;          /* recipient = slot (CivSwitchLDN net_chat_public) */
    put_u32le(b + n, (uint32_t)tlen); n += 4;
    memcpy(b + n, text, tlen); n += tlen;
    put_u32le(b + n, 0xFFFFFFFE); n += 4;      /* chat_type -2 */
    put_u32le(b + n, 0xFFFFFFFF); n += 4;      /* target -1 */
    queue_msg(b, n);
    free(b);
}

/* ------------------------------------------------------------ handshake */

static uint64_t now_ms(void) { return (uint64_t)(esp_timer_get_time() / 1000); }

/* CivSwitchLDN's lobby.py warmup_arp: let the host's LDN IP stack learn our MAC. */
static void send_arp(void) {
    uint8_t f[42];
    memcpy(f, s_host_mac, 6);
    memcpy(f + 6, s_sta_mac, 6);
    f[12] = 0x08; f[13] = 0x06;
    f[14] = 0x00; f[15] = 0x01;              /* htype */
    f[16] = 0x08; f[17] = 0x00;              /* ptype */
    f[18] = 6; f[19] = 4;                    /* hlen/plen */
    f[20] = 0x00; f[21] = 0x01;              /* request */
    memcpy(f + 22, s_sta_mac, 6);            /* sha */
    memcpy(f + 28, s_local_ip, 4);           /* spa */
    memset(f + 32, 0, 6);                    /* tha */
    memcpy(f + 38, s_host_ip, 4);            /* tpa */
    for (int i = 0; i < 2; i++) {
        esp_wifi_internal_tx(WIFI_IF_STA, f, sizeof(f));
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    ESP_LOGI(TAG, "ARP warmup sent");
}

void civ7_run(void) {
    ESP_LOGI(TAG, "Civ7 handshake: host %u.%u.%u.%u -> %u.%u.%u.%u",
             s_host_ip[0],s_host_ip[1],s_host_ip[2],s_host_ip[3],
             s_local_ip[0],s_local_ip[1],s_local_ip[2],s_local_ip[3]);

    send_arp();

    /* FireCONN, retried until FireRPLY/NetWelcome */
    for (int attempt = 0; attempt < 6 && !s_welcome; attempt++) {
        static const uint8_t CONN[8] = {0x4E,0x4E,0x4F,0x43,0x65,0x72,0x69,0x46};
        uint8_t body[8 + 8 + 8 + 4 + 141];
        memset(body, 0, sizeof(body));
        /* u64 sequence 0 */
        memcpy(body + 8, CONN, 8);
        ip_reversed(s_host_ip, body + 16);          /* prefix: reversed host ip + 4 zeros */
        /* 4 zero bytes at 24 */
        uint8_t *intro = body + 28;
        put_u32le(intro, 0);                        /* intro id */
        memcpy(intro + 4, "Civ7-PC", 7);            /* identity (64) */
        memcpy(intro + 4 + 64, "Civ7-PC", 7);       /* name (64) */
        put_u32le(intro + 4 + 128, 6);              /* field84 */
        intro[4 + 132] = 0;                         /* flag */
        put_u32le(intro + 4 + 133, 0);              /* extra len */
        transport_send(body, sizeof(body), true);
        ESP_LOGI(TAG, "FireCONN sent (attempt %d)", attempt + 1);
        for (int i = 0; i < 30 && !s_welcome; i++) vTaskDelay(pdMS_TO_TICKS(100));
    }
    if (!s_welcome) {
        ESP_LOGE(TAG, "no FireRPLY/NetWelcome");
        return;
    }
    ESP_LOGI(TAG, "lobby joined, slot %u; sending 19 chat messages", s_slot);

    vTaskDelay(pdMS_TO_TICKS(300));
    send_net_register();
    vTaskDelay(pdMS_TO_TICKS(300));
    send_net_app_register();
    vTaskDelay(pdMS_TO_TICKS(400));
    send_net_snapshot();

    uint64_t t0 = now_ms();
    for (int i = 0; i < CHAT_LINE_COUNT; i++) {
        uint64_t due = t0 + 1000 + (uint64_t)(200 * i);
        while (now_ms() < due) vTaskDelay(pdMS_TO_TICKS(20));
        send_net_chat(CHAT_LINES[i]);
        ESP_LOGI(TAG, "NetChat %d/%d (%u bytes)", i + 1, CHAT_LINE_COUNT,
                 (unsigned)strlen(CHAT_LINES[i]));
    }
    s_done = true;
    ESP_LOGI(TAG, ">>> ALL %d CHAT MESSAGES SENT", CHAT_LINE_COUNT);
    vTaskDelay(pdMS_TO_TICKS(300));
    ESP_LOGI(TAG, "LDN stays open for the UDP installer channel");
}

bool civ7_done(void) { return s_done; }

void civ7_set_net(const uint8_t local_ip[4], const uint8_t host_ip[4],
                  const uint8_t host_mac[6], const uint8_t sta_mac[6]) {
    memcpy(s_local_ip, local_ip, 4);
    memcpy(s_host_ip, host_ip, 4);
    memcpy(s_host_mac, host_mac, 6);
    memcpy(s_sta_mac, sta_mac, 6);
    s_next_seq = 1;
    s_counter = 0;
    s_welcome = false;
    s_done = false;
}

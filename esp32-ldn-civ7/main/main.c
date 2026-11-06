/*
 * ldn-poc — auto mode: at boot, scan for the Civ7 LDN lobby, derive the
 * network keys (AES-ECB + SHA-256, Kinnay's algorithm), associate to the
 * console's hidden SSID with the derived CCMP key (no 4-way handshake), then
 * run the LDN authentication. Repeats until authenticated.
 *
 * Logs go to the USB-Serial/JTAG console (native USB on the Mac).
 */
#include <stdio.h>
#include <string.h>
#include <stdbool.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "nvs_flash.h"
#include "driver/gpio.h"
#include "esp_random.h"

#include "psa/crypto.h"

#include "esp_private/wifi.h"
#include "private_wifi.h"
#include "civ7.h"
#include "installer.h"

static const char *TAG = "ldnpoc";

#define BOOT_GPIO 0
#define BOOT_HOLD_MS 2000
#define SCAN_CHANNELS {1, 6, 11}
#define DWELL_MS 600
#define FRAME_MAX 1392

/* ---- prod.keys entries: generated from the user's prod.keys (see tools/build_and_flash.py) ---- */
#include "keys.h"

static const uint8_t SRC_ADV[16]   = {0x19,0x18,0x84,0x74,0x3e,0x24,0xc7,0x7d,0x87,0xc6,0x9e,0x42,0x07,0xd0,0xc4,0x38};
static const uint8_t SRC_DATA[16]  = {0xf1,0xe7,0x01,0x84,0x19,0xa8,0x4f,0x71,0x1d,0xa7,0x14,0xc2,0xcf,0x91,0x9c,0x9c};
static const uint8_t CHALLENGE_KEY[32] = {
    0xf8,0x4b,0x48,0x7f,0xb3,0x72,0x51,0xc2,0x63,0xbf,0x11,0x60,0x90,0x36,0x58,0x92,
    0x66,0xaf,0x70,0xca,0x79,0xb4,0x4c,0x93,0xc7,0x37,0x0c,0x57,0x69,0xc0,0xf6,0x02};
/* CivSwitchLDN: bytes.fromhex("cb964487b449f4208b1eabee6e927f8947fad3d88c37") = 22 bytes */
static const uint8_t CIV7_PASS[22] = {
    0xcb,0x96,0x44,0x87,0xb4,0x49,0xf4,0x20,0x8b,0x1e,0xab,
    0xee,0x6e,0x92,0x7f,0x89,0x47,0xfa,0xd3,0xd8,0x8c,0x37};

/* ---- crypto (Kinnay KeyDerivation._derive_key) ---- */
static void sha256(const uint8_t *data, size_t len, uint8_t out[32]) {
    size_t olen = 0;
    psa_hash_compute(PSA_ALG_SHA_256, data, len, out, 32, &olen);
}

static psa_key_id_t import_aes(const uint8_t key[16], psa_algorithm_t alg, psa_key_usage_t usage) {
    psa_key_attributes_t a = PSA_KEY_ATTRIBUTES_INIT;
    psa_set_key_type(&a, PSA_KEY_TYPE_AES);
    psa_set_key_bits(&a, 128);
    psa_set_key_usage_flags(&a, usage);
    psa_set_key_algorithm(&a, alg);
    psa_key_id_t id = 0;
    return psa_import_key(&a, key, 16, &id) == PSA_SUCCESS ? id : 0;
}

static void aes_ecb_dec(const uint8_t key[16], const uint8_t in[16], uint8_t out[16]) {
    psa_key_id_t id = import_aes(key, PSA_ALG_ECB_NO_PADDING, PSA_KEY_USAGE_DECRYPT);
    if (!id) return;
    size_t olen = 0;
    psa_cipher_decrypt(id, PSA_ALG_ECB_NO_PADDING, in, 16, out, 16, &olen);
    psa_destroy_key(id);
}

static void derive_key(const uint8_t *data, size_t data_len, const uint8_t source[16], uint8_t out[16]) {
    uint8_t kek[16], digest[32];
    aes_ecb_dec(MASTER_00, KEK_GEN, kek);      /* AES-ECB-dec(kek_source, master) */
    aes_ecb_dec(kek, source, kek);             /* AES-ECB-dec(source, kek)       */
    aes_ecb_dec(kek, KEY_GEN, kek);            /* AES-ECB-dec(key_source, kek)   */
    sha256(data, data_len, digest);
    aes_ecb_dec(kek, digest, out);             /* AES-ECB-dec(SHA256(data)[:16]) */
}

static void aes_ctr_crypt(const uint8_t key[16], const uint8_t nonce4[4],
                          uint8_t *data, size_t len) {
    psa_key_id_t id = import_aes(key, PSA_ALG_CTR, PSA_KEY_USAGE_ENCRYPT);
    if (!id) return;
    psa_cipher_operation_t op = PSA_CIPHER_OPERATION_INIT;
    uint8_t iv[16];
    memset(iv, 0, sizeof(iv));
    memcpy(iv, nonce4, 4);                     /* nonce(4) + counter(12, from 0) */
    psa_cipher_encrypt_setup(&op, id, PSA_ALG_CTR);
    psa_cipher_set_iv(&op, iv, 16);
    uint8_t *scratch = malloc(len);
    size_t done = 0, tail = 0;
    if (scratch) {
        psa_cipher_update(&op, data, len, scratch, len, &done);
        psa_cipher_finish(&op, scratch + done, len - done, &tail);
        memcpy(data, scratch, done + tail);
        free(scratch);
    }
    psa_cipher_abort(&op);
    psa_destroy_key(id);
}

static int hmac_sha256(const uint8_t key[32], const uint8_t *data, size_t len, uint8_t out[32]) {
    psa_key_attributes_t a = PSA_KEY_ATTRIBUTES_INIT;
    psa_set_key_type(&a, PSA_KEY_TYPE_HMAC);
    psa_set_key_bits(&a, 256);
    psa_set_key_usage_flags(&a, PSA_KEY_USAGE_SIGN_MESSAGE);
    psa_set_key_algorithm(&a, PSA_ALG_HMAC(PSA_ALG_SHA_256));
    psa_key_id_t id = 0;
    psa_status_t st = psa_import_key(&a, key, 32, &id);
    if (st != PSA_SUCCESS) return -1;
    psa_mac_operation_t op = PSA_MAC_OPERATION_INIT;
    size_t olen = 0;
    st = psa_mac_sign_setup(&op, id, PSA_ALG_HMAC(PSA_ALG_SHA_256));
    if (st == PSA_SUCCESS) st = psa_mac_update(&op, data, len);
    if (st == PSA_SUCCESS) st = psa_mac_sign_finish(&op, out, 32, &olen);
    psa_destroy_key(id);
    return (int)st;
}

/* ---- state ---- */
typedef enum { ST_IDLE, ST_SCANNING, ST_ASSOCIATING, ST_ASSOCIATED } state_t;
static volatile state_t s_state = ST_IDLE;
static volatile bool s_got_frame;
static volatile bool s_assoc_seen;
static uint8_t s_frame[FRAME_MAX];
static volatile int s_frame_len;

static uint8_t s_peer[6], s_sta_mac[6], s_ccmp_key[16];
static char s_ssid_hex[33];
static uint8_t s_channel;

/* ---- LDN network state (M3) ---- */
#define ETH_TYPE_LDN 0x88B7
static uint64_t s_lcid;
static uint16_t s_scene;
static uint8_t s_ssid16[16];
static uint8_t s_server_random[16];
static uint64_t s_challenge;
static uint16_t s_app_version;
static uint8_t s_client_random[16];
static volatile bool s_auth_ok;
static volatile uint8_t s_auth_status;
static volatile bool s_have_ip;
static uint8_t s_local_ip[4], s_host_ip[4];

/* ---- wpa patch (station side only) ---- */
static uint8_t s_rsn_ie[] = {
    0x30,0x14,0x01,0x00,0x00,0x0f,0xac,0x04,0x01,0x00,0x00,0x0f,0xac,0x04,
    0x01,0x00,0x00,0x0f,0xac,0x02,0x0c,0x00,
};
static int (*s_stock_sta_connect)(uint8_t *bssid);

static int ldn_sta_connect(uint8_t *bssid) {
    int r = esp_wifi_set_appie_internal(APPIE_RSN, s_rsn_ie, sizeof(s_rsn_ie), 0);
    if (r == 0 && s_stock_sta_connect) r = s_stock_sta_connect(bssid);
    if (r == 0) r = esp_wifi_set_appie_internal(APPIE_RSN, s_rsn_ie, sizeof(s_rsn_ie), 0);
    ESP_LOGI(TAG, "ldn_sta_connect hook fired (appie r=%d)", r);
    return r;
}
static int ldn_sta_rx_eapol(uint8_t *src, uint8_t *buf, uint32_t len) { (void)src;(void)buf;(void)len; return 0; }
static bool ldn_sta_in_handshake(void) { return false; }
static void ldn_sta_connected(uint8_t *bssid) { (void)bssid; }

static void install_hooks(void) {
    struct wpa_funcs *t = malloc(sizeof(*t));
    assert(t);
    memcpy(t, wpa_cb, sizeof(*t));
    s_stock_sta_connect = t->wpa_sta_connect;
    t->wpa_sta_connect = ldn_sta_connect;
    t->wpa_sta_connected_cb = ldn_sta_connected;
    t->wpa_sta_rx_eapol = ldn_sta_rx_eapol;
    t->wpa_sta_in_4way_handshake = ldn_sta_in_handshake;
    ESP_ERROR_CHECK(esp_wifi_register_wpa_cb_internal(t));
    wpa_cb = t;
}

/* ---- association ---- */
static void rx_cb(void *buf, wifi_promiscuous_pkt_type_t type);
static void tx_done(uint8_t ifidx, uint8_t *data, uint16_t *length, bool acked);

/* ---- M3: LDN authentication + IP ---- */
static esp_err_t ethernet_rx(void *buffer, uint16_t length, void *eb);

static int eth_send(const uint8_t *payload, size_t len) {
    uint8_t frame[1600];
    if (len + 14 > sizeof(frame)) return -1;
    memcpy(frame, s_peer, 6);
    memcpy(frame + 6, s_sta_mac, 6);
    frame[12] = ETH_TYPE_LDN >> 8;
    frame[13] = ETH_TYPE_LDN & 0xff;
    memcpy(frame + 14, payload, len);
    return esp_wifi_internal_tx(WIFI_IF_STA, frame, 14 + len);
}

static void put_u64le(uint8_t *p, uint64_t v) { for (int i = 0; i < 8; i++) p[i] = (v >> (8*i)) & 0xff; }

static void ldn_authenticate(void) {
    int rr = esp_wifi_internal_reg_rxcb(WIFI_IF_STA, ethernet_rx);
    ESP_LOGI(TAG, "rxcb reg -> %d, sta_running=%d", rr, esp_wifi_sta_is_running_internal());

    /* ChallengeRequest body (0x2D0) */
    static uint8_t body[0x2D0];
    memset(body, 0, sizeof(body));
    put_u64le(body + 8, s_challenge);
    put_u64le(body + 16, ((uint64_t)esp_random() << 32) | esp_random());
    put_u64le(body + 24, ((uint64_t)esp_random() << 32) | esp_random());
    uint8_t mac[32] = {0};
    int hst = hmac_sha256(CHALLENGE_KEY, body, sizeof(body), mac);
    ESP_LOGI(TAG, "challenge hmac -> %d: %02x%02x%02x%02x", hst, mac[0],mac[1],mac[2],mac[3]);
    static uint8_t ch[0x300];
    memset(ch, 0, sizeof(ch));
    memcpy(ch + 4, mac, 32);
    memcpy(ch + 4 + 32 + 12, body, sizeof(body));

    /* AuthenticationRequest payload */
    static uint8_t req[100 + 0x300];
    memset(req, 0, sizeof(req));
    const char *name = "Civ7-ESP32";
    memcpy(req, name, strlen(name));
    req[32] = s_app_version >> 8; req[33] = s_app_version & 0xff;   /* u16 BE */
    req[34] = 0;                                                   /* platform NX */
    memcpy(req + 100, ch, sizeof(ch));

    /* AuthenticationFrame (protocol 1 -> plaintext) */
    uint16_t size = sizeof(req);
    static uint8_t frame[8 + 32 + 16 + 16 + 0x300 + 0x100];
    uint8_t *o = frame;
    *o++ = 0x00; *o++ = 0x22; *o++ = 0xaa;   /* OUI */
    *o++ = 0x01; *o++ = 0x02;                /* auth frame */
    *o++ = 0x00;                             /* pad */
    *o++ = 4;                                /* version */
    *o++ = size & 0xff;                      /* size lo */
    *o++ = 0;                                /* status */
    *o++ = 0;                                /* is_response = 0 */
    *o++ = size >> 8;                        /* size hi */
    *o++ = 0;                                /* AUTH_FORMAT_PLAIN = 0 */
    *o++ = 0; *o++ = 0;                      /* pad 2 */
    /* network_id, little-endian */
    put_u64le(o, s_lcid); o += 8;
    *o++ = 0; *o++ = 0;
    o[0] = s_scene & 0xff; o[1] = s_scene >> 8; o += 2;
    memset(o, 0, 4); o += 4;
    memcpy(o, s_ssid16, 16); o += 16;
    memcpy(o, s_server_random, 16); o += 16;
    memcpy(o, s_client_random, 16); o += 16;
    memcpy(o, req, sizeof(req)); o += sizeof(req);

    size_t flen = (size_t)(o - frame);
    ESP_LOGI(TAG, "auth request: %u bytes lcid=%016llx scene=%04x", (unsigned)flen,
             (unsigned long long)s_lcid, s_scene);
    ESP_LOGI(TAG, "srv=%02x%02x%02x%02x cli=%02x%02x%02x%02x",
             s_server_random[0],s_server_random[1],s_server_random[2],s_server_random[3],
             s_client_random[0],s_client_random[1],s_client_random[2],s_client_random[3]);
    ESP_LOG_BUFFER_HEX_LEVEL("authhdr", frame, 200, ESP_LOG_INFO);
    for (int attempt = 1; attempt <= 3 && !s_auth_ok; attempt++) {
        int tx = eth_send(frame, flen);
        ESP_LOGI(TAG, "auth tx -> %d", tx);
        for (int i = 0; i < 14 && !s_auth_ok; i++) vTaskDelay(pdMS_TO_TICKS(50));
        ESP_LOGI(TAG, "auth attempt %d -> %s status=%u", attempt,
                 s_auth_ok ? "OK" : "retry", s_auth_status);
    }
    if (!s_auth_ok) {
        ESP_LOGE(TAG, "LDN authentication failed");
        return;
    }
    ESP_LOGI(TAG, ">>> LDN AUTHENTICATED");
}

static void handle_ldn_frame(const uint8_t *data, size_t len) {
    if (len < 6) return;
    if (data[0] != 0x00 || data[1] != 0x22 || data[2] != 0xaa) return;
    uint16_t type = (data[3] << 8) | data[4];
    if (type != 0x0102) return;              /* authentication frame */
    if (len < 6 + 8 + 32 + 16 + 16) return;
    const uint8_t *h = data + 6;
    uint8_t version = h[0];
    uint8_t status = h[2];
    uint8_t is_response = h[3];
    uint8_t format = h[5];
    if (!is_response) return;
    (void)version; (void)format;
    s_auth_status = status;
    /* verify client_random echoes ours */
    if (memcmp(h + 8 + 32 + 16, s_client_random, 16) != 0) {
        ESP_LOGW(TAG, "auth resp client_random mismatch");
        return;
    }
    if (status != 0) {
        ESP_LOGE(TAG, "auth status=%u", status);
        return;
    }
    s_auth_ok = true;
}

static esp_err_t ethernet_rx(void *buffer, uint16_t length, void *eb) {
    const uint8_t *f = buffer;
    if (length >= 14) {
        uint16_t et = (f[12] << 8) | f[13];
        ESP_LOGI(TAG, "eth rx type=%04x len=%u src=%02x:%02x:%02x:%02x:%02x:%02x",
                 et, length, f[6],f[7],f[8],f[9],f[10],f[11]);
        if (et == 0x88B7) {
            handle_ldn_frame(f + 14, length - 14);
        } else if (et == 0x0800 && length >= 14 + 20 + 8) {
            const uint8_t *ip = f + 14;
            if (ip[9] == 17) {
                uint16_t ihl = (ip[0] & 0x0f) * 4;
                if (ihl >= 20 && length >= 14 + ihl + 8) {
                    const uint8_t *udp = ip + ihl;
                    uint16_t dport = (udp[2] << 8) | udp[3];
                    uint16_t sport = (udp[0] << 8) | udp[1];
                    ESP_LOGI(TAG, "udp rx %u->%u len=%u", sport, dport,
                             length - 14 - ihl - 8);
                    if (dport == 62056) {
                        civ7_rx_udp(udp + 8, length - 14 - ihl - 8);
                    }
                }
            }
        }
    }
    esp_wifi_internal_free_rx_buffer(eb);
    return ESP_OK;
}

static void sta_install_keys(void) {
    uint8_t seq[8] = {0};
    int pw = esp_wifi_set_sta_key_internal(WPA_ALG_CCMP, s_peer, 0, 1, seq, 8, s_ccmp_key, 16,
                                           KEY_FLAG_PAIRWISE | KEY_FLAG_RX | KEY_FLAG_TX);
    int grp = esp_wifi_set_sta_key_internal(WPA_ALG_CCMP, s_peer, 1, 0, seq, 8, s_ccmp_key, 16,
                                            KEY_FLAG_GROUP | KEY_FLAG_RX);
    if (pw || grp) {
        ESP_LOGE(TAG, "key install failed pw=%d grp=%d", pw, grp);
        s_state = ST_IDLE;
        return;
    }
    esp_wifi_auth_done_internal();
    s_state = ST_ASSOCIATED;
    ESP_LOGI(TAG, ">>> ASSOCIATED to %02x:%02x:%02x:%02x:%02x:%02x (ch %d)",
             s_peer[0],s_peer[1],s_peer[2],s_peer[3],s_peer[4],s_peer[5], s_channel);

    ldn_authenticate();
}

static volatile bool s_sent;

/* Parse the latest advertisement and find our participant entry (IP). */
static bool adv_find_ip(void) {
    const uint8_t *f = s_frame;
    int len = s_frame_len;
    if (len < 24 + 12 + 40) return false;
    const uint8_t *network_id = f + 36;
    uint8_t adv_key[16];
    derive_key(network_id, 32, SRC_ADV, adv_key);
    if (f[69] != 2) return false;
    uint16_t size = (f[70] << 8) | f[71];
    int ct_len = 32 + size;
    if (size == 0 || 76 + ct_len > len) return false;
    uint8_t *plain = malloc(ct_len);
    if (!plain) return false;
    memcpy(plain, f + 76, ct_len);
    aes_ctr_crypt(adv_key, f + 72, plain, ct_len);
    const uint8_t *p = plain + 32;
    memcpy(s_host_ip, p + 24, 4);           /* participants[0].ip */
    bool found = false;
    for (int i = 0; i < 8; i++) {
        const uint8_t *pt = p + 24 + i * 56;
        if (pt[10] && memcmp(pt + 4, s_sta_mac, 6) == 0) {
            memcpy(s_local_ip, pt, 4);
            found = true;
            ESP_LOGI(TAG, "our IP from advertisement slot %d: %u.%u.%u.%u", i,
                     pt[0], pt[1], pt[2], pt[3]);
            break;
        }
    }
    free(plain);
    if (!found) return false;
    ESP_LOGI(TAG, "host IP: %u.%u.%u.%u", s_host_ip[0],s_host_ip[1],s_host_ip[2],s_host_ip[3]);
    s_have_ip = true;
    return true;
}

static void start_associate(void) {
    for (int attempt = 1; attempt <= 5 && s_state != ST_ASSOCIATED; attempt++) {
        ESP_LOGI(TAG, "assoc attempt %d/5", attempt);
        esp_wifi_stop();
        esp_wifi_set_mac(WIFI_IF_STA, s_sta_mac);   /* fixed MAC (pokeldn uses a stable one) */
        esp_wifi_set_protocol(WIFI_IF_STA, WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G | WIFI_PROTOCOL_11N);
        esp_wifi_start();
        esp_wifi_set_tx_done_cb(tx_done);
        esp_wifi_set_ps(WIFI_PS_NONE);
        /* keep promiscuous on so rx_cb can catch the association response */
        wifi_promiscuous_filter_t flt = {.filter_mask = WIFI_PROMIS_FILTER_MASK_MGMT | WIFI_PROMIS_FILTER_MASK_DATA};
        esp_wifi_set_promiscuous_filter(&flt);
        esp_wifi_set_promiscuous_rx_cb(&rx_cb);
        esp_wifi_set_promiscuous(true);

        wifi_config_t cfg = {0};
        memcpy(cfg.sta.ssid, s_ssid_hex, 32);
        memcpy(cfg.sta.password, "00000000", 8);
        memcpy(cfg.sta.bssid, s_peer, 6);
        cfg.sta.bssid_set = true;
        cfg.sta.channel = s_channel;
        cfg.sta.scan_method = WIFI_FAST_SCAN;
        cfg.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
        esp_wifi_set_config(WIFI_IF_STA, &cfg);

        ESP_LOGI(TAG, "associating: ssid=%s ch=%d mac=%02x:%02x:%02x:%02x:%02x:%02x",
                 s_ssid_hex, s_channel, s_sta_mac[0],s_sta_mac[1],s_sta_mac[2],
                 s_sta_mac[3],s_sta_mac[4],s_sta_mac[5]);
        s_state = ST_ASSOCIATING;
        s_assoc_seen = false;
        esp_err_t r = esp_wifi_connect();
        ESP_LOGI(TAG, "esp_wifi_connect() -> %d", r);
        if (r != ESP_OK) { vTaskDelay(pdMS_TO_TICKS(500)); continue; }
        for (int i = 0; i < 60 && s_state == ST_ASSOCIATING; i++) {
            if (s_assoc_seen && esp_wifi_sta_is_running_internal()) {
                sta_install_keys();          /* normal task context */
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(50));   /* 3 s per attempt */
        }
        if (s_state == ST_ASSOCIATED) break;
        ESP_LOGW(TAG, "attempt %d did not associate", attempt);
        esp_wifi_disconnect();
        vTaskDelay(pdMS_TO_TICKS(300));
    }
    if (s_state != ST_ASSOCIATED) {
        ESP_LOGE(TAG, "all association attempts failed");
        s_state = ST_IDLE;
    }
}

/* ---- promiscuous rx: capture advertisement + detect assoc response ---- */
static void rx_cb(void *buf, wifi_promiscuous_pkt_type_t type) {
    wifi_promiscuous_pkt_t *p = (wifi_promiscuous_pkt_t *)buf;
    const uint8_t *f = (const uint8_t *)p->payload;
    int len = p->rx_ctrl.sig_len - 4;   /* strip FCS */
    if (len < 24) return;
    uint8_t fc = f[0];
    uint8_t ftype = (fc >> 2) & 0x3;

    if (ftype == 2) {  /* data: raw 802.11, before decryption */
        if (s_state == ST_ASSOCIATING || s_state == ST_ASSOCIATED) {
            ESP_LOGI(TAG, "DATA ch=%d len=%d to=%02x:%02x:%02x:%02x:%02x:%02x from=%02x:%02x:%02x:%02x:%02x:%02x",
                     p->rx_ctrl.channel, len,
                     f[4],f[5],f[6],f[7],f[8],f[9], f[10],f[11],f[12],f[13],f[14],f[15]);
        }
        return;
    }
    if (ftype == 0) {  /* mgmt */
        uint8_t subtype = fc & 0xfc;
        /* diagnostics: dump the probe response while joining (see the AP's RSN) */
        if (subtype == 0x50 && s_state == ST_ASSOCIATING && !memcmp(f + 10, s_peer, 6)) {
            ESP_LOGI(TAG, "probe resp len=%d:", len);
            ESP_LOG_BUFFER_HEX_LEVEL("probe", f, len < 220 ? len : 220, ESP_LOG_INFO);
        }
        /* log auth response fields: algo@24 seq@26 status@28 */
        if (subtype == 0xb0 && s_state == ST_ASSOCIATING && !memcmp(f + 10, s_peer, 6)) {
            ESP_LOGI(TAG, "auth resp algo=%02x%02x seq=%02x%02x status=%02x%02x",
                     f[24],f[25],f[26],f[27],f[28],f[29]);
        }
        /* assoc response while joining -> flag only; keys are installed from
         * a normal task context (calling auth_done from the wifi RX callback
         * crashes inside cnx_auth_done) */
        if (subtype == 0x10 && s_state == ST_ASSOCIATING && len >= 28 &&
            !memcmp(f + 4, s_sta_mac, 6) && !memcmp(f + 10, s_peer, 6) &&
            f[26] == 0 && f[27] == 0) {
            s_assoc_seen = true;
            return;
        }
        /* diagnostics: every mgmt frame touching us or the console while joining */
        if (s_state == ST_ASSOCIATING) {
            bool touch = !memcmp(f + 4, s_peer, 6) || !memcmp(f + 10, s_peer, 6) ||
                         !memcmp(f + 4, s_sta_mac, 6) || !memcmp(f + 10, s_sta_mac, 6);
            if (touch) {
                ESP_LOGI(TAG, "mgmt st=%02x from=%02x:%02x:%02x:%02x:%02x:%02x to=%02x:%02x:%02x:%02x:%02x:%02x b26=%02x%02x",
                         subtype, f[10],f[11],f[12],f[13],f[14],f[15],
                         f[4],f[5],f[6],f[7],f[8],f[9], f[26], f[27]);
            }
        }
        /* LDN action frame -> capture once during scan */
        if (subtype == 0xd0 && len >= 28 && !memcmp(f + 24, "\x7f\x00\x22\xaa", 4)) {
            if (s_auth_ok && !s_have_ip && len > 0 && len <= (int)sizeof(s_frame)) {
                memcpy(s_frame, f, len);
                s_frame_len = len;
                s_channel = p->rx_ctrl.channel;
            }
            if (s_state == ST_SCANNING && !s_got_frame) {
                int n = len < (int)sizeof(s_frame) ? len : (int)sizeof(s_frame);
                memcpy(s_frame, f, n);
                s_frame_len = n;
                s_channel = p->rx_ctrl.channel;
                s_got_frame = true;
                ESP_LOGI(TAG, "captured LDN frame ch=%d rssi=%d len=%d",
                         p->rx_ctrl.channel, (int8_t)p->rx_ctrl.rssi, n);
            }
        }
    }
}

static void wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data) {
    (void)arg; (void)base;
    if (base == WIFI_EVENT) {
        switch (id) {
            case WIFI_EVENT_STA_START:      ESP_LOGI(TAG, "EV STA_START"); break;
            case WIFI_EVENT_STA_STOP:       ESP_LOGI(TAG, "EV STA_STOP"); break;
            case WIFI_EVENT_STA_CONNECTED:  ESP_LOGI(TAG, "EV STA_CONNECTED"); break;
            case WIFI_EVENT_STA_DISCONNECTED: {
                wifi_event_sta_disconnected_t *d = (wifi_event_sta_disconnected_t *)data;
                ESP_LOGI(TAG, "EV STA_DISCONNECTED reason=%d", d->reason);
                break;
            }
            case WIFI_EVENT_STA_AUTHMODE_CHANGE:
                ESP_LOGI(TAG, "EV STA_AUTHMODE_CHANGE old=%d new=%d",
                         ((wifi_event_sta_authmode_change_t *)data)->old_mode,
                         ((wifi_event_sta_authmode_change_t *)data)->new_mode);
                break;
            default: break;
        }
    }
}

static void tx_done(uint8_t ifidx, uint8_t *data, uint16_t *length, bool acked) {
    (void)ifidx;
    if (!data || !length || *length < 24) return;
    uint8_t fc = data[0];
    uint8_t ft = (fc >> 2) & 0x3;
    if (ft == 0) ESP_LOGI(TAG, "TX mgmt st=%02x acked=%d", fc & 0xfc, acked);
    else if (ft == 2) ESP_LOGI(TAG, "TX data len=%u acked=%d", *length, acked);
}

static void radio_init(void) {
    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event, NULL));
    ESP_ERROR_CHECK(psa_crypto_init());
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    esp_fill_random(s_sta_mac, 6);
    s_sta_mac[0] = (s_sta_mac[0] & 0xfc) | 2;   /* fixed, locally administered */
    ESP_ERROR_CHECK(esp_wifi_set_mac(WIFI_IF_STA, s_sta_mac));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_ERROR_CHECK(esp_wifi_set_promiscuous_rx_cb(&rx_cb));
    ESP_ERROR_CHECK(esp_wifi_set_promiscuous(true));
    install_hooks();
    ESP_LOGI(TAG, "radio ready");
}

/* ---- scan + derive + associate ---- */
static void hex32(const uint8_t b[16], char out[33]) {
    static const char h[] = "0123456789abcdef";
    for (int i = 0; i < 16; i++) { out[2*i] = h[b[i]>>4]; out[2*i+1] = h[b[i]&15]; }
    out[32] = 0;
}

static void derive_and_associate(void) {
    const uint8_t *f = s_frame;
    int len = s_frame_len;
    if (len < 24 + 12 + 40) { ESP_LOGE(TAG, "frame too short"); s_state = ST_IDLE; return; }

    memcpy(s_peer, f + 10, 6);                 /* addr2 = console MAC */
    /* s_channel was set at capture time */

    const uint8_t *network_id = f + 36;        /* 32 bytes */
    uint8_t advertise_key[16];
    derive_key(network_id, 32, SRC_ADV, advertise_key);
    ESP_LOGI(TAG, "advertise_key=%02x%02x%02x%02x...",
             advertise_key[0],advertise_key[1],advertise_key[2],advertise_key[3]);

    uint8_t format = f[69];                    /* header: version@68, format@69 */
    uint16_t size = (f[70] << 8) | f[71];
    const uint8_t *nonce = f + 72;             /* 4 bytes */
    ESP_LOGI(TAG, "format=%d size=0x%x nonce=%02x%02x%02x%02x",
             format, size, nonce[0],nonce[1],nonce[2],nonce[3]);

    uint8_t ssid16[16];
    memcpy(ssid16, network_id + 16, 16);       /* ssid at network_id[16..31] */
    memcpy(s_ssid16, ssid16, 16);
    hex32(ssid16, s_ssid_hex);
    ESP_LOGI(TAG, "ssid=%s", s_ssid_hex);

    /* save the fields the authentication frame needs */
    s_lcid = 0;
    for (int i = 0; i < 8; i++) s_lcid = (s_lcid << 8) | network_id[i];   /* BE u64 */
    s_scene = (network_id[10] << 8) | network_id[11];                      /* BE u16 */
    for (int i = 0; i < 16; i++) s_client_random[i] = esp_random() & 0xff;

    /* body ciphertext: 32-byte sha + size bytes, encrypted AES-CTR */
    int ct_len = 32 + size;
    uint8_t *plain = malloc(ct_len);
    if (!plain) { s_state = ST_IDLE; return; }
    memcpy(plain, f + 76, ct_len);
    aes_ctr_crypt(advertise_key, nonce, plain, ct_len);

    /* verify the AES-CTR plaintext: sha256(header + 32 zero bytes + payload) == plain[0..31] */
    {
        static uint8_t tmp[40 + 32 + 0x500];
        memcpy(tmp, f + 36, 40);
        memset(tmp + 40, 0, 32);
        memcpy(tmp + 72, plain + 32, size);
        uint8_t dig[32];
        sha256(tmp, 40 + 32 + size, dig);
        ESP_LOGI(TAG, "adv sha %s", memcmp(dig, plain, 32) == 0 ? "OK" : "MISMATCH");
    }

    uint8_t server_random[16];
    memcpy(server_random, plain + 32, 16);     /* sha(32) then payload; server_random first 16 */
    memcpy(s_server_random, server_random, 16);
    /* challenge u64 BE lives at payload offset 1272 (0x500 payload) */
    s_challenge = 0;
    const uint8_t *chp = plain + 32 + 1272;
    for (int i = 0; i < 8; i++) s_challenge = (s_challenge << 8) | chp[i];
    s_app_version = (plain[32 + 68] << 8) | plain[32 + 69];
    ESP_LOGI(TAG, "app_version=%u", s_app_version);
    ESP_LOGI(TAG, "server_random=%02x%02x%02x%02x... challenge=%08x",
             server_random[0],server_random[1],server_random[2],server_random[3],
             (unsigned)(s_challenge >> 32));
    free(plain);

    uint8_t data[16 + sizeof(CIV7_PASS)];
    memcpy(data, server_random, 16);
    memcpy(data + 16, CIV7_PASS, sizeof(CIV7_PASS));
    derive_key(data, sizeof(data), SRC_DATA, s_ccmp_key);
    ESP_LOGI(TAG, "server_random=%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x",
             server_random[0],server_random[1],server_random[2],server_random[3],
             server_random[4],server_random[5],server_random[6],server_random[7],
             server_random[8],server_random[9],server_random[10],server_random[11],
             server_random[12],server_random[13],server_random[14],server_random[15]);
    ESP_LOGI(TAG, "ccmp_key=%02x%02x%02x%02x...%02x%02x",
             s_ccmp_key[0],s_ccmp_key[1],s_ccmp_key[2],s_ccmp_key[3],
             s_ccmp_key[14],s_ccmp_key[15]);

    start_associate();
}

/* boot task: 2s hold -> start a scan round */
static void do_scan(void) {
    static const uint8_t chans[] = SCAN_CHANNELS;
    ESP_LOGI(TAG, "=== scan start ===");
    esp_wifi_disconnect();
    vTaskDelay(pdMS_TO_TICKS(200));
    s_got_frame = false;
    s_state = ST_SCANNING;
    for (int r = 0; r < 6 && !s_got_frame; r++) {
        for (size_t i = 0; i < sizeof(chans) && !s_got_frame; i++) {
            esp_wifi_set_channel(chans[i], WIFI_SECOND_CHAN_NONE);
            vTaskDelay(pdMS_TO_TICKS(DWELL_MS));
        }
    }
    if (s_got_frame) {
        ESP_LOGI(TAG, "frame captured; deriving + associating...");
        derive_and_associate();
    } else {
        ESP_LOGI(TAG, "no LDN frame found; idle");
        s_state = ST_IDLE;
    }
}

void app_main(void) {
    radio_init();
    ESP_LOGI(TAG, "=== ldn-poc auto mode (no BOOT trigger) ===");
    ESP_LOGI(TAG, "scanning every ~8s; keep the Civ7 wireless lobby open");
    for (;;) {
        if (!s_auth_ok) {
            do_scan();
        } else if (!s_have_ip) {
            adv_find_ip();
        } else if (!s_sent) {
            s_sent = true;
            civ7_set_net(s_local_ip, s_host_ip, s_peer, s_sta_mac);
            civ7_run();
            installer_udp_run();
            ESP_LOGI(TAG, "installer flow done; idle (reset the board for another run)");
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

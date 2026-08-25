#include "espnow_sniff.h"
#include "radar_wire.h"
#include "radar_key.h"          // SIMULACRA_ESPNOW_KEY: v4 needs the key to identify a frame
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "esniff";
#define ESPNOW_SNIFF_CH 1

// Byte offsets inside a captured 802.11 vendor-specific action frame (ESP-NOW):
//   [0..23]  802.11 MAC header (fctl,dur,DA,SA@10,BSSID,seq)
//   [24]     category = 0x7f (vendor specific)   [25..27] OUI 18:fe:34   [28..31] random
//   [32]     elem-id 0xdd  [33] len  [34..36] OUI 18:fe:34  [37] type 0x04  [38] version
//   [39..]   ESP-NOW body == our radar_wire frame ([nonce(12)][ct][tag(16)] since wire v4)
#define ENOW_HDR 39
#define SRC_OFF  10

static volatile uint32_t s_req, s_status, s_status_laa, s_status_factory;
static volatile uint32_t s_last_status_ms;

static void rx_cb(void *buf, wifi_promiscuous_pkt_type_t type)
{
    if (type != WIFI_PKT_MGMT) return;
    const wifi_promiscuous_pkt_t *p = (const wifi_promiscuous_pkt_t *)buf;
    int len = p->rx_ctrl.sig_len;
    if (len < ENOW_HDR + RADAR_NONCE_LEN + RADAR_TAG_LEN) return;
    const uint8_t *f = p->payload;
    if (f[0] != 0xD0) return;                                              // action frame
    if (f[24] != 0x7F || f[25] != 0x18 || f[26] != 0xFE || f[27] != 0x34)  // Espressif vendor
        return;
    const uint8_t *ef = f + ENOW_HDR;                                      // our radar_wire frame
    size_t eflen = (size_t)(len - ENOW_HDR);

    // Wire v4 removed the plaintext magic and type byte, so there is nothing to match on and the
    // only way to identify a frame as ours is to AUTHENTICATE it. That is precisely the property
    // this tool exists to verify: an adversary CANNOT do the next line, because it needs the key.
    // `wtype` not `type`: the promiscuous callback's own parameter already owns that name.
    uint8_t wtype, pl[RADAR_FRAME_MAX], salt[RADAR_SALT_LEN]; size_t plen; uint64_t ctr;
    if (radar_wire_open(ef, eflen, SIMULACRA_ESPNOW_KEY,
                        &wtype, pl, sizeof pl, &plen, salt, &ctr) != 0)
        return;                                                            // not our link

    const uint8_t *sa = f + SRC_OFF;
    bool laa = (sa[0] & 0x02) != 0;                                        // locally-administered bit

    if (wtype == RADAR_TYPE_REQUEST) {
        s_req++;
        ESP_LOGW(TAG, "REQ  src=%02x:%02x:%02x:%02x:%02x:%02x %s len=%u",
                 sa[0], sa[1], sa[2], sa[3], sa[4], sa[5], laa ? "[LAA]" : "[FACTORY!]",
                 (unsigned)eflen);
    } else if (wtype == RADAR_TYPE_STATUS) {
        s_status++;
        if (laa) s_status_laa++; else s_status_factory++;
        s_last_status_ms = (uint32_t)(esp_timer_get_time() / 1000);
        // Ciphertext sample straight after the 12-byte nonce (no header in v4), proving the status
        // really is sealed on air. `len` is logged so bucketing is observable: every frame should
        // land on 64/128/250 rather than a payload-shaped size.
        const uint8_t *ct = ef + RADAR_NONCE_LEN;
        ESP_LOGW(TAG, "STAT src=%02x:%02x:%02x:%02x:%02x:%02x %s len=%u ct=%02x%02x%02x%02x%02x%02x%02x%02x",
                 sa[0], sa[1], sa[2], sa[3], sa[4], sa[5], laa ? "[LAA]" : "[FACTORY!]",
                 (unsigned)eflen,
                 ct[0], ct[1], ct[2], ct[3], ct[4], ct[5], ct[6], ct[7]);
    }
}

static void sniff_task(void *arg)
{
    (void)arg;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(5000));
        uint32_t now  = (uint32_t)(esp_timer_get_time() / 1000);
        uint32_t idle = s_status ? (now - s_last_status_ms) : 0;
        ESP_LOGW(TAG, "counts: REQ=%u STATUS=%u (src-MAC LAA=%u factory=%u) last-STATUS=%ums-ago",
                 (unsigned)s_req, (unsigned)s_status,
                 (unsigned)s_status_laa, (unsigned)s_status_factory, (unsigned)idle);
    }
}

void espnow_sniff_start(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_NULL));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_ERROR_CHECK(esp_wifi_set_channel(ESPNOW_SNIFF_CH, WIFI_SECOND_CHAN_NONE));
    wifi_promiscuous_filter_t filt = { .filter_mask = WIFI_PROMIS_FILTER_MASK_MGMT };
    esp_wifi_set_promiscuous_filter(&filt);
    esp_wifi_set_promiscuous_rx_cb(rx_cb);
    ESP_ERROR_CHECK(esp_wifi_set_promiscuous(true));
    xTaskCreate(sniff_task, "esniff", 4096, NULL, 5, NULL);
    ESP_LOGW(TAG, "ESP-NOW opsec sniffer up (parked ch%d): expect NO STATUS until the CYD requests",
             ESPNOW_SNIFF_CH);
}

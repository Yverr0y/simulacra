#include <string.h>
#include "espnow_sniff.h"
#include "radar_wire.h"
#include "radar_key.h"          // SIMULACRA_ESPNOW_KEY: v4 needs the key to identify a frame
#include "fleet.h"              // RADAR_TYPE_FLEET_MACS: the 88% mass being measured
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
#define WIFI_FCS_LEN 4          // rx_ctrl.sig_len includes the hardware FCS; strip it

static volatile uint32_t s_req, s_status, s_status_laa, s_status_factory;
static volatile uint32_t s_last_status_ms;

// RAW frame accounting -- the adversary's view, and the metric that actually matters.
//
// The 2026-08-25 Kismet capture found each board emitting ~25 vendor action frames/min while the
// loudest real device in a 206-device environment managed 0.07/min and the median managed zero.
// Counting them needs no key: the OUI check above is the whole attack. So s_raw is incremented
// BEFORE any decrypt, which is exactly what a passive listener can do, and is the number the
// 2026-08-26 delta broadcast has to move. The decoded counters below cannot serve this purpose --
// they only see REQ and STATUS, and FLEET_MACS was 88% of the traffic.
#define SNIFF_SRC_MAX 12
static volatile uint32_t s_raw, s_fleet, s_start_ms;
// Per-TYPE tally. Added 2026-08-26 after a frame-length assumption went wrong: STATUS and
// FLEET_MACS both pad to the 250-byte bucket, so a Kismet capture that binned by LENGTH could not
// tell them apart, and 88% of the traffic was attributed to the wrong one. Bucketing works, which
// is exactly why length is not an identifier. Count the decoded type instead of inferring it.
#define SNIFF_TYPE_MAX 16
static volatile uint32_t s_type[SNIFF_TYPE_MAX];
static uint8_t  s_src[SNIFF_SRC_MAX][6];
static uint32_t s_src_n_frames[SNIFF_SRC_MAX];
static volatile uint32_t s_src_n;

// Track per-source rates. Boards randomise their ESP-NOW source MAC, so a board may occupy more
// than one slot over a long run; the TOTAL rate is the reliable figure and per-source is a hint.
static void note_src(const uint8_t *sa)
{
    for (uint32_t i = 0; i < s_src_n; i++)
        if (memcmp(s_src[i], sa, 6) == 0) { s_src_n_frames[i]++; return; }
    if (s_src_n < SNIFF_SRC_MAX) {
        memcpy(s_src[s_src_n], sa, 6);
        s_src_n_frames[s_src_n] = 1;
        s_src_n++;
    }
}

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
    // Counted BEFORE the decrypt: this is precisely what a keyless adversary can tally, and the
    // rate of it is the tell that padding and jitter cannot touch.
    s_raw++;
    note_src(f + SRC_OFF);
    const uint8_t *ef = f + ENOW_HDR;                                      // our radar_wire frame
    // rx_ctrl.sig_len INCLUDES the 4-byte hardware FCS. v3 matched a leading magic byte and never
    // cared, but v4 derives the ciphertext length from the frame length -- leaving the FCS on
    // shifts the tag offset by 4 and authentication fails on every single frame. Caught only
    // because the keyed control run also read zero; the unkeyed run alone would have looked like
    // a pass.
    if (len < ENOW_HDR + WIFI_FCS_LEN) return;
    size_t eflen = (size_t)(len - ENOW_HDR - WIFI_FCS_LEN);

    // Wire v4 removed the plaintext magic and type byte, so there is nothing to match on and the
    // only way to identify a frame as ours is to AUTHENTICATE it. That is precisely the property
    // this tool exists to verify: an adversary CANNOT do the next line, because it needs the key.
    // `wtype` not `type`: the promiscuous callback's own parameter already owns that name.
    uint8_t wtype, pl[RADAR_FRAME_MAX], salt[RADAR_SALT_LEN]; size_t plen; uint64_t ctr;
    if (radar_wire_open(ef, eflen, SIMULACRA_ESPNOW_KEY,
                        &wtype, pl, sizeof pl, &plen, salt, &ctr) != 0)
        return;                                                            // not our link
    // ACCEPTANCE TEST (2026-08-25, passed): swapping SIMULACRA_ESPNOW_KEY above for a wrong key
    // makes this board see ZERO frames, while the keyed build sees REQ/STAT normally under the
    // same conditions. That difference IS wire v4's purpose. Do NOT gate that swap behind a new
    // -D flag: main/CMakeLists.txt forwards only an allowlist of flag names to the compiler, so an
    // unlisted -D silently does nothing and the build quietly keeps the real key -- which looked
    // exactly like a passing test until the keyed control run contradicted it.

    const uint8_t *sa = f + SRC_OFF;
    bool laa = (sa[0] & 0x02) != 0;                                        // locally-administered bit

    if (wtype < SNIFF_TYPE_MAX) s_type[wtype]++;    // every decoded type, no inference

    if (wtype == RADAR_TYPE_FLEET_MACS) {
        s_fleet++;
    } else if (wtype == RADAR_TYPE_REQUEST) {
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
        vTaskDelay(pdMS_TO_TICKS(15000));
        uint32_t now  = (uint32_t)(esp_timer_get_time() / 1000);
        uint32_t idle = s_status ? (now - s_last_status_ms) : 0;
        ESP_LOGW(TAG, "counts: REQ=%u STATUS=%u (src-MAC LAA=%u factory=%u) last-STATUS=%ums-ago",
                 (unsigned)s_req, (unsigned)s_status,
                 (unsigned)s_status_laa, (unsigned)s_status_factory, (unsigned)idle);
        // RATE is the headline. Reported per SOURCE as well as in total, because the 2026-08-25
        // baseline is per-board (~25/min then; median ambient device 0.000/min).
        uint32_t el = now - s_start_ms;
        if (el >= 1000) {
            // 64-bit: n * 6000000 overflows uint32 past ~715 frames, which any run long enough to
            // measure a rate will exceed. x100 fixed point, printed as int.frac.
            #define RATE_X100(n) ((uint32_t)((uint64_t)(n) * 6000000ULL / (uint64_t)el))
            uint32_t nsrc = s_src_n ? s_src_n : 1;
            uint32_t tot = RATE_X100(s_raw), per = tot / nsrc;
            ESP_LOGW(TAG, "RATE: raw=%u fleet_macs=%u over %us -> %u.%02u/min TOTAL, "
                          "%u.%02u/min per src (%u srcs)",
                     (unsigned)s_raw, (unsigned)s_fleet, (unsigned)(el / 1000),
                     (unsigned)(tot / 100u), (unsigned)(tot % 100u),
                     (unsigned)(per / 100u), (unsigned)(per % 100u), (unsigned)s_src_n);
            {   // Composition by decoded type, with the undecoded remainder made explicit.
                static const char *tn[SNIFF_TYPE_MAX] = {
                    "?0", "REQUEST", "STATUS", "LEARN_OFFER", "LEARN_SYNC", "SIG_SYNC",
                    "FLEET_MACS", "CONFIG", "ENR_OFFER", "ENR_REQ", "ENR_GRANT",
                    "?11", "?12", "?13", "?14", "?15" };
                uint32_t dec = 0;
                for (uint32_t t = 0; t < SNIFF_TYPE_MAX; t++) dec += s_type[t];
                for (uint32_t t = 0; t < SNIFF_TYPE_MAX; t++) {
                    if (!s_type[t]) continue;
                    uint32_t r = RATE_X100(s_type[t]);
                    ESP_LOGW(TAG, "  type %-11s %5u  %u.%02u/min  %u%% of raw",
                             tn[t], (unsigned)s_type[t], (unsigned)(r / 100u),
                             (unsigned)(r % 100u), (unsigned)(s_type[t] * 100u / (s_raw ? s_raw : 1)));
                }
                ESP_LOGW(TAG, "  UNDECODED   %5u  %u%% of raw (not ours, or lost/corrupt)",
                         (unsigned)(s_raw > dec ? s_raw - dec : 0),
                         (unsigned)((s_raw > dec ? s_raw - dec : 0) * 100u / (s_raw ? s_raw : 1)));
            }
            for (uint32_t i = 0; i < s_src_n; i++) {
                uint32_t r = RATE_X100(s_src_n_frames[i]);
                ESP_LOGW(TAG, "  src%u %02x:%02x:%02x:%02x:%02x:%02x  %u frames  %u.%02u/min",
                         (unsigned)i, s_src[i][0], s_src[i][1], s_src[i][2],
                         s_src[i][3], s_src[i][4], s_src[i][5], (unsigned)s_src_n_frames[i],
                         (unsigned)(r / 100u), (unsigned)(r % 100u));
            }
            #undef RATE_X100
        }
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
    s_start_ms = (uint32_t)(esp_timer_get_time() / 1000);   // rate denominator
    xTaskCreate(sniff_task, "esniff", 4096, NULL, 5, NULL);
    ESP_LOGW(TAG, "ESP-NOW opsec sniffer up (parked ch%d): expect NO STATUS until the CYD requests",
             ESPNOW_SNIFF_CH);
}

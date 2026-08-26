#include <string.h>
#include "templates.h"
#include "esp_random.h"
#include "host/ble_hs.h"
#include "host/ble_uuid.h"

// --- the bundle library ---
//
// Rebuilt 2026-08-26 from a census of the operator's own decoy-free captures (2821 devices across
// four sessions). The previous hand-written table was inverted against reality and was itself a
// fingerprint: only 12 shapes, and its weights bore no relation to what is actually on air.
//
//   measured, device-weighted        was shipped as        result
//   Apple    0x004C  1349 dev 47.8%  weight 16             3x UNDER-represented
//   Samsung  0x0075   137 dev  4.9%  weight 12
//   Microsoft 0x0006   74 dev  2.6%  absent from library   could not be expressed at all
//   0x0040             70 dev  2.5%  absent from library   2nd commonest vendor in stationary use
//   Nordic   0x0059     5 dev  0.2%  weight 18 (highest)   ~80x OVER-represented
//   Google   0x00E0     0 dev  0.0%  weight 10             emitted a vendor that is NOT THERE
//   Tile     0x0157     0 dev  0.0%  weight 14             emitted a vendor that is NOT THERE
//
// 63 distinct company ids appear in the captures; the old library covered 8, three of which were
// effectively absent. Roughly a fifth of the crowd was advertising vendors that do not exist in
// the operator's environment.
//
// IMPORTANT - these weights are a PRIOR, not the operating distribution. generate.c samples vendors
// from the learned rf_model, so the runtime tracks whatever is actually nearby. The library's job
// is COVERAGE: a vendor with no template here cannot be expressed no matter what the model learns,
// which is why Microsoft and 0x0040 were previously unreachable. Weights only govern the cold-start
// and fallback paths.
//
// Type fields (company, name, service uuid) are deliberately NOT unique-per-instance. Real crowds
// cluster hard - a few vendors dominate and every pair of Galaxy Buds says "Galaxy Buds". A crowd
// where no two devices share a vendor exists nowhere and would be more separable, not less.
// Uniqueness belongs to INSTANCE fields (address, uuid, namespace, serial, tx calibration).
//
// Interval bands are the measured p10..p90 of each vendor's per-device median interval. Names are
// only attached where the product is well known enough to be generic; unidentified company ids
// carry no name, which is also what most real devices do. No name string is taken from a capture.
static const device_template_t TEMPLATES[] = {
    // archetype       family            company svc     name           np  imin imax  w
    { "apple-mfg",     FMT_VENDOR_MFG,   0x004C, 0,      NULL,           0,  273,4009, 40 },
    { "samsung-mfg",   FMT_VENDOR_MFG,   0x0075, 0,      "Galaxy Buds", 35,  168,2009, 12 },
    { "ms-mfg",        FMT_VENDOR_MFG,   0x0006, 0,      NULL,           0,  106, 958,  8 },
    { "vend-0040",     FMT_VENDOR_MFG,   0x0040, 0,      NULL,           0,  122,2379,  8 },
    { "vend-06a8",     FMT_VENDOR_MFG,   0x06A8, 0,      NULL,           0,  220, 908,  3 },
    { "vend-8802",     FMT_VENDOR_MFG,   0x8802, 0,      NULL,           0,  109, 614,  2 },
    { "vend-8803",     FMT_VENDOR_MFG,   0x8803, 0,      NULL,           0,  107, 321,  2 },
    { "vend-00c4",     FMT_VENDOR_MFG,   0x00C4, 0,      NULL,           0,  107, 841,  2 },
    { "vend-f0f0",     FMT_VENDOR_MFG,   0xF0F0, 0,      NULL,           0,  158, 784,  2 },
    { "vend-0de8",     FMT_VENDOR_MFG,   0x0DE8, 0,      NULL,           0,   54, 180,  2 },
    { "vend-05a7",     FMT_VENDOR_MFG,   0x05A7, 0,      NULL,           0,  255, 539,  2 },
    { "vend-06d0",     FMT_VENDOR_MFG,   0x06D0, 0,      NULL,           0,  323, 975,  2 },
    { "vend-2502",     FMT_VENDOR_MFG,   0x2502, 0,      NULL,           0,  220,1036,  2 },
    { "amazon-mfg",    FMT_VENDOR_MFG,   0x0171, 0,      NULL,           0,  200,1500,  2 },
    { "fitness-grmn",  FMT_VENDOR_MFG,   0x0087, 0,      "vivosmart",   35,   49,2000,  2 },
    { "sensor-nordic", FMT_VENDOR_MFG,   0x0059, 0,      NULL,           0, 1998,3000,  1 },
    { "earbuds-bose",  FMT_VENDOR_MFG,   0x009E, 0,      "Bose QC",     35,  120,1200,  1 },
    { "earbuds-sony",  FMT_VENDOR_MFG,   0x012D, 0,      NULL,          25,  120,1200,  1 },
    { "ibeacon",       FMT_IBEACON,      0x004C, 0,      NULL,           0,   90,1100,  6 },
    { "eddy-uid",      FMT_EDDYSTONE_UID,0,      0xFEAA, NULL,           0,   90,1100,  3 },
    { "eddy-url",      FMT_EDDYSTONE_URL,0,      0xFEAA, NULL,           0,  650,1500,  2 },
    // Tracker family. Weight 1, not 14: Tile did not appear on ANY device in 2821 observed, and
    // this family emits a byte pattern the project's own signature DB matches as a real tracker.
    { "tile",          FMT_SVC_TRACKER,  0x0157, 0xFEED, NULL,           0, 1000,2000,  1 },
    // Minimal advertisers: real ambient BLE is mostly terse. The no-mfg structural mix
    // (generate.c pick_no_mfg_template) routes most no-mfg mass here, not to service-data beacons.
    { "minimal",       FMT_FLAGS_ONLY,   0,      0,      NULL,           0,  200,1200,  1 },
    { "svc-uuid16",    FMT_SVC_UUID16,   0,      0,      NULL,           0,  500,2000,  1 },
};

static uint16_t rnd_range(uint16_t lo, uint16_t hi) { return lo + (esp_random() % (hi - lo + 1)); }
static uint8_t  rnd_byte(void) { return (uint8_t)(esp_random() & 0xff); }

// Calibrated TX power, as a beacon reports its own 1 m reference level.
//
// Every beacon encoder used to hardcode 0xC5 (-59 dBm). Instance fields were being randomised
// carefully while this one byte stayed identical across every beacon the project has ever emitted,
// which is a single-byte filter that catches the entire population: "svc-data 0xFEAA where
// byte[3] == 0xC5". Real deployments vary - different hardware, different calibration - so draw
// from a plausible band instead. -68..-53 dBm covers what commodity beacons actually report.
static uint8_t rnd_tx_ref(void)
{
    return (uint8_t)(256 - (53 + (esp_random() % 16)));   // two's-complement -53..-68 dBm
}

size_t templates_count(void) { return sizeof(TEMPLATES) / sizeof(TEMPLATES[0]); }
const device_template_t *template_at(size_t i) { return &TEMPLATES[i]; }

const device_template_t *templates_pick(void)
{
    uint32_t total = 0;
    for (size_t i = 0; i < templates_count(); i++) total += TEMPLATES[i].weight;
    uint32_t r = esp_random() % total;
    for (size_t i = 0; i < templates_count(); i++) {
        if (r < TEMPLATES[i].weight) return &TEMPLATES[i];
        r -= TEMPLATES[i].weight;
    }
    return &TEMPLATES[0];
}

// mfg buffer: company(2) + model + status + battery + 1-3 plausible bytes
static void enc_vendor_mfg(uint16_t company_id, struct ble_hs_adv_fields *f, uint8_t *mfg)
{
    mfg[0] = (uint8_t)(company_id & 0xff);
    mfg[1] = (uint8_t)((company_id >> 8) & 0xff);
    mfg[2] = rnd_byte();                       // model/type
    mfg[3] = (uint8_t)(rnd_byte() & 0x0f);     // status flags
    mfg[4] = (uint8_t)(esp_random() % 101);    // battery 0-100
    uint8_t extra = (uint8_t)(1 + (esp_random() % 3));
    for (uint8_t i = 0; i < extra; i++) mfg[5 + i] = rnd_byte();
    f->mfg_data = mfg;
    f->mfg_data_len = (uint8_t)(5 + extra);
}

// iBeacon: Apple company 4C 00, type 0x02, length 0x15, then UUID + major + minor + tx power.
// Hardcoded prefix => can never drift into a Continuity pop-up subtype (refined Law 3).
static void enc_ibeacon(struct ble_hs_adv_fields *f, uint8_t *mfg)
{
    mfg[0] = 0x4C; mfg[1] = 0x00;          // Apple company id
    mfg[2] = 0x02; mfg[3] = 0x15;          // iBeacon type, length 21
    for (int i = 0; i < 16; i++) mfg[4 + i]  = rnd_byte();   // proximity UUID
    for (int i = 0; i < 4;  i++) mfg[20 + i] = rnd_byte();   // major + minor
    mfg[24] = rnd_tx_ref();                 // measured power, per-instance (was a fixed 0xC5)
    f->mfg_data = mfg; f->mfg_data_len = 25;
}

// Eddystone advertises under the 16-bit service UUID 0xFEAA, with the frame payload
// carried as service data for the same UUID (frame byte selects UID / URL / TLM / ...).
static const ble_uuid16_t EDDY_UUID = BLE_UUID16_INIT(0xFEAA);

static void enc_eddystone_uid(struct ble_hs_adv_fields *f, uint8_t *sd)
{
    sd[0] = 0xAA; sd[1] = 0xFE;             // service UUID (little-endian)
    sd[2] = 0x00;                           // frame type: UID
    sd[3] = rnd_tx_ref();                   // ranging tx power, per-instance
    for (int i = 0; i < 10; i++) sd[4 + i]  = rnd_byte();  // namespace
    for (int i = 0; i < 6;  i++) sd[14 + i] = rnd_byte();  // instance
    sd[20] = 0x00; sd[21] = 0x00;           // reserved
    f->uuids16 = &EDDY_UUID; f->num_uuids16 = 1; f->uuids16_is_complete = 1;
    f->svc_data_uuid16 = sd; f->svc_data_uuid16_len = 22;
}

// Eddystone-URL. Scheme byte: 0x02 "http://" or 0x03 "https://". Expansion byte selects the TLD.
//
// This used to emit exactly FOUR distinct payloads in total: one of {example, acme, store, venue},
// always https:// and always .com/. Four byte patterns, repeated forever, across every board and
// every session. The rest of the identity churned around a payload that never changed, which is
// precisely the persistent fingerprint the churn exists to prevent. The space below is ~5k
// combinations, and a per-instance numeric suffix makes repeats rarer still.
static void enc_eddystone_url(struct ble_hs_adv_fields *f, uint8_t *sd)
{
    static const char *hosts[] = {
        "example", "acme", "store", "venue", "cafe", "kiosk", "shop", "hotel",
        "museum", "airport", "campus", "clinic", "gallery", "market", "depot",
        "lounge", "studio", "arena", "transit", "library",
    };
    static const uint8_t tlds[]    = { 0x00, 0x01, 0x02, 0x07, 0x08, 0x09 };  // .com/ .org/ .edu/ .com .org .edu
    static const uint8_t schemes[] = { 0x02, 0x03 };                          // http:// https://
    const char *h = hosts[esp_random() % (sizeof hosts / sizeof hosts[0])];
    uint8_t n = 0;
    sd[n++] = 0xAA; sd[n++] = 0xFE;         // service UUID
    sd[n++] = 0x10;                         // frame type: URL
    sd[n++] = rnd_tx_ref();                 // tx power, per-instance
    sd[n++] = schemes[esp_random() % (sizeof schemes / sizeof schemes[0])];
    for (const char *c = h; *c; c++) sd[n++] = (uint8_t)*c;
    if (esp_random() % 100u < 45u) {        // sometimes a site-number suffix, as real venue beacons carry
        uint8_t k = (uint8_t)(esp_random() % 100u);
        if (k >= 10u) sd[n++] = (uint8_t)('0' + (k / 10u));
        sd[n++] = (uint8_t)('0' + (k % 10u));
    }
    sd[n++] = tlds[esp_random() % (sizeof tlds / sizeof tlds[0])];
    f->uuids16 = &EDDY_UUID; f->num_uuids16 = 1; f->uuids16_is_complete = 1;
    f->svc_data_uuid16 = sd; f->svc_data_uuid16_len = n;
}

// Tile-style tracker: advertises service UUID 0xFEED with a short id-shaped blob.
static const ble_uuid16_t TILE_UUID = BLE_UUID16_INIT(0xFEED);

static void enc_tracker(struct ble_hs_adv_fields *f, uint8_t *sd)
{
    sd[0] = 0xED; sd[1] = 0xFE;             // Tile service UUID (little-endian)
    for (int i = 0; i < 10; i++) sd[2 + i] = rnd_byte();   // tracker-id-shaped blob
    f->uuids16 = &TILE_UUID; f->num_uuids16 = 1; f->uuids16_is_complete = 1;
    f->svc_data_uuid16 = sd; f->svc_data_uuid16_len = 12;
}

// Common 16-bit service UUIDs seen in ambient advertising (battery, device-info, HID, Fast Pair,
// exposure-notification, env-sensing). Varied so the flags+uuid16 structure isn't a monoculture.
static const uint16_t SVC_UUIDS16[] = {
    0x180F, 0x180A, 0x1812, 0x181A, 0xFD6F, 0xFE9F, 0xFD5A, 0xFDCD };
static ble_uuid16_t s_svc_uuid;   // scratch for the picked UUID (single task, not reentrant)
static void enc_svc_uuid16(struct ble_hs_adv_fields *f)
{
    uint16_t u = SVC_UUIDS16[esp_random() % (sizeof SVC_UUIDS16 / sizeof SVC_UUIDS16[0])];
    s_svc_uuid = (ble_uuid16_t)BLE_UUID16_INIT(u);
    f->uuids16 = &s_svc_uuid; f->num_uuids16 = 1; f->uuids16_is_complete = 1;
}

int template_build(const device_template_t *t, uint8_t out_payload[31], uint8_t *out_len,
                   uint16_t *out_itvl_ms, uint16_t *out_company_id)
{
    struct ble_hs_adv_fields f;
    memset(&f, 0, sizeof(f));
    f.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;

    static uint8_t scratch[31];  // mfg/svc-data working buffer (single task, not reentrant)
    switch (t->family) {
        case FMT_VENDOR_MFG:    enc_vendor_mfg(t->company_id, &f, scratch); break;
        case FMT_IBEACON:       enc_ibeacon(&f, scratch); break;
        case FMT_EDDYSTONE_UID: enc_eddystone_uid(&f, scratch); break;
        case FMT_EDDYSTONE_URL: enc_eddystone_url(&f, scratch); break;
        case FMT_SVC_TRACKER:   enc_tracker(&f, scratch); break;
        case FMT_FLAGS_ONLY:    break;                    // flags only (already set) -> AD "01"
        case FMT_SVC_UUID16:    enc_svc_uuid16(&f); break;
        default: *out_len = 0; return 1;   // unreachable: every family has an encoder
    }

    if (t->name && (esp_random() % 100) < t->name_prob) {
        f.name = (uint8_t *)t->name;
        f.name_len = (uint8_t)strlen(t->name);
        f.name_is_complete = 1;
    }
    // No separate TX-power AD: beacons carry measured power inside their own payload,
    // and it would push iBeacon (flags 3 + mfg 27) over the 31-byte budget.

    uint8_t buf[BLE_HS_ADV_MAX_SZ], len = 0;
    if (ble_hs_adv_set_fields(&f, buf, &len, sizeof(buf)) != 0) { *out_len = 0; return 1; }
    memcpy(out_payload, buf, len);
    *out_len = len;
    *out_itvl_ms = rnd_range(t->itvl_min_ms, t->itvl_max_ms);
    *out_company_id = t->company_id;
    return 0;
}

int template_build_vendor_mfg(uint16_t company_id, uint8_t out_payload[31], uint8_t *out_len)
{
    struct ble_hs_adv_fields f;
    memset(&f, 0, sizeof(f));
    f.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    static uint8_t scratch[31];
    enc_vendor_mfg(company_id, &f, scratch);
    uint8_t buf[BLE_HS_ADV_MAX_SZ], len = 0;
    if (ble_hs_adv_set_fields(&f, buf, &len, sizeof(buf)) != 0) { *out_len = 0; return 1; }
    memcpy(out_payload, buf, len);
    *out_len = len;
    return 0;
}

// Real phones present on BLE as a terse advertiser on a wide, jittered interval -- not the tight
// 120-180 ms accessory band. This band spreads N personas across the interval histogram.
#define PHONE_ITVL_MIN_MS  180
#define PHONE_ITVL_MAX_MS 1000

int template_build_phone(bool apple, uint8_t out_payload[31], uint8_t *out_len, uint16_t *out_itvl_ms)
{
    struct ble_hs_adv_fields f;
    memset(&f, 0, sizeof(f));
    f.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;

    // Apple: flags-only (Continuity is off-limits -> the honest iPhone floor). Android: ~40% carry
    // a 16-bit service-UUID list (battery / device-info / HID / Google svc), else flags-only.
    if (!apple && (esp_random() % 100) < 40) enc_svc_uuid16(&f);

    uint8_t buf[BLE_HS_ADV_MAX_SZ], len = 0;
    if (ble_hs_adv_set_fields(&f, buf, &len, sizeof(buf)) != 0) { *out_len = 0; return 1; }
    memcpy(out_payload, buf, len);
    *out_len = len;
    *out_itvl_ms = rnd_range(PHONE_ITVL_MIN_MS, PHONE_ITVL_MAX_MS);
    return 0;
}

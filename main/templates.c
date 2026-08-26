#include <string.h>
#include "templates.h"
#include "rf_model.h"   // RF_MFGS_*: the learned mfg-structure variants
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
    // NO MICROSOFT (0x0006) TEMPLATE, despite the census ranking it 3rd at 2.6% of real devices.
    // Law 3 forbids ANY mfg-data element under company 0x0006, because that is how Swift Pair
    // announces itself and a false negative raises a pairing pop-up on a stranger's phone. The rule
    // is deliberately coarse: not all Microsoft mfg data is Swift Pair, but the cost of
    // over-rejecting is one missing vendor, and the cost of under-rejecting lands on a bystander.
    // Added briefly on 2026-08-26 during the library rebuild and caught by the new law3 gate in
    // generate.c, which blanked every Microsoft payload rather than emitting it.
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
    // NO TRACKER TEMPLATE. Removed 2026-08-26.
    //
    // The `tile` entry emitted service UUID 0xFEED, which is byte-for-byte the Tile signature this
    // project seeds into its OWN detector (sig_seed.c sig_id=3, pattern {0xED,0xFE} at offset 0).
    // Every tile-template decoy was therefore a guaranteed tracker match -- meaning nearby phones
    // running tracker-detection would warn their owners that an unknown Tile was travelling with
    // them. An anti-tracking tool must not make strangers' phones report a stalking device; that
    // inverts the purpose at the human level, not just the technical one.
    //
    // Nothing is lost in realism: Tile appeared on ZERO of 2821 devices across four decoy-free
    // captures. The family enum (FMT_SVC_TRACKER) is retained because learn.c still classifies a
    // genuinely observed tracker shape with it for DETECTION purposes -- classifying one is fine,
    // emitting one is not.
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

// Standard GATT appearance values, for the RF_MFGS_APPEARANCE variant. Ordinary consumer
// categories only -- appearance is a device CLASS, not an instance identifier, so a shared value
// is correct here in the same way a shared company id is.
static const uint16_t APPEARANCES[] = {
    0x0040,   // Generic Phone
    0x00C0,   // Generic Watch
    0x0341,   // Running / Walking Sensor
    0x0180,   // Generic Display
    0x03C1,   // Keyboard
    0x0941,   // Generic Audio Sink (earbuds / speakers)
};

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
    // Under Apple's company id the very next byte is the Continuity/Find My subtype selector, so a
    // uniformly random draw lands on a forbidden one 3 times in 768. 0x07 and 0x0F raise pairing
    // pop-ups on nearby phones (Law 3), and 0x12 is Find My -- which is ALSO the AirTag tracker
    // signature this project seeds into its own detector (sig_seed.c sig_id=1). A decoy rolling
    // 0x12 makes bystanders' phones warn that an unknown AirTag is travelling with them: the exact
    // harm the project exists to oppose, produced by the tool meant to prevent it.
    //
    // learn.c already re-rolls this byte (see the law3_forbidden retry in learn_render); the
    // TEMPLATE path never did, and Apple is now the highest-weighted template in the library.
    if (company_id == 0x004C)
        while (mfg[2] == 0x07 || mfg[2] == 0x0F || mfg[2] == 0x12) mfg[2] = rnd_byte();
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

// Common 16-bit service UUIDs seen in ambient advertising. Varied so the flags+uuid16 structure
// isn't a monoculture. All standard GATT services plus one widely-deployed vendor service.
//
// TWO REMOVALS, 2026-08-26:
//
// 0xFD6F -- COVID Exposure Notification. Emitting it claims to be a public-health beacon. The
// framework needs matching service DATA to be functional, so a bare UUID interferes with nothing,
// but "technically inert" is not the standard: this project has no business impersonating a health
// protocol for one entry's worth of variety in an eight-entry list.
//
// 0xFD5A -- Samsung SmartTag, which is a TRACKER service uuid seeded into this project's own
// detector (sig_seed.c sig_id=2). It probably never triggered sig_match, because that signature
// keys on service DATA and enc_svc_uuid16 emits a bare uuid LIST with no service data attached.
// "Probably never triggered OUR matcher" is the wrong bar. Other tracker-detection implementations
// are not ours to predict, and the rule set on 2026-08-26 is that a decoy never advertises a
// tracker's identifiers at all. Removing it costs nothing measurable.
//
// Replacements are ordinary GATT services carried by fitness bands, sensors and peripherals.
static const uint16_t SVC_UUIDS16[] = {
    0x180F,   // Battery Service
    0x180A,   // Device Information
    0x1812,   // Human Interface Device
    0x181A,   // Environmental Sensing
    0x180D,   // Heart Rate
    0x1816,   // Cycling Speed and Cadence
    0x1826,   // Fitness Machine
    0xFE9F,   // Google (ubiquitous, not a tracker signature)
};
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

// ---------------------------------------------------------------------------------------------
// MFG-BEARING STRUCTURE VARIANTS
//
// enc_vendor_mfg produced exactly one shape: flags + mfg ("01,ff"). A census across four
// decoy-free captures put that shape's real share at 100.0% / 50.0% / 15.6% / 0.0% -- the same
// collapse that made the hardcoded no-mfg mix a single-capture overfit. The variant is therefore
// chosen from the LEARNED mix (rf_mfgstruct_sample) rather than fixed here, and this function only
// applies the choice.
//
// Applied by APPENDING to the serialized payload rather than by setting ble_hs_adv_fields members.
// Two reasons. The host audit's serializer supports neither appearance nor tx-power, so the audit
// would measure bytes the firmware never emits. And NimBLE's field order is fixed, which would
// place appearance BEFORE mfg -- yielding "01,19,ff" where real devices emit "01,ff,19". Appending
// gives exact control of element order and is identical on host and target because it is our code.
//
// AD element layout is [len][type][value...], len counting the type byte.
int template_apply_mfg_variant(uint8_t *payload, uint8_t *len, uint8_t variant)
{
    if (!payload || !len) return 1;
    uint8_t n = *len;
    switch (variant) {
        case RF_MFGS_MFG_ONLY: {
            // Strip a leading flags element (0x02,0x01,<v>) so the advert is bare "ff". Real
            // devices genuinely do this -- 43.1% of one capture's vendor devices had no flags.
            if (n >= 3 && payload[0] == 0x02 && payload[1] == 0x01) {
                memmove(payload, payload + 3, (size_t)(n - 3));
                *len = (uint8_t)(n - 3);
            }
            return 0;
        }
        case RF_MFGS_APPEARANCE: {
            if ((int)n + 4 > 31) return 0;                 // no room: keep the base shape
            uint16_t ap = APPEARANCES[esp_random() % (sizeof APPEARANCES / sizeof APPEARANCES[0])];
            payload[n++] = 0x03; payload[n++] = 0x19;
            payload[n++] = (uint8_t)(ap & 0xFF);
            payload[n++] = (uint8_t)(ap >> 8);
            *len = n;
            return 0;
        }
        case RF_MFGS_TXPOWER: {
            if ((int)n + 3 > 31) return 0;
            payload[n++] = 0x02; payload[n++] = 0x0A;
            payload[n++] = rnd_tx_ref();                   // same per-instance draw as beacons
            *len = n;
            return 0;
        }
        case RF_MFGS_NAME:                                 // name is set through the fields path
        case RF_MFGS_FLAGS_MFG:
        default:
            return 0;                                      // base shape already correct
    }
}

#pragma once
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

// A device "archetype" - a self-contained bundle binding vendor/format, interval band, and
// (optional) name together, so generation can never produce an impossible combination.
typedef enum {
    FMT_VENDOR_MFG,     // company_id + structured blob (earbuds / fitness / sensor)
    FMT_IBEACON,        // 4C 00 02 15 + UUID + major + minor + tx
    FMT_EDDYSTONE_UID,  // svc-data 0xFEAA frame 0x00
    FMT_EDDYSTONE_URL,  // svc-data 0xFEAA frame 0x10
    FMT_SVC_TRACKER,    // service-data tracker (Tile 0xFEED)
    FMT_FLAGS_ONLY,     // flags only (AD "01") - the terse-advertiser majority of real ambient BLE
    FMT_SVC_UUID16,     // flags + complete 16-bit service UUID list (AD "01,03"), no service data
} fmt_family_t;

typedef struct {
    const char  *archetype;    // debug/inspection label
    fmt_family_t family;
    uint16_t     company_id;   // vendor-mfg family (0 otherwise)
    uint16_t     svc_uuid;     // service-data families (0xFEAA / 0xFEED)
    const char  *name;         // friendly name (NULL = nameless)
    uint8_t      name_prob;    // % chance to attach the name (0 = never)
    uint16_t     itvl_min_ms;  // joint interval band
    uint16_t     itvl_max_ms;
    uint8_t      weight;       // mix proportion (relative)
} device_template_t;

size_t                   templates_count(void);
const device_template_t *template_at(size_t i);
const device_template_t *templates_pick(void);   // weighted by .weight

// Render template `t` into a frozen advertisement: serialized AD bytes (<=31), on-air interval,
// and the company id (0 for service-data families). Returns 0 on success, nonzero if the fields
// failed to serialize (e.g. over the 31-byte budget).
int template_build(const device_template_t *t, uint8_t out_payload[31], uint8_t *out_len,
                   uint16_t *out_itvl_ms, uint16_t *out_company_id);

// Build a generic-but-valid vendor manufacturer-data advertisement for an arbitrary company id
// (for model-driven generation of vendors with no specific template). Returns 0 on success.
int template_build_vendor_mfg(uint16_t company_id, uint8_t out_payload[31], uint8_t *out_len);

// Reshape an already-serialized MFG-BEARING payload into one of the RF_MFGS_* variants.
// Applied AFTER template_build so element order is exact: NimBLE's fixed field order would put
// appearance before mfg ("01,19,ff") where real devices emit "01,ff,19", and the host audit's
// serializer supports neither appearance nor tx-power at all. Caller draws the variant from the
// learned mix (rf_mfgstruct_sample); this only applies it. Returns 0 on success.
int template_apply_mfg_variant(uint8_t *payload, uint8_t *len, uint8_t variant);

// Build a phone-plausible BLE advertisement for a cross-protocol persona. Law-3 safe by
// construction: emits only flags-only, or flags + a 16-bit service-UUID LIST (no manufacturer
// data, no service-data), so it can never trigger a Continuity / Fast-Pair pairing pop-up.
// company_id is implicitly 0. apple=true -> flags-only (the iPhone floor; no Continuity available);
// apple=false -> a 16-bit service-UUID list part of the time, else flags-only. Returns 0 on success.
int template_build_phone(bool apple, uint8_t out_payload[31], uint8_t *out_len, uint16_t *out_itvl_ms);

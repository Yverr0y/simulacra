#include <string.h>
#include "generate.h"
#include "templates.h"
#include "ble_devices.h"    // BLE_DEVICES_MAX: the real bound on the crowd (static array size)
#include "roster.h"         // make_random_static_addr_pub
#include "learn.h"          // learned templates (self-learning)
#include "law3.h"           // fail-closed gate on what may go on air
#include "esp_random.h"
#include "esp_log.h"

static const char *TAG = "generate";

// --- persona profile (factor in tenths, to avoid float in the hot path) ---
#if CONFIG_IDF_TARGET_ESP32C5
#define GEN_FACTOR_X10 15   // Ward: 1.5x
#define GEN_FLOOR      6
#else
#define GEN_FACTOR_X10 11   // Shade: 1.1x
#define GEN_FLOOR      4
#endif

// interval bin [lo,hi) edges in ms; the >2000 bin caps at 3000.
static const uint16_t ITVL_LO[RF_ITVL_BINS] = {   0,  50, 100, 200,  500, 1000, 2000 };
static const uint16_t ITVL_HI[RF_ITVL_BINS] = {  50, 100, 200, 500, 1000, 2000, 3000 };

static uint16_t rnd_range16(uint16_t lo, uint16_t hi){ return (hi<=lo)?lo:(lo+(esp_random()%(hi-lo))); }

// Weighted pick over counts[0..n); returns index, or -1 if all zero.
static int weighted_pick(const uint32_t *counts, size_t n)
{
    uint64_t total = 0; for (size_t i=0;i<n;i++) total += counts[i];
    if (total == 0) return -1;
    uint64_t r = (uint64_t)esp_random() % total;
    for (size_t i=0;i<n;i++){ if (r < counts[i]) return (int)i; r -= counts[i]; }
    return (int)n - 1;
}

// Sample an interval (ms) from a vendor slot's histogram; 0 if the slot has no samples.
static uint16_t sample_interval(const uint32_t bins[RF_ITVL_BINS])
{
    int b = weighted_pick(bins, RF_ITVL_BINS);
    if (b < 0) return 0;
    return rnd_range16(ITVL_LO[b], ITVL_HI[b]);
}

static bool is_beacon_family(fmt_family_t f)
{ return f==FMT_EDDYSTONE_UID || f==FMT_EDDYSTONE_URL || f==FMT_SVC_TRACKER; }

// First template of a family (the minimal-advertiser families have a single row each).
static const device_template_t *first_of_family(fmt_family_t fam)
{
    for (size_t i = 0; i < templates_count(); i++)
        if (template_at(i)->family == fam) return template_at(i);
    return 0;
}

// Choose the AD structure for a no-mfg decoy.
//
// LEARNED as of 2026-08-26. This function used to hardcode ~62% flags-only / ~24% flags+uuid16,
// fitted on 2026-07-13 to one capture in which flags-only advertisers were 52.7% of devices. That
// closed ad_structure to 0.153 on that capture and left it at 0.27-0.93 on every other -- the
// worst number on the scorecard, and the only generation axis rf_model could not express. Interval
// and vendor stay closed everywhere precisely because they sample the model; structure now does
// too, so it tracks whatever room the board is actually in.
//
// The hardcoded mix survives ONLY as the cold-start default, used until the model has seen
// RF_ADSTRUCT_MIN_OBS no-mfg adverts. A fresh boot in an unknown room has to emit something, and
// terse-majority is the better prior -- but it is now a starting guess that gets overwritten,
// rather than a permanent constant fitted to one July afternoon.
static const device_template_t *pick_no_mfg_template(const rf_model_t *m)
{
    const device_template_t *t = 0;
    uint8_t bin;
    if (m && rf_adstruct_sample(m, esp_random(), &bin)) {
        switch (bin) {
            case RF_ADS_FLAGS_ONLY: t = first_of_family(FMT_FLAGS_ONLY); break;
            case RF_ADS_UUID16:     t = first_of_family(FMT_SVC_UUID16); break;
            // RF_ADS_SVCDATA falls through to the beacon draw below. RF_ADS_OTHER never arrives:
            // rf_adstruct_sample excludes it and redistributes its weight, because emitting a
            // substitute at OTHER's full observed share spends real mass on a shape the room may
            // not contain at all.
            default: break;
        }
    } else {
        uint32_t r = esp_random() % 100;                    // cold start only
        if      (r < 62) t = first_of_family(FMT_FLAGS_ONLY);
        else if (r < 86) t = first_of_family(FMT_SVC_UUID16);
    }
    if (t) return t;
    // remaining share (and any fallback): a service-data beacon, weighted within the beacon families.
    uint32_t total = 0;
    for (size_t i = 0; i < templates_count(); i++)
        if (is_beacon_family(template_at(i)->family)) total += template_at(i)->weight;
    if (!total) return 0;
    uint32_t rr = esp_random() % total;
    for (size_t i = 0; i < templates_count(); i++) {
        const device_template_t *b = template_at(i);
        if (is_beacon_family(b->family)) { if (rr < b->weight) return b; rr -= b->weight; }
    }
    return 0;
}

// How many live decoys may share ONE learned shape, from how often that shape was actually seen.
//
// A learned skeleton is copied from a REAL nearby device: element order, lengths, and the fields
// learn_strip keeps verbatim (flags, service-uuid lists, tx power, appearance). rand_mask
// re-randomises the instance bytes, so identity does not leak -- but the SHAPE does not vary. With
// a uniform pick per company, if the fleet had learned exactly one shape for vendor X then EVERY
// decoy of vendor X rendered from that one skeleton, and an observer saw the original plus N
// byte-shaped clones of it.
//
// For a common model that is fine and even correct: real crowds contain many identical handsets.
// For a RARE device it is not -- several copies of something unusual is implausible on its face,
// and the promotion gate (LEARN_MIN_SIGHTINGS within a sweep) filters slow advertisers, not rare
// ones, so a rare-but-chatty device is exactly what gets learned.
//
// reinforce_count already distinguishes the two: it counts how often the shape was re-seen. Tie
// the clone budget to it, so a shape seen once appears once and only a well-attested shape gets a
// crowd. Cheap and self-correcting -- a genuinely common model earns its copies by being common.
#define LEARN_CLONE_MAX 6
static uint8_t learn_clone_budget(const learned_template_t *lt)
{
    uint32_t b = 1u + (uint32_t)lt->reinforce_count / 2u;
    return (uint8_t)(b > LEARN_CLONE_MAX ? LEARN_CLONE_MAX : b);
}

// Per-shape usage within ONE roster build. generate_roster fills the whole roster in a single
// pass, so counting here bounds concurrent live copies without any cross-call state.
static uint8_t s_clone_used[LEARN_CAP];
static void generate_reset_clone_budget(void) { memset(s_clone_used, 0, sizeof s_clone_used); }

// Map a sampled company id -> a built payload + a representative archetype index (always valid).
// 0x004C -> iBeacon; 0xFFFF (no-mfg) -> a beacon/tracker family; a templated company -> its
// template; otherwise a generic vendor-mfg carrying that company id.
static int build_for_vendor(const rf_model_t *m, uint16_t company, uint8_t out[31],
                            uint8_t *len, uint8_t *arch_idx)
{
    // Prefer a learned shape for this company when one exists (adds real-world variety).
    // archetype_idx offset scheme: >= templates_count() means learned[idx - templates_count()].
    if (learn_count() > 0) {
        size_t cand[LEARN_CAP]; size_t k = 0;
        for (size_t i = 0; i < learn_count(); i++) {
            const learned_template_t *lt = learn_at(i);
            bool match = (company == RF_VENDOR_UNKNOWN) ? (lt->company_id == 0)
                                                        : (lt->company_id == company);
            // Skip shapes that have already been cloned to their budget this build; falling
            // through to a template is better than another copy of the same real device.
            if (match && i < LEARN_CAP && s_clone_used[i] < learn_clone_budget(lt))
                cand[k++] = i;
        }
        if (k > 0) {
            size_t pick = cand[esp_random() % k];
            uint16_t itvl;
            if (learn_render(learn_at(pick), out, len, &itvl) == 0) {
                if (pick < LEARN_CAP && s_clone_used[pick] < 0xFF) s_clone_used[pick]++;
                *arch_idx = (uint8_t)(templates_count() + pick);
                return 0;
            }
        }
    }
    // observed Apple -> iBeacon (safe subtype; Law 3)
    if (company == 0x004C) {
        for (size_t i = 0; i < templates_count(); i++) {
            const device_template_t *t = template_at(i);
            if (t->family == FMT_IBEACON) {
                uint16_t itvl, cid;
                if (template_build(t, out, len, &itvl, &cid) == 0) { *arch_idx=(uint8_t)i; return 0; }
            }
        }
    }
    // no-mfg observed -> service-data/beacon template (eddystone/tile). Deliberately NOT iBeacon:
    // an iBeacon broadcasts Apple 0x004C manufacturer data, so it belongs to the 0x004C vendor slot,
    // not the no-mfg mass. Keeping OTHER on service-data families makes it no-mfg on air, like real.
    if (company == RF_VENDOR_UNKNOWN) {
        const device_template_t *t = pick_no_mfg_template(m);
        if (t) {
            uint16_t itvl, cid;
            if (template_build(t, out, len, &itvl, &cid)==0){
                for (size_t i=0;i<templates_count();i++) if (template_at(i)==t){ *arch_idx=(uint8_t)i; break; }
                return 0;
            }
        }
    }
    // a templated vendor-mfg company?
    for (size_t i = 0; i < templates_count(); i++) {
        const device_template_t *t = template_at(i);
        if (t->family==FMT_VENDOR_MFG && t->company_id==company) {
            uint16_t itvl, cid;
            if (template_build(t, out, len, &itvl, &cid)==0){ *arch_idx=(uint8_t)i; return 0; }
        }
    }
    // generic vendor-mfg for an arbitrary company; archetype = first vendor-mfg template (valid idx)
    if (template_build_vendor_mfg(company, out, len) == 0) {
        for (size_t i=0;i<templates_count();i++)
            if (template_at(i)->family==FMT_VENDOR_MFG){ *arch_idx=(uint8_t)i; break; }
        return 0;
    }
    return 1;
}

static int8_t dither_tx(void)   // plausible TX spread; not all at max
{
    static const int8_t lv[] = { -12, -9, -6, -3, 0, 3 };   // 0 -> controller default in churn_adv
    return lv[esp_random() % (sizeof(lv)/sizeof(lv[0]))];
}

// Draw a diverse built-in template into an identity, avoiding `avoid` (the over-represented company
// we're diversifying away from - the built-in earbuds-sams template is itself 0x0075). Sets
// payload/len/itvl/archetype; returns the on-air company (RF_VENDOR_UNKNOWN for service-data).
static uint16_t diversify_fill(const rf_model_t *m, identity_t *id, uint16_t avoid)
{
    // When diversifying the no-mfg (OTHER) mass, keep it no-mfg ON AIR: draw only from the
    // service-data beacon families (eddystone/tile), varied within them. Otherwise (an
    // over-represented real vendor) diversify across the whole template library.
    bool no_mfg = (avoid == RF_VENDOR_UNKNOWN);
    const device_template_t *t = no_mfg ? pick_no_mfg_template(m) : 0;
    if (!t) {                       // real over-represented vendor, or no service-data template exists
        t = templates_pick();
        for (int a = 0; a < 8 && t->company_id == avoid; a++) t = templates_pick();
        no_mfg = false;
    }
    uint16_t itvl = 0, cid = 0;
    if (template_build(t, id->payload, &id->payload_len, &itvl, &cid) != 0) id->payload_len = 0;
    id->archetype_idx = 0;
    for (size_t i = 0; i < templates_count(); i++) if (template_at(i) == t) { id->archetype_idx = (uint8_t)i; break; }
    // Prefer the ambient no-mfg interval distribution over the template's natural cadence, so the
    // diversified crowd matches the real environment's advertising rate (population realism). Falls
    // back to the template interval, then a generic 100-300ms, when the model carries no interval data.
    uint16_t amb = sample_interval(m->other_itvl_bins);
    id->adv_itvl_ms = amb ? amb : (itvl ? itvl : (uint16_t)(100 + (esp_random() % 200)));
    // Service-data beacons carry no on-air manufacturer element; report no-mfg so the label matches
    // the bytes (a tile's metadata company_id is never broadcast).
    return no_mfg ? RF_VENDOR_UNKNOWN : (cid ? cid : RF_VENDOR_UNKNOWN);
}

size_t generate_roster(const rf_model_t *m, identity_t *roster, size_t n)
{
    generate_reset_clone_budget();   // per-build: bound how often one real device gets cloned
    // build the vendor sampling table: occupied 24 slots + other(no-mfg 0xFFFF)
    uint32_t counts[RF_VENDOR_SLOTS + 1];
    uint16_t ids[RF_VENDOR_SLOTS + 1];
    int      slot[RF_VENDOR_SLOTS + 1];        // back-ref to the vendor slot (-1 = other/no-mfg)
    size_t k = 0;
    for (size_t i=0;i<RF_VENDOR_SLOTS;i++)
        if (m->vendors[i].count){ counts[k]=m->vendors[i].count; ids[k]=m->vendors[i].company_id; slot[k]=(int)i; k++; }
    if (m->other_count){ counts[k]=m->other_count; ids[k]=RF_VENDOR_UNKNOWN; slot[k]=-1; k++; }
    uint64_t total_w = 0; for (size_t i=0;i<k;i++) total_w += counts[i];

    size_t built = 0;
    for (size_t r=0;r<n;r++){
        identity_t *id=&roster[r];
        make_random_addr_mixed(id->addr);
        int vi = (k>0)? weighted_pick(counts,k) : -1;
        uint16_t company = (vi>=0)? ids[vi] : RF_VENDOR_UNKNOWN;

        // Diversity floor (per-identity, proportional, stateless -> works for bulk build AND
        // single-identity reseed, with no clustering). If the sampled vendor is over-represented in
        // the model (> GEN_MAX_VENDOR_PCT of observations), redirect a proportional fraction of its
        // draws to a varied built-in template so a monoculture model can't yield a monoculture crowd.
        bool redirect = false;
        if (vi >= 0 && total_w > 0) {
            uint64_t num = (uint64_t)counts[vi] * 100;
            uint64_t floor = (uint64_t)GEN_MAX_VENDOR_PCT * total_w;
            if (num > floor && ((uint64_t)esp_random() % num) < (num - floor)) redirect = true;
        }

        if (redirect) {
            company = diversify_fill(m, id, company);   // sets payload/len/itvl/archetype
        } else {
            uint8_t arch=0;
            if (build_for_vendor(m, company, id->payload, &id->payload_len, &arch)!=0){ id->payload_len=0; }
            id->archetype_idx = arch;
            // Interval from the model: this vendor's histogram for a real slot, else the ambient
            // no-mfg (OTHER) histogram. Only fall back to a generic 100-300ms when neither has data.
            uint16_t itvl = 0;
            if (vi>=0) itvl = (slot[vi]>=0) ? sample_interval(m->vendors[slot[vi]].itvl_bins)
                                            : sample_interval(m->other_itvl_bins);
            id->adv_itvl_ms = itvl ? itvl : (uint16_t)(100 + (esp_random()%200));
        }
        id->company_id = company;
        id->tx_power = dither_tx();
        // Fail-closed emission gate on the TEMPLATE path. learn.c has re-rolled forbidden bytes
        // since it was written; nothing checked template output, and the check is not redundant:
        // enc_vendor_mfg draws a random model byte straight after the company id, so an Apple
        // template could roll 0x07/0x0F (pairing pop-up on nearby phones) or 0x12 (Find My, which
        // is also this project's own seeded AirTag signature). enc_vendor_mfg now avoids those
        // three explicitly; this is the backstop for every other family and any future template.
        // Dropping the payload is the right failure: a decoy with payload_len 0 is simply not
        // built, whereas emitting one costs a bystander a stalking alert.
        if (id->payload_len && law3_forbidden(id->payload, id->payload_len)) {
            id->payload_len = 0;
            continue;
        }
        if (id->payload_len) built++;
    }
    return built;
}

// Ambient-derived crowd size for THIS board. No fleet-size divisor: boards are additive, each
// sizing itself from what it measures (see 2026-08-24-additive-fleet-population-design.md).
// The old GEN_CEILING (16/8) and CHURN_ACTIVE_SET (16) clamps are gone -- CHURN_ACTIVE_SET is the
// legacy scale that settings.c already documents as having broken the crowd on hardware when
// conflated with the real crowd size, and it capped this path far below any real environment
// (measured ambient runs 44-529 devices/min). BLE_DEVICES_MAX is the real bound: the static array.
uint8_t generate_active_target(const rf_model_t *m)
{
    int t = (int)((m->pop_ewma * GEN_FACTOR_X10 + 5) / 10);   // round(pop*factor)
    if (t < GEN_FLOOR) t = GEN_FLOOR;
    if (t > BLE_DEVICES_MAX) t = BLE_DEVICES_MAX;
    return (uint8_t)t;
}

void generate_dump_roster(const identity_t *roster, size_t n)
{
    uint16_t ids[24]; uint32_t cnt[24]; size_t k = 0;
    for (size_t i = 0; i < n; i++) {
        uint16_t c = roster[i].company_id; size_t j;
        for (j = 0; j < k; j++) if (ids[j] == c) { cnt[j]++; break; }
        if (j == k && k < 24) { ids[k] = c; cnt[k] = 1; k++; }
    }
    ESP_LOGW(TAG, "GENERATED roster n=%u distinct_companies=%u", (unsigned)n, (unsigned)k);
    for (size_t j = 0; j < k; j++)
        ESP_LOGW(TAG, "  company 0x%04X x%u", ids[j], (unsigned)cnt[j]);
}

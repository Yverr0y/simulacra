#include <string.h>
#include "probe_frame.h"
#include "esp_random.h"
#include "uniq_id.h"

// ============================================================================
// Archetype IE bodies (the frame body after the 24-byte MAC header + seq ctl).
//
// Every tail begins with a WILDCARD SSID (id 0x00, len 0) -- Law 3: we never name
// a network. The DS Parameter Set (id 0x03) carries a placeholder channel that
// probe_build_request() patches per burst. All lengths are self-consistent
// (each id,len pair's len == its data byte count).
//
// source: CAPTURE-DERIVED. The modeled iOS/Android stand-ins that lived here until
// 2026-08-26, and the shared RATES_/HT_/VHT/HE/EXTCAP_/VS_ building blocks they were
// assembled from, are gone. A census of a decoy-free capture (877 probing devices, 159
// distinct IE structures) found NONE of those eight tails on air even once, so every probe
// the fleet emitted was structurally unique to the fleet. They are not kept as dead code
// because there is nothing to fall back to: being modeled was the defect.
// The host tests (tools/probe_audit) pin the replacements.
// ============================================================================

// ============================================================================
// CAPTURE-DERIVED archetypes (2026-08-26). This is the "capture-driven enrichment milestone" the
// header above anticipated, and it was overdue: measured against a decoy-free driving capture of
// 877 probing devices carrying 159 distinct IE structures, ZERO of the eight modeled tails above
// appeared even once. Every Wi-Fi probe the fleet emitted therefore carried a structure that exists
// nowhere in ambient -- a perfect classifier, no matter how well the source MAC is randomised.
//
// Tails below are the MODAL byte content of the most common real structures, per element. What is
// retained is per-MODEL capability description, shared by every unit of that model, which is both
// why it is safe to embed and why it is useful. The SSID element is never retained (Law 3, and a
// directed probe names somebody's home network) and any frame carrying a WPS vendor IE
// (00:50:f2 subtype 04) was discarded wholesale before extraction, because WPS can carry a device
// name or UUID.
//
// Pairing across bands is inferred, not observed: a capture cannot say which 2.4 structure and
// which 5 GHz structure belong to one model, so pairs are matched on shared chipset traits
// (ExtCap and HE element lengths). Two archetypes are deliberately 2.4-ONLY, with a NULL 5 GHz
// tail -- probe_build_request returns 2 and probe.c skips, which is the correct behaviour for what
// it models: a device with no 5 GHz radio. Those exist in quantity and the old table had none.
//
// Ranks and shares are device-weighted out of that 877-device crowd.
// ---------------------------------------------------------------------------

/* rank 3: 76 dev, 9.0% -- CCK + ExtRates + DS + HT + ExtCap/11 + HE/28 */
static const uint8_t R_CCK_HE_24[] = { 0x00,0x00,
    0x01,0x04,0x82,0x84,0x8b,0x96,
    0x32,0x08,0x0c,0x12,0x18,0x24,0x30,0x48,0x60,0x6c,
    0x03,0x01,0x01,
    0x2d,0x1a,0x2d,0x40,0x1b,0xff,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x7f,0x0b,0x00,0x00,0x08,0x04,0x00,0x00,0x00,0x40,0x00,0x00,0x20,
    0xff,0x1c,0x23,0x01,0x08,0x08,0x18,0x00,0x80,0x20,0x30,0x02,0x00,0x0d,0x00,0x9f,
    0x08,0x00,0x00,0x00,0xfd,0xff,0xfd,0xff,0x39,0x1c,0xc7,0x71,0x1c,0x07 };
/* rank 4: 64 dev, 7.6% -- OFDM + HT + ExtCap/11 + VHT + HE/28 (pairs with rank 3) */
static const uint8_t R_OFDM_HE_5[] = { 0x00,0x00,
    0x01,0x08,0x8c,0x12,0x98,0x24,0xb0,0x48,0x60,0x6c,
    0x2d,0x1a,0x6f,0x00,0x1b,0xff,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x7f,0x0b,0x00,0x00,0x08,0x04,0x00,0x00,0x00,0x40,0x00,0x00,0x20,
    0xbf,0x0c,0x32,0x70,0x80,0x0f,0xfe,0xff,0x00,0x00,0xfe,0xff,0x00,0x00,
    0xff,0x1c,0x23,0x01,0x08,0x08,0x00,0x00,0x80,0x44,0x30,0x02,0x00,0x1d,0x00,0x9f,
    0x08,0x00,0x0c,0x00,0xfe,0xff,0xfe,0xff,0x39,0x1c,0xc7,0x71,0x1c,0x07 };

/* rank 5: 36 dev, 4.3% -- mixed rates + ExtRates + DS + HT + ExtCap/15 */
static const uint8_t R_MIX_EC15_24[] = { 0x00,0x00,
    0x01,0x08,0x82,0x84,0x8b,0x96,0x8c,0x12,0x98,0x24,
    0x32,0x04,0xb0,0x48,0x60,0x6c,
    0x03,0x01,0x05,
    0x2d,0x1a,0x2d,0xc8,0x1b,0xff,0xff,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x96,
    0x00,0x01,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x7f,0x0f,0x00,0x00,0x48,0x04,0x00,0x40,0x00,0x40,0x00,0x00,0x10,0x00,0x00,0x00,0x00 };
/* rank 2: 81 dev, 9.6% -- OFDM + HT + ExtCap/15 (pairs with rank 5) */
static const uint8_t R_OFDM_EC15_5[] = { 0x00,0x00,
    0x01,0x08,0x8c,0x12,0x98,0x24,0xb0,0x48,0x60,0x6c,
    0x2d,0x1a,0x6f,0x88,0x17,0xff,0xff,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x96,
    0x00,0x01,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x7f,0x0f,0x00,0x00,0x48,0x04,0x00,0x40,0x00,0x40,0x00,0x00,0x10,0x00,0x00,0x00,0x00 };

/* rank 6: 30 dev, 3.6% -- CCK + ExtRates + DS + Microsoft vendor IE (capability blob) */
static const uint8_t R_CCK_VS_24[] = { 0x00,0x00,
    0x01,0x04,0x82,0x84,0x8b,0x96,
    0x32,0x08,0x0c,0x12,0x18,0x24,0x30,0x48,0x60,0x6c,
    0x03,0x01,0x01,
    0xdd,0x07,0x00,0x50,0xf2,0x08,0x00,0x14,0x00 };
/* rank 1: 121 dev, 14.3% -- the single commonest structure in the crowd: rates + one vendor IE */
static const uint8_t R_OFDM_VS_5[] = { 0x00,0x00,
    0x01,0x08,0x8c,0x12,0x98,0x24,0xb0,0x48,0x60,0x6c,
    0xdd,0x07,0x00,0x50,0xf2,0x08,0x00,0x12,0x00 };

/* rank 7: 28 dev, 3.3% -- rates + ExtRates only. No HT at all, and 2.4 ONLY. */
static const uint8_t R_BARE_24[] = { 0x00,0x00,
    0x01,0x08,0x82,0x84,0x8b,0x96,0x8c,0x12,0x98,0x24,
    0x32,0x04,0xb0,0x48,0x60,0x6c };

/* rank 8: 22 dev, 2.6% -- non-basic CCK rates + ExtRates + HT. 2.4 ONLY. */
static const uint8_t R_HT_ONLY_24[] = { 0x00,0x00,
    0x01,0x08,0x02,0x04,0x0b,0x16,0x0c,0x12,0x18,0x24,
    0x32,0x04,0x30,0x48,0x60,0x6c,
    0x2d,0x1a,0x63,0x09,0x17,0xff,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00 };

// Weights track the measured device shares of the structures each archetype carries.
static const probe_archetype_t ARCHS[PROBE_ARCH_COUNT] = {
    [ARCH_R_VS]     = { "r-vs",     R_CCK_VS_24,   sizeof R_CCK_VS_24,
                                    R_OFDM_VS_5,   sizeof R_OFDM_VS_5,   30 },
    [ARCH_R_EC15]   = { "r-ec15",   R_MIX_EC15_24, sizeof R_MIX_EC15_24,
                                    R_OFDM_EC15_5, sizeof R_OFDM_EC15_5, 25 },
    [ARCH_R_HE]     = { "r-he",     R_CCK_HE_24,   sizeof R_CCK_HE_24,
                                    R_OFDM_HE_5,   sizeof R_OFDM_HE_5,   25 },
    [ARCH_R_BARE]   = { "r-bare",   R_BARE_24,     sizeof R_BARE_24,     NULL, 0, 12 },
    [ARCH_R_HTONLY] = { "r-htonly", R_HT_ONLY_24,  sizeof R_HT_ONLY_24,  NULL, 0,  8 },
};

const probe_archetype_t *probe_archetype(probe_arch_t a)
{
    return (a < PROBE_ARCH_COUNT) ? &ARCHS[a] : 0;
}

size_t probe_archetype_count(void) { return PROBE_ARCH_COUNT; }

probe_arch_t probe_pick_archetype(void)
{
    uint32_t total = 0;
    for (size_t i = 0; i < PROBE_ARCH_COUNT; i++) total += ARCHS[i].weight;
    if (!total) return ARCH_R_VS;
    uint32_t r = esp_random() % total;
    for (size_t i = 0; i < PROBE_ARCH_COUNT; i++) {
        if (r < ARCHS[i].weight) return (probe_arch_t)i;
        r -= ARCHS[i].weight;
    }
    return ARCH_R_VS;
}

void probe_random_mac(uint8_t out[6])
{
    for (;;) {
        for (int i = 0; i < 6; i++) out[i] = (uint8_t)(esp_random() & 0xff);
        out[0] = (uint8_t)((out[0] & 0xFC) | 0x02);   // locally-administered, unicast
        int zero = 1, ff = 1;
        for (int i = 0; i < 6; i++) { if (out[i]) zero = 0; if (out[i] != 0xff) ff = 0; }
        if (zero || ff) continue;
        if (uniq_try(out)) return;                    // shares the allocator with BLE
    }
}

// Walk the copied IE body and patch the DS Parameter Set (id 0x03) channel byte.
// Robust to archetype layout -- no per-archetype offset bookkeeping.
static void patch_ds_channel(uint8_t *body, uint16_t len, uint8_t ch)
{
    uint16_t i = 0;
    while (i + 2 <= len) {
        uint8_t id = body[i], ln = body[i + 1];
        if (id == 0x03 && ln >= 1) {
            if (i + 2 >= len) return;              // truncated DS element: nothing to patch
            body[i + 2] = ch; return;
        }
        i += 2 + ln;
    }
}

int probe_build_request(const uint8_t mac[6], uint8_t ch, probe_arch_t arch, bool band5,
                        const char *ssid, uint8_t ssid_len, uint8_t *out, size_t *out_len)
{
    const probe_archetype_t *a = probe_archetype(arch);
    if (!a) return 1;
    const uint8_t *tail = band5 ? a->tail5 : a->tail24;
    uint16_t tlen       = band5 ? a->tail5_len : a->tail24_len;
    // tlen < 2, not tlen == 0: the memcpy below copies (tlen - 2) bytes, and at tlen == 1 that
    // expression is -1 promoted to a huge size_t. Not reachable today (every archetype tail opens
    // with a 2-byte placeholder SSID element) but the guard should not depend on that invariant.
    if (!tail || tlen < 2) return 2;                   // archetype lacks this band
    if (ssid == 0) ssid_len = 0;                      // NULL -> wildcard
    if (ssid_len > 32) return 4;                      // 802.11 SSID element max
    // body = SSID element (2 + ssid_len) + the archetype tail AFTER its placeholder SSID (tlen - 2)
    if (24u + (uint32_t)tlen + (uint32_t)ssid_len > PROBE_FRAME_MAX) return 3;

    uint8_t *p = out;
    *p++ = 0x40; *p++ = 0x00;                          // frame control: mgmt/probe-req
    *p++ = 0x00; *p++ = 0x00;                          // duration
    memset(p, 0xff, 6); p += 6;                        // DA broadcast
    memcpy(p, mac, 6); p += 6;                         // SA = our randomized MAC
    memset(p, 0xff, 6); p += 6;                        // BSSID broadcast
    *p++ = 0x00; *p++ = 0x00;                          // seq control (driver overwrites)
    *p++ = 0x00; *p++ = ssid_len;                      // SSID element: wildcard (0) or directed
    if (ssid_len) { memcpy(p, ssid, ssid_len); p += ssid_len; }
    memcpy(p, tail + 2, (size_t)(tlen - 2)); p += (tlen - 2);   // tail after its placeholder SSID
    patch_ds_channel(out + 24, (uint16_t)(2u + ssid_len + (tlen - 2)), ch);
    *out_len = (size_t)(p - out);
    return 0;
}

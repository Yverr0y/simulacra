#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

// v4 (2026-08-25): the plaintext header is GONE. A frame is [nonce(12)][ciphertext][tag(16)] and
// nothing identifying rides in the clear. `type` moved into the first plaintext byte, so it is
// encrypted and authenticated rather than merely authenticated as AAD.
//
// v3 framed every frame as [0x5A 0x4D | ver | type], which told any passive listener that this was
// Simulacra specifically -- not merely "some Espressif device using ESP-NOW" -- and let them count
// nodes, tell the controller from the decoys, and see exactly when commands were issued, all
// without the key. Frame LENGTH leaked the same classification; radar_pad.h addresses that half.
//
// There is no magic byte to pre-filter on any more: a frame that is not ours fails the GCM tag and
// is dropped. That costs one AES-GCM attempt per received ESP-NOW frame, which is acceptable --
// the receive callback only fires for ESP-NOW frames addressed to broadcast or to us, and frames
// are already copied into an SPSC ring and processed off the Wi-Fi driver task (the SEC-6 fix).
//
// The NONCE LAYOUT IS UNCHANGED in v4 (salt(8)||counter(4); v2 was salt(4)||counter(8)). Do not
// touch it -- SEC-4 depends on that split.
#define RADAR_WIRE_VER 4
#define RADAR_TYPE_REQUEST 1
#define RADAR_TYPE_STATUS  2
#define RADAR_MAX_THREATS  8        // must match DETECT_MAX_THREATS
// Threat kind (shared so the renderer can label without depending on detect.h):
#define DETECT_KIND_FOLLOWER 0      // behavioral follower (persistence across epochs)
#define DETECT_KIND_KNOWN    1      // fingerprint match (known device class)
#define RADAR_KEY_LEN   32
#define RADAR_NONCE_LEN 12
#define RADAR_SALT_LEN   8          // nonce = salt(8) || counter(4 BE)
#define RADAR_TAG_LEN   16
#define RADAR_FRAME_MAX 250

typedef struct __attribute__((packed)) {
    uint32_t uptime_s; uint8_t flags;            // bit0 paused, bit1 config_mode
    uint16_t active_devices, roster_size; uint32_t probes_sent;
    uint16_t epoch, pop_ewma; uint32_t total_obs;
    uint8_t active_target, threat_count;
    struct __attribute__((packed)) {
        uint32_t hash; uint16_t vendor; uint8_t epochs; int8_t best_rssi;
        uint16_t first_epoch, last_epoch;
        uint8_t kind, class_id, category, confidence;   // KNOWN-device fields (kind=DETECT_KIND_*)
        uint8_t sessions_seen, places_seen;             // recurrence counters (escalation)
    } threats[RADAR_MAX_THREATS];
    uint8_t form_restless, form_wandering, form_bound;  // BLE shade-form counts: RPA/NRPA/static
    uint16_t battery_mv;                                 // cell voltage, 0 = no battery / no gauge
    uint8_t  battery_pct;                                // state-of-charge %, 0xFF = unavailable (ADC backend)
    uint8_t  preset;                                     // running preset: 0-5 sim_preset_t (5=TURBO), 6 CUSTOM, 0xFE MIXED, 0xFF none
} radar_wire_status_t;

typedef struct { uint8_t salt[RADAR_SALT_LEN]; uint64_t counter; bool seen; } radar_replay_t;

// Build [nonce|ct|tag] into frame, where ct = E(type || len(2 LE) || payload || padding). The
// plaintext is padded to a bucket (radar_pad.h) so frame length does not classify traffic. There is
// no AAD: no plaintext header remains, and `type` is covered by the tag as ciphertext instead.
// Returns 0 on success, <0 on error (including a payload too large for any bucket);
// *frame_len set to total bytes.
int radar_wire_seal(uint8_t *frame, size_t *frame_len, uint8_t type,
                    const uint8_t *payload, size_t payload_len,
                    const uint8_t key[32], const uint8_t salt[RADAR_SALT_LEN], uint64_t counter);

// Verify + decrypt a frame. Returns 0 on success (type/payload/salt/counter filled), <0 if the
// header/magic/tag is bad OR the plaintext would not fit in payload_cap bytes. The capacity is
// checked BEFORE decryption (mbedtls writes plaintext before it compares the tag), so an
// oversized frame can never overflow `payload` - pass sizeof(your buffer) and nothing else.
int radar_wire_open(const uint8_t *frame, size_t frame_len, const uint8_t key[32],
                    uint8_t *out_type, uint8_t *payload, size_t payload_cap, size_t *payload_len,
                    uint8_t out_salt[RADAR_SALT_LEN], uint64_t *out_counter);

// Replay gate for TELEMETRY: accept iff salt changed (peer reboot) or counter strictly newer.
// Updates st. Deliberately forgiving - a rebooted peer must be able to resume, and the worst a
// replayed STATUS/LEARN frame can do is restate stale data.
//
// NOT SUFFICIENT FOR CONTROL. The salt-change branch accepts any unfamiliar salt, so an attacker
// holding captures from two sessions can alternate them forever, and the signature (which covers
// salt||counter) re-verifies over exactly the replayed material. Commands that change behaviour
// use radar_replay_monotonic_ok against a floor persisted across reboots.
bool radar_replay_ok(radar_replay_t *st, const uint8_t salt[RADAR_SALT_LEN], uint64_t counter);

// Replay gate for CONTROL: strictly monotonic, salt-independent - no "peer rebooted" escape
// hatch. *floor must be restored from non-volatile storage at boot and re-persisted whenever
// this returns true, or a power-cycle reopens the replay window.
//
// Callers MUST verify the command's signature BEFORE calling this. Advancing the floor on an
// unauthenticated frame would let anyone in range jam it to UINT64_MAX and permanently deafen
// the control channel.
bool radar_replay_monotonic_ok(uint64_t *floor, uint64_t counter);

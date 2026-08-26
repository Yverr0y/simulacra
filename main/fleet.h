#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

// Fleet self-exclusion: a rolling table of OTHER decoys' active synthetic MACs, learned over the
// ESP-NOW link, so a decoy doesn't model / learn / detect its fleet-mates' fake devices as if they
// were real. Pure + always-built (empty when standalone). Fed by esp_now_link (RADAR_TYPE_FLEET_MACS),
// queried by observe.

#ifndef FLEET_MAC_CAP
// Peer synthetic MACs tracked. A single additive board runs up to BLE_DEVICES_MAX (32) +
// PROBE_AGENTS_MAX (16) = 48 identities, so this must hold 48 x (peers you expect to run).
// 256 covers a 5-peer fleet outright and degrades gracefully past that (LRU eviction).
//
// Was 96, sized as "~2 peers x [16 BLE + 16 probe]" back when each board ran 1/K of one fleet-wide
// crowd. Additive population (2026-08-24) made that a hard undersize: the table saturated, peer
// MACs fell out of it, and unexcluded fleetmates were counted as real ambient devices -- inflating
// the density estimate that AUTO sizes the crowd from. See broadcast_fleet_macs in esp_now_link.c
// for the other half of the same bug.
#define FLEET_MAC_CAP 256
#endif
#ifndef FLEET_BCAST_MACS_MAX
// Max MACs per FLEET_MACS frame (sealed <=250: 32*6+1=193 + 32 = 225). This is a HARD LIMIT of the
// ESP-NOW frame size, not a tunable -- raising it overflows the frame. A board with more identities
// than this sends multiple chunks; see broadcast_fleet_macs.
#define FLEET_BCAST_MACS_MAX 32
#endif
#ifndef FLEET_MAC_TTL_MS
// Forget a peer synthetic MAC not re-heard within this.
//
// Raised from 90 s on 2026-08-26, when the broadcast became DELTA-based. Under the old
// send-everything-every-25 s scheme a 90 s TTL gave 3 attempts of margin. Deltas only carry what
// CHANGED, so an unchanged MAC is not re-sent and would expire mid-life at 90 s. The TTL must now
// cover the FULL-RESYNC period instead, with the same 3x margin: resync runs every 3-4 min, so
// 12 min holds. A stale exclusion is cheap - the MAC belongs to a departed peer and is no longer
// on air, so excluding it costs nothing - whereas a PREMATURELY expired one is expensive: the
// fleetmate gets counted as a real ambient device, which is the population feedback loop measured
// on 2026-08-25 (32 -> 65 -> 33 -> 42 with ambient provably flat).
#define FLEET_MAC_TTL_MS 720000u  // 12 min (>= 3x the full-resync period)
#endif
#ifndef FLEET_NODE_TTL_MS
// Peer NODE liveness is a SEPARATE, much shorter TTL. It answers "who is here right now", which
// must stay responsive: a board carried out of range should stop counting as present promptly.
// Sharing FLEET_MAC_TTL_MS would have made a departed peer look live for 12 min.
#define FLEET_NODE_TTL_MS 90000u  // 1.5 min
#endif
#ifndef FLEET_NODE_CAP
#define FLEET_NODE_CAP 8          // distinct peer nodes tracked (real ESP-NOW hardware MACs, not synthetic)
#endif

#define RADAR_TYPE_FLEET_MACS 6   // decoy -> all: my current active synthetic MACs

void   fleet_reset(void);
// Add/refresh peer synthetic MACs heard over the fleet link. Reuses free/expired slots, else evicts
// the oldest.
void   fleet_note_peer_macs(const uint8_t (*macs)[6], size_t n, uint32_t now_ms);
// True iff `mac` is a known, non-expired fleet-peer MAC (caller skips detect/learn/model for it).
bool   fleet_mac_excluded(const uint8_t mac[6], uint32_t now_ms);
size_t fleet_peer_count(uint32_t now_ms);        // non-expired entries (tests/diag)

// Note a live peer NODE (its real ESP-NOW hardware sender MAC -- info->src_addr on receipt of a
// FLEET_MACS broadcast), refreshing if already known. Separate table from the synthetic-MAC
// exclusion table above (different purpose: node IDENTITY, not MAC content).
void   fleet_note_peer_node(const uint8_t mac[6], uint32_t now_ms);
// Distinct non-expired peer NODES heard from recently (does not count this node -- ESP-NOW never
// delivers a station's own transmission to its own receive callback).
size_t fleet_node_count(uint32_t now_ms);

// Wire framing: [uint8 count][count * 6 MAC bytes]. Pure, unit-testable.
size_t fleet_macs_pack(uint8_t *out, size_t out_max, const uint8_t (*macs)[6], size_t n);
size_t fleet_macs_unpack(const uint8_t *in, size_t len, uint8_t (*macs)[6], size_t max);

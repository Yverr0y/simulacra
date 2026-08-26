#pragma once
#include <stdint.h>

// "Let the controller pick" sentinel for identity_t.tx_power.
//
// This was 0, which collides with a perfectly ordinary power level. dither_tx()'s ladder contained
// 0, so 1 in 6 decoys silently transmitted at controller MAXIMUM instead of 0 dBm; and
// ble_device_sync set 0 for every persona, so ALL persona decoys ran at max. Personas are roughly a
// third of the crowd. Together that inflated the loud end of the population's RSSI distribution and
// narrowed its spread -- the very axis being measured when the collision was found (decoy median
// -58 dBm against an ambient -68 to -71).
//
// INT8_MIN is safe as a sentinel: no BLE radio accepts -128 dBm as a setting.
#define IDENTITY_TX_DEFAULT ((int8_t)-128)

typedef struct {
    uint8_t    addr[6];          // stable random-static MAC (top 2 bits set)
    uint16_t   company_id;       // vendor company id (debug/inspection)
    uint8_t    payload[31];      // frozen, serialized AD bytes
    uint8_t    payload_len;
    uint16_t   adv_itvl_ms;      // this identity's on-air interval
    int8_t     tx_power;         // dBm, or IDENTITY_TX_DEFAULT for the controller default
    uint8_t    archetype_idx;    // index into TEMPLATES[], for inspection/test
} identity_t;

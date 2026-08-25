#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "radar_wire.h"        // RADAR_FRAME_MAX

// Spread retransmit scheduler for the ESP-NOW link.
//
// Broadcast ESP-NOW is unacknowledged, so every logical message goes out more than once. Before
// 2026-08-25 those repeats were sent back-to-back inside ~20 ms, putting a burst of identical-length
// frames from one MAC on air -- a recognizable retransmit signature that survives encryption, length
// bucketing and MAC randomization alike. Spacing them by a jittered 40-120 ms makes them read as
// unrelated frames instead of an obvious train.
//
// This module owns SCHEDULING ONLY; the caller transmits. That keeps it free of ESP-NOW and
// therefore host-testable (synth_dump --retx / --retxadapt).
//
// Repeats are deliberately byte-identical. Re-sealing each one with a fresh counter would avoid
// identical bytes on air, but it would also advance the CONFIG monotonic replay floor once per
// repeat and burn counter space for no security gain.

#define RADAR_RETX_MIN_GAP_MS 40
#define RADAR_RETX_MAX_GAP_MS 120

#define RADAR_RETX_MAX_REPEATS 4
#define RADAR_RETX_MIN_REPEATS 1

typedef struct {
    uint8_t  frame[RADAR_FRAME_MAX];
    size_t   len;
    uint8_t  left;            // sends still owed (0 = idle)
    uint32_t next_ms;
} radar_retx_t;

// Arm with a sealed frame. The first send is due immediately.
void radar_retx_arm(radar_retx_t *r, const uint8_t *frame, size_t len,
                    uint8_t repeats, uint32_t now_ms);

// True when the caller should transmit r->frame now. Decrements the owed count and schedules the
// next send. `jitter` is a fresh random value from the caller (esp_random() on target), used only
// when another send remains -- passing it in keeps this pure and testable.
bool radar_retx_due(radar_retx_t *r, uint32_t now_ms, uint32_t jitter);

// Adapt the repeat count from observed delivery. Relaxes one step per fully-delivered cycle,
// resets to maximum on any miss. Deliberately asymmetric: an unheard REQUEST costs a stale
// console, an extra frame costs only exposure.
uint8_t radar_retx_adapt(uint8_t cur, bool all_answered);

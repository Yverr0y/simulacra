#pragma once

// Opsec verifier (SIMULACRA_ESPNOW_SNIFF). Parks a spare board on channel 1 in promiscuous mode
// and decodes the radar ESP-NOW link's frames USING the fleet key: logs each REQUEST/STATUS with
// its 802.11 source MAC (+ whether it is locally-administered), the frame length (so bucketing is
// observable) and a ciphertext sample, plus running counts so "decoy stays silent until the CYD
// asks" and source-MAC hygiene stay verifiable. Wi-Fi-only; NimBLE never starts. Flash to e.g. the
// SparkFun C6.
//
// It USED to work without the key, by matching a plaintext magic. Wire v4 removed that header
// deliberately: a passive adversary can no longer identify these frames as ours at all, which is
// the entire point of v4. Needing the key here is the correct asymmetry, and "an unkeyed build
// sees nothing" is v4's acceptance test.
void espnow_sniff_start(void);

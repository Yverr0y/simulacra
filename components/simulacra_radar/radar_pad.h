#pragma once
#include <stddef.h>

// Frame-length bucketing for the ESP-NOW link.
//
// Removing the plaintext type byte (wire v4) accomplishes little on its own: REQUEST ~32 B,
// CONFIG ~95 B and STATUS ~206 B were cleanly separable clusters, so LENGTH classified traffic
// just as well as the type field did. Padding every frame up to one of a few fixed sizes breaks
// that mapping.
//
// Buckets are TOTAL FRAME sizes. A frame is nonce(12) + ciphertext + tag(16), and the ciphertext
// is the padded plaintext, which itself carries a 3-byte prefix (type + length). So:
//     padded plaintext = bucket - 28
//     max real payload = bucket - 31
//
// Pure: no mbedTLS, no ESP-IDF. Host-tested via synth_dump --padbucket.
//
// HONEST LIMIT: bucketing reduces the length channel, it does not close it. STATUS still lands in
// the 250 bucket and REQUEST in the 64 bucket every time, so an observer can still separate "small
// frequent frames from one MAC" from "large frames from several". Closing that fully means padding
// everything to 250, which nearly 8x's REQUEST airtime -- and airtime is itself exposure. See
// docs/superpowers/specs/2026-08-25-link-signature-reduction-design.md section 2.

#define RADAR_PAD_HDR 3        // plaintext prefix: type(1) + len(2 LE)

// Padded plaintext length for `payload_len` bytes of real payload.
// Returns 0 if the payload cannot fit the largest bucket.
size_t radar_pad_plaintext_len(size_t payload_len);

// Largest payload that fits any bucket (219 bytes).
size_t radar_pad_max_payload(void);

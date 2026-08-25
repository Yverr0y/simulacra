#include "radar_wire.h"
#include "radar_pad.h"
#include <string.h>
#include "mbedtls/gcm.h"

// Padding randomness. radar_wire.c is component code that may be compiled outside ESP-IDF, so the
// ESP-IDF RNG is reached through a shim rather than an unguarded include.
#if defined(ESP_PLATFORM)
#include "esp_random.h"
static inline void wire_fill_random(void *buf, size_t n) { esp_fill_random(buf, n); }
#else
#include <stdlib.h>
static inline void wire_fill_random(void *buf, size_t n)
{ uint8_t *p = (uint8_t *)buf; for (size_t i = 0; i < n; i++) p[i] = (uint8_t)(rand() & 0xFF); }
#endif

// nonce = salt(8) || counter(4 BE). Wire v3 rebalanced this from salt(4)||counter(8).
//
// GCM nonce reuse under a shared key is catastrophic - it leaks the XOR of two plaintexts AND the
// GHASH authentication key, turning a confidentiality break into forgery. Uniqueness here rests on
// the salt, which is redrawn every boot: with 4 bytes, a fleet sharing one key had even odds of a
// collision after ~2^16 (device, boot) pairs, which a long-lived fleet reaches. 8 bytes moves that
// to ~2^32 boots. The counter only has to be unique *within* one salt, so 32 bits is ample; senders
// still never reset it across reboots (see the Vigil's NVS reservation) because the CONFIG replay
// floor is monotonic over exactly this value.
static void make_nonce(uint8_t nonce[12], const uint8_t salt[RADAR_SALT_LEN], uint64_t counter)
{
    memcpy(nonce, salt, RADAR_SALT_LEN);
    for (int i = 0; i < 4; i++) nonce[RADAR_SALT_LEN + i] = (uint8_t)(counter >> (8 * (3 - i)));
}

int radar_wire_seal(uint8_t *frame, size_t *frame_len, uint8_t type,
                    const uint8_t *payload, size_t payload_len,
                    const uint8_t key[32], const uint8_t salt[RADAR_SALT_LEN], uint64_t counter)
{
    size_t ptlen = radar_pad_plaintext_len(payload_len);
    if (ptlen == 0) return -1;                       // payload too large for any bucket

    // Plaintext: [type][len LE][payload][random padding]. Padding is random rather than zeros --
    // GCM would hide a zero run anyway, but random costs nothing and removes the question.
    uint8_t pt[RADAR_FRAME_MAX];
    pt[0] = type;
    pt[1] = (uint8_t)(payload_len & 0xFF);
    pt[2] = (uint8_t)((payload_len >> 8) & 0xFF);
    if (payload_len) memcpy(pt + RADAR_PAD_HDR, payload, payload_len);
    size_t used = RADAR_PAD_HDR + payload_len;
    if (ptlen > used) wire_fill_random(pt + used, ptlen - used);

    uint8_t nonce[12]; make_nonce(nonce, salt, counter);
    memcpy(frame, nonce, RADAR_NONCE_LEN);
    uint8_t *ct  = frame + RADAR_NONCE_LEN;
    uint8_t *tag = ct + ptlen;

    mbedtls_gcm_context g; mbedtls_gcm_init(&g);
    int rc = mbedtls_gcm_setkey(&g, MBEDTLS_CIPHER_ID_AES, key, 256);
    // No AAD: no plaintext header remains. `type` is covered by the tag as ciphertext, which is
    // strictly stronger than its old AAD treatment.
    if (rc == 0) rc = mbedtls_gcm_crypt_and_tag(&g, MBEDTLS_GCM_ENCRYPT, ptlen,
                          nonce, RADAR_NONCE_LEN, NULL, 0,
                          pt, ct, RADAR_TAG_LEN, tag);
    mbedtls_gcm_free(&g);
    if (rc != 0) return -2;
    *frame_len = RADAR_NONCE_LEN + ptlen + RADAR_TAG_LEN;
    return 0;
}

int radar_wire_open(const uint8_t *frame, size_t frame_len, const uint8_t key[32],
                    uint8_t *out_type, uint8_t *payload, size_t payload_cap, size_t *payload_len,
                    uint8_t out_salt[RADAR_SALT_LEN], uint64_t *out_counter)
{
    if (frame_len < RADAR_NONCE_LEN + RADAR_PAD_HDR + RADAR_TAG_LEN) return -1;
    // Pre-decrypt bound. ESP-NOW v2 (IDF >= 5.4) carries up to 1470 B, well past the 250 B v1 cap
    // these buffers are sized for, and mbedtls writes plaintext BEFORE it compares the tag (it
    // zeroises on mismatch, but the write already happened). Capping frame_len here is what makes
    // the local scratch buffer below sufficient by construction -- the SEC-2 finding.
    if (frame_len > RADAR_FRAME_MAX) return -1;
    const uint8_t *nonce = frame;
    size_t ptlen = frame_len - RADAR_NONCE_LEN - RADAR_TAG_LEN;
    const uint8_t *ct  = nonce + RADAR_NONCE_LEN;
    const uint8_t *tag = ct + ptlen;

    // Decrypt into a LOCAL buffer, never the caller's. v3 decrypted straight into `payload`, so the
    // pre-auth write above landed in the caller's memory; v4 keeps the caller's buffer untouched
    // until the tag has verified AND the recovered length has been checked against payload_cap.
    uint8_t pt[RADAR_FRAME_MAX];

    mbedtls_gcm_context g; mbedtls_gcm_init(&g);
    int rc = mbedtls_gcm_setkey(&g, MBEDTLS_CIPHER_ID_AES, key, 256);
    if (rc == 0) rc = mbedtls_gcm_auth_decrypt(&g, ptlen, nonce, RADAR_NONCE_LEN,
                          NULL, 0, tag, RADAR_TAG_LEN, ct, pt);
    mbedtls_gcm_free(&g);
    if (rc != 0) return -2;                             // bad tag / bad key / not ours

    size_t plen = (size_t)pt[1] | ((size_t)pt[2] << 8);
    if (RADAR_PAD_HDR + plen > ptlen) return -1;        // length field disagrees with the frame
    if (plen > payload_cap) return -1;                  // caller's buffer is too small

    *out_type = pt[0];
    *payload_len = plen;
    if (plen) memcpy(payload, pt + RADAR_PAD_HDR, plen);
    memcpy(out_salt, nonce, RADAR_SALT_LEN);
    uint64_t c = 0; for (int i = 0; i < 4; i++) c = (c << 8) | nonce[RADAR_SALT_LEN + i];
    *out_counter = c;
    return 0;
}

bool radar_replay_monotonic_ok(uint64_t *floor, uint64_t counter)
{
    if (counter <= *floor) return false;
    *floor = counter;
    return true;
}

bool radar_replay_ok(radar_replay_t *st, const uint8_t salt[RADAR_SALT_LEN], uint64_t counter)
{
    if (!st->seen || memcmp(st->salt, salt, RADAR_SALT_LEN) != 0) {   // fresh or peer rebooted
        memcpy(st->salt, salt, RADAR_SALT_LEN); st->counter = counter; st->seen = true; return true;
    }
    if (counter > st->counter) { st->counter = counter; return true; }
    return false;
}

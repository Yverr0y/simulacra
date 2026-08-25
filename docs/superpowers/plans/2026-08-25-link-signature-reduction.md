# Link Signature Reduction Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Remove every project-identifying discriminator from the ESP-NOW link's on-air frames, and cut the number of frames each logical message costs.

**Architecture:** Wire v4 deletes the 4-byte plaintext header and moves `type` inside the ciphertext; frames are padded to three size buckets so length stops classifying traffic. Retransmits are spread in time instead of back-to-back, and the Vigil adapts its repeat count to the delivery it actually observes. `radar_wire_seal`/`radar_wire_open` keep their exact signatures, so all 12 call sites are untouched.

**Tech Stack:** C (ESP-IDF v5.5 for ESP32-C5, v5.4 for ESP32-C6 and the classic-ESP32 CYD), mbedTLS AES-256-GCM, Python 3.12 `unittest` for host harnesses, MSVC `cl` on Windows / `cc` + `make` on POSIX CI.

**Spec:** `docs/superpowers/specs/2026-08-25-link-signature-reduction-design.md`

## Global Constraints

- **Whole-fleet reflash required.** `RADAR_WIRE_VER` 3 -> 4 is breaking in both directions: a v3 node rejects a v4 frame for a missing magic, and v4 fails to authenticate a v3 frame because the removed header shifts every byte after it. The failure is clean but total. This is the **second** breaking wire change in two days (CONFIG_WIRE_VER 1 -> 2 landed 2026-08-24); both require the same all-boards-together flash.
- **`radar_wire_seal` and `radar_wire_open` signatures must not change.** Twelve call sites across `main/`, `cyd/main/`, and `main/churn_selftest.c` depend on them. `type` stays a parameter; only its wire position moves.
- **The SEC-2 guard must survive.** mbedTLS writes plaintext before comparing the tag, so an oversized frame must be rejected *before* decryption. v4 makes this structurally stronger (decrypt into a local scratch buffer, copy to the caller only after authentication), but the property must hold.
- **Crypto construction is unchanged.** AES-256-GCM, `nonce = salt(8)||counter(4 BE)`, and both replay gates (`radar_replay_ok`, `radar_replay_monotonic_ok`) stay exactly as they are. Do not touch SEC-4's nonce split.
- **CONFIG keeps unconditional 4x redundancy.** Adaptive backoff applies to REQUEST only. A silently dropped CONFIG means the console reports a command the fleet never applied - the DRIFT-1 failure class this project has already shipped once.
- **Two-places rule for host harnesses.** A new source file added to a tool must be added to BOTH that tool's `run.ps1` (Windows/MSVC) and its `Makefile` (POSIX/CI).
- **Per-target IDF versions do not mix in one shell.** C5 = IDF 5.5, C6 and CYD = IDF 5.4. Building two targets in a single PowerShell invocation fails with `No module named 'esp_idf_monitor'`; use a fresh shell per target.
- Commit identity is the repo-local default. Do not add `Co-Authored-By:` or `Claude-Session:` trailers.

## Frame Layout Reference

Every task below assumes these numbers. They are derived, not guessed - check them before relying on them.

```
v3 (current):  [magic0|magic1|ver|type] [nonce 12] [ciphertext] [tag 16]
v4 (target):                            [nonce 12] [ciphertext] [tag 16]
                                                    ^ = E(type(1) || len(2 LE) || payload || pad)
```

| | v3 | v4 |
|---|---|---|
| Frame overhead | 4 + 12 + 16 = 32 | 12 + 16 = **28** |
| Plaintext overhead | 0 | type(1) + len(2) = **3** |
| Max payload | 250 - 32 = 218 | 250 - 28 - 3 = **219** |

v4 allows one byte *more* payload than v3, so no existing frame type can break.

**Buckets** are total frame sizes: **64, 128, 250**. Therefore padded-plaintext sizes are `bucket - 28` = **36, 100, 222**, and max real payload per bucket is `bucket - 31` = **33, 97, 219**.

Current traffic maps as follows (verify during implementation rather than trusting this table):

| Type | Payload | Bucket |
|---|---|---|
| `REQUEST` | 4 | 64 |
| `CONFIG` | `CONFIG_WIRE_PAYLOAD_LEN` = 67 | 128 |
| `STATUS` | ~174 (observed 206 B sealed in v3, minus 32 overhead) | 250 |
| `FLEET_MACS` | up to 1 + 32*6 = 193 | 250 |
| `LEARN_OFFER` / `LEARN_SYNC` / `SIG_SYNC` | chunked, <= 218 today | 250 |

## File Structure

- **`components/simulacra_radar/radar_pad.{c,h}`** *(new)* - pure bucket arithmetic. No mbedTLS, no ESP-IDF, so a host harness can compile and test it. Follows the project's established pure-core pattern (`law3.c`, `fleet_pop.c`, `probe_agents.c`).
- **`components/simulacra_radar/radar_wire.{c,h}`** - v4 framing. Constants change, function signatures do not.
- **`main/esp_now_link.c`** - decoy STATUS repeat count and spreading.
- **`cyd/main/cyd_main.c`** - Vigil REQUEST spreading + adaptive count; CONFIG spreading at fixed 4x.
- **`main/espnow_sniff.c`** - reworked to decrypt with the key, since unkeyed matching is exactly what v4 removes.
- **`main/churn_selftest.c`** - v4 layout pinned on-target.
- **`tools/decoy_audit/`** - host tests for the pure bucket logic and the adaptive-repeat helper.

---

### Task 1: Pure bucket arithmetic

Smallest self-contained piece, host-testable, and every later task depends on its numbers.

**Files:**
- Create: `components/simulacra_radar/radar_pad.c`, `components/simulacra_radar/radar_pad.h`
- Modify: `tools/decoy_audit/run.ps1`, `tools/decoy_audit/Makefile` (both - see Global Constraints)
- Modify: `tools/decoy_audit/synth_dump.c` (add `--padbucket` mode)
- Test: `tools/decoy_audit/tests/test_radar_pad.py` (create)

**Interfaces:**
- Consumes: nothing.
- Produces:
  - `size_t radar_pad_plaintext_len(size_t payload_len)` - padded plaintext size for a payload, or `0` if the payload cannot fit any bucket.
  - `size_t radar_pad_max_payload(void)` - largest payload that fits the biggest bucket (219).
  - `#define RADAR_PAD_HDR 3` - the `type(1) + len(2)` plaintext prefix.

- [ ] **Step 1: Write the failing test**

Create `tools/decoy_audit/tests/test_radar_pad.py`:

```python
import os, subprocess, unittest

HERE = os.path.dirname(os.path.abspath(__file__)); TOOL = os.path.dirname(HERE)
EXE  = os.path.join(TOOL, "synth_dump.exe" if os.name == "nt" else "synth_dump")

FRAME_OVERHEAD = 28          # nonce(12) + tag(16)
BUCKETS = (64, 128, 250)     # total frame sizes


def padded(payload_len):
    out = subprocess.check_output([EXE, "--padbucket", str(payload_len)], text=True)
    return int(out.strip())


@unittest.skipUnless(os.path.exists(EXE), "synth_dump not built")
class RadarPad(unittest.TestCase):
    def test_every_result_is_a_real_bucket(self):
        for n in (0, 1, 4, 33, 34, 67, 97, 98, 174, 193, 219):
            frame = padded(n) + FRAME_OVERHEAD
            self.assertIn(frame, BUCKETS, f"payload {n} -> frame {frame}, not a bucket")

    def test_picks_the_smallest_bucket_that_fits(self):
        self.assertEqual(padded(4) + FRAME_OVERHEAD, 64)     # REQUEST
        self.assertEqual(padded(33) + FRAME_OVERHEAD, 64)    # exactly fills the small bucket
        self.assertEqual(padded(34) + FRAME_OVERHEAD, 128)   # one byte over -> next bucket
        self.assertEqual(padded(67) + FRAME_OVERHEAD, 128)   # CONFIG
        self.assertEqual(padded(97) + FRAME_OVERHEAD, 128)   # exactly fills the middle bucket
        self.assertEqual(padded(98) + FRAME_OVERHEAD, 250)
        self.assertEqual(padded(219) + FRAME_OVERHEAD, 250)  # exactly fills the largest

    def test_monotonic(self):
        # a bigger payload never yields a smaller frame
        prev = 0
        for n in range(0, 220):
            cur = padded(n)
            self.assertGreaterEqual(cur, prev, f"payload {n} shrank the frame")
            prev = cur

    def test_oversized_payload_is_refused(self):
        # 220 cannot fit: 220 + 3 + 28 = 251 > RADAR_FRAME_MAX
        self.assertEqual(padded(220), 0)
        self.assertEqual(padded(1000), 0)

    def test_padding_never_exceeds_the_frame_cap(self):
        for n in range(0, 220):
            self.assertLessEqual(padded(n) + FRAME_OVERHEAD, 250)
```

- [ ] **Step 2: Run it to verify it fails**

```
cd tools/decoy_audit && python -m unittest tests.test_radar_pad -v
```
Expected: every test errors or fails - `--padbucket` is not a known mode, so `synth_dump` prints nothing parseable.

- [ ] **Step 3: Create the header**

`components/simulacra_radar/radar_pad.h`:

```c
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
// everything to 250, which nearly 8x's REQUEST airtime - and airtime is itself exposure. See the
// design doc's section 2.

#define RADAR_PAD_HDR 3        // plaintext prefix: type(1) + len(2 LE)

// Padded plaintext length for `payload_len` bytes of real payload.
// Returns 0 if the payload cannot fit the largest bucket.
size_t radar_pad_plaintext_len(size_t payload_len);

// Largest payload that fits any bucket (219 bytes).
size_t radar_pad_max_payload(void);
```

- [ ] **Step 4: Create the implementation**

`components/simulacra_radar/radar_pad.c`:

```c
#include "radar_pad.h"
#include "radar_wire.h"        // RADAR_NONCE_LEN, RADAR_TAG_LEN, RADAR_FRAME_MAX

#define PAD_OVERHEAD (RADAR_NONCE_LEN + RADAR_TAG_LEN)     // 28

// Total frame sizes, ascending. Keep the last one <= RADAR_FRAME_MAX.
static const size_t BUCKETS[] = { 64, 128, 250 };
#define N_BUCKETS (sizeof BUCKETS / sizeof BUCKETS[0])

size_t radar_pad_plaintext_len(size_t payload_len)
{
    size_t need = payload_len + RADAR_PAD_HDR + PAD_OVERHEAD;   // total frame this payload needs
    for (size_t i = 0; i < N_BUCKETS; i++)
        if (need <= BUCKETS[i]) return BUCKETS[i] - PAD_OVERHEAD;
    return 0;                                                   // does not fit any bucket
}

size_t radar_pad_max_payload(void)
{
    return BUCKETS[N_BUCKETS - 1] - PAD_OVERHEAD - RADAR_PAD_HDR;
}
```

- [ ] **Step 5: Add the harness mode**

In `tools/decoy_audit/synth_dump.c`, add `#include "radar_pad.h"` with the other includes, then add this block immediately before the existing `--acttarget` block:

```c
    if (argc > 1 && strcmp(argv[1], "--padbucket") == 0) {
        size_t n = argc > 2 ? (size_t)strtoul(argv[2], 0, 10) : 0;
        printf("%u\n", (unsigned)radar_pad_plaintext_len(n));
        return 0;
    }
```

- [ ] **Step 6: Add the source to BOTH build files**

In `tools/decoy_audit/Makefile`, append to the `SRC :=` list:
```
$(ROOT)/components/simulacra_radar/radar_pad.c
```

In `tools/decoy_audit/run.ps1`, add the same file to the `cl` argument line:
```
..\..\components\simulacra_radar\radar_pad.c
```

Both are required. Missing one means local tests pass while CI does not compile the file, or vice versa.

- [ ] **Step 7: Rebuild and run the tests**

```
cd tools/decoy_audit && make && python -m unittest tests.test_radar_pad -v
```
Expected: 5 passed.

On Windows, rebuild with the project's own script instead:
```
powershell -NoProfile -File run.ps1 -Rebuild
```

- [ ] **Step 8: Run the full suite for regressions**

```
cd tools/decoy_audit && python -m unittest discover -s tests -p "test_*.py"
```
Expected: all pass.

- [ ] **Step 9: Commit**

```bash
git add components/simulacra_radar/radar_pad.c components/simulacra_radar/radar_pad.h \
        tools/decoy_audit/synth_dump.c tools/decoy_audit/Makefile tools/decoy_audit/run.ps1 \
        tools/decoy_audit/tests/test_radar_pad.py
git commit -m "radar_pad: frame-length bucketing for the ESP-NOW link"
```

---

### Task 2: Wire v4 framing

**Files:**
- Modify: `components/simulacra_radar/radar_wire.h:6-20` (constants), `components/simulacra_radar/radar_wire.c:20-69` (seal/open)
- Modify: `components/simulacra_radar/CMakeLists.txt` (add `radar_pad.c` to `SRCS` if the component lists sources explicitly)
- Test: `main/churn_selftest.c` (on-target; `radar_wire.c` needs mbedTLS and has no host build)

**Interfaces:**
- Consumes: `radar_pad_plaintext_len()`, `RADAR_PAD_HDR` from Task 1.
- Produces: `radar_wire_seal` / `radar_wire_open` with **identical signatures to today**. `RADAR_WIRE_VER` becomes 4. `RADAR_MAGIC0`, `RADAR_MAGIC1`, and `RADAR_HDR_LEN` are **deleted** - any remaining reference is a compile error, which is intentional (it finds every consumer).

- [ ] **Step 1: Update the constants**

In `components/simulacra_radar/radar_wire.h`, replace lines 6-8 and 19:

```c
// v4 (2026-08-25): the plaintext header is GONE. A frame is [nonce(12)][ciphertext][tag(16)] and
// nothing identifying rides in the clear. `type` moved into the first plaintext byte, so it is
// encrypted and authenticated rather than merely authenticated as AAD.
//
// v3 framed every frame as [0x5A 0x4D | ver | type], which told any passive listener that this was
// Simulacra specifically (not just "some Espressif device using ESP-NOW"), and let them classify
// controller vs decoy traffic and see exactly when commands were issued - all without the key.
//
// There is no magic byte to pre-filter on any more: a frame that is not ours fails the GCM tag and
// is dropped. That costs one AES-GCM attempt per received ESP-NOW frame, which is acceptable
// because the receive callback only fires for ESP-NOW frames addressed to broadcast or to us.
#define RADAR_WIRE_VER 4
```

Delete the `RADAR_MAGIC0`, `RADAR_MAGIC1`, and `RADAR_HDR_LEN` defines entirely. Leave
`RADAR_NONCE_LEN`, `RADAR_SALT_LEN`, `RADAR_TAG_LEN`, `RADAR_FRAME_MAX` and every `RADAR_TYPE_*`
unchanged.

Update the `radar_wire_seal` doc comment (line 41-42) to describe the new layout:

```c
// Build [nonce|ct|tag] into frame, where ct = E(type || len(2 LE) || payload || padding).
// The plaintext is padded to a bucket (see radar_pad.h) so frame length does not classify traffic.
// Returns 0 on success, <0 on error; *frame_len set to total bytes.
```

- [ ] **Step 2: Rewrite `radar_wire_seal`**

Replace `components/simulacra_radar/radar_wire.c:20-40`:

```c
int radar_wire_seal(uint8_t *frame, size_t *frame_len, uint8_t type,
                    const uint8_t *payload, size_t payload_len,
                    const uint8_t key[32], const uint8_t salt[RADAR_SALT_LEN], uint64_t counter)
{
    size_t ptlen = radar_pad_plaintext_len(payload_len);
    if (ptlen == 0) return -1;                       // payload too large for any bucket

    // Plaintext: [type][len LE][payload][random padding]. The padding is random rather than zeros
    // so it is indistinguishable from payload in the ciphertext; GCM would hide a zero run anyway,
    // but random costs nothing and removes the question.
    uint8_t pt[RADAR_FRAME_MAX];
    pt[0] = type;
    pt[1] = (uint8_t)(payload_len & 0xFF);
    pt[2] = (uint8_t)((payload_len >> 8) & 0xFF);
    if (payload_len) memcpy(pt + RADAR_PAD_HDR, payload, payload_len);
    size_t used = RADAR_PAD_HDR + payload_len;
    if (ptlen > used) esp_fill_random_compat(pt + used, ptlen - used);

    uint8_t nonce[12]; make_nonce(nonce, salt, counter);
    memcpy(frame, nonce, RADAR_NONCE_LEN);
    uint8_t *ct  = frame + RADAR_NONCE_LEN;
    uint8_t *tag = ct + ptlen;

    mbedtls_gcm_context g; mbedtls_gcm_init(&g);
    int rc = mbedtls_gcm_setkey(&g, MBEDTLS_CIPHER_ID_AES, key, 256);
    // No AAD: there is no plaintext header left to authenticate. `type` is covered by the tag as
    // ciphertext, which is strictly stronger than its old AAD treatment.
    if (rc == 0) rc = mbedtls_gcm_crypt_and_tag(&g, MBEDTLS_GCM_ENCRYPT, ptlen,
                          nonce, RADAR_NONCE_LEN, NULL, 0,
                          pt, ct, RADAR_TAG_LEN, tag);
    mbedtls_gcm_free(&g);
    if (rc != 0) return -2;
    *frame_len = RADAR_NONCE_LEN + ptlen + RADAR_TAG_LEN;
    return 0;
}
```

`esp_fill_random_compat` does not exist yet - Step 3 adds it. It is needed because `radar_wire.c`
is compiled both for the firmware (where `esp_random.h` is available) and, in principle, for host
tools; the existing file includes neither, so introduce the shim rather than an unguarded
ESP-IDF include.

- [ ] **Step 3: Add the randomness shim**

At the top of `components/simulacra_radar/radar_wire.c`, after the existing includes:

```c
#include "radar_pad.h"

// Padding randomness. radar_wire.c is component code that may be compiled outside ESP-IDF, so the
// ESP-IDF RNG is reached through a shim rather than an unguarded include.
#if defined(ESP_PLATFORM)
#include "esp_random.h"
static inline void esp_fill_random_compat(void *buf, size_t n) { esp_fill_random(buf, n); }
#else
#include <stdlib.h>
static inline void esp_fill_random_compat(void *buf, size_t n)
{ uint8_t *p = (uint8_t*)buf; for (size_t i = 0; i < n; i++) p[i] = (uint8_t)(rand() & 0xFF); }
#endif
```

- [ ] **Step 4: Rewrite `radar_wire_open`**

Replace `components/simulacra_radar/radar_wire.c:42-69`:

```c
int radar_wire_open(const uint8_t *frame, size_t frame_len, const uint8_t key[32],
                    uint8_t *out_type, uint8_t *payload, size_t payload_cap, size_t *payload_len,
                    uint8_t out_salt[RADAR_SALT_LEN], uint64_t *out_counter)
{
    if (frame_len < RADAR_NONCE_LEN + RADAR_PAD_HDR + RADAR_TAG_LEN) return -1;
    if (frame_len > RADAR_FRAME_MAX) return -1;      // pre-decrypt bound: see below
    const uint8_t *nonce = frame;
    size_t ptlen = frame_len - RADAR_NONCE_LEN - RADAR_TAG_LEN;
    const uint8_t *ct  = nonce + RADAR_NONCE_LEN;
    const uint8_t *tag = ct + ptlen;

    // Decrypt into a LOCAL buffer, never the caller's. mbedtls writes plaintext before it compares
    // the tag (it zeroises on mismatch, but the write already happened), so decrypting straight
    // into the caller's buffer let an oversized frame overflow it pre-auth, from anyone in RF
    // range -- the SEC-2 finding. Bounding frame_len by RADAR_FRAME_MAX above makes this local
    // buffer sufficient by construction, and the caller's buffer is not touched until the tag has
    // verified AND the recovered length has been checked against payload_cap.
    uint8_t pt[RADAR_FRAME_MAX];

    mbedtls_gcm_context g; mbedtls_gcm_init(&g);
    int rc = mbedtls_gcm_setkey(&g, MBEDTLS_CIPHER_ID_AES, key, 256);
    if (rc == 0) rc = mbedtls_gcm_auth_decrypt(&g, ptlen, nonce, RADAR_NONCE_LEN,
                          NULL, 0, tag, RADAR_TAG_LEN, ct, pt);
    mbedtls_gcm_free(&g);
    if (rc != 0) return -2;                          // bad tag / bad key / not ours

    size_t plen = (size_t)pt[1] | ((size_t)pt[2] << 8);
    if (RADAR_PAD_HDR + plen > ptlen) return -1;     // length field disagrees with the frame
    if (plen > payload_cap) return -1;               // caller's buffer is too small

    *out_type = pt[0];
    *payload_len = plen;
    if (plen) memcpy(payload, pt + RADAR_PAD_HDR, plen);
    memcpy(out_salt, nonce, RADAR_SALT_LEN);
    uint64_t c = 0; for (int i = 0; i < 4; i++) c = (c << 8) | nonce[RADAR_SALT_LEN + i];
    *out_counter = c;
    return 0;
}
```

- [ ] **Step 5: Fix the CONFIG wire comment**

`components/simulacra_radar/config_wire.h:24` says "the SAME nonce radar_wire_seal will use (wire
v3;...)". Update the version reference to v4. The nonce layout itself is unchanged, so no code
changes - only the comment's version number.

- [ ] **Step 6: Update the on-target wire tests**

In `main/churn_selftest.c`, find the `radar_wire` block near line 1091. Replace the existing
layout assertions with:

```c
    // Wire v4: nothing identifying in the clear. A frame is [nonce(12)][ct][tag(16)] and `type`
    // lives in the first plaintext byte. Pin the layout so a future edit cannot silently
    // reintroduce a plaintext discriminator.
    ST_CHECK(RADAR_WIRE_VER == 4, "wire version is 4");
    ST_CHECK(flen == radar_pad_plaintext_len(sizeof st) + RADAR_NONCE_LEN + RADAR_TAG_LEN,
             "sealed frame is exactly one bucket");
    {   // every bucket round-trips, and the recovered payload matches byte for byte
        static const size_t SIZES[] = { 0, 1, 4, 33, 34, 67, 97, 98, 174, 219 };
        for (size_t i = 0; i < sizeof SIZES / sizeof SIZES[0]; i++) {
            uint8_t src[219], got[219]; size_t n = SIZES[i], glen; uint8_t gt;
            for (size_t j = 0; j < n; j++) src[j] = (uint8_t)(j * 7 + i);
            uint8_t f[RADAR_FRAME_MAX]; size_t fl;
            uint8_t s8[RADAR_SALT_LEN]; for (int j = 0; j < RADAR_SALT_LEN; j++) s8[j] = (uint8_t)j;
            ST_CHECK(radar_wire_seal(f, &fl, RADAR_TYPE_STATUS, src, n,
                                     SIMULACRA_ESPNOW_KEY, s8, 9) == 0, "bucket seal ok");
            ST_CHECK(fl == radar_pad_plaintext_len(n) + RADAR_NONCE_LEN + RADAR_TAG_LEN,
                     "sealed length is the bucket");
            uint8_t os[RADAR_SALT_LEN]; uint64_t oc;
            ST_CHECK(radar_wire_open(f, fl, SIMULACRA_ESPNOW_KEY, &gt, got, sizeof got,
                                     &glen, os, &oc) == 0, "bucket open ok");
            ST_CHECK(gt == RADAR_TYPE_STATUS && glen == n, "type and length recovered");
            ST_CHECK(n == 0 || memcmp(src, got, n) == 0, "payload survives padding");
        }
    }
    {   // A payload larger than the biggest bucket must be refused at seal time, not truncated.
        uint8_t big[240]; size_t fl; uint8_t f[RADAR_FRAME_MAX];
        uint8_t s8[RADAR_SALT_LEN]; for (int j = 0; j < RADAR_SALT_LEN; j++) s8[j] = (uint8_t)j;
        ST_CHECK(radar_wire_seal(f, &fl, RADAR_TYPE_STATUS, big, sizeof big,
                                 SIMULACRA_ESPNOW_KEY, s8, 10) < 0, "oversized payload refused");
    }
```

Add `#include "radar_pad.h"` to `churn_selftest.c` if not already present.

The existing SEC-2 assertions at lines 1119-1128 (small `payload_cap` must be refused, corrupted
frame must fail) stay as they are - their contract is unchanged and they must still pass.

- [ ] **Step 7: Build all three targets, each in its own shell**

```
# C5 (IDF 5.5)
idf.py set-target esp32c5 && idf.py -DSIMULACRA_ESPNOW=1 -DSIMULACRA_CONFIG_CTRL=1 build
# C6 (IDF 5.4) -- fresh shell
idf.py set-target esp32c6 && idf.py -DSIMULACRA_ESPNOW=1 -DSIMULACRA_CONFIG_CTRL=1 build
# CYD (IDF 5.4) -- fresh shell
cd cyd && idf.py set-target esp32 && idf.py -DSIMULACRA_CONFIG_CTRL=1 build
```
Expected: `Project build complete`, warning-clean, for each. Any `RADAR_MAGIC0` / `RADAR_HDR_LEN`
compile error is the deletion doing its job - fix the consumer, which for `espnow_sniff.c` is
Task 5.

- [ ] **Step 8: Run the on-target self-test**

```
idf.py set-target esp32c5
idf.py -DCHURN_SELFTEST=1 -DSIMULACRA_ESPNOW=1 -DSIMULACRA_CONFIG_CTRL=1 build
idf.py -p <C5_PORT> flash
```
Read serial and expect `SELFTEST: PASS (N/N), fails=0`.

Afterwards, `rm -rf build sdkconfig` and rebuild without `CHURN_SELFTEST` before flashing a decoy -
`-D` flags only ever ADD to the CMake cache, so the selftest define lingers otherwise.

- [ ] **Step 9: Commit**

```bash
git add components/simulacra_radar/radar_wire.c components/simulacra_radar/radar_wire.h \
        components/simulacra_radar/config_wire.h main/churn_selftest.c
git commit -m "wire: v4 removes the plaintext header and buckets frame length"
```

---

### Task 3: Spread retransmits in time

**Files:**
- Create: `components/simulacra_radar/radar_retx.c`, `components/simulacra_radar/radar_retx.h`
- Modify: `tools/decoy_audit/run.ps1`, `tools/decoy_audit/Makefile`, `tools/decoy_audit/synth_dump.c`
- Test: `tools/decoy_audit/tests/test_radar_retx.py` (create)

**Interfaces:**
- Consumes: nothing from earlier tasks.
- Produces:
  - `typedef struct { uint8_t frame[RADAR_FRAME_MAX]; size_t len; uint8_t left; uint32_t next_ms; } radar_retx_t;`
  - `void radar_retx_arm(radar_retx_t *r, const uint8_t *frame, size_t len, uint8_t repeats, uint32_t now_ms, uint32_t jitter)`
  - `bool radar_retx_due(radar_retx_t *r, uint32_t now_ms, uint32_t jitter)` - true when the caller should send `r->frame`; decrements `left` and schedules the next.

The caller owns transmission; this module owns only scheduling, which keeps it pure and testable.

- [ ] **Step 1: Write the failing test**

Create `tools/decoy_audit/tests/test_radar_retx.py`:

```python
import os, subprocess, unittest

HERE = os.path.dirname(os.path.abspath(__file__)); TOOL = os.path.dirname(HERE)
EXE  = os.path.join(TOOL, "synth_dump.exe" if os.name == "nt" else "synth_dump")


def fire_times(repeats, seed, horizon_ms=5000):
    """Returns the ms timestamps at which radar_retx_due fired, ticking 1 ms at a time."""
    out = subprocess.check_output(
        [EXE, "--retx", str(repeats), str(seed), str(horizon_ms)], text=True)
    return [int(x) for x in out.split()]


@unittest.skipUnless(os.path.exists(EXE), "synth_dump not built")
class RadarRetx(unittest.TestCase):
    def test_fires_exactly_the_requested_number_of_times(self):
        for n in (1, 2, 3, 4):
            self.assertEqual(len(fire_times(n, 1)), n, f"expected {n} sends")

    def test_first_send_is_immediate(self):
        self.assertEqual(fire_times(4, 1)[0], 0, "first repeat should go out at once")

    def test_repeats_are_spread_not_back_to_back(self):
        # The whole point: v3 sent 4 identical frames inside ~20 ms, a recognizable retransmit
        # train. Every gap must be at least 40 ms.
        for seed in range(1, 6):
            t = fire_times(4, seed)
            gaps = [b - a for a, b in zip(t, t[1:])]
            self.assertTrue(all(g >= 40 for g in gaps), f"seed {seed}: gaps {gaps}")
            self.assertTrue(all(g <= 120 for g in gaps), f"seed {seed}: gaps {gaps}")

    def test_gaps_are_jittered_not_constant(self):
        # A fixed 80 ms spacing would just be a slower metronome.
        seen = set()
        for seed in range(1, 12):
            t = fire_times(4, seed)
            seen.update(b - a for a, b in zip(t, t[1:]))
        self.assertGreater(len(seen), 3, f"gaps barely varied: {sorted(seen)}")

    def test_zero_repeats_never_fires(self):
        self.assertEqual(fire_times(0, 1), [])
```

- [ ] **Step 2: Run it to verify it fails**

```
cd tools/decoy_audit && python -m unittest tests.test_radar_retx -v
```
Expected: all fail - `--retx` is not a known mode.

- [ ] **Step 3: Create the header**

`components/simulacra_radar/radar_retx.h`:

```c
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "radar_wire.h"        // RADAR_FRAME_MAX

// Spread retransmit scheduler for the ESP-NOW link.
//
// Broadcast ESP-NOW is unacknowledged, so every logical message is sent more than once. v3 sent
// those repeats back-to-back inside ~20 ms, which put a burst of identical-length frames from one
// MAC on air -- a recognizable retransmit signature that survives encryption, length bucketing and
// MAC randomization alike. Spacing the repeats by a jittered 40-120 ms makes them read as unrelated
// frames instead of an obvious train.
//
// This module owns SCHEDULING ONLY; the caller transmits. That keeps it free of ESP-NOW and
// therefore host-testable (synth_dump --retx).
//
// Repeats are deliberately byte-identical. Re-sealing each one with a fresh counter would avoid
// identical bytes, but it would also advance the CONFIG monotonic replay floor once per repeat and
// burn counter space for no security gain.

#define RADAR_RETX_MIN_GAP_MS 40
#define RADAR_RETX_MAX_GAP_MS 120

typedef struct {
    uint8_t  frame[RADAR_FRAME_MAX];
    size_t   len;
    uint8_t  left;            // sends still owed (0 = idle)
    uint32_t next_ms;
} radar_retx_t;

// Arm with a sealed frame. The first send is due immediately. `jitter` is a fresh random value
// from the caller (esp_random() on target) so this stays pure.
void radar_retx_arm(radar_retx_t *r, const uint8_t *frame, size_t len,
                    uint8_t repeats, uint32_t now_ms, uint32_t jitter);

// True when the caller should transmit r->frame now. Decrements the owed count and schedules the
// next send. `jitter` is a fresh random value, used only when another send remains.
bool radar_retx_due(radar_retx_t *r, uint32_t now_ms, uint32_t jitter);
```

- [ ] **Step 4: Create the implementation**

`components/simulacra_radar/radar_retx.c`:

```c
#include "radar_retx.h"
#include <string.h>

#define GAP_SPAN (RADAR_RETX_MAX_GAP_MS - RADAR_RETX_MIN_GAP_MS + 1)

void radar_retx_arm(radar_retx_t *r, const uint8_t *frame, size_t len,
                    uint8_t repeats, uint32_t now_ms, uint32_t jitter)
{
    (void)jitter;
    if (len > RADAR_FRAME_MAX) { r->left = 0; return; }
    memcpy(r->frame, frame, len);
    r->len = len;
    r->left = repeats;
    r->next_ms = now_ms;                       // first send is immediate
}

bool radar_retx_due(radar_retx_t *r, uint32_t now_ms, uint32_t jitter)
{
    if (r->left == 0) return false;
    if ((int32_t)(now_ms - r->next_ms) < 0) return false;
    r->left--;
    if (r->left) r->next_ms = now_ms + RADAR_RETX_MIN_GAP_MS + (jitter % GAP_SPAN);
    return true;
}
```

- [ ] **Step 5: Add the harness mode**

In `tools/decoy_audit/synth_dump.c`, add `#include "radar_retx.h"`, then add this block before the
`--padbucket` block from Task 1:

```c
    if (argc > 1 && strcmp(argv[1], "--retx") == 0) {
        int reps    = argc > 2 ? (int)strtol(argv[2], 0, 10) : 4;
        unsigned sd = argc > 3 ? (unsigned)strtoul(argv[3], 0, 10) : 1;
        uint32_t hz = argc > 4 ? (uint32_t)strtoul(argv[4], 0, 10) : 5000;
        srand(sd);
        radar_retx_t r; uint8_t f[8] = {0};
        radar_retx_arm(&r, f, sizeof f, (uint8_t)reps, 0, (uint32_t)rand());
        for (uint32_t t = 0; t <= hz; t++)
            if (radar_retx_due(&r, t, (uint32_t)rand())) printf("%u\n", (unsigned)t);
        return 0;
    }
```

- [ ] **Step 6: Add the source to BOTH build files**

`tools/decoy_audit/Makefile`, append to `SRC :=`:
```
$(ROOT)/components/simulacra_radar/radar_retx.c
```
`tools/decoy_audit/run.ps1`, add to the `cl` argument line:
```
..\..\components\simulacra_radar\radar_retx.c
```

- [ ] **Step 7: Rebuild and run**

```
cd tools/decoy_audit && make && python -m unittest tests.test_radar_retx -v
```
Expected: 5 passed.

- [ ] **Step 8: Commit**

```bash
git add components/simulacra_radar/radar_retx.c components/simulacra_radar/radar_retx.h \
        tools/decoy_audit/synth_dump.c tools/decoy_audit/Makefile tools/decoy_audit/run.ps1 \
        tools/decoy_audit/tests/test_radar_retx.py
git commit -m "radar_retx: jittered retransmit scheduler"
```

---

### Task 4: Wire the scheduler into the decoy and the Vigil

**Files:**
- Modify: `main/esp_now_link.c` (`respond_once`, `espnow_task`)
- Modify: `cyd/main/cyd_main.c` (`send_request`, `send_config`, main loop)

**Interfaces:**
- Consumes: `radar_retx_t`, `radar_retx_arm`, `radar_retx_due` from Task 3.
- Produces: no new symbols.

- [ ] **Step 1: Decoy - arm instead of blasting**

In `main/esp_now_link.c`, add `#include "radar_retx.h"` and a file-scope scheduler:

```c
static radar_retx_t s_status_retx;
```

Find `respond_once` (around line 404) and replace its `for (int i = 0; i < 3; i++) esp_now_send(...)`
send loop with an arm:

```c
    // 2x, down from 3x, and spread rather than back-to-back. A decoy has no delivery feedback
    // (broadcast is unacknowledged and it never hears the Vigil's view), so the count is fixed
    // rather than adaptive -- the Vigil's own re-request covers a lost STATUS within one cycle.
    uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
    radar_retx_arm(&s_status_retx, frame, flen, 2, now, esp_random());
```

- [ ] **Step 2: Decoy - drain the scheduler on the task**

In `espnow_task`'s `for(;;)` body, immediately after the existing `espnow_drain_rx();` call, add:

```c
        {   // spread STATUS repeats; the task already ticks every 50 ms
            uint32_t rnow = (uint32_t)(esp_timer_get_time() / 1000);
            if (radar_retx_due(&s_status_retx, rnow, esp_random()))
                esp_now_send(BCAST, s_status_retx.frame, s_status_retx.len);
        }
```

The task's existing `vTaskDelay(pdMS_TO_TICKS(50))` gives 50 ms resolution, comfortably finer than
the 40-120 ms gaps.

- [ ] **Step 3: Vigil - arm REQUEST and CONFIG**

In `cyd/main/cyd_main.c`, add `#include "radar_retx.h"` and two file-scope schedulers:

```c
static radar_retx_t s_req_retx;
static radar_retx_t s_cfg_retx;
static uint8_t      s_req_repeats = 4;      // adaptive; see Task 5
```

In `send_request` (line ~548), replace `for (int i=0;i<4;i++) esp_now_send(BCAST,frame,flen);` with:

```c
        radar_retx_arm(&s_req_retx, frame, flen, s_req_repeats,
                       (uint32_t)(esp_timer_get_time()/1000), esp_random());
```

In `send_config` (line ~586), replace `for (int i = 0; i < 4; i++) esp_now_send(BCAST, frame, flen);`
with:

```c
    // CONFIG stays at an unconditional 4x. It is rare and operator-initiated, and a silently
    // dropped command means the console reports a preset the fleet never applied -- the same
    // "display lies about the firmware" failure class as DRIFT-1. Reliability wins here.
    radar_retx_arm(&s_cfg_retx, frame, flen, 4,
                   (uint32_t)(esp_timer_get_time()/1000), esp_random());
```

- [ ] **Step 4: Vigil - drain both schedulers in the main loop**

In the main render/poll loop, immediately after the existing `drain_rx();` call, add:

```c
        {   // spread REQUEST/CONFIG repeats rather than bursting them back-to-back
            uint32_t rnow = (uint32_t)(esp_timer_get_time()/1000);
            if (radar_retx_due(&s_req_retx, rnow, esp_random()))
                esp_now_send(BCAST, s_req_retx.frame, s_req_retx.len);
            if (radar_retx_due(&s_cfg_retx, rnow, esp_random()))
                esp_now_send(BCAST, s_cfg_retx.frame, s_cfg_retx.len);
        }
```

- [ ] **Step 5: Build all three targets, each in its own shell**

Same commands as Task 2 Step 7. Expected: all three `Project build complete`, warning-clean.

- [ ] **Step 6: Commit**

```bash
git add main/esp_now_link.c cyd/main/cyd_main.c
git commit -m "link: spread retransmits instead of bursting them back-to-back"
```

---

### Task 5: Adaptive REQUEST repeat count

**Files:**
- Modify: `cyd/main/cyd_main.c` (main loop; the `s_req_repeats` declared in Task 4)
- Modify: `components/simulacra_radar/radar_render.c` (INFO page line)
- Test: `tools/decoy_audit/tests/test_retx_adapt.py` (create), `tools/decoy_audit/synth_dump.c`

**Interfaces:**
- Consumes: `s_req_repeats` from Task 4, `fleet_alive_count()` (already in `cyd_main.c`).
- Produces: `uint8_t radar_retx_adapt(uint8_t cur, bool all_answered)` in `radar_retx.{c,h}`.

- [ ] **Step 1: Write the failing test**

Create `tools/decoy_audit/tests/test_retx_adapt.py`:

```python
import os, subprocess, unittest

HERE = os.path.dirname(os.path.abspath(__file__)); TOOL = os.path.dirname(HERE)
EXE  = os.path.join(TOOL, "synth_dump.exe" if os.name == "nt" else "synth_dump")


def adapt(start, sequence):
    """sequence: string of '1' (all answered) / '0' (someone missed)."""
    out = subprocess.check_output([EXE, "--retxadapt", str(start), sequence], text=True)
    return [int(x) for x in out.split()]


@unittest.skipUnless(os.path.exists(EXE), "synth_dump not built")
class RetxAdapt(unittest.TestCase):
    def test_relaxes_one_step_per_clean_cycle(self):
        self.assertEqual(adapt(4, "111"), [3, 2, 1])

    def test_never_relaxes_below_one(self):
        self.assertEqual(adapt(4, "111111"), [3, 2, 1, 1, 1, 1])

    def test_any_miss_resets_to_max_immediately(self):
        # Asymmetric on purpose: slow to relax, immediate to recover. An unheard REQUEST costs a
        # stale console; an extra frame costs only exposure.
        self.assertEqual(adapt(4, "1110"), [3, 2, 1, 4])

    def test_recovers_from_the_floor(self):
        self.assertEqual(adapt(1, "0"), [4])
```

- [ ] **Step 2: Run it to verify it fails**

```
cd tools/decoy_audit && python -m unittest tests.test_retx_adapt -v
```
Expected: all fail - `--retxadapt` is not a known mode.

- [ ] **Step 3: Add the pure adapt function**

In `components/simulacra_radar/radar_retx.h`, after `radar_retx_due`:

```c
#define RADAR_RETX_MAX_REPEATS 4
#define RADAR_RETX_MIN_REPEATS 1

// Adapt the repeat count from observed delivery. Relaxes one step per fully-delivered cycle,
// resets to maximum on any miss. Deliberately asymmetric: an unheard REQUEST costs a stale
// console, an extra frame costs only exposure.
uint8_t radar_retx_adapt(uint8_t cur, bool all_answered);
```

In `components/simulacra_radar/radar_retx.c`:

```c
uint8_t radar_retx_adapt(uint8_t cur, bool all_answered)
{
    if (!all_answered) return RADAR_RETX_MAX_REPEATS;
    return cur > RADAR_RETX_MIN_REPEATS ? (uint8_t)(cur - 1) : RADAR_RETX_MIN_REPEATS;
}
```

- [ ] **Step 4: Add the harness mode**

In `tools/decoy_audit/synth_dump.c`, before the `--retx` block:

```c
    if (argc > 1 && strcmp(argv[1], "--retxadapt") == 0) {
        uint8_t cur = argc > 2 ? (uint8_t)strtoul(argv[2], 0, 10) : 4;
        const char *seq = argc > 3 ? argv[3] : "";
        for (const char *p = seq; *p; p++) {
            cur = radar_retx_adapt(cur, *p == '1');
            printf("%u\n", (unsigned)cur);
        }
        return 0;
    }
```

- [ ] **Step 5: Run the tests**

```
cd tools/decoy_audit && make && python -m unittest tests.test_retx_adapt -v
```
Expected: 4 passed.

- [ ] **Step 6: Wire it into the Vigil's poll cycle**

In `cyd/main/cyd_main.c`'s main loop, at the point where the next REQUEST is about to be sent
(the jittered `now - last_req > req_period` block added on 2026-08-24), evaluate the previous
cycle's delivery before sending the new one:

```c
        if (ui.backlight_on && !espnow_suspended && now - last_req > req_period) {
            // Judge the cycle that just ended: did every node the roster believes is alive answer?
            // fleet_alive_count() counts nodes whose last status is inside the stale window, so a
            // node that answered this cycle is still counted; one that went quiet is not.
            static int s_prev_alive = -1;
            int alive = fleet_alive_count(now);
            if (s_prev_alive >= 0)
                s_req_repeats = radar_retx_adapt(s_req_repeats, alive >= s_prev_alive);
            s_prev_alive = alive;
            send_request(); last_req = now;
            req_period = 1000 + (esp_random() % 601);
        }
```

- [ ] **Step 7: Surface the level on the INFO page**

The spec requires this: an adaptive count that silently absorbs a degrading link would hide exactly
the symptom an operator needs. In `components/simulacra_radar/radar_render.c`'s INFO page draw
function, add a row using the existing `row_kv` helper and the surrounding call style:

```c
    char rtx[16]; snprintf(rtx, sizeof rtx, "%ux", (unsigned)info->req_repeats);
    row_kv(g, y, "link retry", rtx); y += 16;
```

Add `uint8_t req_repeats;` to whichever info struct that page already renders from (read the
struct definition in `radar_ui.h` / `radar_render.h` and follow the existing field pattern), and
populate it from `s_req_repeats` where `cyd_main.c` fills that struct.

- [ ] **Step 8: Build all three targets and run the radar_audit suite**

```
cd tools/radar_audit && make && python -m unittest discover -s tests -p "test_*.py"
```
Expected: all pass. If an INFO-page test asserts an exact row count or layout, update it to include
the new row and note the date and reason in a comment.

Then build all three firmware targets, each in its own shell (Task 2 Step 7 commands).

- [ ] **Step 9: Commit**

```bash
git add components/simulacra_radar/radar_retx.c components/simulacra_radar/radar_retx.h \
        components/simulacra_radar/radar_render.c cyd/main/cyd_main.c \
        tools/decoy_audit/synth_dump.c tools/decoy_audit/tests/test_retx_adapt.py \
        tools/radar_audit/tests/
git commit -m "link: adapt REQUEST redundancy to observed delivery"
```

---

### Task 6: Rework espnow_sniff to use the key

v4 deliberately breaks this tool's unkeyed frame matching. That is the point of the change and the
acceptance test for it; the tool is rebuilt around the key so it stays useful as an opsec verifier.

**Files:**
- Modify: `main/espnow_sniff.c`

**Interfaces:**
- Consumes: `radar_wire_open` (Task 2).
- Produces: no new symbols.

- [ ] **Step 1: Replace magic matching with a decrypt attempt**

`main/espnow_sniff.c:36` currently reads:

```c
    if (ef[0] != RADAR_MAGIC0 || ef[1] != RADAR_MAGIC1) return;            // not our link
```

Those constants no longer exist, so the file will not compile until this changes. Replace the magic
check with an authenticated open, and take `type` from the decrypted result rather than `ef[3]`:

```c
    // v4 removed the plaintext header, so there is no magic to match and no type byte in the clear.
    // That is exactly the property this tool exists to verify -- an adversary CANNOT do what the
    // next line does, because it requires the fleet key.
    uint8_t type, pl[RADAR_FRAME_MAX], salt[RADAR_SALT_LEN]; size_t plen; uint64_t ctr;
    if (radar_wire_open(f + PAYLOAD_OFF, (size_t)payload_len, SIMULACRA_ESPNOW_KEY,
                        &type, pl, sizeof pl, &plen, salt, &ctr) != 0)
        return;                                                            // not our link
```

Read the surrounding code for the exact names of the frame pointer and payload offset already in
scope (`ef`, `f`, `SRC_OFF` exist today) and use those rather than the placeholders above.

Then replace each `ef[3] == RADAR_TYPE_*` comparison with `type == RADAR_TYPE_*`.

- [ ] **Step 2: Update the file's header comment**

`main/espnow_sniff.h:3-7` describes decoding "WITHOUT the key". Replace with:

```c
// Opsec verifier (SIMULACRA_ESPNOW_SNIFF). Parks a spare board on channel 1 in promiscuous mode
// and decodes the radar ESP-NOW link's frames USING the fleet key, logging each REQUEST/STATUS
// with its 802.11 source MAC (+ whether it is locally-administered) plus running counts, so
// "decoy stays silent until the CYD asks" and source-MAC hygiene stay observable.
//
// It used to work WITHOUT the key by matching a plaintext magic. Wire v4 removed that header
// deliberately -- a passive adversary can no longer identify these frames as ours at all, which is
// the whole point of v4. This tool now needs the key, which is the correct asymmetry.
// Wi-Fi-only; NimBLE never starts. Flash to e.g. the SparkFun C6.
```

- [ ] **Step 3: Build the sniffer**

```
idf.py set-target esp32c6 && idf.py -DSIMULACRA_ESPNOW_SNIFF=1 build
```
Expected: `Project build complete`.

- [ ] **Step 4: Commit**

```bash
git add main/espnow_sniff.c main/espnow_sniff.h
git commit -m "espnow_sniff: decode with the key, since v4 removes the plaintext magic"
```

---

### Task 7: Hardware verification

**Files:** none - bench work. Record results in `private/PROJECT-MAP.md` §11.

- [ ] **Step 1: Flash the entire fleet together**

Wire v4 is breaking in both directions; a partial fleet silently splits into two non-communicating
halves. Every board, same session, baked regime:

```
build_flash_read.ps1 -Target c5  -Fleet -Port <C5_A> -Do all
build_flash_read.ps1 -Target c5  -Fleet -Port <C5_B> -Do flash
build_flash_read.ps1 -Target c6  -Fleet -Port <C6>   -Do buildflash    # fresh shell
build_flash_read.ps1 -Target cyd -Fleet -Port <CYD>  -Do all           # fresh shell
```

- [ ] **Step 2: Confirm the fleet still talks**

Read the CYD serial and expect `status rx: N0/N1/N2` lines from every decoy with no dropped frames.
A silent node means a board missed the v4 flash.

- [ ] **Step 3: THE ACCEPTANCE TEST - sniff without the key**

Flash a spare board with `-DSIMULACRA_ESPNOW_SNIFF=1` but with the **decrypt line stubbed out** (or
a deliberately wrong key), so it behaves as an adversary would. Read its serial for 60 s while the
CYD screen is awake.

Expected: **zero frames identified.** Under v3 the same test printed `REQ`/`STAT` lines with source
MACs and a running count. If it still identifies frames, v4 has failed its purpose - stop and
investigate before proceeding.

- [ ] **Step 4: Confirm the tool still works WITH the key**

Reflash the same board with the real `SIMULACRA_ESPNOW_SNIFF=1` build from Task 6. Read for 60 s
with the CYD awake.

Expected: `REQ`/`STAT` lines again, all source MACs showing `[LAA]`, `factory=0`. This proves the
frames were there all along and the difference in Step 3 is purely the adversary's inability to
classify them.

- [ ] **Step 5: Confirm redundancy adapts**

Read the CYD serial over a few minutes of steady operation with all nodes answering, and check the
INFO page's `link retry` row (Task 5 Step 7) falls from `4x` toward `1x`. Then power a decoy down
and confirm it resets to `4x` within one poll cycle.

- [ ] **Step 6: Restore the sniffer board to a decoy**

```
rm -rf build sdkconfig      # -D flags only ADD to the CMake cache
idf.py set-target esp32c6
idf.py -DSIMULACRA_ESPNOW=1 -DSIMULACRA_CONFIG_CTRL=1 build
idf.py -p <C6> flash
```
Confirm the CYD shows a full fleet again.

- [ ] **Step 7: Record the results**

Update `private/PROJECT-MAP.md` §11 with the acceptance-test outcome. `private/` is gitignored, so
there is nothing to commit for the record itself.

---

## Self-Review

**Spec coverage:** Wire v4 (no header, type in ciphertext, empty AAD) -> Task 2. Length bucketing ->
Tasks 1, 2. Spread repeats -> Tasks 3, 4. Adaptive count with the asymmetric reset rule -> Task 5.
CONFIG stays unconditional 4x -> Task 4 Step 3. STATUS drops to fixed 2x -> Task 4 Step 1. SEC-2
guard preserved and strengthened -> Task 2 Step 4. `espnow_sniff` rework -> Task 6. Acceptance test
(unkeyed sniffer must fail, keyed must succeed) -> Task 7 Steps 3-4. Repeat level surfaced on INFO
so adaptation cannot silently mask a degrading link -> Task 5 Step 7. Migration / whole-fleet flash
-> Global Constraints and Task 7 Step 1.

**Type consistency:** `radar_pad_plaintext_len` and `RADAR_PAD_HDR` are defined in Task 1 and used
in Task 2. `radar_retx_t`, `radar_retx_arm`, `radar_retx_due` are defined in Task 3 and used in
Task 4. `radar_retx_adapt` is defined in Task 5 and used in the same task. `s_req_repeats` is
declared in Task 4 Step 3 and consumed in Task 5 Steps 6-7.

**Known soft spots**, flagged rather than hidden:
- Task 5 Step 7 does not name the exact INFO-page struct or draw function; both need reading first.
  The field name `req_repeats` is chosen here, so use exactly that when adding it.
- Task 6 Step 1 uses placeholder names for the frame pointer and payload offset in
  `espnow_sniff.c`; the real ones (`ef`, `f`, `SRC_OFF` and the promiscuous frame layout) must be
  read from the file. The unkeyed-sniffer variant needed for Task 7 Step 3 is a throwaway build,
  not a committed configuration.
- Task 2 assumes `components/simulacra_radar/CMakeLists.txt` picks up new `.c` files automatically
  (`SRC_DIRS`). If it lists sources explicitly, `radar_pad.c` and `radar_retx.c` must be added
  there too - check before the first firmware build.
- The STATUS payload size (~174 B) is derived from an observed 206 B v3 frame, not measured
  directly. Task 2 Step 6's bucket assertion will catch it if that is wrong.

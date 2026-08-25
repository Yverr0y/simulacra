# Link signature reduction - design

**Date:** 2026-08-25
**Status:** design, not yet implemented
**Scope:** the ESP-NOW control link only. Decoy realism (BLE/Wi-Fi emissions) is out of scope.

## Problem

The fleet's own coordination link is a better fingerprint than the crowd it coordinates.

Every frame the link emits is structured `[magic(2) | ver | type][nonce(12)][ciphertext][tag(16)]`.
The payload is AES-256-GCM sealed and unreadable without the fleet key. **Everything else is not.**
A passive listener on channel 1 who cannot decrypt a single byte can still extract:

| Discriminator | Where | What it reveals |
|---|---|---|
| `0x5A 0x4D` magic | `radar_wire.h:6-7` | This is Simulacra specifically, not merely "some Espressif device" |
| `ver` byte | `radar_wire.c:25` | Firmware generation; a constant to match on |
| `type` byte | `radar_wire.c:25` | Traffic class: REQUEST(1) identifies the **controller**, STATUS(2) the **decoys**, CONFIG(7) marks **when commands are issued**, FLEET_MACS(6) the housekeeping cadence |
| **Frame length** | implicit | Classifies traffic even if `type` were hidden: REQUEST ~32 B, CONFIG ~95 B, STATUS ~206 B are cleanly separable clusters |
| **Repeat count** | `esp_now_link.c`, `cyd_main.c` | REQUEST and CONFIG are sent **4x**, STATUS **3x**, byte-identical, back-to-back within ~20 ms |

Combined, an observer gets a portable, cross-session, cross-location signature for the system, a node
count, a controller-vs-decoy split, and a timing channel showing when the operator issues commands.

This is the same razor the project already applied when rejecting inter-fleet meshing - *"if we can
find each other, so can the adversary"* - except the existing link has the property today.

Measured context: with 3 decoys, one poll cycle puts **13 frames** on air in a tight clump. The
2026-08-24 opsec pass (jittered periods, randomized Vigil MAC) removed the *timing* metronome and the
vendor OUI, but left every item in the table above untouched.

## Goals

- An unkeyed observer cannot distinguish this link's frames from arbitrary ESP-NOW traffic.
- An unkeyed observer cannot classify controller vs decoy, nor detect when commands are sent.
- Reduce the number of frames per logical message without reducing delivery reliability.

## Non-goals

- **Hiding that ESP-NOW is in use.** ESP-NOW is a vendor-specific 802.11 action frame; its envelope
  is Espressif's, not ours. Only a different transport (or a wired link) removes that, and a wired
  inter-board link is the intended long-term answer once the enclosure exists. This design reduces
  what the link leaks *given* that it is on the air.
- Changing the crypto construction. AES-256-GCM, the nonce layout, and the replay gates stay as they
  are; SEC-4's `salt(8)||counter(4)` split is unaffected.
- Defeating RF fingerprinting or traffic-volume analysis. An observer will still see *that* several
  co-located radios exchange periodic frames.

## Design

### 1. Wire v4: nothing in the clear but the nonce

```
v3 (now):  [magic|magic|ver|type] [nonce 12] [ct] [tag 16]
v4:                               [nonce 12] [ct] [tag 16]
                                              ^ ct = E(type || payload)
```

The 4-byte header is removed entirely. `type` moves to the **first byte of the plaintext**, so it is
encrypted and authenticated like any other payload byte. The nonce stays in the clear (it must; it
is an input to decryption) but it is `salt(8)||counter(4)` where salt is random per boot - it looks
like 12 random bytes and carries no project-identifying constant.

**Rejection without a magic byte.** Today the magic is a cheap pre-crypto filter. In v4 rejection
falls to the GCM tag: a frame that is not ours fails authentication and is dropped. Cost is one
AES-GCM attempt per received ESP-NOW frame. This is acceptable because the ESP-NOW receive callback
only fires for ESP-NOW frames addressed to broadcast or to this station, of which there are
essentially none in a normal environment, and because frames are already copied into an SPSC ring
and processed on the owning task rather than the Wi-Fi driver task (the SEC-6 fix).

**AAD becomes empty.** With no plaintext header there is nothing to authenticate as associated data.
The nonce is already bound into GCM. `type` is now covered by the tag as ciphertext, which is
strictly stronger than its current AAD treatment.

**The length pre-check must survive.** `radar_wire_open`'s `payload_cap` guard (the SEC-2 fix -
mbedtls writes plaintext before comparing the tag) still applies and still runs before decryption.
Its arithmetic changes only by the removed header length.

### 2. Length bucketing

Removing the `type` byte accomplishes little while REQUEST/CONFIG/STATUS remain ~32/95/206 bytes.
Frames are padded to a **small fixed set of size buckets** so length no longer maps to traffic class.

Buckets: **64, 128, 250 bytes** (post-seal, total frame). Every frame is padded up to the smallest
bucket that fits it. Padding is applied to the plaintext before sealing (so it is encrypted, not a
recognizable run of zeros) and stripped after opening using an explicit plaintext length field.

Plaintext layout becomes:

```
[type(1)][len(2, LE)][payload(len)][random padding to bucket]
```

`len` is required: without it the receiver cannot distinguish payload from padding. Two bytes covers
the 250-byte frame with room to spare.

**Cost, stated honestly.** A 32-byte REQUEST becomes 64 bytes; the poll cycle's airtime roughly
doubles for the small frames. At ~1 Hz with 250-byte frames already routine (STATUS is ~206 B), this
is a small absolute increase. Three buckets rather than one flat 250 is a deliberate compromise:
padding everything to 250 would be maximally private but would nearly 8x the REQUEST airtime, and
airtime is itself exposure.

**Residual leak, stated honestly.** Bucketing reduces classification, it does not eliminate it.
STATUS will consistently land in the 250 bucket and REQUEST in the 64 bucket, so an observer can
still separate "small frequent frames from one MAC" from "large frames from several MACs". Making
that fully uniform requires padding everything to one size. This design accepts the residual in
exchange for airtime, and the decision should be revisited if a capture shows the buckets are
trivially separable in practice.

### 3. Adaptive, spread redundancy

Two changes, independent of each other.

**(a) Spread the repeats.** Repeats currently go out back-to-back inside ~20 ms, producing a burst of
N identical-length frames from one MAC - a distinctive pattern in its own right, and one that
survives everything above. Repeats are instead spaced by a jittered **40-120 ms**, so they read as
unrelated frames rather than an obvious retransmit train.

**(b) Adapt the count to observed delivery.** The Vigil has ground truth: it knows how many decoys
are on its roster and how many answered the last REQUEST.

- Start at the current 4x.
- Every poll cycle where **all** live roster nodes answered, decrement the repeat count (floor 1).
- Any cycle where a live node failed to answer, reset to the maximum (4).

This is deliberately asymmetric - slow to relax, immediate to recover - because the cost of an
unheard REQUEST is a stale console, while the cost of an extra frame is only exposure.

Decoy-side STATUS keeps a **fixed 2x** (down from 3x). A decoy has no delivery feedback (ESP-NOW
broadcast is unacknowledged and it never hears the Vigil's view), so adapting it would be guesswork;
the Vigil's own re-request already covers a lost STATUS within one cycle.

CONFIG keeps **4x, unconditionally**. It is rare, operator-initiated, and the failure mode of a lost
CONFIG (the fleet silently does not apply a command the console shows as sent) is exactly the
"display lies about the firmware" class this project has been bitten by. Reliability wins over
exposure for a command that fires once per operator action.

**Repeats stay byte-identical.** Re-sealing each repeat with a fresh counter would avoid identical
bytes on air, but it would also advance the CONFIG monotonic replay floor once per repeat and burn
counter space for no security gain. Identical repeats are already indistinguishable from a normal
retransmit once lengths are bucketed and spacing is jittered.

## Migration

**Breaking.** `RADAR_WIRE_VER` 3 -> 4. A v3 node and a v4 node cannot exchange anything: v3 rejects a
v4 frame for a missing magic, and v4 fails to authenticate a v3 frame because the header it no longer
expects shifts every subsequent byte. The failure is clean (no frame is misinterpreted) but total.

**Flash the entire fleet together.** This is the second breaking wire change in two days; the
CONFIG_WIRE_VER 1 -> 2 bump from the additive-population work has the same requirement.

## Impact on our own tooling

`main/espnow_sniff.c` locates frames by matching `RADAR_MAGIC0/1` without the key. **v4 deliberately
breaks it** - that is the point of the change, and it doubles as the acceptance test:

> After v4, `espnow_sniff` must be unable to identify or classify fleet traffic without the key.

The tool is rewritten to take the fleet key and decrypt, so it remains useful as an opsec verifier
(confirming source-MAC hygiene and the "silent until asked" property), while losing exactly the
capability an adversary would have.

## Testing

- **`churn_selftest` (on-target)**: v4 byte layout pinned (no header, `type` recovered from
  plaintext, `len` honoured, padding stripped); a v3-shaped frame is rejected; every bucket
  round-trips; an oversized frame is still refused before decryption (the SEC-2 guard).
- **Host tests**: bucket selection is a pure function - assert every payload size maps to the
  smallest bucket that fits and that a bucket is never exceeded. Assert the adaptive repeat counter
  decrements on full delivery and resets on any miss.
- **On-air acceptance**: run `espnow_sniff` **without** the key against a live v4 fleet and confirm
  it can no longer identify frames as ours. Then run it **with** the key and confirm it still sees
  the traffic - proving the frames are still there and the difference is purely the adversary's
  inability to classify them.
- **Delivery regression**: confirm the Vigil's roster stays fully populated with no `SILENT` nodes
  after the repeat count relaxes to 1, over a session long enough to include re-profile ticks.

## Risks

**Losing the cheap pre-crypto filter.** Every stray ESP-NOW frame now costs an AES-GCM attempt. Low
risk at observed rates, but if a genuinely noisy ESP-NOW environment is ever encountered this becomes
a CPU cost on the owning task. Mitigation if needed: keep the existing length pre-check and add a
cheap rate limit on failed opens.

**Adaptive redundancy hiding a degrading link.** If the count relaxes to 1 and the RF environment
worsens, the first symptom is a node flapping to `SILENT`. The reset-to-max-on-any-miss rule is
designed to catch this within one cycle, but the console should surface the current repeat level on
the INFO page so a persistently elevated count is visible as a link-quality signal rather than being
silently absorbed.

**Bucketing is not uniformity.** See the residual leak noted in section 2. This design reduces the
length channel; it does not close it.

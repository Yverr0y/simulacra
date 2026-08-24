# Additive fleet population + AUTO/MANUAL modes - design

**Date:** 2026-08-24
**Status:** design approved, not yet implemented
**Supersedes:** the per-node `/K` population split introduced 2026-08-11 (the "H8" fix)

## Problem

Every decoy currently sizes its crowd as `1/K` of a fleet-wide target, where K is the live count of
ESP-NOW peers heard plus self. Adding a second board to a fleet therefore does not add decoys - the
same total population is redistributed across more boards.

That division was introduced deliberately. Before it, three co-located boards each sized themselves
as standalone and produced ~88 synthetic BLE devices in a room holding 4-9 real ones - a ~3-5x
density inflation, and exactly the tell that design law 4 (population-match) exists to close. The
fix was correct for the problem it solved.

Its justification rested on two claims, stated in `main/fleet_pop.h`:

1. the fleet aggregate should match observed ambient density, and
2. the crowd should originate from K physical points rather than one.

**Claim 2 does not hold in actual deployment.** The boards are carried together - one bag, one
person, and the project is moving toward a single multi-board enclosure. Boards inches apart share
one antenna position, one RSSI profile, one angle of arrival. The spatial diversity the split pays
for is never realized, so the fleet pays full price (halved density per added board) for no return.

Claim 1 still holds and must be preserved.

A second, independent defect compounds this. `generate_active_target()` clamps the ambient-derived
target to `GEN_CEILING` (16 on C5, 8 on C6) and then again to `CHURN_ACTIVE_SET` (16). Ambient
density measured in this project's own drive captures runs 44 devices/minute minimum, 122 median,
529 maximum. The ambient path therefore saturates far below what any real environment would support,
*before* the `/K` division is applied at all. `CHURN_ACTIVE_SET` is additionally the legacy scale
that `settings.c:11-28` already documents as having "broke the crowd on hardware" when conflated
with the real crowd size; the same stale clamp survives in `generate_active_target`.

## Goals

- Each board contributes additively to fleet population: N boards produce ~N times the decoys.
- Population still tracks ambient density, so a sparse room does not get an implausible crowd.
- The operator can choose between environment-driven and fixed behaviour, and can bound the former.
- No board needs to know the fleet size to size itself correctly.

## Non-goals

- **Fleet self-exclusion is unchanged.** The FLEET_MACS broadcast, `fleet_note_peer_macs`, and the
  peer-MAC exclusion table stay exactly as they are. Boards must still avoid modelling, learning
  from, or detecting each other's synthetic devices as real. Only the population divisor is removed.
- **Spatial-diversity work (M9 Coven) is not revived.** A single-enclosure fleet forecloses it by
  construction. The RSSI/co-location tell (measured separability ~0.15, "modest") stays open and
  unchanged; this design neither improves nor worsens it.
- Per-radio hardware limits are untouched. `CHURN_HW_INSTANCES = 4` is a NimBLE cap; N boards give
  4N concurrent advertising slots, which is the real airtime benefit of added boards.

## Design

### Population math

Per board, with no fleet-size term anywhere:

```
base   = round(pop_ewma * GEN_FACTOR)                    // C5 1.5x, C6 1.1x (unchanged)
target = auto_scale ? base : preset_level                // AUTO follows ambient; MANUAL does not
target = clamp(target, SIM_TARGET_FLOOR, min(auto_cap, BLE_DEVICES_MAX))
```

Fleet total is simply the sum of what each board independently decides. Boards will disagree
slightly on ambient (they measure independently); this is accepted and requires no reconciliation.
Co-located boards read near-identically in practice, and independent measurement degrades gracefully
when a node goes quiet.

### Modes

Two orthogonal concerns, previously conflated into one list of density presets:

| Preset | Target | Realism |
|---|---|---|
| `PAUSE` | frozen | rotation stops, devices stay on-air |
| `AUTO` | ambient-derived, bounded by `auto_cap` | full |
| `LOW` | 25% of `BLE_DEVICES_MAX` | full |
| `MED` | 50% | full |
| `HIGH` | 100% | full |
| `TURBO` | 100% | **none** - personas released, max churn, ambient ignored |

`HIGH` and `TURBO` reach the same device count but differ in behaviour: `HIGH` is a large *plausible*
crowd with intact lifecycles, persona binding, and rotation cadence; `TURBO` abandons realism for
identifier throughput. That distinction already exists in `coexist.c:124-128` and is preserved.

MANUAL levels are **per-board percentages of that board's hardware maximum**, not fleet totals. Each
board applies its level independently - no node-count dependency, and adding a board predictably
adds its share. The console displays the resulting fleet total so the operator sees the real number.

This retires `STEALTH` and `NORMAL`, which on the C5 resolved to identical settings because
`sim_settings_floor()` and `sim_settings_ceiling()` both landed on 32 (documented in
`CODE-REVIEW-VERIFICATION.md` H5 as ambiguous). The collision cannot recur: AUTO and the manual
levels are structurally different states, not two numbers that may clamp together.

### The cap

`auto_cap` bounds AUTO only. In MANUAL the operator has already named the level, so the cap is a
redundant `min()` rather than a competing authority.

The operator sets a **fleet total** on the CYD; the CYD divides by its own roster's live node count
and transmits a per-board value. Node count comes from `fleet_status` (the Vigil's own roster),
not from any decoy-side census - decoys remain free of fleet-size logic.

Worked example, 4 boards, cap 96:

| Environment | base/board | AUTO target | Result |
|---|---|---|---|
| Empty room | 6 (floor) | 24 total | 24 - ambient binds |
| Cafe | 24 | 96 total | 96 - at cap |
| Transit | 32 (hw max) | 128 total | 96 - cap binds |

### Code changes

**Remove `fleet_pop_share` from all four population call sites:**
- `coexist.c:328` - BLE re-profile target
- `simulacra_main.c:149` - BLE boot sizing
- `coexist.c:450` - Wi-Fi probe-agent glide target
- `settings.c:34,43` - `sim_settings_ceiling()` / `sim_settings_floor()`

**Keep `main/fleet_pop.{c,h}`, remove its callers.** After this change nothing in the population
path consumes the K census, but the module is retained deliberately - the live node count remains
useful telemetry and the spatial-deployment option is not being permanently foreclosed at the code
level, only at the product level. The census-change resize hook in `coexist.c:360-370` is removed
along with the four `fleet_pop_share` call sites; `fleet_pop_refresh()` may stay wired to the
coexist tick so the cached census stays current for any future consumer.

**This leaves a module with no callers, which needs an explicit guard.** The DRIFT-1/DRIFT-2 pattern
this project has been bitten by is not unused code per se - it is code that *looks* live, retains
callers, and silently does nothing, so operator-facing surfaces report behaviour the firmware is not
performing. To keep this retention from decaying into that:

- `fleet_pop.h` must carry a header comment stating plainly that it is retained-but-unused as of
  2026-08-24, that population is additive and no longer divided by K, and that any future caller is
  reintroducing fleet-size coupling deliberately rather than restoring an assumed default.
- No firmware surface (status wire, console, logs) may report a value derived from
  `fleet_pop_size()` while nothing acts on it.
- A `decoy_audit` test asserts the population path contains no `/K` term, so a future edit cannot
  quietly reintroduce division through this module without failing a test.

**Retire the stale ceilings in `generate_active_target()` (`generate.c:232-239`):** drop the
`CHURN_ACTIVE_SET` clamp and `GEN_CEILING`; the binding limits become `BLE_DEVICES_MAX` and
`auto_cap`. `GEN_FACTOR` and `GEN_FLOOR` stay - they are the board-tuning knobs and remain correct.

**`sim_settings_t` gains two fields:**

```c
typedef struct {
    uint8_t  active_target;
    bool     paused;
    float    accel;
    bool     turbo;
    bool     auto_scale;   // NEW: re-profile drives target from ambient
    uint8_t  auto_cap;     // NEW: AUTO upper bound (this board's share of the fleet cap)
} sim_settings_t;
```

`auto_scale` is what makes MANUAL stick: when false, `coexist_reprofile` must not overwrite
`active_target`, or a manual level would be silently clobbered at the next re-profile tick (up to
10 minutes on Ward). Both fields drive live behaviour, preserving the contract stated at
`settings.h:25-27` that no field exists for display only.

**Wire:** `config_cmd_t` gains a `cap` byte; `CONFIG_WIRE_VER` 1 -> 2. `CONFIG_CLEAR_THREATS`
sentinel behaviour is unchanged. The bump is required regardless of the cap, because preset ordinals
change meaning - a v1 Vigil sending `STEALTH` to a v2 decoy would be read as `LOW`. Version checking
turns that into a loud rejection instead of silent misbehaviour.

**NVS:** settings key `settings2` -> `settings3`. A persisted `active_target` means something
different under the new scale, and the mode fields did not previously exist. The old blob is
abandoned and defaults re-derived, exactly as the `settings1 -> settings2` migration did.

**CYD console:** CONTROL keeps its cycle-and-send layout with relabelled presets plus an adjustable
cap. DECOYS shows fleet total, per-node share, and current mode:

```
CROWD   96 total  -  32/node  -  AUTO (cap 96)
  N0 24   N1 24   N2 24   N3 24
```

## Testing

- **`tools/radar_audit`** - preset labels, AUTO/MANUAL indication, cap rendering, live-vs-pending
  state, per-node + total display.
- **`tools/decoy_audit`** - AUTO target tracks `pop_ewma`; MANUAL levels ignore it; no `/K` term
  survives anywhere in the population path; population scales linearly with board count.
- **`churn_selftest`** (on-target) - every preset resolves within bounds; personas still fit at every
  level (the H5 invariant: personas capped at half the crowd, so the floor must host the designed
  persona count); wire v2 byte layout pinned.
- **Detectability scorecard, before and after.** Population size feeds `presence_duration` and
  `address_type_mix`, both previously closed. A ~4x density change could move them. This is the gate
  that determines whether additive population costs measurable realism.
- **Hardware** - whole fleet flashed together (wire v2 is breaking). Verify: AUTO tracks a room as
  density changes; MANUAL holds its level across a re-profile tick; cap binds in a dense environment;
  fleet total on the console matches the sum of per-node counts.

## Risks

**This deliberately reopens the H8 density question.** Ambient-scaling is the mitigation, but that is
reasoning, not measurement. The Kismet re-capture is what would actually prove it, and it is a
**required follow-up, not an optional one**. The specific scenario to check is the one H8 was found
in: a device-sparse home environment with the full fleet running, confirming AUTO collapses the crowd
to something plausible rather than stacking N boards' worth of decoys into an empty room.

Secondary: `BLE_DEVICES_MAX` (32) saturates at roughly `pop_ewma >= 21` on the C5. Above that, added
boards - not ambient density - are the only thing that increases fleet population. This is expected
and correct, but means the AUTO curve flattens in dense environments and should not be mistaken for
the ambient path failing.

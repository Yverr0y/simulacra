# Additive Fleet Population + AUTO/MANUAL Modes Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make each decoy board size its crowd independently (additive across boards) instead of running `1/K` of a fleet-wide target, and replace the density-preset list with orthogonal AUTO (ambient-driven) and MANUAL (fixed level) modes.

**Architecture:** Remove the fleet-size divisor from all four population call sites; retire two stale ceilings that capped the ambient path at 16 regardless of measured density; add `auto_scale`/`auto_cap` to the settings struct so MANUAL levels survive a re-profile tick; bump the CONFIG wire to carry the cap. `fleet_pop.{c,h}` is retained but left with no callers in the population path.

**Tech Stack:** C (ESP-IDF v5.5 for ESP32-C5, v5.4 for ESP32-C6 and the classic-ESP32 CYD), Python 3.12 `unittest` for host harnesses, MSVC `cl` on Windows / `cc` + `make` on POSIX-CI.

**Spec:** `docs/superpowers/specs/2026-08-24-additive-fleet-population-design.md`

## Global Constraints

- **Whole-fleet reflash required.** `CONFIG_WIRE_VER` goes 1 -> 2 and preset ordinals change meaning. A v1 Vigil sending `STEALTH` (ordinal 1) to a v2 decoy would be read as `AUTO`. Never flash a partial fleet during or after this work.
- **Fleet self-exclusion is out of scope and must not be touched.** `broadcast_fleet_macs`, `fleet_note_peer_macs`, `fleet_note_peer_node`, `FLEET_MAC_CAP`, and the FLEET_MACS frame stay exactly as they are. Only the population divisor is removed.
- **`main/fleet_pop.{c,h}` is retained, not deleted.** Its existing host tests (`tools/decoy_audit/tests/test_fleet_pop.py`) must continue to pass unchanged - the module still works, it simply has no caller in the population path.
- **Two-places rule for host harnesses.** Any new source file added to a tool must be added to BOTH that tool's `run.ps1` (Windows/MSVC) and its `Makefile` (POSIX/CI). Missing one means local tests pass while CI silently does not compile it, or vice versa.
- **No `-Werror` regressions.** The firmware build is currently warning-clean on all three targets; keep it that way.
- Every field in `sim_settings_t` must drive live behaviour. A field stored for display only is what caused the CYD to report a preset the firmware was not running (DRIFT-1).
- Commit identity is the repo-local default. Do not add `Co-Authored-By:` or `Claude-Session:` trailers to commits (scrubbed from history 2026-08-17).

## Spec Refinements Discovered During Planning

Two spec statements are wrong against the actual code and are corrected here. Both were verified by reading the source, not inferred.

**1. The persona floor blocks MANUAL levels.** `sim_settings_floor()` returns `2 * probe_phone_target()`. On the C5 `PROBE_PHONES` is 16, so the floor is **32** - identical to the ceiling. `sim_settings_clamp` would raise LOW (8) and MED (16) straight back to 32, recreating the exact H5 collision this change exists to fix.

The floor's stated purpose (`settings.h:11-18`) is that room density "must not squeeze out the personas themselves, which are a design constant of the node, not a property of the room." That reasoning governs *room-driven* resizing. It does not govern an explicit operator instruction. **Resolution: the persona floor applies in AUTO only; MANUAL clamps against `SIM_TARGET_FLOOR` (4).** `coexist.c:420-423` already caps personas at `crowd/2` dynamically, so personas shrink to fit a manual level without further work.

**2. MANUAL percentages are of the board's ceiling, not `BLE_DEVICES_MAX`.** `BLE_DEVICES_MAX` is 32 for both chips, but the C6's designed crowd (`probe_desired_ble_floor()`) is 24. Using the raw max would make HIGH mean "32" on a board whose ceiling clamps it to 24 anyway, and would make LOW/MED wrong relative to that board's real capacity. **Resolution: LOW/MED/HIGH = 25%/50%/100% of `ceiling`.** Yields C5 8/16/32 and C6 6/12/24.

---

### Task 1: Retire the stale ceilings in `generate_active_target`

Smallest self-contained change and it unblocks everything else: until this lands, the ambient path clamps at 16 and no amount of removing `/K` produces a larger crowd.

**Files:**
- Modify: `main/generate.c:12-21` (remove `GEN_CEILING`), `main/generate.c:232-239` (the function)
- Modify: `tools/decoy_audit/synth_dump.c` (add `--acttarget` mode)
- Test: `tools/decoy_audit/tests/test_active_target.py` (create)

**Interfaces:**
- Consumes: nothing from earlier tasks.
- Produces: `uint8_t generate_active_target(const rf_model_t *m)` - unchanged signature, new bounds `[GEN_FLOOR, BLE_DEVICES_MAX]`. Task 4 calls it without a fleet divisor.

- [ ] **Step 1: Add the `--acttarget` harness mode**

In `tools/decoy_audit/synth_dump.c`, add this block immediately before the existing `--devices` block (around line 256). It builds a model with a given `pop_ewma` and prints the resulting target:

```c
    if (argc > 1 && strcmp(argv[1], "--acttarget") == 0) {
        rf_model_t m; memset(&m, 0, sizeof m);
        m.pop_ewma = argc > 2 ? (float)strtod(argv[2], 0) : 0.0f;
        m.sweeps   = 1;
        printf("%u\n", (unsigned)generate_active_target(&m));
        return 0;
    }
```

Confirm `rf_model.h` and `generate.h` are already included at the top of the file; add them if not.

- [ ] **Step 2: Write the failing test**

Create `tools/decoy_audit/tests/test_active_target.py`:

```python
import os, subprocess, unittest

HERE = os.path.dirname(os.path.abspath(__file__)); TOOL = os.path.dirname(HERE)
EXE  = os.path.join(TOOL, "synth_dump.exe" if os.name == "nt" else "synth_dump")

def target(pop):
    out = subprocess.check_output([EXE, "--acttarget", str(pop)], text=True)
    return int(out.strip())

@unittest.skipUnless(os.path.exists(EXE), "synth_dump not built")
class ActiveTarget(unittest.TestCase):
    def test_floor_holds_in_empty_room(self):
        # pop 0 must not yield 0 devices; GEN_FLOOR is 6 on C5 / 4 on C6, host build is C5-ish
        self.assertGreaterEqual(target(0), 4)

    def test_scales_with_density(self):
        # the whole point: a denser room yields a bigger crowd
        self.assertGreater(target(24), target(8))

    def test_exceeds_the_old_16_clamp(self):
        # regression guard: CHURN_ACTIVE_SET (16) and GEN_CEILING must no longer bind
        self.assertGreater(target(40), 16)

    def test_saturates_at_hardware_max(self):
        # BLE_DEVICES_MAX is 32; nothing may exceed the static array size
        self.assertLessEqual(target(500), 32)
        self.assertEqual(target(500), 32)
```

- [ ] **Step 3: Run the test to verify it fails**

```
cd tools/decoy_audit && make && python -m pytest tests/test_active_target.py -v
```
Expected: `test_exceeds_the_old_16_clamp` FAILS (returns 16, the `CHURN_ACTIVE_SET` clamp), and `test_saturates_at_hardware_max` FAILS (returns 16, not 32).

- [ ] **Step 4: Remove the stale ceilings**

In `main/generate.c`, delete the two `GEN_CEILING` lines from the persona-profile block so it reads:

```c
#if CONFIG_IDF_TARGET_ESP32C5
#define GEN_FACTOR_X10 15   // Ward: 1.5x
#define GEN_FLOOR      6
#else
#define GEN_FACTOR_X10 11   // Shade: 1.1x
#define GEN_FLOOR      4
#endif
```

Replace the function body at line 232:

```c
// Ambient-derived crowd size for THIS board. No fleet-size divisor: boards are additive, each
// sizing itself from what it measures (see 2026-08-24-additive-fleet-population-design.md).
// The old GEN_CEILING (16/8) and CHURN_ACTIVE_SET (16) clamps are gone -- CHURN_ACTIVE_SET is the
// legacy scale that settings.c already documents as having broken the crowd on hardware when
// conflated with the real crowd size, and it capped this path far below any real environment
// (measured ambient runs 44-529 devices/min). BLE_DEVICES_MAX is the real bound: the static array.
uint8_t generate_active_target(const rf_model_t *m)
{
    int t = (int)((m->pop_ewma * GEN_FACTOR_X10 + 5) / 10);   // round(pop*factor)
    if (t < GEN_FLOOR) t = GEN_FLOOR;
    if (t > BLE_DEVICES_MAX) t = BLE_DEVICES_MAX;
    return (uint8_t)t;
}
```

Add `#include "ble_devices.h"` near the top of `main/generate.c` if absent. Check whether `#include "churn.h"` (line 4, commented `// CHURN_ACTIVE_SET`) is still needed for anything else in the file; if `CHURN_ACTIVE_SET` was its only use, remove the include.

- [ ] **Step 5: Run the tests to verify they pass**

```
cd tools/decoy_audit && make && python -m pytest tests/test_active_target.py -v
```
Expected: 4 passed.

- [ ] **Step 6: Run the full decoy_audit suite for regressions**

```
cd tools/decoy_audit && python -m pytest tests/ -q
```
Expected: all pass. If `test_synth_dump.py` or `test_scorecard.py` assert an old target value, update the expectation to the new bound and note why in the test comment - do not re-pin to a lucky number.

- [ ] **Step 7: Commit**

```bash
git add main/generate.c tools/decoy_audit/synth_dump.c tools/decoy_audit/tests/test_active_target.py
git commit -m "generate: let the ambient crowd target scale past the legacy 16-device clamp"
```

---

### Task 2: AUTO/MANUAL preset model in settings

**Files:**
- Modify: `main/settings.h:20-34` (enum + struct), `main/settings.h` (add two accessors)
- Modify: `main/settings.c:10-16` (NVS key), `main/settings.c:29-47` (floor/ceiling), `main/settings.c:63-91` (resolve), `main/settings.c:140-156` (match_preset), `main/settings.c:165-175` (init)
- Test: extend `main/churn_selftest.c`

**Interfaces:**
- Consumes: `BLE_DEVICES_MAX` (`ble_devices.h`), `probe_desired_ble_floor()` / `probe_phone_target()` (`probe.h`).
- Produces:
  - `sim_preset_t` = `{SIM_PRESET_PAUSE=0, SIM_PRESET_AUTO, SIM_PRESET_LOW, SIM_PRESET_MED, SIM_PRESET_HIGH, SIM_PRESET_TURBO, SIM_PRESET_COUNT}`
  - `sim_settings_t` gains `bool auto_scale;` and `uint8_t auto_cap;`
  - `bool sim_settings_auto_scale(void);` - Task 4 gates the re-profile on this
  - `uint8_t sim_settings_auto_cap(void);` - Task 4 applies this as an upper bound

- [ ] **Step 1: Update the enum and struct**

In `main/settings.h`, replace the enum at line 20:

```c
// AUTO scales the crowd with measured ambient density; the MANUAL levels name a fixed fraction of
// this board's ceiling and ignore the room. TURBO is HIGH's device count without the realism --
// personas released, max churn (coexist_set_turbo owns that, bypassing floor/ceiling entirely).
typedef enum {
    SIM_PRESET_PAUSE = 0, SIM_PRESET_AUTO, SIM_PRESET_LOW,
    SIM_PRESET_MED, SIM_PRESET_HIGH, SIM_PRESET_TURBO, SIM_PRESET_COUNT
} sim_preset_t;
```

Replace the struct at line 28:

```c
typedef struct {
    uint8_t  active_target;                       // concurrent phantom crowd size
    bool     paused;                              // freeze rotation (phantoms stay on-air)
    float    accel;                               // lifetime divisor: >1.0 = faster arrivals/departures
    bool     turbo;                               // TURBO active: coexist_set_turbo owns the REAL
                                                  // population/churn rate, bypassing floor/ceiling
    bool     auto_scale;                          // AUTO: the re-profile tick drives active_target
                                                  // from measured ambient density. When false a
                                                  // manual level sticks -- without this flag the
                                                  // re-profile would clobber it within 10 min.
    uint8_t  auto_cap;                            // AUTO upper bound (this board's share of the
                                                  // operator's fleet-wide cap). 0 = uncapped.
} sim_settings_t;
```

Add these accessors near `sim_settings_get_paused` at line 64:

```c
// AUTO mode active? The re-profile tick must not overwrite active_target when this is false.
bool    sim_settings_auto_scale(void);
// Current AUTO upper bound (0 = uncapped). Applied by the re-profile after the ambient estimate.
uint8_t sim_settings_auto_cap(void);
```

- [ ] **Step 2: Bump the NVS key**

In `main/settings.c`, replace lines 11-16:

```c
// Key bumped to "settings3" for the 2026-08-24 additive-population change. active_target is stored
// as an absolute count and its scale changed again (no more 1/K division), and the struct gained
// auto_scale/auto_cap which no v2 blob carries. A stale blob would restore a number that no longer
// means what it did and a zeroed auto_scale, silently pinning the board to MANUAL. Abandon it and
// re-derive defaults, exactly as the settings1 -> settings2 migration did.
#define SETTINGS_NVS_KEY "settings3"
```

- [ ] **Step 3: Drop the fleet divisor from floor and ceiling**

Replace `sim_settings_ceiling` and `sim_settings_floor` (lines 29-47):

```c
// Preset ceiling = this board's DESIGNED crowd size (personas + unbound companions). No fleet
// divisor: boards are additive as of 2026-08-24, each sizing itself independently.
uint8_t sim_settings_ceiling(void)
{
    int c = probe_desired_ble_floor();
    if (c > BLE_DEVICES_MAX) c = BLE_DEVICES_MAX;
    if (c < SIM_TARGET_FLOOR) c = SIM_TARGET_FLOOR;
    return (uint8_t)c;
}

// Lower bound for AUTO only: enough devices to host this node's designed persona count (personas
// are capped at half the crowd, so N personas need 2N devices). This guards ROOM-driven resizing
// from squeezing out the phones. It deliberately does NOT bind a MANUAL level -- an operator asking
// for LOW means LOW, and coexist already caps personas at crowd/2 so they shrink to fit.
uint8_t sim_settings_floor(void)
{
    int f = 2 * probe_phone_target();
    if (f > BLE_DEVICES_MAX) f = BLE_DEVICES_MAX;
    if (f < SIM_TARGET_FLOOR) f = SIM_TARGET_FLOOR;
    return (uint8_t)f;
}
```

Remove `#include "fleet_pop.h"` from `main/settings.c` (line 6).

- [ ] **Step 4: Rewrite `sim_settings_resolve`**

Replace lines 63-91:

```c
// Presets differ ONLY in knobs the engine actually reads: crowd size, turnover rate, pause, and
// whether ambient density drives the target.
//
// MANUAL levels clamp against SIM_TARGET_FLOOR, not the persona floor. On the C5 the persona floor
// equals the ceiling (16 personas x2 = 32 = BLE_DEVICES_MAX), so clamping a manual level against it
// would raise LOW and MED straight back to HIGH -- the exact preset collision this design removes.
int sim_settings_resolve(sim_preset_t p, uint8_t floor, uint8_t ceiling, sim_settings_t *out)
{
    if (p >= SIM_PRESET_COUNT) return -1;
    sim_settings_t s = { .active_target = ceiling, .paused = false, .accel = 1.0f,
                         .turbo = false, .auto_scale = false, .auto_cap = 0 };
    uint8_t eff_floor = SIM_TARGET_FLOOR;
    switch (p) {
    case SIM_PRESET_PAUSE:                                  // AUTO values, rotation frozen
        s.auto_scale = true; s.paused = true; eff_floor = floor; break;
    case SIM_PRESET_AUTO:
        s.auto_scale = true; eff_floor = floor; break;      // re-profile owns active_target
    case SIM_PRESET_LOW:
        s.active_target = (uint8_t)((ceiling * 25) / 100); break;
    case SIM_PRESET_MED:
        s.active_target = (uint8_t)((ceiling * 50) / 100); break;
    case SIM_PRESET_HIGH:
        s.active_target = ceiling; break;
    case SIM_PRESET_TURBO:
        // active_target/accel are irrelevant once turbo=true: sim_settings_match_preset
        // short-circuits on the flag alone and coexist_set_turbo forces the real population.
        s.turbo = true; break;
    default: return -1;
    }
    sim_settings_clamp(&s, eff_floor, ceiling);
    *out = s;
    return 0;
}
```

- [ ] **Step 5: Teach `match_preset` about the new fields**

In `main/settings.c`, in `sim_settings_match_preset`, add an `auto_scale` comparison so AUTO and a
MANUAL level that happen to share a target are not confused. Replace the comparison at line 151:

```c
        if (r.auto_scale != cur->auto_scale) continue;
        if (cur->auto_scale) return p;   // in AUTO the live target follows the room, not the preset
        if (r.active_target == cur->active_target && r.paused == cur->paused &&
            r.accel == cur->accel)
            return p;
```

Note the `cur->auto_scale` early return mirrors the existing turbo short-circuit and for the same
reason: in AUTO the live `active_target` is whatever the room dictated, so requiring it to equal the
resolved value would always report CUSTOM.

PAUSE and AUTO both set `auto_scale = true`, and they are distinguished by `paused`. Because the
loop runs in enum order, PAUSE (ordinal 0) is tested first; the `cur->auto_scale` early return would
otherwise return PAUSE for an un-paused AUTO board. Guard it:

```c
        if (r.auto_scale != cur->auto_scale) continue;
        if (r.paused != cur->paused) continue;
        if (cur->auto_scale) return p;
```

- [ ] **Step 6: Default to AUTO at boot**

In `sim_settings_init` (line 172), change the unloaded default:

```c
    if (!loaded) sim_settings_resolve(SIM_PRESET_AUTO, sim_settings_floor(), sim_settings_ceiling(), &s);
```

Change the guard clamp on the next line so a MANUAL blob is not forced up to the persona floor:

```c
    sim_settings_clamp(&s, s.auto_scale ? sim_settings_floor() : SIM_TARGET_FLOOR,
                       sim_settings_ceiling());
```

Apply the same conditional-floor treatment in `sim_settings_set` (line 112), `sim_settings_apply_preset` (line 119), and `sim_settings_recalc_bounds` (line 134): each currently passes `sim_settings_floor()` unconditionally. In all four, the floor argument becomes `s.auto_scale ? sim_settings_floor() : SIM_TARGET_FLOOR` using that call site's own settings variable.

- [ ] **Step 7: Add on-target assertions**

In `main/churn_selftest.c`, find the settings block near line 1526 (`const uint8_t fl = sim_settings_floor(), ce = sim_settings_ceiling();`) and add after the existing preset loop:

```c
    {   // MANUAL levels must be distinct and must NOT be raised by the persona floor -- on the C5
        // that floor equals the ceiling, which is what used to collapse STEALTH onto NORMAL.
        sim_settings_t lo, me, hi, au;
        ST_CHECK(sim_settings_resolve(SIM_PRESET_LOW,  fl, ce, &lo) == 0, "LOW resolves");
        ST_CHECK(sim_settings_resolve(SIM_PRESET_MED,  fl, ce, &me) == 0, "MED resolves");
        ST_CHECK(sim_settings_resolve(SIM_PRESET_HIGH, fl, ce, &hi) == 0, "HIGH resolves");
        ST_CHECK(sim_settings_resolve(SIM_PRESET_AUTO, fl, ce, &au) == 0, "AUTO resolves");
        ST_CHECK(lo.active_target < me.active_target, "LOW is smaller than MED");
        ST_CHECK(me.active_target < hi.active_target, "MED is smaller than HIGH");
        ST_CHECK(hi.active_target == ce, "HIGH is the board ceiling");
        ST_CHECK(!lo.auto_scale && !me.auto_scale && !hi.auto_scale, "manual levels are not auto");
        ST_CHECK(au.auto_scale, "AUTO sets auto_scale");
        // Every level must still round-trip through match_preset, or the console lies about mode.
        ST_CHECK(sim_settings_match_preset(&lo, fl, ce) == SIM_PRESET_LOW,  "LOW round-trips");
        ST_CHECK(sim_settings_match_preset(&me, fl, ce) == SIM_PRESET_MED,  "MED round-trips");
        ST_CHECK(sim_settings_match_preset(&hi, fl, ce) == SIM_PRESET_HIGH, "HIGH round-trips");
        ST_CHECK(sim_settings_match_preset(&au, fl, ce) == SIM_PRESET_AUTO, "AUTO round-trips");
    }
```

Search the file for `SIM_PRESET_STEALTH`, `SIM_PRESET_NORMAL`, `SIM_PRESET_DENSE`, and `SIM_PRESET_MAX` and update every occurrence to the new enum. Existing assertions that personas fit at every preset (the H5 invariant) must be re-checked against AUTO only, since MANUAL LOW deliberately runs below the persona floor.

- [ ] **Step 8: Build all three targets**

```
# C5 (IDF 5.5)
idf.py set-target esp32c5 && idf.py build
# C6 (IDF 5.4)
idf.py set-target esp32c6 && idf.py build
# CYD (IDF 5.4)
cd cyd && idf.py set-target esp32 && idf.py build
```
Expected: `Project build complete` for each, warning-clean. The CYD will fail to compile until Task 6 if it references the old preset names - if so, complete Task 6 before re-running the CYD build and note it here.

- [ ] **Step 9: Commit**

```bash
git add main/settings.h main/settings.c main/churn_selftest.c
git commit -m "settings: replace density presets with AUTO/MANUAL modes"
```

---

### Task 3: Remove the fleet divisor from the population path

**Files:**
- Modify: `main/coexist.c:328` (BLE re-profile), `main/coexist.c:360-370` (census resize hook), `main/coexist.c:448-451` (Wi-Fi target)
- Modify: `main/simulacra_main.c:149-153` (boot sizing)
- Modify: `main/fleet_pop.h` (retention comment)
- Test: `tools/decoy_audit/tests/test_no_fleet_divisor.py` (create)

**Interfaces:**
- Consumes: `generate_active_target()` from Task 1.
- Produces: no new symbols. `fleet_pop_share`, `fleet_pop_share_k`, `fleet_pop_size`, `fleet_pop_refresh` remain compiled and exported but are no longer called from the population path.

- [ ] **Step 1: Write the failing guard test**

Create `tools/decoy_audit/tests/test_no_fleet_divisor.py`. This is a source-level structural test - the same technique `probe_audit` already uses to assert `ssid_pool` includes nothing from the observe pipeline:

```python
import os, re, unittest

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", "..", ".."))
MAIN = os.path.join(ROOT, "main")

# fleet_pop.{c,h} legitimately define these; no OTHER file may call them. Boards are additive as of
# 2026-08-24 -- a reintroduced divisor would silently halve the crowd per added board again.
CALLERS_FORBIDDEN = ("coexist.c", "simulacra_main.c", "settings.c", "generate.c", "probe.c")
PATTERN = re.compile(r"\bfleet_pop_share(_k)?\s*\(")

class NoFleetDivisor(unittest.TestCase):
    def test_population_path_has_no_k_division(self):
        for name in CALLERS_FORBIDDEN:
            path = os.path.join(MAIN, name)
            if not os.path.exists(path):
                continue
            with open(path, encoding="utf-8", errors="replace") as f:
                for lineno, line in enumerate(f, 1):
                    if line.lstrip().startswith("//"):
                        continue
                    self.assertIsNone(PATTERN.search(line),
                        f"{name}:{lineno} calls fleet_pop_share - population is additive, "
                        f"see docs/superpowers/specs/2026-08-24-additive-fleet-population-design.md")

    def test_fleet_pop_module_still_exists(self):
        # Retained deliberately (telemetry / future use), just uncalled. Not deleted.
        self.assertTrue(os.path.exists(os.path.join(MAIN, "fleet_pop.c")))
        self.assertTrue(os.path.exists(os.path.join(MAIN, "fleet_pop.h")))
```

- [ ] **Step 2: Run it to verify it fails**

```
cd tools/decoy_audit && python -m pytest tests/test_no_fleet_divisor.py -v
```
Expected: `test_population_path_has_no_k_division` FAILS listing `coexist.c:328`, `coexist.c:450`, `settings.c` (if Task 2 is not yet applied), and `simulacra_main.c:149`.

- [ ] **Step 3: Remove the divisor from the BLE re-profile**

In `main/coexist.c`, replace lines 320-333 (the comment block and the target computation):

```c
    // Room density flexes the crowd, but in AUTO never below what this node's designed persona
    // count needs (personas are capped at half the crowd, so N personas require 2N devices) -- the
    // phones we present are a design constant of the node; it is the unbound beacons/tags that flex.
    // No fleet divisor: each board sizes itself from its own measurement and boards are additive
    // (2026-08-24). A MANUAL level is the operator's explicit choice and must survive this tick.
    if (sim_settings_auto_scale()) {
        uint8_t at = generate_active_target(cur);
        uint8_t floor_n = sim_settings_floor();
        if (at < floor_n) at = floor_n;
        uint8_t cap = sim_settings_auto_cap();
        if (cap && at > cap) at = cap;
        churn_set_active_target(at);                        // resize to the new population
        ESP_LOGW(TAG, "reprofile: drift=%.3f active_target=%u (floor %u, cap %u)",
                 score, (unsigned)at, (unsigned)floor_n, (unsigned)cap);
    } else {
        ESP_LOGW(TAG, "reprofile: drift=%.3f (manual mode, crowd unchanged)", score);
    }
```

- [ ] **Step 4: Remove the census resize hook**

In `main/coexist.c`, delete the entire block at lines 360-370 (`fleet_pop_refresh(now);` plus the `static int s_last_k` resize block). It exists solely to react to K changing, which no longer affects population.

- [ ] **Step 5: Remove the divisor from the Wi-Fi target**

In `main/coexist.c`, replace lines 448-451:

```c
            int wt      = s_wifi_obs_ok ? wifi_obs_target(now) : WIFI_OBS_FALLBACK;
            int agents  = wt;                                  // additive: no fleet divisor
            probe_agents_glide_set_target(agents, now);        // glide toward it (boot-instant first time)
```

Update the log line immediately below it (line 452 onward) to drop the `/nodes=` term, keeping the applied value visible. Read the existing `ESP_LOGW` and remove only the divisor argument.

- [ ] **Step 6: Remove the divisor from boot sizing**

In `main/simulacra_main.c`, replace lines 149-153:

```c
            uint8_t at = generate_active_target(&m);           // this board's own ambient estimate
            ...
            ESP_LOGW(TAG, "boot population: pop_ewma=%u -> active_target=%u",
                     (unsigned)(m.pop_ewma + 0.5f), (unsigned)at);
```

Preserve whatever lines sit between the assignment and the log in the current source; only the `fleet_pop_share(...)` wrapper and the `fleet_k` log argument are being removed. Remove the now-unused `fleet_k` local if one exists.

- [ ] **Step 7: Add the retention comment to `fleet_pop.h`**

At the top of `main/fleet_pop.h`, immediately after `#include <stdint.h>`, insert:

```c
// RETAINED BUT UNCALLED as of 2026-08-24. Fleet population is ADDITIVE: each board sizes its crowd
// from its own ambient measurement and boards are no longer divided by the node census. Nothing in
// the population path calls anything in this file.
//
// It is kept because the live node count remains useful telemetry and because foreclosing spatial
// deployment at the code level is not the intent -- that is a product decision, not a code one.
//
// If you are about to call fleet_pop_share() again: you are REINTRODUCING fleet-size coupling, not
// restoring a default that went missing. tools/decoy_audit/tests/test_no_fleet_divisor.py will fail
// and is meant to. Read docs/superpowers/specs/2026-08-24-additive-fleet-population-design.md first.
//
// Do not surface a value derived from fleet_pop_size() on any operator-facing surface (status wire,
// console, logs) while nothing acts on it -- reporting a number the firmware does not use is the
// DRIFT-1 failure this project has already shipped once.
```

- [ ] **Step 8: Run the guard test and the full suite**

```
cd tools/decoy_audit && make && python -m pytest tests/ -q
```
Expected: all pass, including `test_no_fleet_divisor.py` and the untouched `test_fleet_pop.py` (the module still works standalone).

- [ ] **Step 9: Build C5 and C6**

```
idf.py set-target esp32c5 && idf.py build
idf.py set-target esp32c6 && idf.py build
```
Expected: `Project build complete`, warning-clean. Watch for an unused-variable warning on a leftover `fleet_k` or `k` local and remove it.

- [ ] **Step 10: Commit**

```bash
git add main/coexist.c main/simulacra_main.c main/fleet_pop.h tools/decoy_audit/tests/test_no_fleet_divisor.py
git commit -m "coexist: size the crowd per-board instead of dividing by fleet size"
```

---

### Task 4: Settings accessors for the re-profile gate

Task 3's re-profile calls `sim_settings_auto_scale()` and `sim_settings_auto_cap()`. This task implements them. Sequence Task 4 before Task 3 if you prefer a compiling tree at every commit; the plan orders them this way so the behavioural change and its guard test land together.

**Files:**
- Modify: `main/settings.c` (add two functions near `sim_settings_get_paused`, line 163)

**Interfaces:**
- Consumes: `sim_settings_t.auto_scale` / `.auto_cap` from Task 2.
- Produces: `bool sim_settings_auto_scale(void)`, `uint8_t sim_settings_auto_cap(void)` - both declared in Task 2 Step 1.

- [ ] **Step 1: Implement the accessors**

In `main/settings.c`, after `bool sim_settings_get_paused(void) { return s_cur.paused; }`:

```c
bool    sim_settings_auto_scale(void) { return s_cur.auto_scale; }
uint8_t sim_settings_auto_cap(void)   { return s_cur.auto_cap; }
```

- [ ] **Step 2: Build to verify**

```
idf.py set-target esp32c5 && idf.py build
```
Expected: `Project build complete`.

- [ ] **Step 3: Commit**

```bash
git add main/settings.c
git commit -m "settings: expose auto_scale and auto_cap to the coexist tick"
```

---

### Task 5: CONFIG wire v2 carries the cap

**Files:**
- Modify: `components/simulacra_radar/config_wire.h:5-15`
- Modify: `main/esp_now_link.c` (the CONFIG handling path, around lines 190-200)
- Modify: `cyd/main/cyd_main.c` (the `send_config` path)
- Test: extend `main/churn_selftest.c`

**Interfaces:**
- Consumes: `sim_preset_t` from Task 2.
- Produces: `config_cmd_t` = `{uint8_t version; uint8_t preset_id; uint8_t cap;}`, `CONFIG_WIRE_VER 2`, `CONFIG_WIRE_PAYLOAD_LEN` = 67.

- [ ] **Step 1: Update the wire struct**

In `components/simulacra_radar/config_wire.h`, replace lines 6 and 10-15:

```c
#define CONFIG_WIRE_VER   2          // v2 (2026-08-24): + cap byte; preset ordinals changed meaning

typedef struct __attribute__((packed)) {
    uint8_t version;                 // CONFIG_WIRE_VER
    uint8_t preset_id;               // sim_preset_t value (validated on the decoy)
    uint8_t cap;                     // AUTO upper bound, this board's share (0 = uncapped).
                                     // Ignored for MANUAL presets, which name their own level.
} config_cmd_t;

#define CONFIG_WIRE_PAYLOAD_LEN (sizeof(config_cmd_t) + CONFIG_SIG_LEN)   // 67
```

Leave `CONFIG_CLEAR_THREATS` (0xFF) and both function prototypes unchanged - the signature covers `nonce12 || cmd`, so a longer `cmd` is signed correctly with no change to the pack/open functions.

- [ ] **Step 2: Apply the cap on the decoy**

In `main/esp_now_link.c`, locate the CONFIG branch that calls `config_wire_open_signed` and then dispatches the preset (near line 190). After the version check and before applying the preset, store the cap so the preset resolve can pick it up. Add immediately after a successful `config_wire_open_signed`:

```c
        if (cmd.version != CONFIG_WIRE_VER) {
            ESP_LOGW(ETAG, "config: wire v%u rejected (need v%u) -- reflash the whole fleet",
                     (unsigned)cmd.version, (unsigned)CONFIG_WIRE_VER);
            return;
        }
```

Then route the preset through the existing `coexist_request_preset` inbox (do NOT call settings directly from the RX path - P2 in `CODE-REVIEW-VERIFICATION.md` documents why: the coexist task is the single writer). Extend that request to carry the cap, matching however `coexist_request_preset` is currently declared in `main/coexist.h`; add a `uint8_t cap` parameter and store it alongside the pending preset.

- [ ] **Step 3: Send the cap from the Vigil**

In `cyd/main/cyd_main.c`, find `send_config` and set the new field before packing:

```c
    config_cmd_t cmd = { .version = CONFIG_WIRE_VER, .preset_id = (uint8_t)preset,
                         .cap = cap_per_node };
```

`cap_per_node` is the operator's fleet total divided by the live node count from the Vigil's own roster:

```c
    // The operator sets a FLEET TOTAL; decoys are additive and know nothing about fleet size, so
    // the Vigil does the division here from its own roster.
    int nodes = fleet_status_alive_count(&s_fleet, now);
    if (nodes < 1) nodes = 1;
    uint8_t cap_per_node = (uint8_t)((s_cap_total + nodes / 2) / nodes);
```

If `fleet_status_alive_count` does not exist under that exact name, use whatever the roster already exposes for a live-node count (check `cyd/main/fleet_status.h`) and keep the round-to-nearest form.

- [ ] **Step 4: Pin the wire layout on-target**

In `main/churn_selftest.c`, add near the other wire assertions:

```c
    {   // Wire v2 layout is a fleet-wide contract: a v1 Vigil would send STEALTH(1) and a v2 decoy
        // would read AUTO(1). The version check must reject it rather than silently misapply.
        ST_CHECK(sizeof(config_cmd_t) == 3, "config_cmd_t is 3 bytes in wire v2");
        ST_CHECK(CONFIG_WIRE_PAYLOAD_LEN == 67, "config payload is cmd(3) + sig(64)");
        ST_CHECK(CONFIG_WIRE_VER == 2, "config wire version is 2");
    }
```

- [ ] **Step 5: Build all three targets**

```
idf.py set-target esp32c5 && idf.py build
idf.py set-target esp32c6 && idf.py build
cd cyd && idf.py set-target esp32 && idf.py build
```
Expected: all three `Project build complete`.

- [ ] **Step 6: Commit**

```bash
git add components/simulacra_radar/config_wire.h main/esp_now_link.c main/coexist.h main/coexist.c cyd/main/cyd_main.c main/churn_selftest.c
git commit -m "wire: CONFIG v2 carries the AUTO cap"
```

---

### Task 6: Vigil console - labels, cap control, total + per-node display

**Files:**
- Modify: `components/simulacra_radar/radar_render.c` (the CONTROL preset labels and the DECOYS/STATS page)
- Modify: `cyd/main/cyd_main.c` (cap adjust input handling)
- Test: `tools/radar_audit/tests/test_control.py`, `tools/radar_audit/tests/test_render_modes.py` (create)

**Interfaces:**
- Consumes: `sim_preset_t` from Task 2, `config_cmd_t.cap` from Task 5.
- Produces: no new symbols consumed by later tasks.

- [ ] **Step 1: Write the failing render tests**

Create `tools/radar_audit/tests/test_render_modes.py`. Follow the existing harness pattern - read `tools/radar_audit/tests/test_control.py` first and mirror how it invokes `render_dump` and parses captured text:

```python
import os, subprocess, unittest

HERE = os.path.dirname(os.path.abspath(__file__)); TOOL = os.path.dirname(HERE)
EXE  = os.path.join(TOOL, "render_dump.exe" if os.name == "nt" else "render_dump")

def render(view, *args):
    return subprocess.check_output([EXE, view, *[str(a) for a in args]], text=True)

@unittest.skipUnless(os.path.exists(EXE), "render_dump not built")
class Modes(unittest.TestCase):
    def test_control_lists_new_presets(self):
        out = render("control")
        for label in ("AUTO", "LOW", "MED", "HIGH", "TURBO", "PAUSE"):
            self.assertIn(label, out, f"CONTROL page missing {label}")

    def test_control_drops_retired_presets(self):
        out = render("control")
        for gone in ("STEALTH", "NORMAL", "DENSE"):
            self.assertNotIn(gone, out, f"CONTROL page still shows retired preset {gone}")

    def test_decoys_shows_total_and_per_node(self):
        # 3 nodes at 24 each -> the operator must see the fleet total, not just per-node counts
        out = render("stats")
        self.assertIn("total", out.lower(), "DECOYS page does not show a fleet total")
```

- [ ] **Step 2: Run to verify failure**

```
cd tools/radar_audit && make && python -m pytest tests/test_render_modes.py -v
```
Expected: `test_control_lists_new_presets` and `test_control_drops_retired_presets` FAIL (old labels present).

- [ ] **Step 3: Update the preset labels**

In `components/simulacra_radar/radar_render.c`, find `CTRL_LABELS` and replace its contents so the order matches the `sim_preset_t` enum exactly:

```c
static const char *CTRL_LABELS[] = { "PAUSE", "AUTO", "LOW", "MED", "HIGH", "TURBO" };
```

Ordinal order matters: this array is indexed by `sim_preset_t`, so a mismatch sends the wrong preset.

- [ ] **Step 4: Add the total + per-node + mode line to the DECOYS page**

In `draw_stats` (same file), add a row rendering the fleet aggregate alongside the mode and cap. Use the existing `row_kv` helper and match the surrounding call style:

```c
    // Operator thinks in the fleet total; the wire is per-board. Show both so a lagging or
    // throttled node is visible rather than hidden inside an aggregate.
    char crowd[48];
    snprintf(crowd, sizeof crowd, "%u total  %u/node", (unsigned)agg_decoys, (unsigned)per_node);
    row_kv(g, y, "CROWD", crowd);
```

Derive `agg_decoys` from the existing fleet aggregate the page already renders and `per_node` as `agg_decoys / alive_nodes` (guard `alive_nodes >= 1`). Read the surrounding function to use the names already in scope rather than introducing new plumbing.

- [ ] **Step 5: Add cap adjustment to CONTROL**

In `cyd/main/cyd_main.c`, extend the CONTROL page's touch handling so the cap can be stepped alongside the preset selection, persisting `s_cap_total` and sending it with the next signed command (Task 5 Step 3 consumes it). Mirror the existing preset `< / >` zone logic; do not add a new page.

- [ ] **Step 6: Run the radar_audit suite**

```
cd tools/radar_audit && make && python -m pytest tests/ -q
```
Expected: all pass. `test_control.py` and `test_turbo_control.py` reference the old labels - update their expectations to the new enum and note the date and reason in a comment.

- [ ] **Step 7: Build the CYD**

```
cd cyd && idf.py set-target esp32 && idf.py build
```
Expected: `Project build complete`.

- [ ] **Step 8: Commit**

```bash
git add components/simulacra_radar/radar_render.c cyd/main/cyd_main.c tools/radar_audit/tests/
git commit -m "vigil: AUTO/MANUAL labels, cap control, fleet total on the decoys page"
```

---

### Task 7: Detectability scorecard gate

The spec names this the measurement that determines whether additive population costs realism. It is a gate, not a formality: population size feeds `presence_duration` and `address_type_mix`, both previously closed.

**Files:**
- Modify: `private/RSSI-PHYSICAL-TELL.md` is NOT the right home; record results in the plan's own results section below and in `private/PROJECT-MAP.md` §11.

**Interfaces:**
- Consumes: a fully built `tools/decoy_audit` from Tasks 1-3.
- Produces: a before/after headline number recorded in the repo.

- [ ] **Step 1: Capture the BEFORE baseline**

Do this from a checkout at the commit BEFORE Task 1 (`git stash` or a worktree at the pre-change SHA):

```
cd tools/decoy_audit && ./run.ps1 -Rebuild
```
Record the `HEADLINE` value and the named worst tell. The last recorded nominal value was `0.1526` with `ad_structure` as the worst tell - confirm against the actual run rather than assuming.

- [ ] **Step 2: Capture the AFTER number**

Return to the post-Task-3 tree:

```
cd tools/decoy_audit && ./run.ps1 -Rebuild
```

- [ ] **Step 3: Compare and decide**

Record both numbers in `private/PROJECT-MAP.md` §11 with the date. If the headline worsens by more than 0.05, or if `presence_duration` or `address_type_mix` moves above 0.20, STOP and report - additive population is costing measurable realism and the AUTO curve or the cap default needs retuning before hardware work.

- [ ] **Step 4: Commit the record**

`private/` is gitignored, so there is nothing to commit for the record itself. Commit any test-expectation updates the run required:

```bash
git add tools/decoy_audit/
git commit -m "decoy_audit: retune expectations for additive population"
```

---

### Task 8: Hardware verification

**Files:** none - this is bench work. Record results in `private/PROJECT-MAP.md` §11.

- [ ] **Step 1: Flash the entire fleet together**

Wire v2 is breaking; a partial fleet silently splits. Use the provisioned regime flags per `private/FROM-SCRATCH-BUILD-GUIDE.md` §6, or `-Fleet` for the baked demo regime. Every board, same regime, same session.

- [ ] **Step 2: Verify AUTO tracks a room**

Read a decoy's serial for one full re-profile period (10 min on Ward, 5 on Shade). Expected: `reprofile: ... active_target=N (floor F, cap C)` where N moves with the room. Bring devices in and out and confirm N follows on the next tick.

- [ ] **Step 3: Verify MANUAL sticks**

Send LOW from the CYD, then wait past one full re-profile period. Expected: `reprofile: ... (manual mode, crowd unchanged)` and the decoy count does NOT return to the ambient value. This is the regression that `auto_scale` exists to prevent.

- [ ] **Step 4: Verify the cap binds**

Set a low fleet cap on the CYD in a dense environment. Expected: the per-node count clamps to the cap rather than the ambient estimate.

- [ ] **Step 5: Verify additive totals**

With two or more decoys running AUTO in the same room, confirm the CYD's CROWD total is approximately the sum of the per-node counts and does NOT halve when a second board joins - the specific behaviour this whole change exists to produce.

- [ ] **Step 6: Run the on-target self-test**

Build one C5 with `-DCHURN_SELFTEST=1`, flash, and read serial.
Expected: `SELFTEST: PASS (N/N), fails=0`.

- [ ] **Step 7: REQUIRED - Kismet re-capture in a sparse environment**

The spec names this non-optional. Run a Kismet capture at home (device-sparse, the environment H8 was found in) with the full fleet in AUTO, then:

```
cd tools/probe_audit && python probe_behavior_scorecard.py ..\..\private\<new>.kismet
```
Expected: `density_dominance` stays well below 1.0. If it reads 1.0 (clamped over-population), AUTO is not containing the density tell in sparse rooms and the change must be revisited before it is considered done. Record the result in `private/KISMET-VALIDATION.md`.

---

## Self-Review

**Spec coverage:** Population math -> Tasks 1, 3. AUTO/MANUAL modes -> Task 2. Cap semantics (fleet total, CYD divides) -> Tasks 5, 6. `fleet_pop` retention + three guards -> Task 3 (header comment, no-operator-surface rule in the comment, guard test). Wire v2 -> Task 5. NVS bump -> Task 2. Console display -> Task 6. Testing (radar_audit, decoy_audit, churn_selftest, scorecard, hardware) -> Tasks 1, 2, 3, 6, 7, 8. Risks (H8 reopening, Kismet re-capture) -> Task 8 Step 7.

**Two spec corrections** are documented in "Spec Refinements Discovered During Planning" and implemented in Task 2: the persona floor applies to AUTO only, and MANUAL percentages are of the board ceiling rather than `BLE_DEVICES_MAX`.

**Known soft spots**, flagged rather than hidden:
- Task 5 Step 2 and Task 6 Step 5 describe changes to `coexist_request_preset` and the CYD touch handler without quoting their current bodies - both need reading first, and the exact signatures were not verified while writing this plan.
- Task 6 Step 4 uses `agg_decoys` / `per_node` as stand-ins for whatever names `draw_stats` already has in scope.
- Task 1 Step 6 anticipates that some existing decoy_audit expectations may be pinned to the old 16-device clamp; the fix is to retune with measured values, never to re-pin a passing seed (BUG-3's lesson).

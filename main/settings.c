#include "settings.h"
#include "churn.h"
#include "coexist.h"      // coexist_set_turbo(): the TURBO override lives at the coexist tick level
#include "probe.h"        // probe_desired_ble_floor(): this board's designed crowd size
#include "ble_devices.h"  // BLE_DEVICES_MAX
#include "nvs.h"
#include <string.h>

#define SETTINGS_NVS_NS  "sim"
// Key bumped to "settings3" for the 2026-08-24 additive-population change. active_target is stored
// as an absolute count and its scale changed again (no more 1/K division), and the struct gained
// auto_scale/auto_cap which no v2 blob carries -- a zeroed auto_scale would silently pin a board to
// MANUAL at whatever count the old blob held. Abandon it and re-derive defaults, exactly as the
// settings1 -> settings2 migration did for the same class of reason.
#define SETTINGS_NVS_KEY "settings3"

static sim_settings_t s_cur;   // current in-RAM settings (source of truth)

// Preset ceiling = this board's DESIGNED crowd size (personas + unbound companions), not the
// legacy CHURN_ACTIVE_SET.
//
// Those two are different scales and conflating them broke the crowd on hardware: CHURN_ACTIVE_SET
// is 16, while the C5's designed population is 32 (16 personas + 16 unbound). Once
// churn_set_active_target became live again, applying NORMAL at boot shrank the crowd from 32 to
// 16 -- exactly the persona count -- so every slot was persona-bound and the whole BLE crowd became
// company-0x0000 phone shapes with no beacons or tags at all. A pure-phone crowd is a monoculture,
// which is the failure the diversity log exists to catch.
uint8_t sim_settings_ceiling(void)
{
    // No fleet divisor: boards are additive as of 2026-08-24, each sizing itself independently
    // from its own ambient measurement. See the additive-fleet-population design doc.
    int c = probe_desired_ble_floor();
    if (c > BLE_DEVICES_MAX) c = BLE_DEVICES_MAX;
    if (c < SIM_TARGET_FLOOR) c = SIM_TARGET_FLOOR;
    return (uint8_t)c;
}

// Lower bound for AUTO ONLY: enough devices to host this node's designed persona count (personas
// are capped at half the crowd, so N personas need 2N devices). This guards ROOM-driven resizing
// from squeezing out the phones, which are a design constant of the node.
//
// It deliberately does NOT bind a MANUAL level. On the C5 this returns 32 -- identical to the
// ceiling -- so clamping LOW/MED against it would raise them straight back to HIGH, recreating the
// preset collision this design exists to remove. An operator asking for LOW means LOW, and coexist
// already caps personas at crowd/2 so they shrink to fit. See sim_settings_resolve.
uint8_t sim_settings_floor(void)
{
    int f = 2 * probe_phone_target();
    if (f > BLE_DEVICES_MAX) f = BLE_DEVICES_MAX;
    if (f < SIM_TARGET_FLOOR) f = SIM_TARGET_FLOOR;
    return (uint8_t)f;
}

void sim_settings_clamp(sim_settings_t *s, uint8_t floor, uint8_t ceiling)
{
    if (floor < SIM_TARGET_FLOOR) floor = SIM_TARGET_FLOOR;
    if (ceiling < floor) ceiling = floor;
    if (s->active_target < floor) s->active_target = floor;
    if (s->active_target > ceiling) s->active_target = ceiling;
    if (s->accel < 1.0f) s->accel = 1.0f;
    if (s->accel > 4.0f) s->accel = 4.0f;
}

// Presets differ ONLY in knobs the engine actually reads: crowd size, turnover rate, and pause.
// The old dwell/cooldown windows described the roster promote/retire state machine that Milestone
// A replaced with per-device lifetimes; they were still stored and reported after they stopped
// driving anything, which is how the CYD came to display a preset the firmware was not running.
// MANUAL levels clamp against SIM_TARGET_FLOOR, not the persona floor. On the C5 the persona floor
// equals the ceiling (16 personas x2 = 32 = BLE_DEVICES_MAX), so clamping a manual level against it
// would raise LOW and MED straight back to HIGH -- the exact preset collision this design removes
// (STEALTH and NORMAL used to resolve identically for precisely this reason).
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
        s.auto_scale = true; eff_floor = floor; break;      // the re-profile owns active_target
    case SIM_PRESET_LOW:
        s.active_target = (uint8_t)((ceiling * 25) / 100); break;
    case SIM_PRESET_MED:
        s.active_target = (uint8_t)((ceiling * 50) / 100); break;
    case SIM_PRESET_HIGH:
        s.active_target = ceiling; break;
    case SIM_PRESET_TURBO:
        // active_target/accel are irrelevant once turbo=true: sim_settings_match_preset short-
        // circuits on the flag alone, and coexist_set_turbo forces the real population/churn rate
        // directly, bypassing floor/ceiling entirely. That bypass is the whole point of the mode.
        s.turbo = true; break;
    default: return -1;
    }
    sim_settings_clamp(&s, eff_floor, ceiling);
    *out = s;
    return 0;
}

void sim_settings_apply(const sim_settings_t *s)
{
    churn_set_active_target(s->active_target);
    churn_set_paused(s->paused);
    churn_set_accel(s->accel);
    coexist_set_turbo(s->turbo);
    s_cur = *s;
}

static void settings_save(void)
{
    nvs_handle_t h;
    if (nvs_open(SETTINGS_NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;   // best-effort
    nvs_set_blob(h, SETTINGS_NVS_KEY, &s_cur, sizeof s_cur);
    nvs_commit(h); nvs_close(h);
}

void sim_settings_set(const sim_settings_t *s)
{
    // Conditional floor: the persona floor guards AUTO only (see sim_settings_floor).
    sim_settings_t c = *s;
    sim_settings_clamp(&c, c.auto_scale ? sim_settings_floor() : SIM_TARGET_FLOOR,
                       sim_settings_ceiling());
    sim_settings_apply(&c); settings_save();
}

int sim_settings_apply_preset(sim_preset_t p) { return sim_settings_apply_preset_capped(p, 0); }

int sim_settings_apply_preset_capped(sim_preset_t p, uint8_t cap)
{
    sim_settings_t s;
    if (sim_settings_resolve(p, sim_settings_floor(), sim_settings_ceiling(), &s) != 0) return -1;
    // The cap bounds AUTO only: a MANUAL level is the operator naming a number directly, so a
    // second number competing with it would just be ambiguous. Stored regardless so the re-profile
    // sees it the moment AUTO is selected again.
    s.auto_cap = cap;
    if (s.auto_scale && cap && s.active_target > cap) s.active_target = cap;
    sim_settings_apply(&s); settings_save();
    return 0;
}

// Re-clamp the live settings to the CURRENT board bounds and apply if the target moved.
//
// The bounds depend on the live node census (each node runs 1/K of the fleet crowd), and K changes
// as peers are heard or go quiet. Without this the crowd would only resize at the next re-profile
// -- up to 10 minutes on Ward -- so a fleet powering on together would radiate ~K times the
// intended density for that whole window. Deliberately does NOT persist: the census is a runtime
// observation, not an operator choice, and writing it would overwrite the chosen preset in NVS.
void sim_settings_recalc_bounds(void)
{
    sim_settings_t s = s_cur;
    sim_settings_clamp(&s, s.auto_scale ? sim_settings_floor() : SIM_TARGET_FLOOR,
                       sim_settings_ceiling());
    if (s.active_target != s_cur.active_target) sim_settings_apply(&s);
}

void sim_settings_get(sim_settings_t *out) { *out = s_cur; }

sim_preset_t sim_settings_match_preset(const sim_settings_t *cur, uint8_t floor, uint8_t ceiling)
{
    for (sim_preset_t p = SIM_PRESET_PAUSE; p < SIM_PRESET_COUNT; p++) {
        sim_settings_t r;
        if (sim_settings_resolve(p, floor, ceiling, &r) != 0) continue;
        // Turbo is identified by the flag alone. While turbo is running, coexist_set_turbo forces
        // the real population directly (bypassing floor/ceiling), so cur->active_target is NOT the
        // fleet-shared value resolve() computed above -- requiring it to also match would always
        // report CUSTOM instead of TURBO while the mode is genuinely active.
        if (r.turbo != cur->turbo) continue;
        if (cur->turbo) return p;
        // Same reasoning for AUTO: the live active_target is whatever the ROOM dictated, not what
        // resolve() computed, so requiring equality would always report CUSTOM while AUTO runs.
        // PAUSE and AUTO both set auto_scale, so `paused` must be compared BEFORE the early return
        // -- the loop runs in enum order and PAUSE (ordinal 0) would otherwise match a running
        // AUTO board, making the console report a paused fleet that is actually churning.
        if (r.auto_scale != cur->auto_scale) continue;
        if (r.paused != cur->paused) continue;
        if (cur->auto_scale) return p;
        if (r.active_target == cur->active_target && r.accel == cur->accel)
            return p;
    }
    return SIM_PRESET_COUNT;   // CUSTOM
}

sim_preset_t sim_settings_current_preset(void)
{
    return sim_settings_match_preset(&s_cur, sim_settings_floor(), sim_settings_ceiling());
}

bool sim_settings_get_paused(void) { return s_cur.paused; }

bool    sim_settings_auto_scale(void) { return s_cur.auto_scale; }
uint8_t sim_settings_auto_cap(void)   { return s_cur.auto_cap; }

void sim_settings_init(void)
{
    sim_settings_t s;
    nvs_handle_t h; size_t len = sizeof s;
    bool loaded = (nvs_open(SETTINGS_NVS_NS, NVS_READONLY, &h) == ESP_OK) &&
                  (nvs_get_blob(h, SETTINGS_NVS_KEY, &s, &len) == ESP_OK) && len == sizeof s;
    if (loaded) nvs_close(h);
    if (!loaded) sim_settings_resolve(SIM_PRESET_AUTO, sim_settings_floor(), sim_settings_ceiling(), &s);
    sim_settings_clamp(&s, s.auto_scale ? sim_settings_floor() : SIM_TARGET_FLOOR,
                       sim_settings_ceiling());                            // guard a stale blob
    sim_settings_apply(&s);
}

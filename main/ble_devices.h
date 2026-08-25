#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "identity.h"

// Max concurrent persistent devices per board. The runtime population (set at init) is
// clamped to this. Perceived density comes from turnover, not from raising this ceiling.
#define BLE_DEVICES_MAX 32

typedef enum { BLE_ATYPE_STATIC, BLE_ATYPE_RPA, BLE_ATYPE_NRPA } ble_atype_t;
// PERSISTENT = long-lived static "infrastructure" (beacons/fixtures): one address held for hours,
// matching the real ambient >2h presence tail that a purely-churning fleet lacks. Always static
// (only a non-rotating address can actually persist on air).
typedef enum { BLE_ROLE_TRANSIENT, BLE_ROLE_RESIDENT, BLE_ROLE_PERSISTENT } ble_role_t;

typedef struct {
    identity_t  id;             // advertising identity: addr + frozen behaviour (payload/itvl/tx/company/arch)
    ble_atype_t atype;          // fixed for life; selects rotation policy (STATIC never rotates)
    ble_role_t  role;           // fixed for life; selects the lifetime band
    uint32_t    born_ms;        // set at spawn; == now on a fresh birth/rebirth
    uint32_t    life_ms;        // bounded lifetime; on expiry the device dies and is reborn fresh
    uint32_t    next_rotate_ms; // next address rotation (ignored for STATIC)
    // The address this device will rotate TO, drawn in advance so fleetmates can be told about it
    // BEFORE it appears on air. Unused for STATIC (never rotates).
    //
    // Exclusion is by MAC and only covers addresses already broadcast, so a freshly-rotated
    // fleetmate address used to be invisible to peers for up to one broadcast interval (20-30 s).
    // The 2026-08-25 capture showed that window causes two separate failures at once:
    //   1. peers count the unknown address as a REAL ambient device -> population feedback
    //      (fleet-wide 32 -> 65 -> 33 -> 42 over an hour, ambient provably flat throughout)
    //   2. peers MATCH it against the tracker signature DB -> a `tile`-template decoy is a
    //      guaranteed confidence-75 Tile hit (templates.c writes sd[0..1]={0xED,0xFE}; sig_seed.c
    //      matches pattern={0xED,0xFE} at pat_off=0), so the fleet populates its own threat display
    //      in the exact category that display exists to warn about -- and detect_note_known
    //      persists it to NVS, so the false hits outlive reboots.
    // Broadcasting next_addr alongside the current one closes that window to zero.
    uint8_t     next_addr[6];
    bool        alive;
    int8_t   persona_idx;       // >=0: bound to phantom[persona_idx]; -1: unbound BLE-only crowd
    uint32_t persona_gen;       // last phantom generation this bound member synced to
} ble_device_t;

// The address this device will rotate to next, or NULL for a STATIC device (which never rotates).
// Broadcast alongside the live address so peers can exclude it before it is used.
const uint8_t *ble_device_next_addr(int slot);

// Spawn `n` persistent devices (clamped to BLE_DEVICES_MAX). Behaviour is drawn from the
// roster library, so roster_init() MUST have been called first.
void  ble_devices_init(int n, uint32_t now_ms);
// One scheduler tick: retire+reincarnate expired devices, then rotate the address of any
// rotating-subtype device whose next_rotate_ms has passed. Behaviour is preserved across a
// rotation; only addr changes.
void  ble_devices_tick(uint32_t now_ms);
int   ble_devices_count(void);
const ble_device_t *ble_devices_at(int i);
// Live population resize (the runtime population-match knob). Grows by spawning fresh devices,
// shrinks by dropping high slots - but never below the highest persona-bound slot.
void  ble_devices_set_count(int n, uint32_t now_ms);
// Churn acceleration: lifetimes are divided by `mult` (clamped to [1,8]). Applies to devices born
// later AND rescales the remaining life of live unbound devices, so a change takes effect now.
// Idempotent - safe to call every tick with a slowly-decaying value.
void  ble_devices_set_accel(float mult, uint32_t now_ms);
float ble_devices_accel(void);
// TURBO mode: every freshly spawned device (init/grow/respawn-on-expiry) gets a short fixed
// lifetime instead of the normal role/atype-based bands, overriding accel entirely. Only life_ms
// changes -- atype/role/payload are still drawn normally, so identity diversity is unaffected.
// Turning ON also clamps the remaining lifetime of every live unbound device into the turbo band
// (elapsed + a fresh turbo draw) so the switch bites the already-live crowd immediately instead of
// only future spawns; bound (persona) slots are untouched. No equivalent pass on turbo-off: the
// short turbo lives expire on their own within seconds. `now_ms` is only used when turning on.
void  ble_devices_set_turbo(bool on, uint32_t now_ms);
// Shade-form breakdown of the live population by address subtype: restless=RPA (rotating),
// wandering=NRPA (rotating, no resolvable identity), bound=static (never rotates).
void  ble_devices_form_counts(uint8_t *restless, uint8_t *wandering, uint8_t *bound);

// Bind BLE slot `slot` to phantom `persona_idx` (see phantom.h): when the phantom's generation
// advances, reincarnate the slot as an RPA device carrying a Law-3-safe phone advertisement
// (flags-only / 16-bit service-UUID list, no manufacturer data), the phantom's shared born/life,
// and a fresh unique address. `apple` selects the iPhone floor (flags-only, no Continuity).
// Returns 1 if reincarnated. Bound slots do NOT expire via ble_devices_tick; the phantom owns them.
int ble_device_sync(int slot, int persona_idx, bool apple,
                    uint32_t born_ms, uint32_t life_ms, uint32_t generation);

// Release a bound slot back to the unbound crowd (respawned with a fresh identity). Called when
// the persona count shrinks; a slot left bound to a departed persona would never age out.
void ble_device_unbind(int slot, uint32_t now_ms);

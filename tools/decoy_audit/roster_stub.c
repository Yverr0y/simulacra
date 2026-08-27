/* Host stubs for symbols the audit build cannot supply from the firmware sources.
 *
 * The list has shrunk deliberately over time: every symbol stubbed here is one the audit measures a
 * REIMPLEMENTATION of rather than the real thing, so the smallest possible stub set is the goal.
 * learn_* came from the real main/learn.c so self-learned shapes could be measured; rf_model.c is
 * now linked whole so the audit exercises the real rf_adstruct_sample().
 *
 * That leaves NVS, which does not exist on the host.
 *
 * rf_model_load_nvs is NOT a hard failure any more (2026-08-26). It used to return "no model"
 * unconditionally, which quietly invalidated an entire audit run: --persona-pop calls roster_init(),
 * roster_init() asks NVS for a model, got nothing, and fell through to roster_fill_from_templates()
 * -- a pure template fill with NO learned behaviour at all. That run is the ONLY one modelling the
 * real on-air population (bound RPA personas plus the unbound crowd), and it had been reporting the
 * same ad_structure 0.9216 through every change to the learning code, because it never loaded a
 * model to learn from. The number described a path the firmware barely runs.
 *
 * The stub now acts as the host's NVS: synth_dump hands it the model seed, and roster_init picks it
 * up exactly as the firmware would after a reboot.
 */
#include <string.h>
#include <stdbool.h>
#include "rf_model.h"

static rf_model_t s_host_nvs;
static bool       s_host_nvs_valid;

/* Called by synth_dump after load_model_seed(), before anything touches roster_init(). */
void host_nvs_set_model(const rf_model_t *m)
{
    if (!m) { s_host_nvs_valid = false; return; }
    s_host_nvs = *m;
    s_host_nvs_valid = true;
}

int rf_model_load_nvs(rf_model_t *m)
{
    if (!s_host_nvs_valid || !m) return 1;      /* genuinely no stored model */
    *m = s_host_nvs;
    return 0;
}

int rf_model_save_nvs(const rf_model_t *m) { (void)m; return 0; }

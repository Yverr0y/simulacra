/* Host stubs for symbols the audit build cannot supply from the firmware sources.
 *
 * The list has shrunk deliberately over time: every symbol stubbed here is one the audit measures a
 * REIMPLEMENTATION of rather than the real thing, so the smallest possible stub set is the goal.
 * learn_* came from the real main/learn.c so self-learned shapes could be measured; rf_model.c is
 * now linked whole (2026-08-26) so the audit exercises the real rf_adstruct_sample() -- the
 * AD-structure mix is generated code under test, not a stand-in.
 *
 * That leaves only NVS, which does not exist on the host. rf_model.c's own rf_model_load_nvs()
 * calls into the ESP NVS API, so it is excluded from the host build via SIMULACRA_HOST_NO_NVS and
 * replaced here with "no stored model" -- which is also the truth on a host.
 */
#include "rf_model.h"

int rf_model_load_nvs(rf_model_t *m) { (void)m; return 1; }
int rf_model_save_nvs(const rf_model_t *m) { (void)m; return 0; }

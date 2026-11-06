/*
 * stub_rail.c - placeholder for the rail subsystem so the core builds and runs
 * on its own. Delete this file when the real module lands.
 */
#include "internal.h"

/* ---- rail ---- */
RailState *rail_state_new(OfeGame *g) { OFE_UNUSED(g); return NULL; }
void rail_state_copy(OfeGame *dst, const OfeGame *src) { OFE_UNUSED(dst); OFE_UNUSED(src); }
void rail_state_free(OfeGame *g) { OFE_UNUSED(g); }
void rail_tick(OfeGame *g) { OFE_UNUSED(g); }
void rail_on_structure_changed(OfeGame *g, int32_t slot) { OFE_UNUSED(g); OFE_UNUSED(slot); }

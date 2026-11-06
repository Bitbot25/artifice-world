/*
 * stub_nukes.c - placeholder for the nukes subsystem so the core builds and runs
 * on its own. Delete this file when the real module lands.
 */
#include "internal.h"

/* ---- nukes ---- */
NukesState *nukes_state_new(OfeGame *g) { OFE_UNUSED(g); return NULL; }
void nukes_state_copy(OfeGame *dst, const OfeGame *src) { OFE_UNUSED(dst); OFE_UNUSED(src); }
void nukes_state_free(OfeGame *g) { OFE_UNUSED(g); }
void nukes_tick(OfeGame *g) { OFE_UNUSED(g); }
UnitId nukes_launch(OfeGame *g, PlayerId p, int type, TileRef target, int32_t src_slot, bool rocket_up, bool free_launch) {
    OFE_UNUSED(g); OFE_UNUSED(p); OFE_UNUSED(type); OFE_UNUSED(target); OFE_UNUSED(src_slot); OFE_UNUSED(rocket_up);
    OFE_UNUSED(free_launch);
    return -1;
}

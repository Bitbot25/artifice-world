/*
 * stub_structures.c - placeholder for the structures subsystem so the core builds and runs
 * on its own. Delete this file when the real module lands.
 */
#include "internal.h"

/* ---- structures ---- */
StructuresState *structures_state_new(OfeGame *g) { OFE_UNUSED(g); return NULL; }
void structures_state_copy(OfeGame *dst, const OfeGame *src) { OFE_UNUSED(dst); OFE_UNUSED(src); }
void structures_state_free(OfeGame *g) { OFE_UNUSED(g); }
void structures_tick(OfeGame *g) { OFE_UNUSED(g); }
TileRef structures_can_build(const OfeGame *g, PlayerId p, int type, TileRef tile) {
    OFE_UNUSED(g); OFE_UNUSED(p); OFE_UNUSED(type); OFE_UNUSED(tile);
    return -1;
}
UnitId structures_build(OfeGame *g, PlayerId p, int type, TileRef tile, int32_t amount, int32_t flags) {
    OFE_UNUSED(g); OFE_UNUSED(p); OFE_UNUSED(type); OFE_UNUSED(tile); OFE_UNUSED(amount); OFE_UNUSED(flags);
    return -1;
}
bool structures_upgrade(OfeGame *g, PlayerId p, UnitId id, int32_t amount) {
    OFE_UNUSED(g); OFE_UNUSED(p); OFE_UNUSED(id); OFE_UNUSED(amount);
    return false;
}
bool structures_delete(OfeGame *g, PlayerId p, UnitId id) {
    OFE_UNUSED(g); OFE_UNUSED(p); OFE_UNUSED(id);
    return false;
}

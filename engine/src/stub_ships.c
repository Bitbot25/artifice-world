/*
 * stub_ships.c - placeholder for the ships subsystem so the core builds and runs
 * on its own. Delete this file when the real module lands.
 */
#include "internal.h"

/* ---- ships ---- */
ShipsState *ships_state_new(OfeGame *g) { OFE_UNUSED(g); return NULL; }
void ships_state_copy(OfeGame *dst, const OfeGame *src) { OFE_UNUSED(dst); OFE_UNUSED(src); }
void ships_state_free(OfeGame *g) { OFE_UNUSED(g); }
void ships_tick(OfeGame *g) { OFE_UNUSED(g); }
UnitId ships_launch_transport(OfeGame *g, PlayerId p, TileRef dst, double troops) {
    OFE_UNUSED(g); OFE_UNUSED(p); OFE_UNUSED(dst); OFE_UNUSED(troops);
    return -1;
}
bool ships_retreat_transport(OfeGame *g, PlayerId p, UnitId id) { OFE_UNUSED(g); OFE_UNUSED(p); OFE_UNUSED(id); return false; }
UnitId ships_build_warship(OfeGame *g, PlayerId p, TileRef tile) { OFE_UNUSED(g); OFE_UNUSED(p); OFE_UNUSED(tile); return -1; }
bool ships_move_warship(OfeGame *g, PlayerId p, UnitId id, TileRef tile) {
    OFE_UNUSED(g); OFE_UNUSED(p); OFE_UNUSED(id); OFE_UNUSED(tile);
    return false;
}
int32_t ships_water_component(OfeGame *g, TileRef tile) { OFE_UNUSED(g); OFE_UNUSED(tile); return -1; }
TileRef ships_transport_source(OfeGame *g, PlayerId p, TileRef dst) { OFE_UNUSED(g); OFE_UNUSED(p); OFE_UNUSED(dst); return -1; }

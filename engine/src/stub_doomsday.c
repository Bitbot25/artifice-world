/*
 * stub_doomsday.c - placeholder for the doomsday subsystem so the core builds and runs
 * on its own. Delete this file when the real module lands.
 */
#include "internal.h"

/* ---- doomsday ---- */
DoomsdayState *doomsday_state_new(OfeGame *g) { OFE_UNUSED(g); return NULL; }
void doomsday_state_copy(OfeGame *dst, const OfeGame *src) { OFE_UNUSED(dst); OFE_UNUSED(src); }
void doomsday_state_free(OfeGame *g) { OFE_UNUSED(g); }
void doomsday_tick(OfeGame *g) { OFE_UNUSED(g); }
int32_t doomsday_bar_tenths(const OfeGame *g) { OFE_UNUSED(g); return 0; }

/*
 * pathfind_ts.h - OpenFront's water pathfinder, tile for tile (pathfind_ts.c).
 *
 * Only in the TS-parity build (-DOFE_TS_PARITY=1). One immutable TsWater per
 * map (built on first use through map_cache_get, shared by every game and
 * thread on that map). A ship owns a TsWaterStepper, OpenFront's
 * WaterPathFinder.next(): call ts_water_next() once per move and it returns
 * the tile the ship moves to, or says it has arrived / has no route.
 *
 * Needs the real mini map (map4x.bin, or map16x.bin for compact games), which
 * the parity build's ofe_map_load() reads into OfeMap.mini_terrain. Maps made
 * with ofe_map_from_terrain() get an approximate mini map, and their paths
 * differ from OpenFront's.
 *
 * Not supported: water changing during a game (water nukes). The graph is
 * built once and never rebuilt.
 */
#ifndef OFE_PATHFIND_TS_H
#define OFE_PATHFIND_TS_H

#if OFE_TS_PARITY

#include "internal.h"

typedef struct TsWater TsWater;

/* The map's water pathfinder (built on first use). */
const TsWater *ts_water(OfeMap *map);

/* One-shot route, OpenFront's findPath(from | from[], to). Each endpoint is
 * a water tile or a land tile next to water. Returns false when OpenFront
 * returns null; otherwise out holds the route, one tile per ship move,
 * starting at the chosen source and ending at to. With several sources the
 * one TS picks is the first tile of out. */
bool ts_water_find_path(const TsWater *w, const TileRef *from, int nfrom, TileRef to, IntVec *out);

typedef enum TsPathStatus {
    TS_PATH_NEXT = 0,
    TS_PATH_COMPLETE = 2,
    TS_PATH_NOT_FOUND = 3,
} TsPathStatus;

/* A ship's route cache and position in it (PathFinderStepper). Zero-initialise
 * (TsWaterStepper s = {0}) or call ts_water_stepper_init; free with
 * ts_water_stepper_free. Plain data: copy it to clone a game. */
typedef struct TsWaterStepper {
    IntVec path; /* cached route; valid when has_path */
    int32_t index; /* next entry of path to hand out */
    TileRef last_to;
    bool has_path, has_last_to;
} TsWaterStepper;

void ts_water_stepper_init(TsWaterStepper *s);
void ts_water_stepper_free(TsWaterStepper *s);
void ts_water_stepper_copy(TsWaterStepper *dst, const TsWaterStepper *src);
/* Forget the cached route (the next call recomputes it). */
void ts_water_stepper_invalidate(TsWaterStepper *s);

/* WaterPathFinder.next(from, to, dist): `dist` <= 0 means no distance
 * argument. NEXT: *node is the tile to move to. COMPLETE: *node is to (or
 * from when within dist) and the ship has arrived. NOT_FOUND: no route
 * (*node = -1). Call it once per move with the ship's current tile. */
TsPathStatus ts_water_next(const TsWater *w, TsWaterStepper *s, TileRef from, TileRef to, int32_t dist, TileRef *node);

/* The route after the last NEXT, starting at the tile it returned (TS
 * pathAfterNext, used for trade ship motion plans). NULL/0 if none. */
const int32_t *ts_water_stepper_rest(const TsWaterStepper *s, int32_t *len);

/* Mini map graph size (abstract nodes); under 100 TS uses plain A*. */
int32_t ts_water_node_count(const TsWater *w);

#endif /* OFE_TS_PARITY */
#endif

/*
 * pathfind.h - water connectivity and ship routing (pathfind.c).
 *
 * One immutable WaterGraph per map (built lazily through map_cache_get and
 * shared by every game and clone on that map). All queries are pure
 * functions of the terrain, so results never depend on thread timing or on
 * what other games asked before.
 */
#ifndef OFE_PATHFIND_H
#define OFE_PATHFIND_H

#include "internal.h"

typedef struct WaterGraph WaterGraph;

/* The map's water graph (built on first use). */
const WaterGraph *water_graph(OfeMap *map);

int32_t water_graph_num_components(const WaterGraph *wg);
/* Water body of a water tile; for a land tile, of its first 4-neighbour
 * water tile. -1 if neither. */
int32_t water_tile_component(const WaterGraph *wg, const OfeMap *m, TileRef t);
/* Every distinct water body touching t (t itself if water, else its water
 * 4-neighbours). Returns the count (0..4). */
int water_tile_components(const WaterGraph *wg, const OfeMap *m, TileRef t, int32_t out[4]);

#define WATER_MAX_SOURCES 16

/* Finds a ship route to dst from the best of srcs[0..nsrc) (the shortest by
 * the search's cost; nsrc <= WATER_MAX_SOURCES). Each endpoint is a water
 * tile or a land tile touching water. On success out holds every tile of the
 * route, one step per tick, starting at the chosen source and ending at dst,
 * and the index of the chosen source is returned; -1 if there is no route.
 * Single-source routes are memoised on the map. */
int32_t water_find_path(const WaterGraph *wg, const OfeMap *m, const TileRef *srcs, int nsrc, TileRef dst, IntVec *out);

/* Same, but without the detour check (see DETOUR_RATIO in pathfind.c): for
 * "is there a route" probes whose path is not used. */
int32_t water_find_path_quick(const WaterGraph *wg, const OfeMap *m, const TileRef *srcs, int nsrc, TileRef dst, IntVec *out);

#endif

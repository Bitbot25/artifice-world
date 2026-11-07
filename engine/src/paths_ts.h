/*
 * paths_ts.h - OpenFront's missile, air and rail path code, tile for tile
 * (TS-parity build only, see docs/ts_parity.md).
 *
 * Everything here is a pure function of a map (terrain bytes), a few integers
 * and the order in which things are asked, so it can be compared against the
 * TypeScript engine on its own (tools/parity/air_rail_paths.py).
 *
 * What is ported (TS file -> C):
 *   PathFinder.Parabola.ts + utilities/Line.ts   -> TsParabola   (nukes, MIRV, warheads)
 *   PathFinder.Air.ts + PathFinderStepper.ts     -> TsAir         (SAM missiles, shells)
 *   AStar.ts, PriorityQueue.ts (BucketQueue)     -> TsAStar       (generic A*, bucket queue)
 *   AStar.Rail.ts + MiniMapTransformer.ts        -> TsRailPf      (tiles of a railroad)
 *   PathFinder.Station.ts                        -> ts_rail_net_stations_path
 *   RailNetworkImpl/Railroad/RailroadSpatialGrid/TrainStation/Cluster
 *     + UnitGrid.nearbyUnits ordering            -> TsRailNet     (which stations connect, with what tiles)
 *   TrainExecution.ts (tile motion only)         -> TsTrain
 *   PseudoRandom.ts (sfc32 + splitmix32)         -> TsPathRng     (local copy; swap for TsRandom in util.h)
 *
 * Pathfinders take a TsGrid (width, height, OpenFront terrain bytes) instead
 * of an OfeMap so the half-resolution "mini map" the rail pathfinder runs on
 * can be passed in. NOTE: OpenFront's mini map is a separate file (map4x.bin
 * for normal maps, map16x.bin for compact ones); it is NOT the 2x2 reduction
 * that map.c derives. Use ts_mini_terrain_load() (or make map.c do so in the
 * parity build) for exact rails.
 */
#ifndef OFE_PATHS_TS_H
#define OFE_PATHS_TS_H

#if OFE_TS_PARITY

#include "../include/ofe.h"
#include "util.h"

/* ---- Terrain grid --------------------------------------------------------- */

typedef struct TsGrid {
    int32_t w, h;
    const uint8_t *terrain; /* OpenFront terrain byte per tile */
} TsGrid;

/* Grid over an engine map's full-resolution terrain. */
TsGrid ts_grid_from_map(const OfeMap *m);
/* Loads OpenFront's real mini map (map4x.bin when compact == 0, map16x.bin when
 * compact != 0) from a map directory. Returns malloc'ed terrain or NULL;
 * width/height are the game map's (the mini map is (w+1)/2 x (h+1)/2). */
uint8_t *ts_mini_terrain_load(const char *map_dir, int compact, int32_t game_w, int32_t game_h);

static inline int32_t ts_grid_x(const TsGrid *g, int32_t t) { return t % g->w; }
static inline int32_t ts_grid_y(const TsGrid *g, int32_t t) { return t / g->w; }

/* ---- Random: OpenFront's PseudoRandom -------------------------------------- */

typedef struct TsPathRng {
    uint32_t s0, s1, s2, s3;
} TsPathRng;
void ts_path_rng_seed(TsPathRng *r, int64_t seed); /* seed | 0 */
double ts_path_rng_next(TsPathRng *r);             /* [0, 1) */
int32_t ts_path_rng_next_int(TsPathRng *r, int32_t lo, int32_t hi);
bool ts_path_rng_chance(TsPathRng *r, int32_t odds); /* nextInt(0, odds) == 0 */

/* ---- Path results ------------------------------------------------------------ */

typedef enum TsPathStatus { TS_PATH_NEXT = 0, TS_PATH_COMPLETE = 2, TS_PATH_NOT_FOUND = 3 } TsPathStatus;

typedef struct TsPathResult {
    TsPathStatus status;
    int32_t node; /* valid for NEXT and COMPLETE */
} TsPathResult;

/* ---- Parabola (nukes, MIRV, warheads) ------------------------------------------ */

typedef struct TsPt {
    double x, y;
} TsPt;

typedef struct TsParabolaOpts {
    double increment;            /* TS default 3 */
    bool distance_based_height;  /* TS default true */
    bool direction_up;           /* TS default true: rocketDirectionUp */
    bool ignore_map_bounds;      /* TS default false */
} TsParabolaOpts;

/* The TS defaults: increment 3, distance based height, up, clamped to the map. */
TsParabolaOpts ts_parabola_default_opts(void);

/* getParabolaControlPoints: p0, p1, p2, p3. */
void ts_parabola_controls(const TsGrid *map, int32_t from, int32_t to, const TsParabolaOpts *o, TsPt out[4]);
/* DistanceBasedBezierCurve.getLength. */
double ts_bezier_length(const TsPt p[4]);

/* ParabolaUniversalPathFinder: next() walks the curve at `speed` per call. */
typedef struct TsParabola {
    const TsGrid *map;
    TsParabolaOpts opts;
    int32_t last_to;            /* -1 = no curve yet */
    IntVec px, py;              /* cached curve points (coordinates, not tile refs) */
    int32_t spacing_scaled;
    int32_t current_index;
    int64_t accumulated_scaled;
} TsParabola;

void ts_parabola_init(TsParabola *pf, const TsGrid *map, const TsParabolaOpts *opts);
void ts_parabola_free(TsParabola *pf);
/* findPath: the whole trajectory as tile refs (what NukeExecution stores as `trajectory`). */
void ts_parabola_find_path(const TsGrid *map, const TsParabolaOpts *opts, int32_t from, int32_t to, IntVec *out);
/* next(from, to, speed). Pass 1.0 for TS's omitted `speed` (`speed ?? 1`; 0 is NOT the
 * same as omitted: it advances by 1/256 of a tile). COMPLETE carries `to`. */
TsPathResult ts_parabola_next(TsParabola *pf, int32_t from, int32_t to, double speed);
void ts_parabola_invalidate(TsParabola *pf);
int32_t ts_parabola_current_index(const TsParabola *pf);

/* MirvExecution.calculateDeterministicSpeed(spawnTile, separateDst) with the
 * default config (MIRV base speed 15, normalise target 14 ticks). */
double ts_mirv_speed(const TsGrid *map, int32_t spawn, int32_t separate_dst);

/* ---- Air pathfinder (SAM missiles, shells) -------------------------------------- */

/* PathFinding.Air(game): AirPathFinder inside a PathFinderStepper whose equality
 * is plain tile equality. `seed` is game.ticks() when the pathfinder was built
 * (the execution's init tick); every query reseeds from it. */
typedef struct TsAir {
    const TsGrid *map;
    int64_t seed;
    IntVec path;
    int32_t path_index;
    int32_t last_to; /* -1 = none */
    bool has_path;
} TsAir;

void ts_air_init(TsAir *a, const TsGrid *map, int64_t seed);
void ts_air_free(TsAir *a);
/* AirPathFinder.findPath: from..to inclusive, one orthogonal step per tile. */
void ts_air_find_path(const TsGrid *map, int64_t seed, int32_t from, int32_t to, IntVec *out);
/* PathFinderStepper.next(from, to). The target may change between calls. */
TsPathResult ts_air_next(TsAir *a, int32_t from, int32_t to);

/* ---- Generic A* with TS's bucket queue ------------------------------------------- */

typedef struct TsAStarAdapter {
    void *ctx;
    int32_t num_nodes;
    int32_t max_neighbors;
    int32_t max_priority;
    int32_t (*neighbors)(void *ctx, int32_t node, int32_t *buf);
    int32_t (*cost)(void *ctx, int32_t from, int32_t to, int32_t prev); /* prev -1 = undefined */
    int32_t (*heuristic)(void *ctx, int32_t node, int32_t goal);
} TsAStarAdapter;

typedef struct TsAStar TsAStar;
TsAStar *ts_astar_new(const TsAStarAdapter *adapter);
void ts_astar_free(TsAStar *a);
/* AStar.findPath(start, goal); false when no path (or 500k pops). */
bool ts_astar_find_path(TsAStar *a, int32_t start, int32_t goal, IntVec *out);

/* ---- Rail pathfinder ----------------------------------------------------------------- */

/* PathFinding.Rail(game).findPath: A* on the mini map (water penalty 5,
 * direction change penalty 3, heuristic weight 2), upscaled with interpolation
 * and with the end points fixed to exactly `from` and `to`. */
typedef struct TsRailPf TsRailPf;
TsRailPf *ts_rail_pf_new(const TsGrid *game, const TsGrid *mini);
void ts_rail_pf_free(TsRailPf *pf);
/* false = TS returns null (or an empty path). */
bool ts_rail_pf_find_path(TsRailPf *pf, int32_t from, int32_t to, IntVec *out);

/* ---- Rail network (which stations connect, and the railroads' tiles) -------------------- */

/* Structure types the network knows (order is the UnitGrid type order a query lists). */
typedef enum TsStructure { TS_CITY = 0, TS_PORT = 1, TS_FACTORY = 2 } TsStructure;

typedef struct TsRailNet TsRailNet;
/* `game` and `mini` must outlive the network. */
TsRailNet *ts_rail_net_new(const TsGrid *game, const TsGrid *mini);
void ts_rail_net_free(TsRailNet *n);

/* Registers a finished structure in the unit grid (a Unit exists). Returns the
 * unit index (0, 1, ... in call order). No station is created. */
int32_t ts_rail_net_add_structure(TsRailNet *n, TsStructure type, int32_t tile);
/* The execution a finished structure starts: FactoryExecution (always a station,
 * and a station for every structure in range that has none), CityExecution /
 * PortExecution (a station if a factory is within 110 tiles). Each station is
 * connected in the order TS ticks the TrainStationExecutions. Call once per
 * structure, in build order, after ts_rail_net_add_structure. */
void ts_rail_net_structure_done(TsRailNet *n, int32_t unit);

int32_t ts_rail_net_num_stations(const TsRailNet *n);
int32_t ts_rail_net_num_units(const TsRailNet *n);
int32_t ts_rail_net_station_of_unit(const TsRailNet *n, int32_t unit); /* -1 = none */
int32_t ts_rail_net_station_unit(const TsRailNet *n, int32_t station);
int32_t ts_rail_net_station_tile(const TsRailNet *n, int32_t station);
int32_t ts_rail_net_num_railroads(const TsRailNet *n); /* live ones */
/* i-th live railroad in creation order: from/to stations, id and tiles. */
bool ts_rail_net_railroad(const TsRailNet *n, int32_t i, int32_t *from, int32_t *to, int32_t *id, const int32_t **tiles, int32_t *num_tiles);
/* Railroad tiles oriented from station `a` to station `b` (getOrientedRailroad);
 * false if getRailroadTo(b) is null. */
bool ts_rail_net_oriented(const TsRailNet *n, int32_t a, int32_t b, IntVec *tiles_out);
/* Cluster of a station (-1 = none) and a cluster's trade stations (cities and
 * ports) in TS Set order. */
int32_t ts_rail_net_cluster(const TsRailNet *n, int32_t station);
void ts_rail_net_cluster_trade_stations(const TsRailNet *n, int32_t cluster, IntVec *out);
/* Neighbouring stations in TS order (station.neighbors()). */
void ts_rail_net_neighbors(const TsRailNet *n, int32_t station, IntVec *out);
/* StationPathFinder.findPath: station indices from..to, false when TS gives null. */
bool ts_rail_net_stations_path(TsRailNet *n, int32_t from, int32_t to, IntVec *out);

/* ---- Train motion ---------------------------------------------------------------------------- */

typedef struct TsTrain TsTrain;
/* TrainExecution.init for the given station route (>= 2 stations). NULL if TS
 * would deactivate (route too short or a segment without railroad). */
TsTrain *ts_train_new(const TsRailNet *n, const int32_t *stations, int32_t num_stations, int32_t num_cars);
void ts_train_free(TsTrain *t);

#define TS_TRAIN_MAX_UNITS 64
typedef struct TsTrainStep {
    bool active_after;        /* execution still active */
    bool moved;               /* engine moved this tick */
    int32_t engine_tile;      /* valid when moved */
    int32_t num_cars;         /* cars incl. the tail engine */
    int32_t car_tile[TS_TRAIN_MAX_UNITS]; /* car i's tile after this tick (cars[0] is the tail engine) */
    int32_t stop_station;     /* station index reached this tick, -1 if none */
} TsTrainStep;

/* One TrainExecution.tick. `destination_tradable` is stations[1].tradeAvailable(owner). */
void ts_train_tick(TsTrain *t, bool destination_tradable, TsTrainStep *out);

#endif /* OFE_TS_PARITY */
#endif /* OFE_PATHS_TS_H */

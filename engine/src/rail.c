/*
 * rail.c - train stations, railroads and trains.
 *
 * Rules follow OpenFront (RailNetworkImpl, TrainStation, TrainExecution,
 * TrainStationExecution, FactoryExecution, EngineConfig):
 *   - Factories always become stations when finished and pull every finished
 *     city, port and factory within 110 tiles (any owner) into the network.
 *     A city or port finished later becomes a station only if a factory is
 *     within 110 tiles.
 *   - A new station is linked, nearest first, to every station within 110
 *     tiles that is farther than 15 tiles away and not already within 4 rail
 *     hops, provided a rail path shorter than 155 steps exists.
 *   - Each factory level rolls once per tick (after a 10 tick cooldown) with
 *     chance 1 / trainSpawnRate; the rate falls with the world's train count
 *     (trainSaturation) and with the owner's factory count (small economies
 *     get smallEconomySpawnBoost). A spawned train picks a random reachable
 *     city or port its owner may trade with and drives there at 2 tiles per
 *     tick, stopping at every station on the way.
 *   - A stop at a city or port pays trainGold(relation, stops so far): 10k self,
 *     25k team/other, 35k ally, minus 5k per stop after the 9th (floor 5k),
 *     to the train's owner and, if different, the station's owner. A train
 *     whose next station is embargoed (or gone) is removed.
 *
 * Simplifications versus the TypeScript engine:
 *   - No tile-level track. A railroad is a graph edge with a length. Its
 *     length is the Manhattan distance of the two stations (what a 4-connected
 *     A* path costs on open ground), times 1.4 when the straight line crosses
 *     water or impassable ground. The A* is replaced by a connectivity test:
 *     both stations must lie in the same component of the walkable map (land,
 *     plus the 1-2 tile shoreline strips A* may cross), computed once per map.
 *   - New stations do not snap onto passing rails (connectToExistingRails);
 *     they connect to nearby stations by the normal rule instead.
 *   - The station graph is its own cluster: no cluster objects, so nothing to
 *     recompute after removals. A spawn runs one Dijkstra over the factory's
 *     component, picks the destination by reservoir sampling and reads the
 *     route off it (the TS code samples a cluster set, then A*s).
 *   - One unit row per train. aux0 = unit count (engine + tail + 5 cars = 7,
 *     counted by the saturation), aux1 = trade stops so far, aux2 = length of
 *     the edge being driven, auxf = progress along it. path = station unit ids,
 *     path_pos = index of the station it left. The tile is interpolated along
 *     the straight line between the two stations.
 *   - The per-level spawn rolls draw from one splitmix stream, so trains
 *     spawn with the same odds but not the same sequence as TS.
 *   - A station keeps working when its building is captured; its owner is
 *     always read from the unit, as in TS.
 */
#include "internal.h"

#define STATION_MAX_RANGE 110
#define STATION_MIN_RANGE 15
#define MAX_HOPS 4
#define RAILROAD_MAX_SIZE 155.562 /* trainStationMaxRange * 1.4142 */
#define TRAIN_SPEED 2
#define TRAIN_SPAWN_COOLDOWN 10
#define TRAIN_CARS 5
#define TRAIN_UNITS (TRAIN_CARS + 2) /* cars + engine + tail engine */
#define NO_STATION (-1)

typedef struct RailStation {
    UnitId unit; /* -1 = free entry */
    int32_t head; /* first edge in the adjacency list, -1 = none */
    int32_t last_spawn; /* tick of the last train from here */
} RailStation;
OFE_VEC(StationVec, RailStation)

/* An edge sits in two adjacency lists (one per end), linked through
 * next_a / next_b so the whole graph is flat arrays. a < 0 = free entry. */
typedef struct RailEdge {
    int32_t a, b, len, next_a, next_b;
} RailEdge;
OFE_VEC(EdgeVec, RailEdge)

struct RailState {
    StationVec stations;
    IntVec free_stations;
    EdgeVec edges;
    IntVec free_edges;
    IntVec slot_station; /* unit slot -> station index, NO_STATION if none */
    IntVec trains; /* unit ids of live trains */
    int32_t num_stations, num_edges;
    int32_t train_units; /* sum of train unit counts, for the saturation */
    uint64_t rng;

    /* Scratch for searches: never copied. */
    int32_t scratch_cap;
    int32_t *dist, *prev;
    uint32_t *seen, *done;
    uint32_t gen;
    TileHeap heap;
    IntVec queue;
};

RailState *rail_state_new(OfeGame *g) {
    RailState *rs = (RailState *)ofe_xcalloc(1, sizeof(RailState));
    rs->rng = rng_derive(g->seed, 0x7261696cull); /* "rail" */
    return rs;
}

void rail_state_free(OfeGame *g) {
    RailState *rs = g->sub->rail;
    if (!rs) return;
    free(rs->stations.data);
    IntVec_free(&rs->free_stations);
    free(rs->edges.data);
    IntVec_free(&rs->free_edges);
    IntVec_free(&rs->slot_station);
    IntVec_free(&rs->trains);
    free(rs->dist);
    free(rs->prev);
    free(rs->seen);
    free(rs->done);
    heap_free(&rs->heap);
    IntVec_free(&rs->queue);
    free(rs);
    g->sub->rail = NULL;
}

void rail_state_copy(OfeGame *dst, const OfeGame *src) {
    const RailState *s = src->sub->rail;
    if (!dst->sub->rail) dst->sub->rail = rail_state_new(dst);
    RailState *d = dst->sub->rail;
    StationVec_copy(&d->stations, &s->stations);
    IntVec_copy(&d->free_stations, &s->free_stations);
    EdgeVec_copy(&d->edges, &s->edges);
    IntVec_copy(&d->free_edges, &s->free_edges);
    IntVec_copy(&d->slot_station, &s->slot_station);
    IntVec_copy(&d->trains, &s->trains);
    d->num_stations = s->num_stations;
    d->num_edges = s->num_edges;
    d->train_units = s->train_units;
    d->rng = s->rng;
}

/* ---- Walkable map components (immutable, shared by all games on the map) ---- */

typedef struct RailMapCache {
    int32_t *comp; /* per tile; -1 = not walkable */
} RailMapCache;

/* Where A* may lay track: land that is not impassable, plus water tiles next
 * to land (a river or strait of 1-2 tiles can be crossed). */
static bool tile_walkable(const OfeMap *m, TileRef t) {
    if (map_is_land(m, t)) return !map_is_impassable(m, t);
    return map_is_shoreline(m, t);
}

static void *rail_cache_build(const OfeMap *m) {
    RailMapCache *c = (RailMapCache *)ofe_xmalloc(sizeof(*c));
    c->comp = (int32_t *)ofe_xmalloc((size_t)m->num_tiles * sizeof(int32_t));
    for (TileRef t = 0; t < m->num_tiles; t++) c->comp[t] = -1;
    IntVec stack = {0};
    int32_t next = 0;
    for (TileRef start = 0; start < m->num_tiles; start++) {
        if (c->comp[start] >= 0 || !tile_walkable(m, start)) continue;
        c->comp[start] = next;
        stack.len = 0;
        IntVec_push(&stack, start);
        while (stack.len > 0) {
            TileRef t = stack.data[--stack.len];
            TileRef nb[4];
            int n = map_neighbors4(m, t, nb);
            for (int i = 0; i < n; i++) {
                if (c->comp[nb[i]] < 0 && tile_walkable(m, nb[i])) {
                    c->comp[nb[i]] = next;
                    IntVec_push(&stack, nb[i]);
                }
            }
        }
        next++;
    }
    IntVec_free(&stack);
    return c;
}

static void rail_cache_free(void *p) {
    RailMapCache *c = (RailMapCache *)p;
    if (!c) return;
    free(c->comp);
    free(c);
}

static const RailMapCache *map_cache(OfeGame *g) {
    return (const RailMapCache *)map_cache_get(g->map, MAP_CACHE_RAIL, rail_cache_build, rail_cache_free);
}

/* ---- Small helpers --------------------------------------------------------- */

static bool is_station_type(int type) { return type == OFE_UNIT_CITY || type == OFE_UNIT_PORT || type == OFE_UNIT_FACTORY; }

static int32_t station_of_slot(const RailState *rs, int32_t slot) {
    return slot >= 0 && slot < rs->slot_station.len ? rs->slot_station.data[slot] : NO_STATION;
}

static void set_station_of_slot(RailState *rs, int32_t slot, int32_t idx) {
    while (rs->slot_station.len <= slot) IntVec_push(&rs->slot_station, NO_STATION);
    rs->slot_station.data[slot] = idx;
}

/* Unit slot of station idx, -1 if its unit is gone. */
static int32_t station_slot(const OfeGame *g, const RailState *rs, int32_t idx) {
    return unit_lookup(g, rs->stations.data[idx].unit);
}

static void ensure_scratch(RailState *rs) {
    int32_t need = rs->stations.len;
    if (rs->scratch_cap >= need) return;
    int32_t cap = OFE_MAX(need, 64);
    rs->dist = (int32_t *)ofe_xrealloc(rs->dist, (size_t)cap * sizeof(int32_t));
    rs->prev = (int32_t *)ofe_xrealloc(rs->prev, (size_t)cap * sizeof(int32_t));
    rs->seen = (uint32_t *)ofe_xrealloc(rs->seen, (size_t)cap * sizeof(uint32_t));
    rs->done = (uint32_t *)ofe_xrealloc(rs->done, (size_t)cap * sizeof(uint32_t));
    memset(rs->seen + rs->scratch_cap, 0, (size_t)(cap - rs->scratch_cap) * sizeof(uint32_t));
    memset(rs->done + rs->scratch_cap, 0, (size_t)(cap - rs->scratch_cap) * sizeof(uint32_t));
    rs->scratch_cap = cap;
}

static uint32_t next_gen(RailState *rs) {
    if (++rs->gen == 0) {
        memset(rs->seen, 0, (size_t)rs->scratch_cap * sizeof(uint32_t));
        memset(rs->done, 0, (size_t)rs->scratch_cap * sizeof(uint32_t));
        rs->gen = 1;
    }
    return rs->gen;
}

/* The other end of edge e as seen from station s, and the next edge of s. */
static inline int32_t edge_other(const RailEdge *e, int32_t s) { return e->a == s ? e->b : e->a; }
static inline int32_t edge_next(const RailEdge *e, int32_t s) { return e->a == s ? e->next_a : e->next_b; }

/* ---- Graph editing -------------------------------------------------------------- */

static void edge_add(RailState *rs, int32_t a, int32_t b, int32_t len) {
    int32_t i;
    if (rs->free_edges.len > 0) {
        i = rs->free_edges.data[--rs->free_edges.len];
    } else {
        RailEdge blank = {-1, -1, 0, -1, -1};
        EdgeVec_push(&rs->edges, blank);
        i = rs->edges.len - 1;
    }
    RailEdge *e = &rs->edges.data[i];
    e->a = a;
    e->b = b;
    e->len = len;
    e->next_a = rs->stations.data[a].head;
    e->next_b = rs->stations.data[b].head;
    rs->stations.data[a].head = i;
    rs->stations.data[b].head = i;
    rs->num_edges++;
}

/* Removes edge i from station s's adjacency list. */
static void adjacency_unlink(RailState *rs, int32_t s, int32_t i) {
    int32_t *link = &rs->stations.data[s].head;
    while (*link != i) {
        RailEdge *e = &rs->edges.data[*link];
        link = e->a == s ? &e->next_a : &e->next_b;
    }
    *link = edge_next(&rs->edges.data[i], s);
}

static void station_remove(RailState *rs, int32_t idx) {
    RailStation *st = &rs->stations.data[idx];
    while (st->head >= 0) {
        int32_t i = st->head;
        RailEdge *e = &rs->edges.data[i];
        int32_t other = edge_other(e, idx);
        st->head = edge_next(e, idx);
        adjacency_unlink(rs, other, i);
        e->a = e->b = -1;
        IntVec_push(&rs->free_edges, i);
        rs->num_edges--;
    }
    st->unit = -1;
    IntVec_push(&rs->free_stations, idx);
    rs->num_stations--;
}

/* Marks every station within `hops` rail hops of start (itself included) with
 * the returned generation in rs->seen. */
static uint32_t stations_within(RailState *rs, int32_t start, int32_t hops) {
    uint32_t gen = next_gen(rs);
    rs->queue.len = 0;
    rs->seen[start] = gen;
    rs->dist[start] = 0;
    IntVec_push(&rs->queue, start);
    for (int32_t head = 0; head < rs->queue.len; head++) {
        int32_t s = rs->queue.data[head];
        if (rs->dist[s] >= hops) continue;
        for (int32_t i = rs->stations.data[s].head; i >= 0; i = edge_next(&rs->edges.data[i], s)) {
            int32_t o = edge_other(&rs->edges.data[i], s);
            if (rs->seen[o] == gen) continue;
            rs->seen[o] = gen;
            rs->dist[o] = rs->dist[s] + 1;
            IntVec_push(&rs->queue, o);
        }
    }
    return gen;
}

/* Estimated track length between two tiles, or -1 if no rail path would be
 * short enough (or none exists). See the header comment. */
static int32_t rail_path_length(OfeGame *g, TileRef a, TileRef b) {
    const OfeMap *m = g->map;
    const RailMapCache *mc = map_cache(g);
    if (mc->comp[a] < 0 || mc->comp[a] != mc->comp[b]) return -1;
    int32_t len = map_manhattan(m, a, b);
    int32_t ax = map_x(m, a), ay = map_y(m, a), dx = map_x(m, b) - ax, dy = map_y(m, b) - ay;
    int32_t steps = OFE_MAX(abs(dx), abs(dy)) / 3 + 1;
    for (int32_t k = 1; k < steps; k++) {
        TileRef t = map_ref(m, ax + dx * k / steps, ay + dy * k / steps);
        if (!map_is_land(m, t) || map_is_impassable(m, t)) {
            len = len * 7 / 5;
            break;
        }
    }
    return len < RAILROAD_MAX_SIZE ? len : -1;
}

typedef struct Candidate {
    int64_t d2;
    int32_t idx;
} Candidate;

static int candidate_cmp(const void *x, const void *y) {
    const Candidate *a = (const Candidate *)x, *b = (const Candidate *)y;
    if (a->d2 != b->d2) return a->d2 < b->d2 ? -1 : 1;
    return a->idx - b->idx;
}

/* connectToNearbyStations: link station idx to nearby stations, nearest first,
 * skipping those it can already reach in a few hops. */
static void station_connect(OfeGame *g, int32_t idx) {
    RailState *rs = g->sub->rail;
    const UnitTable *t = &g->units;
    TileRef tile = t->tile[station_slot(g, rs, idx)];
    int64_t max2 = (int64_t)STATION_MAX_RANGE * STATION_MAX_RANGE;
    int64_t min2 = (int64_t)STATION_MIN_RANGE * STATION_MIN_RANGE;

    Candidate *cand = (Candidate *)ofe_xmalloc((size_t)OFE_MAX(rs->stations.len, 1) * sizeof(Candidate));
    int32_t n = 0;
    for (int32_t i = 0; i < rs->stations.len; i++) {
        if (i == idx || rs->stations.data[i].unit < 0) continue;
        int32_t slot = station_slot(g, rs, i);
        if (slot < 0) continue;
        int64_t d2 = map_dist2(g->map, tile, t->tile[slot]);
        if (d2 <= max2) {
            cand[n].d2 = d2;
            cand[n].idx = i;
            n++;
        }
    }
    qsort(cand, (size_t)n, sizeof(Candidate), candidate_cmp);

    ensure_scratch(rs);
    uint32_t gen = stations_within(rs, idx, MAX_HOPS);
    for (int32_t k = 0; k < n; k++) {
        int32_t o = cand[k].idx;
        if (rs->seen[o] == gen || cand[k].d2 <= min2) continue;
        int32_t len = rail_path_length(g, tile, t->tile[station_slot(g, rs, o)]);
        if (len < 0) continue;
        edge_add(rs, idx, o, len);
        gen = stations_within(rs, idx, MAX_HOPS);
    }
    free(cand);
}

/* Makes the unit in `slot` a station and wires it in. Returns its index. */
static int32_t station_add(OfeGame *g, int32_t slot) {
    RailState *rs = g->sub->rail;
    int32_t idx;
    if (rs->free_stations.len > 0) {
        idx = rs->free_stations.data[--rs->free_stations.len];
    } else {
        RailStation blank = {-1, -1, 0};
        StationVec_push(&rs->stations, blank);
        idx = rs->stations.len - 1;
    }
    RailStation *st = &rs->stations.data[idx];
    st->unit = unit_id_of_slot(g, slot);
    st->head = -1;
    st->last_spawn = g->tick;
    set_station_of_slot(rs, slot, idx);
    rs->num_stations++;
    ensure_scratch(rs);
    station_connect(g, idx);
    return idx;
}

/* A new factory pulls in every finished station-capable structure around it
 * (nearest first), as FactoryExecution does. */
static void factory_promote_neighbors(OfeGame *g, int32_t factory_slot) {
    RailState *rs = g->sub->rail;
    const UnitTable *t = &g->units;
    int64_t max2 = (int64_t)STATION_MAX_RANGE * STATION_MAX_RANGE;
    Candidate *cand = (Candidate *)ofe_xmalloc((size_t)OFE_MAX(t->count, 1) * sizeof(Candidate));
    int32_t n = 0;
    for (int32_t s = 0; s < t->count; s++) {
        if (!t->alive[s] || s == factory_slot || !is_station_type(t->type[s])) continue;
        if (t->flags[s] & OFE_UF_UNDER_CONSTRUCTION) continue;
        if (station_of_slot(rs, s) != NO_STATION) continue;
        int64_t d2 = map_dist2(g->map, t->tile[factory_slot], t->tile[s]);
        if (d2 <= max2) {
            cand[n].d2 = d2;
            cand[n].idx = s;
            n++;
        }
    }
    qsort(cand, (size_t)n, sizeof(Candidate), candidate_cmp);
    for (int32_t k = 0; k < n; k++) station_add(g, cand[k].idx);
    free(cand);
}

void rail_on_structure_changed(OfeGame *g, int32_t slot) {
    RailState *rs = g->sub->rail;
    if (!rs) return;
    const UnitTable *t = &g->units;
    int32_t idx = station_of_slot(rs, slot);
    if (!t->alive[slot]) {
        if (idx != NO_STATION) {
            station_remove(rs, idx);
            set_station_of_slot(rs, slot, NO_STATION);
        }
        return;
    }
    /* Captured: the station stays, its owner is read from the unit. */
    if (idx != NO_STATION || (t->flags[slot] & OFE_UF_UNDER_CONSTRUCTION) || !is_station_type(t->type[slot])) return;

    if (t->type[slot] == OFE_UNIT_FACTORY) {
        station_add(g, slot);
        factory_promote_neighbors(g, slot);
    } else if (units_has_nearby(g, t->tile[slot], STATION_MAX_RANGE, OFE_UNIT_FACTORY, 0)) {
        station_add(g, slot);
    }
}

/* ---- Economy rules ---------------------------------------------------------------- */

static double train_saturation(double trains) {
    double damping = 1.0 - sigmoid(trains, M_LN2 / 100.0, 560.0);
    double plateau = 0.25 * (1.0 - sigmoid(trains, M_LN2 / 150.0, 900.0));
    return OFE_MAX(damping, plateau);
}

/* Per-unit spawn multiplier for a small economy (levels = factory count). */
static double small_economy_boost(int32_t levels) {
    if (levels <= 0) return 1.0;
    double f = OFE_CLAMP((6.0 - levels) / 5.0, 0.0, 1.0);
    return 1.0 + 0.5 * f / levels;
}

static int32_t train_spawn_rate(int32_t player_factories, double saturation) {
    double rate = (player_factories + 10) * 15.0;
    double r = floor(rate / (saturation * small_economy_boost(player_factories)));
    return r < 1.0 ? 1 : (int32_t)r;
}

static int64_t train_gold(const OfeGame *g, int rel, int32_t stops_visited) {
    int64_t base = rel == 0 ? 10000 : rel == 2 ? 35000 : 25000; /* self, ally, team/other */
    int64_t penalty = (int64_t)OFE_MAX(0, stops_visited - 9) * 5000;
    int64_t gold = OFE_MAX((int64_t)5000, base - penalty);
    return (int64_t)((double)gold * g->cfg.gold_multiplier);
}

/* 0 self, 1 team, 2 ally, 3 other. */
static int relation_of(const OfeGame *g, PlayerId train_owner, PlayerId station_owner) {
    if (train_owner == station_owner) return 0;
    if (player_same_team(g, train_owner, station_owner)) return 1;
    if (player_allied(g, train_owner, station_owner)) return 2;
    return 3;
}

static bool trade_available(const OfeGame *g, PlayerId train_owner, PlayerId station_owner) {
    return station_owner == train_owner || player_can_trade(g, train_owner, station_owner);
}

/* ---- Trains ----------------------------------------------------------------------------- */

static void train_remove(OfeGame *g, RailState *rs, UnitId id, int32_t slot) {
    rs->train_units -= g->units.aux0[slot];
    unit_destroy(g, id, 0);
}

static void train_place(OfeGame *g, int32_t slot, TileRef from, TileRef to, double frac) {
    const OfeMap *m = g->map;
    int32_t x = map_x(m, from) + (int32_t)floor((map_x(m, to) - map_x(m, from)) * frac + 0.5);
    int32_t y = map_y(m, from) + (int32_t)floor((map_y(m, to) - map_y(m, from)) * frac + 0.5);
    unit_move(g, slot, map_ref(m, x, y));
}

/* Length of the edge joining two adjacent stations, -1 if none. */
static int32_t edge_length_between(const RailState *rs, int32_t a, int32_t b) {
    for (int32_t i = rs->stations.data[a].head; i >= 0; i = edge_next(&rs->edges.data[i], a)) {
        if (edge_other(&rs->edges.data[i], a) == b) return rs->edges.data[i].len;
    }
    return -1;
}

/* Station index for a unit id stored in a train's route, -1 if gone. */
static int32_t route_station(const OfeGame *g, const RailState *rs, UnitId id) {
    int32_t slot = unit_lookup(g, id);
    return slot < 0 ? NO_STATION : station_of_slot(rs, slot);
}

/* Train arrives at the station of unit `to_slot`: pay out. */
static void train_stop(OfeGame *g, int32_t train_slot, int32_t to_slot) {
    UnitTable *t = &g->units;
    int type = t->type[to_slot];
    if (type != OFE_UNIT_CITY && type != OFE_UNIT_PORT) return;
    PlayerId owner = t->owner[train_slot], station_owner = t->owner[to_slot];
    int64_t gold = train_gold(g, relation_of(g, owner, station_owner), t->aux1[train_slot]);
    if (station_owner != owner) {
        player_add_gold(g, station_owner, gold);
        g->players.train_gold[station_owner] += gold;
    }
    player_add_gold(g, owner, gold);
    g->players.train_gold[owner] += gold;
    t->aux1[train_slot]++;
}

/* Moves one train. Returns false if it was removed. */
static bool train_step(OfeGame *g, RailState *rs, UnitId id) {
    int32_t slot = unit_lookup(g, id);
    if (slot < 0) return false;
    UnitTable *t = &g->units;
    const IntVec *route = &t->path[slot];
    int32_t pos = t->path_pos[slot];
    int32_t from_slot = unit_lookup(g, route->data[pos]);
    int32_t to_slot = unit_lookup(g, route->data[pos + 1]);
    if (from_slot < 0 || to_slot < 0 || station_of_slot(rs, from_slot) == NO_STATION ||
        station_of_slot(rs, to_slot) == NO_STATION || !trade_available(g, t->owner[slot], t->owner[to_slot])) {
        train_remove(g, rs, id, slot);
        return false;
    }
    t->auxf[slot] += TRAIN_SPEED;
    if (t->auxf[slot] < t->aux2[slot]) {
        train_place(g, slot, t->tile[from_slot], t->tile[to_slot], t->auxf[slot] / t->aux2[slot]);
        return true;
    }
    /* Reached the next station. */
    double left = t->auxf[slot] - t->aux2[slot];
    train_stop(g, slot, to_slot);
    pos++;
    t->path_pos[slot] = pos;
    unit_move(g, slot, t->tile[to_slot]);
    if (pos + 1 >= route->len) {
        train_remove(g, rs, id, slot);
        return false;
    }
    int32_t a = station_of_slot(rs, to_slot);
    int32_t b = route_station(g, rs, route->data[pos + 1]);
    int32_t len = b == NO_STATION ? -1 : edge_length_between(rs, a, b);
    if (len < 0) {
        train_remove(g, rs, id, slot);
        return false;
    }
    t->aux2[slot] = len;
    t->auxf[slot] = left;
    return true;
}

/* Dijkstra from `src` over its connected stations; picks a random city or
 * port `owner` may trade with (reservoir sampling) and returns its index, or
 * -1. rs->prev holds the route back to src. */
static int32_t pick_destination(OfeGame *g, RailState *rs, int32_t src, PlayerId owner) {
    ensure_scratch(rs);
    uint32_t gen = next_gen(rs);
    rs->heap.len = 0;
    rs->dist[src] = 0;
    rs->prev[src] = -1;
    rs->seen[src] = gen;
    heap_push(&rs->heap, src, 0.0f);
    int32_t chosen = -1, eligible = 0;
    while (rs->heap.len > 0) {
        int32_t s = heap_pop(&rs->heap);
        if (rs->done[s] == gen) continue;
        rs->done[s] = gen;
        int32_t slot = station_slot(g, rs, s);
        int type = g->units.type[slot];
        if ((type == OFE_UNIT_CITY || type == OFE_UNIT_PORT) && trade_available(g, owner, g->units.owner[slot])) {
            eligible++;
            if (rng_int(&rs->rng, 0, eligible) == 0) chosen = s;
        }
        for (int32_t i = rs->stations.data[s].head; i >= 0; i = edge_next(&rs->edges.data[i], s)) {
            const RailEdge *e = &rs->edges.data[i];
            int32_t o = edge_other(e, s), d = rs->dist[s] + e->len;
            if (rs->seen[o] == gen && d >= rs->dist[o]) continue;
            rs->seen[o] = gen;
            rs->dist[o] = d;
            rs->prev[o] = s;
            heap_push(&rs->heap, o, (float)d);
        }
    }
    return chosen;
}

static void train_spawn(OfeGame *g, RailState *rs, int32_t src, int32_t dst) {
    /* Route src -> dst from the prev chain, built backwards then reversed. */
    rs->queue.len = 0;
    for (int32_t s = dst; s >= 0; s = rs->prev[s]) IntVec_push(&rs->queue, s);
    int32_t hops = rs->queue.len;
    if (hops < 2) return;
    int32_t src_slot = station_slot(g, rs, src);
    UnitId id = unit_create(g, OFE_UNIT_TRAIN, g->units.owner[src_slot], g->units.tile[src_slot]);
    if (id < 0) return;
    int32_t slot = unit_lookup(g, id);
    UnitTable *t = &g->units;
    IntVec *route = &t->path[slot];
    for (int32_t k = hops - 1; k >= 0; k--) {
        IntVec_push(route, rs->stations.data[rs->queue.data[k]].unit);
    }
    int32_t first_len = edge_length_between(rs, src, rs->queue.data[hops - 2]);
    t->target_unit[slot] = rs->stations.data[dst].unit;
    t->target_tile[slot] = t->tile[station_slot(g, rs, dst)];
    t->aux0[slot] = TRAIN_UNITS;
    t->aux1[slot] = 0;
    t->aux2[slot] = OFE_MAX(first_len, 1);
    t->auxf[slot] = 0;
    t->path_pos[slot] = 0;
    rs->train_units += TRAIN_UNITS;
    IntVec_push(&rs->trains, id);
}

void rail_tick(OfeGame *g) {
    RailState *rs = g->sub->rail;
    if (!rs || (rs->num_stations == 0 && rs->trains.len == 0)) return;

    /* Move trains, dropping the dead. */
    int32_t kept = 0;
    for (int32_t i = 0; i < rs->trains.len; i++) {
        UnitId id = rs->trains.data[i];
        if (train_step(g, rs, id)) rs->trains.data[kept++] = id;
    }
    rs->trains.len = kept;
    if (g->cfg.disabled_units & (1u << OFE_UNIT_TRAIN)) return;

    /* Factories roll for new trains. */
    double saturation = -1.0;
    for (int32_t s = 0; s < rs->stations.len; s++) {
        RailStation *st = &rs->stations.data[s];
        if (st->unit < 0 || g->tick < st->last_spawn + TRAIN_SPAWN_COOLDOWN) continue;
        int32_t slot = unit_lookup(g, st->unit);
        if (slot < 0 || g->units.type[slot] != OFE_UNIT_FACTORY) continue;
        PlayerId owner = g->units.owner[slot];
        if (!player_alive(g, owner)) continue;
        if (saturation < 0) saturation = train_saturation(rs->train_units);
        int32_t rate = train_spawn_rate(g->players.unit_count[owner][OFE_UNIT_FACTORY], saturation);
        bool roll = false;
        for (int32_t lvl = 0; lvl < g->units.level[slot] && !roll; lvl++) roll = rng_int(&rs->rng, 0, rate) == 0;
        if (!roll) continue;
        int32_t dst = pick_destination(g, rs, s, owner);
        if (dst < 0 || dst == s) continue;
        st->last_spawn = g->tick;
        train_spawn(g, rs, s, dst);
    }
}

/* ---- Queries (tests and feature extraction) ---------------------------------------------- */

int32_t rail_station_count(const OfeGame *g) { return g->sub->rail ? g->sub->rail->num_stations : 0; }
int32_t rail_edge_count(const OfeGame *g) { return g->sub->rail ? g->sub->rail->num_edges : 0; }

int32_t rail_station_owner(const OfeGame *g, UnitId id) {
    int32_t slot = unit_lookup(g, id);
    if (slot < 0 || station_of_slot(g->sub->rail, slot) == NO_STATION) return -1;
    return g->units.owner[slot];
}

bool rail_connected(const OfeGame *g, UnitId a, UnitId b) {
    RailState *rs = g->sub->rail;
    int32_t sa = route_station(g, rs, a), sb = route_station(g, rs, b);
    if (sa == NO_STATION || sb == NO_STATION) return false;
    ensure_scratch(rs);
    uint32_t gen = stations_within(rs, sa, g->sub->rail->stations.len);
    return rs->seen[sb] == gen;
}

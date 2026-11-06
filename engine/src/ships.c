/*
 * ships.c - transport ships, trade ships, warships and shells.
 *
 * Rules and constants follow OpenFront's TransportShipExecution,
 * TradeShipExecution, PortExecution, WarshipExecution, MoveWarshipExecution
 * and ShellExecution. Water bodies and routing are in pathfind.c.
 *
 * Speeds (as in OpenFront): a ship moves one path tile per tick (transport,
 * trade, warship; a warship chasing a trade ship moves two); a shell moves
 * three tiles per tick.
 *
 * Unit columns, by type (everything not listed is unused):
 *
 *   TRANSPORT_SHIP  troops = troops on board
 *                   src_tile = shore tile it left from
 *                   target_tile = landing shore tile
 *                   path / path_pos = route from src_tile to target_tile
 *                   flags OFE_UF_RETREATING = cancelled, sailing home
 *                   aux0 = 1 once the route has been reversed for the retreat
 *
 *   TRADE_SHIP      target_unit = destination port (id)
 *                   src_tile = the origin port's tile
 *                   path / path_pos = route; target_tile = destination tile
 *                   aux0 = tiles travelled (sets the payout)
 *                   aux1 = origin port (id), aux2 = original owner
 *                   aux3 = last tick it was in shallow water (pirate-safe
 *                          for 20 ticks afterwards)
 *                   timer = 1 once it has ever been captured
 *
 *   WARSHIP         health, aux0 = patrol centre tile + 1 (0 = where it is)
 *                   target_tile = the tile it is sailing to (patrol leg or
 *                          retreat port), -1 = none; path / path_pos = route
 *                   target_unit = ship it is shooting / hunting
 *                   aux1 = repair port tile + 1 (0 = none)
 *                   aux2 = tick of the last shell (reload)
 *                   aux3 = veterancy (bits 0-1) | state (bits 2-3: 0 patrol,
 *                          1 retreating, 2 docked) | veterancy progress
 *                          (bits 4-12)
 *                   timer = tick of the last manual move order
 *                   auxf = fractional dock healing carried over
 *
 *   SHELL           target_unit = unit it is chasing, target_tile = its last
 *                          known tile
 *                   aux0 = base damage, aux1 = damage bonus in percent,
 *                   aux2 = firing warship (id, -1 none)
 *                   timer = tick the shell expires (-1 = not yet, set once
 *                          the firing warship is gone)
 *
 * Simplifications versus OpenFront:
 *   - Routes come from pathfind.c (weighted A* on the half-resolution map,
 *     string-pulled, expanded to one tile per tick), not OpenFront's
 *     hierarchical A*. Straight-line stretches take the same number of ticks.
 *   - A cancelled transport ship retraces its own route back to its source
 *     shore instead of searching for the nearest own shore from where it is.
 *     The 25% troop loss on return is kept.
 *   - A transport ship whose route cannot be found is never launched (no
 *     troops are taken), instead of launching and returning them.
 *   - The trade-ship pity timer is kept per port (in ShipsState, indexed by
 *     unit slot) as in OpenFront, not in players.trade_spawn_rejections,
 *     which is left unused. Ports check every 10 ticks, offset by their slot.
 *   - Only finished ports are trade partners; piracy gold is paid like trade
 *     gold (no separate piracy statistic).
 *   - A warship remembers no list of "already shelled" ships: it skips ships
 *     that already have one of its shells in flight, which has the same
 *     effect for units that die in one hit.
 *   - Warship repair retreat, port capacity and docking follow OpenFront but
 *     with simpler port selection (nearest by straight line on the same
 *     water body).
 *   - Shells fly in a deterministic staircase line (the dominant axis first)
 *     instead of OpenFront's random choice between the two axes.
 *   - Water bodies are 4-connected labels capped at 65535 per map.
 *
 * Randomness: every draw comes from a splitmix64 stream derived from the
 * game seed, the tick and the unit slot, so no state is carried between ticks.
 */
#include "pathfind.h"

/* ---- Constants (EngineConfig.ts) ------------------------------------------- */
#define BOAT_MAX_NUMBER 3
#define RETREAT_MALUS 0.25
#define TRADE_SHORT_RANGE 300
#define SAFE_FROM_PIRATES_TICKS 20
#define TRADE_CHECK_PERIOD 10
#define LANDING_SEARCH_RANGE 50
#define SOURCE_CANDIDATES 12

#define WARSHIP_MAX_HEALTH 1000
#define WARSHIP_PATROL_RANGE 100
#define WARSHIP_TARGET_RANGE 130
#define WARSHIP_SHELL_RELOAD 20
#define WARSHIP_DOCKING_RANGE 5
#define WARSHIP_PORT_HEAL_PER_LEVEL 5
#define WARSHIP_RETREAT_PERCENT 75
#define WARSHIP_PASSIVE_HEAL 1
#define WARSHIP_PASSIVE_HEAL_RANGE 150
#define WARSHIP_PORT_SWITCH 0.75
#define WARSHIP_MAX_VETERANCY 3
#define WARSHIP_VET_HEALTH_PERCENT 20
#define WARSHIP_VET_DAMAGE_PERCENT 20
#define WARSHIP_VET_TRANSPORT_KILLS 10
#define WARSHIP_VET_TRADE_CAPTURES 25
#define WARSHIP_MANUAL_RETREAT_PAUSE 50
#define WARSHIP_CAPTURE_DIST 5
#define WARSHIP_GREEDY_DIST 20

#define SHELL_BASE_DAMAGE 250
#define SHELL_SPEED 3
#define SHELL_LIFETIME 50

enum { WS_PATROL = 0, WS_RETREAT = 1, WS_DOCKED = 2 };
enum { MOVE_FAILED = -1, MOVE_STEPPED = 0, MOVE_ARRIVED = 1 };

/* ---- State ------------------------------------------------------------------ */

typedef struct PortCand {
    int32_t slot, dist, level;
} PortCand;

struct ShipsState {
    /* Logical state: trade spawn pity timers, per unit slot. */
    int32_t cap;
    int16_t *port_rej;
    uint16_t *port_gen; /* generation the counter belongs to */

    /* Scratch, rebuilt as needed; never copied. */
    IntVec ports, ships, shells; /* unit slots, rebuilt every tick */
    IntVec route; /* route under construction */
    uint32_t *comp_mark;
    int32_t comp_mark_cap;
    uint32_t comp_gen;
    PortCand *cands;
    int32_t cands_cap;
};

ShipsState *ships_state_new(OfeGame *g) {
    OFE_UNUSED(g);
    return (ShipsState *)ofe_xcalloc(1, sizeof(ShipsState));
}

static void state_reserve(ShipsState *st, int32_t cap) {
    if (cap <= st->cap) return;
    st->port_rej = (int16_t *)ofe_xrealloc(st->port_rej, (size_t)cap * sizeof(int16_t));
    st->port_gen = (uint16_t *)ofe_xrealloc(st->port_gen, (size_t)cap * sizeof(uint16_t));
    memset(st->port_rej + st->cap, 0, (size_t)(cap - st->cap) * sizeof(int16_t));
    memset(st->port_gen + st->cap, 0, (size_t)(cap - st->cap) * sizeof(uint16_t));
    st->cap = cap;
}

void ships_state_copy(OfeGame *dst, const OfeGame *src) {
    const ShipsState *s = src->sub->ships;
    if (!dst->sub->ships) dst->sub->ships = ships_state_new(dst);
    ShipsState *d = dst->sub->ships;
    state_reserve(d, s->cap);
    memcpy(d->port_rej, s->port_rej, (size_t)s->cap * sizeof(int16_t));
    memcpy(d->port_gen, s->port_gen, (size_t)s->cap * sizeof(uint16_t));
    /* Slots past s->cap are fresh in the source. */
    if (d->cap > s->cap) {
        memset(d->port_rej + s->cap, 0, (size_t)(d->cap - s->cap) * sizeof(int16_t));
        memset(d->port_gen + s->cap, 0, (size_t)(d->cap - s->cap) * sizeof(uint16_t));
    }
}

void ships_state_free(OfeGame *g) {
    ShipsState *st = g->sub->ships;
    if (!st) return;
    free(st->port_rej);
    free(st->port_gen);
    IntVec_free(&st->ports);
    IntVec_free(&st->ships);
    IntVec_free(&st->shells);
    IntVec_free(&st->route);
    free(st->comp_mark);
    free(st->cands);
    free(st);
    g->sub->ships = NULL;
}

static inline ShipsState *state_of(OfeGame *g) {
    if (!g->sub->ships) g->sub->ships = ships_state_new(g);
    return g->sub->ships;
}

/* ---- Water queries ------------------------------------------------------------ */

int32_t ships_water_component(OfeGame *g, TileRef tile) {
    if (tile < 0 || tile >= g->map->num_tiles) return -1;
    if (map_is_land(g->map, tile)) return -1;
    return water_tile_component(water_graph(g->map), g->map, tile);
}

static bool comps_intersect(const int32_t *a, int na, const int32_t *b, int nb) {
    for (int i = 0; i < na; i++) {
        for (int j = 0; j < nb; j++) {
            if (a[i] == b[j]) return true;
        }
    }
    return false;
}

/* Does tile t (water, or a shore) touch water body comp? */
static bool tile_touches_comp(const WaterGraph *wg, const OfeMap *m, TileRef t, int32_t comp) {
    int32_t c[4];
    int n = water_tile_components(wg, m, t, c);
    for (int i = 0; i < n; i++) {
        if (c[i] == comp) return true;
    }
    return false;
}

static inline int64_t dist2(const OfeMap *m, TileRef a, TileRef b) { return map_dist2(m, a, b); }

/* ---- Transport ship: launch ---------------------------------------------------- */

/* Marks the water bodies touching p's shore tiles. Returns false if none. */
static bool mark_reachable_bodies(OfeGame *g, ShipsState *st, PlayerId p, const WaterGraph *wg) {
    int32_t need = water_graph_num_components(wg) + 1;
    if (st->comp_mark_cap < need) {
        st->comp_mark = (uint32_t *)ofe_xrealloc(st->comp_mark, (size_t)need * sizeof(uint32_t));
        memset(st->comp_mark, 0, (size_t)need * sizeof(uint32_t));
        st->comp_mark_cap = need;
        st->comp_gen = 0;
    }
    if (++st->comp_gen == 0) {
        memset(st->comp_mark, 0, (size_t)st->comp_mark_cap * sizeof(uint32_t));
        st->comp_gen = 1;
    }
    bool any = false;
    const IntVec *border = &g->players.border[p];
    for (int32_t i = 0; i < border->len; i++) {
        TileRef t = border->data[i];
        if (!map_is_shore(g->map, t)) continue;
        int32_t c[4];
        int n = water_tile_components(wg, g->map, t, c);
        for (int k = 0; k < n; k++) {
            st->comp_mark[c[k]] = st->comp_gen;
            any = true;
        }
    }
    return any;
}

/* OpenFront's closestReachableShore: the nearest shore tile owned by
 * whoever owns `tile`, on a water body the attacker's own shore touches. */
static TileRef find_landing(OfeGame *g, ShipsState *st, PlayerId attacker, TileRef tile) {
    const OfeMap *m = g->map;
    const WaterGraph *wg = water_graph(g->map);
    if (!mark_reachable_bodies(g, st, attacker, wg)) return -1;
    PlayerId target = tile_owner(g, tile);
    const int tx = map_x(m, tile), ty = map_y(m, tile);
    for (int d = 0; d <= LANDING_SEARCH_RANGE; d++) {
        for (int dx = -d; dx <= d; dx++) {
            int rem = d - abs(dx);
            for (int side = 0; side < (rem == 0 ? 1 : 2); side++) {
                int x = tx + dx, y = ty + (side == 0 ? -rem : rem);
                if (!map_in_bounds(m, x, y)) continue;
                TileRef t = map_ref(m, x, y);
                if (!map_is_shore(m, t) || !map_is_ownable(m, t) || tile_owner(g, t) != target) continue;
                int32_t c[4];
                int n = water_tile_components(wg, m, t, c);
                for (int k = 0; k < n; k++) {
                    if (st->comp_mark[c[k]] == st->comp_gen) return t;
                }
            }
        }
    }
    return -1;
}

/* Up to SOURCE_CANDIDATES of p's shore tiles on the landing's water body,
 * nearest (straight line) first. */
static int pick_sources(OfeGame *g, PlayerId p, TileRef landing, TileRef *out) {
    const OfeMap *m = g->map;
    const WaterGraph *wg = water_graph(g->map);
    int32_t lc[4];
    int nl = water_tile_components(wg, m, landing, lc);
    int32_t best_d[SOURCE_CANDIDATES];
    int n = 0;
    const IntVec *border = &g->players.border[p];
    for (int32_t i = 0; i < border->len; i++) {
        TileRef t = border->data[i];
        if (!map_is_shore(m, t)) continue;
        int32_t d = map_manhattan(m, t, landing);
        if (n == SOURCE_CANDIDATES && d >= best_d[n - 1]) continue;
        int32_t c[4];
        int nc = water_tile_components(wg, m, t, c);
        if (!comps_intersect(c, nc, lc, nl)) continue;
        int k = n < SOURCE_CANDIDATES ? n++ : n - 1;
        while (k > 0 && best_d[k - 1] > d) {
            best_d[k] = best_d[k - 1];
            out[k] = out[k - 1];
            k--;
        }
        best_d[k] = d;
        out[k] = t;
    }
    return n;
}

static bool unit_disabled(const OfeGame *g, int type) { return (g->cfg.disabled_units >> type) & 1u; }

/* The checks both launching and ships_transport_source need. Returns the
 * landing tile or -1. */
static TileRef transport_landing(OfeGame *g, ShipsState *st, PlayerId p, TileRef dst) {
    if (!player_valid(g, p) || !player_alive(g, p) || unit_disabled(g, OFE_UNIT_TRANSPORT_SHIP)) return -1;
    if (g->players.unit_count[p][OFE_UNIT_TRANSPORT_SHIP] >= BOAT_MAX_NUMBER) return -1;
    if (dst < 0 || dst >= g->map->num_tiles) return -1;
    PlayerId target = tile_owner(g, dst);
    if (target == p) return -1;
    if (target != 0 && !player_can_attack_player(g, p, target)) return -1;
    return find_landing(g, st, p, dst);
}

TileRef ships_transport_source(OfeGame *g, PlayerId p, TileRef dst) {
    ShipsState *st = state_of(g);
    TileRef landing = transport_landing(g, st, p, dst);
    if (landing < 0) return -1;
    TileRef srcs[SOURCE_CANDIDATES];
    int n = pick_sources(g, p, landing, srcs);
    if (n == 0) return -1;
    int32_t best = water_find_path(water_graph(g->map), g->map, srcs, n, landing, &st->route);
    return best < 0 ? -1 : srcs[best];
}

UnitId ships_launch_transport(OfeGame *g, PlayerId p, TileRef dst, double troops) {
    ShipsState *st = state_of(g);
    TileRef landing = transport_landing(g, st, p, dst);
    if (landing < 0) return -1;
    PlayerId target = tile_owner(g, landing);

    TileRef srcs[SOURCE_CANDIDATES];
    int n = pick_sources(g, p, landing, srcs);
    if (n == 0) return -1;

    if (troops < 0) troops = floor((double)g->players.troops[p] / 5.0);
    troops = OFE_MIN(troops, (double)g->players.troops[p]);
    if (troops < 1) return -1;

    int32_t best = water_find_path(water_graph(g->map), g->map, srcs, n, landing, &st->route);
    if (best < 0) return -1;

    /* The attacker's pending alliance request from the target no longer makes sense. */
    if (target != 0 && g->players.type[target] != OFE_PLAYER_BOT && g->players.type[p] != OFE_PLAYER_BOT) {
        alliance_reject(g, target, p);
    }

    troops = player_remove_troops(g, p, troops);
    UnitId id = unit_create(g, OFE_UNIT_TRANSPORT_SHIP, p, srcs[best]);
    if (id < 0) {
        player_add_troops(g, p, troops);
        return -1;
    }
    int32_t s = unit_lookup(g, id);
    UnitTable *u = &g->units;
    u->troops[s] = troops;
    u->src_tile[s] = srcs[best];
    u->target_tile[s] = landing;
    u->path_pos[s] = 0;
    IntVec_copy(&u->path[s], &st->route);
    return id;
}

bool ships_retreat_transport(OfeGame *g, PlayerId p, UnitId id) {
    int32_t s = unit_lookup(g, id);
    if (s < 0 || g->units.type[s] != OFE_UNIT_TRANSPORT_SHIP || g->units.owner[s] != p) return false;
    g->units.flags[s] |= OFE_UF_RETREATING;
    return true;
}

/* ---- Transport ship: tick -------------------------------------------------------- */

static void transport_arrive(OfeGame *g, int32_t s) {
    UnitTable *u = &g->units;
    PlayerId p = u->owner[s];
    TileRef dst = u->target_tile[s];
    double troops = u->troops[s];
    bool retreating = (u->flags[s] & OFE_UF_RETREATING) != 0;
    UnitId id = unit_id_of_slot(g, s);
    PlayerId owner_now = tile_owner(g, dst);

    if (retreating || owner_now == p) {
        /* Back home (or the landing is already ours): some troops are lost. */
        unit_destroy(g, id, 0);
        player_add_troops(g, p, troops * (1.0 - RETREAT_MALUS));
        return;
    }
    unit_destroy(g, id, 0);
    if (!map_is_ownable(g->map, dst) ||
        (owner_now != 0 && (player_friendly(g, p, owner_now) || !player_can_attack_player(g, p, owner_now)))) {
        player_add_troops(g, p, troops);
        return;
    }
    tile_set_owner(g, dst, p);
    attack_start(g, p, owner_now, troops, dst, false);
}

static void transport_tick(OfeGame *g, int32_t s) {
    UnitTable *u = &g->units;
    IntVec *path = &u->path[s];
    if (path->len == 0 || u->target_tile[s] < 0) return; /* added by a state edit: stays put */

    /* Landing tile turned into water (nuked): go home. */
    if (map_is_water(g->map, u->target_tile[s])) u->flags[s] |= OFE_UF_RETREATING;

    if ((u->flags[s] & OFE_UF_RETREATING) && !u->aux0[s]) {
        /* Retrace the route back to where we left. */
        int32_t pos = u->path_pos[s];
        for (int32_t a = 0, b = pos; a < b; a++, b--) {
            int32_t t = path->data[a];
            path->data[a] = path->data[b];
            path->data[b] = t;
        }
        path->len = pos + 1;
        u->path_pos[s] = 0;
        u->aux0[s] = 1;
    }
    if (u->path_pos[s] + 1 >= path->len) {
        transport_arrive(g, s);
        return;
    }
    u->path_pos[s]++;
    unit_move(g, s, path->data[u->path_pos[s]]);
}

/* ---- Trade ships ------------------------------------------------------------------- */

static double small_economy_boost(int32_t levels) {
    if (levels <= 0) return 1.0;
    double f = OFE_CLAMP((6.0 - levels) / 5.0, 0.0, 1.0);
    return 1.0 + 0.5 * f / levels;
}

static double trade_saturation(int32_t num_ships) {
    double damping = 1.0 - sigmoid(num_ships, M_LN2 / 50.0, 330.0);
    double plateau = 0.25 * (1.0 - sigmoid(num_ships, M_LN2 / 100.0, 800.0));
    return OFE_MAX(damping, plateau);
}

/* 1 / probability of a spawn for one check of one port level. */
static int32_t trade_spawn_rate(int32_t rejections, int32_t num_ships, int32_t port_levels) {
    double boost = small_economy_boost(port_levels);
    double base = 100.0 / (boost * boost);
    double rate = floor(base / (rejections + 1) / trade_saturation(num_ships));
    return rate < 1.0 ? 1 : (int32_t)rate;
}

static int64_t trade_ship_gold(const OfeGame *g, int32_t dist) {
    double mult = g->cfg.gold_multiplier > 0 ? g->cfg.gold_multiplier : 1.0;
    double base = 75000.0 / (1.0 + exp(-0.03 * (dist - TRADE_SHORT_RANGE))) + 50.0 * dist;
    return (int64_t)floor(base * mult);
}

static int cand_cmp(const void *a, const void *b) {
    const PortCand *x = (const PortCand *)a, *y = (const PortCand *)b;
    if (x->dist != y->dist) return x->dist < y->dist ? -1 : 1;
    return x->slot < y->slot ? -1 : (x->slot > y->slot);
}

/* A random trade partner for port s, weighted like OpenFront's
 * tradingPorts(): by level, double for the nearest ports (not too close)
 * and for friendly owners. Returns a port slot or -1. */
static int32_t pick_trade_partner(OfeGame *g, ShipsState *st, int32_t s, uint64_t *rs) {
    const UnitTable *u = &g->units;
    const OfeMap *m = g->map;
    const WaterGraph *wg = water_graph(g->map);
    PlayerId owner = u->owner[s];
    int32_t sc[4];
    int nsc = water_tile_components(wg, m, u->tile[s], sc);
    if (nsc == 0) return -1;

    if (st->cands_cap < st->ports.len) {
        st->cands_cap = st->ports.len * 2;
        st->cands = (PortCand *)ofe_xrealloc(st->cands, (size_t)st->cands_cap * sizeof(PortCand));
    }
    int32_t n = 0;
    for (int32_t i = 0; i < st->ports.len; i++) {
        int32_t q = st->ports.data[i];
        PlayerId o = u->owner[q];
        if (!u->alive[q] || o == owner || (u->flags[q] & OFE_UF_UNDER_CONSTRUCTION)) continue;
        if (!player_can_trade(g, o, owner)) continue;
        int32_t qc[4];
        int nqc = water_tile_components(wg, m, u->tile[q], qc);
        if (!comps_intersect(sc, nsc, qc, nqc)) continue;
        st->cands[n].slot = q;
        st->cands[n].dist = map_manhattan(m, u->tile[s], u->tile[q]);
        st->cands[n].level = u->level[q];
        n++;
    }
    if (n == 0) return -1;
    qsort(st->cands, (size_t)n, sizeof(PortCand), cand_cmp);

    double close_n = OFE_CLAMP((double)n / 3.0, 4.0, (double)n);
    int64_t total = 0;
    for (int32_t i = 0; i < n; i++) {
        PortCand *c = &st->cands[i];
        int32_t w = c->level;
        bool far_enough = c->dist >= TRADE_SHORT_RANGE;
        if (far_enough && i < close_n) w += c->level;
        if (far_enough && player_friendly(g, owner, u->owner[c->slot])) w += c->level;
        c->level = w; /* from here on: the weight */
        total += w;
    }
    if (total <= 0) return -1;
    int64_t r = (int64_t)(rng_next(rs) >> 33) % total;
    for (int32_t i = 0; i < n; i++) {
        r -= st->cands[i].level;
        if (r < 0) return st->cands[i].slot;
    }
    return st->cands[n - 1].slot;
}

static void spawn_trade_ship(OfeGame *g, ShipsState *st, int32_t port, int32_t dst_port) {
    UnitTable *u = &g->units;
    PlayerId owner = u->owner[port];
    TileRef from = u->tile[port], to = u->tile[dst_port];
    if (water_find_path(water_graph(g->map), g->map, &from, 1, to, &st->route) < 0) return;
    UnitId dst_id = unit_id_of_slot(g, dst_port), port_id = unit_id_of_slot(g, port);
    UnitId id = unit_create(g, OFE_UNIT_TRADE_SHIP, owner, from);
    if (id < 0) return;
    int32_t s = unit_lookup(g, id);
    u = &g->units;
    u->target_unit[s] = dst_id;
    u->target_tile[s] = to;
    u->src_tile[s] = from;
    u->aux1[s] = port_id;
    u->aux2[s] = owner;
    u->aux3[s] = g->tick;
    u->path_pos[s] = 0;
    IntVec_copy(&u->path[s], &st->route);
}

static void port_tick(OfeGame *g, ShipsState *st, int32_t s, int32_t num_trade_ships) {
    UnitTable *u = &g->units;
    if (u->flags[s] & OFE_UF_UNDER_CONSTRUCTION) return;
    if ((g->tick + s) % TRADE_CHECK_PERIOD != 0) return;
    PlayerId owner = u->owner[s];
    if (!player_alive(g, owner)) return;

    state_reserve(st, u->cap);
    if (st->port_gen[s] != u->generation[s]) {
        st->port_gen[s] = u->generation[s];
        st->port_rej[s] = 0;
    }
    uint64_t rs = rng_derive(g->seed, ((uint64_t)g->tick << 24) ^ (uint64_t)s ^ 0x7AADE5ull << 40);
    int32_t levels = g->players.unit_levels[owner][OFE_UNIT_PORT];
    bool spawn = false;
    for (int i = 0; i < u->level[s]; i++) {
        int32_t rate = trade_spawn_rate(st->port_rej[s], num_trade_ships, levels);
        if (rng_chance(&rs, rate)) {
            st->port_rej[s] = 0;
            spawn = true;
            break;
        }
        if (st->port_rej[s] < INT16_MAX) st->port_rej[s]++;
    }
    if (!spawn) return;
    int32_t partner = pick_trade_partner(g, st, s, &rs);
    if (partner >= 0) spawn_trade_ship(g, st, s, partner);
}

static void trade_complete(OfeGame *g, int32_t s, PlayerId orig, PlayerId src_owner, PlayerId dst_owner) {
    UnitTable *u = &g->units;
    PlayerId owner = u->owner[s];
    int64_t gold = trade_ship_gold(g, u->aux0[s]);
    bool captured = u->timer[s] != 0;
    unit_destroy(g, unit_id_of_slot(g, s), 0);
    if (captured && owner == orig) {
        player_add_gold(g, orig, gold);
    } else if (captured) {
        player_add_gold(g, owner, gold);
    } else {
        player_add_gold(g, src_owner, gold);
        player_add_gold(g, dst_owner, gold);
        g->players.trade_gold[src_owner] += gold;
        g->players.trade_gold[dst_owner] += gold;
    }
}

static void trade_tick(OfeGame *g, ShipsState *st, int32_t s) {
    UnitTable *u = &g->units;
    UnitId id = unit_id_of_slot(g, s);
    PlayerId owner = u->owner[s];
    PlayerId orig = u->aux2[s];
    if (u->path[s].len == 0 && u->target_unit[s] < 0) return; /* added by a state edit: stays put */
    if (owner != orig) u->timer[s] = 1;
    bool captured = u->timer[s] != 0;

    int32_t dp = unit_lookup(g, u->target_unit[s]);
    int32_t sp = unit_lookup(g, u->aux1[s]);
    PlayerId src_owner = sp >= 0 ? u->owner[sp] : orig;
    PlayerId dst_owner = dp >= 0 ? u->owner[dp] : 0;

    /* The destination was taken by the origin's owner: no trade. */
    if (dp >= 0 && dst_owner == src_owner) {
        unit_destroy(g, id, 0);
        return;
    }
    if (!captured && (dp < 0 || !player_can_trade(g, owner, dst_owner))) {
        unit_destroy(g, id, 0);
        return;
    }
    if (captured && (dp < 0 || owner != dst_owner)) {
        /* A pirate prize sails to the captor's nearest port on this water body. */
        const WaterGraph *wg = water_graph(g->map);
        int32_t comp = water_tile_component(wg, g->map, u->tile[s]);
        int32_t best = -1;
        int32_t best_d = 0;
        for (int32_t i = 0; i < st->ports.len; i++) {
            int32_t q = st->ports.data[i];
            if (!u->alive[q] || u->owner[q] != owner || (u->flags[q] & (OFE_UF_UNDER_CONSTRUCTION | OFE_UF_MARKED_FOR_DELETION))) continue;
            if (comp < 0 || !tile_touches_comp(wg, g->map, u->tile[q], comp)) continue;
            int32_t d = map_manhattan(g->map, u->tile[q], u->tile[s]);
            if (best < 0 || d < best_d) {
                best = q;
                best_d = d;
            }
        }
        TileRef from = u->tile[s], to = best >= 0 ? u->tile[best] : -1;
        if (best < 0 || water_find_path(wg, g->map, &from, 1, to, &st->route) < 0) {
            unit_destroy(g, id, 0);
            return;
        }
        u->target_unit[s] = unit_id_of_slot(g, best);
        u->target_tile[s] = to;
        IntVec_copy(&u->path[s], &st->route);
        u->path_pos[s] = 0;
        dp = best;
        dst_owner = owner;
    }

    IntVec *path = &u->path[s];
    if (u->path_pos[s] + 1 >= path->len || u->tile[s] == u->tile[dp]) {
        trade_complete(g, s, orig, src_owner, dst_owner);
        return;
    }
    TileRef next = path->data[++u->path_pos[s]];
    if (map_is_water(g->map, next) && map_is_shoreline(g->map, next)) u->aux3[s] = g->tick;
    unit_move(g, s, next);
    u->aux0[s]++;
}

/* ---- Warships: helpers --------------------------------------------------------------- */

static inline int ws_vet(const UnitTable *u, int32_t s) { return u->aux3[s] & 3; }
static inline int ws_state(const UnitTable *u, int32_t s) { return (u->aux3[s] >> 2) & 3; }
static inline int ws_progress(const UnitTable *u, int32_t s) { return (u->aux3[s] >> 4) & 0x1ff; }
static inline void ws_set_state(UnitTable *u, int32_t s, int st) { u->aux3[s] = (u->aux3[s] & ~(3 << 2)) | (st << 2); }
static inline void ws_set_vet(UnitTable *u, int32_t s, int v) { u->aux3[s] = (u->aux3[s] & ~3) | v; }
static inline void ws_set_progress(UnitTable *u, int32_t s, int p) { u->aux3[s] = (u->aux3[s] & ~(0x1ff << 4)) | (p << 4); }

static inline TileRef ws_patrol_tile(const UnitTable *u, int32_t s) { return u->aux0[s] ? u->aux0[s] - 1 : u->tile[s]; }
static inline TileRef ws_port_tile(const UnitTable *u, int32_t s) { return u->aux1[s] - 1; }
static inline void ws_set_port_tile(UnitTable *u, int32_t s, TileRef t) { u->aux1[s] = t + 1; }

static inline int32_t ws_max_health(const UnitTable *u, int32_t s) {
    return WARSHIP_MAX_HEALTH + WARSHIP_MAX_HEALTH * ws_vet(u, s) * WARSHIP_VET_HEALTH_PERCENT / 100;
}

static void ws_heal(UnitTable *u, int32_t s, int32_t amount) {
    u->health[s] = OFE_MIN(u->health[s] + amount, ws_max_health(u, s));
}

/* Veterancy: a warship kill is an instant level; transport kills and trade
 * captures share one progress meter (10 kills or 25 captures per level). */
static void ws_add_progress(UnitTable *u, int32_t s, int points) {
    if (ws_vet(u, s) >= WARSHIP_MAX_VETERANCY) return;
    const int per_level = WARSHIP_VET_TRANSPORT_KILLS * WARSHIP_VET_TRADE_CAPTURES;
    int progress = ws_progress(u, s) + points;
    while (progress >= per_level && ws_vet(u, s) < WARSHIP_MAX_VETERANCY) {
        progress -= per_level;
        ws_set_vet(u, s, ws_vet(u, s) + 1);
    }
    if (ws_vet(u, s) >= WARSHIP_MAX_VETERANCY) progress = 0;
    ws_set_progress(u, s, progress);
}

static void ws_record_kill(OfeGame *g, UnitId shooter, int target_type) {
    int32_t s = unit_lookup(g, shooter);
    if (s < 0 || g->units.type[s] != OFE_UNIT_WARSHIP) return;
    UnitTable *u = &g->units;
    if (target_type == OFE_UNIT_WARSHIP) {
        ws_set_progress(u, s, 0);
        if (ws_vet(u, s) < WARSHIP_MAX_VETERANCY) ws_set_vet(u, s, ws_vet(u, s) + 1);
    } else if (target_type == OFE_UNIT_TRANSPORT_SHIP) {
        ws_add_progress(u, s, WARSHIP_VET_TRADE_CAPTURES);
    }
}

/* Follows the unit's route to `target`, (re)routing when it has none, has
 * left it, or its end is more than `tol` tiles from the target. One tile per
 * call. */
static int ws_move(OfeGame *g, ShipsState *st, int32_t s, TileRef target, int tol) {
    UnitTable *u = &g->units;
    if (u->tile[s] == target) return MOVE_ARRIVED;
    IntVec *path = &u->path[s];
    int32_t pos = u->path_pos[s];
    bool ok = path->len > 0 && pos >= 0 && pos + 1 < path->len && path->data[pos] == u->tile[s] &&
              map_manhattan(g->map, path->data[path->len - 1], target) <= tol;
    if (!ok) {
        TileRef from = u->tile[s];
        if (water_find_path(water_graph(g->map), g->map, &from, 1, target, &st->route) < 0) return MOVE_FAILED;
        IntVec_copy(path, &st->route);
        u->path_pos[s] = pos = 0;
        if (path->len < 2) return u->tile[s] == target ? MOVE_ARRIVED : MOVE_FAILED;
    }
    u->path_pos[s] = ++pos;
    unit_move(g, s, path->data[pos]);
    return MOVE_STEPPED;
}

static void ws_stop_moving(UnitTable *u, int32_t s) {
    u->target_tile[s] = -1;
    u->path[s].len = 0;
    u->path_pos[s] = 0;
}

/* ---- Warships: shells ------------------------------------------------------------------- */

static UnitId fire_shell(OfeGame *g, PlayerId owner, TileRef from, int32_t target_slot, int32_t base_damage, int32_t bonus_percent, UnitId shooter) {
    UnitId target = unit_id_of_slot(g, target_slot);
    TileRef target_tile = g->units.tile[target_slot];
    UnitId id = unit_create(g, OFE_UNIT_SHELL, owner, from);
    if (id < 0) return -1;
    int32_t s = unit_lookup(g, id);
    UnitTable *u = &g->units;
    u->target_unit[s] = target;
    u->target_tile[s] = target_tile;
    u->aux0[s] = base_damage > 0 ? base_damage : SHELL_BASE_DAMAGE;
    u->aux1[s] = bonus_percent;
    u->aux2[s] = shooter;
    u->timer[s] = -1;
    IntVec_push(&state_of(g)->shells, s);
    return id;
}

UnitId ships_fire_shell(OfeGame *g, PlayerId owner, TileRef from, int32_t target_slot, int32_t damage) {
    if (target_slot < 0 || target_slot >= g->units.count || !g->units.alive[target_slot]) return -1;
    return fire_shell(g, owner, from, target_slot, damage, 0, -1);
}

static void shell_hit(OfeGame *g, int32_t s, int32_t t) {
    UnitTable *u = &g->units;
    uint64_t rs = rng_derive(g->seed, ((uint64_t)g->tick << 24) ^ (uint64_t)s ^ 0x5E11ull << 40);
    int mult = (rng_int(&rs, 1, 6) - 1) * 25 + 200;
    if (u->aux1[s] > 0) mult = mult * (100 + u->aux1[s]) / 100;
    int32_t damage = (int32_t)lround((double)u->aux0[s] / SHELL_BASE_DAMAGE * mult);
    PlayerId attacker = u->owner[s];
    UnitId shooter = u->aux2[s];
    int type = u->type[t];
    UnitId tid = unit_id_of_slot(g, t);
    bool dead = true; /* only warships have health */
    if (type == OFE_UNIT_WARSHIP) {
        u->health[t] = OFE_MAX(0, u->health[t] - damage);
        dead = u->health[t] == 0;
    }
    unit_destroy(g, unit_id_of_slot(g, s), 0);
    if (dead) {
        unit_destroy(g, tid, attacker);
        ws_record_kill(g, shooter, type);
    }
}

static void shell_tick(OfeGame *g, int32_t s) {
    UnitTable *u = &g->units;
    UnitId id = unit_id_of_slot(g, s);
    int32_t t = unit_lookup(g, u->target_unit[s]);
    if (t < 0 || u->owner[t] == u->owner[s] || (u->timer[s] >= 0 && g->tick >= u->timer[s])) {
        unit_destroy(g, id, 0);
        return;
    }
    if (u->timer[s] < 0 && u->aux2[s] >= 0 && unit_lookup(g, u->aux2[s]) < 0) u->timer[s] = g->tick + SHELL_LIFETIME;

    const OfeMap *m = g->map;
    for (int i = 0; i < SHELL_SPEED; i++) {
        TileRef cur = u->tile[s], dst = u->tile[t];
        if (cur == dst) {
            shell_hit(g, s, t);
            return;
        }
        int x = map_x(m, cur), y = map_y(m, cur);
        int dx = map_x(m, dst) - x, dy = map_y(m, dst) - y;
        if (dx != 0 && (dy == 0 || abs(dx) >= abs(dy))) x += dx > 0 ? 1 : -1;
        else y += dy > 0 ? 1 : -1;
        unit_move(g, s, map_ref(m, x, y));
    }
    u->target_tile[s] = u->tile[t];
}

/* ---- Warships: building and orders ---------------------------------------------------------- */

/* The owner's finished port nearest to `tile` (Manhattan) on water body `comp`, or -1. */
static int32_t nearest_port_on_comp(OfeGame *g, PlayerId p, TileRef tile, int32_t comp) {
    const UnitTable *u = &g->units;
    const WaterGraph *wg = water_graph(g->map);
    int32_t best = -1, best_d = 0;
    for (int32_t i = 0; i < u->count; i++) {
        if (!u->alive[i] || u->type[i] != OFE_UNIT_PORT || u->owner[i] != p) continue;
        if (u->flags[i] & OFE_UF_UNDER_CONSTRUCTION) continue;
        if (!tile_touches_comp(wg, g->map, u->tile[i], comp)) continue;
        int32_t d = map_manhattan(g->map, u->tile[i], tile);
        if (best < 0 || d < best_d) {
            best = i;
            best_d = d;
        }
    }
    return best;
}

UnitId ships_build_warship(OfeGame *g, PlayerId p, TileRef tile) {
    if (!player_valid(g, p) || !player_alive(g, p) || unit_disabled(g, OFE_UNIT_WARSHIP)) return -1;
    if (tile < 0 || tile >= g->map->num_tiles || map_is_land(g->map, tile)) return -1;
    int32_t comp = ships_water_component(g, tile);
    if (comp < 0) return -1;
    int32_t port = nearest_port_on_comp(g, p, tile, comp);
    if (port < 0) return -1;
    int64_t cost = unit_cost(g, p, OFE_UNIT_WARSHIP);
    if (g->players.gold[p] < cost) return -1;

    TileRef spawn = g->units.tile[port];
    player_remove_gold(g, p, cost);
    g->players.units_built[p][OFE_UNIT_WARSHIP]++;
    UnitId id = unit_create(g, OFE_UNIT_WARSHIP, p, spawn);
    if (id < 0) return -1;
    int32_t s = unit_lookup(g, id);
    UnitTable *u = &g->units;
    u->health[s] = WARSHIP_MAX_HEALTH;
    u->aux0[s] = tile + 1;
    u->aux1[s] = 0;
    u->aux2[s] = -1000;
    u->aux3[s] = 0;
    u->timer[s] = -1000;
    return id;
}

bool ships_move_warship(OfeGame *g, PlayerId p, UnitId id, TileRef tile) {
    int32_t s = unit_lookup(g, id);
    UnitTable *u = &g->units;
    if (s < 0 || u->type[s] != OFE_UNIT_WARSHIP || u->owner[s] != p) return false;
    if (tile < 0 || tile >= g->map->num_tiles || map_is_land(g->map, tile)) return false;
    const WaterGraph *wg = water_graph(g->map);
    int32_t comp = water_tile_component(wg, g->map, tile);
    if (comp < 0 || !tile_touches_comp(wg, g->map, u->tile[s], comp)) return false;
    u->aux0[s] = tile + 1;
    u->timer[s] = g->tick; /* no repair retreat for a while */
    if (ws_state(u, s) != WS_PATROL) {
        ws_set_state(u, s, WS_PATROL);
        u->aux1[s] = 0;
        u->auxf[s] = 0;
    }
    ws_stop_moving(u, s);
    return true;
}

/* ---- Warships: ports and repair ---------------------------------------------------------------- */

static int32_t ws_port_at(const OfeGame *g, const ShipsState *st, PlayerId owner, TileRef tile) {
    const UnitTable *u = &g->units;
    for (int32_t i = 0; i < st->ports.len; i++) {
        int32_t q = st->ports.data[i];
        if (u->alive[q] && u->owner[q] == owner && u->tile[q] == tile) return q;
    }
    return -1;
}

/* Warships of `owner` sitting at (or waiting by) the port, healing. */
static int ws_docked_at(const OfeGame *g, const ShipsState *st, PlayerId owner, TileRef port_tile, int32_t exclude, int32_t *out, int cap) {
    const UnitTable *u = &g->units;
    int n = 0;
    for (int32_t i = 0; i < st->ships.len; i++) {
        int32_t q = st->ships.data[i];
        if (q == exclude || !u->alive[q] || u->type[q] != OFE_UNIT_WARSHIP || u->owner[q] != owner) continue;
        if (ws_state(u, q) == WS_PATROL || u->target_tile[q] >= 0) continue;
        if (dist2(g->map, u->tile[q], port_tile) > WARSHIP_DOCKING_RANGE * WARSHIP_DOCKING_RANGE) continue;
        if (out && n < cap) out[n] = q;
        n++;
    }
    return n;
}

static bool ws_port_full(const OfeGame *g, const ShipsState *st, int32_t port, int32_t exclude) {
    return ws_docked_at(g, st, g->units.owner[port], g->units.tile[port], exclude, NULL, 0) >= g->units.level[port];
}

/* Nearest own port on the warship's water body; if `skip_full`, only ports
 * with room. Returns the port slot or -1. */
static int32_t ws_nearest_port(OfeGame *g, const ShipsState *st, int32_t s, bool skip_full, int64_t *d2_out) {
    const UnitTable *u = &g->units;
    const WaterGraph *wg = water_graph(g->map);
    PlayerId owner = u->owner[s];
    int32_t comp = water_tile_component(wg, g->map, u->tile[s]);
    if (comp < 0) return -1;
    int32_t best = -1;
    int64_t best_d = 0;
    for (int32_t i = 0; i < st->ports.len; i++) {
        int32_t q = st->ports.data[i];
        if (!u->alive[q] || u->owner[q] != owner) continue;
        if (skip_full && ws_port_full(g, st, q, s)) continue;
        if (!tile_touches_comp(wg, g->map, u->tile[q], comp)) continue;
        int64_t d = dist2(g->map, u->tile[s], u->tile[q]);
        if (best < 0 || d < best_d) {
            best = q;
            best_d = d;
        }
    }
    if (d2_out) *d2_out = best_d;
    return best;
}

static void ws_cancel_retreat(UnitTable *u, int32_t s, bool clear_target) {
    u->auxf[s] = 0;
    ws_set_state(u, s, WS_PATROL);
    u->aux1[s] = 0;
    if (clear_target) ws_stop_moving(u, s);
}

static bool ws_has_port(const OfeGame *g, const ShipsState *st, PlayerId owner) {
    for (int32_t i = 0; i < st->ports.len; i++) {
        int32_t q = st->ports.data[i];
        if (g->units.alive[q] && g->units.owner[q] == owner) return true;
    }
    return false;
}

static void ws_passive_heal(OfeGame *g, ShipsState *st, int32_t s) {
    UnitTable *u = &g->units;
    PlayerId owner = u->owner[s];
    if (g->players.doomsday_tick[owner] >= 0) return; /* a doomed side cannot repair */
    const int64_t r2 = (int64_t)WARSHIP_PASSIVE_HEAL_RANGE * WARSHIP_PASSIVE_HEAL_RANGE;
    for (int32_t i = 0; i < st->ports.len; i++) {
        int32_t q = st->ports.data[i];
        if (u->alive[q] && u->owner[q] == owner && dist2(g->map, u->tile[s], u->tile[q]) <= r2) {
            ws_heal(u, s, WARSHIP_PASSIVE_HEAL);
            break;
        }
    }
    if (ws_state(u, s) != WS_DOCKED) return;
    int32_t port = ws_port_at(g, st, owner, ws_port_tile(u, s));
    if (port < 0) return;
    int32_t docked[64];
    int n = ws_docked_at(g, st, owner, u->tile[port], -1, docked, 64);
    bool me = false;
    for (int i = 0; i < n && i < 64; i++) me |= docked[i] == s;
    if (!me || n == 0) return;
    double pool = (double)u->level[port] * WARSHIP_PORT_HEAL_PER_LEVEL;
    u->auxf[s] += pool / n;
    int32_t whole = (int32_t)floor(u->auxf[s]);
    if (whole <= 0) return;
    u->auxf[s] -= whole;
    ws_heal(u, s, whole);
}

/* Keeps the repair port valid. Returns false if there is none (abort). */
static bool ws_refresh_port(OfeGame *g, ShipsState *st, int32_t s) {
    UnitTable *u = &g->units;
    PlayerId owner = u->owner[s];
    if (!ws_has_port(g, st, owner)) return false;
    int32_t cur = u->aux1[s] > 0 ? ws_port_at(g, st, owner, ws_port_tile(u, s)) : -1;
    int64_t d2 = 0;
    if (cur < 0) {
        int32_t np = ws_nearest_port(g, st, s, true, NULL);
        u->aux1[s] = np >= 0 ? u->tile[np] + 1 : 0;
        return np >= 0;
    }
    if (ws_port_full(g, st, cur, -1)) {
        int32_t alt = ws_nearest_port(g, st, s, true, NULL);
        if (alt >= 0) ws_set_port_tile(u, s, u->tile[alt]);
        return u->aux1[s] > 0;
    }
    int32_t closer = ws_nearest_port(g, st, s, true, &d2);
    if (closer >= 0 && closer != cur &&
        (double)d2 < (double)dist2(g->map, u->tile[s], u->tile[cur]) * WARSHIP_PORT_SWITCH) {
        ws_set_port_tile(u, s, u->tile[closer]);
    }
    return true;
}

/* ---- Warships: targeting and shooting ------------------------------------------------------------ */

static bool ws_shell_in_flight(const OfeGame *g, const ShipsState *st, PlayerId owner, UnitId target) {
    const UnitTable *u = &g->units;
    for (int32_t i = 0; i < st->shells.len; i++) {
        int32_t q = st->shells.data[i];
        if (u->alive[q] && u->type[q] == OFE_UNIT_SHELL && u->owner[q] == owner && u->target_unit[q] == target) return true;
    }
    return false;
}

/* The best enemy ship in range: transport ships first, then warships, then
 * (optionally) trade ships, nearest first within a class. */
static int32_t ws_find_target(OfeGame *g, ShipsState *st, int32_t s, bool with_trade) {
    const UnitTable *u = &g->units;
    const OfeMap *m = g->map;
    PlayerId owner = u->owner[s];
    const int64_t r2 = (int64_t)WARSHIP_TARGET_RANGE * WARSHIP_TARGET_RANGE;
    int32_t best = -1, best_pri = 3;
    int64_t best_d = 0;
    int trade_ready = -1; /* lazily: this warship can deliver pirate prizes */
    int32_t comp = -1;
    for (int32_t i = 0; i < st->ships.len; i++) {
        int32_t q = st->ships.data[i];
        if (q == s || !u->alive[q]) continue;
        int type = u->type[q];
        if (type == OFE_UNIT_TRADE_SHIP && !with_trade) continue;
        int64_t d = dist2(m, u->tile[s], u->tile[q]);
        if (d > r2) continue;
        PlayerId o = u->owner[q];
        if (o == owner || !player_can_attack_player(g, owner, o)) continue;
        if (type == OFE_UNIT_WARSHIP && ws_state(u, q) == WS_DOCKED) continue;
        if (type != OFE_UNIT_WARSHIP && ws_shell_in_flight(g, st, owner, unit_id_of_slot(g, q))) continue;
        if (type == OFE_UNIT_TRADE_SHIP) {
            if (trade_ready < 0) {
                const WaterGraph *wg = water_graph(g->map);
                comp = water_tile_component(wg, m, u->tile[s]);
                trade_ready = 0;
                for (int32_t k = 0; k < st->ports.len && comp >= 0 && !trade_ready; k++) {
                    int32_t pp = st->ports.data[k];
                    trade_ready = u->alive[pp] && u->owner[pp] == owner && !(u->flags[pp] & (OFE_UF_UNDER_CONSTRUCTION | OFE_UF_MARKED_FOR_DELETION)) &&
                                  tile_touches_comp(wg, m, u->tile[pp], comp);
                }
            }
            if (!trade_ready) continue;
            if (g->tick - u->aux3[q] < SAFE_FROM_PIRATES_TICKS) continue;
            int32_t dp = unit_lookup(g, u->target_unit[q]);
            if (dp >= 0 && (u->owner[dp] == owner || player_friendly(g, owner, u->owner[dp]))) continue;
            if (dist2(m, ws_patrol_tile(u, s), u->tile[q]) > (int64_t)WARSHIP_PATROL_RANGE * WARSHIP_PATROL_RANGE) continue;
        }
        int pri = type == OFE_UNIT_TRANSPORT_SHIP ? 0 : (type == OFE_UNIT_WARSHIP ? 1 : 2);
        if (best < 0 || pri < best_pri || (pri == best_pri && d < best_d)) {
            best = q;
            best_pri = pri;
            best_d = d;
        }
    }
    return best;
}

static void ws_shoot(OfeGame *g, int32_t s) {
    UnitTable *u = &g->units;
    int32_t t = unit_lookup(g, u->target_unit[s]);
    if (t < 0 || g->tick - u->aux2[s] <= WARSHIP_SHELL_RELOAD) return;
    int type = u->type[t];
    /* Transport ships die in one hit, so a warship needn't reload for them. */
    if (type != OFE_UNIT_TRANSPORT_SHIP) u->aux2[s] = g->tick;
    fire_shell(g, u->owner[s], u->tile[s], t, SHELL_BASE_DAMAGE, ws_vet(u, s) * WARSHIP_VET_DAMAGE_PERCENT, unit_id_of_slot(g, s));
    u = &g->units;
    if (type != OFE_UNIT_WARSHIP) u->target_unit[s] = -1; /* one shell is enough */
}

/* ---- Warships: patrol, hunt, retreat ---------------------------------------------------------------- */

/* A random calm water tile near the patrol centre on the warship's water body. */
static TileRef ws_random_patrol_tile(OfeGame *g, int32_t s) {
    const UnitTable *u = &g->units;
    const OfeMap *m = g->map;
    const WaterGraph *wg = water_graph(g->map);
    TileRef centre = ws_patrol_tile(u, s);
    int32_t comp = water_tile_component(wg, m, u->tile[s]);
    uint64_t rs = rng_derive(g->seed, ((uint64_t)g->tick << 24) ^ (uint64_t)s ^ 0x9A7201ull << 40);
    int range = WARSHIP_PATROL_RANGE;
    for (int pass = 0; pass < 2; pass++) { /* second pass accepts the shoreline */
        for (int attempt = 0; attempt < 3 * 100; attempt++) {
            if (attempt > 0 && attempt % 100 == 0) range += range / 2;
            int x = map_x(m, centre) + rng_int(&rs, -range / 2, range / 2);
            int y = map_y(m, centre) + rng_int(&rs, -range / 2, range / 2);
            if (!map_in_bounds(m, x, y)) continue;
            TileRef t = map_ref(m, x, y);
            if (map_is_land(m, t) || (pass == 0 && map_is_shoreline(m, t))) continue;
            if (comp >= 0 && water_tile_component(wg, m, t) != comp) continue;
            return t;
        }
    }
    return -1;
}

static void ws_patrol(OfeGame *g, ShipsState *st, int32_t s) {
    UnitTable *u = &g->units;
    if (u->target_tile[s] < 0) {
        u->target_tile[s] = ws_random_patrol_tile(g, s);
        if (u->target_tile[s] < 0) return;
    }
    int r = ws_move(g, st, s, u->target_tile[s], 0);
    if (r != MOVE_STEPPED) ws_stop_moving(u, s);
}

static void ws_hunt(OfeGame *g, ShipsState *st, int32_t s) {
    UnitTable *u = &g->units;
    for (int i = 0; i < 2; i++) {
        int32_t t = unit_lookup(g, u->target_unit[s]);
        if (t < 0) return;
        TileRef tt = u->tile[t];
        int32_t dist = map_manhattan(g->map, u->tile[s], tt);
        if (dist <= WARSHIP_CAPTURE_DIST) {
            unit_capture(g, unit_id_of_slot(g, t), u->owner[s]);
            ws_add_progress(u, s, WARSHIP_VET_TRANSPORT_KILLS);
            u->target_unit[s] = -1;
            return;
        }
        if (dist <= WARSHIP_GREEDY_DIST) {
            /* Close in directly: routes on the coarse map are too rough up close. */
            TileRef nb[4], best = -1;
            int k = map_neighbors4(g->map, u->tile[s], nb);
            int32_t best_d = dist;
            for (int j = 0; j < k; j++) {
                if (map_is_land(g->map, nb[j])) continue;
                int32_t d = map_manhattan(g->map, nb[j], tt);
                if (d < best_d) {
                    best_d = d;
                    best = nb[j];
                }
            }
            if (best >= 0) {
                unit_move(g, s, best);
                continue;
            }
        }
        if (ws_move(g, st, s, tt, 8) == MOVE_FAILED) u->path[s].len = 0;
    }
}

/* Returns true if the warship spent the tick on a repair retreat. */
static bool ws_handle_retreat(OfeGame *g, ShipsState *st, int32_t s) {
    UnitTable *u = &g->units;
    if (ws_state(u, s) == WS_PATROL) return false;

    int32_t aggro = ws_find_target(g, st, s, false);
    if (aggro >= 0) {
        u->target_unit[s] = unit_id_of_slot(g, aggro);
        ws_shoot(g, s);
    }
    if (!ws_refresh_port(g, st, s)) {
        ws_cancel_retreat(u, s, true);
        return false;
    }
    if (aggro < 0) u->target_unit[s] = -1;

    TileRef port_tile = ws_port_tile(u, s);
    if (dist2(g->map, u->tile[s], port_tile) <= WARSHIP_DOCKING_RANGE * WARSHIP_DOCKING_RANGE) {
        int32_t port = ws_port_at(g, st, u->owner[s], port_tile);
        if (port >= 0 && !ws_port_full(g, st, port, s)) {
            ws_stop_moving(u, s);
            ws_set_state(u, s, WS_DOCKED);
            return true;
        }
        if (u->health[s] >= ws_max_health(u, s)) {
            ws_cancel_retreat(u, s, true);
            return false;
        }
        return true; /* wait for a free berth */
    }
    u->target_tile[s] = port_tile;
    if (ws_move(g, st, s, port_tile, 0) == MOVE_FAILED) {
        int32_t np = ws_nearest_port(g, st, s, true, NULL);
        u->path[s].len = 0;
        if (np < 0) ws_cancel_retreat(u, s, true);
        else ws_set_port_tile(u, s, u->tile[np]);
    }
    return true;
}

static void warship_tick(OfeGame *g, ShipsState *st, int32_t s) {
    UnitTable *u = &g->units;
    UnitId id = unit_id_of_slot(g, s);
    PlayerId owner = u->owner[s];
    if (u->health[s] <= 0) {
        unit_destroy(g, id, 0);
        return;
    }
    const int32_t health_before = u->health[s];
    ws_passive_heal(g, st, s);
    const bool doomed = g->players.doomsday_tick[owner] >= 0;

    /* A doomed side can't repair, so a retreat would only idle at the dock. */
    if (doomed && ws_state(u, s) != WS_PATROL) ws_cancel_retreat(u, s, true);

    if (ws_state(u, s) == WS_DOCKED) {
        if (ws_port_at(g, st, owner, ws_port_tile(u, s)) < 0 || u->health[s] >= ws_max_health(u, s)) ws_cancel_retreat(u, s, true);
        if (ws_state(u, s) == WS_DOCKED) return;
    }
    if (ws_handle_retreat(g, st, s)) return;

    /* Badly hurt: head for a port. */
    if (ws_state(u, s) == WS_PATROL && !doomed && g->tick - u->timer[s] >= WARSHIP_MANUAL_RETREAT_PAUSE &&
        health_before < ws_max_health(u, s) * WARSHIP_RETREAT_PERCENT / 100 && ws_has_port(g, st, owner)) {
        int32_t port = ws_nearest_port(g, st, s, false, NULL);
        if (port >= 0) {
            ws_set_state(u, s, WS_RETREAT);
            ws_set_port_tile(u, s, u->tile[port]);
            u->auxf[s] = 0;
            u->target_unit[s] = -1;
            if (ws_handle_retreat(g, st, s)) return;
        }
    }

    int32_t t = ws_find_target(g, st, s, true);
    u->target_unit[s] = t >= 0 ? unit_id_of_slot(g, t) : -1;
    if (t >= 0 && u->type[t] != OFE_UNIT_TRADE_SHIP) {
        ws_shoot(g, s);
        ws_patrol(g, st, s);
    } else if (t >= 0) {
        ws_hunt(g, st, s);
    } else {
        ws_patrol(g, st, s);
    }
}

/* ---- Tick ------------------------------------------------------------------------------------------------ */

void ships_tick(OfeGame *g) {
    ShipsState *st = state_of(g);
    UnitTable *u = &g->units;
    const int32_t n = u->count;
    state_reserve(st, u->cap);

    st->ports.len = st->ships.len = st->shells.len = 0;
    int32_t num_trade = 0;
    for (int32_t i = 0; i < n; i++) {
        if (!u->alive[i]) continue;
        switch (u->type[i]) {
        case OFE_UNIT_PORT: IntVec_push(&st->ports, i); break;
        case OFE_UNIT_TRADE_SHIP: num_trade++; IntVec_push(&st->ships, i); break;
        case OFE_UNIT_TRANSPORT_SHIP:
        case OFE_UNIT_WARSHIP: IntVec_push(&st->ships, i); break;
        case OFE_UNIT_SHELL: IntVec_push(&st->shells, i); break;
        default: break;
        }
    }
    if (st->ports.len + st->ships.len + st->shells.len == 0) return;

    for (int32_t i = 0; i < n; i++) {
        u = &g->units;
        if (!u->alive[i]) continue;
        switch (u->type[i]) {
        case OFE_UNIT_PORT: port_tick(g, st, i, num_trade); break;
        case OFE_UNIT_TRANSPORT_SHIP: transport_tick(g, i); break;
        case OFE_UNIT_TRADE_SHIP: trade_tick(g, st, i); break;
        case OFE_UNIT_WARSHIP: warship_tick(g, st, i); break;
        case OFE_UNIT_SHELL: shell_tick(g, i); break;
        default: break;
        }
    }
}

/*
 * structures.c - building, upgrading and deleting structures (city, port,
 * factory, defense post, missile silo, SAM launcher), plus the spatial
 * index every structure query goes through.
 *
 * Rules follow OpenFront's ConstructionExecution / PlayerImpl.canBuild /
 * UpgradeStructureExecution / DeleteUnitExecution:
 *   - A structure goes on the nearest tile (to the requested one) that the
 *     player owns, that is connected to the request through owned land
 *     within 15 tiles, and that is at least 15 tiles (euclidean) from every
 *     other structure of any owner, finished or not. Ports additionally
 *     need a shore tile.
 *   - It is charged when placed and takes a per-type number of ticks to
 *     finish (instant_build: zero). Upgrades add a level at once and cost
 *     what the next structure of that type would cost.
 *   - Deleting marks the unit; it is destroyed 30 s later, and a player can
 *     mark one unit every 30 s. A capture cancels the mark.
 *
 * Simplifications vs the TS engine (not bit-exact):
 *   - Construction finishes `duration` ticks after the build intent (TS:
 *     duration + 1 ticks, an artefact of its execution ordering).
 *   - Only structures can be deleted (TS lets any unit on owned land be
 *     marked, which only matters for trains).
 *   - A BUILD_UNIT intent for a structure always builds one, as in TS
 *     (`amount` only multiplies nukes there); `amount` buys several levels
 *     for upgrades only.
 *   - Defense posts do not shoot ships: TS has that code commented out
 *     (DefensePostExecution), so ships_fire_shell is never called from here.
 *   - Silo reload timers, SAM range growth after upgrades and trade/train
 *     logic belong to nukes.c / ships.c / rail.c.
 *
 * Spatial index: all structures sit in a grid of 32x32-tile cells (they never
 * move). unit_create / unit_destroy keep it current, so placement checks and
 * units_has_nearby (called for every tile taken from a defender with
 * defense posts) look at a handful of cells instead of the whole unit table.
 */
#include "internal.h"

#define CELL_BITS 5 /* 32 x 32 tiles per cell */
#define STRUCTURE_MIN_DIST 15
#define MAX_UPGRADE_AMOUNT 50
#define DELETION_MARK_TICKS 300 /* deletionMarkDuration, 30 s */
#define DELETE_COOLDOWN_TICKS 300 /* deleteUnitCooldown, 30 s */

typedef struct PendingDeletion {
    UnitId id;
    PlayerId owner; /* a capture cancels the deletion */
    int32_t at; /* destroyed once tick > at */
} PendingDeletion;
OFE_VEC(PendingDeletionVec, PendingDeletion)

struct StructuresState {
    int32_t cells_x, cells_y;
    IntVec *cells; /* slots of structures, per grid cell */
    IntVec constructing; /* ids of structures still under construction */
    PendingDeletionVec deletions;
    int32_t *last_delete; /* per player: tick of the last delete order, -1 = never */
    int32_t last_delete_cap;
    IntVec scratch_units, scratch_queue, scratch_stack; /* query scratch, not game state */
};

/* ---- Per-type constants --------------------------------------------------- */

static int32_t construction_ticks(int type) {
    switch (type) {
    case OFE_UNIT_PORT: return 50;
    case OFE_UNIT_MISSILE_SILO: return 100;
    case OFE_UNIT_DEFENSE_POST: return 50;
    case OFE_UNIT_SAM_LAUNCHER: return 300;
    case OFE_UNIT_CITY:
    case OFE_UNIT_FACTORY: return 20;
    default: return 0;
    }
}

int32_t structure_construction_ticks(const OfeGame *g, int type) {
    return g->cfg.instant_build ? 0 : construction_ticks(type);
}

bool structure_upgradable(int type) {
    return type == OFE_UNIT_PORT || type == OFE_UNIT_MISSILE_SILO || type == OFE_UNIT_SAM_LAUNCHER ||
           type == OFE_UNIT_CITY || type == OFE_UNIT_FACTORY;
}

/* ---- State ---------------------------------------------------------------- */

static void state_size_for_map(StructuresState *s, const OfeMap *m) {
    int32_t cx = (m->width + (1 << CELL_BITS) - 1) >> CELL_BITS;
    int32_t cy = (m->height + (1 << CELL_BITS) - 1) >> CELL_BITS;
    if (s->cells && s->cells_x == cx && s->cells_y == cy) return;
    for (int32_t i = 0; i < s->cells_x * s->cells_y; i++) IntVec_free(&s->cells[i]);
    free(s->cells);
    s->cells_x = cx;
    s->cells_y = cy;
    s->cells = (IntVec *)ofe_xcalloc((size_t)cx * cy, sizeof(IntVec));
}

static void state_size_for_players(StructuresState *s, int32_t cap) {
    if (s->last_delete_cap >= cap) return;
    s->last_delete = (int32_t *)ofe_xrealloc(s->last_delete, (size_t)cap * sizeof(int32_t));
    for (int32_t i = s->last_delete_cap; i < cap; i++) s->last_delete[i] = -1;
    s->last_delete_cap = cap;
}

StructuresState *structures_state_new(OfeGame *g) {
    StructuresState *s = (StructuresState *)ofe_xcalloc(1, sizeof(*s));
    state_size_for_map(s, g->map);
    state_size_for_players(s, g->players.cap);
    return s;
}

void structures_state_copy(OfeGame *dst, const OfeGame *src) {
    const StructuresState *s = src->sub->structures;
    if (!dst->sub->structures) dst->sub->structures = structures_state_new(dst);
    StructuresState *d = dst->sub->structures;
    state_size_for_map(d, dst->map);
    state_size_for_players(d, src->players.cap);
    for (int32_t i = 0; i < s->cells_x * s->cells_y; i++) IntVec_copy(&d->cells[i], &s->cells[i]);
    IntVec_copy(&d->constructing, &s->constructing);
    d->deletions.len = 0;
    for (int32_t i = 0; i < s->deletions.len; i++) PendingDeletionVec_push(&d->deletions, s->deletions.data[i]);
    memcpy(d->last_delete, s->last_delete, (size_t)s->last_delete_cap * sizeof(int32_t));
}

void structures_state_free(OfeGame *g) {
    StructuresState *s = g->sub->structures;
    if (!s) return;
    for (int32_t i = 0; i < s->cells_x * s->cells_y; i++) IntVec_free(&s->cells[i]);
    free(s->cells);
    IntVec_free(&s->constructing);
    PendingDeletionVec_free(&s->deletions);
    free(s->last_delete);
    IntVec_free(&s->scratch_units);
    IntVec_free(&s->scratch_queue);
    IntVec_free(&s->scratch_stack);
    free(s);
    g->sub->structures = NULL;
}

/* ---- Spatial index ----------------------------------------------------------- */

static IntVec *cell_of(StructuresState *s, const OfeMap *m, TileRef t) {
    return &s->cells[(map_y(m, t) >> CELL_BITS) * s->cells_x + (map_x(m, t) >> CELL_BITS)];
}

/* Called by unit_create / unit_destroy for every unit; ignores non-structures. */
void structures_index_add(OfeGame *g, int32_t slot) {
    if (!g->sub || !g->sub->structures || !unit_is_structure(g->units.type[slot])) return;
    IntVec_push(cell_of(g->sub->structures, g->map, g->units.tile[slot]), slot);
}

void structures_index_remove(OfeGame *g, int32_t slot) {
    if (!g->sub || !g->sub->structures || !unit_is_structure(g->units.type[slot])) return;
    IntVec *c = cell_of(g->sub->structures, g->map, g->units.tile[slot]);
    for (int32_t i = 0; i < c->len; i++) {
        if (c->data[i] == slot) {
            c->data[i] = c->data[--c->len];
            return;
        }
    }
}

/* Calls fn for every indexed structure within euclidean `range` of tile.
 * fn returns true to stop early; the whole call returns true if it did. */
typedef bool (*StructureVisit)(const OfeGame *g, int32_t slot, void *ctx);

static bool index_visit(const OfeGame *g, TileRef tile, int32_t range, StructureVisit fn, void *ctx) {
    const StructuresState *s = g->sub->structures;
    const OfeMap *m = g->map;
    int32_t x = map_x(m, tile), y = map_y(m, tile);
    int32_t cx0 = OFE_MAX(x - range, 0) >> CELL_BITS, cx1 = OFE_MIN(x + range, m->width - 1) >> CELL_BITS;
    int32_t cy0 = OFE_MAX(y - range, 0) >> CELL_BITS, cy1 = OFE_MIN(y + range, m->height - 1) >> CELL_BITS;
    int64_t r2 = (int64_t)range * range;
    for (int32_t cy = cy0; cy <= cy1; cy++) {
        for (int32_t cx = cx0; cx <= cx1; cx++) {
            const IntVec *c = &s->cells[cy * s->cells_x + cx];
            for (int32_t i = 0; i < c->len; i++) {
                int32_t slot = c->data[i];
                if (map_dist2(m, tile, g->units.tile[slot]) <= r2 && fn(g, slot, ctx)) return true;
            }
        }
    }
    return false;
}

typedef struct NearbyQuery {
    int type; /* -1 = any structure */
    PlayerId owner; /* 0 = any */
    bool finished_only;
    IntVec *out; /* collects slots when non-NULL, else the first hit stops */
} NearbyQuery;

static bool nearby_match(const OfeGame *g, int32_t slot, void *ctx) {
    NearbyQuery *q = (NearbyQuery *)ctx;
    const UnitTable *u = &g->units;
    if (q->type >= 0 && u->type[slot] != q->type) return false;
    if (q->owner != 0 && u->owner[slot] != q->owner) return false;
    if (q->finished_only && (u->flags[slot] & OFE_UF_UNDER_CONSTRUCTION)) return false;
    if (!q->out) return true;
    IntVec_push(q->out, slot);
    return false;
}

bool structures_has_nearby(const OfeGame *g, TileRef tile, int32_t range, int type, PlayerId owner) {
    NearbyQuery q = {type, owner, true, NULL};
    return index_visit(g, tile, range, nearby_match, &q);
}

int32_t structures_nearby(const OfeGame *g, TileRef tile, int32_t range, int type, PlayerId owner, int32_t *out_slots, int32_t cap) {
    IntVec *hits = &g->sub->structures->scratch_units;
    hits->len = 0;
    NearbyQuery q = {type, owner, false, hits};
    index_visit(g, tile, range, nearby_match, &q);
    /* Nearest first; equal distances by slot so the order doesn't depend on
     * how the index was filled. */
    for (int32_t i = 1; i < hits->len; i++) {
        int32_t s = hits->data[i];
        int64_t d = map_dist2(g->map, tile, g->units.tile[s]);
        int32_t j = i - 1;
        while (j >= 0) {
            int32_t o = hits->data[j];
            int64_t od = map_dist2(g->map, tile, g->units.tile[o]);
            if (od < d || (od == d && o < s)) break;
            hits->data[j + 1] = o;
            j--;
        }
        hits->data[j + 1] = s;
    }
    int32_t n = OFE_MIN(hits->len, cap);
    for (int32_t i = 0; i < n; i++) out_slots[i] = hits->data[i];
    return n;
}

/* ---- Placement ---------------------------------------------------------------- */

static bool blocked_by_structure(const OfeGame *g, TileRef t, const IntVec *near) {
    int64_t min2 = (int64_t)STRUCTURE_MIN_DIST * STRUCTURE_MIN_DIST;
    for (int32_t i = 0; i < near->len; i++) {
        if (map_dist2(g->map, g->units.tile[near->data[i]], t) < min2) return true;
    }
    return false;
}

/* PlayerImpl.validStructureSpawnTiles + its first entry: the tile closest to
 * `tile` where p may put a structure, or -1. TS floods p's land within 15
 * tiles with a depth-first stack (neighbours pushed north, south, west, east),
 * drops tiles within 15 of any structure, and takes the closest by a stable
 * sort, so among equally near tiles the one the flood found first wins. */
static TileRef find_structure_tile(const OfeGame *g, PlayerId p, TileRef tile, bool need_shore) {
    if (tile_owner(g, tile) != p) return -1;
    StructuresState *s = g->sub->structures;
    const OfeMap *m = g->map;

    /* A candidate within the min distance of the request is only blocked by
     * structures within twice the min distance of the request. */
    IntVec *near = &s->scratch_units;
    near->len = 0;
    NearbyQuery q = {-1, 0, false, near};
    index_visit(g, tile, 2 * STRUCTURE_MIN_DIST, nearby_match, &q);

    if ((!need_shore || map_is_shore(m, tile)) && !blocked_by_structure(g, tile, near)) return tile;

    int64_t radius2 = (int64_t)STRUCTURE_MIN_DIST * STRUCTURE_MIN_DIST;
    OfeGame *mg = (OfeGame *)g; /* only the visit-stamp scratch is written */
    uint32_t gen = tiles_new_visit_gen(mg);
    IntVec *found = &s->scratch_queue, *stack = &s->scratch_stack;
    found->len = stack->len = 0;
    IntVec_push(found, tile);
    IntVec_push(stack, tile);
    mg->visit_stamp[tile] = gen;
    TileRef best = -1;
    int64_t best_d = 0;
    while (stack->len > 0) {
        TileRef t = stack->data[--stack->len];
        TileRef nb[4];
        int k = map_neighbors4(m, t, nb);
        for (int i = 0; i < k; i++) {
            TileRef n = nb[i];
            if (mg->visit_stamp[n] == gen || tile_owner(g, n) != p || map_dist2(m, n, tile) >= radius2) continue;
            mg->visit_stamp[n] = gen;
            IntVec_push(found, n);
            IntVec_push(stack, n);
        }
    }
    for (int32_t i = 0; i < found->len; i++) {
        TileRef t = found->data[i];
        int64_t d = map_dist2(m, t, tile);
        if ((best < 0 || d < best_d) && (!need_shore || map_is_shore(m, t)) && !blocked_by_structure(g, t, near)) {
            best = t;
            best_d = d;
        }
    }
    return best;
}

/* Tile of p's nearest ready silo (finished, reloaded), or -1. */
static TileRef ready_silo_tile(const OfeGame *g, PlayerId p, TileRef tile) {
    const UnitTable *u = &g->units;
    TileRef best = -1;
    int32_t best_d = 0;
    for (int32_t i = 0; i < u->count; i++) {
        if (!u->alive[i] || u->type[i] != OFE_UNIT_MISSILE_SILO || u->owner[i] != p) continue;
        if ((u->flags[i] & OFE_UF_UNDER_CONSTRUCTION) || u->timer[i] > g->tick) continue;
        int32_t d = map_manhattan(g->map, u->tile[i], tile);
        if (best < 0 || d < best_d) {
            best = u->tile[i];
            best_d = d;
        }
    }
    return best;
}

/* Tile of p's nearest finished port on the same body of water as `tile`, or -1. */
static TileRef warship_port_tile(const OfeGame *g, PlayerId p, TileRef tile) {
    if (!map_is_water(g->map, tile)) return -1;
    OfeGame *mg = (OfeGame *)g; /* the water components are built lazily */
    int32_t comp = ships_water_component(mg, tile);
    if (comp < 0) return -1;
    const UnitTable *u = &g->units;
    TileRef best = -1;
    int32_t best_d = 0;
    for (int32_t i = 0; i < u->count; i++) {
        if (!u->alive[i] || u->type[i] != OFE_UNIT_PORT || u->owner[i] != p) continue;
        if (u->flags[i] & OFE_UF_UNDER_CONSTRUCTION) continue;
        int32_t d = map_manhattan(g->map, u->tile[i], tile);
        if (best >= 0 && d >= best_d) continue;
        if (!ships_touches_water(mg, u->tile[i], comp)) continue;
        best = u->tile[i];
        best_d = d;
    }
    return best;
}

static bool spawn_immunity_active(const OfeGame *g) {
    return g->start_tick < 0 || g->tick - g->start_tick < g->cfg.spawn_immunity_ticks;
}

/* PlayerImpl.nukeSpawn: the silo tile a nuke would launch from, or -1. */
static TileRef nuke_launch_tile(const OfeGame *g, PlayerId p, int type, TileRef target) {
    if (spawn_immunity_active(g) || map_is_impassable(g->map, target)) return -1;
    if (type == OFE_UNIT_MIRV && !tile_has_owner(g, target)) return -1;
    PlayerId owner = tile_owner(g, target);
    /* After the game ends teammates may nuke each other. */
    if (!g->game_over) {
        if (owner != 0 && owner != p && player_same_team(g, p, owner)) return -1;
        if (g->cfg.game_mode == OFE_MODE_TEAM && type != OFE_UNIT_MIRV) {
            /* Don't blow up a teammate's structures. */
            IntVec *hits = &g->sub->structures->scratch_units;
            hits->len = 0;
            NearbyQuery q = {-1, 0, false, hits};
            index_visit(g, target, type == OFE_UNIT_HYDROGEN_BOMB ? 100 : 30, nearby_match, &q);
            for (int32_t i = 0; i < hits->len; i++) {
                PlayerId o = g->units.owner[hits->data[i]];
                if (o != p && player_same_team(g, p, o)) return -1;
            }
        }
    }
    return ready_silo_tile(g, p, target);
}

TileRef structures_can_build(const OfeGame *g, PlayerId p, int type, TileRef tile) {
    if (!player_valid(g, p) || type < 0 || type >= OFE_UNIT_TYPE_COUNT) return -1;
    if (tile < 0 || tile >= g->map->num_tiles) return -1;
    if (g->cfg.disabled_units & (1u << type)) return -1;
    if (!player_alive(g, p) || game_in_spawn_phase(g)) return -1;
    if (g->players.gold[p] < unit_cost(g, p, type)) return -1;
    switch (type) {
    case OFE_UNIT_ATOM_BOMB:
    case OFE_UNIT_HYDROGEN_BOMB:
    case OFE_UNIT_MIRV: return nuke_launch_tile(g, p, type, tile);
    case OFE_UNIT_WARSHIP: return warship_port_tile(g, p, tile);
    case OFE_UNIT_PORT: return find_structure_tile(g, p, tile, true);
    case OFE_UNIT_CITY:
    case OFE_UNIT_FACTORY:
    case OFE_UNIT_DEFENSE_POST:
    case OFE_UNIT_MISSILE_SILO:
    case OFE_UNIT_SAM_LAUNCHER: return find_structure_tile(g, p, tile, false);
    default: return -1; /* boats come from BOAT_ATTACK, the rest are spawned by their modules */
    }
}

/* ---- Build / upgrade / delete ------------------------------------------------------ */

static UnitId build_structure(OfeGame *g, PlayerId p, int type, TileRef tile) {
    int64_t cost = unit_cost(g, p, type); /* before units_built grows */
    UnitId id = unit_create(g, type, p, tile);
    if (id < 0) return -1;
    int32_t slot = unit_lookup(g, id);
    g->players.units_built[p][type]++;
    player_remove_gold(g, p, cost);

    int32_t ticks = g->cfg.instant_build ? 0 : construction_ticks(type);
    unit_begin_construction(g, slot, g->tick + ticks);
    if (ticks == 0) unit_finish_construction(g, slot);
    else IntVec_push(&g->sub->structures->constructing, id);
    return id;
}

UnitId structures_build(OfeGame *g, PlayerId p, int type, TileRef tile, int32_t amount, int32_t flags) {
    TileRef at = structures_can_build(g, p, type, tile);
    if (at < 0) return -1;
    switch (type) {
    case OFE_UNIT_WARSHIP: {
        int32_t built = g->players.units_built[p][type];
        UnitId id = ships_build_warship(g, p, tile);
        /* The price escalates with units_built; make sure it is counted once. */
        if (id >= 0 && g->players.units_built[p][type] == built) g->players.units_built[p][type]++;
        return id;
    }
    case OFE_UNIT_ATOM_BOMB:
    case OFE_UNIT_HYDROGEN_BOMB:
    case OFE_UNIT_MIRV: {
        UnitId first = -1;
        int32_t count = type == OFE_UNIT_MIRV ? 1 : OFE_CLAMP(amount, 1, MAX_UPGRADE_AMOUNT);
        for (int32_t i = 0; i < count; i++) {
            /* Gold and silo readiness change with every launch. */
            if (i > 0 && structures_can_build(g, p, type, tile) < 0) break;
            UnitId id = nukes_launch(g, p, type, tile, -1, (flags & 1) != 0, false);
            if (id < 0) break;
            if (first < 0) first = id;
        }
        return first;
    }
    default:
        return build_structure(g, p, type, at);
    }
}

bool structures_upgrade(OfeGame *g, PlayerId p, UnitId id, int32_t amount) {
    int32_t slot = unit_lookup(g, id);
    if (slot < 0 || !player_valid(g, p) || g->units.owner[slot] != p) return false;
    int type = g->units.type[slot];
    if (!structure_upgradable(type) || (g->cfg.disabled_units & (1u << type)) || !player_alive(g, p)) return false;
    if (g->units.flags[slot] & (OFE_UF_UNDER_CONSTRUCTION | OFE_UF_MARKED_FOR_DELETION)) return false;
    bool any = false;
    for (int32_t i = 0; i < OFE_CLAMP(amount, 1, MAX_UPGRADE_AMOUNT) && g->units.level[slot] < 255; i++) {
        int64_t cost = unit_cost(g, p, type); /* escalates with units_built */
        if (g->players.gold[p] < cost) break;
        player_remove_gold(g, p, cost);
        unit_set_level(g, slot, g->units.level[slot] + 1);
        g->players.units_built[p][type]++;
        /* New silo missiles start reloading; SAM range ramps up. */
        if (type == OFE_UNIT_MISSILE_SILO || type == OFE_UNIT_SAM_LAUNCHER) nukes_on_level_up(g, slot);
        any = true;
    }
    return any;
}

/* PlayerImpl.canDeleteUnit / recordDeleteUnit: the 30 s cooldown counts from tick -1,
 * so nobody can delete during the first 299 ticks. */
bool structures_delete_ready(const OfeGame *g, PlayerId p) {
    return g->tick - g->sub->structures->last_delete[p] >= DELETE_COOLDOWN_TICKS;
}

void structures_delete_record(OfeGame *g, PlayerId p) { g->sub->structures->last_delete[p] = g->tick; }

bool structures_delete(OfeGame *g, PlayerId p, UnitId id) {
    int32_t slot = unit_lookup(g, id);
    if (slot < 0 || !player_valid(g, p) || g->units.owner[slot] != p) return false;
    const UnitTable *u = &g->units;
    if (!unit_is_structure(u->type[slot]) || (u->flags[slot] & OFE_UF_MARKED_FOR_DELETION)) return false;
    if (tile_owner(g, u->tile[slot]) != p || game_in_spawn_phase(g)) return false;
    StructuresState *s = g->sub->structures;
    if (!structures_delete_ready(g, p)) return false;
    structures_delete_record(g, p);
    g->units.flags[slot] |= OFE_UF_MARKED_FOR_DELETION;
    PendingDeletion d = {id, p, g->tick + DELETION_MARK_TICKS};
    PendingDeletionVec_push(&s->deletions, d);
    return true;
}

/* ---- Tick ------------------------------------------------------------------------------ */

void structures_tick(OfeGame *g) {
    StructuresState *s = g->sub->structures;

    int32_t keep = 0;
    for (int32_t i = 0; i < s->constructing.len; i++) {
        UnitId id = s->constructing.data[i];
        int32_t slot = unit_lookup(g, id);
        if (slot < 0) continue; /* destroyed while building */
        if (g->tick + 1 >= g->units.timer[slot]) { /* done after `duration` steps */
            unit_finish_construction(g, slot);
            continue;
        }
        s->constructing.data[keep++] = id;
    }
    s->constructing.len = keep;

    keep = 0;
    for (int32_t i = 0; i < s->deletions.len; i++) {
        PendingDeletion d = s->deletions.data[i];
        int32_t slot = unit_lookup(g, d.id);
        if (slot < 0) continue;
        if (g->units.owner[slot] != d.owner) {
            g->units.flags[slot] &= (uint8_t)~OFE_UF_MARKED_FOR_DELETION;
            continue;
        }
        if (g->tick - d.at > 0) {
            unit_destroy(g, d.id, 0);
            continue;
        }
        s->deletions.data[keep++] = d;
    }
    s->deletions.len = keep;
}

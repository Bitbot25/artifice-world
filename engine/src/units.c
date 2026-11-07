/*
 * units.c - the unit table (allocation, ids, ownership bookkeeping), unit
 * costs, spatial queries and the per-tick dispatch to unit subsystems.
 *
 * Behaviour lives in the subsystems: structures.c (construction, upgrades,
 * defense posts, silos), ships.c (boats, trade, warships), nukes.c (nukes
 * and SAMs), rail.c (factories and trains).
 */
#include "internal.h"

/* ---- Table ------------------------------------------------------------------ */

void units_table_init(UnitTable *t) { memset(t, 0, sizeof(*t)); }

void units_table_free(UnitTable *t) {
    UNIT_COLUMNS(FREE_COLUMN)
    for (int32_t i = 0; i < t->cap; i++) IntVec_free(&t->path[i]);
    free(t->path);
    for (int32_t i = 0; i < t->cap; i++) ts_water_stepper_free(&t->pf[i]);
    free(t->pf);
    IntVec_free(&t->free_slots);
    memset(t, 0, sizeof(*t));
}

static void units_table_grow(UnitTable *t, int32_t cap) {
    UNIT_COLUMNS(GROW_COLUMN)
    t->path = (IntVec *)ofe_xrealloc(t->path, (size_t)cap * sizeof(IntVec));
    memset(t->path + t->cap, 0, (size_t)(cap - t->cap) * sizeof(IntVec));
    t->pf = (TsWaterStepper *)ofe_xrealloc(t->pf, (size_t)cap * sizeof(TsWaterStepper));
    memset(t->pf + t->cap, 0, (size_t)(cap - t->cap) * sizeof(TsWaterStepper));
    t->cap = cap;
}

void units_table_copy(UnitTable *dst, const UnitTable *src) {
    if (dst->cap < src->cap) units_table_grow(dst, src->cap);
    {
        UNIT_COLUMNS(COPY_COLUMN)
    }
    for (int32_t i = 0; i < src->count; i++) {
        if (src->alive[i]) {
            IntVec_copy(&dst->path[i], &src->path[i]);
            ts_water_stepper_copy(&dst->pf[i], &src->pf[i]);
        } else {
            dst->path[i].len = 0;
            ts_water_stepper_invalidate(&dst->pf[i]);
        }
    }
    /* Rows past src->cap (dst was bigger) are dead in src. */
    for (int32_t i = src->cap; i < dst->cap; i++) {
        dst->alive[i] = 0;
        dst->path[i].len = 0;
        ts_water_stepper_invalidate(&dst->pf[i]);
    }
    dst->count = src->count;
    dst->num_alive = src->num_alive;
    IntVec_copy(&dst->free_slots, &src->free_slots);
}

int32_t unit_lookup(const OfeGame *g, UnitId id) {
    if (id < 0) return -1;
    int32_t slot = unit_slot(id);
    const UnitTable *t = &g->units;
    if (slot >= t->count || !t->alive[slot]) return -1;
    if (t->generation[slot] != (uint16_t)(id >> UNIT_SLOT_BITS)) return -1;
    return slot;
}

bool unit_is_structure(int type) {
    switch (type) {
    case OFE_UNIT_CITY:
    case OFE_UNIT_PORT:
    case OFE_UNIT_DEFENSE_POST:
    case OFE_UNIT_MISSILE_SILO:
    case OFE_UNIT_SAM_LAUNCHER:
    case OFE_UNIT_FACTORY:
        return true;
    default:
        return false;
    }
}

UnitId unit_create(OfeGame *g, int type, PlayerId owner, TileRef tile) {
    UnitTable *t = &g->units;
    int32_t slot;
    if (t->free_slots.len > 0) {
        slot = t->free_slots.data[--t->free_slots.len];
    } else {
        if (t->count == t->cap) units_table_grow(t, t->cap ? t->cap * 2 : 256);
        if (t->count > UNIT_SLOT_MASK) return -1;
        slot = t->count++;
    }
    uint16_t gen = (uint16_t)(t->generation[slot] + 1);
    /* Reset the row. */
#define RESET_COLUMN(type_, name) memset(&t->name[slot], 0, sizeof(type_));
    UNIT_COLUMNS(RESET_COLUMN)
#undef RESET_COLUMN
    t->path[slot].len = 0;
    ts_water_stepper_invalidate(&t->pf[slot]);
    t->alive[slot] = 1;
    t->generation[slot] = gen;
    t->type[slot] = (uint8_t)type;
    t->owner[slot] = owner;
    t->last_owner[slot] = owner;
    t->tile[slot] = tile;
    t->target_tile[slot] = -1;
    t->target_unit[slot] = -1;
    t->src_tile[slot] = tile;
    t->level[slot] = 1;
    t->created_tick[slot] = g->tick;
    t->num_alive++;
    g->players.unit_count[owner][type]++;
    /* Invariant: unit_levels = summed levels of finished structures. A new
     * structure is finished until unit_begin_construction says otherwise. */
    if (unit_is_structure(type)) g->players.unit_levels[owner][type] += 1;
    structures_index_add(g, slot);
    UnitId id = unit_id_of_slot(g, slot);
    event_emit(g, OFE_EV_UNIT_CREATED, id, type, owner, tile, 0);
    return id;
}

static bool counts_level(const OfeGame *g, int32_t slot) {
    return unit_is_structure(g->units.type[slot]) && !(g->units.flags[slot] & OFE_UF_UNDER_CONSTRUCTION);
}

void unit_destroy(OfeGame *g, UnitId id, PlayerId destroyer) {
    int32_t slot = unit_lookup(g, id);
    if (slot < 0) return;
    UnitTable *t = &g->units;
    PlayerId owner = t->owner[slot];
    int type = t->type[slot];
    event_emit(g, OFE_EV_UNIT_DESTROYED, id, type, owner, destroyer, 0);
    g->players.unit_count[owner][type]--;
    if (counts_level(g, slot)) g->players.unit_levels[owner][type] -= t->level[slot];
    if (t->flags[slot] & OFE_UF_UNDER_CONSTRUCTION) g->players.units_constructing[owner][type]--;
    bool rail = type == OFE_UNIT_CITY || type == OFE_UNIT_PORT || type == OFE_UNIT_FACTORY;
    structures_index_remove(g, slot);
    t->alive[slot] = 0;
    t->num_alive--;
    t->path[slot].len = 0;
    if (rail) rail_on_structure_changed(g, slot);
    IntVec_push(&t->free_slots, slot);
}

void unit_capture(OfeGame *g, UnitId id, PlayerId new_owner) {
    int32_t slot = unit_lookup(g, id);
    if (slot < 0) return;
    UnitTable *t = &g->units;
    PlayerId old = t->owner[slot];
    if (old == new_owner) return;
    int type = t->type[slot];
    g->players.unit_count[old][type]--;
    g->players.unit_count[new_owner][type]++;
    if (counts_level(g, slot)) {
        g->players.unit_levels[old][type] -= t->level[slot];
        g->players.unit_levels[new_owner][type] += t->level[slot];
    }
    if (t->flags[slot] & OFE_UF_UNDER_CONSTRUCTION) {
        g->players.units_constructing[old][type]--;
        g->players.units_constructing[new_owner][type]++;
    }
    t->last_owner[slot] = old;
    t->owner[slot] = new_owner;
    event_emit(g, OFE_EV_UNIT_CAPTURED, id, type, old, new_owner, 0);
    if (type == OFE_UNIT_CITY || type == OFE_UNIT_PORT || type == OFE_UNIT_FACTORY) rail_on_structure_changed(g, slot);
}

void unit_begin_construction(OfeGame *g, int32_t slot, int32_t end_tick) {
    UnitTable *t = &g->units;
    t->timer[slot] = end_tick;
    if (t->flags[slot] & OFE_UF_UNDER_CONSTRUCTION) return;
    /* Levels count only for finished structures. */
    if (counts_level(g, slot)) g->players.unit_levels[t->owner[slot]][t->type[slot]] -= t->level[slot];
    t->flags[slot] |= OFE_UF_UNDER_CONSTRUCTION;
    g->players.units_constructing[t->owner[slot]][t->type[slot]]++;
}

void unit_finish_construction(OfeGame *g, int32_t slot) {
    UnitTable *t = &g->units;
    if (!(t->flags[slot] & OFE_UF_UNDER_CONSTRUCTION)) return;
    t->flags[slot] &= (uint8_t)~OFE_UF_UNDER_CONSTRUCTION;
    g->players.units_constructing[t->owner[slot]][t->type[slot]]--;
    if (unit_is_structure(t->type[slot])) g->players.unit_levels[t->owner[slot]][t->type[slot]] += t->level[slot];
    int type = t->type[slot];
    if (type == OFE_UNIT_CITY || type == OFE_UNIT_PORT || type == OFE_UNIT_FACTORY) rail_on_structure_changed(g, slot);
}

void unit_set_level(OfeGame *g, int32_t slot, int level) {
    UnitTable *t = &g->units;
    if (counts_level(g, slot)) g->players.unit_levels[t->owner[slot]][t->type[slot]] += level - t->level[slot];
    t->level[slot] = (uint8_t)OFE_CLAMP(level, 0, 255);
}

void unit_move(OfeGame *g, int32_t slot, TileRef tile) { g->units.tile[slot] = tile; }

/* ---- Costs -------------------------------------------------------------------- */

/* OpenFront counts min(owned, constructed) per type, where owned counts a
 * finished structure's level and 1 for one under construction (a unit that
 * isn't a structure counts 1). O(1) from the per-player counters. */
static int32_t cost_count(const OfeGame *g, PlayerId p, int type) {
    const PlayerTable *pl = &g->players;
    int32_t owned = unit_is_structure(type) ? pl->unit_levels[p][type] + pl->units_constructing[p][type]
                                            : pl->unit_count[p][type];
    return OFE_MIN(owned, pl->units_built[p][type]);
}

static int64_t doubling_cost(int32_t n) {
    if (n >= 3) return 1000000;
    return OFE_MIN((int64_t)1000000, ((int64_t)1 << n) * 125000);
}

int64_t unit_cost(const OfeGame *g, PlayerId p, int type) {
    if (g->players.type[p] == OFE_PLAYER_HUMAN && g->cfg.infinite_gold) return 0;
    switch (type) {
    case OFE_UNIT_WARSHIP: return OFE_MIN((int64_t)1000000, (int64_t)(cost_count(g, p, type) + 1) * 250000);
    case OFE_UNIT_PORT:
    case OFE_UNIT_FACTORY:
        return doubling_cost(cost_count(g, p, OFE_UNIT_PORT) + cost_count(g, p, OFE_UNIT_FACTORY));
    case OFE_UNIT_CITY: return doubling_cost(cost_count(g, p, OFE_UNIT_CITY));
    case OFE_UNIT_ATOM_BOMB: return 750000;
    case OFE_UNIT_HYDROGEN_BOMB: return 5000000;
    case OFE_UNIT_MIRV: return 25000000 + (int64_t)g->mirvs_launched * 15000000;
    case OFE_UNIT_MISSILE_SILO: return 1000000;
    case OFE_UNIT_DEFENSE_POST: return OFE_MIN((int64_t)250000, (int64_t)(cost_count(g, p, type) + 1) * 50000);
    case OFE_UNIT_SAM_LAUNCHER: return OFE_MIN((int64_t)3000000, (int64_t)(cost_count(g, p, type) + 1) * 1500000);
    default: return 0;
    }
}

/* ---- Spatial queries ------------------------------------------------------------- */

bool units_has_nearby(const OfeGame *g, TileRef tile, int32_t range, int type, PlayerId owner) {
    if (unit_is_structure(type)) return structures_has_nearby(g, tile, range, type, owner);
    const UnitTable *t = &g->units;
    int64_t r2 = (int64_t)range * range;
    for (int32_t i = 0; i < t->count; i++) {
        if (!t->alive[i] || t->type[i] != type) continue;
        if (owner != 0 && t->owner[i] != owner) continue;
        if (t->flags[i] & OFE_UF_UNDER_CONSTRUCTION) continue;
        if (map_dist2(g->map, tile, t->tile[i]) <= r2) return true;
    }
    return false;
}

int32_t units_nearby(const OfeGame *g, TileRef tile, int32_t range, int type, PlayerId owner, int32_t *out, int32_t cap) {
    if (type >= 0 && unit_is_structure(type)) return structures_nearby(g, tile, range, type, owner, out, cap);
    const UnitTable *t = &g->units;
    int64_t r2 = (int64_t)range * range;
    int32_t n = 0;
    for (int32_t i = 0; i < t->count && n < cap; i++) {
        if (!t->alive[i] || (type >= 0 && t->type[i] != type)) continue;
        if (owner != 0 && t->owner[i] != owner) continue;
        if (map_dist2(g->map, tile, t->tile[i]) <= r2) out[n++] = i;
    }
    /* Nearest first (insertion sort; n is small). */
    for (int32_t i = 1; i < n; i++) {
        int32_t s = out[i];
        int64_t d = map_dist2(g->map, tile, t->tile[s]);
        int32_t j = i - 1;
        while (j >= 0 && map_dist2(g->map, tile, t->tile[out[j]]) > d) {
            out[j + 1] = out[j];
            j--;
        }
        out[j + 1] = s;
    }
    return n;
}

/* ---- Tick --------------------------------------------------------------------------- */

void units_check_structure_owners(OfeGame *g) {
    UnitTable *t = &g->units;
    for (int32_t i = 0; i < t->count; i++) {
        if (!t->alive[i] || !unit_is_structure(t->type[i])) continue;
        PlayerId owner = tile_owner(g, t->tile[i]);
        if (owner == t->owner[i]) continue;
        UnitId id = unit_id_of_slot(g, i);
        if (owner == 0) {
            unit_destroy(g, id, 0);
        } else if (t->type[i] == OFE_UNIT_DEFENSE_POST) {
            /* Defense posts are destroyed, not captured. */
            unit_destroy(g, id, owner);
        } else {
            unit_capture(g, id, owner);
        }
    }
}

void units_tick(OfeGame *g) {
    units_check_structure_owners(g);
    structures_tick(g);
    ships_tick(g);
    nukes_tick(g);
    rail_tick(g);
}

/* ---- Public queries ------------------------------------------------------------------- */

int32_t ofe_game_num_units(const OfeGame *g) { return g->units.num_alive; }

int32_t ofe_game_units(const OfeGame *g, OfeUnitInfo *out, int32_t cap) {
    const UnitTable *t = &g->units;
    int32_t n = 0;
    for (int32_t i = 0; i < t->count && n < cap; i++) {
        if (!t->alive[i]) continue;
        OfeUnitInfo *o = &out[n++];
        o->id = unit_id_of_slot(g, i);
        o->type = t->type[i];
        o->owner = t->owner[i];
        o->tile = t->tile[i];
        o->target_tile = t->target_tile[i];
        o->level = t->level[i];
        o->flags = t->flags[i];
        o->health = t->health[i];
        o->troops = t->troops[i];
        o->ticks_left = t->timer[i] > g->tick ? t->timer[i] - g->tick : 0;
        o->reserved = 0;
    }
    return n;
}

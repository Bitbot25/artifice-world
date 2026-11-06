/*
 * edit.c - the state-edit API: direct changes for sensitivity tests,
 * scenario setup and importing states from the TypeScript engine.
 */
#include "internal.h"

static bool valid_tile(const OfeGame *g, int32_t t) { return t >= 0 && t < g->map->num_tiles; }

static void mark_spawned(OfeGame *g, PlayerId p, TileRef t) {
    if (p > 0 && !g->players.spawned[p]) {
        g->players.spawned[p] = 1;
        g->players.spawn_tile[p] = t;
    }
    if (p > 0) g->players.dead[p] = 0;
}

int32_t ofe_edit_set_owner(OfeGame *g, int32_t tile, int32_t owner) {
    if (!valid_tile(g, tile)) return -1;
    if (owner != 0 && (!player_valid(g, owner) || !map_is_ownable(g->map, tile))) return -2;
    tile_set_owner(g, tile, owner);
    mark_spawned(g, owner, tile);
    return 0;
}

int32_t ofe_edit_set_owner_map(OfeGame *g, const uint16_t *owners) {
    for (int32_t t = 0; t < g->map->num_tiles; t++) {
        PlayerId o = owners[t] & TS_OWNER_MASK;
        if (o != 0 && (!player_valid(g, o) || !map_is_ownable(g->map, t))) continue;
        tile_set_owner(g, t, o);
        mark_spawned(g, o, t);
    }
    return 0;
}

int32_t ofe_edit_set_fallout(OfeGame *g, int32_t tile, int32_t fallout) {
    if (!valid_tile(g, tile) || !map_is_land(g->map, tile)) return -1;
    if (fallout && tile_has_owner(g, tile)) tile_set_owner(g, tile, 0);
    tile_set_fallout(g, tile, fallout != 0);
    return 0;
}

int32_t ofe_edit_set_troops(OfeGame *g, int32_t p, double troops) {
    if (!player_valid(g, p) || troops < 0) return -1;
    g->players.troops[p] = (int64_t)floor(troops);
    return 0;
}

int32_t ofe_edit_set_gold(OfeGame *g, int32_t p, int64_t gold) {
    if (!player_valid(g, p) || gold < 0) return -1;
    g->players.gold[p] = gold;
    return 0;
}

int32_t ofe_edit_add_unit(OfeGame *g, int32_t type, int32_t owner, int32_t tile, int32_t level) {
    if (!player_valid(g, owner) || !valid_tile(g, tile) || type < 0 || type >= OFE_UNIT_TYPE_COUNT) return -1;
    UnitId id = unit_create(g, type, owner, tile);
    if (id < 0) return -2;
    int32_t slot = unit_lookup(g, id);
    if (unit_is_structure(type)) {
        /* Created finished: its whole level counts toward the owner's
         * totals (unit_create doesn't count structure levels). */
        int32_t lvl = OFE_CLAMP(level, 1, 255);
        g->players.units_built[owner][type] += lvl;
        g->units.level[slot] = (uint8_t)lvl;
        g->players.unit_levels[owner][type] += lvl;
    }
    if (type == OFE_UNIT_WARSHIP) g->units.health[slot] = 1000;
    if (type == OFE_UNIT_CITY || type == OFE_UNIT_PORT || type == OFE_UNIT_FACTORY) rail_on_structure_changed(g, slot);
    return id;
}

int32_t ofe_edit_remove_unit(OfeGame *g, int32_t id) {
    if (unit_lookup(g, id) < 0) return -1;
    unit_destroy(g, id, 0);
    return 0;
}

int32_t ofe_edit_set_alliance(OfeGame *g, int32_t a, int32_t b, int32_t allied, int32_t duration) {
    if (!player_valid(g, a) || !player_valid(g, b) || a == b) return -1;
    int32_t i = alliance_find(g, a, b);
    if (allied && i < 0) alliance_form(g, a, b, duration);
    if (!allied && i >= 0) {
        g->alliances.data[i] = g->alliances.data[--g->alliances.len];
        event_emit(g, OFE_EV_ALLIANCE_EXPIRED, 0, a, b, 0, 0);
    }
    return 0;
}

int32_t ofe_edit_launch_nuke(OfeGame *g, int32_t type, int32_t owner, int32_t src_tile, int32_t dst_tile) {
    if (!player_valid(g, owner) || !valid_tile(g, src_tile) || !valid_tile(g, dst_tile)) return -1;
    if (type != OFE_UNIT_ATOM_BOMB && type != OFE_UNIT_HYDROGEN_BOMB && type != OFE_UNIT_MIRV) return -1;
    /* Launch from a temporary silo position: nukes_launch with src_slot -1
     * picks a silo, so create the nuke through a free launch from src. */
    UnitId silo = unit_create(g, OFE_UNIT_MISSILE_SILO, owner, src_tile);
    int32_t slot = unit_lookup(g, silo);
    UnitId nuke = nukes_launch(g, owner, type, dst_tile, slot, true, true);
    unit_destroy(g, silo, 0);
    return nuke;
}

int32_t ofe_edit_set_relation(OfeGame *g, int32_t nation, int32_t other, double value) {
    if (!player_valid(g, nation) || !player_valid(g, other)) return -1;
    if (g->players.relation_row[nation] < 0) return -2;
    player_update_relation(g, nation, other, OFE_CLAMP(value, -100.0, 100.0) - player_relation(g, nation, other));
    return 0;
}

int32_t ofe_edit_set_disconnected(OfeGame *g, int32_t p, int32_t disconnected) {
    if (!player_valid(g, p)) return -1;
    g->players.disconnected[p] = (uint8_t)(disconnected != 0);
    event_emit(g, OFE_EV_PLAYER_DISCONNECTED, 0, p, disconnected != 0, 0, 0);
    return 0;
}

int32_t ofe_edit_end_spawn_phase(OfeGame *g) {
    spawn_phase_end(g);
    return 0;
}

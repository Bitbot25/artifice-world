/*
 * test_integration.c - cross-module flows driven only through intents and
 * the public API, the way an agent plays: build -> construction -> effect,
 * nukes through BUILD_UNIT, factories feeding trains, alliances cancelling
 * nukes, capture handing structures and stations over.
 */
#include "test_util.h"

static OfeGame *two_player_game(OfeMap *m, uint64_t seed) {
    OfeConfig c;
    ofe_config_default(&c);
    c.num_humans = 2;
    c.num_bots = 0;
    c.num_nations = 0;
    c.spawn_phase_ticks = 0;
    c.auto_spawn_humans = 0;
    c.spawn_immunity_ticks = 0;
    OfeGame *g = ofe_game_create(m, &c, seed);
    /* Player 1 on the west half, player 2 on the east half. */
    for (int y = 10; y < m->height - 10; y++) {
        for (int x = 10; x < m->width - 10; x++) {
            TileRef t = map_ref(m, x, y);
            if (!map_is_ownable(m, t)) continue;
            ofe_edit_set_owner(g, t, x < m->width / 2 ? 1 : 2);
        }
    }
    ofe_edit_set_troops(g, 1, 100000);
    ofe_edit_set_troops(g, 2, 100000);
    return g;
}

static OfeIntent build(PlayerId p, int type, TileRef tile) {
    OfeIntent in = {0};
    in.type = OFE_INTENT_BUILD_UNIT;
    in.player = p;
    in.unit_type = type;
    in.tile = tile;
    in.amount = 1;
    in.troops = -1;
    return in;
}

static int32_t count_units(const OfeGame *g, PlayerId p, int type, bool finished_only) {
    int32_t n = 0;
    const UnitTable *u = &g->units;
    for (int32_t i = 0; i < u->count; i++) {
        if (!u->alive[i] || u->owner[i] != p || u->type[i] != type) continue;
        if (finished_only && (u->flags[i] & OFE_UF_UNDER_CONSTRUCTION)) continue;
        n++;
    }
    return n;
}

static int32_t first_unit(const OfeGame *g, PlayerId p, int type) {
    const UnitTable *u = &g->units;
    for (int32_t i = 0; i < u->count; i++) {
        if (u->alive[i] && u->owner[i] == p && u->type[i] == type) return unit_id_of_slot(g, i);
    }
    return -1;
}

/* A city built by intent finishes construction and raises max troops. */
static void test_city_via_intent(OfeMap *m) {
    OfeGame *g = two_player_game(m, 1);
    ofe_edit_set_gold(g, 1, 10000000);
    double max_before = player_max_troops(g, 1);
    int64_t gold_before = g->players.gold[1];
    OfeIntent in = build(1, OFE_UNIT_CITY, map_ref(m, 40, 50));
    ofe_game_push_intents(g, &in, 1);
    ofe_game_step(g, 1);
    CHECK(count_units(g, 1, OFE_UNIT_CITY, false) == 1);
    CHECK(g->players.gold[1] < gold_before);
    ofe_game_step(g, 30);
    CHECK(count_units(g, 1, OFE_UNIT_CITY, true) == 1);
    CHECK(player_max_troops(g, 1) >= max_before + 250000 - 1);
    check_invariants(g);
    ofe_game_destroy(g);
}

/* Silo by intent, then an atom bomb by intent: one charge, a detonation
 * that clears enemy land and leaves fallout. */
static void test_nuke_via_intent(OfeMap *m) {
    OfeGame *g = two_player_game(m, 2);
    ofe_edit_set_gold(g, 1, 10000000);
    OfeIntent silo = build(1, OFE_UNIT_MISSILE_SILO, map_ref(m, 30, 30));
    ofe_game_push_intents(g, &silo, 1);
    ofe_game_step(g, 120);
    CHECK(count_units(g, 1, OFE_UNIT_MISSILE_SILO, true) == 1);

    int64_t gold = g->players.gold[1];
    int32_t enemy_tiles = g->players.num_tiles[2];
    TileRef target = map_ref(m, 120, 60);
    OfeIntent nuke = build(1, OFE_UNIT_ATOM_BOMB, target);
    ofe_game_push_intents(g, &nuke, 1);
    ofe_game_step(g, 1);
    CHECK(count_units(g, 1, OFE_UNIT_ATOM_BOMB, false) == 1);
    /* Gold: the bomb's 750k once, plus a tick of income. */
    CHECK(gold - g->players.gold[1] == 750000 - 100);
    ofe_game_step(g, 200);
    CHECK(count_units(g, 1, OFE_UNIT_ATOM_BOMB, false) == 0);
    CHECK(g->players.num_tiles[2] < enemy_tiles);
    CHECK(g->num_fallout_tiles > 0);
    CHECK(tile_owner(g, target) == 0 && tile_has_fallout(g, target));
    check_invariants(g);
    ofe_game_destroy(g);
}

/* Forming an alliance calls off a nuke in flight between the two. */
static void test_alliance_cancels_nuke(OfeMap *m) {
    OfeGame *g = two_player_game(m, 3);
    ofe_edit_add_unit(g, OFE_UNIT_MISSILE_SILO, 1, map_ref(m, 20, 20), 1);
    ofe_edit_set_gold(g, 1, 10000000);
    OfeIntent nuke = build(1, OFE_UNIT_ATOM_BOMB, map_ref(m, 130, 90));
    ofe_game_push_intents(g, &nuke, 1);
    ofe_game_step(g, 2);
    CHECK(count_units(g, 1, OFE_UNIT_ATOM_BOMB, false) == 1);
    OfeIntent req = {0};
    req.type = OFE_INTENT_ALLIANCE_REQUEST;
    req.player = 1;
    req.target = 2;
    OfeIntent back = req;
    back.player = 2;
    back.target = 1;
    OfeIntent both[2] = {req, back};
    ofe_game_push_intents(g, both, 2);
    ofe_game_step(g, 1);
    CHECK(player_allied(g, 1, 2));
    CHECK(count_units(g, 1, OFE_UNIT_ATOM_BOMB, false) == 0);
    ofe_game_destroy(g);
}

/* A factory and two cities form a rail network and trains earn gold. */
static void test_factory_trains(OfeMap *m) {
    OfeGame *g = two_player_game(m, 4);
    ofe_edit_set_gold(g, 1, 50000000);
    OfeIntent ins[3] = {build(1, OFE_UNIT_FACTORY, map_ref(m, 40, 60)), build(1, OFE_UNIT_CITY, map_ref(m, 20, 25)),
                        build(1, OFE_UNIT_CITY, map_ref(m, 70, 100))};
    ofe_game_push_intents(g, ins, 3);
    ofe_game_step(g, 40);
    CHECK(count_units(g, 1, OFE_UNIT_FACTORY, true) == 1);
    CHECK(rail_station_count(g) >= 2);
    ofe_game_step(g, 3000);
    CHECK(g->players.train_gold[1] > 0);
    check_invariants(g);
    ofe_game_destroy(g);
}

/* Conquering a city's tile captures it; a defense post is destroyed. */
static void test_capture(OfeMap *m) {
    OfeGame *g = two_player_game(m, 5);
    TileRef city_tile = map_ref(m, m->width / 2 + 3, 90); /* clear of the central lake */
    TileRef post_tile = map_ref(m, m->width / 2 + 3, 30);
    UnitId city = ofe_edit_add_unit(g, OFE_UNIT_CITY, 2, city_tile, 1);
    ofe_edit_add_unit(g, OFE_UNIT_DEFENSE_POST, 2, post_tile, 1);
    /* Push the front 6 columns east (a lone flipped tile would be an
     * enclave and get annexed straight back). */
    for (int y = 0; y < m->height; y++) {
        for (int x = m->width / 2; x < m->width / 2 + 6; x++) {
            TileRef t = map_ref(m, x, y);
            if (tile_owner(g, t) == 2) ofe_edit_set_owner(g, t, 1);
        }
    }
    ofe_game_step(g, 1);
    int32_t slot = unit_lookup(g, city);
    CHECK(slot >= 0 && g->units.owner[slot] == 1);
    CHECK(count_units(g, 2, OFE_UNIT_DEFENSE_POST, false) == 0);
    CHECK(count_units(g, 1, OFE_UNIT_DEFENSE_POST, false) == 0);
    CHECK(g->players.unit_levels[1][OFE_UNIT_CITY] == 1);
    CHECK(g->players.unit_levels[2][OFE_UNIT_CITY] == 0);
    ofe_game_destroy(g);
}

/* Upgrade and delete through intents. */
static void test_upgrade_delete(OfeMap *m) {
    OfeGame *g = two_player_game(m, 6);
    ofe_edit_set_gold(g, 1, 10000000);
    OfeIntent in = build(1, OFE_UNIT_CITY, map_ref(m, 40, 50));
    ofe_game_push_intents(g, &in, 1);
    ofe_game_step(g, 30);
    UnitId city = first_unit(g, 1, OFE_UNIT_CITY);
    CHECK(city >= 0);
    OfeIntent up = {0};
    up.type = OFE_INTENT_UPGRADE_STRUCTURE;
    up.player = 1;
    up.target = city;
    up.amount = 2;
    ofe_game_push_intents(g, &up, 1);
    ofe_game_step(g, 1);
    CHECK(g->units.level[unit_lookup(g, city)] == 3);
    CHECK(g->players.unit_levels[1][OFE_UNIT_CITY] == 3);
    ofe_game_step(g, 300); /* nobody may delete before tick 299 (OpenFront's cooldown counts from tick -1) */
    OfeIntent del = {0};
    del.type = OFE_INTENT_DELETE_UNIT;
    del.player = 1;
    del.target = city;
    ofe_game_push_intents(g, &del, 1);
    ofe_game_step(g, 400);
    CHECK(unit_lookup(g, city) < 0);
    CHECK(g->players.unit_levels[1][OFE_UNIT_CITY] == 0);
    ofe_game_destroy(g);
}

/* Everything at once, cloned midway: both halves stay identical. */
static void test_full_clone_determinism(OfeMap *m) {
    OfeConfig c;
    ofe_config_default(&c);
    c.num_humans = 2;
    c.num_bots = 10;
    c.spawn_phase_ticks = 5;
    c.humans_use_ai = 1;
    OfeGame *g = ofe_game_create(m, &c, 77);
    ofe_game_step(g, 200);
    ofe_edit_add_unit(g, OFE_UNIT_MISSILE_SILO, 1, g->players.spawn_tile[1], 2);
    ofe_edit_set_gold(g, 1, 20000000);
    ofe_game_step(g, 300);
    OfeGame *k = ofe_game_clone(g);
    ofe_game_step(g, 800);
    ofe_game_step(k, 800);
    CHECK(ofe_game_hash(g) == ofe_game_hash(k));
    check_invariants(g);
    ofe_game_destroy(g);
    ofe_game_destroy(k);
}

int main(void) {
    OfeMap *m = make_map(160, 120);
    test_city_via_intent(m);
    test_nuke_via_intent(m);
    test_alliance_cancels_nuke(m);
    test_factory_trains(m);
    test_capture(m);
    test_upgrade_delete(m);
    test_full_clone_determinism(m);
    ofe_map_release(m);
    return test_summary("test_integration");
}

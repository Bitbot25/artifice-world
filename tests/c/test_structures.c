/*
 * test_structures.c - structures: placement rules, costs, construction,
 * upgrades, deletion, defense posts, capture, the spatial index and clones.
 * Uses a synthetic map; players and land are set up with the edit API.
 */
#include "test_util.h"

#define W 160
#define H 120

/* A started game (no spawn phase) with two human players and nothing else. */
static OfeGame *new_game(OfeMap *m, uint64_t seed, int instant_build) {
    OfeConfig c = base_config();
    c.num_bots = 0;
    c.num_nations = 0;
    c.spawn_phase_ticks = 0;
    c.instant_build = instant_build;
    OfeGame *g = ofe_game_create(m, &c, seed);
    ofe_game_step(g, 60); /* past spawn immunity */
    return g;
}

static void own_rect(OfeGame *g, PlayerId p, int x0, int y0, int x1, int y1) {
    for (int y = y0; y < y1; y++)
        for (int x = x0; x < x1; x++) ofe_edit_set_owner(g, map_ref(g->map, x, y), p);
}

static int32_t slot_of(OfeGame *g, UnitId id) { return unit_lookup(g, id); }

static void test_city_build_and_cost(OfeMap *m) {
    OfeGame *g = new_game(m, 1, 0);
    own_rect(g, 1, 10, 10, 100, 50);
    ofe_edit_set_gold(g, 1, 10000000);
    TileRef t0 = map_ref(m, 20, 20);

    CHECK(structures_can_build(g, 1, OFE_UNIT_CITY, t0) == t0);
    int64_t gold = g->players.gold[1];
    double max_before = player_max_troops(g, 1);
    UnitId id = structures_build(g, 1, OFE_UNIT_CITY, t0, 1, 0);
    CHECK(id >= 0);
    int32_t s = slot_of(g, id);
    CHECK(s >= 0 && g->units.tile[s] == t0 && g->units.owner[s] == 1);
    CHECK(g->units.flags[s] & OFE_UF_UNDER_CONSTRUCTION);
    CHECK(gold - g->players.gold[1] == 125000);
    CHECK(g->players.units_built[1][OFE_UNIT_CITY] == 1);
    CHECK(g->players.unit_count[1][OFE_UNIT_CITY] == 1);
    CHECK(g->players.unit_levels[1][OFE_UNIT_CITY] == 0); /* not finished */

    /* Finishes after the construction time (2 s = 20 ticks). */
    ofe_game_step(g, 19);
    CHECK(g->units.flags[s] & OFE_UF_UNDER_CONSTRUCTION);
    ofe_game_step(g, 1);
    CHECK(!(g->units.flags[s] & OFE_UF_UNDER_CONSTRUCTION));
    CHECK(g->players.unit_levels[1][OFE_UNIT_CITY] == 1);
    CHECK(player_max_troops(g, 1) > max_before); /* a city raises the troop cap */

    /* Costs escalate: 125k, 250k, 500k, 1M, 1M. */
    int64_t expect[4] = {250000, 500000, 1000000, 1000000};
    for (int i = 0; i < 4; i++) {
        TileRef t = map_ref(m, 20 + 16 * (i + 1), 20);
        CHECK(unit_cost(g, 1, OFE_UNIT_CITY) == expect[i]);
        gold = g->players.gold[1];
        CHECK(structures_build(g, 1, OFE_UNIT_CITY, t, 1, 0) >= 0);
        CHECK(gold - g->players.gold[1] == expect[i]);
    }
    check_invariants(g);
    ofe_game_destroy(g);
}

static void test_structure_types_and_durations(OfeMap *m) {
    struct { int type; int ticks; } cases[] = {
        {OFE_UNIT_CITY, 20}, {OFE_UNIT_FACTORY, 20}, {OFE_UNIT_DEFENSE_POST, 50},
        {OFE_UNIT_MISSILE_SILO, 100}, {OFE_UNIT_SAM_LAUNCHER, 300},
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        OfeGame *g = new_game(m, 2, 0);
        own_rect(g, 1, 10, 10, 60, 50);
        ofe_edit_set_gold(g, 1, 50000000);
        TileRef t = map_ref(m, 30, 30);
        int64_t cost = unit_cost(g, 1, cases[i].type);
        int64_t gold = g->players.gold[1];
        UnitId id = structures_build(g, 1, cases[i].type, t, 1, 0);
        CHECK(id >= 0);
        CHECK(gold - g->players.gold[1] == cost);
        int32_t s = slot_of(g, id);
        ofe_game_step(g, cases[i].ticks - 1);
        CHECK(s >= 0 && (g->units.flags[s] & OFE_UF_UNDER_CONSTRUCTION));
        ofe_game_step(g, 1);
        CHECK(!(g->units.flags[s] & OFE_UF_UNDER_CONSTRUCTION));
        ofe_game_destroy(g);
    }

    /* Port on a shore tile. */
    OfeGame *g = new_game(m, 2, 0);
    own_rect(g, 1, 4, 10, 30, 40);
    ofe_edit_set_gold(g, 1, 50000000);
    UnitId id = structures_build(g, 1, OFE_UNIT_PORT, map_ref(m, 8, 20), 1, 0);
    CHECK(id >= 0);
    int32_t s = slot_of(g, id);
    CHECK(s >= 0 && map_is_shore(m, g->units.tile[s]));
    CHECK(map_dist2(m, g->units.tile[s], map_ref(m, 8, 20)) < 225);
    ofe_game_step(g, 50);
    CHECK(!(g->units.flags[s] & OFE_UF_UNDER_CONSTRUCTION));
    /* Ports and factories share a price ladder. */
    CHECK(unit_cost(g, 1, OFE_UNIT_FACTORY) == 250000);
    ofe_game_destroy(g);

    /* Instant build finishes at once. */
    g = new_game(m, 2, 1);
    own_rect(g, 1, 10, 10, 60, 50);
    ofe_edit_set_gold(g, 1, 50000000);
    id = structures_build(g, 1, OFE_UNIT_SAM_LAUNCHER, map_ref(m, 30, 30), 1, 0);
    CHECK(id >= 0);
    CHECK(!(g->units.flags[slot_of(g, id)] & OFE_UF_UNDER_CONSTRUCTION));
    CHECK(g->players.unit_levels[1][OFE_UNIT_SAM_LAUNCHER] == 1);
    ofe_game_destroy(g);
}

static void test_placement_rules(OfeMap *m) {
    OfeGame *g = new_game(m, 3, 0);
    own_rect(g, 1, 10, 10, 70, 50);
    own_rect(g, 2, 70, 10, 120, 50);
    ofe_edit_set_gold(g, 1, 100000000);
    TileRef mine = map_ref(m, 30, 30);

    /* Not my tile / nobody's tile. */
    CHECK(structures_can_build(g, 1, OFE_UNIT_CITY, map_ref(m, 90, 30)) == -1);
    CHECK(structures_can_build(g, 1, OFE_UNIT_CITY, map_ref(m, 100, 100)) == -1);
    /* Not on land. */
    CHECK(structures_can_build(g, 1, OFE_UNIT_CITY, map_ref(m, 1, 1)) == -1);
    /* Bad arguments. */
    CHECK(structures_can_build(g, 1, OFE_UNIT_TRANSPORT_SHIP, mine) == -1);
    CHECK(structures_can_build(g, 1, -1, mine) == -1);
    CHECK(structures_can_build(g, 0, OFE_UNIT_CITY, mine) == -1);

    /* A port needs a shore: the interior of the land is too far from water. */
    CHECK(structures_can_build(g, 1, OFE_UNIT_PORT, mine) == -1);

    /* Too close to another structure: the request snaps to the nearest free tile. */
    CHECK(structures_build(g, 1, OFE_UNIT_CITY, mine, 1, 0) >= 0);
    TileRef near = map_ref(m, 35, 30);
    TileRef snap = structures_can_build(g, 1, OFE_UNIT_DEFENSE_POST, near);
    CHECK(snap >= 0);
    CHECK(map_dist2(m, snap, mine) >= 225);
    CHECK(map_dist2(m, snap, near) < 225);
    CHECK(tile_owner(g, snap) == 1);
    /* Any structure type blocks, and so does an enemy one. */
    ofe_edit_add_unit(g, OFE_UNIT_CITY, 2, map_ref(m, 80, 30), 1);
    TileRef edge = map_ref(m, 66, 30); /* mine, 14 from the enemy city */
    TileRef s2 = structures_can_build(g, 1, OFE_UNIT_FACTORY, edge);
    CHECK(s2 >= 0 && map_dist2(m, s2, map_ref(m, 80, 30)) >= 225);

    /* Nowhere to put it: a small plot surrounded by structures. */
    OfeGame *h = new_game(m, 3, 0);
    own_rect(h, 1, 10, 10, 30, 30);
    ofe_edit_set_gold(h, 1, 100000000);
    ofe_edit_add_unit(h, OFE_UNIT_CITY, 1, map_ref(m, 20, 20), 1);
    CHECK(structures_can_build(h, 1, OFE_UNIT_CITY, map_ref(m, 22, 22)) == -1);
    CHECK(structures_build(h, 1, OFE_UNIT_CITY, map_ref(m, 22, 22), 1, 0) == -1);
    CHECK(h->units.num_alive == 1);
    ofe_game_destroy(h);

    /* Not enough gold. */
    ofe_edit_set_gold(g, 1, 40000);
    int32_t before = g->units.num_alive;
    CHECK(structures_can_build(g, 1, OFE_UNIT_DEFENSE_POST, map_ref(m, 50, 40)) == -1);
    CHECK(structures_build(g, 1, OFE_UNIT_MISSILE_SILO, map_ref(m, 50, 40), 1, 0) == -1);
    CHECK(g->units.num_alive == before && g->players.gold[1] == 40000);
    ofe_game_destroy(g);

    /* Disabled unit. */
    OfeConfig c = base_config();
    c.num_bots = 0;
    c.num_nations = 0;
    c.spawn_phase_ticks = 0;
    c.disabled_units = 1u << OFE_UNIT_CITY;
    g = ofe_game_create(m, &c, 3);
    ofe_game_step(g, 60);
    own_rect(g, 1, 10, 10, 60, 50);
    ofe_edit_set_gold(g, 1, 100000000);
    CHECK(structures_can_build(g, 1, OFE_UNIT_CITY, mine) == -1);
    CHECK(structures_can_build(g, 1, OFE_UNIT_FACTORY, mine) == mine);
    ofe_game_destroy(g);

    /* Spawn phase. */
    c.disabled_units = 0;
    c.spawn_phase_ticks = 20;
    g = ofe_game_create(m, &c, 3);
    own_rect(g, 1, 10, 10, 60, 50);
    ofe_edit_set_gold(g, 1, 100000000);
    CHECK(game_in_spawn_phase(g));
    CHECK(structures_can_build(g, 1, OFE_UNIT_CITY, mine) == -1);
    ofe_game_step(g, 22); /* the phase ends during tick 21 (tick > 20), as in OpenFront */
    CHECK(structures_can_build(g, 1, OFE_UNIT_CITY, mine) == mine);
    ofe_game_destroy(g);
}

static void test_build_intent(OfeMap *m) {
    OfeGame *g = new_game(m, 4, 0);
    own_rect(g, 1, 10, 10, 60, 50);
    ofe_edit_set_gold(g, 1, 10000000);
    OfeIntent in = {0};
    in.type = OFE_INTENT_BUILD_UNIT;
    in.player = 1;
    in.unit_type = OFE_UNIT_CITY;
    in.tile = map_ref(m, 30, 30);
    in.amount = 1;
    ofe_game_push_intents(g, &in, 1);
    ofe_game_step(g, 1);
    CHECK(g->players.unit_count[1][OFE_UNIT_CITY] == 1);
    ofe_game_destroy(g);
}

static void test_upgrade(OfeMap *m) {
    OfeGame *g = new_game(m, 5, 0);
    own_rect(g, 1, 10, 10, 100, 50);
    ofe_edit_set_gold(g, 1, 100000000);
    UnitId city = structures_build(g, 1, OFE_UNIT_CITY, map_ref(m, 20, 20), 1, 0);
    UnitId post = structures_build(g, 1, OFE_UNIT_DEFENSE_POST, map_ref(m, 50, 20), 1, 0);
    CHECK(!structures_upgrade(g, 1, city, 1)); /* still under construction */
    ofe_game_step(g, 60);
    int32_t s = slot_of(g, city);

    int64_t gold = g->players.gold[1];
    double max_before = player_max_troops(g, 1);
    CHECK(unit_cost(g, 1, OFE_UNIT_CITY) == 250000);
    CHECK(structures_upgrade(g, 1, city, 1));
    CHECK(g->units.level[s] == 2);
    CHECK(g->players.unit_levels[1][OFE_UNIT_CITY] == 2);
    CHECK(gold - g->players.gold[1] == 250000);
    CHECK(player_max_troops(g, 1) > max_before);

    /* Several levels at once pay the escalating price: 500k + 1M + 1M. */
    gold = g->players.gold[1];
    CHECK(structures_upgrade(g, 1, city, 3));
    CHECK(g->units.level[s] == 5);
    CHECK(gold - g->players.gold[1] == 2500000);
    CHECK(g->players.unit_levels[1][OFE_UNIT_CITY] == 5);

    /* Not enough gold for all of them: buy what we can afford. */
    ofe_edit_set_gold(g, 1, 2500000);
    CHECK(structures_upgrade(g, 1, city, 10));
    CHECK(g->units.level[s] == 7);
    CHECK(!structures_upgrade(g, 1, city, 1));
    ofe_edit_set_gold(g, 1, 100000000);

    CHECK(!structures_upgrade(g, 1, post, 1)); /* defense posts have no levels */
    CHECK(!structures_upgrade(g, 2, city, 1)); /* not the owner */
    CHECK(!structures_upgrade(g, 1, -1, 1));
    check_invariants(g);
    ofe_game_destroy(g);
}

static void test_delete(OfeMap *m) {
    OfeGame *g = new_game(m, 6, 1);
    own_rect(g, 1, 10, 10, 100, 50);
    ofe_edit_set_gold(g, 1, 100000000);
    UnitId a = structures_build(g, 1, OFE_UNIT_CITY, map_ref(m, 20, 20), 1, 0);
    UnitId b = structures_build(g, 1, OFE_UNIT_CITY, map_ref(m, 50, 20), 1, 0);
    ofe_game_step(g, 300); /* the delete cooldown is counted from tick 0 */

    CHECK(!structures_delete(g, 2, a)); /* not the owner */
    CHECK(structures_delete(g, 1, a));
    CHECK(g->units.flags[slot_of(g, a)] & OFE_UF_MARKED_FOR_DELETION);
    CHECK(!structures_delete(g, 1, b)); /* one delete per 30 s */
    CHECK(!structures_upgrade(g, 1, a, 1)); /* marked units can't be upgraded */

    ofe_game_step(g, 301);
    CHECK(slot_of(g, a) >= 0);
    ofe_game_step(g, 1);
    CHECK(slot_of(g, a) < 0);
    CHECK(g->players.unit_count[1][OFE_UNIT_CITY] == 1);
    CHECK(g->players.unit_levels[1][OFE_UNIT_CITY] == 1);
    CHECK(structures_delete(g, 1, b)); /* cooldown over */

    /* A capture cancels the mark. */
    UnitId c = ofe_edit_add_unit(g, OFE_UNIT_CITY, 1, map_ref(m, 80, 40), 1);
    ofe_game_step(g, 301);
    CHECK(structures_delete(g, 1, c));
    own_rect(g, 2, 78, 38, 130, 44);
    ofe_game_step(g, 2);
    CHECK(g->units.owner[slot_of(g, c)] == 2);
    ofe_game_step(g, 400);
    CHECK(slot_of(g, c) >= 0);
    CHECK(!(g->units.flags[slot_of(g, c)] & OFE_UF_MARKED_FOR_DELETION));
    ofe_game_destroy(g);
}

/* How many tiles p1 takes from p2 in 100 ticks, and the troops that cost. */
static int run_attack(OfeMap *m, int with_post, double *troops_per_tile) {
    OfeGame *g = new_game(m, 7, 0);
    own_rect(g, 1, 10, 30, 60, 60);
    own_rect(g, 2, 60, 30, 110, 60);
    if (with_post) {
        /* A post every 30 tiles, so every tile of p2 is within 30 of one. */
        ofe_edit_add_unit(g, OFE_UNIT_DEFENSE_POST, 2, map_ref(m, 75, 45), 1);
        ofe_edit_add_unit(g, OFE_UNIT_DEFENSE_POST, 2, map_ref(m, 100, 45), 1);
    }
    ofe_edit_set_troops(g, 1, 200000);
    ofe_edit_set_troops(g, 2, 20000);
    int32_t before = g->players.num_tiles[2];
    OfeIntent atk = {0};
    atk.type = OFE_INTENT_ATTACK;
    atk.player = 1;
    atk.target = 2;
    atk.troops = 100000;
    ofe_game_push_intents(g, &atk, 1);
    ofe_game_step(g, 100);
    int taken = before - g->players.num_tiles[2];
    double left = 0;
    for (int32_t i = 0; i < g->attacks.count; i++) left += g->attacks.active[i] ? g->attacks.troops[i] : 0;
    *troops_per_tile = taken > 0 ? (100000.0 - left) / taken : 1e18;
    ofe_game_destroy(g);
    return taken;
}

static void test_defense_post_slows_conquest(OfeMap *m) {
    double plain_cost, post_cost;
    int plain = run_attack(m, 0, &plain_cost);
    int post = run_attack(m, 1, &post_cost);
    CHECK(plain > 0);
    CHECK(post > 0);
    CHECK(post < plain); /* slower */
    CHECK(post_cost > plain_cost); /* and costlier per tile */
}

static void test_capture_on_conquest(OfeMap *m) {
    OfeGame *g = new_game(m, 8, 1);
    own_rect(g, 1, 10, 10, 50, 50);
    own_rect(g, 2, 50, 10, 90, 50);
    ofe_edit_set_gold(g, 2, 100000000);
    UnitId city = structures_build(g, 2, OFE_UNIT_CITY, map_ref(m, 70, 30), 1, 0);
    UnitId post = structures_build(g, 2, OFE_UNIT_DEFENSE_POST, map_ref(m, 70, 45), 1, 0);
    CHECK(city >= 0 && post >= 0);
    CHECK(g->players.unit_levels[2][OFE_UNIT_CITY] == 1);

    own_rect(g, 1, 50, 24, 76, 36); /* p1 pushes through to the land under the city */
    ofe_game_step(g, 1);
    int32_t s = slot_of(g, city);
    CHECK(s >= 0 && g->units.owner[s] == 1);
    CHECK(g->players.unit_levels[1][OFE_UNIT_CITY] == 1 && g->players.unit_levels[2][OFE_UNIT_CITY] == 0);
    CHECK(slot_of(g, post) >= 0 && g->units.owner[slot_of(g, post)] == 2);

    own_rect(g, 1, 50, 40, 76, 50); /* ... and under the post: destroyed, not captured */
    ofe_game_step(g, 1);
    CHECK(slot_of(g, post) < 0);
    CHECK(g->players.unit_count[2][OFE_UNIT_DEFENSE_POST] == 0);
    CHECK(g->players.unit_count[1][OFE_UNIT_DEFENSE_POST] == 0);
    ofe_game_destroy(g);
}

static void test_nuke_needs_silo(OfeMap *m) {
    OfeGame *g = new_game(m, 9, 1);
    own_rect(g, 1, 10, 10, 60, 50);
    own_rect(g, 2, 70, 10, 110, 50);
    ofe_edit_set_gold(g, 1, 100000000);
    TileRef target = map_ref(m, 90, 30);
    CHECK(structures_can_build(g, 1, OFE_UNIT_ATOM_BOMB, target) == -1); /* no silo */

    UnitId silo = structures_build(g, 1, OFE_UNIT_MISSILE_SILO, map_ref(m, 30, 30), 1, 0);
    int32_t s = slot_of(g, silo);
    CHECK(structures_can_build(g, 1, OFE_UNIT_ATOM_BOMB, target) == map_ref(m, 30, 30));
    CHECK(structures_can_build(g, 1, OFE_UNIT_MIRV, map_ref(m, 100, 100)) == -1); /* nobody's land */
    CHECK(structures_can_build(g, 2, OFE_UNIT_ATOM_BOMB, target) == -1); /* p2 has no silo */

    g->units.timer[s] = g->tick + 5; /* reloading */
    CHECK(structures_can_build(g, 1, OFE_UNIT_ATOM_BOMB, target) == -1);
    g->units.timer[s] = g->tick;
    CHECK(structures_can_build(g, 1, OFE_UNIT_ATOM_BOMB, target) >= 0);
    ofe_edit_set_gold(g, 1, 700000);
    CHECK(structures_can_build(g, 1, OFE_UNIT_ATOM_BOMB, target) == -1); /* 750k */
    ofe_game_destroy(g);

    /* Spawn immunity blocks nukes. */
    OfeConfig c = base_config();
    c.num_bots = 0;
    c.num_nations = 0;
    c.spawn_phase_ticks = 0;
    g = ofe_game_create(m, &c, 9);
    own_rect(g, 1, 10, 10, 60, 50);
    ofe_edit_add_unit(g, OFE_UNIT_MISSILE_SILO, 1, map_ref(m, 30, 30), 1);
    ofe_edit_set_gold(g, 1, 100000000);
    CHECK(structures_can_build(g, 1, OFE_UNIT_ATOM_BOMB, map_ref(m, 40, 30)) == -1);
    ofe_game_step(g, 60);
    CHECK(structures_can_build(g, 1, OFE_UNIT_ATOM_BOMB, map_ref(m, 40, 30)) >= 0);
    ofe_game_destroy(g);
}

/* The grid index must agree with a plain scan of the unit table. */
static bool brute_nearby(const OfeGame *g, TileRef tile, int range, int type, PlayerId owner) {
    const UnitTable *u = &g->units;
    for (int32_t i = 0; i < u->count; i++) {
        if (!u->alive[i] || u->type[i] != type || (owner && u->owner[i] != owner)) continue;
        if (u->flags[i] & OFE_UF_UNDER_CONSTRUCTION) continue;
        if (map_dist2(g->map, tile, u->tile[i]) <= (int64_t)range * range) return true;
    }
    return false;
}

static void check_index(const OfeGame *g, uint64_t seed) {
    uint64_t rng = seed;
    for (int i = 0; i < 400; i++) {
        TileRef t = rng_int(&rng, 0, g->map->num_tiles);
        int range = rng_int(&rng, 1, 60);
        int type = (int)(rng_int(&rng, 0, 2) ? OFE_UNIT_DEFENSE_POST : OFE_UNIT_CITY);
        PlayerId owner = rng_int(&rng, 0, 3);
        CHECK(units_has_nearby(g, t, range, type, owner) == brute_nearby(g, t, range, type, owner));
    }
}

static void test_index_and_clone(OfeMap *m) {
    OfeGame *g = new_game(m, 10, 1);
    own_rect(g, 1, 10, 10, 80, 100);
    own_rect(g, 2, 80, 10, 150, 100);
    ofe_edit_set_gold(g, 1, 1000000000);
    ofe_edit_set_gold(g, 2, 1000000000);
    uint64_t rng = 99;
    for (int i = 0; i < 80; i++) {
        PlayerId p = 1 + (i & 1);
        int type = i % 3 == 0 ? OFE_UNIT_DEFENSE_POST : (i % 3 == 1 ? OFE_UNIT_CITY : OFE_UNIT_FACTORY);
        TileRef t = map_ref(m, rng_int(&rng, 10, 150), rng_int(&rng, 10, 100));
        structures_build(g, p, type, t, 1, 0);
    }
    CHECK(g->units.num_alive > 20);
    check_index(g, 1);

    /* Destroy some, capture some, keep checking. */
    int removed = 0;
    for (int32_t i = 0; i < g->units.count && removed < 10; i += 3) {
        if (g->units.alive[i]) {
            unit_destroy(g, unit_id_of_slot(g, i), 0);
            removed++;
        }
    }
    own_rect(g, 2, 60, 10, 90, 100); /* p2 takes a strip of p1's land */
    ofe_game_step(g, 2);
    check_index(g, 2);

    /* A clone has its own index. */
    OfeGame *k = ofe_game_clone(g);
    CHECK(ofe_game_hash(k) == ofe_game_hash(g));
    check_index(k, 3);
    TileRef probe = map_ref(m, 100, 60);
    TileRef a = structures_can_build(g, 2, OFE_UNIT_CITY, probe);
    CHECK(a == structures_can_build(k, 2, OFE_UNIT_CITY, probe));
    ofe_edit_set_gold(k, 2, 1000000000);
    CHECK(structures_build(k, 2, OFE_UNIT_DEFENSE_POST, probe, 1, 0) >= 0);
    check_index(k, 4);
    check_index(g, 5);
    CHECK(structures_can_build(g, 2, OFE_UNIT_CITY, probe) == a); /* parent untouched */

    /* copy_into restores the parent's index and pending state. */
    ofe_game_copy_into(k, g);
    CHECK(ofe_game_hash(k) == ofe_game_hash(g));
    check_index(k, 6);
    ofe_game_step(k, 100);
    ofe_game_step(g, 100);
    CHECK(ofe_game_hash(k) == ofe_game_hash(g));
    check_index(g, 7);
    check_invariants(g);
    ofe_game_destroy(k);
    ofe_game_destroy(g);
}

static void test_deterministic(OfeMap *m) {
    uint64_t h[2];
    for (int run = 0; run < 2; run++) {
        OfeGame *g = new_game(m, 11, 0);
        own_rect(g, 1, 10, 10, 80, 100);
        ofe_edit_set_gold(g, 1, 1000000000);
        uint64_t rng = 5;
        for (int i = 0; i < 40; i++) {
            structures_build(g, 1, OFE_UNIT_CITY + 0, map_ref(m, rng_int(&rng, 10, 80), rng_int(&rng, 10, 100)), 1, 0);
            ofe_game_step(g, 3);
        }
        h[run] = ofe_game_hash(g);
        ofe_game_destroy(g);
    }
    CHECK(h[0] == h[1]);
}

int main(void) {
    OfeMap *m = make_map(W, H);
    test_city_build_and_cost(m);
    test_structure_types_and_durations(m);
    test_placement_rules(m);
    test_build_intent(m);
    test_upgrade(m);
    test_delete(m);
    test_defense_post_slows_conquest(m);
    test_capture_on_conquest(m);
    test_nuke_needs_silo(m);
    test_index_and_clone(m);
    test_deterministic(m);
    ofe_map_release(m);
    return test_summary("test_structures");
}

/*
 * test_rail.c - stations, railroads, trains and the train gold economy.
 * Synthetic map; structures are placed finished with ofe_edit_add_unit.
 */
#include <time.h>

#include "test_util.h"

#define W 200
#define H 150

/* Two humans, no AI. Player 1 owns x < 120, player 2 owns x >= 120. */
static OfeGame *make_game(OfeMap *m, uint64_t seed) {
    OfeConfig c = base_config();
    c.num_nations = 0;
    c.num_bots = 0;
    OfeGame *g = ofe_game_create(m, &c, seed);
    OfeIntent in[2] = {{0}};
    in[0].type = OFE_INTENT_SPAWN;
    in[0].player = 1;
    in[0].tile = map_ref(m, 30, 30);
    in[1].type = OFE_INTENT_SPAWN;
    in[1].player = 2;
    in[1].tile = map_ref(m, 170, 120);
    ofe_game_push_intents(g, in, 2);
    ofe_game_step(g, 12);
    for (TileRef t = 0; t < m->num_tiles; t++) {
        if (!map_is_ownable(m, t)) continue;
        int x = map_x(m, t), y = map_y(m, t);
        if (y < 20 || y > 130 || x < 20 || x > 180) continue;
        ofe_edit_set_owner(g, t, x < 120 ? 1 : 2);
    }
    return g;
}

static UnitId add(OfeGame *g, int type, int owner, int x, int y) {
    return ofe_edit_add_unit(g, type, owner, map_ref(g->map, x, y), 1);
}

static int count_units(const OfeGame *g, int type) {
    int n = 0;
    for (int32_t i = 0; i < g->units.count; i++) n += g->units.alive[i] && g->units.type[i] == type;
    return n;
}

static void test_stations_connect(OfeMap *m) {
    OfeGame *g = make_game(m, 1);
    /* A city with no factory in range is not a station. */
    UnitId lone = add(g, OFE_UNIT_CITY, 1, 40, 90);
    CHECK(rail_station_count(g) == 0);
    CHECK(rail_station_owner(g, lone) == -1);

    /* A factory makes itself and the city in range stations. */
    UnitId f1 = add(g, OFE_UNIT_FACTORY, 1, 40, 60);
    CHECK(rail_station_count(g) == 2);
    CHECK(rail_connected(g, f1, lone));
    UnitId f2 = add(g, OFE_UNIT_FACTORY, 2, 130, 100);
    UnitId c2 = add(g, OFE_UNIT_CITY, 2, 125, 60);
    CHECK(rail_station_count(g) == 4);
    CHECK(rail_connected(g, f1, f2));
    CHECK(rail_connected(g, lone, c2));
    CHECK(rail_edge_count(g) >= 3);

    /* Stations closer than 15 tiles to a neighbour are not linked directly
     * but still join through the network. */
    UnitId near = add(g, OFE_UNIT_CITY, 1, 45, 66);
    CHECK(rail_connected(g, near, f1));

    /* Destroying a hub drops it and its tracks. */
    int edges = rail_edge_count(g);
    ofe_edit_remove_unit(g, f1);
    CHECK(rail_station_owner(g, f1) == -1);
    CHECK(rail_station_count(g) == 4);
    CHECK(rail_edge_count(g) < edges);
    ofe_game_step(g, 50);
    check_invariants(g);
    ofe_game_destroy(g);
}

static void test_trains_pay(OfeMap *m) {
    OfeGame *g = make_game(m, 2);
    add(g, OFE_UNIT_FACTORY, 1, 40, 60);
    add(g, OFE_UNIT_CITY, 1, 40, 90);
    add(g, OFE_UNIT_CITY, 2, 125, 60);
    int64_t gold1 = g->players.gold[1];
    int max_trains = 0;
    for (int i = 0; i < 300; i++) {
        ofe_game_step(g, 10);
        int n = count_units(g, OFE_UNIT_TRAIN);
        max_trains = OFE_MAX(max_trains, n);
    }
    CHECK(max_trains > 0);
    CHECK(g->players.train_gold[1] > 0);
    CHECK(g->players.train_gold[2] > 0); /* the other player's city pays too */
    /* 10k per self stop, 25k per foreign stop: gold went well beyond the base income. */
    CHECK(g->players.gold[1] - gold1 > 3000 * 100 + g->players.train_gold[1] / 2);
    /* Every train stop is a multiple of 5k. */
    CHECK(g->players.train_gold[1] % 5000 == 0);
    ofe_game_destroy(g);
}

static void test_embargo_blocks(OfeMap *m) {
    for (int embargo = 0; embargo < 2; embargo++) {
        OfeGame *g = make_game(m, 3);
        add(g, OFE_UNIT_FACTORY, 1, 40, 60);
        add(g, OFE_UNIT_CITY, 1, 40, 90);
        add(g, OFE_UNIT_CITY, 2, 125, 60);
        if (embargo) player_add_embargo(g, 2, 1, false);
        ofe_game_step(g, 3000);
        CHECK(g->players.train_gold[1] > 0);
        if (embargo) CHECK(g->players.train_gold[2] == 0);
        else CHECK(g->players.train_gold[2] > 0);
        ofe_game_destroy(g);
    }
}

static void test_capture(OfeMap *m) {
    OfeGame *g = make_game(m, 4);
    add(g, OFE_UNIT_FACTORY, 1, 40, 60);
    add(g, OFE_UNIT_CITY, 1, 40, 90);
    UnitId c2 = add(g, OFE_UNIT_CITY, 2, 123, 60);
    CHECK(rail_station_owner(g, c2) == 2);
    /* Player 1 takes the land under the city (a strip joined to its own
     * territory, not an enclave): the core captures the city. */
    for (int dy = -3; dy <= 3; dy++) {
        for (int x = 120; x <= 126; x++) ofe_edit_set_owner(g, map_ref(m, x, 60 + dy), 1);
    }
    ofe_game_step(g, 2);
    CHECK(g->units.owner[unit_lookup(g, c2)] == 1);
    CHECK(rail_station_owner(g, c2) == 1);
    CHECK(rail_station_count(g) == 3);
    int64_t before = g->players.train_gold[2];
    ofe_game_step(g, 3000);
    CHECK(g->players.train_gold[2] == before); /* nothing flows to the old owner */
    CHECK(g->players.train_gold[1] > 0);
    ofe_game_destroy(g);
}

static void test_factory_levels_and_clone(OfeMap *m) {
    OfeGame *g = make_game(m, 5);
    UnitId f = add(g, OFE_UNIT_FACTORY, 1, 40, 60);
    add(g, OFE_UNIT_CITY, 1, 40, 90);
    add(g, OFE_UNIT_CITY, 1, 80, 60);
    ofe_game_step(g, 500);
    OfeGame *k = ofe_game_clone(g);
    CHECK(ofe_game_hash(k) == ofe_game_hash(g));
    CHECK(rail_station_count(k) == rail_station_count(g));
    CHECK(rail_connected(k, f, f));
    ofe_game_step(g, 1500);
    ofe_game_step(k, 1500);
    CHECK(ofe_game_hash(k) == ofe_game_hash(g));
    CHECK(g->players.train_gold[1] == k->players.train_gold[1]);

    /* Mutating the clone leaves the parent alone. */
    int stations = rail_station_count(g);
    ofe_edit_remove_unit(k, f);
    CHECK(rail_station_count(g) == stations);
    CHECK(rail_station_owner(g, f) == 1);

    /* copy_into restores. */
    ofe_game_copy_into(k, g);
    CHECK(ofe_game_hash(k) == ofe_game_hash(g));
    ofe_game_step(g, 300);
    ofe_game_step(k, 300);
    CHECK(ofe_game_hash(k) == ofe_game_hash(g));
    check_invariants(k);
    ofe_game_destroy(k);
    ofe_game_destroy(g);
}

static void test_real_map(void) {
    OfeMap *m = load_real_map("world");
    if (!m) {
        printf("test_rail: OFE_MAPS_DIR not set, skipping real map\n");
        return;
    }
    OfeConfig c = base_config();
    c.num_humans = 0;
    c.num_nations = -1;
    c.num_bots = 100;
    c.spawn_phase_ticks = 0;
    c.humans_use_ai = 1;
    c.event_mask = OFE_EVENTS_NO_TILES;
    OfeGame *g = ofe_game_create(m, &c, 9);
    /* Let the nations grow, then give each a factory at its spawn and a few
     * cities on its border, 16 to 80 tiles away. */
    ofe_game_step(g, 1500);
    int placed = 0;
    for (int p = 1; p < g->players.count; p++) {
        if (g->players.type[p] != OFE_PLAYER_NATION || g->players.num_tiles[p] == 0) continue;
        TileRef s = g->players.spawn_tile[p];
        if (tile_owner(g, s) != p) continue;
        ofe_edit_add_unit(g, OFE_UNIT_FACTORY, p, s, 1);
        placed++;
        int cities = 0;
        const IntVec *b = &g->players.border[p];
        for (int32_t k = 0; k < b->len && cities < 3; k += 7) {
            int64_t d2 = map_dist2(m, s, b->data[k]);
            if (d2 >= 16 * 16 && d2 <= 80 * 80) {
                ofe_edit_add_unit(g, OFE_UNIT_CITY, p, b->data[k], 1);
                cities++;
            }
        }
    }
    clock_t t0 = clock();
    ofe_game_step(g, 3000);
    printf("test_rail: %.0f ticks/s with rail active\n", 3000 / ((double)(clock() - t0) / CLOCKS_PER_SEC));
    printf("test_rail: world map, %d factories -> %d stations, %d rails, %d trains\n", placed, rail_station_count(g),
           rail_edge_count(g), count_units(g, OFE_UNIT_TRAIN));
    check_invariants(g);
    ofe_game_destroy(g);
    ofe_map_release(m);
}

int main(void) {
    OfeMap *m = make_map(W, H);
    test_stations_connect(m);
    test_trains_pay(m);
    test_embargo_blocks(m);
    test_capture(m);
    test_factory_levels_and_clone(m);
    ofe_map_release(m);
    test_real_map();
    return test_summary("test_rail");
}

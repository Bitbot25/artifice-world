/*
 * test_ships.c - water bodies, routing, transport ships, trade ships,
 * warships and shells. Synthetic two-island map; the World map (when
 * OFE_MAPS_DIR is set) is used to report boat travel times and the cost of
 * a ship-heavy game.
 */
#include <time.h>

#include "../../engine/src/pathfind.h"
#include "test_util.h"

#define W 200
#define H 100

/* Island L: x 10..69, y 20..79 with a lake (radius 6) at (40,50).
 * Island R: x 130..189, y 20..79. Everything else is ocean. */
static OfeMap *make_islands(void) {
    static uint8_t t[W * H], s[W * H];
    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            bool left = x >= 10 && x < 70 && y >= 20 && y < 80;
            bool right = x >= 130 && x < 190 && y >= 20 && y < 80;
            bool lake = (x - 40) * (x - 40) + (y - 50) * (y - 50) < 36;
            if ((left && !lake) || right) t[y * W + x] = TERRAIN_LAND_BIT | 3;
            else t[y * W + x] = lake ? 0 : TERRAIN_OCEAN_BIT;
        }
    }
    memcpy(s, t, sizeof(t));
    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            int land = t[y * W + x] & TERRAIN_LAND_BIT;
            const int dx[4] = {1, -1, 0, 0}, dy[4] = {0, 0, 1, -1};
            for (int k = 0; k < 4; k++) {
                int xx = x + dx[k], yy = y + dy[k];
                if (xx < 0 || yy < 0 || xx >= W || yy >= H) continue;
                if ((t[yy * W + xx] & TERRAIN_LAND_BIT) != land) s[y * W + x] |= TERRAIN_SHORELINE_BIT;
            }
        }
    }
    return ofe_map_from_terrain(W, H, s);
}

static OfeGame *two_player_game(OfeMap *m, uint64_t seed) {
    OfeConfig c = base_config();
    c.num_humans = 2;
    c.num_bots = 0;
    c.num_nations = 0;
    c.spawn_phase_ticks = 0;
    OfeGame *g = ofe_game_create(m, &c, seed);
    for (int y = 20; y < 80; y++) {
        for (int x = 10; x < 70; x++) {
            TileRef t = map_ref(m, x, y);
            if (map_is_land(m, t)) ofe_edit_set_owner(g, t, 1);
        }
        for (int x = 130; x < 190; x++) ofe_edit_set_owner(g, map_ref(m, x, y), 2);
    }
    ofe_edit_set_troops(g, 1, 100000);
    ofe_edit_set_troops(g, 2, 2000);
    ofe_game_step(g, 60); /* past spawn immunity */
    return g;
}

static int count_units(const OfeGame *g, int type) {
    int n = 0;
    for (int32_t i = 0; i < g->units.count; i++) n += g->units.alive[i] && g->units.type[i] == type;
    return n;
}

static int32_t first_unit(const OfeGame *g, int type) {
    for (int32_t i = 0; i < g->units.count; i++) {
        if (g->units.alive[i] && g->units.type[i] == type) return i;
    }
    return -1;
}

static void boat_intent(OfeGame *g, int player, TileRef tile, double troops) {
    OfeIntent in = {0};
    in.type = OFE_INTENT_BOAT_ATTACK;
    in.player = player;
    in.tile = tile;
    in.troops = troops;
    ofe_game_push_intents(g, &in, 1);
}

/* A route is valid if it starts and ends at the given tiles, moves one
 * tile at a time, and crosses only water between its (shore) ends. */
static bool route_ok(const OfeMap *m, const IntVec *p, TileRef from, TileRef to) {
    if (p->len < 1 || p->data[0] != from || p->data[p->len - 1] != to) return false;
    for (int32_t i = 0; i < p->len; i++) {
        if (i > 0 && i < p->len - 1 && map_is_land(m, p->data[i])) return false;
        if (i > 0) {
            int dx = abs(map_x(m, p->data[i]) - map_x(m, p->data[i - 1]));
            int dy = abs(map_y(m, p->data[i]) - map_y(m, p->data[i - 1]));
            if (dx > 1 || dy > 1) return false;
        }
    }
    return true;
}

/* ---- Tests -------------------------------------------------------------------- */

static void test_components_and_routes(OfeMap *m) {
    const WaterGraph *wg = water_graph(m);
    int32_t ocean = water_tile_component(wg, m, map_ref(m, 100, 50));
    int32_t lake = water_tile_component(wg, m, map_ref(m, 40, 50));
    CHECK(ocean >= 0 && lake >= 0 && ocean != lake);
    CHECK(water_graph_num_components(wg) == 2);
    CHECK(water_tile_component(wg, m, map_ref(m, 30, 30)) == -1); /* inland */
    CHECK(water_tile_component(wg, m, map_ref(m, 69, 50)) == ocean); /* shore */
    CHECK(water_tile_component(wg, m, map_ref(m, 40, 44)) == lake); /* lake shore */
    CHECK(water_tile_component(wg, m, map_ref(m, 0, 0)) == ocean);

    IntVec path = {0};
    TileRef a = map_ref(m, 69, 50), b = map_ref(m, 130, 50);
    CHECK(water_find_path(wg, m, &a, 1, b, &path) == 0);
    CHECK(route_ok(m, &path, a, b));
    CHECK(path.len >= 62 && path.len <= 70); /* a straight crossing */
    IntVec again = {0};
    CHECK(water_find_path(wg, m, &a, 1, b, &again) == 0); /* from the memo */
    CHECK(again.len == path.len && memcmp(again.data, path.data, (size_t)path.len * 4) == 0);

    /* Around the island: from the north shore to the south shore. */
    TileRef n = map_ref(m, 30, 20), s = map_ref(m, 30, 79);
    CHECK(water_find_path(wg, m, &n, 1, s, &path) == 0);
    CHECK(route_ok(m, &path, n, s));
    CHECK(path.len > 100);

    /* A lake shore can't reach the ocean. */
    TileRef lk = map_ref(m, 40, 44);
    CHECK(water_find_path(wg, m, &lk, 1, b, &path) < 0);

    /* Several sources: the nearer one wins. */
    TileRef srcs[3] = {map_ref(m, 30, 20), map_ref(m, 69, 60), map_ref(m, 12, 79)};
    CHECK(water_find_path(wg, m, srcs, 3, b, &path) == 1);
    CHECK(route_ok(m, &path, srcs[1], b));

    OfeGame *g = two_player_game(m, 1);
    CHECK(ships_water_component(g, map_ref(m, 100, 50)) == ocean);
    CHECK(ships_water_component(g, map_ref(m, 40, 50)) == lake);
    CHECK(ships_water_component(g, map_ref(m, 30, 30)) == -1);
    ofe_game_destroy(g);
    IntVec_free(&path);
    IntVec_free(&again);
}

static void test_boat_attack(OfeMap *m) {
    OfeGame *g = two_player_game(m, 11);
    TileRef dst = map_ref(m, 140, 50);
    CHECK(ships_transport_source(g, 1, dst) >= 0);
    CHECK(ships_transport_source(g, 1, map_ref(m, 40, 30)) < 0); /* own land */
    int64_t troops_before = g->players.troops[1];
    boat_intent(g, 1, dst, -1);
    ofe_game_step(g, 1);
    CHECK(count_units(g, OFE_UNIT_TRANSPORT_SHIP) == 1);
    int32_t b = first_unit(g, OFE_UNIT_TRANSPORT_SHIP);
    CHECK(b >= 0);
    if (b >= 0) {
        /* Launched at the end of the tick, after this tick's growth. */
        CHECK(g->units.troops[b] > 15000 && g->units.troops[b] <= (troops_before + 2000) / 5);
        CHECK(g->players.troops[1] < troops_before - 15000 + 2000); /* taken at launch (growth is small) */
        CHECK(g->units.path[b].len >= 62 && g->units.path[b].len <= 80);
    }
    int32_t path_len = b >= 0 ? g->units.path[b].len : 0;
    int ticks = 1;
    while (count_units(g, OFE_UNIT_TRANSPORT_SHIP) > 0 && ticks < 300) {
        ofe_game_step(g, 1);
        ticks++;
    }
    CHECK(ticks >= path_len - 1 && ticks <= path_len + 3); /* one tile per tick */
    CHECK(tile_owner(g, map_ref(m, 130, 50)) == 1 || g->players.num_tiles[1] > 3600);
    int32_t before = g->players.num_tiles[2];
    ofe_game_step(g, 200);
    CHECK(g->players.num_tiles[2] < before);
    int right = 0;
    for (int y = 20; y < 80; y++) {
        for (int x = 130; x < 190; x++) right += tile_owner(g, map_ref(m, x, y)) == 1;
    }
    CHECK(right > 10);
    check_invariants(g);
    ofe_game_destroy(g);
}

static void test_boat_limit_and_cancel(OfeMap *m) {
    OfeGame *g = two_player_game(m, 12);
    TileRef dst = map_ref(m, 140, 50);
    for (int i = 0; i < 4; i++) {
        boat_intent(g, 1, dst, 1000);
        ofe_game_step(g, 1);
    }
    CHECK(count_units(g, OFE_UNIT_TRANSPORT_SHIP) == 3); /* the fourth was refused */
    CHECK(g->players.unit_count[1][OFE_UNIT_TRANSPORT_SHIP] == 3);
    OfeUnitInfo infos[8];
    int n = ofe_game_units(g, infos, 8);
    double boat_troops = 0;
    for (int i = 0; i < n; i++) boat_troops += infos[i].type == OFE_UNIT_TRANSPORT_SHIP ? infos[i].troops : 0;
    CHECK(boat_troops == 3000);

    /* Cancel one: it sails home and returns 75% of its troops. */
    int32_t b = first_unit(g, OFE_UNIT_TRANSPORT_SHIP);
    UnitId id = unit_id_of_slot(g, b);
    OfeIntent cancel = {0};
    cancel.type = OFE_INTENT_CANCEL_BOAT;
    cancel.player = 1;
    cancel.target = id;
    ofe_game_push_intents(g, &cancel, 1);
    int64_t troops_at_cancel = g->players.troops[1];
    ofe_game_step(g, 1);
    int guard = 0;
    while (unit_lookup(g, id) >= 0 && guard++ < 200) ofe_game_step(g, 1);
    CHECK(unit_lookup(g, id) < 0);
    CHECK(g->players.unit_count[1][OFE_UNIT_TRANSPORT_SHIP] == 2);
    CHECK(g->players.troops[1] >= troops_at_cancel + 750 - 1);

    /* Others may not cancel it, and a bogus tile is refused. */
    CHECK(!ships_retreat_transport(g, 2, unit_id_of_slot(g, first_unit(g, OFE_UNIT_TRANSPORT_SHIP))));
    CHECK(ships_launch_transport(g, 1, map_ref(m, 40, 30), 100) < 0); /* own land */
    CHECK(ships_launch_transport(g, 1, map_ref(m, 100, 50), 100) < 0); /* open water */
    ofe_game_destroy(g);
}

static void test_trade(OfeMap *m) {
    OfeGame *g = two_player_game(m, 13);
    ofe_edit_add_unit(g, OFE_UNIT_PORT, 1, map_ref(m, 69, 50), 1);
    ofe_edit_add_unit(g, OFE_UNIT_PORT, 2, map_ref(m, 130, 50), 1);
    int64_t gold1 = g->players.gold[1], gold2 = g->players.gold[2];
    int seen_ships = 0;
    for (int i = 0; i < 2500; i++) {
        ofe_game_step(g, 1);
        seen_ships = OFE_MAX(seen_ships, count_units(g, OFE_UNIT_TRADE_SHIP));
    }
    CHECK(seen_ships >= 1);
    CHECK(g->players.trade_gold[1] > 0 && g->players.trade_gold[2] > 0);
    CHECK(g->players.trade_gold[1] == g->players.trade_gold[2]); /* both ends get the same */
    CHECK(g->players.gold[1] > gold1 + g->players.trade_gold[1] - 1);
    CHECK(g->players.gold[2] > gold2);

    /* An embargo stops it: no new ships, and ships under way are scrapped. */
    OfeIntent e = {0};
    e.type = OFE_INTENT_EMBARGO;
    e.player = 1;
    e.target = 2;
    e.flags = 1;
    ofe_game_push_intents(g, &e, 1);
    ofe_game_step(g, 400);
    CHECK(count_units(g, OFE_UNIT_TRADE_SHIP) == 0);
    int64_t tg = g->players.trade_gold[1];
    ofe_game_step(g, 500);
    CHECK(g->players.trade_gold[1] == tg);
    ofe_game_destroy(g);
}

static void test_warship(OfeMap *m) {
    OfeGame *g = two_player_game(m, 14);
    ofe_edit_add_unit(g, OFE_UNIT_PORT, 1, map_ref(m, 69, 50), 1);
    ofe_edit_set_gold(g, 1, 10000000);
    TileRef patrol = map_ref(m, 90, 50);
    UnitId w = ships_build_warship(g, 1, patrol);
    CHECK(w >= 0);
    int32_t ws = unit_lookup(g, w);
    CHECK(ws >= 0 && g->units.health[ws] == 1000);
    CHECK(g->players.gold[1] == 10000000 - 250000);
    CHECK(g->players.units_built[1][OFE_UNIT_WARSHIP] == 1);
    CHECK(ships_build_warship(g, 2, patrol) < 0); /* player 2 has no port */
    CHECK(ships_build_warship(g, 1, map_ref(m, 40, 50)) < 0); /* lake: no port on it */

    /* It patrols near its patrol tile, away from the port. */
    ofe_game_step(g, 80);
    ws = unit_lookup(g, w);
    CHECK(ws >= 0 && map_is_water(m, g->units.tile[ws]));
    CHECK(map_dist2(m, g->units.tile[ws], patrol) < 60 * 60);

    /* An enemy transport ship in range is sunk. */
    ofe_edit_add_unit(g, OFE_UNIT_TRANSPORT_SHIP, 2, map_ref(m, 100, 55), 1);
    CHECK(count_units(g, OFE_UNIT_TRANSPORT_SHIP) == 1);
    ofe_game_step(g, 120);
    CHECK(count_units(g, OFE_UNIT_TRANSPORT_SHIP) == 0);
    ws = unit_lookup(g, w);
    CHECK(ws >= 0);
    if (ws >= 0) CHECK(((g->units.aux3[ws] >> 4) & 0x1ff) == 25); /* veterancy progress */

    /* Orders: moving the patrol is accepted for water in the same body only. */
    CHECK(ships_move_warship(g, 1, w, map_ref(m, 100, 30)));
    CHECK(!ships_move_warship(g, 1, w, map_ref(m, 40, 50)));
    CHECK(!ships_move_warship(g, 2, w, map_ref(m, 100, 30)));
    ofe_game_step(g, 100);
    ws = unit_lookup(g, w);
    CHECK(ws >= 0 && map_dist2(m, g->units.tile[ws], map_ref(m, 100, 30)) < 70 * 70);

    /* Warships fight: a hit from a defense-post style shell does damage. */
    ofe_edit_add_unit(g, OFE_UNIT_WARSHIP, 2, map_ref(m, 100, 35), 1);
    int32_t enemy = first_unit(g, OFE_UNIT_WARSHIP);
    if (enemy == ws) {
        for (int32_t i = 0; i < g->units.count; i++) {
            if (g->units.alive[i] && g->units.type[i] == OFE_UNIT_WARSHIP && g->units.owner[i] == 2) enemy = i;
        }
    }
    int32_t hp = g->units.health[enemy];
    CHECK(ships_fire_shell(g, 1, g->units.tile[ws], enemy, 250) >= 0);
    for (int i = 0; i < 80 && g->units.health[enemy] == hp; i++) ofe_game_step(g, 1);
    CHECK(g->units.health[enemy] < hp);
    check_invariants(g);
    ofe_game_destroy(g);
}

static void test_piracy(OfeMap *m) {
    /* Player 2 and 3 split island R and trade along its coast; player 1's
     * warship, patrolling nearby, captures their ships. */
    OfeConfig c = base_config();
    c.num_humans = 3;
    c.num_bots = 0;
    c.num_nations = 0;
    c.spawn_phase_ticks = 0;
    OfeGame *g = ofe_game_create(m, &c, 15);
    for (int y = 20; y < 80; y++) {
        for (int x = 10; x < 70; x++) {
            if (map_is_land(m, map_ref(m, x, y))) ofe_edit_set_owner(g, map_ref(m, x, y), 1);
        }
        for (int x = 130; x < 190; x++) ofe_edit_set_owner(g, map_ref(m, x, y), y < 50 ? 2 : 3);
    }
    ofe_edit_add_unit(g, OFE_UNIT_PORT, 1, map_ref(m, 69, 50), 1);
    ofe_edit_add_unit(g, OFE_UNIT_PORT, 2, map_ref(m, 130, 22), 1);
    ofe_edit_add_unit(g, OFE_UNIT_PORT, 3, map_ref(m, 130, 78), 1);
    ofe_edit_set_gold(g, 1, 10000000);
    ofe_game_step(g, 60);
    CHECK(ships_build_warship(g, 1, map_ref(m, 112, 50)) >= 0);
    int captured = 0, completed_after = 0;
    for (int i = 0; i < 4000; i++) {
        ofe_game_step(g, 1);
        int mine = 0;
        for (int32_t k = 0; k < g->units.count; k++) {
            if (g->units.alive[k] && g->units.type[k] == OFE_UNIT_TRADE_SHIP && g->units.owner[k] == 1) mine++;
        }
        if (mine) captured = 1;
        else if (captured) completed_after = 1;
    }
    CHECK(captured);
    CHECK(completed_after); /* the prize reached player 1's port */
    check_invariants(g);
    ofe_game_destroy(g);
}

static void test_clone_mid_voyage(OfeMap *m) {
    OfeGame *g = two_player_game(m, 16);
    ofe_edit_add_unit(g, OFE_UNIT_PORT, 1, map_ref(m, 69, 50), 1);
    ofe_edit_add_unit(g, OFE_UNIT_PORT, 2, map_ref(m, 130, 50), 1);
    ofe_edit_set_gold(g, 1, 10000000);
    ships_build_warship(g, 1, map_ref(m, 90, 50));
    boat_intent(g, 1, map_ref(m, 140, 60), -1);
    ofe_game_step(g, 300); /* boats, trade ships and a warship under way */
    OfeGame *k = ofe_game_clone(g);
    CHECK(ofe_game_hash(k) == ofe_game_hash(g));
    boat_intent(g, 1, map_ref(m, 140, 40), -1);
    boat_intent(k, 1, map_ref(m, 140, 40), -1);
    for (int i = 0; i < 600; i++) {
        ofe_game_step(g, 1);
        ofe_game_step(k, 1);
        if (ofe_game_hash(k) != ofe_game_hash(g)) {
            CHECK(!"clone diverged");
            break;
        }
    }
    CHECK(ofe_game_hash(k) == ofe_game_hash(g));
    /* Restoring into an existing game works too. */
    OfeGame *r = ofe_game_create(m, &g->cfg, 99);
    ofe_game_copy_into(r, g);
    ofe_game_step(g, 100);
    ofe_game_step(r, 100);
    CHECK(ofe_game_hash(r) == ofe_game_hash(g));
    check_invariants(r);
    ofe_game_destroy(g);
    ofe_game_destroy(k);
    ofe_game_destroy(r);
}

/* ---- World map: travel times and cost ------------------------------------------------ */

static double now(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}

/* The Bering Strait is two tiles wide on the compact world map: the routing
 * grids cut it, and a trip from the Arctic coast to the Pacific one went all
 * the way round the Atlantic (1250 steps; OpenFront's boat needs about 700). */
static void test_strait_detour(void) {
    const char *dir = getenv("OFE_MAPS_DIR");
    if (!dir || !*dir) return;
    char path[1024];
    snprintf(path, sizeof(path), "%s/world", dir);
    OfeMap *m = ofe_map_load(path, 1);
    const WaterGraph *wg = water_graph(m);
    TileRef a = map_ref(m, 803, 33), b = map_ref(m, 873, 374);
    IntVec route = {0};
    CHECK(water_find_path(wg, m, &a, 1, b, &route) == 0);
    CHECK(route_ok(m, &route, a, b));
    CHECK(route.len > 600 && route.len < 800);
    IntVec_free(&route);
    ofe_map_release(m);
}

static void test_world(void) {
    OfeMap *m = load_real_map("world");
    if (!m) {
        printf("skip world-map ship tests (set OFE_MAPS_DIR)\n");
        return;
    }
    const WaterGraph *wg = water_graph(m);
    printf("world: %d water bodies\n", water_graph_num_components(wg));

    /* Route length versus the straight Chebyshev distance for random shore pairs. */
    uint64_t rs = 12345;
    IntVec shores = {0}, path = {0};
    int32_t ocean = water_tile_component(wg, m, map_ref(m, 1000, 10));
    for (TileRef t = 0; t < m->num_tiles; t++) {
        if (map_is_shore(m, t) && water_tile_component(wg, m, t) == ocean) IntVec_push(&shores, t);
    }
    int found = 0, failed = 0, invalid = 0;
    double sum_ratio = 0, max_ratio = 0, total_s = 0;
    int buckets[4] = {0}, bucket_n[4] = {0};
    double bucket_ratio[4] = {0};
    for (int i = 0; i < 300; i++) {
        TileRef a = shores.data[rng_int(&rs, 0, shores.len)], b = shores.data[rng_int(&rs, 0, shores.len)];
        int cheb = OFE_MAX(abs(map_x(m, a) - map_x(m, b)), abs(map_y(m, a) - map_y(m, b)));
        if (cheb < 20) continue;
        double t0 = now();
        int r = water_find_path(wg, m, &a, 1, b, &path);
        total_s += now() - t0;
        if (r < 0) {
            failed++;
            continue;
        }
        found++;
        if (!route_ok(m, &path, a, b)) invalid++;
        double ratio = (double)(path.len - 1) / cheb;
        sum_ratio += ratio;
        max_ratio = OFE_MAX(max_ratio, ratio);
        int bk = cheb < 100 ? 0 : cheb < 300 ? 1 : cheb < 800 ? 2 : 3;
        bucket_n[bk]++;
        bucket_ratio[bk] += ratio;
        buckets[bk] += path.len - 1;
    }
    printf("world routes: %d found, %d failed, mean steps/straight-line %.2f (max %.2f), %.0f us per route (cold)\n", found,
           failed, found ? sum_ratio / found : 0, max_ratio, found ? total_s / found * 1e6 : 0);
    const char *names[4] = {"<100", "100-300", "300-800", ">800"};
    for (int i = 0; i < 4; i++) {
        if (bucket_n[i]) printf("  straight line %-8s: %3d routes, mean %.0f ticks, ratio %.2f\n", names[i], bucket_n[i], (double)buckets[i] / bucket_n[i], bucket_ratio[i] / bucket_n[i]);
    }
    CHECK(found > 100);
    CHECK(failed == 0); /* narrow straits (Gibraltar, Bosphorus, Danish) do not close seas */
    CHECK(invalid == 0);
    CHECK(max_ratio < 40.0); /* orthogonal steps; the worst case is around a peninsula */
    IntVec_free(&shores);
    IntVec_free(&path);

    /* Boats in a real game: launch from each nation toward a foreign shore. */
    OfeConfig c;
    ofe_config_default(&c);
    c.spawn_phase_ticks = 20;
    c.event_mask = OFE_EVENTS_NO_TILES;
    OfeGame *g = ofe_game_create(m, &c, 7);
    ofe_game_step(g, 400);
    int launched = 0, arrived = 0;
    double sum_ticks = 0;
    for (PlayerId p = 1; p < g->players.count && launched < 25; p++) {
        if (!player_alive(g, p)) continue;
        for (int tries = 0; tries < 40; tries++) {
            TileRef t = rng_int(&rs, 0, m->num_tiles);
            if (!map_is_shore(m, t) || tile_owner(g, t) == p) continue;
            UnitId id = ships_launch_transport(g, p, t, 50);
            if (id < 0) continue;
            launched++;
            int32_t s = unit_lookup(g, id);
            int32_t len = g->units.path[s].len;
            int64_t t0 = g->tick;
            int guard = 0;
            while (unit_lookup(g, id) >= 0 && guard++ < 6000) ofe_game_step(g, 1);
            if (unit_lookup(g, id) < 0) {
                arrived++;
                sum_ticks += (double)(g->tick - t0);
                CHECK(g->tick - t0 <= len + 2);
            }
            break;
        }
    }
    printf("world boats: %d launched, %d arrived, mean voyage %.0f ticks\n", launched, arrived, arrived ? sum_ticks / arrived : 0);
    CHECK(launched >= 5);
    check_invariants(g);
    ofe_game_destroy(g);

    /* A ship-heavy game: ports for every nation and a few warships. */
    OfeGame *h = ofe_game_create(m, &c, 8);
    ofe_game_step(h, 300);
    int ports = 0;
    for (PlayerId p = 1; p < h->players.count; p++) {
        if (!player_alive(h, p)) continue;
        const IntVec *bd = &h->players.border[p];
        for (int tries = 0; tries < 60 && bd->len > 0; tries++) {
            TileRef t = bd->data[rng_int(&rs, 0, bd->len)];
            if (map_is_shore(m, t) && water_tile_component(wg, m, t) == ocean) {
                ofe_edit_add_unit(h, OFE_UNIT_PORT, p, t, 1);
                ports++;
                if (ports % 7 == 0) {
                    TileRef nb[4];
                    int k = map_neighbors4(m, t, nb);
                    for (int j = 0; j < k; j++) {
                        if (map_is_water(m, nb[j])) {
                            ofe_edit_set_gold(h, p, 100000000);
                            ships_build_warship(h, p, nb[j]);
                            break;
                        }
                    }
                }
                break;
            }
        }
    }
    double t0 = now();
    int ticks = 3000;
    int peak_trade = 0;
    for (int i = 0; i < ticks; i++) {
        ofe_game_step(h, 1);
        ofe_game_clear_events(h);
        if (i % 100 == 0) peak_trade = OFE_MAX(peak_trade, count_units(h, OFE_UNIT_TRADE_SHIP));
    }
    double dt = now() - t0;
    printf("world with %d ports, %d warships: %.0f ticks/s, peak %d trade ships, %d now\n", ports,
           count_units(h, OFE_UNIT_WARSHIP), ticks / dt, peak_trade, count_units(h, OFE_UNIT_TRADE_SHIP));
    int64_t tg = 0;
    for (PlayerId p = 1; p < h->players.count; p++) tg += h->players.trade_gold[p];
    CHECK(tg > 0);
    OfeGame *hc = ofe_game_clone(h);
    ofe_game_step(h, 200);
    ofe_game_step(hc, 200);
    CHECK(ofe_game_hash(h) == ofe_game_hash(hc));
    check_invariants(h);
    ofe_game_destroy(hc);
    ofe_game_destroy(h);
    ofe_map_release(m);
}

int main(void) {
    (void)make_map;
    OfeMap *m = make_islands();
    test_components_and_routes(m);
    test_boat_attack(m);
    test_boat_limit_and_cancel(m);
    test_trade(m);
    test_warship(m);
    test_piracy(m);
    test_clone_mid_voyage(m);
    ofe_map_release(m);
    test_strait_detour();
    test_world();
    return test_summary("test_ships");
}

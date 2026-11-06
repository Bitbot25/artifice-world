/*
 * test_engine.c - core engine tests: invariants, determinism, clone/COW,
 * attacks, edits. Uses synthetic maps so it runs without map assets.
 *
 * Set OFE_MAPS_DIR to an OpenFront resources/maps directory to also run a
 * smoke test on a real map (world).
 */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

#include "../../engine/src/internal.h"

static int failures = 0;
#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);  \
            failures++;                                                      \
        }                                                                    \
    } while (0)

/* Island in an ocean, a lake in the middle, a mountain band. */
static OfeMap *make_map(int w, int h) {
    uint8_t *t = (uint8_t *)malloc((size_t)w * h);
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            int edge = x < 4 || y < 4 || x >= w - 4 || y >= h - 4;
            int lake = (x - w / 2) * (x - w / 2) + (y - h / 2) * (y - h / 2) < 36;
            uint8_t b;
            if (edge) b = TERRAIN_OCEAN_BIT;
            else if (lake) b = 0;
            else {
                int mag = (x > w / 3 && x < w / 3 + 6) ? 25 : (y % 17 == 0 ? 12 : 3);
                b = (uint8_t)(TERRAIN_LAND_BIT | mag);
            }
            t[y * w + x] = b;
        }
    }
    /* Shoreline bits: land next to water and water next to land. */
    uint8_t *s = (uint8_t *)malloc((size_t)w * h);
    memcpy(s, t, (size_t)w * h);
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            int land = t[y * w + x] & TERRAIN_LAND_BIT;
            int dx[4] = {1, -1, 0, 0}, dy[4] = {0, 0, 1, -1};
            for (int k = 0; k < 4; k++) {
                int xx = x + dx[k], yy = y + dy[k];
                if (xx < 0 || yy < 0 || xx >= w || yy >= h) continue;
                if ((t[yy * w + xx] & TERRAIN_LAND_BIT) != land) s[y * w + x] |= TERRAIN_SHORELINE_BIT;
            }
        }
    }
    OfeMap *m = ofe_map_from_terrain(w, h, s);
    ofe_map_add_nation(m, "Alpha", w / 4, h / 4);
    ofe_map_add_nation(m, "Beta", 3 * w / 4, 3 * h / 4);
    free(t);
    free(s);
    return m;
}

/* Recomputes tile counts and border lists from scratch and compares. */
static void check_invariants(const OfeGame *g) {
    const OfeMap *m = g->map;
    int32_t n = g->players.count;
    int32_t *count = (int32_t *)calloc((size_t)n, sizeof(int32_t));
    int32_t *borders = (int32_t *)calloc((size_t)n, sizeof(int32_t));
    for (TileRef t = 0; t < m->num_tiles; t++) {
        PlayerId o = tile_owner(g, t);
        CHECK(o >= 0 && o < n);
        if (o == 0) {
            CHECK(!tile_is_border(g, t));
            continue;
        }
        CHECK(map_is_ownable(m, t));
        count[o]++;
        TileRef nb[4];
        int k = map_neighbors4(m, t, nb);
        bool border = false;
        for (int i = 0; i < k; i++) border |= tile_owner(g, nb[i]) != o;
        CHECK(border == tile_is_border(g, t));
        if (border) {
            borders[o]++;
            int32_t pos = tile_border_pos_raw(&g->tiles, t);
            CHECK(pos >= 0 && pos < g->players.border[o].len && g->players.border[o].data[pos] == t);
        }
    }
    for (PlayerId p = 1; p < n; p++) {
        CHECK(count[p] == g->players.num_tiles[p]);
        CHECK(borders[p] == g->players.border[p].len);
        CHECK(g->players.troops[p] >= 0);
    }
    free(count);
    free(borders);
}

static OfeConfig base_config(void) {
    OfeConfig c;
    ofe_config_default(&c);
    c.num_humans = 2;
    c.num_bots = 12;
    c.spawn_phase_ticks = 10;
    c.event_mask = OFE_EVENTS_ALL;
    return c;
}

static void test_basic_run(OfeMap *m) {
    OfeConfig c = base_config();
    OfeGame *g = ofe_game_create(m, &c, 42);
    CHECK(g != NULL);
    CHECK(ofe_game_num_players(g) == 2 + 2 + 12);
    OfeIntent in[2] = {{0}};
    in[0].type = OFE_INTENT_SPAWN;
    in[0].player = 1;
    in[0].tile = map_ref(m, 20, 60);
    in[1].type = OFE_INTENT_SPAWN;
    in[1].player = 2;
    in[1].tile = map_ref(m, 100, 30);
    ofe_game_push_intents(g, in, 2);
    ofe_game_step(g, 12);
    CHECK(!ofe_game_in_spawn_phase(g));
    CHECK(g->players.spawned[1] && g->players.num_tiles[1] > 0);
    check_invariants(g);

    int64_t troops_before = g->players.troops[1];
    int64_t gold_before = g->players.gold[1];
    ofe_game_step(g, 10);
    CHECK(g->players.troops[1] > troops_before); /* troops grow */
    CHECK(g->players.gold[1] - gold_before == 10 * 100); /* 100 gold per tick */

    /* Attack the wilderness: territory grows. */
    int32_t tiles_before = g->players.num_tiles[1];
    OfeIntent atk = {0};
    atk.type = OFE_INTENT_ATTACK;
    atk.player = 1;
    atk.target = 0;
    atk.troops = -1;
    ofe_game_push_intents(g, &atk, 1);
    ofe_game_step(g, 50);
    CHECK(g->players.num_tiles[1] > tiles_before);
    check_invariants(g);

    ofe_game_step(g, 600);
    check_invariants(g);
    ofe_game_destroy(g);
}

static void test_determinism_and_clone(OfeMap *m) {
    OfeConfig c = base_config();
    OfeGame *a = ofe_game_create(m, &c, 7);
    OfeGame *b = ofe_game_create(m, &c, 7);
    ofe_game_step(a, 300);
    ofe_game_step(b, 300);
    CHECK(ofe_game_hash(a) == ofe_game_hash(b));

    /* Clone mid-game: both continue identically. */
    OfeGame *k = ofe_game_clone(a);
    CHECK(ofe_game_hash(k) == ofe_game_hash(a));
    ofe_game_step(a, 200);
    ofe_game_step(k, 200);
    CHECK(ofe_game_hash(k) == ofe_game_hash(a));
    check_invariants(k);

    /* Copy-on-write: editing the clone leaves the parent untouched. */
    uint64_t ha = ofe_game_hash(a);
    for (TileRef t = 0; t < m->num_tiles; t += 7) {
        if (map_is_ownable(m, t)) ofe_edit_set_owner(k, t, 1);
    }
    check_invariants(k);
    CHECK(ofe_game_hash(a) == ha);
    check_invariants(a);

    /* copy_into restores. */
    ofe_game_copy_into(k, a);
    CHECK(ofe_game_hash(k) == ha);
    check_invariants(k);

    /* Different seeds diverge. */
    OfeGame *d = ofe_game_create(m, &c, 8);
    ofe_game_step(d, 300);
    CHECK(ofe_game_hash(d) != ofe_game_hash(b));

    ofe_game_destroy(a);
    ofe_game_destroy(b);
    ofe_game_destroy(k);
    ofe_game_destroy(d);
}

static void test_batch_thread_independence(OfeMap *m) {
    OfeConfig c = base_config();
    enum { N = 6 };
    OfeGame *g1[N], *g2[N];
    for (int i = 0; i < N; i++) {
        g1[i] = ofe_game_create(m, &c, 100 + i);
        g2[i] = ofe_game_create(m, &c, 100 + i);
    }
    ofe_step_batch(g1, N, 250, 1);
    ofe_step_batch(g2, N, 250, 4);
    for (int i = 0; i < N; i++) {
        CHECK(ofe_game_hash(g1[i]) == ofe_game_hash(g2[i]));
        ofe_game_destroy(g1[i]);
        ofe_game_destroy(g2[i]);
    }
}

static void test_player_attack(OfeMap *m) {
    OfeConfig c = base_config();
    c.num_bots = 0;
    c.num_nations = 0;
    c.spawn_phase_ticks = 0;
    OfeGame *g = ofe_game_create(m, &c, 3);
    /* Two players side by side, set up with edits. */
    for (int y = 30; y < 50; y++) {
        for (int x = 10; x < 30; x++) ofe_edit_set_owner(g, map_ref(m, x, y), 1);
        for (int x = 30; x < 50; x++) ofe_edit_set_owner(g, map_ref(m, x, y), 2);
    }
    check_invariants(g);
    ofe_edit_set_troops(g, 1, 200000);
    ofe_edit_set_troops(g, 2, 20000);
    ofe_game_step(g, 60); /* past spawn immunity */
    OfeIntent atk = {0};
    atk.type = OFE_INTENT_ATTACK;
    atk.player = 1;
    atk.target = 2;
    atk.troops = 150000;
    ofe_game_push_intents(g, &atk, 1);
    ofe_game_step(g, 1);
    OfeAttackInfo info[4];
    CHECK(ofe_game_attacks(g, info, 4) == 1);
    int32_t before = g->players.num_tiles[2];
    ofe_game_step(g, 100);
    CHECK(g->players.num_tiles[2] < before);
    check_invariants(g);

    /* Alliance stops the attack. */
    ofe_edit_set_alliance(g, 1, 2, 1, 0);
    ofe_game_step(g, 2);
    CHECK(ofe_game_attacks(g, info, 4) == 0);
    ofe_game_destroy(g);
}

static void test_enclave_annexation(OfeMap *m) {
    OfeConfig c = base_config();
    c.num_bots = 0;
    c.num_nations = 0;
    c.spawn_phase_ticks = 0;
    OfeGame *g = ofe_game_create(m, &c, 5);
    for (int y = 20; y < 60; y++)
        for (int x = 50; x < 90; x++) ofe_edit_set_owner(g, map_ref(m, x, y), 1);
    /* A small pocket of player 2 inside player 1. */
    for (int y = 35; y < 40; y++)
        for (int x = 65; x < 70; x++) ofe_edit_set_owner(g, map_ref(m, x, y), 2);
    /* Player 2's main land elsewhere so it survives. */
    for (int y = 80; y < 100; y++)
        for (int x = 20; x < 40; x++) ofe_edit_set_owner(g, map_ref(m, x, y), 2);
    int32_t p2 = g->players.num_tiles[2];
    ofe_game_step(g, 30);
    CHECK(g->players.num_tiles[2] == p2 - 25);
    CHECK(tile_owner(g, map_ref(m, 67, 37)) == 1);
    check_invariants(g);
    ofe_game_destroy(g);
}

static void test_real_map(void) {
    const char *dir = getenv("OFE_MAPS_DIR");
    if (!dir) {
        printf("skip real-map test (set OFE_MAPS_DIR)\n");
        return;
    }
    char path[1024];
    snprintf(path, sizeof(path), "%s/world", dir);
    OfeMap *m = ofe_map_load(path, 0);
    CHECK(m != NULL);
    if (!m) return;
    CHECK(m->width == 2000 && m->height == 1000);
    CHECK(m->num_nations > 50);
    OfeConfig c;
    ofe_config_default(&c);
    c.spawn_phase_ticks = 20;
    OfeGame *g = ofe_game_create(m, &c, 1);
    ofe_game_step(g, 1000);
    check_invariants(g);
    ofe_game_destroy(g);
    ofe_map_release(m);
}

int main(void) {
    OfeMap *m = make_map(160, 120);
    test_basic_run(m);
    test_determinism_and_clone(m);
    test_batch_thread_independence(m);
    test_player_attack(m);
    test_enclave_annexation(m);
    test_real_map();
    ofe_map_release(m);
    if (failures) {
        fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    printf("all engine tests passed\n");
    return 0;
}

/*
 * test_doomsday.c - the doomsday clock: bar schedule, troop drain, territory
 * rot, recovery. Synthetic map, two humans, no AI.
 */
#include "test_util.h"

#define W 200
#define H 150

/* Jumps the clock so that `seconds` have elapsed since the start. */
static void set_elapsed(OfeGame *g, int seconds) { g->tick = g->start_tick + seconds * TICKS_PER_SECOND; }

static OfeGame *make_game(OfeMap *m, int enabled, int speed, int mode) {
    OfeConfig c = base_config();
    c.num_nations = 0;
    c.num_bots = 0;
    c.doomsday_enabled = enabled;
    c.doomsday_speed = speed;
    c.game_mode = mode;
    c.num_teams = 2;
    OfeGame *g = ofe_game_create(m, &c, 11);
    OfeIntent in[2] = {{0}};
    in[0].type = OFE_INTENT_SPAWN;
    in[0].player = 1;
    in[0].tile = map_ref(m, 30, 30);
    in[1].type = OFE_INTENT_SPAWN;
    in[1].player = 2;
    in[1].tile = map_ref(m, 170, 120);
    ofe_game_push_intents(g, in, 2);
    ofe_game_step(g, 12);
    /* Player 1: everything left of x=130 (about 60% of the land). */
    for (TileRef t = 0; t < m->num_tiles; t++) {
        if (map_is_ownable(m, t) && tile_owner(g, t) != 0) ofe_edit_set_owner(g, t, 0);
        if (map_is_ownable(m, t) && map_x(m, t) < 130) ofe_edit_set_owner(g, t, 1);
    }
    return g;
}

/* Gives player p every free ownable tile with x in [x0, x1]. */
static void claim(OfeGame *g, PlayerId p, int x0, int x1) {
    for (TileRef t = 0; t < g->map->num_tiles; t++) {
        int x = map_x(g->map, t);
        if (map_is_ownable(g->map, t) && x >= x0 && x <= x1 && tile_owner(g, t) == 0) ofe_edit_set_owner(g, t, p);
    }
}

static int count_events(OfeGame *g, int type, int player, int c) {
    const OfeEvent *ev;
    int n = ofe_game_events(g, &ev), k = 0;
    for (int i = 0; i < n; i++) k += ev[i].type == type && ev[i].b == player && ev[i].c == c;
    return k;
}

static void test_off_by_default(OfeMap *m) {
    OfeGame *g = make_game(m, 0, 1, OFE_MODE_FFA);
    claim(g, 2, 160, 165);
    set_elapsed(g, 2000);
    CHECK(doomsday_bar_tenths(g) == 0);
    int32_t tiles = g->players.num_tiles[2];
    ofe_game_step(g, 200);
    CHECK(g->players.doomsday_tick[2] == -1);
    CHECK(g->players.num_tiles[2] == tiles);
    ofe_game_destroy(g);
}

static void test_bar_schedule(OfeMap *m) {
    /* Fast: 10:00 grace, 102 s ramps, 31 s pauses, 35% at 25:00. */
    OfeGame *g = make_game(m, 1, 2, OFE_MODE_FFA);
    int32_t last = 0;
    for (int e = 0; e <= 1700; e += 7) {
        set_elapsed(g, e);
        int32_t bar = doomsday_bar_tenths(g);
        CHECK(bar >= last); /* never falls */
        last = bar;
        if (e <= 600) CHECK(bar == 0);
    }
    set_elapsed(g, 651);
    CHECK(doomsday_bar_tenths(g) == 10); /* halfway up the first 2% */
    set_elapsed(g, 710);
    CHECK(doomsday_bar_tenths(g) == 20); /* end of the first ramp, holding */
    set_elapsed(g, 730);
    CHECK(doomsday_bar_tenths(g) == 20);
    set_elapsed(g, 1500);
    CHECK(doomsday_bar_tenths(g) == 350);
    set_elapsed(g, 5000);
    CHECK(doomsday_bar_tenths(g) == 350);
    ofe_game_destroy(g);

    /* Slow reaches the same level later. */
    g = make_game(m, 1, 0, OFE_MODE_FFA);
    set_elapsed(g, 1500);
    CHECK(doomsday_bar_tenths(g) < 350 && doomsday_bar_tenths(g) > 0);
    set_elapsed(g, 2700);
    CHECK(doomsday_bar_tenths(g) == 350);
    ofe_game_destroy(g);

    /* Team games climb a higher ladder. */
    g = make_game(m, 1, 2, OFE_MODE_TEAM);
    set_elapsed(g, 702);
    CHECK(doomsday_bar_tenths(g) == 30);
    ofe_game_destroy(g);
}

static void test_drain_then_rot(OfeMap *m) {
    OfeGame *g = make_game(m, 1, 2, OFE_MODE_FFA);
    claim(g, 2, 160, 165);
    int64_t land = g->num_land_tiles;
    CHECK(g->players.num_tiles[1] * 100 / land > 40 && g->players.num_tiles[1] * 100 / land < 75);
    CHECK(g->players.num_tiles[2] * 100 / land < 10);
    set_elapsed(g, 2000); /* bar 35% */
    int32_t tiles0 = g->players.num_tiles[2];
    int64_t max_troops = (int64_t)player_max_troops(g, 2);
    ofe_edit_set_troops(g, 2, (double)max_troops);

    ofe_game_clear_events(g);
    ofe_game_step(g, 20);
    CHECK(g->players.doomsday_tick[2] >= 0);
    CHECK(g->players.doomsday_tick[1] == -1); /* the leader is never doomed */
    CHECK(count_events(g, OFE_EV_DOOMSDAY, 2, 1) == 1);
    int32_t entered = g->players.doomsday_tick[2];
    OfePlayerInfo info;
    ofe_game_player_info(g, 2, &info);
    CHECK(info.flags & OFE_PF_IN_DOOMSDAY);

    /* Warn period: no drain yet (troops only grow, capped at max). */
    ofe_edit_set_troops(g, 2, (double)max_troops);
    ofe_game_step(g, entered + 250 - g->tick);
    CHECK(g->players.troops[2] >= max_troops - 1);

    /* Past the warn: troops bleed toward the decaying floor. */
    ofe_game_step(g, 100);
    int64_t t1 = g->players.troops[2];
    CHECK(t1 < max_troops);
    ofe_game_step(g, 200);
    CHECK(g->players.troops[2] < t1);
    CHECK(g->players.num_tiles[2] == tiles0); /* no rot yet */

    /* By 125 s the floor is 5% and rot has begun. */
    ofe_game_step(g, entered + 1250 - g->tick);
    CHECK(g->players.troops[2] <= max_troops * 5 / 100 + 3000);
    CHECK(g->players.num_tiles[2] < tiles0);
    CHECK(g->players.num_tiles[2] > 0);
    CHECK(g->num_fallout_tiles > 0);
    check_invariants(g);

    /* Clone mid-rot: both continue identically. */
    OfeGame *k = ofe_game_clone(g);
    ofe_game_step(g, 100);
    ofe_game_step(k, 100);
    CHECK(ofe_game_hash(g) == ofe_game_hash(k));
    CHECK(g->players.num_tiles[2] == k->players.num_tiles[2]);
    ofe_game_destroy(k);

    /* Gone by the 150 s deadline. */
    ofe_game_step(g, entered + 1520 - g->tick);
    CHECK(g->players.num_tiles[2] == 0);
    CHECK(g->players.num_tiles[1] > 0);
    check_invariants(g);
    ofe_game_destroy(g);
}

static void test_recovery(OfeMap *m) {
    OfeGame *g = make_game(m, 1, 2, OFE_MODE_FFA);
    claim(g, 2, 160, 165);
    set_elapsed(g, 2000);
    ofe_edit_set_troops(g, 2, (double)(int64_t)player_max_troops(g, 2));
    ofe_game_step(g, 20);
    int32_t entered = g->players.doomsday_tick[2];
    CHECK(entered >= 0);
    ofe_game_step(g, entered + 600 - g->tick);
    int64_t draining = g->players.troops[2];
    CHECK(draining < (int64_t)player_max_troops(g, 2));

    /* Back over 35% of the clean land: flag cleared, troops recover. */
    ofe_game_clear_events(g);
    for (TileRef t = 0; t < m->num_tiles; t++) {
        if (map_is_ownable(m, t) && map_x(m, t) >= 90 && map_x(m, t) < 160) ofe_edit_set_owner(g, t, 2);
    }
    CHECK(g->players.num_tiles[2] * 100 / g->num_land_tiles >= 36); /* above the 35% bar, still behind the leader */
    CHECK(g->players.num_tiles[1] > g->players.num_tiles[2]);
    ofe_game_step(g, 20);
    CHECK(g->players.doomsday_tick[2] == -1);
    CHECK(count_events(g, OFE_EV_DOOMSDAY, 2, 0) == 1);
    int64_t t0 = g->players.troops[2];
    int32_t tiles = g->players.num_tiles[2];
    ofe_game_step(g, 600);
    CHECK(g->players.troops[2] > t0);
    CHECK(g->players.num_tiles[2] == tiles);
    ofe_game_destroy(g);
}

static void test_team_games(OfeMap *m) {
    /* A team is judged on its combined land. */
    OfeConfig c = base_config();
    c.num_nations = 0;
    c.num_bots = 0;
    c.num_humans = 3;
    c.doomsday_enabled = 1;
    c.doomsday_speed = 2;
    c.game_mode = OFE_MODE_TEAM;
    c.num_teams = 2;
    OfeGame *g = ofe_game_create(m, &c, 5);
    ofe_edit_end_spawn_phase(g);
    for (PlayerId p = 1; p <= 3; p++) CHECK(g->players.team[p] >= 0);
    for (TileRef t = 0; t < m->num_tiles; t++) {
        if (map_is_ownable(m, t) && tile_owner(g, t) != 0) ofe_edit_set_owner(g, t, 0);
        if (map_is_ownable(m, t) && map_x(m, t) < 100) ofe_edit_set_owner(g, t, 1);
    }
    claim(g, 2, 101, 104);
    claim(g, 3, 105, 108);
    set_elapsed(g, 1000);
    ofe_game_step(g, 20);
    int64_t land = g->num_land_tiles - g->num_fallout_tiles;
    int64_t bar = (int64_t)doomsday_bar_tenths(g) * land / 1000;
    for (PlayerId p = 2; p <= 3; p++) {
        int64_t side = 0;
        for (PlayerId q = 1; q <= 3; q++) {
            if (g->players.team[q] == g->players.team[p]) side += g->players.num_tiles[q];
        }
        bool leader = g->players.team[p] == g->players.team[1];
        if (!leader) CHECK((g->players.doomsday_tick[p] >= 0) == (side < bar));
    }
    ofe_game_destroy(g);
}

int main(void) {
    OfeMap *m = make_map(W, H);
    test_off_by_default(m);
    test_bar_schedule(m);
    test_drain_then_rot(m);
    test_recovery(m);
    test_team_games(m);
    ofe_map_release(m);
    return test_summary("test_doomsday");
}

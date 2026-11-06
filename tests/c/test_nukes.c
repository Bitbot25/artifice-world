/*
 * test_nukes.c - atom / hydrogen bombs, MIRVs, silo cooldowns and SAMs.
 *
 * Structures.c is not needed: silos, SAMs and cities are placed with
 * ofe_edit_add_unit (finished structures).
 */
#include "test_util.h"

#define W 400
#define H 300

static OfeGame *new_game(OfeMap *m, int immunity) {
    OfeConfig c = base_config();
    c.num_humans = 3;
    c.num_bots = 0;
    c.num_nations = 0;
    c.spawn_phase_ticks = 0;
    c.spawn_immunity_ticks = immunity;
    OfeGame *g = ofe_game_create(m, &c, 11);
    return g;
}

static void fill(OfeGame *g, PlayerId p, int x0, int y0, int x1, int y1) {
    for (int y = y0; y < y1; y++)
        for (int x = x0; x < x1; x++) ofe_edit_set_owner(g, map_ref(g->map, x, y), p);
}

static int count_events(const OfeGame *g, int type, int unit_type) {
    const OfeEvent *ev;
    int n = ofe_game_events(g, &ev), c = 0;
    for (int i = 0; i < n; i++) {
        if (ev[i].type == type && (unit_type < 0 || ev[i].b == unit_type)) c++;
    }
    return c;
}

static int owned_within(const OfeGame *g, PlayerId p, TileRef c, int r) {
    int n = 0;
    for (TileRef t = 0; t < g->map->num_tiles; t++) {
        if (tile_owner(g, t) == p && map_dist2(g->map, t, c) <= (int64_t)r * r) n++;
    }
    return n;
}

static int fallout_within(const OfeGame *g, TileRef c, int r) {
    int n = 0;
    for (TileRef t = 0; t < g->map->num_tiles; t++) {
        if (tile_has_fallout(g, t) && map_dist2(g->map, t, c) <= (int64_t)r * r) n++;
    }
    return n;
}

/* Player 1 in the west with a silo, player 2 in the middle. */
static OfeGame *standard_game(OfeMap *m) {
    OfeGame *g = new_game(m, 0);
    fill(g, 1, 20, 20, 120, 100);
    fill(g, 2, 130, 60, 300, 240);
    ofe_edit_add_unit(g, OFE_UNIT_MISSILE_SILO, 1, map_ref(m, 60, 60), 1);
    ofe_edit_set_gold(g, 1, 100000000);
    ofe_edit_set_troops(g, 2, 300000);
    ofe_game_step(g, 2);
    return g;
}

static int run_until_event(OfeGame *g, int type, int unit_type, int max_ticks) {
    ofe_game_clear_events(g);
    for (int i = 0; i < max_ticks; i++) {
        ofe_game_step(g, 1);
        if (count_events(g, type, unit_type) > 0) return i + 1;
    }
    return -1;
}

static void test_atom_bomb(OfeMap *m) {
    OfeGame *g = standard_game(m);
    TileRef target = map_ref(m, 240, 110);
    UnitId city_in = ofe_edit_add_unit(g, OFE_UNIT_CITY, 2, map_ref(m, 245, 110), 1);
    UnitId city_out = ofe_edit_add_unit(g, OFE_UNIT_CITY, 2, map_ref(m, 290, 110), 1);
    int64_t gold = g->players.gold[1];
    OfeGame *control = ofe_game_clone(g);

    UnitId nuke = nukes_launch(g, 1, OFE_UNIT_ATOM_BOMB, target, -1, true, false);
    CHECK(nuke >= 0);
    CHECK(gold - g->players.gold[1] == 750000);
    CHECK(count_events(g, OFE_EV_NUKE_LAUNCHED, OFE_UNIT_ATOM_BOMB) == 1);
    CHECK(g->units.path[unit_slot(nuke)].len > 5);

    int before_inner = owned_within(g, 2, target, 12);
    int before_ring = owned_within(g, 2, target, 30);
    CHECK(before_inner > 440);

    int ticks = run_until_event(g, OFE_EV_NUKE_DETONATED, OFE_UNIT_ATOM_BOMB, 400);
    CHECK(ticks > 10);
    ofe_game_step(control, ticks);
    check_invariants(g);

    CHECK(unit_lookup(g, nuke) < 0);
    CHECK(owned_within(g, 2, target, 12) == 0); /* inner radius is always cleared */
    int left_ring = owned_within(g, 2, target, 30);
    CHECK(left_ring > 0 && left_ring < before_ring); /* the outer ring is patchy */
    /* Nothing beyond the outer radius is touched. */
    CHECK(owned_within(g, 2, target, 31) - left_ring == owned_within(control, 2, target, 31) - owned_within(control, 2, target, 30));
    CHECK(fallout_within(g, target, 12) > 440);
    CHECK(fallout_within(g, target, 400) == g->num_fallout_tiles);
    CHECK(owned_within(g, 2, target, 60) > 0);
    CHECK(tile_has_fallout(g, target));
    CHECK(unit_lookup(g, city_in) < 0); /* structure in the blast */
    CHECK(unit_lookup(g, city_out) >= 0); /* structure outside */
    CHECK(g->players.troops[2] < control->players.troops[2] * 97 / 100);
    CHECK(g->players.num_tiles[2] < control->players.num_tiles[2]);

    const OfeEvent *ev;
    int n = ofe_game_events(g, &ev);
    for (int i = 0; i < n; i++) {
        if (ev[i].type == OFE_EV_NUKE_DETONATED) {
            CHECK(ev[i].d == target && ev[i].c == 1);
            CHECK(ev[i].v > 500 && ev[i].v < 2900);
        }
    }
    ofe_game_destroy(control);
    ofe_game_destroy(g);
}

static void test_hydrogen_bomb(OfeMap *m) {
    OfeGame *g = standard_game(m);
    TileRef target = map_ref(m, 245, 110);
    OfeGame *atom = ofe_game_clone(g);
    ofe_edit_set_gold(g, 1, 6000000);
    CHECK(nukes_launch(g, 1, OFE_UNIT_HYDROGEN_BOMB, target, -1, false, false) >= 0);
    CHECK(g->players.gold[1] == 1000000);
    CHECK(nukes_launch(atom, 1, OFE_UNIT_ATOM_BOMB, target, -1, true, false) >= 0);
    CHECK(run_until_event(g, OFE_EV_NUKE_DETONATED, OFE_UNIT_HYDROGEN_BOMB, 600) > 0);
    CHECK(run_until_event(atom, OFE_EV_NUKE_DETONATED, OFE_UNIT_ATOM_BOMB, 600) > 0);
    check_invariants(g);
    int h_cleared = fallout_within(g, target, 400);
    int a_cleared = fallout_within(atom, target, 400);
    CHECK(a_cleared > 0 && h_cleared > 5 * a_cleared);
    CHECK(owned_within(g, 2, target, 79) == 0);
    CHECK(owned_within(g, 2, target, 120) > 0);
    CHECK(g->players.troops[2] < atom->players.troops[2]);
    ofe_game_destroy(atom);
    ofe_game_destroy(g);
}

static void test_silo_cooldown_and_gold(OfeMap *m) {
    OfeGame *g = standard_game(m);
    TileRef target = map_ref(m, 240, 110);
    int32_t silo = -1;
    for (int32_t i = 0; i < g->units.count; i++) {
        if (g->units.alive[i] && g->units.type[i] == OFE_UNIT_MISSILE_SILO) silo = i;
    }
    CHECK(silo >= 0);
    CHECK(nukes_can_launch(g, 1, OFE_UNIT_ATOM_BOMB, target));
    CHECK(nukes_launch(g, 1, OFE_UNIT_ATOM_BOMB, target, -1, true, false) >= 0);
    CHECK(g->units.flags[silo] & OFE_UF_COOLDOWN);
    CHECK(g->units.timer[silo] == g->tick + 90);
    int64_t gold = g->players.gold[1];
    /* The only silo is reloading. */
    CHECK(!nukes_can_launch(g, 1, OFE_UNIT_ATOM_BOMB, target));
    CHECK(nukes_launch(g, 1, OFE_UNIT_ATOM_BOMB, target, -1, true, false) < 0);
    CHECK(g->players.gold[1] == gold); /* a refused launch costs nothing */
    ofe_game_step(g, 89);
    CHECK(nukes_launch(g, 1, OFE_UNIT_ATOM_BOMB, target, -1, true, false) < 0);
    ofe_game_step(g, 1);
    CHECK(nukes_launch(g, 1, OFE_UNIT_ATOM_BOMB, target, -1, true, false) >= 0);

    /* Not enough gold. */
    ofe_edit_set_gold(g, 1, 100);
    ofe_game_step(g, 90);
    CHECK(nukes_launch(g, 1, OFE_UNIT_ATOM_BOMB, target, -1, true, false) < 0);
    /* Someone else's silo / a player without one. */
    CHECK(nukes_launch(g, 2, OFE_UNIT_ATOM_BOMB, map_ref(m, 60, 60), -1, true, false) < 0);

    /* MIRV prices climb with every MIRV launched. */
    ofe_edit_set_gold(g, 1, 200000000);
    ofe_game_step(g, 90);
    CHECK(unit_cost(g, 1, OFE_UNIT_MIRV) == 25000000);
    int64_t before = g->players.gold[1];
    target = map_ref(m, 270, 190); /* MIRVs need owned ground; the old crater is bare */
    CHECK(nukes_launch(g, 1, OFE_UNIT_MIRV, target, -1, true, false) >= 0);
    CHECK(before - g->players.gold[1] == 25000000);
    CHECK(g->mirvs_launched == 1);
    CHECK(unit_cost(g, 1, OFE_UNIT_MIRV) == 40000000);
    ofe_game_destroy(g);

    /* A level-2 silo holds two missiles; the second trails the first. */
    g = standard_game(m);
    for (int32_t i = 0; i < g->units.count; i++) {
        if (g->units.alive[i] && g->units.type[i] == OFE_UNIT_MISSILE_SILO) {
            unit_set_level(g, i, 2);
            silo = i;
        }
    }
    UnitId a = nukes_launch(g, 1, OFE_UNIT_ATOM_BOMB, target, -1, true, false);
    UnitId b = nukes_launch(g, 1, OFE_UNIT_ATOM_BOMB, target, -1, true, false);
    CHECK(a >= 0 && b >= 0);
    CHECK(g->units.aux0[unit_slot(a)] == 0 && g->units.aux0[unit_slot(b)] == 1);
    CHECK(nukes_launch(g, 1, OFE_UNIT_ATOM_BOMB, target, -1, true, false) < 0);
    ofe_game_destroy(g);
}

static void test_spawn_immunity(OfeMap *m) {
    OfeGame *g = new_game(m, 50);
    fill(g, 1, 20, 20, 120, 100);
    fill(g, 2, 130, 60, 300, 240);
    ofe_edit_add_unit(g, OFE_UNIT_MISSILE_SILO, 1, map_ref(m, 60, 60), 1);
    ofe_edit_set_gold(g, 1, 100000000);
    ofe_game_step(g, 10);
    CHECK(nukes_launch(g, 1, OFE_UNIT_ATOM_BOMB, map_ref(m, 200, 150), -1, true, false) < 0);
    ofe_game_step(g, 45);
    CHECK(nukes_launch(g, 1, OFE_UNIT_ATOM_BOMB, map_ref(m, 200, 150), -1, true, false) >= 0);
    ofe_game_destroy(g);
}

static void test_sam(OfeMap *m) {
    TileRef target = map_ref(m, 200, 120);

    /* A SAM right next to the target shoots the bomb down. */
    OfeGame *g = standard_game(m);
    ofe_edit_add_unit(g, OFE_UNIT_SAM_LAUNCHER, 2, map_ref(m, 190, 130), 1);
    UnitId nuke = nukes_launch(g, 1, OFE_UNIT_ATOM_BOMB, target, -1, true, false);
    CHECK(nuke >= 0);
    bool saw_missile = false;
    for (int i = 0; i < 300 && unit_lookup(g, nuke) >= 0; i++) {
        ofe_game_step(g, 1);
        saw_missile |= g->players.unit_count[2][OFE_UNIT_SAM_MISSILE] > 0;
    }
    CHECK(unit_lookup(g, nuke) < 0);
    CHECK(saw_missile);
    CHECK(count_events(g, OFE_EV_NUKE_DETONATED, -1) == 0);
    CHECK(g->players.unit_count[2][OFE_UNIT_SAM_MISSILE] == 0);
    CHECK(fallout_within(g, target, 400) == 0);
    int32_t sam = -1;
    for (int32_t i = 0; i < g->units.count; i++) {
        if (g->units.alive[i] && g->units.type[i] == OFE_UNIT_SAM_LAUNCHER) sam = i;
    }
    CHECK(g->units.aux0[sam] == 1 && (g->units.flags[sam] & OFE_UF_COOLDOWN));
    ofe_game_step(g, 90);
    CHECK(g->units.aux0[sam] == 0 && !(g->units.flags[sam] & OFE_UF_COOLDOWN));
    ofe_game_destroy(g);

    /* Range: 75 tiles away is out of reach for level 1 (70) but not level 2. */
    for (int level = 1; level <= 2; level++) {
        g = standard_game(m);
        ofe_edit_add_unit(g, OFE_UNIT_SAM_LAUNCHER, 2, map_ref(m, 275, 120), level);
        nuke = nukes_launch(g, 1, OFE_UNIT_ATOM_BOMB, target, -1, true, false);
        run_until_event(g, OFE_EV_NUKE_DETONATED, -1, 400);
        int detonated = count_events(g, OFE_EV_NUKE_DETONATED, -1);
        CHECK(detonated == (level == 1 ? 1 : 0));
        ofe_game_destroy(g);
    }

    /* A SAM owned by an ally of the launcher holds fire. */
    g = standard_game(m);
    fill(g, 3, 140, 100, 160, 140);
    ofe_edit_add_unit(g, OFE_UNIT_SAM_LAUNCHER, 3, map_ref(m, 150, 120), 1);
    ofe_edit_set_alliance(g, 1, 3, 1, 0);
    nuke = nukes_launch(g, 1, OFE_UNIT_ATOM_BOMB, target, -1, true, false);
    CHECK(nuke >= 0);
    run_until_event(g, OFE_EV_NUKE_DETONATED, -1, 400);
    CHECK(count_events(g, OFE_EV_NUKE_DETONATED, -1) == 1);
    ofe_game_destroy(g);

    /* A SAM that is still being built does nothing. */
    g = standard_game(m);
    UnitId s = ofe_edit_add_unit(g, OFE_UNIT_SAM_LAUNCHER, 2, map_ref(m, 190, 130), 1);
    g->units.flags[unit_slot(s)] |= OFE_UF_UNDER_CONSTRUCTION;
    nukes_launch(g, 1, OFE_UNIT_ATOM_BOMB, target, -1, true, false);
    run_until_event(g, OFE_EV_NUKE_DETONATED, -1, 400);
    CHECK(count_events(g, OFE_EV_NUKE_DETONATED, -1) == 1);
    ofe_game_destroy(g);

    /* An upgrade ramps the range up over 45 ticks. */
    g = standard_game(m);
    UnitId up = ofe_edit_add_unit(g, OFE_UNIT_SAM_LAUNCHER, 2, map_ref(m, 275, 120), 1);
    int32_t us = unit_slot(up);
    unit_set_level(g, us, 2);
    nukes_on_level_up(g, us);
    CHECK(g->units.aux0[us] == 1); /* the new missile is reloading */
    CHECK(g->units.aux3[us] == 2 && g->units.auxf[us] > 69.9 && g->units.auxf[us] < 70.1);
    ofe_game_destroy(g);
}

static void test_alliances(OfeMap *m) {
    TileRef target = map_ref(m, 240, 110);
    OfeGame *g = standard_game(m);
    ofe_edit_set_alliance(g, 1, 2, 1, 0);
    CHECK(player_allied(g, 1, 2));
    UnitId nuke = nukes_launch(g, 1, OFE_UNIT_ATOM_BOMB, target, -1, true, false);
    CHECK(nuke >= 0);
    CHECK(!player_allied(g, 1, 2)); /* a heavy hit on an ally breaks the alliance */
    CHECK(player_is_traitor(g, 1));
    ofe_game_destroy(g);

    /* A nuke at an ally's lone corner tile does not (under the threshold). */
    g = standard_game(m);
    ofe_edit_set_alliance(g, 1, 2, 1, 0);
    TileRef edge = map_ref(m, 105, 150); /* only a sliver of player 2 is in range */
    CHECK(nukes_launch(g, 1, OFE_UNIT_ATOM_BOMB, edge, -1, true, false) >= 0);
    CHECK(player_allied(g, 1, 2));
    ofe_game_destroy(g);

    /* Becoming allies cancels nukes in flight between the two. */
    g = standard_game(m);
    nuke = nukes_launch(g, 1, OFE_UNIT_ATOM_BOMB, target, -1, true, false);
    ofe_game_step(g, 5);
    CHECK(unit_lookup(g, nuke) >= 0);
    ofe_edit_set_alliance(g, 1, 2, 1, 0);
    nukes_cancel_between(g, 1, 2);
    CHECK(unit_lookup(g, nuke) < 0);
    ofe_game_step(g, 300);
    CHECK(count_events(g, OFE_EV_NUKE_DETONATED, -1) == 0);
    ofe_game_destroy(g);

    /* Teammates can't be nuked. */
    OfeConfig c = base_config();
    c.game_mode = OFE_MODE_TEAM;
    c.num_teams = 2;
    c.num_humans = 4;
    c.num_bots = 0;
    c.num_nations = 0;
    c.spawn_phase_ticks = 0;
    c.spawn_immunity_ticks = 0;
    g = ofe_game_create(m, &c, 3);
    PlayerId mate = 0;
    for (PlayerId p = 2; p <= 4; p++) {
        if (player_same_team(g, 1, p)) mate = p;
    }
    CHECK(mate != 0);
    fill(g, 1, 20, 20, 120, 100);
    fill(g, mate, 130, 60, 300, 240);
    ofe_edit_add_unit(g, OFE_UNIT_MISSILE_SILO, 1, map_ref(m, 60, 60), 1);
    ofe_edit_set_gold(g, 1, 100000000);
    ofe_game_step(g, 2);
    CHECK(!nukes_can_launch(g, 1, OFE_UNIT_ATOM_BOMB, target));
    CHECK(nukes_launch(g, 1, OFE_UNIT_ATOM_BOMB, target, -1, true, false) < 0);
    CHECK(nukes_can_launch(g, 1, OFE_UNIT_ATOM_BOMB, map_ref(m, 300, 280)));
    ofe_game_destroy(g);
}

static void test_mirv(OfeMap *m) {
    OfeGame *g = standard_game(m);
    ofe_edit_set_alliance(g, 1, 2, 1, 0);
    TileRef target = map_ref(m, 245, 110);
    UnitId mirv = nukes_launch(g, 1, OFE_UNIT_MIRV, target, -1, true, false);
    CHECK(mirv >= 0);
    CHECK(!player_allied(g, 1, 2)); /* launching a MIRV at an ally is a betrayal */
    CHECK(player_relation(g, 2, 1) == 0.0); /* nobody tracks relations here */
    int64_t troops_before = g->players.troops[2];

    int max_warheads = 0;
    TileRef first[400];
    int first_n = 0;
    for (int i = 0; i < 400; i++) {
        ofe_game_step(g, 1);
        int w = g->players.unit_count[1][OFE_UNIT_MIRV_WARHEAD];
        if (w > max_warheads) {
            max_warheads = w;
            if (!first_n) {
                for (int32_t s = 0; s < g->units.count; s++) {
                    if (g->units.alive[s] && g->units.type[s] == OFE_UNIT_MIRV_WARHEAD) first[first_n++] = g->units.target_tile[s];
                }
            }
        }
        if (unit_lookup(g, mirv) < 0 && w == 0) break;
    }
    CHECK(max_warheads >= 8);
    CHECK(first_n == max_warheads);
    bool spaced = true, owned = true;
    for (int i = 0; i < first_n; i++) {
        owned &= tile_owner(g, first[i]) != 1;
        for (int j = i + 1; j < first_n; j++) {
            if (map_manhattan(m, first[i], first[j]) < 55) spaced = false;
        }
    }
    CHECK(spaced);
    CHECK(owned);
    int hits = count_events(g, OFE_EV_NUKE_DETONATED, OFE_UNIT_MIRV_WARHEAD);
    CHECK(hits == max_warheads);
    CHECK(g->players.unit_count[1][OFE_UNIT_MIRV_WARHEAD] == 0);
    CHECK(g->players.troops[2] < troops_before);
    CHECK(fallout_within(g, target, 400) > 1500);
    /* Hits spread over the territory, not one spot. */
    int far = 0;
    for (int i = 0; i < first_n; i++) far += map_manhattan(m, first[i], target) > 60;
    CHECK(far >= 3);
    check_invariants(g);
    ofe_game_destroy(g);
}

/* A small holding in the middle of a crater expands slower than the same
 * holding on clean ground. */
static void test_fallout_slows_attacks(OfeMap *m) {
    OfeGame *g = new_game(m, 0);
    ofe_edit_add_unit(g, OFE_UNIT_MISSILE_SILO, 1, map_ref(m, 60, 60), 1);
    fill(g, 1, 20, 20, 120, 100);
    ofe_edit_set_gold(g, 1, 100000000);
    ofe_game_step(g, 2);
    OfeGame *control = ofe_game_clone(g);
    TileRef c = map_ref(m, 300, 150);
    nukes_launch(g, 1, OFE_UNIT_ATOM_BOMB, c, -1, true, false);
    CHECK(run_until_event(g, OFE_EV_NUKE_DETONATED, -1, 400) > 0);
    CHECK(g->num_fallout_tiles > 450);
    CHECK(tile_has_fallout(g, c));

    OfeGame *both[2] = {g, control};
    int32_t start[2];
    for (int k = 0; k < 2; k++) {
        fill(both[k], 3, 298, 148, 303, 153);
        ofe_edit_set_troops(both[k], 3, 200000);
        start[k] = both[k]->players.num_tiles[3];
        OfeIntent atk = {0};
        atk.type = OFE_INTENT_ATTACK;
        atk.player = 3;
        atk.target = 0;
        atk.troops = 5000;
        ofe_game_push_intents(both[k], &atk, 1);
        ofe_game_step(both[k], 40);
    }
    int32_t gain_fallout = g->players.num_tiles[3] - start[0];
    int32_t gain_clean = control->players.num_tiles[3] - start[1];
    CHECK(gain_clean > 100);
    CHECK(gain_fallout * 2 < gain_clean);
    ofe_game_destroy(control);
    ofe_game_destroy(g);
}

static OfeGame *busy_game(OfeMap *m) {
    OfeGame *g = standard_game(m);
    fill(g, 3, 300, 60, 340, 100);
    ofe_edit_add_unit(g, OFE_UNIT_MISSILE_SILO, 1, map_ref(m, 70, 70), 2);
    ofe_edit_add_unit(g, OFE_UNIT_SAM_LAUNCHER, 2, map_ref(m, 200, 130), 2);
    ofe_edit_add_unit(g, OFE_UNIT_SAM_LAUNCHER, 2, map_ref(m, 230, 170), 1);
    ofe_edit_add_unit(g, OFE_UNIT_SAM_LAUNCHER, 3, map_ref(m, 320, 80), 1);
    for (int32_t i = 0; i < g->units.count; i++) {
        if (g->units.alive[i] && g->units.type[i] == OFE_UNIT_MISSILE_SILO) unit_set_level(g, i, 2);
    }
    nukes_launch(g, 1, OFE_UNIT_ATOM_BOMB, map_ref(m, 205, 135), -1, true, false);
    nukes_launch(g, 1, OFE_UNIT_HYDROGEN_BOMB, map_ref(m, 250, 190), -1, false, false);
    nukes_launch(g, 1, OFE_UNIT_MIRV, map_ref(m, 245, 110), -1, true, false);
    return g;
}

static void test_clone_mid_flight(OfeMap *m) {
    OfeGame *a = busy_game(m), *b = busy_game(m);
    ofe_game_step(a, 25);
    ofe_game_step(b, 25);
    CHECK(ofe_game_hash(a) == ofe_game_hash(b)); /* deterministic */
    OfeGame *k = ofe_game_clone(a);
    CHECK(ofe_game_hash(k) == ofe_game_hash(a));
    OfeGame *r = busy_game(m);
    ofe_game_step(r, 3);
    ofe_game_copy_into(r, a); /* restore into a used game */
    for (int round = 0; round < 12; round++) {
        ofe_game_step(a, 30);
        ofe_game_step(k, 30);
        ofe_game_step(r, 30);
        CHECK(ofe_game_hash(k) == ofe_game_hash(a));
        CHECK(ofe_game_hash(r) == ofe_game_hash(a));
    }
    CHECK(count_events(a, OFE_EV_NUKE_DETONATED, -1) > 0);
    check_invariants(k);
    /* The clone is independent: stepping it further leaves the parent alone. */
    uint64_t h = ofe_game_hash(a);
    nukes_launch(k, 1, OFE_UNIT_ATOM_BOMB, map_ref(m, 250, 100), -1, true, true);
    ofe_game_step(k, 10);
    CHECK(ofe_game_hash(a) == h);
    ofe_game_destroy(a);
    ofe_game_destroy(b);
    ofe_game_destroy(k);
    ofe_game_destroy(r);
}

/* World map smoke test: a MIRV and a hydrogen bomb on a big nation, with
 * SAMs scattered around it. */
static void test_real_map(void) {
    OfeMap *m = load_real_map("world");
    if (!m) {
        printf("skip real-map nuke test (set OFE_MAPS_DIR)\n");
        return;
    }
    OfeConfig c;
    ofe_config_default(&c);
    c.num_humans = 1;
    c.random_spawn = 1;
    c.spawn_phase_ticks = 20;
    c.event_mask = OFE_EVENTS_NO_TILES;
    OfeGame *g = ofe_game_create(m, &c, 5);
    ofe_game_step(g, 400);
    PlayerId big = 2;
    for (PlayerId p = 3; p < g->players.count; p++) {
        if (g->players.num_tiles[p] > g->players.num_tiles[big]) big = p;
    }
    CHECK(big > 0 && player_alive(g, big));
    TileRef target = g->players.spawn_tile[big];
    PlayerId me = 1;
    if (!player_alive(g, me)) {
        ofe_game_destroy(g);
        ofe_map_release(m);
        return;
    }
    ofe_edit_add_unit(g, OFE_UNIT_MISSILE_SILO, me, g->players.spawn_tile[me], 3);
    for (int i = 0; i < 3; i++) {
        TileRef t = g->players.border[big].data[(i * 7) % g->players.border[big].len];
        ofe_edit_add_unit(g, OFE_UNIT_SAM_LAUNCHER, big, t, 1 + i % 3);
    }
    ofe_edit_set_gold(g, me, 200000000);
    CHECK(nukes_launch(g, me, OFE_UNIT_MIRV, target, -1, true, false) >= 0);
    CHECK(nukes_launch(g, me, OFE_UNIT_HYDROGEN_BOMB, target, -1, false, false) >= 0);
    int32_t peak = 0;
    for (int i = 0; i < 60; i++) {
        ofe_game_step(g, 10);
        peak = OFE_MAX(peak, g->num_fallout_tiles);
    }
    check_invariants(g);
    CHECK(count_events(g, OFE_EV_NUKE_DETONATED, -1) > 0);
    CHECK(peak > 0);
    ofe_game_destroy(g);
    ofe_map_release(m);
}

int main(void) {
    OfeMap *m = make_map(W, H);
    test_atom_bomb(m);
    test_hydrogen_bomb(m);
    test_silo_cooldown_and_gold(m);
    test_spawn_immunity(m);
    test_sam(m);
    test_alliances(m);
    test_mirv(m);
    test_fallout_slows_attacks(m);
    test_clone_mid_flight(m);
    test_real_map();
    ofe_map_release(m);
    return test_summary("test_nukes");
}

/*
 * test_ai.c - the built-in AIs: tribes expand then fight, nations grow faster
 * on harder difficulties, alliance answers, retaliation, determinism across
 * clones, and (with OFE_MAPS_DIR) a long run on the World map.
 *
 * Ships, structures and nukes may be stubs in a given build; nothing here
 * depends on them.
 */
#include "test_util.h"
#include "../../engine/src/ai_common.h"

/* A bigger island with six nations spread out. */
static OfeMap *make_nation_map(void) {
    OfeMap *m = make_map(240, 180);
    static const int xs[6] = {50, 120, 190, 50, 120, 190}, ys[6] = {45, 45, 45, 135, 135, 135};
    char name[16];
    for (int i = 0; i < 6; i++) {
        snprintf(name, sizeof(name), "Nation %d", i);
        ofe_map_add_nation(m, name, xs[i], ys[i]);
    }
    return m;
}

static int32_t total_tiles(const OfeGame *g, int type) {
    int32_t sum = 0;
    for (PlayerId p = 1; p < g->players.count; p++) {
        if (g->players.type[p] == type) sum += g->players.num_tiles[p];
    }
    return sum;
}

static int32_t alive_count(const OfeGame *g, int type) {
    int32_t n = 0;
    for (PlayerId p = 1; p < g->players.count; p++) n += g->players.type[p] == type && g->players.num_tiles[p] > 0;
    return n;
}

/* ---- Tribes ------------------------------------------------------------------ */

static void test_tribes_expand_then_fight(OfeMap *m) {
    OfeConfig c = base_config();
    c.num_humans = 0;
    c.num_nations = 0;
    c.num_bots = 12;
    OfeGame *g = ofe_game_create(m, &c, 11);
    ofe_game_step(g, 12);
    int32_t start_tiles = total_tiles(g, OFE_PLAYER_BOT);
    CHECK(alive_count(g, OFE_PLAYER_BOT) == 12);

    ofe_game_step(g, 600);
    CHECK(total_tiles(g, OFE_PLAYER_BOT) > start_tiles * 10); /* grabbed free land */
    check_invariants(g);

    /* Once the land is gone they turn on each other. */
    int32_t tribe_attacks = 0;
    for (int i = 0; i < 80; i++) {
        ofe_game_step(g, 50);
        const OfeEvent *ev;
        int32_t n = ofe_game_events(g, &ev);
        for (int32_t k = 0; k < n; k++) {
            if (ev[k].type == OFE_EV_ATTACK_STARTED && ev[k].c != 0) tribe_attacks++;
        }
        ofe_game_clear_events(g);
    }
    CHECK(tribe_attacks > 0);
    CHECK(alive_count(g, OFE_PLAYER_BOT) < 12);
    check_invariants(g);
    ofe_game_destroy(g);
}

/* Tribes accept any alliance request, on their next turn rather than at once. */
static void test_tribe_accepts_alliance(OfeMap *m) {
    OfeConfig c = base_config();
    c.num_humans = 1;
    c.num_nations = 0;
    c.num_bots = 1;
    c.spawn_phase_ticks = 0;
    OfeGame *g = ofe_game_create(m, &c, 5);
    for (int y = 30; y < 50; y++) {
        for (int x = 10; x < 30; x++) ofe_edit_set_owner(g, map_ref(m, x, y), 1);
        for (int x = 30; x < 50; x++) ofe_edit_set_owner(g, map_ref(m, x, y), 2);
    }
    OfeIntent in = {0};
    in.type = OFE_INTENT_ALLIANCE_REQUEST;
    in.player = 1;
    in.target = 2;
    ofe_game_push_intents(g, &in, 1);
    ofe_game_step(g, 2);
    OfeAllianceInfo al[2];
    CHECK(ofe_game_alliances(g, al, 2) == 0); /* not answered the moment it arrives */
    ofe_game_step(g, 200);
    CHECK(ofe_game_alliances(g, al, 2) == 1);
    ofe_game_destroy(g);
}

/* ---- Nations ------------------------------------------------------------------ */

/* ---- Nation spawn and opening ------------------------------------------------ */

/* Nations settle near their map spot (not on it) and hop to a new spot on
 * their beat; tribes were there first, so no nation's disc overlaps one. */
static void test_nation_spawn_hops(void) {
    OfeMap *m = make_nation_map();
    OfeConfig c = base_config();
    c.num_humans = 0;
    c.num_bots = 10;
    c.num_nations = -1;
    c.spawn_phase_ticks = 200;
    OfeGame *g = ofe_game_create(m, &c, 77);
    int32_t spawn_events[64] = {0};
    TileRef first[64], last[64];
    for (PlayerId p = 1; p < g->players.count; p++) {
        CHECK(g->players.type[p] != OFE_PLAYER_NATION || g->players.spawned[p]); /* placed at creation */
        first[p] = g->players.spawn_tile[p];
    }
    for (int t = 0; t < 200; t++) {
        ofe_game_step(g, 1);
        const OfeEvent *ev;
        int32_t n = ofe_game_events(g, &ev);
        for (int32_t k = 0; k < n; k++) {
            if (ev[k].type == OFE_EV_PLAYER_SPAWNED && g->players.type[ev[k].b] == OFE_PLAYER_NATION) spawn_events[ev[k].b]++;
        }
        ofe_game_clear_events(g);
    }
    int32_t hopped = 0, near_spot = 0, nations = 0;
    for (PlayerId p = 1; p < g->players.count; p++) {
        if (g->players.type[p] != OFE_PLAYER_NATION) continue;
        nations++;
        last[p] = g->players.spawn_tile[p];
        hopped += spawn_events[p] > 0 && last[p] != first[p];
        const Nation *n = &m->nations[g->players.nation_index[p]];
        near_spot += abs(map_x(m, last[p]) - map_x(m, n->tile)) <= 25 && abs(map_y(m, last[p]) - map_y(m, n->tile)) <= 25;
        CHECK(g->players.num_tiles[p] > 0 && g->players.num_tiles[p] <= 52);
    }
    CHECK(nations == m->num_nations);
    CHECK(near_spot == nations);
    CHECK(hopped >= 4);
    check_invariants(g);
    ofe_game_destroy(g);
    ofe_map_release(m);
}

/* The opening move (half the troops at free land) comes on the first tick
 * after the spawn phase, not on the nation's beat. */
static void test_nation_opening_is_immediate(void) {
    OfeMap *m = make_nation_map();
    OfeConfig c = base_config();
    c.num_humans = 0;
    c.num_bots = 0;
    c.num_nations = -1;
    c.spawn_phase_ticks = 30;
    OfeGame *g = ofe_game_create(m, &c, 5);
    ofe_game_step(g, 31); /* tick 30: phase running; tick 31 ends it */
    ofe_game_clear_events(g);
    ofe_game_step(g, 3);
    const OfeEvent *ev;
    int32_t n = ofe_game_events(g, &ev), openings = 0;
    for (int32_t k = 0; k < n; k++) openings += ev[k].type == OFE_EV_ATTACK_STARTED && ev[k].c == 0;
    CHECK(openings == m->num_nations);
    ofe_game_destroy(g);
    ofe_map_release(m);
}


static int32_t nation_tiles_after(OfeMap *m, int difficulty, uint64_t seed, int ticks) {
    OfeConfig c = base_config();
    c.num_humans = 0;
    c.num_bots = 0;
    c.num_nations = -1;
    c.difficulty = difficulty;
    OfeGame *g = ofe_game_create(m, &c, seed);
    ofe_game_step(g, ticks);
    check_invariants(g);
    int32_t t = total_tiles(g, OFE_PLAYER_NATION);
    ofe_game_destroy(g);
    return t;
}

#define GROWTH_TICKS 300

static void test_difficulty_growth(void) {
    OfeMap *m = make_nation_map();
    int64_t easy = 0, impossible = 0;
    for (uint64_t seed = 1; seed <= 3; seed++) {
        easy += nation_tiles_after(m, OFE_DIFFICULTY_EASY, seed, GROWTH_TICKS);
        impossible += nation_tiles_after(m, OFE_DIFFICULTY_IMPOSSIBLE, seed, GROWTH_TICKS);
    }
    printf("  nation tiles after %d ticks (3 seeds): easy %lld, impossible %lld\n", GROWTH_TICKS, (long long)easy, (long long)impossible);
    CHECK(easy > 0);
    CHECK(impossible > easy * 1.15);
    ofe_map_release(m);
}

/* Agent 1 (human) next to nation 2 on a map with no free land. */
static OfeGame *make_standoff(OfeMap *m, int difficulty, uint64_t seed) {
    OfeConfig c = base_config();
    c.num_humans = 1;
    c.num_bots = 0;
    c.num_nations = 1;
    c.difficulty = difficulty;
    c.spawn_phase_ticks = 0;
    OfeGame *g = ofe_game_create(m, &c, seed);
    for (TileRef t = 0; t < m->num_tiles; t++) {
        if (!map_is_ownable(m, t)) continue;
        ofe_edit_set_owner(g, t, map_x(m, t) < m->width / 2 ? 1 : 2);
    }
    ofe_edit_set_troops(g, 1, 60000);
    ofe_edit_set_troops(g, 2, 40000);
    return g;
}

static int32_t answer_alliance(OfeMap *m, int difficulty, double relation, uint64_t seed) {
    OfeGame *g = make_standoff(m, difficulty, seed);
    ofe_edit_set_relation(g, 2, 1, relation);
    ofe_game_step(g, 3); /* requests from the first ticks count as spawn-phase requests and are refused */
    g->players.ai_next_tick[2] = g->tick + 20; /* its next full turn */
    g->players.ai_state[2][AI_PHASE] = 0;
    OfeIntent in = {0};
    in.type = OFE_INTENT_ALLIANCE_REQUEST;
    in.player = 1;
    in.target = 2;
    ofe_game_push_intents(g, &in, 1);
    ofe_game_step(g, 3);
    OfeAllianceInfo al[2];
    CHECK(ofe_game_alliances(g, al, 2) == 0); /* the nation answers on its own turn */
    ofe_game_step(g, 30);
    int32_t formed = ofe_game_alliances(g, al, 2);
    ofe_game_destroy(g);
    return formed;
}

static void test_alliance_answers(OfeMap *m) {
    for (int diff = OFE_DIFFICULTY_MEDIUM; diff <= OFE_DIFFICULTY_IMPOSSIBLE; diff++) {
        int32_t friendly = 0, hostile = 0;
        for (uint64_t seed = 1; seed <= 24; seed++) {
            friendly += answer_alliance(m, diff, 80, seed);
            hostile += answer_alliance(m, diff, -80, seed);
        }
        printf("  difficulty %d: accepted %d/24 friendly, %d/24 hostile\n", diff, friendly, hostile);
        CHECK(friendly >= 20); /* a friendly, strong neighbour is (almost) always accepted */
        CHECK(hostile <= 3); /* a hostile one almost never (only by confusion) */
    }
}

/* Did `attacker` start an attack on `target` in the events since the last clear? */
static bool started_attack(const OfeGame *g, PlayerId attacker, PlayerId target) {
    const OfeEvent *ev;
    int32_t n = ofe_game_events(g, &ev);
    for (int32_t k = 0; k < n; k++) {
        if (ev[k].type == OFE_EV_ATTACK_STARTED && ev[k].b == attacker && ev[k].c == target) return true;
    }
    return false;
}

static void test_retaliation(OfeMap *m) {
    for (int diff = OFE_DIFFICULTY_EASY; diff <= OFE_DIFFICULTY_IMPOSSIBLE; diff++) {
        int hit_back = 0;
        for (uint64_t seed = 1; seed <= 6; seed++) {
            OfeGame *g = make_standoff(m, diff, seed);
            ofe_game_step(g, 150); /* past spawn immunity and the nation's opening move */
            ofe_edit_set_troops(g, 1, 500000);
            OfePlayerInfo info;
            ofe_game_player_info(g, 2, &info);
            ofe_edit_set_troops(g, 2, info.max_troops * 0.7);
            OfeIntent atk = {0};
            atk.type = OFE_INTENT_ATTACK;
            atk.player = 1;
            atk.target = 2;
            atk.troops = 400000; /* a long-lived attack: still incoming when the nation decides */
            ofe_game_push_intents(g, &atk, 1);
            ofe_game_clear_events(g);
            bool retaliated = false;
            for (int i = 0; i < 80 && !retaliated; i++) {
                ofe_game_step(g, 5);
                retaliated = started_attack(g, 2, 1);
            }
            hit_back += retaliated;
            check_invariants(g);
            ofe_game_destroy(g);
        }
        printf("  difficulty %d: retaliated in %d/6 games\n", diff, hit_back);
        CHECK(hit_back >= 5);
    }
}

/* ---- Determinism --------------------------------------------------------------- */

static void test_determinism_and_clone(void) {
    OfeMap *m = make_nation_map();
    OfeConfig c = base_config();
    c.num_humans = 1;
    c.humans_use_ai = 1;
    c.num_bots = 30;
    c.difficulty = OFE_DIFFICULTY_HARD;
    OfeGame *a = ofe_game_create(m, &c, 99);
    OfeGame *b = ofe_game_create(m, &c, 99);
    ofe_game_step(a, 500);
    ofe_game_step(b, 500);
    CHECK(ofe_game_hash(a) == ofe_game_hash(b));

    /* A clone and its parent evolve identically (the AI state is copied). */
    OfeGame *cl = ofe_game_clone(a);
    ofe_game_step(a, 600);
    ofe_game_step(cl, 600);
    CHECK(ofe_game_hash(a) == ofe_game_hash(cl));

    /* Restoring into an existing game too. */
    OfeGame *r = ofe_game_clone(b);
    ofe_game_step(r, 300);
    ofe_game_copy_into(r, b);
    ofe_game_step(r, 600);
    ofe_game_step(b, 600);
    CHECK(ofe_game_hash(r) == ofe_game_hash(b));
    check_invariants(a);
    ofe_game_destroy(a);
    ofe_game_destroy(b);
    ofe_game_destroy(cl);
    ofe_game_destroy(r);
    ofe_map_release(m);
}

/* Teams and no-alliance games exercise other branches of the decisions. */
static void test_modes(void) {
    OfeMap *m = make_nation_map();
    for (int mode = 0; mode < 3; mode++) {
        for (int diff = 0; diff < 4; diff++) {
            OfeConfig c = base_config();
            c.num_humans = 1;
            c.humans_use_ai = 1;
            c.num_bots = 20;
            c.difficulty = diff;
            c.game_mode = mode == 1 ? OFE_MODE_TEAM : OFE_MODE_FFA;
            c.num_teams = 2;
            c.disable_alliances = mode == 2;
            OfeGame *g = ofe_game_create(m, &c, 7 + diff);
            ofe_game_step(g, 1500);
            check_invariants(g);
            ofe_game_destroy(g);
        }
    }
    ofe_map_release(m);
}

/* ---- Real map ---------------------------------------------------------------------- */

static void test_world_run(void) {
    OfeMap *m = load_real_map("world");
    if (!m) {
        printf("skip world run (set OFE_MAPS_DIR)\n");
        return;
    }
    OfeConfig c;
    ofe_config_default(&c);
    c.humans_use_ai = 1;
    c.difficulty = OFE_DIFFICULTY_HARD;
    c.event_mask = OFE_EVENTS_NO_TILES;
    OfeGame *g = ofe_game_create(m, &c, 4242);
    CHECK(alive_count(g, OFE_PLAYER_BOT) > 300);
    for (int i = 0; i < 4; i++) {
        ofe_game_step(g, 500);
        ofe_game_clear_events(g);
        check_invariants(g);
    }
    CHECK(alive_count(g, OFE_PLAYER_NATION) > 10);
    CHECK(total_tiles(g, OFE_PLAYER_NATION) > 50000);
    OfeGame *h = ofe_game_create(m, &c, 4242);
    ofe_game_step(h, 500);
    OfeGame *g2 = ofe_game_create(m, &c, 4242);
    ofe_game_step(g2, 500);
    CHECK(ofe_game_hash(h) == ofe_game_hash(g2));
    ofe_game_destroy(g);
    ofe_game_destroy(h);
    ofe_game_destroy(g2);
    ofe_map_release(m);
}

int main(void) {
    OfeMap *m = make_map(160, 120);
    test_tribes_expand_then_fight(m);
    test_tribe_accepts_alliance(m);
    test_difficulty_growth();
    test_nation_spawn_hops();
    test_nation_opening_is_immediate();
    test_alliance_answers(m);
    test_retaliation(m);
    test_determinism_and_clone();
    test_modes();
    test_world_run();
    ofe_map_release(m);
    return test_summary("test_ai");
}

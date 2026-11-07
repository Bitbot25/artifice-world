/*
 * ai_tribe.c - tribes (OpenFront's "bots"): TribeExecution + TribeSpawner.
 *
 * A tribe wakes every 40-80 ticks (its own period and phase). The first time
 * it grabs free land; afterwards it accepts every alliance, deletes captured
 * structures, hits a traitor neighbour now and then, keeps expanding into free
 * land, and once it has saved up troops (trigger ratio) retaliates against
 * whoever attacks it or picks a random non-allied neighbour. Tribes never
 * build anything and only use boats to reach free land or a neighbour across
 * a strait.
 *
 * This is the default build's approximation. The TS-parity build runs
 * ai_tribe_ts.c instead (OpenFront's TribeExecution step for step).
 */
#include "ai_common.h"

/* ---- Spawning ------------------------------------------------------------------ */

/* Tribes take random spots at least 30 tiles from every other spawn, nations
 * included (they spawned first). Own random stream so tribe placement does
 * not depend on how many other spawn attempts the game made. */
void ai_spawn_tribes(OfeGame *g) {
    uint64_t rng = rng_derive(g->seed, 0x7B1BE5);
    for (PlayerId p = 1; p < g->players.count; p++) {
        if (g->players.type[p] != OFE_PLAYER_BOT) continue;
        TileRef spot = spawn_random_tile(g, p, &rng);
        if (!spawn_player(g, p, spot)) spawn_player(g, p, -1);
    }
}

void ai_tribe_init(OfeGame *g, PlayerId p) {
    PlayerTable *pl = &g->players;
    uint64_t *rng = &pl->ai_rng[p];
    int32_t *st = pl->ai_state[p];
    int32_t rate = rng_int(rng, 40, 80);
    st[AI_OFFSET] = rng_int(rng, 0, rate);
    st[AI_RATE] = rate;
    st[AI_TRIGGER] = rng_int(rng, 50, 60);
    st[AI_RESERVE] = rng_int(rng, 30, 40);
    st[AI_EXPAND] = rng_int(rng, 10, 20);
    st[AI_FLAGS] = 0;
    /* Tribes sleep through the spawn phase; the systems start the tick after it. */
    pl->ai_next_tick[p] = ai_next_beat(OFE_MAX(g->tick, g->spawn_phase_ticks + 2), rate, st[AI_OFFSET]);
}

/* ---- Attacks ---------------------------------------------------------------------- */

/* Troops above the reserve, as TribeAttackBehavior.calculateAttackTroops. */
static bool tribe_land_attack(AiCtx *c, PlayerId target) {
    double troops = (double)c->g->players.troops[c->p] - ai_reserve_troops(c, target);
    return troops >= 1 && ai_issue_attack(c, target, troops);
}

static bool tribe_send_attack(AiCtx *c, PlayerId target) {
    OfeGame *g = c->g;
    if (target != 0) {
        if (ai_borders_by_land(c, target)) return tribe_land_attack(c, target);
        TileRef landing;
        int32_t len;
        if (!ai_boat_route(c, target, &landing, &len)) return false;
        return ai_issue_boat(c, landing, (double)g->players.troops[c->p] / 5.0);
    }
    if (c->nbi.tn_land) return tribe_land_attack(c, 0);
    TileRef landing = ai_find_free_land_landing(c);
    return landing >= 0 && ai_issue_boat(c, landing, (double)g->players.troops[c->p] / 5.0);
}

/* A traitor among our neighbours (allies too when include_allies). */
static PlayerId neighbor_traitor(AiCtx *c, bool include_allies) {
    OfeGame *g = c->g;
    if (g->cfg.disable_alliances) return 0;
    const IntVec *nb = &c->ai->main.nb;
    PlayerId found[64];
    int32_t n = 0;
    for (int32_t i = 0; i < nb->len && n < 64; i++) {
        PlayerId q = nb->data[i];
        if (!include_allies && player_friendly(g, c->p, q)) continue;
        if (player_is_traitor(g, q)) found[n++] = q;
    }
    return n ? found[rng_int(c->rng, 0, n)] : 0;
}

static void attack_random_target(AiCtx *c) {
    OfeGame *g = c->g;
    const int32_t *st = g->players.ai_state[c->p];
    /* Save up troops until the trigger ratio. */
    if (!ai_has_ratio_troops(c, st[AI_TRIGGER], false)) return;

    PlayerId attacker = ai_find_incoming_attacker(c, false);
    if (attacker && tribe_send_attack(c, attacker)) return;

    PlayerId traitor = neighbor_traitor(c, false);
    if (traitor && rng_chance(c->rng, 3) && tribe_send_attack(c, traitor)) return;

    /* A random neighbour; nations and humans are skipped half of the time. */
    const IntVec *nb = &c->ai->main.nb;
    PlayerId order[96];
    int32_t n = OFE_MIN(nb->len, 96);
    for (int32_t i = 0; i < n; i++) order[i] = nb->data[i];
    for (int32_t i = n - 1; i > 0; i--) {
        int32_t j = rng_int(c->rng, 0, i + 1);
        PlayerId t = order[i];
        order[i] = order[j];
        order[j] = t;
    }
    for (int32_t i = 0; i < n; i++) {
        PlayerId q = order[i];
        if (player_friendly(g, c->p, q)) continue;
        if (g->players.type[q] != OFE_PLAYER_BOT && rng_chance(c->rng, 2)) continue;
        if (tribe_send_attack(c, q)) return;
    }
}

/* Captured structures are worthless to a tribe: delete one at a time. */
static void delete_next_structure(AiCtx *c) {
    OfeGame *g = c->g;
    if (!ai_player_has_structures(g, c->p)) return;
    const UnitTable *u = &g->units;
    for (int32_t i = 0; i < u->count; i++) {
        if (!u->alive[i] || u->owner[i] != c->p || !unit_is_structure(u->type[i])) continue;
        if (u->flags[i] & OFE_UF_MARKED_FOR_DELETION) continue;
        OfeIntent in = {0};
        in.type = OFE_INTENT_DELETE_UNIT;
        in.player = c->p;
        in.target = unit_id_of_slot(g, i);
        game_issue(g, &in);
        return;
    }
}

static void maybe_attack(AiCtx *c) {
    OfeGame *g = c->g;
    int32_t *st = g->players.ai_state[c->p];

    PlayerId traitor = neighbor_traitor(c, true);
    if (traitor) {
        bool friendly = player_friendly(g, c->p, traitor);
        if (rng_chance(c->rng, friendly ? 6 : 3)) {
            if (player_allied(g, c->p, traitor)) {
                OfeIntent in = {0};
                in.type = OFE_INTENT_BREAK_ALLIANCE;
                in.player = c->p;
                in.target = traitor;
                game_issue(g, &in);
            }
            if (tribe_send_attack(c, traitor)) return;
        }
    }

    if (!(st[AI_FLAGS] & AIF_NO_FREE_LAND)) {
        if (c->nbi.tn_clean || c->nbi.tn_shore) {
            if (tribe_send_attack(c, 0)) return;
        } else {
            st[AI_FLAGS] |= AIF_NO_FREE_LAND;
        }
    }
    attack_random_target(c);
}

void ai_tribe_decide(OfeGame *g, PlayerId p) {
    PlayerTable *pl = &g->players;
    int32_t *st = pl->ai_state[p];
    pl->ai_next_tick[p] = g->tick + st[AI_RATE];

    AiCtx c;
    ai_ctx_begin(&c, g, p);
    ai_ctx_scan(&c);
    if (!(st[AI_FLAGS] & AIF_FIRST_DONE)) {
        st[AI_FLAGS] |= AIF_FIRST_DONE;
        tribe_send_attack(&c, 0);
        return;
    }
    ai_answer_alliance_asks(&c);
    delete_next_structure(&c);
    maybe_attack(&c);
}

/* Tribes extend every alliance. */
bool ai_tribe_alliance_extension(OfeGame *g, PlayerId tribe, PlayerId ally) {
    OfeIntent in = {0};
    in.type = OFE_INTENT_ALLIANCE_EXTENSION;
    in.player = tribe;
    in.target = ally;
    game_issue(g, &in);
    return true;
}

/*
 * stub_ai.c - placeholder for the ai subsystem so the core builds and runs
 * on its own. Delete this file when the real module lands.
 */
#include "internal.h"

/* ---- AI ----
 * Minimal stand-in: tribes and nations attack terra nullius, then their
 * weakest neighbour, now and then. */
AiSysState *ai_state_new(OfeGame *g) { OFE_UNUSED(g); return NULL; }
void ai_state_copy(OfeGame *dst, const OfeGame *src) { OFE_UNUSED(dst); OFE_UNUSED(src); }
void ai_state_free(OfeGame *g) { OFE_UNUSED(g); }
void ai_on_spawn(OfeGame *g, PlayerId p) { g->players.ai_next_tick[p] = rng_int(&g->players.ai_rng[p], 0, 40); }
void ai_on_alliance_request(OfeGame *g, PlayerId requestor, PlayerId recipient) { OFE_UNUSED(g); OFE_UNUSED(requestor); OFE_UNUSED(recipient); }
void ai_on_alliance_extension_prompt(OfeGame *g, PlayerId ai, PlayerId ally) { OFE_UNUSED(g); OFE_UNUSED(ai); OFE_UNUSED(ally); }

void ai_spawn_tribes(OfeGame *g) {
    for (PlayerId p = 1; p < g->players.count; p++) {
        if (g->players.type[p] == OFE_PLAYER_BOT) spawn_player(g, p, -1);
    }
}

void ai_tick(OfeGame *g) {
    PlayerTable *pl = &g->players;
    for (PlayerId p = 1; p < pl->count; p++) {
        int type = pl->type[p];
        if (type == OFE_PLAYER_HUMAN && !g->cfg.humans_use_ai) continue;
        if (!player_alive(g, p) || g->tick < pl->ai_next_tick[p]) continue;
        pl->ai_next_tick[p] = g->tick + 30 + rng_int(&pl->ai_rng[p], 0, 30);
        if ((double)pl->troops[p] < player_max_troops(g, p) * 0.3) continue;
        PlayerId target = 0;
        bool wild = false;
        const IntVec *b = &pl->border[p];
        int32_t weakest_troops = INT32_MAX;
        for (int32_t i = 0; i < b->len; i++) {
            TileRef nb[4];
            int n = map_neighbors4(g->map, b->data[i], nb);
            for (int k = 0; k < n; k++) {
                PlayerId o = tile_owner(g, nb[k]);
                if (o == 0 && map_is_ownable(g->map, nb[k])) wild = true;
                else if (o != 0 && o != p && !player_friendly(g, p, o) && pl->troops[o] < weakest_troops) {
                    weakest_troops = (int32_t)OFE_MIN(pl->troops[o], (int64_t)INT32_MAX);
                    target = o;
                }
            }
        }
        OfeIntent in = {0};
        in.type = OFE_INTENT_ATTACK;
        in.player = p;
        in.target = wild ? 0 : target;
        in.troops = -1;
        if (wild || target) game_issue(g, &in);
    }
}

/*
 * intent.c - the intent queue: what agents (and built-in AIs) ask for.
 *
 * Intents pushed between ticks are applied at the end of the next tick, in
 * the order pushed (see game_tick). During the spawn phase only spawn intents run; the
 * rest wait until the phase ends (like OpenFront's uninitialised
 * executions). Each intent is validated here or by the module it calls, so
 * a malformed intent is a no-op.
 */
#include "internal.h"

#define EMBARGO_ALL_COOLDOWN (10 * TICKS_PER_SECOND)
#define TARGET_COOLDOWN (15 * TICKS_PER_SECOND)

int32_t ofe_game_push_intents(OfeGame *g, const OfeIntent *intents, int32_t count) {
    int32_t accepted = 0;
    for (int32_t i = 0; i < count; i++) {
        const OfeIntent *in = &intents[i];
        if (in->type <= OFE_INTENT_NONE || in->type >= OFE_INTENT_TYPE_COUNT) continue;
        if (!player_valid(g, in->player)) continue;
        IntentVec_push(&g->pending_intents, *in);
        accepted++;
    }
    return accepted;
}

void game_issue(OfeGame *g, const OfeIntent *in) {
    /* AI actions take effect immediately: the AI runs after every other
     * system this tick, so this matches an agent intent applied at the end
     * of the tick. */
    if (player_valid(g, in->player)) intent_apply_one(g, in);
}

static bool valid_tile(const OfeGame *g, int32_t t) { return t >= 0 && t < g->map->num_tiles; }

void intent_apply_one(OfeGame *g, const OfeIntent *in) {
    PlayerId p = in->player;
    PlayerTable *pl = &g->players;
    bool alive = player_alive(g, p);

    if (in->type == OFE_INTENT_SPAWN) {
        if (g->start_tick >= 0 || !valid_tile(g, in->tile)) return;
        if (g->cfg.random_spawn && pl->type[p] == OFE_PLAYER_HUMAN) return;
        spawn_player(g, p, in->tile);
        return;
    }
    if (in->type == OFE_INTENT_MARK_DISCONNECTED) {
        uint8_t d = (uint8_t)(in->flags & 1);
        if (pl->disconnected[p] != d) {
            pl->disconnected[p] = d;
            event_emit(g, OFE_EV_PLAYER_DISCONNECTED, 0, p, d, 0, 0);
        }
        return;
    }
    if (!alive) return;

    switch (in->type) {
    case OFE_INTENT_ATTACK:
        if (in->target != 0 && !player_valid(g, in->target)) return;
        attack_start(g, p, in->target, in->troops, -1, true);
        break;
    case OFE_INTENT_CANCEL_ATTACK:
        attack_order_retreat(g, p, in->target);
        break;
    case OFE_INTENT_BOAT_ATTACK:
        if (!valid_tile(g, in->tile)) return;
        ships_launch_transport(g, p, in->tile, in->troops);
        break;
    case OFE_INTENT_CANCEL_BOAT:
        ships_retreat_transport(g, p, in->target);
        break;
    case OFE_INTENT_ALLIANCE_REQUEST:
        alliance_request(g, p, in->target);
        break;
    case OFE_INTENT_ALLIANCE_REJECT:
        if (player_valid(g, in->target)) alliance_reject(g, in->target, p);
        break;
    case OFE_INTENT_ALLIANCE_EXTENSION:
        if (player_valid(g, in->target)) alliance_request_extension(g, p, in->target);
        break;
    case OFE_INTENT_BREAK_ALLIANCE:
        if (player_valid(g, in->target)) alliance_break(g, p, in->target);
        break;
    case OFE_INTENT_TARGET_PLAYER:
        if (!player_valid(g, in->target) || in->target == p || player_friendly(g, p, in->target)) return;
        if (pl->last_target_tick[p] >= 0 && g->tick - pl->last_target_tick[p] < TARGET_COOLDOWN) return;
        pl->last_target_tick[p] = g->tick;
        pl->target[p] = in->target;
        event_emit(g, OFE_EV_TARGET, 0, p, in->target, 0, 0);
        break;
    case OFE_INTENT_DONATE_GOLD:
        if (player_valid(g, in->target)) player_donate_gold(g, p, in->target, in->troops);
        break;
    case OFE_INTENT_DONATE_TROOPS:
        if (player_valid(g, in->target)) player_donate_troops(g, p, in->target, in->troops);
        break;
    case OFE_INTENT_BUILD_UNIT:
        if (!valid_tile(g, in->tile) || in->unit_type < 0 || in->unit_type >= OFE_UNIT_TYPE_COUNT) return;
        structures_build(g, p, in->unit_type, in->tile, OFE_MAX(1, in->amount), in->flags);
        break;
    case OFE_INTENT_EMBARGO:
        if (!player_valid(g, in->target) || in->target == p) return;
        if (in->flags & 1) player_add_embargo(g, p, in->target, false);
        else player_stop_embargo(g, p, in->target);
        break;
    case OFE_INTENT_EMBARGO_ALL:
        if (pl->embargo_all_tick[p] >= 0 && g->tick - pl->embargo_all_tick[p] < EMBARGO_ALL_COOLDOWN) return;
        pl->embargo_all_tick[p] = g->tick;
        for (PlayerId o = 1; o < pl->count; o++) {
            if (o == p || !player_alive(g, o)) continue;
            if (in->flags & 1) {
                if (!player_friendly(g, p, o)) player_add_embargo(g, p, o, false);
            } else {
                player_stop_embargo(g, p, o);
            }
        }
        break;
    case OFE_INTENT_MOVE_WARSHIP:
        if (valid_tile(g, in->tile)) ships_move_warship(g, p, in->target, in->tile);
        break;
    case OFE_INTENT_UPGRADE_STRUCTURE:
        structures_upgrade(g, p, in->target, OFE_MAX(1, in->amount));
        break;
    case OFE_INTENT_DELETE_UNIT:
        structures_delete(g, p, in->target);
        break;
    default:
        break;
    }
}

void intents_apply(OfeGame *g) {
    IntentVec *q = &g->pending_intents;
    if (q->len == 0) return;
    bool spawning = g->start_tick < 0;
    int32_t kept = 0;
    /* Intents may queue more intents (none do today); iterate a stable length. */
    int32_t n = q->len;
    for (int32_t i = 0; i < n; i++) {
        OfeIntent in = q->data[i];
        if (spawning && in.type != OFE_INTENT_SPAWN && in.type != OFE_INTENT_MARK_DISCONNECTED) {
            q->data[kept++] = in;
            continue;
        }
        intent_apply_one(g, &in);
    }
    /* Anything queued during application goes after the kept intents. */
    if (q->len > n) memmove(q->data + kept, q->data + n, (size_t)(q->len - n) * sizeof(OfeIntent));
    q->len = kept + (q->len - n);
}

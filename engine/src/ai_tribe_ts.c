/*
 * ai_tribe_ts.c - OpenFront's TribeExecution and TribeAttackBehavior, step for
 * step, for the TS-parity build. Compiled only with -DOFE_TS_PARITY=1; the
 * default build keeps the cheaper tribe in ai_tribe.c.
 *
 * A tribe's execution is created when its spawn execution first ticks (right
 * after its PlayerExecution, see exec_ts.c) and runs from the end of the spawn
 * phase. Its random stream is PseudoRandom(simpleHash(player id)); the
 * constructor draws the beat (attack rate 40..79, beat 0..rate-1) and the
 * trigger / reserve / expand ratios, and every later draw (traitor pick,
 * odds, neighbour shuffle) comes from the same stream in TS's order.
 *
 * On its beat a tribe: (1) the first time, sends its troops at free land and
 * stops; (2) accepts every alliance request and every half-agreed extension,
 * (3) asks to delete one captured structure, (4) maybe betrays a traitor
 * neighbour, (5) expands into free land while it borders any, (6) once it has
 * the trigger ratio of its troop cap, retaliates against the biggest incoming
 * attack, a traitor, or a random neighbour. Attacks and deletions are queued
 * as executions (initialised at the end of the tick); alliance changes happen
 * at once, as in TS.
 *
 * Boats: the decision to cross water uses the default engine's boat helpers
 * (ships_transport_source stands for canBuildTransportShip), so games stay
 * exact only until the first transport ship is launched; the ship work is
 * ships.c's, which is made exact separately.
 */
#include "ai_common.h"
#include "tribe_ts.h"

#if OFE_TS_PARITY

void tribe_ts_init(OfeGame *g, PlayerId p, TribeTs *t) {
    memset(t, 0, sizeof(*t));
    ts_rand_init(&t->rng, (int64_t)g->players.id_hash[p]);
    t->attack_rate = (int32_t)ts_rand_int(&t->rng, 40, 80);
    t->attack_tick = (int32_t)ts_rand_int(&t->rng, 0, t->attack_rate);
    t->trigger_ratio = ts_rand_int(&t->rng, 50, 60) / 100;
    t->reserve_ratio = ts_rand_int(&t->rng, 30, 40) / 100;
    t->expand_ratio = ts_rand_int(&t->rng, 10, 20) / 100;
    t->neighbors_terra_nullius = 1;
}

/* ---- The player's surroundings ------------------------------------------------- */

static void ids_add(IntVec *v, PlayerId o) {
    for (int32_t i = 0; i < v->len; i++) {
        if (v->data[i] == o) return;
    }
    IntVec_push(v, o);
}

/* player.nearby(): owners (0 = terra nullius) of the land next to the border
 * tiles, then of the land five tiles across a strait from every tenth shore
 * border tile, in first-seen order (a JS Set). Free land with fallout does not
 * count. */
static void tribe_nearby(const OfeGame *g, PlayerId p, IntVec *out) {
    static const int dx[4] = {0, 0, -1, 1}, dy[4] = {-1, 1, 0, 0};
    const OfeMap *m = g->map;
    const TileSeq *b = &g->players.border_seq[p];
    out->len = 0;
    for (int32_t i = 0; i < b->len; i++) {
        TileRef t = b->data[i];
        if (t < 0) continue;
        TileRef nb[4];
        int n = map_neighbors4(m, t, nb);
        for (int k = 0; k < n; k++) {
            if (!map_is_ownable(m, nb[k])) continue;
            PlayerId o = tile_owner(g, nb[k]);
            if (o == 0 && tile_has_fallout(g, nb[k])) continue;
            if (o != p) ids_add(out, o);
        }
    }
    int32_t shore_idx = 0;
    for (int32_t i = 0; i < b->len; i++) {
        TileRef t = b->data[i];
        if (t < 0 || !map_is_shore(m, t) || shore_idx++ % 10 != 0) continue;
        int32_t bx = map_x(m, t), by = map_y(m, t);
        for (int d = 0; d < 4; d++) {
            int32_t x1 = bx + dx[d], y1 = by + dy[d];
            if (!map_in_bounds(m, x1, y1) || !map_is_water(m, map_ref(m, x1, y1))) continue;
            int32_t nx = bx + dx[d] * 5, ny = by + dy[d] * 5;
            if (!map_in_bounds(m, nx, ny)) continue;
            TileRef land = map_ref(m, nx, ny);
            if (!map_is_ownable(m, land)) continue;
            PlayerId o = tile_owner(g, land);
            if (o == 0 && tile_has_fallout(g, land)) continue;
            if (o != p) ids_add(out, o);
        }
    }
}

/* player.sharesBorderWith(other) for a player. */
static bool shares_border_with(const OfeGame *g, PlayerId p, PlayerId other) {
    const TileSeq *b = &g->players.border_seq[p];
    for (int32_t i = 0; i < b->len; i++) {
        if (b->data[i] >= 0 && tile_borders_player(g, b->data[i], other)) return true;
    }
    return false;
}

/* Free, passable land (fallout allowed) next to the border. */
static bool has_land_border_with_free_land(const OfeGame *g, PlayerId p) {
    const OfeMap *m = g->map;
    const TileSeq *b = &g->players.border_seq[p];
    for (int32_t i = 0; i < b->len; i++) {
        if (b->data[i] < 0) continue;
        TileRef nb[4];
        int n = map_neighbors4(m, b->data[i], nb);
        for (int k = 0; k < n; k++) {
            if (map_is_ownable(m, nb[k]) && !tile_has_owner(g, nb[k])) return true;
        }
    }
    return false;
}

/* ---- Troop budget -------------------------------------------------------------------- */

/* Troops kept home when attacking target: the expand ratio for free land and
 * for bots holding structures (we want those back fast), otherwise the
 * reserve ratio. Tribes have no starting gold, so the cap is always the full
 * maxTroops. */
static double reserve_troops(const OfeGame *g, PlayerId p, const TribeTs *t, PlayerId target) {
    bool is_player = target != 0;
    bool bot_with_structures = is_player && g->players.type[target] == OFE_PLAYER_BOT && ai_player_has_structures(g, target);
    double ratio = is_player && !bot_with_structures ? t->reserve_ratio : t->expand_ratio;
    return player_max_troops(g, p) * ratio;
}

/* ---- Boats (see the module comment) ----------------------------------------------------------- */

static bool launch_boat(OfeGame *g, PlayerId p, TileRef dst, double troops) {
    if (troops < 1) return false;
    OfeIntent in = {0};
    in.type = OFE_INTENT_BOAT_ATTACK;
    in.player = p;
    in.tile = dst;
    in.troops = troops;
    int32_t before = g->players.unit_count[p][OFE_UNIT_TRANSPORT_SHIP];
    game_issue(g, &in);
    return g->players.unit_count[p][OFE_UNIT_TRANSPORT_SHIP] > before;
}

static bool boat_attack_on_player(OfeGame *g, PlayerId p, PlayerId target) {
    AiCtx c;
    ai_ctx_begin(&c, g, p);
    ai_ctx_scan(&c);
    TileRef landing;
    int32_t len;
    if (!ai_boat_route(&c, target, &landing, &len)) return false;
    return launch_boat(g, p, landing, (double)g->players.troops[p] / 5.0);
}

/* sendBoatAttackToNearbyTerraNullius: every tenth shore border tile, free
 * land five tiles away across the water in a cardinal direction. */
static bool boat_attack_on_free_land(OfeGame *g, PlayerId p) {
    static const int dx[4] = {0, 0, -1, 1}, dy[4] = {-1, 1, 0, 0};
    const OfeMap *m = g->map;
    if (g->cfg.disabled_units & (1u << OFE_UNIT_TRANSPORT_SHIP)) return false;
    if (g->players.unit_count[p][OFE_UNIT_TRANSPORT_SHIP] >= MAX_TRANSPORT_SHIPS) return false;
    const TileSeq *b = &g->players.border_seq[p];
    int32_t shore_idx = 0;
    for (int32_t i = 0; i < b->len; i++) {
        TileRef t = b->data[i];
        if (t < 0 || !map_is_shore(m, t) || shore_idx++ % 10 != 0) continue;
        int32_t bx = map_x(m, t), by = map_y(m, t);
        for (int d = 0; d < 4; d++) {
            int32_t x1 = bx + dx[d], y1 = by + dy[d];
            if (!map_in_bounds(m, x1, y1) || !map_is_water(m, map_ref(m, x1, y1))) continue;
            int32_t nx = bx + dx[d] * 5, ny = by + dy[d] * 5;
            if (!map_in_bounds(m, nx, ny)) continue;
            TileRef land = map_ref(m, nx, ny);
            if (!map_is_ownable(m, land) || tile_has_owner(g, land) || tile_has_fallout(g, land)) continue;
            if (ships_transport_source(g, p, land) < 0) continue;
            return launch_boat(g, p, land, (double)g->players.troops[p] / 5.0);
        }
    }
    return false;
}

/* ---- Attacks ------------------------------------------------------------------------------------ */

/* sendLandAttack: everything above the reserve, as a new attack execution. */
static bool send_land_attack(OfeGame *g, PlayerId p, const TribeTs *t, PlayerId target) {
    double troops = (double)g->players.troops[p] - reserve_troops(g, p, t, target);
    if (troops < 1) return false;
    ts_exec_add_attack(g, p, target, troops);
    return true;
}

/* sendAttack: a land attack if we border the target (free land: if free land
 * touches our border), else a boat. Tribes never refuse a target. */
static bool send_attack(OfeGame *g, PlayerId p, const TribeTs *t, PlayerId target) {
    if (target != 0) {
        return shares_border_with(g, p, target) ? send_land_attack(g, p, t, target) : boat_attack_on_player(g, p, target);
    }
    return has_land_border_with_free_land(g, p) ? send_land_attack(g, p, t, 0) : boat_attack_on_free_land(g, p);
}

/* findIncomingAttackPlayer: the attacker with the single largest attack on us
 * (the earliest one on ties), friends excluded. */
static PlayerId find_incoming_attacker(const OfeGame *g, PlayerId p) {
    const AttackTable *a = &g->attacks;
    IntVec order = {0};
    attacks_in_creation_order(g, &order);
    PlayerId best = 0;
    double largest = 0;
    for (int32_t k = 0; k < order.len; k++) {
        int32_t i = order.data[k];
        if (a->target[i] != p || player_friendly(g, p, a->attacker[i]) || a->troops[i] <= largest) continue;
        largest = a->troops[i];
        best = a->attacker[i];
    }
    IntVec_free(&order);
    return best;
}

/* getNeighborTraitorToAttack: a random traitor among the neighbours (allies
 * too when include_allies). Draws even for a single candidate. */
static PlayerId neighbor_traitor(OfeGame *g, PlayerId p, TribeTs *t, bool include_allies) {
    if (g->cfg.disable_alliances) return 0;
    IntVec near = {0};
    tribe_nearby(g, p, &near);
    int32_t found[MAX_PLAYERS];
    int32_t n = 0;
    for (int32_t i = 0; i < near.len; i++) {
        PlayerId q = near.data[i];
        if (q != 0 && (include_allies || !player_friendly(g, p, q)) && player_is_traitor(g, q)) found[n++] = q;
    }
    IntVec_free(&near);
    return n ? found[ts_rand_index(&t->rng, n)] : 0;
}

/* attackRandomTarget */
static void attack_random_target(OfeGame *g, PlayerId p, TribeTs *t) {
    /* Save up troops until the trigger ratio. */
    if ((double)g->players.troops[p] / player_max_troops(g, p) < t->trigger_ratio) return;

    PlayerId incoming = find_incoming_attacker(g, p);
    if (incoming && send_attack(g, p, t, incoming)) return;

    PlayerId traitor = neighbor_traitor(g, p, t, false);
    if (traitor && ts_rand_chance(&t->rng, 3) && send_attack(g, p, t, traitor)) return;

    /* A random neighbour (the terra nullius entry takes part in the shuffle);
     * nations and humans are skipped half of the time. */
    IntVec near = {0};
    tribe_nearby(g, p, &near);
    ts_rand_shuffle(&t->rng, near.data, near.len);
    for (int32_t i = 0; i < near.len; i++) {
        PlayerId q = near.data[i];
        if (q == 0 || player_friendly(g, p, q)) continue;
        int type = g->players.type[q];
        if ((type == OFE_PLAYER_NATION || type == OFE_PLAYER_HUMAN) && ts_rand_chance(&t->rng, 2)) continue;
        if (send_attack(g, p, t, q)) break;
    }
    IntVec_free(&near);
}

/* ---- Alliances and structures ---------------------------------------------------------------------- */

/* acceptAllAllianceRequests: accept every request (directly, not through an
 * AllianceRequestExecution: no relation or embargo changes), and answer every
 * extension the other side already asked for with an extension execution. */
static void accept_alliances(OfeGame *g, PlayerId p) {
    PlayerId requestors[MAX_PLAYERS];
    int32_t n = 0;
    for (int32_t i = 0; i < g->alliance_requests.len && n < MAX_PLAYERS; i++) {
        if (g->alliance_requests.data[i].recipient == p) requestors[n++] = g->alliance_requests.data[i].requestor;
    }
    for (int32_t i = 0; i < n; i++) alliance_form(g, requestors[i], p, 0);

    for (int32_t i = 0; i < g->alliances.len; i++) {
        const Alliance *al = &g->alliances.data[i];
        if ((al->a != p && al->b != p) || al->extend_a == al->extend_b) continue;
        OfeIntent in = {0};
        in.type = OFE_INTENT_ALLIANCE_EXTENSION;
        in.player = p;
        in.target = al->a == p ? al->b : al->a;
        ts_exec_add_intent(g, &in);
    }
}

/* deleteNextStructure: captured structures are worthless to a tribe. */
static void delete_next_structure(OfeGame *g, PlayerId p) {
    if (!ai_player_has_structures(g, p)) return;
    const UnitTable *u = &g->units;
    for (int32_t i = 0; i < u->count; i++) {
        if (!u->alive[i] || u->owner[i] != p || !unit_is_structure(u->type[i])) continue;
        if (u->flags[i] & OFE_UF_MARKED_FOR_DELETION) continue;
        OfeIntent in = {0};
        in.type = OFE_INTENT_DELETE_UNIT;
        in.player = p;
        in.target = unit_id_of_slot(g, i);
        ts_exec_add_intent(g, &in);
        return;
    }
}

/* ---- The beat ------------------------------------------------------------------------------------------ */

static void maybe_attack(OfeGame *g, PlayerId p, TribeTs *t) {
    PlayerId traitor = neighbor_traitor(g, p, t, true);
    if (traitor) {
        bool friendly = player_friendly(g, p, traitor);
        if (ts_rand_chance(&t->rng, friendly ? 6 : 3)) {
            if (player_allied(g, p, traitor)) alliance_break(g, p, traitor);
            if (send_attack(g, p, t, traitor)) return;
        }
    }

    if (t->neighbors_terra_nullius) {
        IntVec near = {0};
        tribe_nearby(g, p, &near);
        bool free_land_near = false;
        for (int32_t i = 0; i < near.len; i++) free_land_near |= near.data[i] == 0;
        IntVec_free(&near);
        if (free_land_near) {
            if (send_attack(g, p, t, 0)) return;
        } else {
            t->neighbors_terra_nullius = 0;
        }
    }
    attack_random_target(g, p, t);
}

bool tribe_ts_tick(OfeGame *g, PlayerId p, TribeTs *t) {
    if (g->tick % t->attack_rate != t->attack_tick) return true;
    if (!player_alive(g, p)) return false;
    if (!t->has_behaviour) {
        t->has_behaviour = 1;
        send_attack(g, p, t, 0); /* the opening land grab */
        return true;
    }
    accept_alliances(g, p);
    delete_next_structure(g, p);
    maybe_attack(g, p, t);
    return true;
}

#endif /* OFE_TS_PARITY */

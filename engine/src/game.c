/*
 * game.c - game lifecycle (create, clone, destroy), the tick loop, batched
 * stepping and read-only queries.
 */
#include "internal.h"

#ifdef _OPENMP
#include <omp.h>
#endif

/* ======================================================================== */
/* Tables                                                                    */
/* ======================================================================== */

static void player_table_alloc(PlayerTable *t, int32_t cap) {
    memset(t, 0, sizeof(*t));
    t->cap = cap;
    PLAYER_COLUMNS(ALLOC_COLUMN)
    t->border = (IntVec *)ofe_xcalloc((size_t)cap, sizeof(IntVec));
    t->name = (char(*)[48])ofe_xcalloc((size_t)cap, 48);
}

static void player_table_free(PlayerTable *t) {
    PLAYER_COLUMNS(FREE_COLUMN)
    for (int32_t i = 0; i < t->cap; i++) IntVec_free(&t->border[i]);
    free(t->border);
    free(t->name);
    memset(t, 0, sizeof(*t));
}

static void player_table_copy(PlayerTable *dst, const PlayerTable *src) {
    if (dst->cap != src->cap) {
        player_table_free(dst);
        player_table_alloc(dst, src->cap);
    }
    dst->count = src->count;
    PLAYER_COLUMNS(COPY_COLUMN)
    for (int32_t i = 0; i < src->cap; i++) IntVec_copy(&dst->border[i], &src->border[i]);
    memcpy(dst->name, src->name, (size_t)src->cap * 48);
}

static void attack_table_free(AttackTable *t) {
    ATTACK_COLUMNS(FREE_COLUMN)
    for (int32_t i = 0; i < t->cap; i++) {
        heap_free(&t->to_conquer[i]);
        intset_free(&t->front[i]);
    }
    free(t->to_conquer);
    free(t->front);
    memset(t, 0, sizeof(*t));
}

static void attack_table_copy(AttackTable *dst, const AttackTable *src) {
    if (dst->cap < src->cap) {
        AttackTable *t = dst;
        int32_t cap = src->cap;
        ATTACK_COLUMNS(GROW_COLUMN)
        t->to_conquer = (TileHeap *)ofe_xrealloc(t->to_conquer, (size_t)cap * sizeof(TileHeap));
        t->front = (IntSet *)ofe_xrealloc(t->front, (size_t)cap * sizeof(IntSet));
        memset(t->to_conquer + t->cap, 0, (size_t)(cap - t->cap) * sizeof(TileHeap));
        memset(t->front + t->cap, 0, (size_t)(cap - t->cap) * sizeof(IntSet));
        t->cap = cap;
    }
    dst->count = src->count;
    {
        /* COPY_COLUMN copies src->cap rows; dst->cap >= src->cap here. */
        ATTACK_COLUMNS(COPY_COLUMN)
    }
    for (int32_t i = 0; i < src->count; i++) {
        if (src->active[i]) {
            heap_copy(&dst->to_conquer[i], &src->to_conquer[i]);
            intset_copy(&dst->front[i], &src->front[i]);
        } else {
            dst->to_conquer[i].len = 0;
            intset_clear(&dst->front[i]);
        }
    }
    for (int32_t i = src->count; i < dst->cap; i++) {
        dst->active[i] = 0;
        dst->to_conquer[i].len = 0;
        intset_clear(&dst->front[i]);
    }
}

/* ======================================================================== */
/* Create / destroy                                                          */
/* ======================================================================== */

void ofe_config_default(OfeConfig *c) {
    memset(c, 0, sizeof(*c));
    c->difficulty = OFE_DIFFICULTY_MEDIUM;
    c->game_mode = OFE_MODE_FFA;
    c->num_teams = 2;
    c->num_humans = 1;
    c->num_nations = -1;
    c->num_bots = 400;
    c->spawn_phase_ticks = -1;
    c->auto_spawn_humans = 1;
    c->donate_gold = 1;
    c->donate_troops = 1;
    c->overtime_start_minutes = 30;
    c->doomsday_speed = 1;
    c->spawn_immunity_ticks = -1;
    c->gold_multiplier = 1.0;
    c->event_mask = OFE_EVENTS_ALL;
}

static void resolve_config(OfeConfig *c) {
    if (c->spawn_immunity_ticks < 0) c->spawn_immunity_ticks = 50;
    if (c->gold_multiplier <= 0) c->gold_multiplier = 1.0;
    if (c->overtime_start_minutes <= 0) c->overtime_start_minutes = 30;
    if (c->num_teams < 2) c->num_teams = 2;
    c->num_humans = OFE_MAX(0, c->num_humans);
    c->num_bots = OFE_MAX(0, c->num_bots);
}

static int64_t start_troops(const OfeGame *g, int type) {
    if (type == OFE_PLAYER_BOT) return 10000;
    if (type == OFE_PLAYER_NATION) {
        static const int64_t by_difficulty[4] = {12500, 18750, 25000, 31250};
        return by_difficulty[OFE_CLAMP(g->cfg.difficulty, 0, 3)];
    }
    return g->cfg.infinite_troops ? 1000000 : 25000;
}

static PlayerId add_player(OfeGame *g, int type, const char *name, int16_t team, int16_t nation_index) {
    PlayerTable *pl = &g->players;
    PlayerId p = pl->count++;
    pl->type[p] = (uint8_t)type;
    pl->team[p] = team;
    pl->nation_index[p] = nation_index;
    pl->relation_row[p] = -1;
    pl->spawn_tile[p] = -1;
    pl->traitor_tick[p] = -1;
    pl->embargo_all_tick[p] = -1;
    pl->last_donate_tick[p] = -1;
    pl->last_target_tick[p] = -1;
    pl->doomsday_tick[p] = -1;
    pl->troops[p] = start_troops(g, type);
    pl->gold[p] = type == OFE_PLAYER_BOT ? 0 : g->cfg.starting_gold;
    pl->ai_rng[p] = rng_derive(g->seed, 0xA1000000u + (uint64_t)p);
    snprintf(pl->name[p], 48, "%s", name);
    return p;
}

OfeGame *ofe_game_create(OfeMap *map, const OfeConfig *config, uint64_t seed) {
    if (!map) return NULL;
    OfeGame *g = (OfeGame *)ofe_xcalloc(1, sizeof(OfeGame));
    ofe_map_retain(map);
    g->map = map;
    if (config) g->cfg = *config;
    else ofe_config_default(&g->cfg);
    resolve_config(&g->cfg);
    g->seed = seed;
    g->rng = rng_derive(seed, 0x5EED);
    g->start_tick = -1;
    g->winning_team = -1;
    g->num_land_tiles = map->num_land_tiles;
    g->spawn_phase_ticks = g->cfg.spawn_phase_ticks >= 0 ? g->cfg.spawn_phase_ticks : (g->cfg.random_spawn ? 150 : 200);

    int32_t num_nations = g->cfg.num_nations < 0 ? map->num_nations : OFE_MIN(g->cfg.num_nations, map->num_nations);
    int32_t total = 1 + g->cfg.num_humans + num_nations + g->cfg.num_bots;
    if (total > MAX_PLAYERS) {
        fprintf(stderr, "ofe: too many players (%d > %d)\n", total, MAX_PLAYERS);
        total = MAX_PLAYERS;
    }
    player_table_alloc(&g->players, total);
    g->players.count = 1; /* row 0 = terra nullius */
    tiles_init(&g->tiles, map->num_tiles);
    units_table_init(&g->units);

    /* Players: humans, then nations, then tribes. Team games deal humans and
     * nations round-robin into teams; tribes are teamless. */
    bool teams = g->cfg.game_mode == OFE_MODE_TEAM;
    int32_t dealt = 0;
    char name[32];
    for (int32_t i = 0; i < g->cfg.num_humans && g->players.count < total; i++) {
        snprintf(name, sizeof(name), "Agent %d", i + 1);
        add_player(g, OFE_PLAYER_HUMAN, name, teams ? (int16_t)(dealt++ % g->cfg.num_teams) : -1, -1);
    }
    for (int32_t i = 0; i < num_nations && g->players.count < total; i++) {
        add_player(g, OFE_PLAYER_NATION, map->nations[i].name, teams ? (int16_t)(dealt++ % g->cfg.num_teams) : -1, (int16_t)i);
    }
    for (int32_t i = 0; i < g->cfg.num_bots && g->players.count < total; i++) {
        snprintf(name, sizeof(name), "Tribe %d", i + 1);
        add_player(g, OFE_PLAYER_BOT, name, -1, -1);
    }

    /* Relation rows for AI-driven players. */
    int32_t rows = 0;
    for (PlayerId p = 1; p < g->players.count; p++) {
        int type = g->players.type[p];
        if (type == OFE_PLAYER_NATION || (type == OFE_PLAYER_HUMAN && g->cfg.humans_use_ai)) g->players.relation_row[p] = rows++;
    }
    g->num_relation_rows = rows;
    g->relations = (float *)ofe_xcalloc((size_t)OFE_MAX(rows, 1) * g->players.cap, sizeof(float));

    g->sub = (Subsystems *)ofe_xcalloc(1, sizeof(Subsystems));
    g->sub->structures = structures_state_new(g);
    g->sub->ships = ships_state_new(g);
    g->sub->nukes = nukes_state_new(g);
    g->sub->rail = rail_state_new(g);
    g->sub->doomsday = doomsday_state_new(g);
    g->sub->ai = ai_state_new(g);

    /* Nations spawn on their map spots right away; tribes spread out after. */
    for (PlayerId p = 1; p < g->players.count; p++) {
        if (g->players.type[p] != OFE_PLAYER_NATION) continue;
        spawn_player(g, p, map->nations[g->players.nation_index[p]].tile);
    }
    ai_spawn_tribes(g);
    if (g->spawn_phase_ticks == 0) spawn_phase_end(g);
    return g;
}

void ofe_game_destroy(OfeGame *g) {
    if (!g) return;
    if (g->sub) {
        structures_state_free(g);
        ships_state_free(g);
        nukes_state_free(g);
        rail_state_free(g);
        doomsday_state_free(g);
        ai_state_free(g);
        free(g->sub);
    }
    tiles_free(&g->tiles);
    player_table_free(&g->players);
    attack_table_free(&g->attacks);
    units_table_free(&g->units);
    AllianceVec_free(&g->alliances);
    AllianceRequestVec_free(&g->alliance_requests);
    AllianceRequestVec_free(&g->past_requests);
    EmbargoVec_free(&g->embargoes);
    IntentVec_free(&g->pending_intents);
    EventVec_free(&g->events);
    IntVec_free(&g->scratch_a);
    IntVec_free(&g->scratch_b);
    free(g->relations);
    free(g->visit_stamp);
    ofe_map_release(g->map);
    free(g);
}

/* ======================================================================== */
/* Clone                                                                     */
/* ======================================================================== */

void ofe_game_copy_into(OfeGame *dst, const OfeGame *src) {
    if (dst == src) return;
    if (dst->map != src->map) {
        ofe_map_retain(src->map);
        ofe_map_release(dst->map);
        dst->map = src->map;
    }
    dst->cfg = src->cfg;
    dst->seed = src->seed;
    dst->rng = src->rng;
    dst->tick = src->tick;
    dst->spawn_phase_ticks = src->spawn_phase_ticks;
    dst->start_tick = src->start_tick;
    dst->winner = src->winner;
    dst->winning_team = src->winning_team;
    dst->game_over = src->game_over;
    dst->num_land_tiles = src->num_land_tiles;
    dst->num_fallout_tiles = src->num_fallout_tiles;
    dst->next_attack_id = src->next_attack_id;
    dst->mirvs_launched = src->mirvs_launched;

    tiles_share(&dst->tiles, &src->tiles);
    player_table_copy(&dst->players, &src->players);
    attack_table_copy(&dst->attacks, &src->attacks);
    units_table_copy(&dst->units, &src->units);
    AllianceVec_copy(&dst->alliances, &src->alliances);
    AllianceRequestVec_copy(&dst->alliance_requests, &src->alliance_requests);
    AllianceRequestVec_copy(&dst->past_requests, &src->past_requests);
    EmbargoVec_copy(&dst->embargoes, &src->embargoes);
    IntentVec_copy(&dst->pending_intents, &src->pending_intents);
    EventVec_copy(&dst->events, &src->events);

    size_t rel = (size_t)OFE_MAX(src->num_relation_rows, 1) * src->players.cap;
    dst->relations = (float *)ofe_xrealloc(dst->relations, rel * sizeof(float));
    memcpy(dst->relations, src->relations, rel * sizeof(float));
    dst->num_relation_rows = src->num_relation_rows;

    structures_state_copy(dst, src);
    ships_state_copy(dst, src);
    nukes_state_copy(dst, src);
    rail_state_copy(dst, src);
    doomsday_state_copy(dst, src);
    ai_state_copy(dst, src);
}

OfeGame *ofe_game_clone(const OfeGame *src) {
    OfeGame *g = (OfeGame *)ofe_xcalloc(1, sizeof(OfeGame));
    ofe_map_retain(src->map);
    g->map = src->map;
    units_table_init(&g->units);
    g->sub = (Subsystems *)ofe_xcalloc(1, sizeof(Subsystems));
    ofe_game_copy_into(g, src);
    return g;
}

/* ======================================================================== */
/* Tick loop                                                                 */
/* ======================================================================== */

/* One tick, in OpenFront's order: the spawn timer, then every running
 * system, then new actions. Intents (and AI actions) take effect at the end
 * of the tick, so a new attack first conquers on the next tick and troop
 * growth happens before an attack's troops are taken, as in OpenFront,
 * where executions created during a tick are initialised after it. */
static void game_tick(OfeGame *g) {
    if (g->start_tick < 0 && g->tick > g->spawn_phase_ticks) spawn_phase_end(g);
    if (g->start_tick >= 0) {
        players_tick(g); /* growth, alliances, enclaves, deaths */
        attacks_tick(g); /* land fronts */
        units_tick(g); /* structures, ships, nukes, rail */
        doomsday_tick(g);
        ai_tick(g); /* nations and tribes act through game_issue */
        win_check(g);
    }
    intents_apply(g);
    g->tick++;
}

int32_t ofe_game_step(OfeGame *g, int32_t ticks) {
    int32_t ran = 0;
    while (ran < ticks && !g->game_over) {
        game_tick(g);
        ran++;
    }
    return ran;
}

void ofe_step_batch(OfeGame **games, int32_t count, int32_t ticks, int32_t num_threads) {
#ifdef _OPENMP
    if (num_threads <= 0) num_threads = omp_get_max_threads();
#pragma omp parallel for schedule(dynamic, 1) num_threads(num_threads)
    for (int32_t i = 0; i < count; i++) ofe_game_step(games[i], ticks);
#else
    OFE_UNUSED(num_threads);
    for (int32_t i = 0; i < count; i++) ofe_game_step(games[i], ticks);
#endif
}

/* ======================================================================== */
/* Queries                                                                   */
/* ======================================================================== */

uint64_t hash_bytes(uint64_t h, const void *data, size_t n) {
    const uint8_t *p = (const uint8_t *)data;
    while (n >= 8) {
        uint64_t x;
        memcpy(&x, p, 8);
        h = hash_mix(h, x);
        p += 8;
        n -= 8;
    }
    uint64_t tail = 0;
    memcpy(&tail, p, n);
    return hash_mix(h, tail ^ ((uint64_t)n << 56));
}

uint64_t ofe_game_hash(const OfeGame *g) {
    uint64_t h = 0x0FE0FE0FE0FE0FEull;
    h = hash_mix(h, (uint64_t)g->tick);
    for (int32_t c = 0; c < g->tiles.num_chunks; c++) {
        int32_t n = OFE_MIN(TILE_CHUNK_SIZE, g->map->num_tiles - c * TILE_CHUNK_SIZE);
        h = hash_bytes(h, g->tiles.chunks[c]->state, (size_t)n * sizeof(uint16_t));
    }
    const PlayerTable *pt = &g->players;
    h = hash_bytes(h, pt->troops, (size_t)pt->count * sizeof(int64_t));
    h = hash_bytes(h, pt->gold, (size_t)pt->count * sizeof(int64_t));
    h = hash_bytes(h, pt->num_tiles, (size_t)pt->count * sizeof(int32_t));
    const UnitTable *u = &g->units;
    for (int32_t i = 0; i < u->count; i++) {
        if (!u->alive[i]) continue;
        h = hash_mix(h, ((uint64_t)u->type[i] << 32) | (uint32_t)u->owner[i]);
        h = hash_mix(h, ((uint64_t)(uint32_t)u->tile[i] << 32) | (uint32_t)u->health[i]);
        h = hash_mix(h, (uint64_t)(int64_t)u->troops[i]);
    }
    const AttackTable *a = &g->attacks;
    for (int32_t i = 0; i < a->count; i++) {
        if (!a->active[i]) continue;
        h = hash_mix(h, ((uint64_t)a->attacker[i] << 32) | (uint32_t)a->target[i]);
        h = hash_mix(h, (uint64_t)(int64_t)a->troops[i]);
    }
    for (int32_t i = 0; i < g->alliances.len; i++) {
        h = hash_mix(h, ((uint64_t)g->alliances.data[i].a << 32) | (uint32_t)g->alliances.data[i].b);
    }
    return h;
}

const OfeMap *ofe_game_map(const OfeGame *g) { return g->map; }
int32_t ofe_game_tick(const OfeGame *g) { return g->tick; }
int32_t ofe_game_in_spawn_phase(const OfeGame *g) { return g->start_tick < 0; }
int32_t ofe_game_is_over(const OfeGame *g) { return g->game_over; }
int32_t ofe_game_winner(const OfeGame *g) { return g->winner; }
int32_t ofe_game_winning_team(const OfeGame *g) { return g->winning_team; }
int32_t ofe_game_num_players(const OfeGame *g) { return g->players.count - 1; }
int32_t ofe_game_num_land_tiles(const OfeGame *g) { return g->num_land_tiles; }
int32_t ofe_game_num_fallout_tiles(const OfeGame *g) { return g->num_fallout_tiles; }
int32_t ofe_game_win_threshold_tenths(const OfeGame *g) { return win_threshold_tenths(g); }

/* Per-player sums over the unit and attack tables, gathered in one pass so
 * that querying every player stays O(players + units + attacks). */
typedef struct PlayerSums {
    double boat_troops, attack_troops;
    int32_t attacks;
} PlayerSums;

static void gather_sums(const OfeGame *g, PlayerId only, PlayerSums *sums) {
    const UnitTable *u = &g->units;
    for (int32_t i = 0; i < u->count; i++) {
        if (!u->alive[i] || u->type[i] != OFE_UNIT_TRANSPORT_SHIP) continue;
        PlayerId o = u->owner[i];
        if (only == 0 || o == only) sums[only ? 0 : o].boat_troops += u->troops[i];
    }
    const AttackTable *a = &g->attacks;
    for (int32_t i = 0; i < a->count; i++) {
        if (!a->active[i]) continue;
        PlayerId o = a->attacker[i];
        if (only != 0 && o != only) continue;
        sums[only ? 0 : o].attack_troops += a->troops[i];
        sums[only ? 0 : o].attacks++;
    }
}

static void fill_player_info(const OfeGame *g, PlayerId p, const PlayerSums *sum, OfePlayerInfo *out) {
    const PlayerTable *pl = &g->players;
    memset(out, 0, sizeof(*out));
    out->id = p;
    out->type = pl->type[p];
    out->team = pl->team[p];
    bool alive = player_alive(g, p);
    uint32_t f = 0;
    if (alive) f |= OFE_PF_ALIVE;
    if (pl->spawned[p]) f |= OFE_PF_SPAWNED;
    if (pl->disconnected[p]) f |= OFE_PF_DISCONNECTED;
    if (player_is_traitor(g, p)) f |= OFE_PF_TRAITOR;
    if (pl->doomsday_tick[p] >= 0 && alive) f |= OFE_PF_IN_DOOMSDAY;
    if (pl->type[p] != OFE_PLAYER_BOT && player_is_immune(g, p)) f |= OFE_PF_IMMUNE;
    out->flags = f;
    out->num_tiles = pl->num_tiles[p];
    out->num_border_tiles = pl->border[p].len;
    out->spawn_tile = pl->spawn_tile[p];
    out->num_outgoing_attacks = sum->attacks;
    out->troops = pl->troops[p];
    out->gold = pl->gold[p];
    out->max_troops = player_max_troops(g, p);
    out->troop_rate = pl->spawned[p] && alive ? player_troop_increase(g, p) : 0.0;
    out->outgoing_attack_troops = sum->attack_troops;
    out->boat_troops = sum->boat_troops;
    memcpy(out->units, pl->unit_count[p], sizeof(out->units));
    memcpy(out->unit_levels, pl->unit_levels[p], sizeof(out->unit_levels));
    out->traitor_ticks_left = player_is_traitor(g, p) ? 300 - (g->tick - pl->traitor_tick[p]) : 0;
    out->doomsday_ticks = pl->doomsday_tick[p] >= 0 ? g->tick - pl->doomsday_tick[p] : 0;
}

int32_t ofe_game_player_info(const OfeGame *g, int32_t p, OfePlayerInfo *out) {
    if (!player_valid(g, p)) return -1;
    PlayerSums sum = {0, 0, 0};
    gather_sums(g, p, &sum);
    fill_player_info(g, p, &sum, out);
    return 0;
}

int32_t ofe_game_players(const OfeGame *g, OfePlayerInfo *out, int32_t cap) {
    int32_t n = OFE_MIN(cap, g->players.count - 1);
    PlayerSums *sums = (PlayerSums *)ofe_xcalloc((size_t)g->players.count, sizeof(PlayerSums));
    gather_sums(g, 0, sums);
    for (int32_t i = 0; i < n; i++) fill_player_info(g, i + 1, &sums[i + 1], &out[i]);
    free(sums);
    return n;
}

void ofe_game_tile_state(const OfeGame *g, uint16_t *out) {
    for (int32_t c = 0; c < g->tiles.num_chunks; c++) {
        int32_t n = OFE_MIN(TILE_CHUNK_SIZE, g->map->num_tiles - c * TILE_CHUNK_SIZE);
        memcpy(out + (size_t)c * TILE_CHUNK_SIZE, g->tiles.chunks[c]->state, (size_t)n * sizeof(uint16_t));
    }
}

void ofe_game_owner_map(const OfeGame *g, uint16_t *out) {
    ofe_game_tile_state(g, out);
    for (int32_t t = 0; t < g->map->num_tiles; t++) out[t] &= TS_OWNER_MASK;
}

int32_t ofe_game_tile_owner(const OfeGame *g, int32_t tile) {
    if (tile < 0 || tile >= g->map->num_tiles) return -1;
    return tile_owner(g, tile);
}

int32_t ofe_game_attacks(const OfeGame *g, OfeAttackInfo *out, int32_t cap) {
    const AttackTable *t = &g->attacks;
    int32_t n = 0;
    for (int32_t i = 0; i < t->count && n < cap; i++) {
        if (!t->active[i]) continue;
        OfeAttackInfo *o = &out[n++];
        o->id = t->id[i];
        o->attacker = t->attacker[i];
        o->target = t->target[i];
        o->source_tile = t->source_tile[i];
        o->border_size = t->front[i].len;
        o->retreating = t->retreat_at[i] >= 0;
        o->troops = t->troops[i];
    }
    return n;
}

int32_t ofe_game_alliances(const OfeGame *g, OfeAllianceInfo *out, int32_t cap) {
    int32_t n = OFE_MIN(cap, g->alliances.len);
    for (int32_t i = 0; i < n; i++) {
        const Alliance *a = &g->alliances.data[i];
        out[i].a = a->a;
        out[i].b = a->b;
        out[i].created_tick = a->created_tick;
        out[i].expires_tick = a->expires_tick;
    }
    return n;
}

double ofe_game_relation(const OfeGame *g, int32_t a, int32_t b) {
    if (!player_valid(g, a) || !player_valid(g, b)) return 0;
    return player_relation(g, a, b);
}

int32_t ofe_game_is_embargoed(const OfeGame *g, int32_t a, int32_t b) {
    if (!player_valid(g, a) || !player_valid(g, b)) return 0;
    return player_embargoed(g, a, b);
}

int32_t ofe_game_player_name(const OfeGame *g, int32_t p, char *out, int32_t cap) {
    if (!player_valid(g, p) || cap <= 0) return -1;
    snprintf(out, (size_t)cap, "%s", g->players.name[p]);
    return 0;
}

int64_t ofe_game_unit_cost(const OfeGame *g, int32_t p, int32_t type) {
    if (!player_valid(g, p) || type < 0 || type >= OFE_UNIT_TYPE_COUNT) return -1;
    return unit_cost(g, p, type);
}

int32_t ofe_game_can_build(const OfeGame *g, int32_t p, int32_t type, int32_t tile) {
    if (!player_valid(g, p) || type < 0 || type >= OFE_UNIT_TYPE_COUNT) return 0;
    if (tile < 0 || tile >= g->map->num_tiles) return 0;
    return structures_can_build(g, p, type, tile) >= 0;
}

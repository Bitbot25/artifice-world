/*
 * ai_common.c - the AI scheduler and the pieces nations and tribes share.
 *
 * OpenFront's AiAttackBehavior (the base of both TribeAttackBehavior and
 * NationAttackBehavior) is split here into: a per-decision context (border
 * scan = player.nearby(), attack summary = incomingAttacks/outgoingAttacks),
 * troop budgeting (reserve / expand ratios), and boat helpers.
 *
 * Everything that touches many tiles or many players is done once per
 * decision (every 30-100 ticks per AI) from border lists and the attack
 * table; nothing scans the whole map.
 */
#include "ai_common.h"

#define BOX_REFRESH_TICKS 100
#define LEADER_REFRESH_TICKS 30
#define HIGH_STARTING_GOLD 3000000

/* ---- State ------------------------------------------------------------------ */

static void ai_alloc_arrays(AiSysState *ai, int32_t cap) {
    ai->cap = cap;
    AiScan *scans[2] = {&ai->main, &ai->side};
    for (int i = 0; i < 2; i++) {
        scans[i]->stamp = (uint32_t *)ofe_xcalloc((size_t)cap, sizeof(uint32_t));
        scans[i]->flag = (uint8_t *)ofe_xcalloc((size_t)cap, 1);
    }
    ai->box = (AiBox *)ofe_xcalloc((size_t)cap, sizeof(AiBox));
    ai->box_tick = (int32_t *)ofe_xmalloc((size_t)cap * sizeof(int32_t));
    for (int32_t i = 0; i < cap; i++) ai->box_tick[i] = INT32_MIN / 2;
    ai->leader_tick = ai->top_tick = INT32_MIN / 2;
}

static void ai_free_arrays(AiSysState *ai) {
    AiScan *scans[2] = {&ai->main, &ai->side};
    for (int i = 0; i < 2; i++) {
        free(scans[i]->stamp);
        free(scans[i]->flag);
        IntVec_free(&scans[i]->nb);
    }
    free(ai->box);
    free(ai->box_tick);
    IntVec_free(&ai->tmp);
}

AiSysState *ai_state_new(OfeGame *g) {
    AiSysState *ai = (AiSysState *)ofe_xcalloc(1, sizeof(AiSysState));
    ai_alloc_arrays(ai, g->players.cap);
    return ai;
}

void ai_state_copy(OfeGame *dst, const OfeGame *src) {
    AiSysState *s = src->sub->ai;
    AiSysState *d = dst->sub->ai;
    if (!s) return;
    if (!d) {
        d = (AiSysState *)ofe_xcalloc(1, sizeof(AiSysState));
        dst->sub->ai = d;
    }
    if (d->cap != s->cap) {
        if (d->cap) ai_free_arrays(d);
        ai_alloc_arrays(d, s->cap);
    }
    memcpy(d->box, s->box, (size_t)s->cap * sizeof(AiBox));
    memcpy(d->box_tick, s->box_tick, (size_t)s->cap * sizeof(int32_t));
    d->leader_tick = s->leader_tick;
    d->leader = s->leader;
    d->top_tick = s->top_tick;
    d->top_player = s->top_player;
    d->top_tiles = s->top_tiles;
    d->seen_attack_id = s->seen_attack_id;
    memcpy(d->mirv_tick, s->mirv_tick, sizeof(d->mirv_tick));
    memcpy(d->mirv_who, s->mirv_who, sizeof(d->mirv_who));
    d->mirv_pos = s->mirv_pos;
    /* Scan scratch is rebuilt on every scan. */
}

void ai_state_free(OfeGame *g) {
    AiSysState *ai = g->sub->ai;
    if (!ai) return;
    ai_free_arrays(ai);
    free(ai);
    g->sub->ai = NULL;
}

/* ---- Scheduler ----------------------------------------------------------------- */

static bool ai_drives(const OfeGame *g, PlayerId p) {
    int type = g->players.type[p];
    return type != OFE_PLAYER_HUMAN || g->cfg.humans_use_ai;
}

static void ai_init_player(OfeGame *g, PlayerId p) {
    if (g->players.type[p] == OFE_PLAYER_BOT) ai_tribe_init(g, p);
    else ai_nation_init(g, p);
}

void ai_on_spawn(OfeGame *g, PlayerId p) {
    if (g->players.ai_state[p][AI_RATE] == 0) ai_init_player(g, p);
}

int32_t ai_next_beat(int32_t from, int32_t rate, int32_t offset) {
    int32_t d = (offset - from) % rate;
    return from + (d < 0 ? d + rate : d);
}

void ai_spawn_phase_tick(OfeGame *g) { ai_nation_spawn_tick(g); }

/* OpenFront creates a nation's behaviours (and sends its opening attack) on
 * the first tick after the spawn phase, so the opening is not delayed by the
 * nation's beat. */
void ai_on_spawn_phase_end(OfeGame *g) {
    PlayerTable *pl = &g->players;
    for (PlayerId p = 1; p < pl->count; p++) {
        if (pl->type[p] == OFE_PLAYER_BOT || !ai_drives(g, p) || !pl->spawned[p] || pl->num_tiles[p] <= 0) continue;
        if (pl->ai_state[p][AI_RATE] == 0) ai_init_player(g, p);
        ai_nation_open(g, p);
    }
}

void ai_tick(OfeGame *g) {
    PlayerTable *pl = &g->players;
    if (g->cfg.difficulty >= OFE_DIFFICULTY_HARD) ai_nation_follow_up_landings(g);
    for (PlayerId p = 1; p < pl->count; p++) {
        if (!ai_drives(g, p) || !pl->spawned[p] || pl->dead[p] || pl->num_tiles[p] <= 0) continue;
        if (pl->ai_state[p][AI_RATE] == 0) ai_init_player(g, p);
        if (g->tick < pl->ai_next_tick[p]) continue;
        if (pl->type[p] == OFE_PLAYER_BOT) ai_tribe_decide(g, p);
        else if (pl->ai_state[p][AI_PHASE] == 0) ai_nation_decide(g, p);
        else ai_nation_structures_only(g, p);
    }
}

/* ---- Border scan (player.nearby()) ------------------------------------------------- */

static void add_neighbor(AiScan *ai, PlayerId o, uint8_t bits) {
    if (ai->stamp[o] != ai->gen) {
        ai->stamp[o] = ai->gen;
        ai->flag[o] = bits;
        IntVec_push(&ai->nb, o);
    } else {
        ai->flag[o] |= bits;
    }
}

/* Land 5 tiles out across a narrow strait, as OpenFront's shoreReachableNeighbors. */
static void scan_across_strait(AiScan *ai, const OfeGame *g, PlayerId p, TileRef shore, AiNeighborInfo *info) {
    static const int dx[4] = {0, 0, -1, 1}, dy[4] = {-1, 1, 0, 0};
    const OfeMap *m = g->map;
    int32_t bx = map_x(m, shore), by = map_y(m, shore);
    for (int d = 0; d < 4; d++) {
        int32_t x1 = bx + dx[d], y1 = by + dy[d];
        if (!map_in_bounds(m, x1, y1) || !map_is_water(m, map_ref(m, x1, y1))) continue;
        int32_t nx = bx + dx[d] * 5, ny = by + dy[d] * 5;
        if (!map_in_bounds(m, nx, ny)) continue;
        TileRef t = map_ref(m, nx, ny);
        if (!map_is_ownable(m, t)) continue;
        PlayerId o = tile_owner(g, t);
        if (o == p) continue;
        if (o == 0) {
            if (!tile_has_fallout(g, t)) info->tn_shore = true;
        } else {
            add_neighbor(ai, o, 2);
        }
    }
}

static void scan_neighbors(AiScan *ai, int32_t cap, const OfeGame *g, PlayerId p, AiNeighborInfo *info) {
    const OfeMap *m = g->map;
    const IntVec *b = &g->players.border[p];
    if (++ai->gen == 0) {
        memset(ai->stamp, 0, (size_t)cap * sizeof(uint32_t));
        ai->gen = 1;
    }
    ai->nb.len = 0;
    memset(info, 0, sizeof(*info));
    info->minx = info->miny = INT32_MAX;
    info->maxx = info->maxy = INT32_MIN;
    int32_t shore_idx = 0;
    for (int32_t i = 0; i < b->len; i++) {
        TileRef t = b->data[i];
        int32_t x = map_x(m, t), y = map_y(m, t);
        if (x < info->minx) info->minx = x;
        if (x > info->maxx) info->maxx = x;
        if (y < info->miny) info->miny = y;
        if (y > info->maxy) info->maxy = y;
        TileRef nb[4];
        int n = map_neighbors4(m, t, nb);
        for (int k = 0; k < n; k++) {
            TileRef q = nb[k];
            if (!map_is_ownable(m, q)) continue;
            PlayerId o = tile_owner(g, q);
            if (o == p) continue;
            if (o == 0) {
                info->tn_land = true;
                if (tile_has_fallout(g, q)) info->tn_fallout = true;
                else info->tn_clean = true;
            } else {
                add_neighbor(ai, o, 1);
            }
        }
        if (map_is_shore(m, t)) {
            info->shore_tiles++;
            if (shore_idx++ % 10 == 0) scan_across_strait(ai, g, p, t, info);
        }
    }
}

bool ai_is_neighbor(const AiCtx *c, PlayerId q) { return c->ai->main.stamp[q] == c->ai->main.gen; }
bool ai_borders_by_land(const AiCtx *c, PlayerId q) { return c->ai->main.stamp[q] == c->ai->main.gen && (c->ai->main.flag[q] & 1); }

int32_t ai_side_neighbors(OfeGame *g, PlayerId p, PlayerId *out, int32_t cap) {
    AiSysState *ai = ai_sys(g);
    AiNeighborInfo info;
    scan_neighbors(&ai->side, ai->cap, g, p, &info);
    int32_t n = 0;
    for (int32_t i = 0; i < ai->side.nb.len && n < cap; i++) {
        PlayerId q = ai->side.nb.data[i];
        if (g->players.type[q] != OFE_PLAYER_BOT) out[n++] = q;
    }
    return n;
}

const AiBox *ai_player_box(OfeGame *g, AiSysState *ai, PlayerId q) {
    if (g->tick - ai->box_tick[q] >= BOX_REFRESH_TICKS) {
        const OfeMap *m = g->map;
        const IntVec *b = &g->players.border[q];
        AiBox box = {INT32_MAX, INT32_MAX, INT32_MIN, INT32_MIN};
        for (int32_t i = 0; i < b->len; i++) {
            int32_t x = map_x(m, b->data[i]), y = map_y(m, b->data[i]);
            if (x < box.minx) box.minx = x;
            if (x > box.maxx) box.maxx = x;
            if (y < box.miny) box.miny = y;
            if (y > box.maxy) box.maxy = y;
        }
        ai->box[q] = box;
        ai->box_tick[q] = g->tick;
    }
    return &ai->box[q];
}

/* ---- Attack summary (incomingAttacks / outgoingAttacks) ------------------------------ */

static void fight_add(AiFight *list, int32_t *n, PlayerId who, double troops, bool land) {
    for (int32_t i = 0; i < *n; i++) {
        if (list[i].who == who) {
            list[i].troops += troops;
            if (land) list[i].land += troops;
            return;
        }
    }
    if (*n >= AI_MAX_FIGHTS) return;
    list[*n].who = who;
    list[*n].troops = troops;
    list[*n].land = land ? troops : 0;
    (*n)++;
}

static void collect_fights(const OfeGame *g, PlayerId p, AiFights *f) {
    const AttackTable *t = &g->attacks;
    memset(f, 0, sizeof(*f));
    for (int32_t i = 0; i < t->count; i++) {
        if (!t->active[i]) continue;
        if (t->attacker[i] == p) {
            fight_add(f->out, &f->n_out, t->target[i], t->troops[i], t->source_tile[i] < 0);
            f->out_total += t->troops[i];
        } else if (t->target[i] == p) {
            fight_add(f->in, &f->n_in, t->attacker[i], t->troops[i], t->source_tile[i] < 0);
            f->in_total += t->troops[i];
        }
    }
}

void ai_ctx_begin(AiCtx *c, OfeGame *g, PlayerId p) {
    memset(c, 0, sizeof(*c));
    c->g = g;
    c->ai = ai_sys(g);
    c->p = p;
    c->rng = &g->players.ai_rng[p];
    c->diff = OFE_CLAMP(g->cfg.difficulty, 0, 3);
    collect_fights(g, p, &c->fights);
}

void ai_ctx_scan(AiCtx *c) {
    if (c->scanned) return;
    c->scanned = true;
    scan_neighbors(&c->ai->main, c->ai->cap, c->g, c->p, &c->nbi);
    if (c->nbi.minx <= c->nbi.maxx) {
        AiBox *b = &c->ai->box[c->p];
        b->minx = c->nbi.minx;
        b->miny = c->nbi.miny;
        b->maxx = c->nbi.maxx;
        b->maxy = c->nbi.maxy;
        c->ai->box_tick[c->p] = c->g->tick;
    }
}

double ai_incoming_from(const AiCtx *c, PlayerId who) {
    for (int32_t i = 0; i < c->fights.n_in; i++) {
        if (c->fights.in[i].who == who) return c->fights.in[i].troops;
    }
    return 0;
}

double ai_incoming_on(const OfeGame *g, PlayerId target) {
    const AttackTable *t = &g->attacks;
    double sum = 0;
    for (int32_t i = 0; i < t->count; i++) {
        if (t->active[i] && t->target[i] == target) sum += t->troops[i];
    }
    return sum;
}

PlayerId ai_find_incoming_attacker(const AiCtx *c, bool ignore_bots) {
    const OfeGame *g = c->g;
    PlayerId best = 0;
    double largest = 0;
    for (int32_t i = 0; i < c->fights.n_in; i++) {
        const AiFight *f = &c->fights.in[i];
        if (player_friendly(g, c->p, f->who)) continue;
        if (ignore_bots && g->players.type[f->who] == OFE_PLAYER_BOT) continue;
        if (f->troops <= largest) continue;
        largest = f->troops;
        best = f->who;
    }
    return best;
}

/* ---- Relations -------------------------------------------------------------------------- */

int ai_relation_class(double v) {
    if (v < -50) return REL_HOSTILE;
    if (v < 0) return REL_DISTRUSTFUL;
    if (v < 50) return REL_NEUTRAL;
    return REL_FRIENDLY;
}

int ai_relation(const OfeGame *g, PlayerId self, PlayerId other) { return ai_relation_class(player_relation(g, self, other)); }

/* ---- Troops ------------------------------------------------------------------------------- */

bool ai_high_start_gold(const OfeGame *g) { return g->cfg.starting_gold >= HIGH_STARTING_GOLD; }

/* With high starting gold, cities built in one rush must not hold back
 * expansion: the ratios then refer to the troop cap without cities. */
double ai_max_troops(const OfeGame *g, PlayerId p, bool expansion) {
    double max = player_max_troops(g, p);
    if (!expansion || !ai_high_start_gold(g)) return max;
    double city = g->players.unit_levels[p][OFE_UNIT_CITY] * 250000.0;
    if (city <= 0) return max;
    double base = 2.0 * (pow((double)g->players.num_tiles[p], 0.6) * 1000.0 + 50000.0) + city;
    return max * (base - city) / base;
}

bool ai_player_has_structures(const OfeGame *g, PlayerId p) {
    const int32_t *n = g->players.unit_count[p];
    return n[OFE_UNIT_CITY] + n[OFE_UNIT_PORT] + n[OFE_UNIT_FACTORY] + n[OFE_UNIT_SAM_LAUNCHER] + n[OFE_UNIT_MISSILE_SILO] +
               n[OFE_UNIT_DEFENSE_POST] > 0;
}

/* Troops kept home when attacking target: the expand ratio for free land and
 * for bots holding structures (we want those back fast), otherwise the
 * reserve ratio. */
double ai_reserve_troops(const AiCtx *c, PlayerId target) {
    const OfeGame *g = c->g;
    const int32_t *st = g->players.ai_state[c->p];
    bool is_player = target != 0;
    bool is_bot = is_player && g->players.type[target] == OFE_PLAYER_BOT;
    double max = ai_max_troops(g, c->p, !is_player || is_bot);
    bool bot_with_structures = is_bot && ai_player_has_structures(g, target);
    int pct = (is_player && !bot_with_structures) ? st[AI_RESERVE] : st[AI_EXPAND];
    return max * pct / 100.0;
}

bool ai_has_ratio_troops(const AiCtx *c, int ratio_percent, bool expansion) {
    const OfeGame *g = c->g;
    double max = ai_max_troops(g, c->p, expansion);
    return max > 0 && (double)g->players.troops[c->p] / max >= ratio_percent / 100.0;
}

/* ---- Issuing ---------------------------------------------------------------------------------- */

bool ai_issue_attack(AiCtx *c, PlayerId target, double troops) {
    OfeGame *g = c->g;
    if (troops < 1) return false;
    OfeIntent in = {0};
    in.type = OFE_INTENT_ATTACK;
    in.player = c->p;
    in.target = target;
    in.troops = troops;
    int32_t before = g->next_attack_id;
    game_issue(g, &in);
    return g->next_attack_id != before;
}

bool ai_issue_boat(AiCtx *c, TileRef dst, double troops) {
    OfeGame *g = c->g;
    if (troops < 1) return false;
    OfeIntent in = {0};
    in.type = OFE_INTENT_BOAT_ATTACK;
    in.player = c->p;
    in.tile = dst;
    in.troops = troops;
    int32_t before = g->players.unit_count[c->p][OFE_UNIT_TRANSPORT_SHIP];
    game_issue(g, &in);
    return g->players.unit_count[c->p][OFE_UNIT_TRANSPORT_SHIP] > before;
}

/* ---- Boats ---------------------------------------------------------------------------------------- */

bool ai_boats_available(const AiCtx *c) {
    const OfeGame *g = c->g;
    if (g->cfg.disabled_units & (1u << OFE_UNIT_TRANSPORT_SHIP)) return false;
    return g->players.unit_count[c->p][OFE_UNIT_TRANSPORT_SHIP] < MAX_TRANSPORT_SHIPS;
}

bool ai_boat_route(AiCtx *c, PlayerId target, TileRef *landing, int32_t *length) {
    OfeGame *g = c->g;
    const OfeMap *m = g->map;
    for (int32_t i = 0; i < c->n_routes; i++) {
        if (c->routes[i].target == target) {
            *landing = c->routes[i].landing;
            *length = c->routes[i].length;
            return c->routes[i].ok;
        }
    }
    AiRoute r = {target, false, -1, 0};
    const IntVec *tb = &g->players.border[target];
    if (ai_boats_available(c) && tb->len > 0 && c->nbi.minx <= c->nbi.maxx) {
        /* A handful of random shore tiles of the target, nearest to our land first. */
        TileRef cand[8];
        int32_t n = 0;
        for (int tries = 0; tries < 24 && n < 8; tries++) {
            TileRef t = tb->data[rng_int(c->rng, 0, tb->len)];
            if (map_is_shore(m, t)) cand[n++] = t;
        }
        TileRef center = map_ref(m, (c->nbi.minx + c->nbi.maxx) / 2, (c->nbi.miny + c->nbi.maxy) / 2);
        for (int32_t i = 1; i < n; i++) { /* insertion sort by distance */
            TileRef t = cand[i];
            int32_t d = map_manhattan(m, center, t), j = i - 1;
            while (j >= 0 && map_manhattan(m, center, cand[j]) > d) {
                cand[j + 1] = cand[j];
                j--;
            }
            cand[j + 1] = t;
        }
        for (int32_t i = 0; i < n && i < 3 && !r.ok; i++) {
            TileRef src = ships_transport_source(g, c->p, cand[i]);
            if (src < 0) continue;
            r.ok = true;
            r.landing = cand[i];
            r.length = map_manhattan(m, src, cand[i]);
        }
    }
    if (c->n_routes < AI_MAX_ROUTES) c->routes[c->n_routes++] = r;
    *landing = r.landing;
    *length = r.length;
    return r.ok;
}

TileRef ai_find_free_land_landing(AiCtx *c) {
    static const int dx[4] = {0, 0, -1, 1}, dy[4] = {-1, 1, 0, 0};
    OfeGame *g = c->g;
    const OfeMap *m = g->map;
    const IntVec *b = &g->players.border[c->p];
    if (!ai_boats_available(c) || b->len == 0 || c->nbi.shore_tiles == 0) return -1;
    int32_t checks = 0;
    int32_t steps = b->len / 10 + 1;
    int32_t start = rng_int(c->rng, 0, b->len);
    for (int32_t k = 0; k < steps; k++) {
        TileRef t = b->data[(start + (int64_t)k * 10) % b->len];
        if (!map_is_shore(m, t)) continue;
        int32_t bx = map_x(m, t), by = map_y(m, t);
        for (int d = 0; d < 4; d++) {
            int32_t x1 = bx + dx[d], y1 = by + dy[d];
            if (!map_in_bounds(m, x1, y1) || !map_is_water(m, map_ref(m, x1, y1))) continue;
            int32_t nx = bx + dx[d] * 5, ny = by + dy[d] * 5;
            if (!map_in_bounds(m, nx, ny)) continue;
            TileRef land = map_ref(m, nx, ny);
            if (!map_is_ownable(m, land) || tile_has_owner(g, land) || tile_has_fallout(g, land)) continue;
            if (checks++ >= 6) return -1;
            if (ships_transport_source(g, c->p, land) >= 0) return land;
        }
    }
    return -1;
}

/* ---- Leaders (shared by nation alliance / attack / MIRV decisions) --------------------------------------- */

/* The FFA player (not a tribe) with the most land, if far enough ahead of the
 * runner-up that nations stop helping it and start pressuring it. */
static double runaway_factor(int diff) {
    static const double f[4] = {0, 3, 2, 1.5}; /* Easy never notices */
    return f[diff];
}

static void refresh_leaders(OfeGame *g, AiSysState *ai) {
    if (g->tick - ai->top_tick < LEADER_REFRESH_TICKS) return;
    const PlayerTable *pl = &g->players;
    PlayerId leader = 0;
    int32_t top = 0, second = 0;
    for (PlayerId q = 1; q < pl->count; q++) {
        if (pl->type[q] == OFE_PLAYER_BOT || pl->num_tiles[q] <= 0) continue;
        int32_t n = pl->num_tiles[q];
        if (n > top) {
            second = top;
            top = n;
            leader = q;
        } else if (n > second) {
            second = n;
        }
    }
    ai->top_tick = ai->leader_tick = g->tick;
    ai->top_player = leader;
    ai->top_tiles = top;
    double factor = runaway_factor(OFE_CLAMP(g->cfg.difficulty, 0, 3));
    bool runaway = g->cfg.game_mode == OFE_MODE_FFA && factor > 0 && leader != 0 && second > 0 && top >= second * factor;
    ai->leader = runaway ? leader : 0;
}

PlayerId ai_land_leader(OfeGame *g, AiSysState *ai, int32_t *tiles) {
    refresh_leaders(g, ai);
    if (tiles) *tiles = ai->top_tiles;
    return ai->top_player;
}

PlayerId ai_runaway_leader(OfeGame *g, AiSysState *ai) {
    refresh_leaders(g, ai);
    return ai->leader;
}

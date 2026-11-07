/*
 * attack.c - land attacks (and the land phase of boat landings).
 *
 * An attack holds a troop pool and a min-heap of candidate tiles: target
 * tiles adjacent to the attacker. Each tick it pops tiles and conquers them
 * until its tick budget (1.0) is spent; every conquered tile costs the
 * attacker troops and the defender its average troops per tile, and uses up
 * a fraction of the budget (see attack_logic, OpenFront's attackLogic).
 *
 * Tile priority = (rand[0,7) + 10) * (1 - 0.5 * owned_neighbours + mag / 2)
 *                 + current tick, lowest first,
 * so the front advances where the attacker already surrounds a tile, and
 * prefers plains over mountains.
 *
 * Exact in both builds: each attack draws from its own TsRandom(123) like
 * OpenFront's AttackExecution (priority draws, then one jitter draw per tick),
 * lists of attacks are walked in creation order, an empty front divides by
 * zero as in TS, and attack troops never go below 0. The parity build adds
 * the TS iteration of the border set and its handleDeadDefender pass.
 */
#include "internal.h"

#define RETREAT_MALUS_PERCENT 25.0
#define RETREAT_DELAY_TICKS 20

/* attackLogic tunables (EngineConfig.ts) */
#define LARGE_TERRITORY_MIDPOINT 300000.0
#define LARGE_TERRITORY_STEEPNESS 2.5
#define LARGE_ATTACKER_DEPTH 0.7
#define LARGE_DEFENDER_DEPTH 0.3
#define LARGE_ATTACKER_SPEED_DEPTH 0.73
#define BOT_DEFENDER_LOSS_MULT 0.7
#define TERRA_NULLIUS_COST_SCALE 2000.0
#define TERRA_NULLIUS_MIN_COST 5.0
#define TERRA_NULLIUS_MAX_COST 100.0
#define ATTACKER_LOSS_BASE 0.463
#define CHEAPEST_TROOP_RATIO 0.6
#define ATTACKER_LOSS_PER_DENSITY 0.0039
#define SPEED_COST_DIVISOR 8.55
#define DEFENSE_POST_RANGE 30
#define DEFENSE_POST_DEFENSE_BONUS 5.0
#define DEFENSE_POST_SPEED_BONUS 3.0
#define TRAITOR_DEFENSE_DEBUFF 0.5
#define TRAITOR_SPEED_DEBUFF 0.8

/* ---- Table management ------------------------------------------------------ */

static int32_t attack_alloc(OfeGame *g) {
    AttackTable *t = &g->attacks;
    /* Reuse an inactive row. */
    for (int32_t i = 0; i < t->count; i++) {
        if (!t->active[i]) return i;
    }
    if (t->count == t->cap) {
        int32_t cap = t->cap ? t->cap * 2 : 32;
        ATTACK_COLUMNS(GROW_COLUMN)
        t->to_conquer = (TileHeap *)ofe_xrealloc(t->to_conquer, (size_t)cap * sizeof(TileHeap));
        t->front = (IntSet *)ofe_xrealloc(t->front, (size_t)cap * sizeof(IntSet));
        memset(t->to_conquer + t->cap, 0, (size_t)(cap - t->cap) * sizeof(TileHeap));
        memset(t->front + t->cap, 0, (size_t)(cap - t->cap) * sizeof(IntSet));
        t->cap = cap;
    }
    return t->count++;
}

static void attack_free_row(OfeGame *g, int32_t a) {
    AttackTable *t = &g->attacks;
    t->active[a] = 0;
    t->to_conquer[a].len = 0;
    intset_clear(&t->front[a]);
    /* Trim trailing inactive rows so loops stay short. */
    while (t->count > 0 && !t->active[t->count - 1]) t->count--;
}

static void attack_end(OfeGame *g, int32_t a, int reason) {
    AttackTable *t = &g->attacks;
    event_emit(g, OFE_EV_ATTACK_ENDED, t->id[a], t->attacker[a], t->target[a], reason, t->troops[a]);
    attack_free_row(g, a);
}

double attack_outgoing_troops(const OfeGame *g, PlayerId p) {
    const AttackTable *t = &g->attacks;
    double sum = 0;
    for (int32_t i = 0; i < t->count; i++) {
        if (t->active[i] && t->attacker[i] == p) sum += t->troops[i];
    }
    return sum;
}

int32_t attack_count_outgoing(const OfeGame *g, PlayerId p) {
    const AttackTable *t = &g->attacks;
    int32_t n = 0;
    for (int32_t i = 0; i < t->count; i++) n += t->active[i] && t->attacker[i] == p;
    return n;
}

double attack_troops_against(const OfeGame *g, PlayerId attacker, PlayerId target) {
    const AttackTable *t = &g->attacks;
    double best = 0;
    for (int32_t i = 0; i < t->count; i++) {
        if (t->active[i] && t->attacker[i] == attacker && t->target[i] == target) best = OFE_MAX(best, t->troops[i]);
    }
    return best;
}

/* Active attack rows sorted by id: the order OpenFront creates them in. Row
 * indices are reused, so they do not tell. */
static void attacks_in_creation_order(const OfeGame *g, IntVec *out) {
    const AttackTable *t = &g->attacks;
    for (int32_t i = 0; i < t->count; i++) {
        if (!t->active[i]) continue;
        int32_t j = out->len;
        IntVec_push(out, i);
        while (j > 0 && t->id[out->data[j - 1]] > t->id[i]) {
            out->data[j] = out->data[j - 1];
            j--;
        }
        out->data[j] = i;
    }
}

/* ---- Combat formula ----------------------------------------------------------- */

typedef struct AttackResult {
    double attacker_loss;
    double defender_loss;
    double tick_fraction;
} AttackResult;

static double large_territory_bonus(double num_tiles, double depth) {
    return 1.0 - depth * sigmoid(ofe_log(OFE_MAX(num_tiles, 1.0)), LARGE_TERRITORY_STEEPNESS, ofe_log(LARGE_TERRITORY_MIDPOINT));
}

/* OpenFront's attackLogic: the cost of taking one tile. */
static AttackResult attack_logic(const OfeGame *g, int32_t a, TileRef tile, double attack_troops, double border_size) {
    const PlayerTable *pl = &g->players;
    PlayerId attacker = g->attacks.attacker[a];
    PlayerId defender = g->attacks.target[a];
    double mag, tile_cost;
    switch (g->map->tclass[tile]) {
    case TC_HIGHLAND: mag = 100.0; tile_cost = 20.0; break;
    case TC_MOUNTAIN: mag = 120.0; tile_cost = 25.0; break;
    default: mag = 80.0; tile_cost = 16.5; break;
    }

    if (defender != 0 && pl->unit_count[defender][OFE_UNIT_DEFENSE_POST] > 0 &&
        units_has_nearby(g, tile, DEFENSE_POST_RANGE, OFE_UNIT_DEFENSE_POST, defender)) {
        mag *= DEFENSE_POST_DEFENSE_BONUS;
        tile_cost *= DEFENSE_POST_SPEED_BONUS;
    }
    if (tile_has_fallout(g, tile)) {
        double ratio = (double)g->num_fallout_tiles / OFE_MAX(1, g->num_land_tiles);
        double fallout = 5.0 - ratio * 2.0;
        mag *= fallout;
        tile_cost *= fallout;
    }

    AttackResult r;
    if (defender == 0) {
        double tick_budget = border_size * 2.0;
        r.attacker_loss = mag / (pl->type[attacker] == OFE_PLAYER_BOT ? 10.0 : 5.0);
        r.defender_loss = 0;
        r.tick_fraction = OFE_CLAMP(TERRA_NULLIUS_COST_SCALE * tile_cost / attack_troops, TERRA_NULLIUS_MIN_COST,
                                    TERRA_NULLIUS_MAX_COST) / tick_budget;
        return r;
    }

    if (pl->disconnected[defender] && player_same_team(g, attacker, defender)) mag = 0;
    int atype = pl->type[attacker];
    if ((atype == OFE_PLAYER_HUMAN || atype == OFE_PLAYER_NATION) && pl->type[defender] == OFE_PLAYER_BOT) {
        mag *= BOT_DEFENDER_LOSS_MULT;
    }

    double att_tiles = pl->num_tiles[attacker], def_tiles = OFE_MAX(1, pl->num_tiles[defender]);
    double large_att = large_territory_bonus(att_tiles, LARGE_ATTACKER_DEPTH);
    double large_def = large_territory_bonus(def_tiles, LARGE_DEFENDER_DEPTH);
    bool traitor = player_is_traitor(g, defender);
    double traitor_loss = traitor ? TRAITOR_DEFENSE_DEBUFF : 1.0;
    double traitor_cost = traitor ? TRAITOR_SPEED_DEBUFF : 1.0;

    double def_troops = (double)pl->troops[defender];
    r.defender_loss = def_troops / def_tiles;
    double ratio = def_troops / attack_troops;
    r.attacker_loss = mag * traitor_loss * OFE_CLAMP(ratio, CHEAPEST_TROOP_RATIO, 2.0) *
                      (ATTACKER_LOSS_BASE * large_att * large_def + ATTACKER_LOSS_PER_DENSITY * r.defender_loss);
    double speed_cost = OFE_CLAMP(ratio, 0.82, 7.5) * OFE_CLAMP(ratio / 20.0, 1.0, 50.0) / SPEED_COST_DIVISOR;
    double large_att_speed = large_territory_bonus(att_tiles, LARGE_ATTACKER_SPEED_DEPTH);
    r.tick_fraction = speed_cost * tile_cost * large_att_speed * large_def * traitor_cost / border_size;
    return r;
}

/* ---- Candidate tiles ------------------------------------------------------------ */

static void add_neighbors(OfeGame *g, int32_t a, TileRef tile) {
    AttackTable *t = &g->attacks;
    const OfeMap *m = g->map;
    PlayerId me = t->attacker[a], target = t->target[a];
    TileRef nb[4];
    int n = map_neighbors4(m, tile, nb);
    for (int i = 0; i < n; i++) {
        TileRef q = nb[i];
        if (!map_is_ownable(m, q) || tile_owner(g, q) != target) continue;
        intset_add(&t->front[a], q);
        TileRef nb2[4];
        int n2 = map_neighbors4(m, q, nb2);
        int owned_by_me = 0;
        for (int j = 0; j < n2; j++) owned_by_me += tile_owner(g, nb2[j]) == me;
        double mag;
        switch (m->tclass[q]) {
        case TC_PLAINS: mag = 1.0; break;
        case TC_HIGHLAND: mag = 1.5; break;
        case TC_MOUNTAIN: mag = 2.0; break;
        default: mag = 0.0; break;
        }
        double pri = (ts_rand_int(&t->rng[a], 0, 7) + 10) * (1.0 - owned_by_me * 0.5 + mag / 2.0) + g->tick;
        heap_push(&t->to_conquer[a], q, (float)pri);
    }
}

static void refresh_to_conquer(OfeGame *g, int32_t a) {
    AttackTable *t = &g->attacks;
    t->to_conquer[a].len = 0;
    intset_clear(&t->front[a]);
#if OFE_TS_PARITY
    /* TS: ownerBorderTiles.forEach in set insertion order. */
    const TileSeq *b = &g->players.border_seq[t->attacker[a]];
    for (int32_t i = 0; i < b->len; i++) {
        if (b->data[i] >= 0) add_neighbors(g, a, b->data[i]);
    }
#else
    const IntVec *b = &g->players.border[t->attacker[a]];
    for (int32_t i = 0; i < b->len; i++) add_neighbors(g, a, b->data[i]);
#endif
}

/* ---- Retreat and merging ------------------------------------------------------------ */

static void retreat(OfeGame *g, int32_t a, double malus_percent, int reason) {
    AttackTable *t = &g->attacks;
    double survivors = t->troops[a] * (1.0 - malus_percent / 100.0);
    player_add_troops(g, t->attacker[a], survivors);
    t->troops[a] = survivors;
    attack_end(g, a, reason);
}

void attack_order_retreat(OfeGame *g, PlayerId owner, int32_t attack_id) {
    AttackTable *t = &g->attacks;
    for (int32_t i = 0; i < t->count; i++) {
        if (t->active[i] && t->id[i] == attack_id && t->attacker[i] == owner) {
            if (t->retreat_at[i] < 0) t->retreat_at[i] = g->tick + RETREAT_DELAY_TICKS;
            return;
        }
    }
}

/* ---- Start ----------------------------------------------------------------------------- */

static double relation_penalty(int difficulty) {
    switch (difficulty) {
    case OFE_DIFFICULTY_EASY: return -60;
    case OFE_DIFFICULTY_MEDIUM: return -70;
    case OFE_DIFFICULTY_HARD: return -80;
    default: return -100;
    }
}

int32_t attack_start(OfeGame *g, PlayerId attacker, PlayerId target, double troops, TileRef source_tile, bool remove_troops) {
    PlayerTable *pl = &g->players;
    if (!player_valid(g, attacker) || attacker == target) return -1;
    if (target != 0 && !player_valid(g, target)) return -1;
    if (target != 0 && player_friendly(g, attacker, target)) return -1;

    if (target != 0 && pl->type[target] != OFE_PLAYER_BOT && pl->type[attacker] != OFE_PLAYER_BOT) {
        /* Being attacked starts a temporary embargo and rejects the
         * attacker's pending alliance request... the reverse way round. */
        player_add_embargo(g, target, attacker, true);
        alliance_reject(g, target, attacker);
    }
    if (target != 0 && !player_can_attack_player(g, attacker, target)) return -1;

    if (troops < 0) troops = (double)pl->troops[attacker] / (pl->type[attacker] == OFE_PLAYER_BOT ? 20.0 : 5.0);
    if (remove_troops) {
        troops = OFE_MIN((double)pl->troops[attacker], troops);
        troops = player_remove_troops(g, attacker, troops);
    }

    AttackTable *t = &g->attacks;
    int32_t a = attack_alloc(g);
    t->active[a] = 1;
    t->retreat_at[a] = -1;
    t->id[a] = ++g->next_attack_id;
    t->attacker[a] = attacker;
    t->target[a] = target;
    t->troops[a] = troops;
    t->source_tile[a] = source_tile;
    ts_rand_init(&t->rng[a], 123); /* AttackExecution: new PseudoRandom(123) */
    t->to_conquer[a].len = 0;
    intset_clear(&t->front[a]);
    pl->attacks_sent[attacker]++;
    event_emit(g, OFE_EV_ATTACK_STARTED, t->id[a], attacker, target, source_tile, troops);

    if (source_tile >= 0) add_neighbors(g, a, source_tile);
    else refresh_to_conquer(g, a);

    /* The owner's attack lists in creation order (TS iterates arrays). */
    IntVec order = {0};
    attacks_in_creation_order(g, &order);
    /* An attack the other way cancels out troop for troop. */
    for (int32_t k = 0; k < order.len && target != 0; k++) {
        int32_t i = order.data[k];
        if (!t->active[i] || i == a || t->attacker[i] != target || t->target[i] != attacker) continue;
        if (t->troops[i] > t->troops[a]) {
            t->troops[i] -= t->troops[a];
            t->troops[a] = 0;
            attack_end(g, a, OFE_ATTACK_END_MERGED);
            IntVec_free(&order);
            return -1;
        }
        t->troops[a] -= t->troops[i];
        t->troops[i] = 0;
        attack_end(g, i, OFE_ATTACK_END_MERGED);
    }
    /* Land attacks on the same target merge into this one. */
    if (source_tile < 0) {
        for (int32_t k = 0; k < order.len; k++) {
            int32_t i = order.data[k];
            if (!t->active[i] || i == a || t->attacker[i] != attacker || t->target[i] != target) continue;
            t->troops[a] += t->troops[i];
            t->troops[i] = 0;
            attack_end(g, i, OFE_ATTACK_END_MERGED);
        }
    }

    IntVec_free(&order);
    if (t->to_conquer[a].len == 0) {
        /* Nothing to conquer next to us: give the troops back. */
        if (!remove_troops && source_tile < 0) t->troops[a] = OFE_MAX(0.0, t->troops[a] - troops);
        retreat(g, a, 0, OFE_ATTACK_END_FINISHED);
        return -1;
    }

    if (target != 0 && pl->relation_row[target] >= 0) {
        /* Hitting back at someone we're hostile to doesn't anger them more,
         * except on Easy. */
        bool hostile = player_relation(g, attacker, target) < -50;
        if (g->cfg.difficulty == OFE_DIFFICULTY_EASY || !hostile) {
            player_update_relation(g, target, attacker, relation_penalty(g->cfg.difficulty));
        }
    }
    return t->id[a];
}

/* ---- Tick ------------------------------------------------------------------------------- */

/* Target lost (almost) everything: it's conquered and its last tiles go to
 * whoever borders them. */
#if OFE_TS_PARITY
/* TS handleDeadDefender: up to 100 passes over the target's tiles in set
 * order; a tile next to the attacker goes to it, else to the first
 * neighbouring player that is neither the target nor friendly to it. Tiles
 * handed over during a pass are skipped, and later tiles see the changes. */
static void dead_defender_pass_ts(OfeGame *g, PlayerId attacker, PlayerId target, bool *progressed) {
    TileSeq *seq = &g->players.tiles_seq[target];
    tile_seq_iter_begin(g);
    for (int32_t i = 0; i < seq->len; i++) {
        TileRef tile = seq->data[i];
        if (tile < 0) continue;
        TileRef nb[4];
        int n = map_neighbors4(g->map, tile, nb);
        PlayerId new_owner = 0;
        for (int k = 0; k < n && !new_owner; k++) {
            if (tile_owner(g, nb[k]) == attacker) new_owner = attacker;
        }
        for (int k = 0; k < n && !new_owner; k++) {
            PlayerId o = tile_owner(g, nb[k]);
            if (o != 0 && o != target && !player_friendly(g, o, target)) new_owner = o;
        }
        if (!new_owner) continue;
        tile_set_owner(g, tile, new_owner);
        *progressed = true;
    }
    tile_seq_iter_end(g);
}

static void handle_dead_defender(OfeGame *g, PlayerId attacker, PlayerId target) {
    if (target == 0 || g->players.num_tiles[target] >= 100) return;
    player_conquered(g, attacker, target);
    for (int pass = 0; pass < 100; pass++) {
        bool progressed = false;
        dead_defender_pass_ts(g, attacker, target, &progressed);
        if (!progressed) break;
    }
}
#else
static void handle_dead_defender(OfeGame *g, PlayerId attacker, PlayerId target) {
    if (target == 0 || g->players.num_tiles[target] >= 100) return;
    player_conquered(g, attacker, target);
    IntVec *tiles = &g->scratch_b;
    for (int pass = 0; pass < 100; pass++) {
        tiles_collect_player(g, target, tiles);
        bool progressed = false;
        for (int32_t i = 0; i < tiles->len; i++) {
            TileRef tile = tiles->data[i];
            if (tile_owner(g, tile) != target) continue;
            if (tile_borders_player(g, tile, attacker)) {
                tile_set_owner(g, tile, attacker);
                progressed = true;
                continue;
            }
            TileRef nb[4];
            int n = map_neighbors4(g->map, tile, nb);
            for (int k = 0; k < n; k++) {
                PlayerId o = tile_owner(g, nb[k]);
                if (o != 0 && o != target && !player_friendly(g, o, target)) {
                    tile_set_owner(g, tile, o);
                    progressed = true;
                    break;
                }
            }
        }
        if (!progressed) break;
    }
}

#endif

static void attack_tick_one(OfeGame *g, int32_t a) {
    AttackTable *t = &g->attacks;
    const OfeMap *m = g->map;
    PlayerId me = t->attacker[a], target = t->target[a];

    if (t->retreat_at[a] >= 0) {
        if (g->tick >= t->retreat_at[a]) retreat(g, a, target != 0 ? RETREAT_MALUS_PERCENT : 0, OFE_ATTACK_END_RETREAT);
        return; /* paused while the retreat is pending */
    }
    if (target != 0 && player_friendly(g, me, target)) {
        retreat(g, a, 0, OFE_ATTACK_END_FRIENDLY);
        return;
    }

    double troops = t->troops[a];
    /* No clamp: an empty front divides by zero (inf) like OpenFront's. */
    double border_size = t->front[a].len + ts_rand_int(&t->rng[a], 0, 5);
    double budget = 1.0;
    while (budget > 0) {
        if (troops < 1) {
            t->troops[a] = troops;
            attack_end(g, a, OFE_ATTACK_END_DEPLETED);
            return;
        }
        if (t->to_conquer[a].len == 0) {
            refresh_to_conquer(g, a);
            t->troops[a] = troops;
            retreat(g, a, 0, OFE_ATTACK_END_FINISHED);
            return;
        }
        TileRef tile = heap_pop(&t->to_conquer[a]);
        intset_remove(&t->front[a], tile);
        if (tile_owner(g, tile) != target || !tile_borders_player(g, tile, me) || !map_is_ownable(m, tile)) continue;

        add_neighbors(g, a, tile);
        AttackResult r = attack_logic(g, a, tile, troops, border_size);
        budget -= r.tick_fraction;
        troops -= r.attacker_loss;
        t->troops[a] = OFE_MAX(0.0, troops); /* Attack.setTroops clamps at 0 */
        if (target != 0) player_remove_troops(g, target, r.defender_loss);
        tile_set_owner(g, tile, me);
        handle_dead_defender(g, me, target);
    }
}

/* Runs one attack's tick (the parity build ticks attacks in execution order). */
void attack_tick_row(OfeGame *g, int32_t a) { attack_tick_one(g, a); }

int32_t attack_row_by_id(const OfeGame *g, int32_t id) {
    for (int32_t i = 0; i < g->attacks.count; i++) {
        if (g->attacks.active[i] && g->attacks.id[i] == id) return i;
    }
    return -1;
}

void attacks_tick(OfeGame *g) {
    AttackTable *t = &g->attacks;
    /* Attacks started this tick also run this tick; that's fine. */
    int32_t n = t->count;
    for (int32_t a = 0; a < n; a++) {
        if (t->active[a]) attack_tick_one(g, a);
    }
}

/*
 * ai_nation.c - nations: NationExecution, NationAttackBehavior and
 * NationAllianceBehavior. Building, nukes, MIRVs and warships are in
 * ai_build.c.
 *
 * A nation wakes every `rate` ticks (Easy 65-100 ... Impossible 30-50) and
 * runs, in OpenFront's order: alliance extension replies, MIRV, structures,
 * warship, hostile embargoes, attack, nuke. Structures also get a turn at 1/3
 * and 2/3 of the period (AI_PHASE) so the gold doesn't pile up.
 *
 * The attack decision follows NationAttackBehavior: take free land first, now
 * and then boat somewhere, then (after saving up reserve / trigger troops)
 * walk a difficulty-specific list of strategies (retaliate, bots, assist
 * allies, traitors, afk players, betray, victims, juicy targets, hated
 * nations, very weak, weakest, islands, donate, crown) until one attacks.
 * Easy runs the dumbest list, Impossible the smartest one. Hard and
 * Impossible additionally cap what they send so the strongest neighbour
 * cannot walk over them, and land small beachhead boats.
 *
 * Simplifications: no emojis; no walled-in-bot annexation planner (bots are
 * attacked densest-last); boats are not checked against hostile warships;
 * "largest cluster" boxes are the border bounding box.
 */
#include <float.h>

#include "ai_common.h"

#define MAX_BORDER 96
#define ALLY_TARGET_TICKS 600 /* how long an ally's target counts */

typedef struct Bordering {
    PlayerId all[MAX_BORDER];
    PlayerId friends[MAX_BORDER];
    PlayerId enemies[MAX_BORDER];
    int32_t n, nf, ne;
} Bordering;

typedef PlayerId (*FindFn)(AiCtx *c, const PlayerId *list, int32_t n);

static bool is_ffa(const OfeGame *g) { return g->cfg.game_mode == OFE_MODE_FFA; }
static bool hard_or_impossible(const AiCtx *c) { return c->diff >= OFE_DIFFICULTY_HARD; }
static double my_troops(const AiCtx *c) { return (double)c->g->players.troops[c->p]; }
static double troops_of(const OfeGame *g, PlayerId q) { return (double)g->players.troops[q]; }

static double outgoing_total(const OfeGame *g, PlayerId q) {
    const AttackTable *t = &g->attacks;
    double sum = 0;
    for (int32_t i = 0; i < t->count; i++) {
        if (t->active[i] && t->attacker[i] == q) sum += t->troops[i];
    }
    return sum;
}

static void issue_simple(OfeGame *g, int type, PlayerId from, PlayerId target) {
    OfeIntent in = {0};
    in.type = type;
    in.player = from;
    in.target = target;
    game_issue(g, &in);
}

/* ---- Setup ------------------------------------------------------------------------ */

static int32_t attack_rate(OfeGame *g, uint64_t *rng) {
    switch (OFE_CLAMP(g->cfg.difficulty, 0, 3)) {
    case OFE_DIFFICULTY_EASY: return rng_int(rng, 65, 100);
    case OFE_DIFFICULTY_MEDIUM: return rng_int(rng, 55, 70);
    case OFE_DIFFICULTY_HARD: return rng_int(rng, 45, 60);
    default: return rng_int(rng, 30, 50);
    }
}

void ai_nation_init(OfeGame *g, PlayerId p) {
    PlayerTable *pl = &g->players;
    uint64_t *rng = &pl->ai_rng[p];
    int32_t *st = pl->ai_state[p];
    st[AI_TRIGGER] = rng_int(rng, 50, 60);
    st[AI_RESERVE] = rng_int(rng, 30, 40);
    st[AI_EXPAND] = rng_int(rng, 10, 20);
    st[AI_PLACEMENTS] = 0;
    st[AI_LAST_STRUCT] = -1;
    st[AI_PHASE] = 0;
    st[AI_FLAGS] = 0;
    int32_t rate = attack_rate(g, rng);
    st[AI_RATE] = rate;
    pl->ai_next_tick[p] = OFE_MAX(g->tick, g->spawn_phase_ticks) + rng_int(rng, 0, rate);
}

/* ---- Alliance decisions (NationAllianceBehavior) ------------------------------------------- */

static int32_t alliance_count(const OfeGame *g, PlayerId p) {
    int32_t n = 0;
    for (int32_t i = 0; i < g->alliances.len; i++) n += g->alliances.data[i].a == p || g->alliances.data[i].b == p;
    return n;
}

/* Most structure levels, most troop headroom and most land, each normalised
 * over the candidates (OpenFront's findJuiciestTarget). */
static PlayerId juiciest_target(const OfeGame *g, const PlayerId *cand, int32_t n) {
    if (n == 0) return 0;
    double s[MAX_BORDER], gap[MAX_BORDER], tiles[MAX_BORDER];
    double smin = DBL_MAX, smax = -DBL_MAX, gmin = DBL_MAX, gmax = -DBL_MAX, tmin = DBL_MAX, tmax = -DBL_MAX;
    n = OFE_MIN(n, MAX_BORDER);
    for (int32_t i = 0; i < n; i++) {
        const int32_t *lv = g->players.unit_levels[cand[i]];
        s[i] = lv[OFE_UNIT_CITY] + lv[OFE_UNIT_PORT] + lv[OFE_UNIT_FACTORY] + lv[OFE_UNIT_SAM_LAUNCHER];
        double max = player_max_troops(g, cand[i]);
        gap[i] = max > 0 ? 1.0 - troops_of(g, cand[i]) / max : 0;
        tiles[i] = g->players.num_tiles[cand[i]];
        smin = OFE_MIN(smin, s[i]); smax = OFE_MAX(smax, s[i]);
        gmin = OFE_MIN(gmin, gap[i]); gmax = OFE_MAX(gmax, gap[i]);
        tmin = OFE_MIN(tmin, tiles[i]); tmax = OFE_MAX(tmax, tiles[i]);
    }
    PlayerId best = 0;
    double best_score = -DBL_MAX;
    for (int32_t i = 0; i < n; i++) {
        double score = (smax > smin ? (s[i] - smin) / (smax - smin) : 0) + (gmax > gmin ? (gap[i] - gmin) / (gmax - gmin) : 0) +
                       (tmax > tmin ? (tiles[i] - tmin) / (tmax - tmin) : 0);
        if (score > best_score) {
            best_score = score;
            best = cand[i];
        }
    }
    return best;
}

static bool alliance_confused(OfeGame *g, uint64_t *rng) {
    static const int32_t one_in[4] = {10, 20, 40, 0};
    int32_t n = one_in[OFE_CLAMP(g->cfg.difficulty, 0, 3)];
    return n > 0 && rng_chance(rng, n);
}

/* Hard / Impossible refuse to ally with someone who already has many allies,
 * so enough non-friendly players remain to stop the crown. */
static bool too_many_alliances(OfeGame *g, PlayerId self, PlayerId other) {
    int diff = OFE_CLAMP(g->cfg.difficulty, 0, 3);
    if (diff < OFE_DIFFICULTY_HARD) return false;
    int32_t total = 0;
    for (PlayerId q = 1; q < g->players.count; q++) total += g->players.type[q] != OFE_PLAYER_BOT && player_alive(g, q);
    int32_t theirs = alliance_count(g, other) - (player_allied(g, other, self) ? 1 : 0);
    return theirs >= total * (diff == OFE_DIFFICULTY_HARD ? 0.5 : 0.25);
}

static bool earlygame(OfeGame *g, uint64_t *rng) {
    static const int32_t window[4] = {3000, 1800, 1800, 600};
    static const int32_t skip_pct[4] = {10, 30, 50, 70};
    int diff = OFE_CLAMP(g->cfg.difficulty, 0, 3);
    return g->tick < window[diff] + g->spawn_phase_ticks && rng_int(rng, 0, 100) >= skip_pct[diff];
}

static bool partner_is_threat(OfeGame *g, PlayerId self, PlayerId other) {
    double mine = troops_of(g, self), theirs = troops_of(g, other);
    switch (OFE_CLAMP(g->cfg.difficulty, 0, 3)) {
    case OFE_DIFFICULTY_EASY: return false;
    case OFE_DIFFICULTY_MEDIUM: return theirs > mine * 2.5;
    case OFE_DIFFICULTY_HARD: return theirs > mine && player_max_troops(g, other) > player_max_troops(g, self) * 2;
    default:
        return theirs > mine * 1.5 || (theirs > mine && player_max_troops(g, other) > player_max_troops(g, self) * 1.5) ||
               (theirs > mine && g->players.num_tiles[other] > g->players.num_tiles[self] * 1.5);
    }
}

static bool reject_in_team_game(OfeGame *g, uint64_t *rng) {
    if (g->cfg.game_mode != OFE_MODE_TEAM) return false;
    static const int32_t pct[4] = {25, 50, 75, 100};
    return rng_int(rng, 0, 100) < pct[OFE_CLAMP(g->cfg.difficulty, 0, 3)];
}

/* Already allied with enough of the neighbourhood. */
static bool enough_alliances(OfeGame *g, uint64_t *rng, PlayerId self, PlayerId other) {
    int diff = OFE_CLAMP(g->cfg.difficulty, 0, 3);
    switch (diff) {
    case OFE_DIFFICULTY_EASY: return false;
    case OFE_DIFFICULTY_MEDIUM: return alliance_count(g, self) >= rng_int(rng, 4, 6);
    default: {
        PlayerId nb[MAX_BORDER];
        int32_t n = ai_side_neighbors(g, self, nb, MAX_BORDER), friends = 0;
        bool has_other = false;
        for (int32_t i = 0; i < n; i++) {
            friends += player_friendly(g, self, nb[i]);
            has_other |= nb[i] == other;
        }
        if (n >= 2 && has_other) return n <= friends + 1;
        return alliance_count(g, self) >= (diff == OFE_DIFFICULTY_HARD ? rng_int(rng, 3, 5) : rng_int(rng, 2, 4));
    }
    }
}

static bool partner_is_friendly(OfeGame *g, uint64_t *rng, PlayerId self, PlayerId other) {
    if (ai_relation(g, self, other) != REL_FRIENDLY) return false;
    switch (OFE_CLAMP(g->cfg.difficulty, 0, 3)) {
    case OFE_DIFFICULTY_HARD: return rng_int(rng, 0, 100) >= 17;
    case OFE_DIFFICULTY_IMPOSSIBLE: return rng_int(rng, 0, 100) >= 33;
    default: return true;
    }
}

static bool partner_similarly_strong(OfeGame *g, uint64_t *rng, PlayerId self, PlayerId other) {
    static const int32_t troop_range[4][2] = {{60, 70}, {70, 80}, {75, 85}, {80, 90}};
    static const int32_t tile_range[4][2] = {{70, 80}, {80, 90}, {85, 95}, {90, 100}};
    int diff = OFE_CLAMP(g->cfg.difficulty, 0, 3);
    double mine = troops_of(g, self) + outgoing_total(g, self);
    double theirs = troops_of(g, other) + outgoing_total(g, other);
    double troop_threshold = mine * (rng_int(rng, troop_range[diff][0], troop_range[diff][1]) / 100.0);
    double tile_threshold = g->players.num_tiles[self] * (rng_int(rng, tile_range[diff][0], tile_range[diff][1]) / 100.0);
    bool comparable_troops = theirs > troop_threshold;
    bool comparable_tiles = g->players.num_tiles[other] > tile_threshold && theirs > mine * 0.5;
    return comparable_troops || comparable_tiles;
}

/* Would `self` ally with `other`? is_response: other asked us (vs. we ask). */
bool ai_nation_alliance_decision(OfeGame *g, PlayerId self, PlayerId other, bool is_response) {
    uint64_t *rng = &g->players.ai_rng[self];
    AiSysState *ai = ai_sys(g);
    if (alliance_confused(g, rng)) return rng_chance(rng, 2);
    if (player_is_traitor(g, other) && rng_int(rng, 0, 100) >= 10) return false;
    if (too_many_alliances(g, self, other)) return false;
    if (g->cfg.difficulty >= OFE_DIFFICULTY_MEDIUM && ai_runaway_leader(g, ai) == other) return false;
    /* A threat is befriended; Easy nations only care about the relation. */
    if (partner_is_threat(g, self, other)) return true;
    if (reject_in_team_game(g, rng)) return false;
    if (ai_relation(g, self, other) < REL_NEUTRAL) return false;
    if (partner_is_friendly(g, rng, self, other)) return true;
    if (enough_alliances(g, rng, self, other)) return false;
    if (earlygame(g, rng)) return true;
    return partner_similarly_strong(g, rng, self, other);
}

void ai_on_alliance_request(OfeGame *g, PlayerId requestor, PlayerId recipient) {
    if (g->players.type[recipient] == OFE_PLAYER_BOT) {
        /* Tribes accept every request. */
        issue_simple(g, OFE_INTENT_ALLIANCE_REQUEST, recipient, requestor);
        return;
    }
    if (ai_nation_alliance_decision(g, recipient, requestor, true)) issue_simple(g, OFE_INTENT_ALLIANCE_REQUEST, recipient, requestor);
    else issue_simple(g, OFE_INTENT_ALLIANCE_REJECT, recipient, requestor);
}

void ai_on_alliance_extension_prompt(OfeGame *g, PlayerId ai, PlayerId ally) {
    if (g->players.type[ai] == OFE_PLAYER_BOT) {
        ai_tribe_alliance_extension(g, ai, ally);
        return;
    }
    if (ai_nation_alliance_decision(g, ai, ally, true)) issue_simple(g, OFE_INTENT_ALLIANCE_EXTENSION, ai, ally);
}

/* An ally asked to extend; answer if we haven't (the prompt may have come first). */
static void handle_extension_requests(AiCtx *c) {
    OfeGame *g = c->g;
    if (g->cfg.disable_alliances) return;
    for (int32_t i = 0; i < g->alliances.len; i++) {
        const Alliance *al = &g->alliances.data[i];
        if (al->a != c->p && al->b != c->p) continue;
        bool mine_asked = al->a == c->p ? al->extend_a : al->extend_b;
        bool theirs_asked = al->a == c->p ? al->extend_b : al->extend_a;
        if (mine_asked || !theirs_asked) continue;
        PlayerId other = al->a == c->p ? al->b : al->a;
        if (ai_nation_alliance_decision(g, c->p, other, true)) {
            issue_simple(g, OFE_INTENT_ALLIANCE_EXTENSION, c->p, other);
            return; /* the alliance list may have changed */
        }
    }
}

static void maybe_send_alliance_requests(AiCtx *c, const Bordering *bd) {
    OfeGame *g = c->g;
    if (g->cfg.disable_alliances) return;
    for (int32_t i = 0; i < bd->ne; i++) {
        PlayerId e = bd->enemies[i];
        /* Only Easy nations court tribes. */
        bool acceptable = g->players.type[e] != OFE_PLAYER_BOT || c->diff == OFE_DIFFICULTY_EASY;
        if (!rng_chance(c->rng, 30) || !acceptable || !alliance_can_request(g, c->p, e)) continue;
        if (ai_nation_alliance_decision(g, c->p, e, false)) issue_simple(g, OFE_INTENT_ALLIANCE_REQUEST, c->p, e);
    }
}

/* ---- Betrayal ------------------------------------------------------------------------------- */

static bool safe_to_betray(AiCtx *c, const Bordering *bd, PlayerId target) {
    const OfeGame *g = c->g;
    double threat = troops_of(g, target) + outgoing_total(g, target);
    for (int32_t i = 0; i < bd->ne; i++) threat += troops_of(g, bd->enemies[i]) + outgoing_total(g, bd->enemies[i]);
    if (!player_is_traitor(g, target)) {
        for (int32_t i = 0; i < bd->nf; i++) {
            PlayerId f = bd->friends[i];
            if (f != target && player_allied(g, c->p, f)) threat += troops_of(g, f) + outgoing_total(g, f);
        }
    }
    return threat < my_troops(c) * 0.33;
}

static bool maybe_betray(AiCtx *c, const Bordering *bd, PlayerId other, PlayerId juiciest_ally) {
    OfeGame *g = c->g;
    if (!player_allied(g, c->p, other)) return false;
    double mine = my_troops(c), theirs = troops_of(g, other);
    bool betray = false;
    /* Betray the juiciest ally when everybody around us is weak. */
    if (hard_or_impossible(c) && juiciest_ally == other && safe_to_betray(c, bd, other)) betray = true;
    /* Easy and Medium betray very weak players (Easy never humans). */
    else if ((c->diff == OFE_DIFFICULTY_EASY || c->diff == OFE_DIFFICULTY_MEDIUM) &&
             !(c->diff == OFE_DIFFICULTY_EASY && g->players.type[other] == OFE_PLAYER_HUMAN) && mine >= theirs * 10) betray = true;
    else if (c->diff != OFE_DIFFICULTY_EASY && player_is_traitor(g, other) && theirs < mine * 1.2) betray = true;
    else if (c->diff != OFE_DIFFICULTY_EASY && bd->nf + bd->ne == 1 && theirs * 3 < mine) betray = true;
    if (betray) issue_simple(g, OFE_INTENT_BREAK_ALLIANCE, c->p, other);
    return betray;
}

/* ---- Troop budgeting (NationAttackBehavior.calculateAttackTroops) -------------------------------- */

/* Hard / Impossible FFA nations never let their troops fall below a fraction
 * of the strongest non-allied neighbour's, so they can't be walked over. */
static double neighbor_troop_cap(const AiCtx *c, PlayerId ignore) {
    const OfeGame *g = c->g;
    if (g->cfg.game_mode == OFE_MODE_TEAM) return INFINITY;
    double retain;
    if (c->diff == OFE_DIFFICULTY_HARD) retain = 0.75;
    else if (c->diff == OFE_DIFFICULTY_IMPOSSIBLE) retain = 0.9;
    else return INFINITY;
    double max_nb = 0;
    const IntVec *nb = &c->ai->main.nb;
    for (int32_t i = 0; i < nb->len; i++) {
        PlayerId q = nb->data[i];
        if (q == ignore || player_friendly(g, c->p, q) || g->players.type[q] == OFE_PLAYER_BOT) continue;
        if (troops_of(g, q) > max_nb) max_nb = troops_of(g, q);
    }
    if (max_nb == 0) return INFINITY;
    return OFE_MAX(0.0, my_troops(c) - ceil(max_nb * retain));
}

/* Nations under attack may retaliate with at least the incoming troops. */
static double troop_send_cap(const AiCtx *c, PlayerId ignore) {
    double cap = neighbor_troop_cap(c, ignore);
    if (c->fights.in_total > 0) cap = OFE_MAX(cap, c->fights.in_total);
    return cap;
}

/* Free land can't fight back, so it is throttled, never frozen. */
static double expansion_troop_cap(const AiCtx *c) {
    double cap = troop_send_cap(c, 0);
    return cap > 0 ? cap : ceil(my_troops(c) * 0.05);
}

/* Hard / Impossible FFA: not worth attacking with less than 20% of the target's troops. */
static bool attack_too_weak(const AiCtx *c, double troops, PlayerId target) {
    if (c->g->cfg.game_mode == OFE_MODE_TEAM || c->fights.n_in > 0) return false;
    return hard_or_impossible(c) && troops < troops_of(c->g, target) * 0.2;
}

static double large_territory_bonus(double tiles, double depth) {
    return 1.0 - depth * sigmoid(log(OFE_MAX(tiles, 1.0)), 2.5, log(300000.0));
}

/* Troops needed to eat a bot at the cheapest loss rate until it collapses
 * under 100 tiles (assumes highland, the common worst case). */
static double bot_troops_needed(const AiCtx *c, PlayerId bot) {
    const OfeGame *g = c->g;
    double stack = OFE_MAX(1.0, troops_of(g, bot) / 0.6);
    double tiles = OFE_MAX(0.0, (double)g->players.num_tiles[bot] - 100);
    double def_tiles = OFE_MAX(1, g->players.num_tiles[bot]);
    double density = troops_of(g, bot) / def_tiles;
    double ratio = OFE_CLAMP(troops_of(g, bot) / stack, 0.6, 2.0);
    double loss = 0.7 * 100.0 * ratio *
                  (0.463 * large_territory_bonus(g->players.num_tiles[c->p], 0.7) * large_territory_bonus(def_tiles, 0.3) + 0.0039 * density);
    return OFE_MAX(stack, tiles * loss);
}

/* Land troops already on the target, minus what it sends back (they cancel). */
static double our_stack_against(const AiCtx *c, PlayerId target) {
    double t = 0;
    for (int32_t i = 0; i < c->fights.n_out; i++) {
        if (c->fights.out[i].who == target) t += c->fights.out[i].land;
    }
    return t - ai_incoming_from(c, target);
}

static double bot_attack_troops(const AiCtx *c, PlayerId bot, double max_troops) {
    double bt = troops_of(c->g, bot);
    switch (c->diff) {
    case OFE_DIFFICULTY_EASY: return max_troops;
    case OFE_DIFFICULTY_MEDIUM: {
        double troops = bt * 4;
        if (troops > max_troops) troops = max_troops < bt * 2 ? 0 : max_troops;
        return troops;
    }
    default: {
        /* Top up what already attacks the bot, or wait until we can send it all. */
        double troops = bot_troops_needed(c, bot) * 1.3 - our_stack_against(c, bot);
        return troops <= max_troops ? troops : 0;
    }
    }
}

/* Troops to send at target, 0 if the attack isn't worth it. `fixed` (not NaN)
 * replaces the default "everything above the reserve". `opponent` is a
 * player we take on on purpose: it doesn't count as a threat for the send cap
 * and troops already attacking it count toward the too-weak check. */
static double attack_troops(AiCtx *c, PlayerId target, double fixed, PlayerId opponent) {
    const OfeGame *g = c->g;
    double reserve = ai_reserve_troops(c, target);
    bool is_bot = target != 0 && g->players.type[target] == OFE_PLAYER_BOT;
    double troops;
    if (is_bot) troops = bot_attack_troops(c, target, my_troops(c) - reserve - c->bot_troops_sent);
    else troops = isnan(fixed) ? my_troops(c) - reserve : fixed;
    troops = OFE_MIN(troops, target != 0 ? troop_send_cap(c, opponent) : expansion_troop_cap(c));
    if (troops < 1) return 0;
    double pile_on = (target != 0 && target == opponent) ? ai_incoming_on(g, target) : 0;
    if (target != 0 && attack_too_weak(c, troops + pile_on, target)) return 0;
    if (is_bot) c->bot_troops_sent += troops;
    return troops;
}

static bool send_land_attack(AiCtx *c, PlayerId target, double fixed, PlayerId opponent) {
    double troops = attack_troops(c, target, fixed, opponent);
    return troops >= 1 && ai_issue_attack(c, target, troops);
}

/* Hard and Impossible land a 1% boat and attack by land as soon as it lands
 * (ai_nation_follow_up_landings); this checks the land attack is worth it. */
static bool lands_beachheads(const AiCtx *c) { return hard_or_impossible(c); }

static bool send_beachhead(AiCtx *c, PlayerId target, TileRef landing) {
    if (attack_troops(c, target, NAN, target) < 1) return false;
    double boat = OFE_MAX(1.0, floor(my_troops(c) / 100.0));
    return ai_issue_boat(c, landing, boat);
}

static bool send_boat_attack(AiCtx *c, PlayerId target) {
    TileRef landing;
    int32_t len;
    if (!ai_boat_route(c, target, &landing, &len)) return false;
    if (lands_beachheads(c)) return send_beachhead(c, target, landing);
    double troops = attack_troops(c, target, my_troops(c) / 5.0, 0);
    return troops >= 1 && ai_issue_boat(c, landing, troops);
}

static bool send_expansion_boat(AiCtx *c) {
    TileRef landing = ai_find_free_land_landing(c);
    if (landing < 0) return false;
    if (lands_beachheads(c)) return send_beachhead(c, 0, landing);
    double troops = OFE_MIN(my_troops(c) / 5.0, expansion_troop_cap(c));
    return troops >= 1 && ai_issue_boat(c, landing, troops);
}

/* Easy and Medium nations rarely attack humans (the agents). */
bool ai_nation_should_attack(AiCtx *c, PlayerId other) {
    const OfeGame *g = c->g;
    if (other == 0 || g->players.type[other] != OFE_PLAYER_HUMAN || player_is_traitor(g, other)) return true;
    if (c->diff == OFE_DIFFICULTY_EASY && rng_int(c->rng, 0, 4) != 0) return false;
    if (c->diff == OFE_DIFFICULTY_MEDIUM && rng_chance(c->rng, 4)) return false;
    return true;
}

static bool send_attack(AiCtx *c, PlayerId target, bool force) {
    if (!force && !ai_nation_should_attack(c, target)) return false;
    if (target != 0) {
        if (ai_borders_by_land(c, target)) return send_land_attack(c, target, NAN, 0);
        return send_boat_attack(c, target);
    }
    if (c->nbi.tn_land) return send_land_attack(c, 0, NAN, 0);
    return send_expansion_boat(c);
}

/* ---- Reachability and boats --------------------------------------------------------------------- */

static int32_t max_boat_route(const AiCtx *c) {
    return c->diff == OFE_DIFFICULTY_HARD ? 300 : c->diff == OFE_DIFFICULTY_IMPOSSIBLE ? 500 : INT32_MAX;
}

/* By land, or by a boat that isn't sailing too far. */
static bool can_reach(AiCtx *c, PlayerId target) {
    if (ai_borders_by_land(c, target)) return true;
    TileRef landing;
    int32_t len;
    return ai_boat_route(c, target, &landing, &len) && len <= max_boat_route(c);
}

static bool find_random_boat_target(AiCtx *c, TileRef from, const PlayerId *enemies, int32_t ne, bool high_interest, TileRef *out) {
    OfeGame *g = c->g;
    const OfeMap *m = g->map;
    PlayerId unreachable[16];
    int32_t nu = 0, calls = 0;
    int32_t x = map_x(m, from), y = map_y(m, from);
    for (int i = 0; i < 48; i++) {
        int32_t rx = rng_int(c->rng, x - 150, x + 150), ry = rng_int(c->rng, y - 150, y + 150);
        if (!map_in_bounds(m, rx, ry)) continue;
        TileRef t = map_ref(m, rx, ry);
        if (!map_is_ownable(m, t)) continue;
        PlayerId o = tile_owner(g, t);
        if (o == c->p) continue;
        bool skip = false;
        for (int32_t k = 0; k < nu && !skip; k++) skip = unreachable[k] == o;
        for (int32_t k = 0; k < ne && !skip; k++) skip = enemies[k] == o; /* bordering players: boats there look stupid */
        if (skip) continue;
        if (o != 0 && is_ffa(g) && troops_of(g, o) > my_troops(c)) continue;
        bool matches = high_interest ? (o == 0 || g->players.type[o] == OFE_PLAYER_BOT) : (o == 0 || !player_friendly(g, c->p, o));
        if (!matches) continue;
        if (calls++ >= 6) break;
        if (ships_transport_source(g, c->p, t) < 0) {
            if (o != 0 && nu < 16) unreachable[nu++] = o;
            continue;
        }
        *out = t;
        return true;
    }
    return false;
}

static void attack_with_random_boat(AiCtx *c, const PlayerId *enemies, int32_t ne) {
    OfeGame *g = c->g;
    const IntVec *b = &g->players.border[c->p];
    if (!ai_boats_available(c) || c->nbi.shore_tiles == 0 || b->len == 0) return;
    TileRef src = -1;
    for (int i = 0; i < 30 && src < 0; i++) {
        TileRef t = b->data[rng_int(c->rng, 0, b->len)];
        if (map_is_shore(g->map, t)) src = t;
    }
    if (src < 0) return;
    TileRef dst;
    /* High-interest targets first (free or tribe land), then anyone. */
    if (!find_random_boat_target(c, src, enemies, ne, true, &dst) && !find_random_boat_target(c, src, enemies, ne, false, &dst)) return;
    PlayerId owner = tile_owner(g, dst);
    if (lands_beachheads(c)) {
        send_beachhead(c, owner, dst);
        return;
    }
    double cap = owner != 0 ? troop_send_cap(c, 0) : expansion_troop_cap(c);
    double troops = OFE_MIN(my_troops(c) / 5.0, cap);
    if (troops < 1 || (owner != 0 && attack_too_weak(c, troops, owner))) return;
    ai_issue_boat(c, dst, troops);
}

/* Under attack, Hard and Impossible keep their troops home; Medium half the time. */
static bool holds_boats_under_attack(AiCtx *c) {
    if (ai_find_incoming_attacker(c, true) == 0) return false;
    if (c->diff == OFE_DIFFICULTY_EASY) return false;
    if (c->diff == OFE_DIFFICULTY_MEDIUM) return rng_chance(c->rng, 2);
    return true;
}

/* ---- Target lists -------------------------------------------------------------------------------- */

static void sort_by_troops(const OfeGame *g, PlayerId *a, int32_t n) {
    for (int32_t i = 1; i < n; i++) { /* stable insertion sort: ties keep discovery order */
        PlayerId x = a[i];
        int32_t j = i - 1;
        while (j >= 0 && g->players.troops[a[j]] > g->players.troops[x]) {
            a[j + 1] = a[j];
            j--;
        }
        a[j + 1] = x;
    }
}

static void build_bordering(const AiCtx *c, Bordering *bd) {
    const OfeGame *g = c->g;
    const IntVec *nb = &c->ai->main.nb;
    bd->n = bd->nf = bd->ne = 0;
    for (int32_t i = 0; i < nb->len && bd->n < MAX_BORDER; i++) bd->all[bd->n++] = nb->data[i];
    sort_by_troops(g, bd->all, bd->n);
    for (int32_t i = 0; i < bd->n; i++) {
        if (player_friendly(g, c->p, bd->all[i])) bd->friends[bd->nf++] = bd->all[i];
        else bd->enemies[bd->ne++] = bd->all[i];
    }
}

static bool in_list(const PlayerId *a, int32_t n, PlayerId x) {
    for (int32_t i = 0; i < n; i++) {
        if (a[i] == x) return true;
    }
    return false;
}

/* Enemies across the water whose main territory is within boat range. */
static int32_t overseas_targets(AiCtx *c, const Bordering *bd, PlayerId *out, int32_t cap) {
    OfeGame *g = c->g;
    if (!ai_boats_available(c) || c->nbi.minx > c->nbi.maxx) return 0;
    int32_t range = max_boat_route(c), n = 0;
    for (PlayerId q = 1; q < g->players.count && n < cap; q++) {
        if (q == c->p || g->players.type[q] == OFE_PLAYER_BOT || g->players.num_tiles[q] <= 0) continue;
        if (player_friendly(g, c->p, q) || in_list(bd->enemies, bd->ne, q)) continue;
        if (is_ffa(g) && troops_of(g, q) >= my_troops(c)) continue;
        const AiBox *b = ai_player_box(g, c->ai, q);
        int32_t dx = OFE_MAX(0, OFE_MAX(c->nbi.minx - b->maxx, b->minx - c->nbi.maxx));
        int32_t dy = OFE_MAX(0, OFE_MAX(c->nbi.miny - b->maxy, b->miny - c->nbi.maxy));
        if (dx + dy <= range) out[n++] = q;
    }
    sort_by_troops(g, out, n);
    return n;
}

typedef struct Groups {
    PlayerId list[2][MAX_BORDER];
    int32_t n[2];
    int32_t count;
    bool ready;
} Groups;

/* Target groups tried in order, each sorted by troops. Hard and Impossible
 * without a land front weigh every enemy within boat range alongside the
 * bordering ones; Impossible with a land front turns to them when it finds
 * nothing there. */
static void build_groups(AiCtx *c, const Bordering *bd, Groups *gr) {
    if (gr->ready) return;
    gr->ready = true;
    bool land_front = false;
    for (int32_t i = 0; i < bd->ne; i++) land_front |= ai_borders_by_land(c, bd->enemies[i]);
    memcpy(gr->list[0], bd->enemies, (size_t)bd->ne * sizeof(PlayerId));
    gr->n[0] = bd->ne;
    gr->count = 1;
    if (c->diff == OFE_DIFFICULTY_IMPOSSIBLE && land_front) {
        gr->n[1] = overseas_targets(c, bd, gr->list[1], MAX_BORDER);
        gr->count = 2;
    } else if (hard_or_impossible(c) && !land_front) {
        PlayerId extra[MAX_BORDER];
        int32_t ne = overseas_targets(c, bd, extra, MAX_BORDER - bd->ne);
        for (int32_t i = 0; i < ne; i++) gr->list[0][gr->n[0]++] = extra[i];
        sort_by_troops(c->g, gr->list[0], gr->n[0]);
    }
}

/* What `find` picks among targets we can reach right now, from the first group where it picks anything. */
static PlayerId pick(AiCtx *c, const Bordering *bd, Groups *gr, FindFn find) {
    build_groups(c, bd, gr);
    for (int32_t k = 0; k < gr->count; k++) {
        PlayerId pool[MAX_BORDER];
        int32_t n = gr->n[k];
        memcpy(pool, gr->list[k], (size_t)n * sizeof(PlayerId));
        for (;;) {
            PlayerId t = find(c, pool, n);
            if (!t) break;
            if (c->diff == OFE_DIFFICULTY_EASY || can_reach(c, t)) return t;
            int32_t w = 0;
            for (int32_t i = 0; i < n; i++) {
                if (pool[i] != t) pool[w++] = pool[i];
            }
            n = w;
        }
    }
    return 0;
}

/* Finders: pools are sorted by troops, so the first match is the weakest. */
static PlayerId find_traitor(AiCtx *c, const PlayerId *l, int32_t n) {
    const OfeGame *g = c->g;
    if (g->cfg.disable_alliances) return 0;
    for (int32_t i = 0; i < n; i++) {
        if (player_is_traitor(g, l[i]) && (!is_ffa(g) || troops_of(g, l[i]) < my_troops(c) * 1.2)) return l[i];
    }
    return 0;
}

static PlayerId find_afk(AiCtx *c, const PlayerId *l, int32_t n) {
    const OfeGame *g = c->g;
    for (int32_t i = 0; i < n; i++) {
        if (g->players.disconnected[l[i]] && (!is_ffa(g) || troops_of(g, l[i]) < my_troops(c) * 3)) return l[i];
    }
    return 0;
}

/* Someone not much stronger than us who is under heavy attack by others. */
static PlayerId find_victim(AiCtx *c, const PlayerId *l, int32_t n) {
    const OfeGame *g = c->g;
    for (int32_t i = 0; i < n; i++) {
        if (is_ffa(g) && troops_of(g, l[i]) > my_troops(c) * 1.2) continue;
        if (ai_incoming_on(g, l[i]) > troops_of(g, l[i]) * 0.5) return l[i];
    }
    return 0;
}

/* Under 15% of their max troops (e.g. just MIRVed) and not much stronger than us. */
static PlayerId find_very_weak(AiCtx *c, const PlayerId *l, int32_t n) {
    const OfeGame *g = c->g;
    for (int32_t i = 0; i < n; i++) {
        if (troops_of(g, l[i]) < player_max_troops(g, l[i]) * 0.15 && (!is_ffa(g) || troops_of(g, l[i]) < my_troops(c) * 1.2)) return l[i];
    }
    return 0;
}

/* The juiciest enemy we could plausibly beat (troops <= 75% of ours). */
static PlayerId find_juicy(AiCtx *c, const PlayerId *l, int32_t n) {
    PlayerId cand[MAX_BORDER];
    int32_t k = 0;
    for (int32_t i = 0; i < n; i++) {
        if (troops_of(c->g, l[i]) <= my_troops(c) * 0.75) cand[k++] = l[i];
    }
    return juiciest_target(c->g, cand, k);
}

static PlayerId find_weakest(AiCtx *c, const PlayerId *l, int32_t n) {
    for (int32_t i = 0; i < n; i++) {
        if (!is_ffa(c->g) || troops_of(c->g, l[i]) < my_troops(c)) return l[i];
    }
    return 0;
}

static PlayerId find_first(AiCtx *c, const PlayerId *l, int32_t n) { return n > 0 ? l[0] : 0; }

/* ---- Strategies ------------------------------------------------------------------------------------- */

/* Cancel the attacker's incoming troops; if we can also match its home army
 * while keeping back what the send cap wants for every other neighbour, push
 * into its land with at least that much. Never dips below the expand ratio. */
static bool send_retaliation(AiCtx *c, PlayerId attacker) {
    const OfeGame *g = c->g;
    const int32_t *st = g->players.ai_state[c->p];
    double incoming = ai_incoming_from(c, attacker), pressing = 0;
    for (int32_t i = 0; i < c->fights.n_out; i++) {
        if (c->fights.out[i].who == attacker) pressing += c->fights.out[i].troops;
    }
    double troops = my_troops(c), max = player_max_troops(g, c->p);
    double spare = OFE_MIN(troops - max * st[AI_EXPAND] / 100.0, neighbor_troop_cap(c, attacker));
    double above_reserve = troops - max * st[AI_RESERVE] / 100.0;
    double counter = incoming + OFE_MAX(0.0, troops_of(g, attacker) - pressing);
    double send = spare >= counter ? OFE_MAX(counter, OFE_MIN(above_reserve, spare)) : OFE_MIN(incoming, OFE_MAX(spare, above_reserve));
    return send_land_attack(c, attacker, send, attacker);
}

static bool retaliate(AiCtx *c) {
    PlayerId attacker = ai_find_incoming_attacker(c, true);
    if (!attacker) return false;
    if (hard_or_impossible(c) && is_ffa(c->g) && ai_borders_by_land(c, attacker)) return send_retaliation(c, attacker);
    return send_attack(c, attacker, true);
}

static bool neighboring_bot_with_structures(const AiCtx *c) {
    const OfeGame *g = c->g;
    const IntVec *nb = &c->ai->main.nb;
    for (int32_t i = 0; i < nb->len; i++) {
        PlayerId q = nb->data[i];
        if (g->players.type[q] == OFE_PLAYER_BOT && !player_friendly(g, c->p, q) && ai_player_has_structures(g, q)) return true;
    }
    return false;
}

static int32_t bot_parallelism(AiCtx *c) {
    switch (c->diff) {
    case OFE_DIFFICULTY_EASY: return 1;
    case OFE_DIFFICULTY_MEDIUM: return rng_chance(c->rng, 2) ? 1 : 2;
    case OFE_DIFFICULTY_HARD: return 3;
    default: return 100;
    }
}

/* Neighbouring tribes, least dense first, those holding structures (maybe
 * ours, stolen) before the rest; several at once. */
static bool attack_bots(AiCtx *c) {
    OfeGame *g = c->g;
    const IntVec *nb = &c->ai->main.nb;
    PlayerId bots[MAX_BORDER];
    int32_t n = 0;
    for (int32_t i = 0; i < nb->len && n < MAX_BORDER; i++) {
        PlayerId q = nb->data[i];
        if (g->players.type[q] == OFE_PLAYER_BOT && !player_friendly(g, c->p, q)) bots[n++] = q;
    }
    if (n == 0) return false;
    c->bot_troops_sent = 0;

    double key[MAX_BORDER];
    for (int32_t i = 0; i < n; i++) {
        double density = troops_of(g, bots[i]) / OFE_MAX(1, g->players.num_tiles[bots[i]]);
        key[i] = density - (ai_player_has_structures(g, bots[i]) ? 1e18 : 0.0);
    }
    for (int32_t i = 1; i < n; i++) {
        PlayerId x = bots[i];
        double kx = key[i];
        int32_t j = i - 1;
        while (j >= 0 && key[j] > kx) {
            bots[j + 1] = bots[j];
            key[j + 1] = key[j];
            j--;
        }
        bots[j + 1] = x;
        key[j + 1] = kx;
    }
    int32_t limit = OFE_MIN(n, bot_parallelism(c));
    for (int32_t i = 0; i < limit; i++) send_attack(c, bots[i], false);
    if (c->bot_troops_sent > 0) return true;
    if (hard_or_impossible(c)) { /* only top-ups: running attacks count */
        for (int32_t i = 0; i < limit; i++) {
            for (int32_t k = 0; k < c->fights.n_out; k++) {
                if (c->fights.out[k].who == bots[i]) return true;
            }
        }
    }
    return false;
}

/* Join an ally's target, if we like the ally enough. */
static bool assist_allies(AiCtx *c) {
    OfeGame *g = c->g;
    if (g->cfg.disable_alliances) return false;
    for (int32_t i = 0; i < g->alliances.len; i++) {
        const Alliance al = g->alliances.data[i];
        if (al.a != c->p && al.b != c->p) continue;
        PlayerId ally = al.a == c->p ? al.b : al.a;
        PlayerId target = g->players.target[ally];
        if (target <= 0 || target == c->p || !player_alive(g, target)) continue;
        if (g->players.last_target_tick[ally] < 0 || g->tick - g->players.last_target_tick[ally] > ALLY_TARGET_TICKS) continue;
        if (ai_relation(g, c->p, ally) < REL_FRIENDLY) continue;
        if (player_friendly(g, c->p, target)) continue;
        if (!send_attack(c, target, false)) continue;
        player_update_relation(g, c->p, ally, -20);
        return true;
    }
    return false;
}

static bool betray_and_attack(AiCtx *c, const Bordering *bd) {
    if (c->g->cfg.disable_alliances || bd->nf == 0) return false;
    PlayerId allies[MAX_BORDER];
    int32_t n = 0;
    for (int32_t i = 0; i < bd->nf; i++) {
        if (player_allied(c->g, c->p, bd->friends[i])) allies[n++] = bd->friends[i];
    }
    PlayerId juiciest = juiciest_target(c->g, allies, n);
    for (int32_t i = 0; i < bd->nf; i++) {
        if (maybe_betray(c, bd, bd->friends[i], juiciest)) return send_attack(c, bd->friends[i], true);
    }
    return false;
}

/* The most hated nation we can reach (relation "hostile"). */
static bool attack_hated(AiCtx *c) {
    OfeGame *g = c->g;
    if (g->players.relation_row[c->p] < 0) return false;
    PlayerId cand[MAX_BORDER];
    float val[MAX_BORDER];
    int32_t n = 0;
    for (PlayerId q = 1; q < g->players.count && n < MAX_BORDER; q++) {
        if (q == c->p || g->players.num_tiles[q] <= 0) continue;
        float r = (float)player_relation(g, c->p, q);
        if (r >= -50) continue;
        int32_t j = n++;
        while (j > 0 && val[j - 1] > r) { /* ascending: most hated first */
            cand[j] = cand[j - 1];
            val[j] = val[j - 1];
            j--;
        }
        cand[j] = q;
        val[j] = r;
    }
    for (int32_t i = 0; i < n; i++) {
        PlayerId q = cand[i];
        if (player_friendly(g, c->p, q)) continue;
        if (is_ffa(g) && troops_of(g, q) > my_troops(c) * 3) continue;
        if (c->diff != OFE_DIFFICULTY_EASY && !can_reach(c, q)) continue;
        return send_attack(c, q, false);
    }
    return false;
}

/* The nearest enemy across the water, for nations with no land front. */
static PlayerId nearest_island_enemy(AiCtx *c) {
    OfeGame *g = c->g;
    if (!ai_boats_available(c) || c->nbi.shore_tiles == 0 || c->nbi.minx > c->nbi.maxx) return 0;
    int32_t cx = (c->nbi.minx + c->nbi.maxx) / 2, cy = (c->nbi.miny + c->nbi.maxy) / 2;
    PlayerId best[6];
    int32_t dist[6], nb = 0;
    for (PlayerId q = 1; q < g->players.count; q++) {
        if (q == c->p || g->players.num_tiles[q] <= 0 || player_friendly(g, c->p, q)) continue;
        if (is_ffa(g) && troops_of(g, q) >= my_troops(c)) continue;
        const AiBox *b = ai_player_box(g, c->ai, q);
        int32_t d = abs((b->minx + b->maxx) / 2 - cx) + abs((b->miny + b->maxy) / 2 - cy);
        if (nb == 6 && d >= dist[5]) continue;
        int32_t j = nb < 6 ? nb++ : 5;
        while (j > 0 && dist[j - 1] > d) {
            best[j] = best[j - 1];
            dist[j] = dist[j - 1];
            j--;
        }
        best[j] = q;
        dist[j] = d;
    }
    PlayerId reachable[2];
    int32_t nr = 0;
    for (int32_t i = 0; i < nb && nr < 2; i++) {
        bool ok;
        if (hard_or_impossible(c)) { /* beachhead boats sail any distance */
            TileRef landing;
            int32_t len;
            ok = ai_borders_by_land(c, best[i]) || ai_boat_route(c, best[i], &landing, &len);
        } else {
            ok = can_reach(c, best[i]);
        }
        if (ok) reachable[nr++] = best[i];
    }
    if (nr == 0) return 0;
    if (nr == 2 && rng_chance(c->rng, 3)) return reachable[1];
    return reachable[0];
}

/* Team games: send troops to the teammate who is fighting and has the fewest. */
static bool donate_troops(AiCtx *c) {
    OfeGame *g = c->g;
    PlayerTable *pl = &g->players;
    if (g->cfg.game_mode != OFE_MODE_TEAM || !g->cfg.donate_troops) return false;
    if (c->diff == OFE_DIFFICULTY_EASY) return false;
    if (c->diff == OFE_DIFFICULTY_MEDIUM && !rng_chance(c->rng, 4)) return false;
    if (c->diff == OFE_DIFFICULTY_HARD && !rng_chance(c->rng, 2)) return false;
    if (pl->last_donate_tick[c->p] >= 0 && g->tick - pl->last_donate_tick[c->p] < 100) return false;

    IntVec *busy = &c->ai->tmp; /* 1 = has an attack going */
    busy->len = 0;
    IntVec_reserve(busy, pl->count);
    memset(busy->data, 0, (size_t)pl->count * sizeof(int32_t));
    busy->len = pl->count;
    const AttackTable *t = &g->attacks;
    for (int32_t i = 0; i < t->count; i++) {
        if (!t->active[i]) continue;
        busy->data[t->attacker[i]] = 1;
        busy->data[t->target[i]] = 1;
    }
    PlayerId best = 0;
    double best_pct = DBL_MAX;
    for (PlayerId q = 1; q < pl->count; q++) {
        if (q == c->p || !busy->data[q] || pl->num_tiles[q] <= 0 || !player_same_team(g, c->p, q)) continue;
        double pct = troops_of(g, q) / OFE_MAX(1.0, player_max_troops(g, q));
        if (pct < best_pct) {
            best_pct = pct;
            best = q;
        }
    }
    if (!best) return false;
    double available = my_troops(c) - player_max_troops(g, c->p) * pl->ai_state[c->p][AI_RESERVE] / 100.0;
    if (available < 1) return false;
    OfeIntent in = {0};
    in.type = OFE_INTENT_DONATE_TROOPS;
    in.player = c->p;
    in.target = best;
    in.troops = available;
    game_issue(g, &in);
    return true;
}

/* A bordering runaway leader. Hard and Impossible always consider it (the
 * too-weak check decides); Medium only joins attacks already under way. */
static bool attack_crown(AiCtx *c, const Bordering *bd) {
    OfeGame *g = c->g;
    PlayerId leader = ai_runaway_leader(g, c->ai);
    if (!leader || leader == c->p || !in_list(bd->enemies, bd->ne, leader) || !ai_borders_by_land(c, leader)) return false;
    if (!hard_or_impossible(c) && ai_incoming_on(g, leader) < troops_of(g, leader) * 0.1) return false;
    return ai_nation_should_attack(c, leader) && send_land_attack(c, leader, NAN, leader);
}

enum Strategy {
    S_END = 0, S_RETALIATE, S_BOTS, S_ASSIST, S_TRAITOR, S_AFK, S_BETRAY, S_NUKED, S_VICTIM, S_JUICY, S_HATED, S_VERY_WEAK,
    S_WEAKEST, S_ISLAND, S_DONATE, S_CROWN
};

/* Easy gets the dumbest order, Impossible the smartest. Medium and up
 * retaliate earlier, in attack_best_target. */
static const uint8_t STRATEGIES[4][16] = {
    {S_NUKED, S_BOTS, S_RETALIATE, S_ASSIST, S_BETRAY, S_HATED, S_WEAKEST, S_END},
    {S_BOTS, S_NUKED, S_ASSIST, S_BETRAY, S_HATED, S_AFK, S_TRAITOR, S_CROWN, S_WEAKEST, S_ISLAND, S_DONATE, S_END},
    {S_BOTS, S_ASSIST, S_BETRAY, S_NUKED, S_TRAITOR, S_AFK, S_HATED, S_VERY_WEAK, S_JUICY, S_VICTIM, S_CROWN, S_WEAKEST, S_ISLAND,
     S_DONATE, S_END},
    {S_BOTS, S_VERY_WEAK, S_BETRAY, S_ASSIST, S_VICTIM, S_CROWN, S_TRAITOR, S_JUICY, S_AFK, S_NUKED, S_HATED, S_WEAKEST, S_ISLAND,
     S_DONATE, S_END},
};

static bool run_strategy(AiCtx *c, const Bordering *bd, Groups *gr, int strategy) {
    PlayerId t = 0;
    switch (strategy) {
    case S_RETALIATE: return retaliate(c);
    case S_BOTS: return attack_bots(c);
    case S_ASSIST: return assist_allies(c);
    case S_BETRAY: return betray_and_attack(c, bd);
    case S_HATED: return attack_hated(c);
    case S_DONATE: return donate_troops(c);
    case S_CROWN: return attack_crown(c, bd);
    case S_NUKED:
        /* Nuked land next to us is worth taking (only where nukes exist). */
        if (c->g->cfg.disabled_units & (1u << OFE_UNIT_MISSILE_SILO)) return false;
        return c->nbi.tn_fallout && send_attack(c, 0, false);
    case S_ISLAND:
        if (pick(c, bd, gr, find_first)) return false;
        t = nearest_island_enemy(c);
        return t && send_attack(c, t, false);
    case S_TRAITOR: t = pick(c, bd, gr, find_traitor); break;
    case S_AFK: t = pick(c, bd, gr, find_afk); break;
    case S_VICTIM: t = pick(c, bd, gr, find_victim); break;
    case S_JUICY: t = pick(c, bd, gr, find_juicy); break;
    case S_VERY_WEAK: t = pick(c, bd, gr, find_very_weak); break;
    case S_WEAKEST: t = pick(c, bd, gr, find_weakest); break;
    default: return false;
    }
    return t && send_attack(c, t, false);
}

static bool has_reserve_ratio_troops(const AiCtx *c) {
    return ai_has_ratio_troops(c, c->g->players.ai_state[c->p][AI_RESERVE], false);
}

static void attack_best_target(AiCtx *c, const Bordering *bd) {
    const int32_t *st = c->g->players.ai_state[c->p];
    /* Hard and Impossible answer attacks right away, below the reserve and trigger ratios. */
    if (hard_or_impossible(c) && retaliate(c)) return;

    /* Tribes holding structures (we need those back) are attacked before the ratio checks. */
    if (neighboring_bot_with_structures(c) || (ai_high_start_gold(c->g) && ai_has_ratio_troops(c, st[AI_TRIGGER], true))) {
        if (attack_bots(c)) return;
    }
    if (!has_reserve_ratio_troops(c)) return;
    if (c->diff == OFE_DIFFICULTY_MEDIUM && retaliate(c)) return;
    if (!ai_has_ratio_troops(c, st[AI_TRIGGER], false) && !rng_chance(c->rng, 10)) return;

    Groups gr;
    gr.ready = false;
    gr.count = 0;
    for (const uint8_t *s = STRATEGIES[c->diff]; *s != S_END; s++) {
        if (run_strategy(c, bd, &gr, *s)) return;
    }
}

/* ---- The attack decision ------------------------------------------------------------------------------- */

static void choose_attack(AiCtx *c, Bordering *bd) {
    build_bordering(c, bd);
    /* Free land first, but not nuked land. */
    if (c->nbi.tn_clean || c->nbi.tn_shore) {
        if (send_attack(c, 0, false)) return;
    }
    bool hold_boats = holds_boats_under_attack(c);
    if (bd->ne == 0) {
        if (!hold_boats && rng_chance(c->rng, 5)) attack_with_random_boat(c, bd->enemies, 0);
    } else {
        if (!hold_boats && rng_chance(c->rng, 10)) {
            attack_with_random_boat(c, bd->enemies, bd->ne);
            return;
        }
        maybe_send_alliance_requests(c, bd);
    }
    attack_best_target(c, bd);
}

/* ---- Embargoes ------------------------------------------------------------------------------------------- */

/* Hostile players get an embargo, kept until relations recover (Impossible
 * never lifts it, Hard only once friendly). Higher difficulties in team games
 * refuse to trade with other teams at all. */
static void handle_embargoes(AiCtx *c) {
    OfeGame *g = c->g;
    PlayerId p = c->p;
    bool higher = c->diff >= OFE_DIFFICULTY_HARD;
    bool team_game = g->cfg.game_mode == OFE_MODE_TEAM;
    for (PlayerId q = 1; q < g->players.count; q++) {
        if (q == p || g->players.num_tiles[q] <= 0) continue;
        if (team_game && higher && g->players.type[q] != OFE_PLAYER_BOT && !player_same_team(g, p, q)) {
            if (!player_embargoed(g, p, q)) {
                OfeIntent in = {0};
                in.type = OFE_INTENT_EMBARGO;
                in.player = p;
                in.target = q;
                in.flags = 1;
                game_issue(g, &in);
            }
            continue;
        }
        double r = player_relation(g, p, q);
        if (r <= -50 && !player_same_team(g, p, q) && !player_embargoed(g, p, q)) {
            OfeIntent in = {0};
            in.type = OFE_INTENT_EMBARGO;
            in.player = p;
            in.target = q;
            in.flags = 1;
            game_issue(g, &in);
        }
    }
    /* Lifting: walk our own embargoes (a short list). */
    PlayerId lift[64];
    int32_t n = 0;
    for (int32_t i = 0; i < g->embargoes.len && n < 64; i++) {
        const Embargo *e = &g->embargoes.data[i];
        if (e->from != p || e->temporary) continue;
        int rel = ai_relation(g, p, e->to);
        if ((rel >= REL_NEUTRAL && c->diff != OFE_DIFFICULTY_HARD && c->diff != OFE_DIFFICULTY_IMPOSSIBLE) ||
            (rel >= REL_FRIENDLY && c->diff != OFE_DIFFICULTY_IMPOSSIBLE)) {
            lift[n++] = e->to;
        }
    }
    for (int32_t i = 0; i < n; i++) {
        OfeIntent in = {0};
        in.type = OFE_INTENT_EMBARGO;
        in.player = p;
        in.target = lift[i];
        in.flags = 0;
        game_issue(g, &in);
    }
}

/* ---- Beachhead follow-up ----------------------------------------------------------------------------------- */

static bool holds_land_next_to(const OfeGame *g, PlayerId owner, TileRef tile) {
    TileRef nb[4];
    int n = map_neighbors4(g->map, tile, nb);
    for (int i = 0; i < n; i++) {
        if (map_is_ownable(g->map, nb[i]) && tile_owner(g, nb[i]) == owner) return true;
    }
    return false;
}

/* Hard and Impossible boats are beachheads: the moment one lands (its attack
 * starts at the landing tile) the nation attacks its target by land from
 * there, once. Attack ids only grow, so one pass over the new ones is enough. */
void ai_nation_follow_up_landings(OfeGame *g) {
    AiSysState *ai = ai_sys(g);
    const AttackTable *t = &g->attacks;
    int32_t seen = ai->seen_attack_id;
    for (int32_t i = 0; i < t->count; i++) {
        if (!t->active[i] || t->id[i] <= seen || t->source_tile[i] < 0) continue;
        PlayerId p = t->attacker[i], target = t->target[i];
        if (g->players.type[p] == OFE_PLAYER_BOT || (g->players.type[p] == OFE_PLAYER_HUMAN && !g->cfg.humans_use_ai)) continue;
        if (!holds_land_next_to(g, target, t->source_tile[i])) continue;
        AiCtx c;
        ai_ctx_begin(&c, g, p);
        ai_ctx_scan(&c);
        send_land_attack(&c, target, NAN, target);
    }
    ai->seen_attack_id = g->next_attack_id;
}

/* ---- Entry points ------------------------------------------------------------------------------------------------ */

static void schedule_next(OfeGame *g, PlayerId p) {
    PlayerTable *pl = &g->players;
    int32_t *st = pl->ai_state[p];
    int32_t third = st[AI_RATE] / 3;
    if (st[AI_PHASE] == 2) {
        pl->ai_next_tick[p] = g->tick + st[AI_RATE] - 2 * third;
        st[AI_PHASE] = 0;
    } else {
        pl->ai_next_tick[p] = g->tick + third;
        st[AI_PHASE]++;
    }
}

void ai_nation_decide(OfeGame *g, PlayerId p) {
    int32_t *st = g->players.ai_state[p];
    schedule_next(g, p);

    AiCtx c;
    ai_ctx_begin(&c, g, p);
    if (!(st[AI_FLAGS] & AIF_FIRST_DONE)) {
        /* Opening: send half the troops at the free land around the spawn. */
        st[AI_FLAGS] |= AIF_FIRST_DONE;
        ai_issue_attack(&c, 0, my_troops(&c) / 2.0);
        return;
    }
    ai_ctx_scan(&c);
    handle_extension_requests(&c);
    ai_build_consider_mirv(&c);
    ai_build_handle_structures(&c);
    ai_build_maybe_warship(&c);
    handle_embargoes(&c);
    Bordering bd;
    choose_attack(&c, &bd);
    ai_build_maybe_nuke(&c);
}

void ai_nation_structures_only(OfeGame *g, PlayerId p) {
    schedule_next(g, p);
    if (!(g->players.ai_state[p][AI_FLAGS] & AIF_FIRST_DONE)) return;
    AiCtx c;
    ai_ctx_begin(&c, g, p);
    ai_build_handle_structures(&c);
}

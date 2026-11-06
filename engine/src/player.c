/*
 * player.c - per-player rules: troop and gold growth, relations, teams,
 * alliances, embargoes and donations.
 *
 * Formulas follow OpenFront's Config / EngineConfig:
 *   max troops   = 2 * (tiles^0.6 * 1000 + 50000) + 250000 * city levels
 *                  (bots / 3; nations x0.5 / 0.75 / 1 / 1.25 by difficulty)
 *   troop growth = (10 + troops^0.73 / 4) * (1 - troops / max)
 *                  (bots x0.5; nations x0.9 / 0.95 / 1 / 1.05)
 *   gold         = 100 per tick (bots 50) x gold multiplier
 */
#include "internal.h"

#define CITY_TROOP_INCREASE 250000.0
#define TRAITOR_DURATION_TICKS (30 * TICKS_PER_SECOND)
#define ALLIANCE_REQUEST_DURATION (20 * TICKS_PER_SECOND)
#define ALLIANCE_REQUEST_COOLDOWN (30 * TICKS_PER_SECOND)
#define DONATE_COOLDOWN (10 * TICKS_PER_SECOND)
#define TEMPORARY_EMBARGO_DURATION (300 * TICKS_PER_SECOND)
#define RELATION_DECAY 0.05f

/* ---- Identity and relations -------------------------------------------- */

bool player_is_traitor(const OfeGame *g, PlayerId p) {
    int32_t t = g->players.traitor_tick[p];
    return t >= 0 && g->tick - t < TRAITOR_DURATION_TICKS;
}

bool player_same_team(const OfeGame *g, PlayerId a, PlayerId b) {
    if (a == b) return true;
    int16_t ta = g->players.team[a];
    return ta >= 0 && ta == g->players.team[b];
}

int32_t alliance_find(const OfeGame *g, PlayerId a, PlayerId b) {
    for (int32_t i = 0; i < g->alliances.len; i++) {
        const Alliance *al = &g->alliances.data[i];
        if ((al->a == a && al->b == b) || (al->a == b && al->b == a)) return i;
    }
    return -1;
}

bool player_allied(const OfeGame *g, PlayerId a, PlayerId b) { return alliance_find(g, a, b) >= 0; }

bool player_friendly(const OfeGame *g, PlayerId self, PlayerId other) {
    if (self == other) return true;
    if (other <= 0) return false;
    if (g->players.disconnected[other]) return false;
    return player_same_team(g, self, other) || player_allied(g, self, other);
}

bool player_is_immune(const OfeGame *g, PlayerId p) {
    if (g->start_tick < 0) return true;
    int32_t since = g->tick - g->start_tick;
    int type = g->players.type[p];
    if (type == OFE_PLAYER_HUMAN) return since < g->cfg.spawn_immunity_ticks;
    if (type == OFE_PLAYER_NATION) return since < 50;
    return false;
}

bool player_can_attack_player(const OfeGame *g, PlayerId attacker, PlayerId target) {
    if (target == 0) return true;
    if (player_friendly(g, attacker, target)) return false;
    /* Only human attackers respect spawn immunity. */
    if (g->players.type[attacker] == OFE_PLAYER_HUMAN && player_is_immune(g, target)) return false;
    return true;
}

bool player_shares_border(const OfeGame *g, PlayerId a, PlayerId b) {
    const IntVec *bd = &g->players.border[a];
    for (int32_t i = 0; i < bd->len; i++) {
        if (tile_borders_player(g, bd->data[i], b)) return true;
    }
    return false;
}

double player_relation(const OfeGame *g, PlayerId self, PlayerId other) {
    int32_t row = g->players.relation_row[self];
    if (row < 0 || other <= 0) return 0.0;
    return g->relations[(size_t)row * g->players.cap + other];
}

void player_update_relation(OfeGame *g, PlayerId self, PlayerId other, double delta) {
    int32_t row = g->players.relation_row[self];
    if (row < 0 || other <= 0 || other == self) return;
    float *r = &g->relations[(size_t)row * g->players.cap + other];
    *r = (float)OFE_CLAMP(*r + delta, -100.0, 100.0);
}

static void decay_relations(OfeGame *g) {
    size_t n = (size_t)g->num_relation_rows * g->players.cap;
    for (size_t i = 0; i < n; i++) {
        float r = g->relations[i];
        if (r == 0.0f) continue;
        r += r > 0 ? -RELATION_DECAY : RELATION_DECAY;
        if (fabsf(r) < RELATION_DECAY * 2) r = 0.0f;
        g->relations[i] = r;
    }
}

/* ---- Troops and gold ----------------------------------------------------- */

static double difficulty_scale(const OfeGame *g, const double by_difficulty[4]) {
    return by_difficulty[OFE_CLAMP(g->cfg.difficulty, 0, 3)];
}

double player_max_troops(const OfeGame *g, PlayerId p) {
    const PlayerTable *pl = &g->players;
    int type = pl->type[p];
    double city = pl->unit_levels[p][OFE_UNIT_CITY] * CITY_TROOP_INCREASE;
    double base;
    if (type == OFE_PLAYER_HUMAN && g->cfg.infinite_troops) {
        base = 1e9;
    } else {
        base = 2.0 * (pow((double)pl->num_tiles[p], 0.6) * 1000.0 + 50000.0) + city;
    }
    if (type == OFE_PLAYER_BOT) return base / 3.0;
    if (type == OFE_PLAYER_HUMAN) return base;
    static const double nation_scale[4] = {0.5, 0.75, 1.0, 1.25};
    return base * difficulty_scale(g, nation_scale);
}

double player_troop_increase(const OfeGame *g, PlayerId p) {
    const PlayerTable *pl = &g->players;
    double max = player_max_troops(g, p);
    double troops = (double)pl->troops[p];
    double add = 10.0 + pow(troops, 0.73) / 4.0;
    add *= 1.0 - troops / max;
    if (pl->type[p] == OFE_PLAYER_BOT) add *= 0.5;
    if (pl->type[p] == OFE_PLAYER_NATION) {
        static const double nation_scale[4] = {0.9, 0.95, 1.0, 1.05};
        add *= difficulty_scale(g, nation_scale);
    }
    return OFE_MIN(troops + add, max) - troops;
}

int64_t player_gold_rate(const OfeGame *g, PlayerId p) {
    double base = g->players.type[p] == OFE_PLAYER_BOT ? 50.0 : 100.0;
    return (int64_t)floor(base * g->cfg.gold_multiplier);
}

void player_add_troops(OfeGame *g, PlayerId p, double troops) {
    if (troops < 0) {
        player_remove_troops(g, p, -troops);
        return;
    }
    g->players.troops[p] += (int64_t)floor(troops);
}

double player_remove_troops(OfeGame *g, PlayerId p, double troops) {
    if (troops <= 0) return 0;
    int64_t want = (int64_t)floor(troops);
    int64_t removed = OFE_MIN(g->players.troops[p], want);
    g->players.troops[p] -= removed;
    return (double)removed;
}

void player_add_gold(OfeGame *g, PlayerId p, int64_t gold) {
    g->players.gold[p] += gold;
    g->players.gold_earned[p] += gold;
}

int64_t player_remove_gold(OfeGame *g, PlayerId p, int64_t gold) {
    if (gold <= 0) return 0;
    int64_t removed = OFE_MIN(g->players.gold[p], gold);
    g->players.gold[p] -= removed;
    return removed;
}

/* ---- Embargoes ----------------------------------------------------------- */

static int32_t embargo_find(const OfeGame *g, PlayerId from, PlayerId to) {
    for (int32_t i = 0; i < g->embargoes.len; i++) {
        if (g->embargoes.data[i].from == from && g->embargoes.data[i].to == to) return i;
    }
    return -1;
}

bool player_embargoed(const OfeGame *g, PlayerId from, PlayerId to) { return embargo_find(g, from, to) >= 0; }

bool player_can_trade(const OfeGame *g, PlayerId a, PlayerId b) {
    return a != b && !player_embargoed(g, a, b) && !player_embargoed(g, b, a);
}

void player_add_embargo(OfeGame *g, PlayerId from, PlayerId to, bool temporary) {
    int32_t i = embargo_find(g, from, to);
    if (i >= 0) {
        /* A permanent embargo stays permanent. */
        if (!temporary) g->embargoes.data[i].temporary = 0;
        return;
    }
    Embargo e = {from, to, g->tick, (uint8_t)temporary};
    EmbargoVec_push(&g->embargoes, e);
    event_emit(g, OFE_EV_EMBARGO, 0, from, to, 1, 0);
}

void player_stop_embargo(OfeGame *g, PlayerId from, PlayerId to) {
    int32_t i = embargo_find(g, from, to);
    if (i < 0) return;
    g->embargoes.data[i] = g->embargoes.data[--g->embargoes.len];
    event_emit(g, OFE_EV_EMBARGO, 0, from, to, 0, 0);
}

static void expire_embargoes(OfeGame *g) {
    for (int32_t i = g->embargoes.len - 1; i >= 0; i--) {
        Embargo *e = &g->embargoes.data[i];
        if (e->temporary && g->tick - e->created_tick > TEMPORARY_EMBARGO_DURATION) {
            player_stop_embargo(g, e->from, e->to);
        }
    }
}

/* ---- Alliances ------------------------------------------------------------ */

static int32_t request_find(const OfeGame *g, PlayerId from, PlayerId to) {
    for (int32_t i = 0; i < g->alliance_requests.len; i++) {
        const AllianceRequest *r = &g->alliance_requests.data[i];
        if (r->requestor == from && r->recipient == to) return i;
    }
    return -1;
}

static void request_remove(OfeGame *g, int32_t i) {
    g->alliance_requests.data[i] = g->alliance_requests.data[--g->alliance_requests.len];
}

static int32_t alliance_duration(const OfeGame *g) {
    int32_t m = g->cfg.alliance_duration_minutes;
    return m > 0 ? m * 60 * TICKS_PER_SECOND : 300 * TICKS_PER_SECOND;
}

bool alliance_can_request(const OfeGame *g, PlayerId from, PlayerId to) {
    if (g->cfg.disable_alliances) return false;
    if (!player_valid(g, from) || !player_valid(g, to) || from == to) return false;
    if (g->players.disconnected[from] || g->players.disconnected[to]) return false;
    if (player_friendly(g, from, to) || !player_alive(g, from)) return false;
    if (request_find(g, from, to) >= 0) return false;
    if (request_find(g, to, from) >= 0) return true;
    /* Cooldown after an earlier request to the same player. */
    for (int32_t i = 0; i < g->past_requests.len; i++) {
        const AllianceRequest *r = &g->past_requests.data[i];
        if (r->requestor == from && r->recipient == to && g->tick - r->created_tick < ALLIANCE_REQUEST_COOLDOWN) return false;
    }
    return true;
}

void alliance_form(OfeGame *g, PlayerId a, PlayerId b, int32_t duration) {
    if (player_allied(g, a, b)) return;
    Alliance al = {a, b, g->tick, g->tick + (duration > 0 ? duration : alliance_duration(g)), 0, 0};
    AllianceVec_push(&g->alliances, al);
    /* New allies' nukes in flight at each other are called off. */
    nukes_cancel_between(g, a, b);
    /* Pending requests between the two are settled. */
    int32_t i;
    if ((i = request_find(g, a, b)) >= 0) request_remove(g, i);
    if ((i = request_find(g, b, a)) >= 0) request_remove(g, i);
    event_emit(g, OFE_EV_ALLIANCE_FORMED, 0, a, b, 0, 0);
}

void alliance_accept(OfeGame *g, PlayerId requestor, PlayerId recipient) {
    int32_t i = request_find(g, requestor, recipient);
    if (i < 0) return;
    request_remove(g, i);
    alliance_form(g, requestor, recipient, 0);
    player_update_relation(g, requestor, recipient, 100);
    player_update_relation(g, recipient, requestor, 100);
    /* Allies lift temporary embargoes on each other. */
    for (int32_t k = g->embargoes.len - 1; k >= 0; k--) {
        Embargo *e = &g->embargoes.data[k];
        if (e->temporary && ((e->from == requestor && e->to == recipient) || (e->from == recipient && e->to == requestor))) {
            player_stop_embargo(g, e->from, e->to);
        }
    }
}

void alliance_reject(OfeGame *g, PlayerId requestor, PlayerId recipient) {
    int32_t i = request_find(g, requestor, recipient);
    if (i < 0) return;
    request_remove(g, i);
    event_emit(g, OFE_EV_ALLIANCE_REJECTED, 0, requestor, recipient, 0, 0);
}

void alliance_request(OfeGame *g, PlayerId from, PlayerId to) {
    if (!alliance_can_request(g, from, to)) return;
    if (request_find(g, to, from) >= 0) {
        /* Both asked: accept at once. */
        alliance_accept(g, to, from);
        return;
    }
    AllianceRequest r = {from, to, g->tick};
    AllianceRequestVec_push(&g->alliance_requests, r);
    AllianceRequestVec_push(&g->past_requests, r);
    event_emit(g, OFE_EV_ALLIANCE_REQUEST, 0, from, to, 0, 0);
}

void alliance_break(OfeGame *g, PlayerId breaker, PlayerId other) {
    int32_t i = alliance_find(g, breaker, other);
    if (i < 0) return;
    if (!player_is_traitor(g, other) && !g->players.disconnected[other]) {
        g->players.traitor_tick[breaker] = g->tick;
        g->players.betrayals[breaker]++;
    }
    g->alliances.data[i] = g->alliances.data[--g->alliances.len];
    player_update_relation(g, other, breaker, -100);
    /* Neighbours of the traitor (not on the victim's team) trust it less. */
    for (PlayerId n = 1; n < g->players.count; n++) {
        if (n == breaker || n == other || !player_alive(g, n)) continue;
        if (player_same_team(g, n, other)) continue;
        if (g->players.relation_row[n] < 0) continue;
        if (player_shares_border(g, breaker, n)) player_update_relation(g, n, breaker, -40);
    }
    event_emit(g, OFE_EV_ALLIANCE_BROKEN, 0, breaker, other, 0, 0);
}

void alliance_request_extension(OfeGame *g, PlayerId from, PlayerId to) {
    int32_t i = alliance_find(g, from, to);
    if (i < 0 || !player_alive(g, from) || !player_alive(g, to)) return;
    Alliance *al = &g->alliances.data[i];
    if (al->a == from) al->extend_a = 1;
    else al->extend_b = 1;
    if (al->extend_a && al->extend_b) {
        al->expires_tick = g->tick + alliance_duration(g);
        al->extend_a = al->extend_b = 0;
    }
}

void alliances_tick(OfeGame *g) {
    /* Forget past requests once their cooldown is over. */
    for (int32_t i = g->past_requests.len - 1; i >= 0; i--) {
        if (g->tick - g->past_requests.data[i].created_tick >= ALLIANCE_REQUEST_COOLDOWN) {
            g->past_requests.data[i] = g->past_requests.data[--g->past_requests.len];
        }
    }
    /* Unanswered requests expire. */
    for (int32_t i = g->alliance_requests.len - 1; i >= 0; i--) {
        AllianceRequest *r = &g->alliance_requests.data[i];
        if (g->tick - r->created_tick > ALLIANCE_REQUEST_DURATION) {
            event_emit(g, OFE_EV_ALLIANCE_REJECTED, 0, r->requestor, r->recipient, 0, 0);
            request_remove(g, i);
        }
    }
    for (int32_t i = g->alliances.len - 1; i >= 0; i--) {
        Alliance *al = &g->alliances.data[i];
        if (!player_alive(g, al->a) || !player_alive(g, al->b)) {
            g->alliances.data[i] = g->alliances.data[--g->alliances.len];
            continue;
        }
        if (al->expires_tick <= g->tick) {
            event_emit(g, OFE_EV_ALLIANCE_EXPIRED, 0, al->a, al->b, 0, 0);
            g->alliances.data[i] = g->alliances.data[--g->alliances.len];
            continue;
        }
    }
}

/* ---- Donations ------------------------------------------------------------ */

static bool can_donate(const OfeGame *g, PlayerId from, PlayerId to, bool gold) {
    if (!player_valid(g, from) || !player_valid(g, to) || from == to) return false;
    if (!player_alive(g, from) || !player_alive(g, to) || !player_friendly(g, from, to)) return false;
    if (g->players.type[to] == OFE_PLAYER_HUMAN && !(gold ? g->cfg.donate_gold : g->cfg.donate_troops)) return false;
    /* Simplification: one cooldown per donor rather than per recipient. */
    if (g->players.last_donate_tick[from] >= 0 && g->tick - g->players.last_donate_tick[from] < DONATE_COOLDOWN) return false;
    return true;
}

void player_donate_troops(OfeGame *g, PlayerId from, PlayerId to, double troops) {
    if (!can_donate(g, from, to, false)) return;
    if (troops < 0) troops = floor((double)g->players.troops[from] / 3.0);
    double removed = player_remove_troops(g, from, troops);
    if (removed <= 0) return;
    player_add_troops(g, to, removed);
    g->players.last_donate_tick[from] = g->tick;
    player_update_relation(g, to, from, 50);
    event_emit(g, OFE_EV_DONATION, 0, from, to, 0, removed);
}

void player_donate_gold(OfeGame *g, PlayerId from, PlayerId to, double gold) {
    if (!can_donate(g, from, to, true)) return;
    int64_t amount = gold < 0 ? g->players.gold[from] / 3 : (int64_t)floor(gold);
    int64_t removed = player_remove_gold(g, from, amount);
    if (removed <= 0) return;
    player_add_gold(g, to, removed);
    g->players.last_donate_tick[from] = g->tick;
    player_update_relation(g, to, from, 50);
    event_emit(g, OFE_EV_DONATION, 0, from, to, 1, (double)removed);
}

/* ---- Conquest ------------------------------------------------------------- */

void player_conquered(OfeGame *g, PlayerId conqueror, PlayerId conquered) {
    PlayerTable *pl = &g->players;
    /* A human who never attacked anyone gives no gold (anti-farming). */
    bool skip_gold = pl->attacks_sent[conquered] == 0 && pl->type[conquered] == OFE_PLAYER_HUMAN;
    int64_t taken = 0;
    if (!skip_gold) {
        int64_t gold = pl->gold[conquered];
        int type = pl->type[conquered];
        taken = (type == OFE_PLAYER_BOT || type == OFE_PLAYER_NATION) ? gold : gold / 2;
        player_add_gold(g, conqueror, taken);
        player_remove_gold(g, conquered, gold);
    }
    event_emit(g, OFE_EV_PLAYER_CONQUERED, 0, conqueror, conquered, 0, (double)taken);
}

/* ---- Per-tick update -------------------------------------------------------- */

static bool is_nuke(int type) {
    return type == OFE_UNIT_ATOM_BOMB || type == OFE_UNIT_HYDROGEN_BOMB || type == OFE_UNIT_MIRV ||
           type == OFE_UNIT_MIRV_WARHEAD;
}

static void player_died(OfeGame *g, PlayerId p) {
    PlayerTable *pl = &g->players;
    pl->dead[p] = 1;
    /* A dead player keeps nothing but the nukes already in flight. */
    pl->gold[p] = 0;
    UnitTable *u = &g->units;
    for (int32_t i = 0; i < u->count; i++) {
        if (u->alive[i] && u->owner[i] == p && !is_nuke(u->type[i])) unit_destroy(g, unit_id_of_slot(g, i), 0);
    }
    /* Alliances and requests involving the dead player vanish silently. */
    for (int32_t i = g->alliances.len - 1; i >= 0; i--) {
        Alliance *al = &g->alliances.data[i];
        if (al->a == p || al->b == p) g->alliances.data[i] = g->alliances.data[--g->alliances.len];
    }
    for (int32_t i = g->alliance_requests.len - 1; i >= 0; i--) {
        AllianceRequest *r = &g->alliance_requests.data[i];
        if (r->requestor == p || r->recipient == p) request_remove(g, i);
    }
    event_emit(g, OFE_EV_PLAYER_DIED, 0, p, 0, 0, 0);
}

void players_tick(OfeGame *g) {
    PlayerTable *pl = &g->players;
    decay_relations(g);
    for (PlayerId p = 1; p < pl->count; p++) {
        if (!pl->spawned[p] || pl->dead[p]) continue;
        if (!player_alive(g, p)) {
            player_died(g, p);
            continue;
        }
        double inc = player_troop_increase(g, p);
        player_add_troops(g, p, inc);
        int64_t gold = player_gold_rate(g, p);
        player_add_gold(g, p, gold);

        /* Enclave annexation: every 20 ticks if the territory changed, or
         * every tick while small. */
        bool due = g->tick - pl->cluster_calc_tick[p] > 20 || pl->num_tiles[p] < 100;
        if (due && pl->last_tile_change[p] >= pl->cluster_calc_tick[p]) {
            pl->cluster_calc_tick[p] = g->tick;
            cluster_check_player(g, p);
        }
    }
    alliances_tick(g);
    expire_embargoes(g);
}

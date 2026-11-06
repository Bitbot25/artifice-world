/*
 * ai_build.c - what nations spend gold on: structures (NationStructureBehavior
 * + NationStructurePlacement), warships (NationWarshipBehavior), nukes
 * (NationNukeBehavior) and MIRVs (NationMIRVBehavior).
 *
 * Structures: cities first, other structures in proportion to the city count
 * (ports, factories, SAMs, silos), upgrades once the land is crowded. Hard and
 * Impossible nations "save up" for nukes by perceiving structures as pricier.
 * Placement samples ~25 tiles and scores them (elevation, distance from the
 * border, spacing from similar structures).
 *
 * Simplifications: no rail-connectivity placement score; no nuke price
 * memory or strike follow-ups; SAM interception is judged by SAMs within
 * range of the blast, not by trajectory; MIRV leak share counts the target's
 * SAM levels against its warheads.
 */
#include <float.h>

#include "ai_common.h"

#define MAX_LIST 256
#define BORDER_SPACING 30 /* atom bomb outer radius */
#define STRUCTURE_SPACING 60
#define DEFENSE_POST_RANGE 30
#define SAM_DEFAULT_RANGE 70
#define MAX_SILOS 3
#define UPGRADE_DENSITY (1.0 / 1500.0)
#define THREAT_RATIO 0.35 /* incoming / own troops needed before building defence posts */
#define MIRV_COOLDOWN_TICKS 300

static const double SAM_RATIO[4] = {0.15, 0.2, 0.25, 0.3};
static const int CITIES_BEFORE_SAVING[4] = {0, 3, 4, 5};
static const double INCOME_SAVING_WEIGHT[4] = {1, 0.8, 0.6, 0.4};
static const double FACTORY_COASTAL_RATIO[4] = {0.33, 0.45, 0.55, 0.6};
static const int UPGRADE_RANDOM_PCT[4] = {70, 40, 25, 10};
static const int SAM_UPGRADE_COVERED_PCT[4] = {0, 50, 100, 100};
static const int MAX_UPGRADES[4] = {3, 10, 1000, 1000};
static const double MIN_MIRV_LEAK[4] = {0, 0.1, 0.2, 0.25};
static const int SAVE_UP_HORIZON_MINUTES[4] = {0, 0, 5, 6}; /* 0 = unlimited */

typedef struct UnitList {
    int32_t slot[MAX_LIST];
    int32_t n;
} UnitList;

static bool unit_enabled(const OfeGame *g, int type) { return !(g->cfg.disabled_units & (1u << type)); }
static double gold_of(const OfeGame *g, PlayerId p) { return (double)g->players.gold[p]; }
static double cost_of(const AiCtx *c, int type) { return (double)unit_cost(c->g, c->p, type); }
static bool hard_or_impossible(const AiCtx *c) { return c->diff >= OFE_DIFFICULTY_HARD; }

/* Units of p whose type is in the mask, finished ones only if asked. One pass over the unit table. */
static void list_units(const OfeGame *g, PlayerId p, uint32_t type_mask, bool finished_only, UnitList *out) {
    const UnitTable *u = &g->units;
    out->n = 0;
    for (int32_t i = 0; i < u->count && out->n < MAX_LIST; i++) {
        if (!u->alive[i] || u->owner[i] != p || !((1u << u->type[i]) & type_mask)) continue;
        if (finished_only && (u->flags[i] & OFE_UF_UNDER_CONSTRUCTION)) continue;
        out->slot[out->n++] = i;
    }
}

#define BIT(t) (1u << (t))
#define STRUCTURE_MASK (BIT(OFE_UNIT_CITY) | BIT(OFE_UNIT_PORT) | BIT(OFE_UNIT_FACTORY) | BIT(OFE_UNIT_SAM_LAUNCHER) | BIT(OFE_UNIT_MISSILE_SILO) | BIT(OFE_UNIT_DEFENSE_POST))
#define PROTECTED_MASK (BIT(OFE_UNIT_CITY) | BIT(OFE_UNIT_FACTORY) | BIT(OFE_UNIT_MISSILE_SILO) | BIT(OFE_UNIT_PORT))

static double dist2(const OfeGame *g, TileRef a, TileRef b) { return (double)map_dist2(g->map, a, b); }

static void issue_build(AiCtx *c, int type, TileRef tile, int32_t flags) {
    OfeIntent in = {0};
    in.type = OFE_INTENT_BUILD_UNIT;
    in.player = c->p;
    in.unit_type = type;
    in.tile = tile;
    in.amount = 1;
    in.flags = flags;
    game_issue(c->g, &in);
}

/* A purchase went through if it cost gold (or was free). */
static bool paid(const OfeGame *g, PlayerId p, int64_t gold_before, int64_t price) { return price == 0 || g->players.gold[p] < gold_before; }

/* ---- MIRV worth and save-up target -------------------------------------------------------------- */

/* Share of a MIRV's warheads at target that its SAMs would miss. */
static double mirv_leak_share(const OfeGame *g, PlayerId target) {
    double warheads = OFE_CLAMP((double)g->players.num_tiles[target] / 2000.0, 1.0, 350.0);
    double shots = g->players.unit_levels[target][OFE_UNIT_SAM_LAUNCHER];
    return OFE_MAX(0.0, 1.0 - shots / warheads);
}

static bool mirv_gets_through(const AiCtx *c, PlayerId target) {
    double min_leak = MIN_MIRV_LEAK[c->diff];
    return min_leak == 0 || mirv_leak_share(c->g, target) >= min_leak;
}

static bool valid_mirv_target(const OfeGame *g, PlayerId p, PlayerId q) {
    return q != p && g->players.type[q] != OFE_PLAYER_BOT && g->players.num_tiles[q] > 0 && !player_same_team(g, p, q);
}

/* Would a MIRV get through at the land or city leader (who nations MIRV)? */
static bool mirv_worth_saving_for(AiCtx *c) {
    OfeGame *g = c->g;
    AiSysState *ai = c->ai;
    if (ai->worth_tick == g->tick + 1 && ai->worth_p == c->p) return ai->worth;
    PlayerId land = 0, city = 0;
    for (PlayerId q = 1; q < g->players.count; q++) {
        if (!valid_mirv_target(g, c->p, q)) continue;
        if (!land || g->players.num_tiles[q] > g->players.num_tiles[land]) land = q;
        if (!city || g->players.unit_count[q][OFE_UNIT_CITY] > g->players.unit_count[city][OFE_UNIT_CITY]) city = q;
    }
    bool worth = (land && mirv_gets_through(c, land)) || (city && city != land && mirv_gets_through(c, city));
    ai->worth_tick = g->tick + 1;
    ai->worth_p = c->p;
    ai->worth = worth;
    return worth;
}

/* Gold target nations save for, by what nukes are enabled. */
double ai_save_up_target(const AiCtx *c) {
    const OfeGame *g = c->g;
    if (!unit_enabled(g, OFE_UNIT_MISSILE_SILO)) return cost_of(c, OFE_UNIT_SAM_LAUNCHER);
    if (g->cfg.game_mode == OFE_MODE_TEAM) return cost_of(c, OFE_UNIT_HYDROGEN_BOMB);
    if (unit_enabled(g, OFE_UNIT_MIRV) && mirv_worth_saving_for((AiCtx *)c)) {
        return cost_of(c, OFE_UNIT_MIRV) + cost_of(c, OFE_UNIT_HYDROGEN_BOMB);
    }
    if (unit_enabled(g, OFE_UNIT_HYDROGEN_BOMB)) return cost_of(c, OFE_UNIT_HYDROGEN_BOMB) * 5;
    if (unit_enabled(g, OFE_UNIT_ATOM_BOMB)) return cost_of(c, OFE_UNIT_ATOM_BOMB) * 20;
    return cost_of(c, OFE_UNIT_SAM_LAUNCHER);
}

/* Hard and Impossible don't save for what their income can't reach in minutes. */
static bool save_up_within_reach(const AiCtx *c, double target) {
    const OfeGame *g = c->g;
    int32_t horizon = SAVE_UP_HORIZON_MINUTES[c->diff];
    double missing = target - gold_of(g, c->p);
    if (horizon == 0 || missing <= 0) return true;
    /* Income grows about linearly over a game, so the current rate is about twice the average. */
    double minutes = OFE_MAX(1.0, g->tick / 600.0);
    double per_minute = 2.0 * (double)g->players.gold_earned[c->p] / minutes;
    return missing <= per_minute * horizon;
}

/* ---- Counting ------------------------------------------------------------------------------------ */

static int32_t city_count(const OfeGame *g, PlayerId p) {
    if (!unit_enabled(g, OFE_UNIT_CITY)) return OFE_MAX(1, g->players.num_tiles[p] / 2000);
    return g->players.unit_count[p][OFE_UNIT_CITY];
}

static int32_t owned_levels(const OfeGame *g, PlayerId p, int type) {
    return OFE_MAX(g->players.unit_count[p][type], g->players.unit_levels[p][type]);
}

static bool has_ocean_neighbor(const OfeMap *m, TileRef t) {
    TileRef nb[4];
    int n = map_neighbors4(m, t, nb);
    for (int i = 0; i < n; i++) {
        if (map_is_water(m, nb[i]) && map_is_ocean(m, nb[i])) return true;
    }
    return false;
}

/* Do we border an ocean (trade and boats possible)? Sampled. */
static bool has_coastal_tiles(AiCtx *c) {
    const IntVec *b = &c->g->players.border[c->p];
    if (b->len == 0) return false;
    for (int i = 0; i < 150; i++) {
        TileRef t = b->data[rng_int(c->rng, 0, b->len)];
        if (map_is_shore(c->g->map, t) && has_ocean_neighbor(c->g->map, t)) return true;
    }
    return false;
}

/* Levels of this structure the ratio to the city count still asks for. */
static int32_t levels_due(AiCtx *c, int type, int32_t cities, bool coastal) {
    const OfeGame *g = c->g;
    bool team_ports = g->cfg.game_mode == OFE_MODE_TEAM && c->diff != OFE_DIFFICULTY_EASY;
    double ratio;
    switch (type) {
    case OFE_UNIT_PORT: ratio = team_ports ? 1.0 : 0.75; break;
    case OFE_UNIT_FACTORY:
        ratio = 0.75;
        if (coastal && unit_enabled(g, OFE_UNIT_PORT)) ratio *= FACTORY_COASTAL_RATIO[c->diff];
        break;
    case OFE_UNIT_SAM_LAUNCHER: ratio = SAM_RATIO[c->diff]; break;
    case OFE_UNIT_MISSILE_SILO: ratio = 0.2; break;
    default: return 0;
    }
    int32_t owned = owned_levels(g, c->p, type), target;
    if (type == OFE_UNIT_MISSILE_SILO) {
        if (owned == 0) ratio = 0.4; /* the first silo comes early so nations start nuking sooner */
        target = OFE_MIN((int32_t)floor(cities * ratio), MAX_SILOS);
    } else {
        target = (int32_t)floor(cities * ratio);
    }
    return target - owned;
}

/* Each structure owned makes the next feel pricier while saving for nukes. */
static double perceived_cost(AiCtx *c, int type) {
    const OfeGame *g = c->g;
    double real = cost_of(c, type);
    if (city_count(g, c->p) < CITIES_BEFORE_SAVING[c->diff]) return real; /* like humans, no saving before the first few cities */
    double target = ai_save_up_target(c);
    if (target <= 0 || gold_of(g, c->p) >= target || !save_up_within_reach(c, target)) return real;

    double per_owned;
    switch (type) {
    case OFE_UNIT_CITY: per_owned = 1.0; break;
    case OFE_UNIT_PORT:
    case OFE_UNIT_FACTORY: per_owned = 1.0; break;
    case OFE_UNIT_SAM_LAUNCHER: per_owned = 0.3; break;
    case OFE_UNIT_MISSILE_SILO: per_owned = 1.0; break;
    default: per_owned = 0.1; break;
    }
    /* Income structures pay for themselves, so smarter nations keep building them. */
    if (type == OFE_UNIT_CITY || type == OFE_UNIT_PORT || type == OFE_UNIT_FACTORY) per_owned *= INCOME_SAVING_WEIGHT[c->diff];
    return ceil(real * (1.0 + per_owned * owned_levels(g, c->p, type)));
}

/* ---- Placement ---------------------------------------------------------------------------------------- */

/* Distance to the nearest tile not ours, searched in square rings up to cap. */
static double border_distance(const OfeGame *g, PlayerId p, TileRef t, int32_t cap) {
    const OfeMap *m = g->map;
    int32_t x = map_x(m, t), y = map_y(m, t);
    for (int32_t r = 1; r <= cap; r++) {
        for (int32_t k = -r; k <= r; k++) {
            int32_t xs[4] = {x + k, x + k, x - r, x + r}, ys[4] = {y - r, y + r, y + k, y + k};
            for (int j = 0; j < 4; j++) {
                if (!map_in_bounds(m, xs[j], ys[j]) || tile_owner(g, map_ref(m, xs[j], ys[j])) != p) return r;
            }
        }
    }
    return cap;
}

static double nearest_dist(const OfeGame *g, const UnitList *l, TileRef t, double cap) {
    double best = cap * cap;
    bool any = false;
    for (int32_t i = 0; i < l->n; i++) {
        double d = dist2(g, g->units.tile[l->slot[i]], t);
        if (d == 0) continue; /* the tile itself */
        if (d < best) best = d;
        any = true;
    }
    return any ? sqrt(best) : -1;
}

typedef struct PlaceInfo {
    UnitList same, cities, factories, protect;
} PlaceInfo;

static double tile_value(AiCtx *c, const PlaceInfo *pi, int type, TileRef t) {
    const OfeGame *g = c->g;
    double d, w = 0;
    if (type == OFE_UNIT_PORT) { /* as far as possible from other ports */
        d = nearest_dist(g, &pi->same, t, 1000);
        return d < 0 ? 0 : d;
    }
    w += map_magnitude(g->map, t); /* higher ground */
    w += border_distance(g, c->p, t, BORDER_SPACING);
    d = nearest_dist(g, &pi->same, t, STRUCTURE_SPACING);
    if (d >= 0) w += d;
    if (type == OFE_UNIT_CITY || type == OFE_UNIT_FACTORY) {
        d = nearest_dist(g, type == OFE_UNIT_CITY ? &pi->factories : &pi->cities, t, STRUCTURE_SPACING);
        if (d >= 0) w += d;
    } else if (type == OFE_UNIT_SAM_LAUNCHER && c->diff != OFE_DIFFICULTY_EASY) {
        /* Near the structures it protects, more so for high-level ones. */
        for (int32_t i = 0; i < pi->protect.n; i++) {
            int32_t s = pi->protect.slot[i];
            if (dist2(g, g->units.tile[s], t) <= SAM_DEFAULT_RANGE * SAM_DEFAULT_RANGE) {
                w += STRUCTURE_SPACING * (hard_or_impossible(c) ? g->units.level[s] : 1);
            }
        }
    }
    return w;
}

/* The best of ~25 sampled tiles where `type` can be built, or -1. */
static TileRef spawn_tile(AiCtx *c, int type) {
    OfeGame *g = c->g;
    const OfeMap *m = g->map;
    const IntVec *b = &g->players.border[c->p];
    if (b->len == 0) return -1;
    TileRef cand[25];
    int32_t n = 0;
    if (type == OFE_UNIT_PORT) {
        for (int tries = 0; tries < 200 && n < 25; tries++) {
            TileRef t = b->data[rng_int(c->rng, 0, b->len)];
            if (map_is_shore(m, t) && has_ocean_neighbor(m, t)) cand[n++] = t;
        }
    } else {
        const AiBox *box = ai_player_box(g, c->ai, c->p);
        if (box->minx > box->maxx) return -1;
        for (int tries = 0; tries < 250 && n < 25; tries++) {
            TileRef t = map_ref(m, rng_int(c->rng, box->minx, box->maxx + 1), rng_int(c->rng, box->miny, box->maxy + 1));
            if (tile_owner(g, t) == c->p) cand[n++] = t;
        }
    }
    if (n == 0) return -1;

    PlaceInfo pi;
    list_units(g, c->p, BIT(type), false, &pi.same);
    list_units(g, c->p, BIT(OFE_UNIT_CITY), false, &pi.cities);
    list_units(g, c->p, BIT(OFE_UNIT_FACTORY), false, &pi.factories);
    list_units(g, c->p, PROTECTED_MASK, false, &pi.protect);

    TileRef best = -1;
    double best_value = 0;
    for (int32_t i = 0; i < n; i++) {
        double v = tile_value(c, &pi, type, cand[i]);
        if (best >= 0 && v <= best_value) continue;
        if (structures_can_build(g, c->p, type, cand[i]) < 0) continue;
        best = cand[i];
        best_value = v;
    }
    return best;
}

/* ---- Upgrades ------------------------------------------------------------------------------------------- */

/* Which structure to upgrade: often a random one, else those under SAM cover first. */
static int32_t best_to_upgrade(AiCtx *c, const UnitList *l) {
    const OfeGame *g = c->g;
    UnitList up;
    up.n = 0;
    for (int32_t i = 0; i < l->n; i++) {
        if (!(g->units.flags[l->slot[i]] & OFE_UF_UNDER_CONSTRUCTION)) up.slot[up.n++] = l->slot[i];
    }
    if (up.n == 0) return -1;
    if (rng_int(c->rng, 0, 100) < UPGRADE_RANDOM_PCT[c->diff]) return up.slot[rng_int(c->rng, 0, up.n)];

    UnitList sams;
    list_units(g, c->p, BIT(OFE_UNIT_SAM_LAUNCHER), true, &sams);
    double score[MAX_LIST];
    for (int32_t i = 0; i < up.n; i++) {
        double s = 0;
        for (int32_t k = 0; k < sams.n; k++) {
            double range = 150.0 - 480.0 / (g->units.level[sams.slot[k]] + 5);
            if (dist2(g, g->units.tile[up.slot[i]], g->units.tile[sams.slot[k]]) <= range * range) {
                s += 10 + (g->units.level[sams.slot[k]] > 1 ? (g->units.level[sams.slot[k]] - 1) * 7.5 : 0);
            }
        }
        score[i] = s + rng_int(c->rng, 0, 5);
    }
    /* Sort indices by score, descending (stable). */
    int32_t order[MAX_LIST];
    for (int32_t i = 0; i < up.n; i++) {
        int32_t j = i;
        while (j > 0 && score[order[j - 1]] < score[i]) {
            order[j] = order[j - 1];
            j--;
        }
        order[j] = i;
    }
    int32_t pick = 0;
    if (up.n >= 2 && rng_chance(c->rng, 2)) pick = up.n >= 3 ? rng_int(c->rng, 1, 3) : 1; /* variety: second or third best */
    return up.slot[order[pick]];
}

static int32_t upgrade_amount(AiCtx *c, int type) {
    const OfeGame *g = c->g;
    int32_t due = 1 << 20;
    if (type != OFE_UNIT_CITY) due = levels_due(c, type, city_count(g, c->p), true);
    int32_t max = OFE_MIN(MAX_UPGRADES[c->diff], due);
    double cost = cost_of(c, type);
    double spare = gold_of(g, c->p) - ai_save_up_target(c);
    int32_t amount = cost > 0 ? (int32_t)OFE_MIN(spare / cost, (double)max) : max;
    return OFE_MAX(1, amount);
}

static bool upgrade_unit(AiCtx *c, int32_t slot) {
    OfeGame *g = c->g;
    if (slot < 0) return false;
    int type = g->units.type[slot];
    int64_t before = g->players.gold[c->p], price = unit_cost(g, c->p, type);
    OfeIntent in = {0};
    in.type = OFE_INTENT_UPGRADE_STRUCTURE;
    in.player = c->p;
    in.target = unit_id_of_slot(g, slot);
    in.amount = upgrade_amount(c, type);
    game_issue(g, &in);
    return paid(g, c->p, before, price);
}

/* SAMs covering every structure: another would add nothing new, but a higher level stops more of a salvo. */
static bool should_upgrade_sam_instead(AiCtx *c, const UnitList *sams) {
    const OfeGame *g = c->g;
    int32_t pct = SAM_UPGRADE_COVERED_PCT[c->diff];
    if (pct == 0 || sams->n == 0) return false;
    UnitList prot;
    list_units(g, c->p, PROTECTED_MASK, false, &prot);
    for (int32_t i = 0; i < prot.n; i++) {
        bool covered = false;
        for (int32_t k = 0; k < sams->n && !covered; k++) {
            double range = 150.0 - 480.0 / (g->units.level[sams->slot[k]] + 5);
            covered = dist2(g, g->units.tile[prot.slot[i]], g->units.tile[sams->slot[k]]) <= range * range;
        }
        if (!covered) return false;
    }
    return pct == 100 || rng_int(c->rng, 0, 100) < pct;
}

/* The SAM with the most structure levels in its range: the likeliest nuke target. */
static int32_t sam_protecting_most(AiCtx *c, const UnitList *sams) {
    const OfeGame *g = c->g;
    UnitList prot;
    list_units(g, c->p, PROTECTED_MASK, false, &prot);
    int32_t best = -1, best_levels = -1;
    for (int32_t k = 0; k < sams->n; k++) {
        int32_t s = sams->slot[k];
        if (g->units.flags[s] & OFE_UF_UNDER_CONSTRUCTION) continue;
        double range = 150.0 - 480.0 / (g->units.level[s] + 5);
        int32_t levels = 0;
        for (int32_t i = 0; i < prot.n; i++) {
            if (dist2(g, g->units.tile[prot.slot[i]], g->units.tile[s]) <= range * range) levels += g->units.level[prot.slot[i]];
        }
        if (levels > best_levels) {
            best_levels = levels;
            best = s;
        }
    }
    return best;
}

static bool maybe_spawn_structure(AiCtx *c, int type) {
    OfeGame *g = c->g;
    if (gold_of(g, c->p) < perceived_cost(c, type)) return false;

    UnitList same;
    list_units(g, c->p, BIT(type), false, &same);
    if (type == OFE_UNIT_SAM_LAUNCHER && should_upgrade_sam_instead(c, &same)) return upgrade_unit(c, sam_protecting_most(c, &same));

    bool upgradable = type != OFE_UNIT_DEFENSE_POST;
    /* Crowded land: upgrade rather than build. */
    UnitList all;
    list_units(g, c->p, STRUCTURE_MASK & ~BIT(OFE_UNIT_DEFENSE_POST), false, &all);
    if (upgradable && g->players.num_tiles[c->p] > 0 && (double)all.n / g->players.num_tiles[c->p] > UPGRADE_DENSITY) {
        if (upgrade_unit(c, best_to_upgrade(c, &same))) return true;
        if (same.n > 0) return false; /* wait for construction */
        /* none of this type yet: fall through and build the first one */
    }
    TileRef tile = spawn_tile(c, type);
    if (tile < 0) {
        /* No room for a new one (e.g. a short coastline full of ports): upgrade one instead. */
        return upgradable && c->diff != OFE_DIFFICULTY_EASY && upgrade_unit(c, best_to_upgrade(c, &same));
    }
    int64_t before = g->players.gold[c->p], price = unit_cost(g, c->p, type);
    int32_t count = g->players.unit_count[c->p][type];
    issue_build(c, type, tile, 0);
    return g->players.unit_count[c->p][type] > count || paid(g, c->p, before, price);
}

/* ---- Defence posts ----------------------------------------------------------------------------------------- */

/* Our border tiles next to land of the given players, sampled to at most MAX_LIST. */
static int32_t facing_tiles(AiCtx *c, const PlayerId *who, int32_t nwho, TileRef *out) {
    const OfeGame *g = c->g;
    const IntVec *b = &g->players.border[c->p];
    int32_t n = 0;
    int32_t stride = b->len / MAX_LIST + 1;
    for (int32_t i = 0; i < b->len && n < MAX_LIST; i += stride) {
        TileRef nb[4];
        int k = map_neighbors4(g->map, b->data[i], nb);
        for (int j = 0; j < k; j++) {
            PlayerId o = tile_owner(g, nb[j]);
            bool hit = false;
            for (int32_t w = 0; w < nwho && !hit; w++) hit = who[w] == o;
            if (hit) {
                out[n++] = b->data[i];
                break;
            }
        }
    }
    return n;
}

static bool near_post(const OfeGame *g, const UnitList *posts, TileRef t, double range) {
    for (int32_t i = 0; i < posts->n; i++) {
        if (dist2(g, g->units.tile[posts->slot[i]], t) < range * range) return true;
    }
    return false;
}

static int32_t count_posts_near(const OfeGame *g, const UnitList *posts, const TileRef *front, int32_t nf, double range, int32_t cap) {
    int32_t count = 0;
    for (int32_t i = 0; i < posts->n && count < cap; i++) {
        for (int32_t k = 0; k < nf; k++) {
            if (dist2(g, g->units.tile[posts->slot[i]], front[k]) <= range * range) {
                count++;
                break;
            }
        }
    }
    return count;
}

/* Builds a post inland of the front, min_d..max_d tiles from its nearest tile. */
static bool build_post_behind(AiCtx *c, const TileRef *front, int32_t nf, double min_d, double max_d) {
    OfeGame *g = c->g;
    const OfeMap *m = g->map;
    if (nf == 0 || gold_of(g, c->p) < cost_of(c, OFE_UNIT_DEFENSE_POST)) return false;
    UnitList posts;
    list_units(g, c->p, BIT(OFE_UNIT_DEFENSE_POST), false, &posts);
    double spread = DEFENSE_POST_RANGE * 1.5;
    TileRef anchors[MAX_LIST];
    int32_t na = 0;
    for (int32_t i = 0; i < nf; i++) {
        if (!near_post(g, &posts, front[i], spread)) anchors[na++] = front[i];
    }
    if (na == 0) return false;
    int32_t radius = (int32_t)ceil(max_d);
    for (int attempt = 0; attempt < 150; attempt++) {
        TileRef a = anchors[rng_int(c->rng, 0, na)];
        int32_t x = rng_int(c->rng, map_x(m, a) - radius, map_x(m, a) + radius + 1);
        int32_t y = rng_int(c->rng, map_y(m, a) - radius, map_y(m, a) + radius + 1);
        if (!map_in_bounds(m, x, y)) continue;
        TileRef t = map_ref(m, x, y);
        if (tile_owner(g, t) != c->p || near_post(g, &posts, t, spread)) continue;
        double nearest = DBL_MAX;
        for (int32_t k = 0; k < nf && nearest >= min_d * min_d; k++) nearest = OFE_MIN(nearest, dist2(g, t, front[k]));
        if (nearest < min_d * min_d || nearest > max_d * max_d) continue;
        if (structures_can_build(g, c->p, OFE_UNIT_DEFENSE_POST, t) < 0) continue;
        int32_t count = g->players.unit_count[c->p][OFE_UNIT_DEFENSE_POST];
        issue_build(c, OFE_UNIT_DEFENSE_POST, t, 0);
        return g->players.unit_count[c->p][OFE_UNIT_DEFENSE_POST] > count;
    }
    return false;
}

static bool defense_post_needed(const AiCtx *c) {
    if (c->diff == OFE_DIFFICULTY_EASY || c->fights.n_in == 0) return false;
    double troops = (double)c->g->players.troops[c->p];
    return troops > 0 && c->fights.in_total / troops >= THREAT_RATIO;
}

/* A post behind an active attack front, far enough back to finish before the attack arrives. */
static bool try_build_defense_post(AiCtx *c) {
    OfeGame *g = c->g;
    if (!defense_post_needed(c)) return false;
    if (c->diff == OFE_DIFFICULTY_MEDIUM && !rng_chance(c->rng, 2)) return false;
    double ratio = c->fights.in_total / (double)g->players.troops[c->p];
    int32_t allowed = c->diff == OFE_DIFFICULTY_MEDIUM ? 1 : (int32_t)ceil(ratio / 0.4);
    double scaled = c->diff == OFE_DIFFICULTY_MEDIUM ? 0 : OFE_MIN(ratio, 2.0);
    double min_d = DEFENSE_POST_RANGE * (2.0 / 3.0 + scaled / 3.0), max_d = min_d + DEFENSE_POST_RANGE * 0.5;

    PlayerId attackers[AI_MAX_FIGHTS];
    int32_t na = 0;
    for (int32_t i = 0; i < c->fights.n_in; i++) {
        if (c->fights.in[i].land > 0) attackers[na++] = c->fights.in[i].who;
    }
    TileRef front[MAX_LIST];
    int32_t nf = facing_tiles(c, attackers, na, front);
    UnitList posts;
    list_units(g, c->p, BIT(OFE_UNIT_DEFENSE_POST), false, &posts);
    if (count_posts_near(g, &posts, front, nf, max_d, allowed) >= allowed) return false;
    return build_post_behind(c, front, nf, min_d, max_d);
}

/* Hard and Impossible cover the border with a hostile neighbour before it attacks again. */
static bool maybe_build_proactive_post(AiCtx *c) {
    OfeGame *g = c->g;
    if (!hard_or_impossible(c) || !unit_enabled(g, OFE_UNIT_DEFENSE_POST)) return false;
    int32_t cities = city_count(g, c->p);
    if (cities < 2 || g->players.unit_count[c->p][OFE_UNIT_DEFENSE_POST] >= cities * 0.5) return false;
    if (gold_of(g, c->p) < cost_of(c, OFE_UNIT_DEFENSE_POST)) return false;
    ai_ctx_scan(c);
    double min_troops = (double)g->players.troops[c->p] * (c->diff == OFE_DIFFICULTY_HARD ? 1.0 : 0.5);
    PlayerId threats[AI_MAX_FIGHTS];
    int32_t nt = 0;
    const IntVec *nb = &c->ai->main.nb;
    for (int32_t i = 0; i < nb->len && nt < AI_MAX_FIGHTS; i++) {
        PlayerId q = nb->data[i];
        if (g->players.type[q] == OFE_PLAYER_BOT || player_friendly(g, c->p, q)) continue;
        if (ai_relation(g, c->p, q) == REL_HOSTILE && (double)g->players.troops[q] >= min_troops) threats[nt++] = q;
    }
    if (nt == 0) return false;
    TileRef front[MAX_LIST];
    int32_t nf = facing_tiles(c, threats, nt, front);
    UnitList posts;
    list_units(g, c->p, BIT(OFE_UNIT_DEFENSE_POST), false, &posts);
    if (count_posts_near(g, &posts, front, nf, DEFENSE_POST_RANGE * 1.5, nt) >= nt) return false;
    return build_post_behind(c, front, nf, DEFENSE_POST_RANGE * 0.5, DEFENSE_POST_RANGE);
}

/* ---- Structure round ----------------------------------------------------------------------------------------- */

/* Nations starting rich place their first structures with pauses so they
 * don't all land inside one blast radius. */
static bool on_structure_cooldown(const AiCtx *c) {
    static const int32_t GAP[5] = {0, 0, 250, 150, 100};
    const int32_t *st = c->g->players.ai_state[c->p];
    if (st[AI_LAST_STRUCT] < 0 || !ai_high_start_gold(c->g) || c->g->cfg.starting_gold >= 10000000) return false;
    int32_t gap = st[AI_PLACEMENTS] < 5 ? GAP[st[AI_PLACEMENTS]] : 0;
    return gap > 0 && c->g->tick - st[AI_LAST_STRUCT] < gap;
}

static bool do_handle_structures(AiCtx *c) {
    OfeGame *g = c->g;
    int32_t *st = g->players.ai_state[c->p];
    bool cities_on = unit_enabled(g, OFE_UNIT_CITY);
    int32_t cities = city_count(g, c->p);
    bool silo_on = unit_enabled(g, OFE_UNIT_MISSILE_SILO);
    bool nukes_on = unit_enabled(g, OFE_UNIT_ATOM_BOMB) || unit_enabled(g, OFE_UNIT_HYDROGEN_BOMB) || unit_enabled(g, OFE_UNIT_MIRV);
    bool coastal = has_coastal_tiles(c);

    /* Crowded maps: the first structure is a port (or factory) rather than a city, for earlier income. */
    bool crowded = g->num_land_tiles > 0 && (double)g->map->num_nations / g->num_land_tiles > 1.0 / 7500.0;
    if (cities_on && !(st[AI_FLAGS] & AIF_FIRST_BUILT) && crowded) {
        int first = coastal && unit_enabled(g, OFE_UNIT_PORT) ? OFE_UNIT_PORT : OFE_UNIT_FACTORY;
        if (!unit_enabled(g, first)) st[AI_FLAGS] |= AIF_FIRST_BUILT;
        else if (maybe_spawn_structure(c, first)) {
            st[AI_FLAGS] |= AIF_FIRST_BUILT;
            return true;
        }
    }
    if (maybe_build_proactive_post(c)) return true;

    static const int ORDER[4] = {OFE_UNIT_PORT, OFE_UNIT_FACTORY, OFE_UNIT_SAM_LAUNCHER, OFE_UNIT_MISSILE_SILO};
    for (int i = 0; i < 4; i++) {
        int type = ORDER[i];
        if (!unit_enabled(g, type)) continue;
        if (type == OFE_UNIT_PORT && !coastal) continue;
        if ((type == OFE_UNIT_MISSILE_SILO || type == OFE_UNIT_SAM_LAUNCHER) && !nukes_on) continue;
        if (type == OFE_UNIT_SAM_LAUNCHER && !silo_on) continue;
        if (levels_due(c, type, cities, coastal) > 0 && maybe_spawn_structure(c, type)) return true;
    }
    return cities_on && maybe_spawn_structure(c, OFE_UNIT_CITY);
}

bool ai_build_handle_structures(AiCtx *c) {
    OfeGame *g = c->g;
    int32_t *st = g->players.ai_state[c->p];
    /* Defence posts sit outside the normal pacing; never the first structure. */
    if (st[AI_PLACEMENTS] > 0 && unit_enabled(g, OFE_UNIT_DEFENSE_POST)) {
        if (try_build_defense_post(c)) return true;
        if (defense_post_needed(c)) return false; /* save the gold for it */
    }
    if (on_structure_cooldown(c)) return false;
    bool built = do_handle_structures(c);
    if (built) {
        st[AI_LAST_STRUCT] = g->tick;
        st[AI_PLACEMENTS]++;
    }
    return built;
}

/* ---- Warships ------------------------------------------------------------------------------------------------------ */

/* Gold for warships; with atom bombs off, silo owners keep a hydrogen bomb's price for nukes. */
static double spare_gold(const AiCtx *c) {
    const OfeGame *g = c->g;
    if (unit_enabled(g, OFE_UNIT_ATOM_BOMB) || !unit_enabled(g, OFE_UNIT_HYDROGEN_BOMB) || g->players.unit_count[c->p][OFE_UNIT_MISSILE_SILO] == 0) {
        return gold_of(g, c->p);
    }
    return gold_of(g, c->p) - cost_of(c, OFE_UNIT_HYDROGEN_BOMB);
}

/* One warship per port owner, now and then (1 in 50 decisions). */
void ai_build_maybe_warship(AiCtx *c) {
    OfeGame *g = c->g;
    if (!unit_enabled(g, OFE_UNIT_WARSHIP) || !rng_chance(c->rng, 50)) return;
    if (g->players.unit_count[c->p][OFE_UNIT_PORT] == 0 || g->players.unit_count[c->p][OFE_UNIT_WARSHIP] > 0) return;
    if (spare_gold(c) <= cost_of(c, OFE_UNIT_WARSHIP)) return;
    UnitList ports;
    list_units(g, c->p, BIT(OFE_UNIT_PORT), true, &ports);
    if (ports.n == 0) return;
    TileRef port = g->units.tile[ports.slot[rng_int(c->rng, 0, ports.n)]];
    const OfeMap *m = g->map;
    for (int attempt = 0; attempt < 50; attempt++) {
        int32_t x = rng_int(c->rng, map_x(m, port) - 250, map_x(m, port) + 250);
        int32_t y = rng_int(c->rng, map_y(m, port) - 250, map_y(m, port) + 250);
        if (!map_in_bounds(m, x, y) || !map_is_water(m, map_ref(m, x, y))) continue;
        TileRef t = map_ref(m, x, y);
        if (structures_can_build(g, c->p, OFE_UNIT_WARSHIP, t) < 0) return;
        issue_build(c, OFE_UNIT_WARSHIP, t, 0);
        return;
    }
}

/* ---- MIRVs ----------------------------------------------------------------------------------------------------------- */

static bool recently_mirved(const AiCtx *c, PlayerId target) {
    for (int i = 0; i < 8; i++) {
        if (c->ai->mirv_who[i] == target && c->ai->mirv_tick[i] > 0 && c->g->tick - c->ai->mirv_tick[i] < MIRV_COOLDOWN_TICKS) return true;
    }
    return false;
}

static void record_mirv(AiCtx *c, PlayerId target) {
    c->ai->mirv_who[c->ai->mirv_pos] = target;
    c->ai->mirv_tick[c->ai->mirv_pos] = c->g->tick + 1;
    c->ai->mirv_pos = (c->ai->mirv_pos + 1) & 7;
}

/* A tile the target owns near the middle of its land. */
static TileRef territory_center(AiCtx *c, PlayerId target) {
    const OfeGame *g = c->g;
    const OfeMap *m = g->map;
    const AiBox *b = ai_player_box((OfeGame *)g, c->ai, target);
    if (b->minx > b->maxx) return -1;
    int32_t cx = (b->minx + b->maxx) / 2, cy = (b->miny + b->maxy) / 2;
    for (int32_t r = 0; r <= 30; r++) {
        for (int32_t k = -r; k <= r; k++) {
            int32_t xs[4] = {cx + k, cx + k, cx - r, cx + r}, ys[4] = {cy - r, cy + r, cy + k, cy + k};
            for (int j = 0; j < 4; j++) {
                if (map_in_bounds(m, xs[j], ys[j]) && tile_owner(g, map_ref(m, xs[j], ys[j])) == target) return map_ref(m, xs[j], ys[j]);
            }
        }
    }
    const IntVec *tb = &g->players.border[target];
    return tb->len ? tb->data[rng_int(c->rng, 0, tb->len)] : -1;
}

static PlayerId inbound_mirv_sender(const AiCtx *c) {
    const OfeGame *g = c->g;
    const UnitTable *u = &g->units;
    PlayerId best = 0;
    for (int32_t i = 0; i < u->count; i++) {
        if (!u->alive[i] || u->type[i] != OFE_UNIT_MIRV || u->target_tile[i] < 0) continue;
        PlayerId sender = u->owner[i];
        if (!valid_mirv_target(g, c->p, sender) || tile_owner(g, u->target_tile[i]) != c->p) continue;
        if (!best || g->players.num_tiles[sender] > g->players.num_tiles[best]) best = sender;
    }
    return best;
}

/* The player nearing the win bar (percent of land by difficulty). */
static PlayerId victory_denial_target(AiCtx *c) {
    static const int32_t PCT[4] = {75, 65, 55, 40};
    OfeGame *g = c->g;
    int32_t tiles;
    PlayerId leader = ai_land_leader(g, c->ai, &tiles);
    if (!leader || g->num_land_tiles <= 0 || !valid_mirv_target(g, c->p, leader)) return 0;
    return (int64_t)tiles * 100 >= (int64_t)g->num_land_tiles * PCT[c->diff] ? leader : 0;
}

/* Far more cities than anyone else. */
static PlayerId steamroll_target(AiCtx *c) {
    static const double GAP[4] = {2, 1.5, 1.25, 1.15};
    static const int32_t MIN_CITIES[4] = {20, 10, 10, 8};
    const OfeGame *g = c->g;
    PlayerId top = 0;
    int32_t top_n = -1, second_n = -1;
    for (PlayerId q = 1; q < g->players.count; q++) {
        if (g->players.num_tiles[q] <= 0) continue;
        int32_t n = g->players.unit_count[q][OFE_UNIT_CITY];
        if (n > top_n) {
            second_n = top_n;
            top_n = n;
            top = q;
        } else if (n > second_n) {
            second_n = n;
        }
    }
    if (!top || second_n < 0 || top_n <= MIN_CITIES[c->diff] || top_n < second_n * GAP[c->diff]) return 0;
    return valid_mirv_target(g, c->p, top) ? top : 0;
}

/* Hard and Impossible: the biggest bordering player about our size. */
static PlayerId rival_target(AiCtx *c) {
    const OfeGame *g = c->g;
    if (!hard_or_impossible(c)) return 0;
    ai_ctx_scan(c);
    double min_tiles = g->players.num_tiles[c->p] * 0.5;
    PlayerId best = 0;
    const IntVec *nb = &c->ai->main.nb;
    for (int32_t i = 0; i < nb->len; i++) {
        PlayerId q = nb->data[i];
        if (!valid_mirv_target(g, c->p, q) || player_friendly(g, c->p, q) || !ai_borders_by_land(c, q)) continue;
        if (g->players.num_tiles[q] < min_tiles || (best && g->players.num_tiles[q] <= g->players.num_tiles[best])) continue;
        best = q;
    }
    return best;
}

static void send_mirv(AiCtx *c, PlayerId target) {
    TileRef center = territory_center(c, target);
    if (center < 0 || structures_can_build(c->g, c->p, OFE_UNIT_MIRV, center) < 0) return;
    int64_t before = c->g->players.gold[c->p];
    issue_build(c, OFE_UNIT_MIRV, center, 0);
    if (c->g->players.gold[c->p] < before) record_mirv(c, target);
}

/* Nations MIRV in answer to a MIRV, to stop a runaway player, or to wipe out a rival they border. */
bool ai_build_consider_mirv(AiCtx *c) {
    OfeGame *g = c->g;
    static const int32_t HESITATION[4] = {2, 4, 8, 16};
    if (!unit_enabled(g, OFE_UNIT_MIRV) || g->players.unit_count[c->p][OFE_UNIT_MISSILE_SILO] == 0) return false;
    if (gold_of(g, c->p) < cost_of(c, OFE_UNIT_MIRV)) return false;
    if (rng_chance(c->rng, HESITATION[c->diff])) return false;

    PlayerId target = inbound_mirv_sender(c);
    if (!target || recently_mirved(c, target) || !mirv_gets_through(c, target)) target = 0;
    if (!target) {
        target = victory_denial_target(c);
        if (!target || recently_mirved(c, target) || !mirv_gets_through(c, target)) target = 0;
    }
    if (!target) {
        target = steamroll_target(c);
        if (!target || recently_mirved(c, target) || !mirv_gets_through(c, target)) target = 0;
    }
    if (!target) {
        target = rival_target(c);
        if (!target || recently_mirved(c, target) || !mirv_gets_through(c, target)) target = 0;
    }
    if (!target) return false;
    send_mirv(c, target);
    return true;
}

/* ---- Nukes ------------------------------------------------------------------------------------------------------------ */

typedef struct NukeKind {
    int type;
    int inner, outer;
    double min_coverage;
} NukeKind;
static const NukeKind HYDROGEN = {OFE_UNIT_HYDROGEN_BOMB, 80, 100, 0.4};
static const NukeKind ATOM = {OFE_UNIT_ATOM_BOMB, 12, 30, 0.25};

static int32_t structure_levels(const OfeGame *g, PlayerId q) {
    const int32_t *lv = g->players.unit_levels[q];
    return lv[OFE_UNIT_CITY] + lv[OFE_UNIT_PORT] + lv[OFE_UNIT_FACTORY] + lv[OFE_UNIT_SAM_LAUNCHER] + lv[OFE_UNIT_MISSILE_SILO] +
           lv[OFE_UNIT_DEFENSE_POST];
}

static int32_t alive_players(const OfeGame *g) {
    int32_t n = 0;
    for (PlayerId q = 1; q < g->players.count; q++) n += g->players.num_tiles[q] > 0;
    return n;
}

static PlayerId incoming_nuke_sender(const AiCtx *c) {
    const OfeGame *g = c->g;
    const UnitTable *u = &g->units;
    for (int32_t i = 0; i < u->count; i++) {
        if (!u->alive[i] || (u->type[i] != OFE_UNIT_ATOM_BOMB && u->type[i] != OFE_UNIT_HYDROGEN_BOMB) || u->target_tile[i] < 0) continue;
        if (player_friendly(g, c->p, u->owner[i])) continue;
        if (tile_owner(g, u->target_tile[i]) == c->p) return u->owner[i];
    }
    return 0;
}

/* Team games: the enemy hitting our teammates hardest, else the one they hit hardest. */
static PlayerId teammate_fight_target(const AiCtx *c) {
    const OfeGame *g = c->g;
    if (g->cfg.game_mode != OFE_MODE_TEAM || c->diff == OFE_DIFFICULTY_EASY) return 0;
    const AttackTable *t = &g->attacks;
    PlayerId best = 0;
    double best_troops = 0;
    for (int pass = 0; pass < 2 && !best; pass++) {
        for (int32_t i = 0; i < t->count; i++) {
            if (!t->active[i]) continue;
            PlayerId mate = pass == 0 ? t->target[i] : t->attacker[i], enemy = pass == 0 ? t->attacker[i] : t->target[i];
            if (mate == 0 || mate == c->p || !player_same_team(g, c->p, mate)) continue;
            if (enemy == 0 || g->players.type[enemy] == OFE_PLAYER_BOT || player_friendly(g, c->p, enemy)) continue;
            if (t->troops[i] > best_troops) {
                best_troops = t->troops[i];
                best = enemy;
            }
        }
    }
    return best;
}

static bool can_strike(AiCtx *c, PlayerId target) {
    const OfeGame *g = c->g;
    if (g->players.type[target] == OFE_PLAYER_BOT || player_friendly(g, c->p, target) || !ai_borders_by_land(c, target)) return false;
    double send = OFE_MIN((double)g->players.troops[c->p] - player_max_troops(g, c->p) * g->players.ai_state[c->p][AI_RESERVE] / 100.0, INFINITY);
    return send >= (double)g->players.troops[target] * 0.5;
}

static PlayerId strike_target(AiCtx *c) {
    const OfeGame *g = c->g;
    double mine = (double)g->players.troops[c->p];
    if (c->diff == OFE_DIFFICULTY_EASY) return 0;
    if (c->diff == OFE_DIFFICULTY_MEDIUM) {
        for (int32_t i = 0; i < c->fights.n_out; i++) {
            PlayerId q = c->fights.out[i].who;
            if (q && g->players.type[q] != OFE_PLAYER_BOT && !player_friendly(g, c->p, q) && (double)g->players.troops[q] >= mine * 0.5) return q;
        }
        return 0;
    }
    PlayerId best = 0;
    const IntVec *nb = &c->ai->main.nb;
    for (int32_t i = 0; i < nb->len; i++) {
        PlayerId q = nb->data[i];
        if (g->players.type[q] == OFE_PLAYER_BOT || player_friendly(g, c->p, q) || (double)g->players.troops[q] < mine * 0.5) continue;
        if (best && g->players.troops[q] <= g->players.troops[best]) continue;
        if (can_strike(c, q)) best = q;
    }
    return best;
}

static PlayerId ffa_crown_target(AiCtx *c) {
    static const double THRESH[4] = {0, 0.3, 0.2, 0.1};
    OfeGame *g = c->g;
    if (g->cfg.game_mode != OFE_MODE_FFA || c->diff == OFE_DIFFICULTY_EASY) return 0;
    int32_t tiles;
    PlayerId first = ai_land_leader(g, c->ai, &tiles);
    int32_t land = g->num_land_tiles - g->num_fallout_tiles;
    if (!first || first == c->p || player_friendly(g, c->p, first) || land <= 0) return 0;
    double share = (double)tiles / land;
    if (share > 0.5) return first;
    if (ai_runaway_leader(g, c->ai) != first) return 0;
    double threshold = THRESH[c->diff];
    ai_ctx_scan(c);
    if (!ai_borders_by_land(c, first)) threshold *= 2;
    return share - (double)g->players.num_tiles[c->p] / land > threshold ? first : 0;
}

static PlayerId strongest_team_target(AiCtx *c) {
    const OfeGame *g = c->g;
    if (g->cfg.game_mode != OFE_MODE_TEAM) return 0;
    int32_t teams = OFE_MAX(2, g->cfg.num_teams);
    int64_t tiles[8] = {0};
    for (PlayerId q = 1; q < g->players.count; q++) {
        int t = g->players.team[q];
        if (t >= 0 && t < 8) tiles[t] += g->players.num_tiles[q];
    }
    int strongest = -1;
    for (int t = 0; t < teams && t < 8; t++) {
        if (t == g->players.team[c->p]) continue;
        if (strongest < 0 || tiles[t] > tiles[strongest]) strongest = t;
    }
    if (strongest < 0) return 0;
    PlayerId best = 0;
    int32_t best_score = -1;
    for (PlayerId q = 1; q < g->players.count; q++) {
        if (g->players.team[q] != strongest || g->players.num_tiles[q] <= 0 || player_friendly(g, c->p, q)) continue;
        int32_t score = hard_or_impossible(c) ? structure_levels(g, q) : (int32_t)(player_max_troops(g, q) / 1000);
        if (score > best_score) {
            best_score = score;
            best = q;
        }
    }
    return best;
}

/* The richest nation hunts players whose structures are packed tightly. */
static PlayerId high_density_target(AiCtx *c) {
    const OfeGame *g = c->g;
    for (PlayerId q = 1; q < g->players.count; q++) {
        if (q != c->p && g->players.type[q] == OFE_PLAYER_NATION && g->players.num_tiles[q] > 0 && g->players.gold[q] > g->players.gold[c->p]) return 0;
    }
    PlayerId best = 0;
    double best_density = 1.0 / 75.0;
    for (PlayerId q = 1; q < g->players.count; q++) {
        if (q == c->p || g->players.type[q] == OFE_PLAYER_BOT || player_friendly(g, c->p, q) || g->players.num_tiles[q] <= 0) continue;
        int32_t levels = structure_levels(g, q);
        if (levels < 5) continue;
        double d = (double)levels / g->players.num_tiles[q];
        if (d > best_density) {
            best_density = d;
            best = q;
        }
    }
    return best;
}

static PlayerId best_nuke_target(AiCtx *c) {
    OfeGame *g = c->g;
    PlayerId t;
    if (hard_or_impossible(c) && alive_players(g) == 2) {
        for (PlayerId q = 1; q < g->players.count; q++) {
            if (q != c->p && g->players.num_tiles[q] > 0) return q;
        }
    }
    if ((t = ai_find_incoming_attacker(c, true))) return t; /* retaliation matters most */
    if ((t = incoming_nuke_sender(c))) return t;
    if ((t = teammate_fight_target(c))) return t;
    if (c->diff == OFE_DIFFICULTY_IMPOSSIBLE && g->cfg.game_mode == OFE_MODE_FFA) {
        int32_t tiles;
        PlayerId crown = ai_land_leader(g, c->ai, &tiles);
        int32_t land = g->num_land_tiles - g->num_fallout_tiles;
        if (crown && crown != c->p && !player_friendly(g, c->p, crown) && land > 0 && (double)tiles / land > 0.5) return crown;
    }
    ai_ctx_scan(c);
    if ((t = strike_target(c))) return t;
    if (c->diff == OFE_DIFFICULTY_IMPOSSIBLE && rng_chance(c->rng, 2) && (t = high_density_target(c))) return t;

    /* Our allies' targets, if we like them. */
    for (int32_t i = 0; i < g->alliances.len; i++) {
        const Alliance al = g->alliances.data[i];
        if (al.a != c->p && al.b != c->p) continue;
        PlayerId ally = al.a == c->p ? al.b : al.a;
        PlayerId target = g->players.target[ally];
        if (target > 0 && target != c->p && player_alive(g, target) && g->tick - g->players.last_target_tick[ally] < 600 &&
            ai_relation(g, c->p, ally) >= REL_FRIENDLY && !player_friendly(g, c->p, target)) {
            return target;
        }
    }
    /* The most hated player, unless we dwarf them. */
    if (g->players.relation_row[c->p] >= 0) {
        double my_max = player_max_troops(g, c->p), worst = -50;
        PlayerId hated = 0;
        for (PlayerId q = 1; q < g->players.count; q++) {
            if (q == c->p || g->players.num_tiles[q] <= 0 || player_friendly(g, c->p, q)) continue;
            double r = player_relation(g, c->p, q);
            if (r >= worst || my_max >= player_max_troops(g, q) * 2) continue;
            worst = r;
            hated = q;
        }
        if (hated) return hated;
    }
    if ((t = ffa_crown_target(c))) return t;
    return strongest_team_target(c);
}

/* Grid-sampled blast: how much is enemy land, and does it touch friendly land? */
typedef struct Blast {
    double coverage; /* share of the blast on land of players we aren't friendly with (inner counts double) */
    bool hits_friendly;
} Blast;

static Blast sample_blast(AiCtx *c, TileRef center, const NukeKind *k, int margin) {
    const OfeGame *g = c->g;
    const OfeMap *m = g->map;
    int32_t cx = map_x(m, center), cy = map_y(m, center);
    int32_t outer = k->outer;
    int32_t step = OFE_MAX(1, outer / 10);
    double total = 0, enemy = 0;
    Blast b = {0, false};
    for (int32_t dy = -(outer + margin); dy <= outer + margin; dy += step) {
        for (int32_t dx = -(outer + margin); dx <= outer + margin; dx += step) {
            int32_t d2 = dx * dx + dy * dy;
            if (d2 > (outer + margin) * (outer + margin)) continue;
            bool inside = d2 <= outer * outer;
            double weight = d2 <= k->inner * k->inner ? 2 : 1;
            if (inside) total += weight;
            if (!map_in_bounds(m, cx + dx, cy + dy)) continue;
            PlayerId o = tile_owner(g, map_ref(m, cx + dx, cy + dy));
            if (o == 0) continue;
            if (player_friendly(g, c->p, o)) b.hits_friendly = true;
            else if (inside) enemy += weight;
        }
    }
    b.coverage = total > 0 ? enemy / total : 0;
    return b;
}

/* Easy and Medium only nuke deep inside the target's land; Hard and Impossible
 * accept free land and non-allied players around it. Checked on two rings. */
static bool blast_area_ok(AiCtx *c, TileRef center, const NukeKind *k, PlayerId target) {
    const OfeGame *g = c->g;
    const OfeMap *m = g->map;
    int32_t cx = map_x(m, center), cy = map_y(m, center);
    for (int ring = 0; ring < 2; ring++) {
        int32_t r = ring == 0 ? k->outer : k->outer / 2;
        int32_t step = OFE_MAX(1, r / 6);
        for (int32_t i = -r; i <= r; i += step) {
            int32_t xs[4] = {cx + i, cx + i, cx - r, cx + r}, ys[4] = {cy - r, cy + r, cy + i, cy + i};
            for (int j = 0; j < 4; j++) {
                if (!map_in_bounds(m, xs[j], ys[j])) return false;
                PlayerId o = tile_owner(g, map_ref(m, xs[j], ys[j]));
                if (o == target) continue;
                if (hard_or_impossible(c) && (o == 0 || !player_friendly(g, c->p, o))) continue;
                return false;
            }
        }
    }
    return true;
}

static int64_t structure_value(int type, int level) {
    switch (type) {
    case OFE_UNIT_CITY: return 25000LL * level;
    case OFE_UNIT_DEFENSE_POST: return 5000LL * level;
    case OFE_UNIT_MISSILE_SILO: return 50000LL * level;
    case OFE_UNIT_PORT:
    case OFE_UNIT_FACTORY: return 15000LL * level;
    default: return 0;
    }
}

/* Is a bomb of ours or anyone's already about to land within both inner radii? */
static bool nuke_landing_near(const OfeGame *g, TileRef tile, const NukeKind *k) {
    const UnitTable *u = &g->units;
    for (int32_t i = 0; i < u->count; i++) {
        if (!u->alive[i] || u->target_tile[i] < 0) continue;
        int inner;
        switch (u->type[i]) {
        case OFE_UNIT_ATOM_BOMB: inner = 12; break;
        case OFE_UNIT_HYDROGEN_BOMB: inner = 80; break;
        case OFE_UNIT_MIRV_WARHEAD: inner = 12; break;
        default: continue;
        }
        double r = k->inner + inner;
        if (dist2(g, tile, u->target_tile[i]) <= r * r) return true;
    }
    return false;
}

static bool sam_near(const OfeGame *g, PlayerId p, TileRef tile, int32_t range) {
    const UnitTable *u = &g->units;
    for (int32_t i = 0; i < u->count; i++) {
        if (!u->alive[i] || u->type[i] != OFE_UNIT_SAM_LAUNCHER || u->owner[i] == p || player_friendly(g, p, u->owner[i])) continue;
        if (dist2(g, tile, u->tile[i]) <= (double)range * range) return true;
    }
    return false;
}

static TileRef find_best_nuke_tile(AiCtx *c, const NukeKind *k, PlayerId target, const UnitList *structures, const UnitList *silos, bool strike) {
    OfeGame *g = c->g;
    const OfeMap *m = g->map;
    TileRef cand[MAX_LIST];
    int32_t n = 0;
    for (int32_t i = 0; i < structures->n && n < 24; i++) cand[n++] = g->units.tile[structures->slot[i]];
    const AiBox *box = ai_player_box(g, c->ai, target);
    int32_t randoms = c->diff == OFE_DIFFICULTY_IMPOSSIBLE ? 30 : 10;
    for (int tries = 0; tries < randoms * 4 && box->minx <= box->maxx && n < 24 + randoms; tries++) {
        TileRef t = map_ref(m, rng_int(c->rng, box->minx, box->maxx + 1), rng_int(c->rng, box->miny, box->maxy + 1));
        if (tile_owner(g, t) == target) cand[n++] = t;
    }
    struct {
        TileRef tile;
        double score;
    } scored[MAX_LIST];
    int32_t ns = 0;
    for (int32_t i = 0; i < n; i++) {
        TileRef t = cand[i];
        if (!blast_area_ok(c, t, k, target)) continue;
        if (structures_can_build(g, c->p, k->type, t) < 0) continue;
        double coverage = 0;
        if (c->diff != OFE_DIFFICULTY_EASY) {
            if (nuke_landing_near(g, t, k)) continue;
            coverage = sample_blast(c, t, k, 0).coverage;
            if (coverage < k->min_coverage) continue;
        }
        if (c->diff == OFE_DIFFICULTY_MEDIUM && sam_near(g, c->p, t, 50)) continue;
        if (hard_or_impossible(c) && sam_near(g, c->p, t, SAM_DEFAULT_RANGE)) continue; /* it would be shot down */

        double value = 0;
        for (int32_t s = 0; s < structures->n; s++) {
            int32_t u = structures->slot[s];
            if (dist2(g, t, g->units.tile[u]) <= (double)k->outer * k->outer) value += (double)structure_value(g->units.type[u], g->units.level[u]);
        }
        double nearest = DBL_MAX;
        for (int32_t s = 0; s < silos->n; s++) nearest = OFE_MIN(nearest, dist2(g, t, g->units.tile[silos->slot[s]]));
        double base = value;
        value = OFE_MAX(base * 0.2, value - sqrt(nearest) * 30); /* closer silos first, but keep 20% of the value */
        if (c->diff == OFE_DIFFICULTY_IMPOSSIBLE && !strike ? value <= 0 : value < 0) continue;
        double score = value + coverage * 50000;
        int32_t j = ns++;
        while (j > 0 && scored[j - 1].score < score) { /* descending, stable */
            scored[j] = scored[j - 1];
            j--;
        }
        scored[j].tile = t;
        scored[j].score = score;
    }
    int margin = strike ? 10 : 0;
    for (int32_t i = 0; i < ns; i++) {
        if (!sample_blast(c, scored[i].tile, k, margin).hits_friendly) return scored[i].tile;
    }
    return -1;
}

/* Are we holding gold back for a MIRV (Hard and Impossible in FFA)? */
static bool saving_for_mirv(AiCtx *c) {
    const OfeGame *g = c->g;
    if (!hard_or_impossible(c) || g->cfg.game_mode == OFE_MODE_TEAM || !unit_enabled(g, OFE_UNIT_MIRV) || alive_players(g) <= 2) return false;
    double target = cost_of(c, OFE_UNIT_MIRV) + cost_of(c, OFE_UNIT_HYDROGEN_BOMB);
    if (gold_of(g, c->p) > target) return false;
    if (c->fights.in_total >= (double)g->players.troops[c->p]) return false; /* under heavy attack: go all in */
    return mirv_worth_saving_for(c) && save_up_within_reach(c, target);
}

static bool hitting_us(const AiCtx *c, PlayerId other) {
    if (ai_incoming_from(c, other) > 0) return true;
    const UnitTable *u = &c->g->units;
    for (int32_t i = 0; i < u->count; i++) {
        if (!u->alive[i] || u->owner[i] != other || u->target_tile[i] < 0) continue;
        int t = u->type[i];
        if ((t == OFE_UNIT_ATOM_BOMB || t == OFE_UNIT_HYDROGEN_BOMB || t == OFE_UNIT_MIRV || t == OFE_UNIT_MIRV_WARHEAD) &&
            tile_owner(c->g, u->target_tile[i]) == c->p) {
            return true;
        }
    }
    return false;
}

/* Nukes: pick a target, then the best spot (structures in the blast, enemy
 * land covered, away from friends), hydrogen bombs first. */
void ai_build_maybe_nuke(AiCtx *c) {
    OfeGame *g = c->g;
    if (g->players.unit_count[c->p][OFE_UNIT_MISSILE_SILO] == 0 || !unit_enabled(g, OFE_UNIT_MISSILE_SILO)) return;
    if (!unit_enabled(g, OFE_UNIT_ATOM_BOMB) && !unit_enabled(g, OFE_UNIT_HYDROGEN_BOMB)) return;
    UnitList silos;
    list_units(g, c->p, BIT(OFE_UNIT_MISSILE_SILO), true, &silos);
    if (silos.n == 0) return;
    /* Cheap gate first: can we afford any bomb at all? */
    double gold = gold_of(g, c->p);
    if (gold < cost_of(c, OFE_UNIT_ATOM_BOMB) && gold < cost_of(c, OFE_UNIT_HYDROGEN_BOMB)) return;

    ai_ctx_scan(c);
    PlayerId target = best_nuke_target(c);
    if (!target || g->players.type[target] == OFE_PLAYER_BOT || player_same_team(g, c->p, target) || !ai_nation_should_attack(c, target)) return;

    bool saving = saving_for_mirv(c);
    if (saving && !hitting_us(c, target)) return;

    const NukeKind *kinds[2];
    int32_t nk = 0;
    if (!saving && unit_enabled(g, OFE_UNIT_HYDROGEN_BOMB) && gold >= cost_of(c, OFE_UNIT_HYDROGEN_BOMB)) kinds[nk++] = &HYDROGEN;
    if ((nk == 0 || hard_or_impossible(c)) && unit_enabled(g, OFE_UNIT_ATOM_BOMB) && gold >= cost_of(c, OFE_UNIT_ATOM_BOMB)) kinds[nk++] = &ATOM;
    if (nk == 0) return;

    UnitList structures;
    list_units(g, target, STRUCTURE_MASK, false, &structures);
    bool strike = hard_or_impossible(c) && can_strike(c, target);
    if (c->diff == OFE_DIFFICULTY_MEDIUM) strike = ai_incoming_from(c, target) >= 0 && c->fights.n_out > 0;
    for (int32_t i = 0; i < nk; i++) {
        TileRef tile = find_best_nuke_tile(c, kinds[i], target, &structures, &silos, strike);
        if (tile < 0) continue;
        issue_build(c, kinds[i]->type, tile, 1); /* rocket up */
        return;
    }
}

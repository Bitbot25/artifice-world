/*
 * nukes.c - atom / hydrogen bombs, MIRVs and their warheads, missile silo
 * cooldowns, SAM launchers and SAM missiles.
 *
 * Rules and constants follow OpenFront (NukeExecution, MIRVExecution,
 * SAMLauncherExecution, SAMMissileExecution, NukeAlliance, Config):
 *   - nuke speed 10 (atom / hydrogen), 15 (MIRV), 22+ (warhead) tiles/tick,
 *     flown along a bezier "parabola" (control points as in
 *     PathFinder.Parabola), one trajectory point per tick;
 *   - blast: inner radius always, outer ring with probability 1/2 per tile
 *     (atom 12/30, hydrogen 80/100, warhead 12/18); tiles lose their owner
 *     and get fallout, troops die by nukeDeathFactor, every non-nuke unit
 *     within the outer radius is destroyed;
 *   - alliances break with players whose weighted blast tiles exceed 100 or
 *     whose structures are in the blast (atoms / hydrogens only);
 *   - silos and SAM launchers hold one missile per level; each launched
 *     missile reloads for 90 ticks;
 *   - SAMs fire at enemy nukes whose trajectory enters their (level and
 *     upgrade dependent) range; the SAM missile flies 12 tiles/tick and
 *     destroys its target when it arrives.
 *
 * Simplifications vs the TypeScript engine (not bit-exact):
 *   - waterNukes is off (the OpenFront default): terrain is never turned
 *     into water, so only fallout and ownership change.
 *   - Trajectories are sampled from a cubic bezier at about one sample per
 *     tile and resampled at equal arc length (OpenFront uses fixed point
 *     subdivision); flight times match to within a tick.
 *   - SAM targeting keeps OpenFront's rules (range at the time of arrival,
 *     "targetable" band of 150 tiles around source and target, cooldown
 *     queue, scoring) but caches per (SAM, nuke) pair in a two-generation
 *     hash table instead of a per-SAM map; the results are identical.
 *   - The SAM missile flies a straight line and takes ceil(manhattan / 12)
 *     ticks; on arrival it destroys its target wherever it is.
 *   - MIRV warhead targets are drawn in one go ten ticks before separation
 *     (OpenFront stages them from tick 20 and tops up at tick 10), from the
 *     target player's tiles with the same 55-tile minimum spread, 350
 *     warhead cap and 1500 tile range.
 *   - Blast "tiles destroyed" in OFE_EV_NUKE_DETONATED counts land tiles
 *     inside the blast (owned or not).
 *   - Nuke death rolls loop per tile like OpenFront's but are done per pool
 *     (troops, each outgoing attack, each transport ship) with plain math.
 *
 * Unit column use:
 *   ATOM_BOMB / HYDROGEN_BOMB / MIRV_WARHEAD
 *     tile = position, src_tile = launch tile, target_tile = destination,
 *     path = trajectory, path_pos = index of the current point,
 *     aux0 = ticks left before launch (stacked launches / warhead stagger),
 *     target_unit = SAM missile chasing it (-1 none), auxf = speed.
 *   MIRV  as above; aux1 = warheads released, aux2 = target player at launch,
 *     path ends at the separation point.
 *   SAM_MISSILE
 *     src_tile = SAM tile, target_tile = interception tile, target_unit = the
 *     nuke, aux0 = launcher unit id, timer = arrival tick.
 *   MISSILE_SILO / SAM_LAUNCHER
 *     aux0 = missiles reloading, timer = tick the oldest finishes reloading
 *     while every missile is out (0 otherwise), OFE_UF_COOLDOWN set then.
 *     SAM launchers: aux3 = upgrade target level (0 = no ramp running),
 *     aux1 = upgrade start tick, auxf = range when the upgrade started.
 */
#include "internal.h"

/* ---- Constants (Config.ts / EngineConfig.ts) -------------------------------- */

#define SILO_COOLDOWN 90
#define SAM_COOLDOWN 90
#define SAM_UPGRADE_TICKS (SAM_COOLDOWN / 2)
#define SAM_MISSILE_SPEED 12
#define MAX_SAM_RANGE 150.0
#define SAM_DETECTION_RANGE (4 * 150)
#define NUKE_TARGETABLE_RANGE 150
#define NUKE_ALLIANCE_BREAK_THRESHOLD 100
#define ATOM_SPEED 10.0
#define HYDROGEN_SPEED 10.0
#define MIRV_SPEED 15.0
#define WARHEAD_SPEED 22.0
#define MIRV_NORMALIZE_TICKS 14
#define PARABOLA_MIN_HEIGHT 50.0

#define MIRV_RANGE 1500
#define MIRV_MIN_SPREAD 55
#define MIRV_WARHEAD_COUNT 350
#define MIRV_SATURATION 1500
#define MIRV_GRID_W ((MIRV_RANGE * 2) / MIRV_MIN_SPREAD + 2)
#define MIRV_SPAWN_LEAD 10 /* ticks before separation the warheads appear */

/* ---- State ---------------------------------------------------------------------- */

/* One cached SAM-vs-nuke interception result. tick >= 0 is the absolute tick
 * to fire at, -1 "out of reach at the SAM's current level", -2 "never in
 * reach". */
typedef struct SamCacheEntry {
    uint64_t key; /* 0 = empty */
    int32_t tick, tile, level, pad;
} SamCacheEntry;

typedef struct SamCache {
    SamCacheEntry *e;
    int32_t cap, len; /* cap is a power of two or 0 */
} SamCache;

struct NukesState {
    /* FIFO of (launcher unit id, tick its missile is reloaded). Silos and
     * SAMs share the same cooldown so appending keeps it sorted. */
    IntVec reloads;
    int32_t reload_head; /* index of the first live pair's first int */
    SamCache prev, cur; /* interception cache, this and the previous tick */

    /* Scratch, rebuilt every tick and never cloned. */
    IntVec nukes, mirvs, missiles, sams; /* unit ids */
    IntVec targets; /* triples (unit id, tile, score) */
    IntVec tiles; /* blast area / candidate tiles */
    IntVec touched; /* players touched by a blast */
    int32_t *pcount;
    int32_t pcount_cap;
};

NukesState *nukes_state_new(OfeGame *g) {
    OFE_UNUSED(g);
    return (NukesState *)ofe_xcalloc(1, sizeof(NukesState));
}

static void cache_free(SamCache *c) {
    free(c->e);
    memset(c, 0, sizeof(*c));
}

static void cache_copy(SamCache *dst, const SamCache *src) {
    if (dst->cap != src->cap) {
        free(dst->e);
        dst->e = src->cap ? (SamCacheEntry *)ofe_xmalloc((size_t)src->cap * sizeof(SamCacheEntry)) : NULL;
        dst->cap = src->cap;
    }
    if (src->cap) memcpy(dst->e, src->e, (size_t)src->cap * sizeof(SamCacheEntry));
    dst->len = src->len;
}

void nukes_state_copy(OfeGame *dst, const OfeGame *src) {
    NukesState *d = dst->sub->nukes, *s = src->sub->nukes;
    if (!d) d = dst->sub->nukes = nukes_state_new(dst);
    IntVec_copy(&d->reloads, &s->reloads);
    d->reload_head = s->reload_head;
    cache_copy(&d->prev, &s->prev);
    cache_copy(&d->cur, &s->cur);
}

void nukes_state_free(OfeGame *g) {
    NukesState *st = g->sub->nukes;
    if (!st) return;
    IntVec_free(&st->reloads);
    cache_free(&st->prev);
    cache_free(&st->cur);
    IntVec_free(&st->nukes);
    IntVec_free(&st->mirvs);
    IntVec_free(&st->missiles);
    IntVec_free(&st->sams);
    IntVec_free(&st->targets);
    IntVec_free(&st->tiles);
    IntVec_free(&st->touched);
    free(st->pcount);
    free(st);
    g->sub->nukes = NULL;
}

/* ---- Small helpers --------------------------------------------------------------- */

static bool is_flying_nuke(int type) {
    return type == OFE_UNIT_ATOM_BOMB || type == OFE_UNIT_HYDROGEN_BOMB || type == OFE_UNIT_MIRV_WARHEAD;
}

static bool is_any_nuke(int type) { return is_flying_nuke(type) || type == OFE_UNIT_MIRV; }

static void magnitude(int type, int32_t *inner, int32_t *outer) {
    switch (type) {
    case OFE_UNIT_HYDROGEN_BOMB: *inner = 80; *outer = 100; break;
    case OFE_UNIT_MIRV_WARHEAD: *inner = 12; *outer = 18; break;
    default: *inner = 12; *outer = 30; break;
    }
}

static double nuke_speed(int type) {
    switch (type) {
    case OFE_UNIT_MIRV: return MIRV_SPEED;
    case OFE_UNIT_MIRV_WARHEAD: return WARHEAD_SPEED;
    case OFE_UNIT_HYDROGEN_BOMB: return HYDROGEN_SPEED;
    default: return ATOM_SPEED;
    }
}

static int32_t ceil_div(int32_t a, int32_t b) { return (a + b - 1) / b; }

/* A per-event random stream, independent of call order elsewhere. */
static uint64_t event_rng(const OfeGame *g, UnitId id, uint64_t salt) {
    return rng_derive(g->seed ^ salt, ((uint64_t)(uint32_t)id << 24) ^ (uint64_t)(uint32_t)g->tick);
}

static bool game_decided(const OfeGame *g) { return g->game_over && (g->winner != 0 || g->winning_team >= 0); }

/* ---- Reload queue (silo and SAM cooldowns) ------------------------------------------ */

static void reload_push(NukesState *st, UnitId id, int32_t end_tick) {
    IntVec_push(&st->reloads, id);
    IntVec_push(&st->reloads, end_tick);
}

/* Tick the oldest missile of this launcher is back, or -1. */
static int32_t reload_front_end(const NukesState *st, UnitId id) {
    for (int32_t i = st->reload_head; i < st->reloads.len; i += 2) {
        if (st->reloads.data[i] == id) return st->reloads.data[i + 1];
    }
    return -1;
}

/* Brings timer / cooldown flag in line with the number of missiles out. */
static void launcher_sync(OfeGame *g, const NukesState *st, int32_t slot) {
    UnitTable *u = &g->units;
    if (u->aux0[slot] >= u->level[slot]) {
        int32_t end = reload_front_end(st, unit_id_of_slot(g, slot));
        u->flags[slot] |= OFE_UF_COOLDOWN;
        u->timer[slot] = end >= 0 ? end : g->tick;
    } else {
        u->flags[slot] &= (uint8_t)~OFE_UF_COOLDOWN;
        u->timer[slot] = 0;
    }
}

static void launcher_fire(OfeGame *g, NukesState *st, int32_t slot, int32_t cooldown) {
    g->units.aux0[slot]++;
    reload_push(st, unit_id_of_slot(g, slot), g->tick + cooldown);
    launcher_sync(g, st, slot);
}

static void reloads_tick(OfeGame *g, NukesState *st) {
    while (st->reload_head < st->reloads.len && st->reloads.data[st->reload_head + 1] <= g->tick) {
        int32_t slot = unit_lookup(g, st->reloads.data[st->reload_head]);
        st->reload_head += 2;
        if (slot < 0) continue;
        if (g->units.aux0[slot] > 0) g->units.aux0[slot]--;
        launcher_sync(g, st, slot);
    }
    if (st->reload_head >= 64 && st->reload_head * 2 >= st->reloads.len) {
        int32_t rest = st->reloads.len - st->reload_head;
        memmove(st->reloads.data, st->reloads.data + st->reload_head, (size_t)rest * sizeof(int32_t));
        st->reloads.len = rest;
        st->reload_head = 0;
    } else if (st->reload_head == st->reloads.len) {
        st->reloads.len = st->reload_head = 0;
    }
}

/* Ready for another missile? Counts reloads that are due this tick but not
 * yet processed (intents run before nukes_tick). */
static bool launcher_ready(const OfeGame *g, int32_t slot) {
    const UnitTable *u = &g->units;
    if (!u->alive[slot] || (u->flags[slot] & OFE_UF_UNDER_CONSTRUCTION)) return false;
    int32_t busy = u->aux0[slot];
    if (busy >= u->level[slot]) {
        const NukesState *st = g->sub->nukes;
        UnitId id = unit_id_of_slot(g, slot);
        for (int32_t i = st->reload_head; i < st->reloads.len && st->reloads.data[i + 1] <= g->tick; i += 2) {
            if (st->reloads.data[i] == id) busy--;
        }
    }
    return busy < u->level[slot];
}

/* ---- SAM range ------------------------------------------------------------------------- */

static double sam_range_for_level(double level) { return MAX_SAM_RANGE - 480.0 / (level + 5.0); }

/* Range at a given tick, following an upgrade ramp if one is running. */
static double sam_range_at(const OfeGame *g, int32_t slot, int32_t tick) {
    const UnitTable *u = &g->units;
    if (u->aux3[slot] <= 0) return sam_range_for_level(u->level[slot]);
    int32_t elapsed = tick - u->aux1[slot];
    double target = sam_range_for_level(u->aux3[slot]);
    if (elapsed >= SAM_UPGRADE_TICKS) return sam_range_for_level(u->level[slot]);
    if (elapsed < 0) elapsed = 0;
    return u->auxf[slot] + (target - u->auxf[slot]) * elapsed / SAM_UPGRADE_TICKS;
}

void nukes_on_level_up(OfeGame *g, int32_t slot) {
    NukesState *st = g->sub->nukes;
    UnitTable *u = &g->units;
    if (slot < 0 || slot >= u->count || !u->alive[slot]) return;
    int type = u->type[slot];
    if (type != OFE_UNIT_SAM_LAUNCHER && type != OFE_UNIT_MISSILE_SILO) return;
    if (type == OFE_UNIT_SAM_LAUNCHER) {
        double start = u->aux3[slot] > 0 ? sam_range_at(g, slot, g->tick) : sam_range_for_level(u->level[slot] - 1);
        u->aux1[slot] = g->tick;
        u->auxf[slot] = start;
        u->aux3[slot] = u->level[slot];
    }
    /* The new missile starts out reloading, like in OpenFront. */
    launcher_fire(g, st, slot, type == OFE_UNIT_SAM_LAUNCHER ? SAM_COOLDOWN : SILO_COOLDOWN);
}

/* ---- Trajectories -------------------------------------------------------------------------- */

typedef struct Bezier {
    double p[4][2];
} Bezier;

/* Control points as PathFinder.Parabola builds them: the curve bulges
 * towards the top (rocket up) or bottom of the map by max(dist / 3, 50). */
static Bezier bezier_controls(const OfeMap *m, TileRef from, TileRef to, bool up, bool ignore_bounds) {
    double x0 = map_x(m, from), y0 = map_y(m, from);
    double x3 = map_x(m, to), y3 = map_y(m, to);
    double dx = x3 - x0, dy = y3 - y0;
    double height = OFE_MAX(sqrt(dx * dx + dy * dy) / 3.0, PARABOLA_MIN_HEIGHT);
    double mult = up ? -1.0 : 1.0;
    double y1 = y0 + dy / 4.0 + mult * height;
    double y2 = y0 + dy * 3.0 / 4.0 + mult * height;
    if (!ignore_bounds) {
        y1 = OFE_CLAMP(y1, 0.0, (double)(m->height - 1));
        y2 = OFE_CLAMP(y2, 0.0, (double)(m->height - 1));
    }
    Bezier b = {{{x0, y0}, {x0 + dx / 4.0, y1}, {x0 + dx * 3.0 / 4.0, y2}, {x3, y3}}};
    return b;
}

static void bezier_eval(const Bezier *b, double t, double *x, double *y) {
    double s = 1.0 - t;
    double w0 = s * s * s, w1 = 3.0 * s * s * t, w2 = 3.0 * s * t * t, w3 = t * t * t;
    *x = w0 * b->p[0][0] + w1 * b->p[1][0] + w2 * b->p[2][0] + w3 * b->p[3][0];
    *y = w0 * b->p[0][1] + w1 * b->p[1][1] + w2 * b->p[2][1] + w3 * b->p[3][1];
}

/* Samples per curve: about one per tile of control polygon length. */
static int32_t bezier_steps(const Bezier *b) {
    double len = 0;
    for (int i = 0; i < 3; i++) len += hypot(b->p[i + 1][0] - b->p[i][0], b->p[i + 1][1] - b->p[i][1]);
    return (int32_t)OFE_CLAMP(ceil(len) + 4.0, 8.0, 40000.0);
}

static double bezier_length(const Bezier *b) {
    int32_t steps = bezier_steps(b);
    double len = 0, px = b->p[0][0], py = b->p[0][1];
    for (int32_t k = 1; k <= steps; k++) {
        double x, y;
        bezier_eval(b, (double)k / steps, &x, &y);
        len += hypot(x - px, y - py);
        px = x;
        py = y;
    }
    return len;
}

static TileRef tile_at(const OfeMap *m, double x, double y) {
    int32_t xi = (int32_t)OFE_CLAMP(floor(x + 0.5), 0.0, (double)(m->width - 1));
    int32_t yi = (int32_t)OFE_CLAMP(floor(y + 0.5), 0.0, (double)(m->height - 1));
    return map_ref(m, xi, yi);
}

/* Trajectory from -> to with one point per tick: points are `speed` tiles
 * apart along the curve; the first is `from`, the last is `to`. */
static void parabola_path(const OfeMap *m, IntVec *out, TileRef from, TileRef to, double speed, bool up) {
    Bezier b = bezier_controls(m, from, to, up, false);
    int32_t steps = bezier_steps(&b);
    double spacing = OFE_MAX(1.0, speed);
    out->len = 0;
    IntVec_push(out, from);
    double next = spacing, acc = 0, px = b.p[0][0], py = b.p[0][1];
    for (int32_t k = 1; k <= steps; k++) {
        double x, y;
        bezier_eval(&b, (double)k / steps, &x, &y);
        double seg = hypot(x - px, y - py);
        while (acc + seg >= next) {
            double f = (next - acc) / seg;
            IntVec_push(out, tile_at(m, px + (x - px) * f, py + (y - py) * f));
            next += spacing;
        }
        acc += seg;
        px = x;
        py = y;
    }
    if (out->data[out->len - 1] != to) IntVec_push(out, to);
}

/* MIRV flight speed: normalise the flight time towards 14 ticks, long
 * flights shortened by sqrt + linear terms (MIRVExecution). */
static double mirv_speed(const OfeMap *m, TileRef from, TileRef sep) {
    Bezier ideal = bezier_controls(m, from, sep, true, true);
    double ideal_ticks = bezier_length(&ideal) / MIRV_SPEED;
    int64_t base = (int64_t)MIRV_NORMALIZE_TICKS * 100;
    int64_t ideal_int = (int64_t)floor(ideal_ticks * 100.0);
    int64_t target = base;
    if (ideal_int > base) {
        int64_t diff = ideal_int - base;
        target = base + (int64_t)floor(sqrt((double)diff)) * 14 + diff * 10 / 100;
    } else if (ideal_int < base) {
        int64_t diff = base - ideal_int;
        target = base - (int64_t)floor(sqrt((double)diff)) * 10;
    }
    target = OFE_MAX(target, (int64_t)100);
    Bezier actual = bezier_controls(m, from, sep, true, false);
    return OFE_MAX(1.0, bezier_length(&actual) * 100.0 / (double)target);
}

/* ---- Launch validation ------------------------------------------------------------------------ */

/* Is this tile a legal nuke target for p? Spawn immunity, impassable ground,
 * teammates' land and (team games) teammates' structures in the blast. */
static bool target_allowed(const OfeGame *g, PlayerId p, int type, TileRef t) {
    const OfeMap *m = g->map;
    if (t < 0 || t >= m->num_tiles) return false;
    if (g->start_tick < 0 || g->tick - g->start_tick < g->cfg.spawn_immunity_ticks) return false;
    if (map_is_impassable(m, t)) return false;
    PlayerId owner = tile_owner(g, t);
    bool decided = game_decided(g);
    if (owner > 0 && owner != p && player_same_team(g, p, owner) && !decided) return false;
    if (type == OFE_UNIT_MIRV && owner == 0) return false;
    if (g->cfg.game_mode == OFE_MODE_TEAM && type != OFE_UNIT_MIRV && !decided) {
        int32_t inner, outer;
        magnitude(type, &inner, &outer);
        int64_t r2 = (int64_t)outer * outer;
        const UnitTable *u = &g->units;
        for (int32_t i = 0; i < u->count; i++) {
            if (!u->alive[i] || !unit_is_structure(u->type[i])) continue;
            PlayerId o = u->owner[i];
            if (o == p || !player_same_team(g, p, o)) continue;
            if (map_dist2(m, t, u->tile[i]) <= r2) return false;
        }
    }
    return true;
}

int32_t nukes_pick_silo(const OfeGame *g, PlayerId p, TileRef target) {
    const UnitTable *u = &g->units;
    int32_t best = -1;
    int32_t best_d = 0;
    for (int32_t i = 0; i < u->count; i++) {
        if (!u->alive[i] || u->type[i] != OFE_UNIT_MISSILE_SILO || u->owner[i] != p || !launcher_ready(g, i)) continue;
        int32_t d = map_manhattan(g->map, u->tile[i], target);
        if (best < 0 || d < best_d) {
            best = i;
            best_d = d;
        }
    }
    return best;
}

bool nukes_can_launch(const OfeGame *g, PlayerId p, int type, TileRef target) {
    if (!player_valid(g, p) || !player_alive(g, p)) return false;
    if (type != OFE_UNIT_ATOM_BOMB && type != OFE_UNIT_HYDROGEN_BOMB && type != OFE_UNIT_MIRV) return false;
    if (g->cfg.disabled_units & (1u << type)) return false;
    if (!target_allowed(g, p, type, target)) return false;
    return nukes_pick_silo(g, p, target) >= 0;
}

/* ---- Alliance consequences -------------------------------------------------------------------------- */

static int32_t *pcount_get(const OfeGame *g, NukesState *st) {
    if (st->pcount_cap < g->players.cap) {
        st->pcount = (int32_t *)ofe_xrealloc(st->pcount, (size_t)g->players.cap * sizeof(int32_t));
        memset(st->pcount + st->pcount_cap, 0, (size_t)(g->players.cap - st->pcount_cap) * sizeof(int32_t));
        st->pcount_cap = g->players.cap;
    }
    return st->pcount;
}

/* Twice the weighted blast tile count per owner (inner tiles weigh 1, outer
 * 0.5), over the whole circle whether or not tiles survive the random ring.
 * Players with tiles are listed in st->touched; callers zero the counts. */
static void blast_weights(const OfeGame *g, NukesState *st, TileRef c, int32_t inner, int32_t outer) {
    const OfeMap *m = g->map;
    int32_t *cnt = pcount_get(g, st);
    int32_t cx = map_x(m, c), cy = map_y(m, c);
    int64_t i2 = (int64_t)inner * inner, o2 = (int64_t)outer * outer;
    st->touched.len = 0;
    for (int32_t y = OFE_MAX(0, cy - outer); y <= OFE_MIN(m->height - 1, cy + outer); y++) {
        for (int32_t x = OFE_MAX(0, cx - outer); x <= OFE_MIN(m->width - 1, cx + outer); x++) {
            int64_t d2 = (int64_t)(x - cx) * (x - cx) + (int64_t)(y - cy) * (y - cy);
            if (d2 > o2) continue;
            PlayerId o = tile_owner(g, map_ref(m, x, y));
            if (o == 0) continue;
            if (cnt[o] == 0) IntVec_push(&st->touched, o);
            cnt[o] += d2 <= i2 ? 2 : 1;
        }
    }
}

static void touched_reset(NukesState *st) {
    for (int32_t i = 0; i < st->touched.len; i++) st->pcount[st->touched.data[i]] = 0;
    st->touched.len = 0;
}

static int cmp_int(const void *a, const void *b) {
    int32_t x = *(const int32_t *)a, y = *(const int32_t *)b;
    return (x > y) - (x < y);
}

/* NukeExecution.maybeBreakAlliances: everyone hit hard enough (or with a
 * structure in the blast) loses its alliance with the launcher and its
 * trust; pending alliance requests between them are rejected. */
static void break_alliances_for_blast(OfeGame *g, NukesState *st, PlayerId nuker, TileRef c, int type) {
    int32_t inner, outer;
    magnitude(type, &inner, &outer);
    blast_weights(g, st, c, inner, outer);
    int32_t *cnt = st->pcount;
    IntVec angry = {0};
    for (int32_t i = 0; i < st->touched.len; i++) {
        PlayerId o = st->touched.data[i];
        if (cnt[o] > 2 * NUKE_ALLIANCE_BREAK_THRESHOLD) IntVec_push(&angry, o);
    }
    touched_reset(st);
    const UnitTable *u = &g->units;
    int64_t o2 = (int64_t)outer * outer;
    for (int32_t i = 0; i < u->count; i++) {
        if (!u->alive[i] || !unit_is_structure(u->type[i])) continue;
        if (map_dist2(g->map, c, u->tile[i]) <= o2) IntVec_push(&angry, u->owner[i]);
    }
    if (angry.len > 1) qsort(angry.data, (size_t)angry.len, sizeof(int32_t), cmp_int);

    /* Requests to the launcher from anyone angry are rejected. */
    IntVec reject = {0};
    for (int32_t i = 0; i < g->alliance_requests.len; i++) {
        const AllianceRequest *r = &g->alliance_requests.data[i];
        bool a_hit = false, b_hit = false;
        for (int32_t k = 0; k < angry.len; k++) {
            if (angry.data[k] == r->requestor) a_hit = true;
            if (angry.data[k] == r->recipient) b_hit = true;
        }
        if ((r->recipient == nuker && a_hit) || (r->requestor == nuker && b_hit)) {
            IntVec_push(&reject, r->requestor);
            IntVec_push(&reject, r->recipient);
        }
    }
    for (int32_t i = 0; i < reject.len; i += 2) alliance_reject(g, reject.data[i], reject.data[i + 1]);

    PlayerId last = -1;
    for (int32_t i = 0; i < angry.len; i++) {
        PlayerId o = angry.data[i];
        if (o == last) continue;
        last = o;
        bool had_request = false;
        for (int32_t k = 0; k < reject.len; k += 2) had_request |= reject.data[k] == nuker && reject.data[k + 1] == o;
        if (had_request) continue;
        if (player_allied(g, nuker, o)) alliance_break(g, nuker, o);
        if (o != nuker) player_update_relation(g, o, nuker, -100);
    }
    IntVec_free(&angry);
    IntVec_free(&reject);
}

/* Would the strike on c hit player `other` hard enough to count as an
 * attack on them (weighted tiles above the threshold or a structure)? */
static bool blast_hits_player(const OfeGame *g, NukesState *st, PlayerId other, TileRef c, int type) {
    int32_t inner, outer;
    magnitude(type, &inner, &outer);
    const UnitTable *u = &g->units;
    int64_t o2 = (int64_t)outer * outer;
    for (int32_t i = 0; i < u->count; i++) {
        if (u->alive[i] && u->owner[i] == other && unit_is_structure(u->type[i]) && map_dist2(g->map, c, u->tile[i]) <= o2) return true;
    }
    blast_weights(g, st, c, inner, outer);
    bool hit = st->pcount[other] > 2 * NUKE_ALLIANCE_BREAK_THRESHOLD;
    touched_reset(st);
    return hit;
}

void nukes_cancel_between(OfeGame *g, PlayerId a, PlayerId b) {
    NukesState *st = g->sub->nukes;
    UnitTable *u = &g->units;
    IntVec doomed = {0};
    for (int32_t i = 0; i < u->count; i++) {
        if (!u->alive[i] || !is_any_nuke(u->type[i])) continue;
        PlayerId owner = u->owner[i];
        if (owner != a && owner != b) continue;
        PlayerId other = owner == a ? b : a;
        TileRef dst = u->target_tile[i];
        if (dst < 0) continue;
        bool cancel;
        if (u->type[i] == OFE_UNIT_MIRV) cancel = u->aux2[i] == other;
        else if (u->type[i] == OFE_UNIT_MIRV_WARHEAD) cancel = tile_owner(g, dst) == other;
        else cancel = blast_hits_player(g, st, other, dst, u->type[i]);
        if (cancel) IntVec_push(&doomed, unit_id_of_slot(g, i));
    }
    for (int32_t i = 0; i < doomed.len; i++) unit_destroy(g, doomed.data[i], 0);
    IntVec_free(&doomed);
}

/* ---- Launch ------------------------------------------------------------------------------------------ */

/* Ticks a new launch from this silo must wait so stacked missiles trail each
 * other instead of overlapping (NukeExecution). */
static int32_t silo_stagger(const OfeGame *g, const NukesState *st, UnitId silo) {
    int32_t last = 0;
    for (int32_t i = st->reload_head; i < st->reloads.len; i += 2) {
        if (st->reloads.data[i] != silo) continue;
        int32_t launched = st->reloads.data[i + 1] - SILO_COOLDOWN;
        last = OFE_MAX(launched + 1, last + 1);
    }
    return last > g->tick ? last - g->tick : 0;
}

UnitId nukes_launch(OfeGame *g, PlayerId p, int type, TileRef target, int32_t src_slot, bool rocket_up, bool free_launch) {
    NukesState *st = g->sub->nukes;
    const OfeMap *m = g->map;
    reloads_tick(g, st);
    if (!player_valid(g, p)) return -1;
    if (type != OFE_UNIT_ATOM_BOMB && type != OFE_UNIT_HYDROGEN_BOMB && type != OFE_UNIT_MIRV) return -1;
    if (target < 0 || target >= m->num_tiles) return -1;

    int32_t silo = -1;
    if (free_launch) {
        if (src_slot >= 0 && src_slot < g->units.count && g->units.alive[src_slot]) silo = src_slot;
    } else {
        if (!player_alive(g, p) || (g->cfg.disabled_units & (1u << type))) return -1;
        if (!target_allowed(g, p, type, target)) return -1;
        if (src_slot >= 0) {
            if (src_slot < g->units.count && g->units.type[src_slot] == OFE_UNIT_MISSILE_SILO && g->units.owner[src_slot] == p &&
                launcher_ready(g, src_slot))
                silo = src_slot;
        } else {
            silo = nukes_pick_silo(g, p, target);
        }
    }
    if (silo < 0) return -1;

    int64_t cost = free_launch ? 0 : unit_cost(g, p, type);
    if (g->players.gold[p] < cost) return -1;
    player_remove_gold(g, p, cost);

    UnitId silo_id = unit_id_of_slot(g, silo);
    TileRef src = g->units.tile[silo];
    int32_t wait = silo_stagger(g, st, silo_id);
    PlayerId target_owner = tile_owner(g, target);

    UnitId id = unit_create(g, type, p, src);
    if (id < 0) return -1;
    int32_t slot = unit_slot(id);
    UnitTable *u = &g->units;
    u->target_tile[slot] = target;
    u->src_tile[slot] = src;
    u->aux0[slot] = type == OFE_UNIT_MIRV ? 0 : wait; /* MIRVs are not staggered */
    u->aux2[slot] = target_owner;
    u->path_pos[slot] = 0;
    if (type == OFE_UNIT_MIRV) {
        /* Flies to a point above the target, then splits. */
        int32_t sx = (map_x(m, target) + map_x(m, src)) / 2;
        int32_t sy = OFE_MAX(0, map_y(m, target) - 500) + 50;
        TileRef sep = map_ref(m, OFE_CLAMP(sx, 0, m->width - 1), OFE_CLAMP(sy, 0, m->height - 1));
        double speed = mirv_speed(m, src, sep);
        u->auxf[slot] = speed;
        parabola_path(m, &u->path[slot], src, sep, speed, true);
        g->mirvs_launched++;
        if (target_owner > 0) {
            if (player_allied(g, p, target_owner)) alliance_break(g, p, target_owner);
            if (target_owner != p) {
                player_update_relation(g, target_owner, p, -100);
                player_update_relation(g, p, target_owner, -100);
            }
        }
    } else {
        double speed = nuke_speed(type);
        u->auxf[slot] = speed;
        parabola_path(m, &u->path[slot], src, target, speed, rocket_up);
        break_alliances_for_blast(g, st, p, target, type);
    }
    launcher_fire(g, st, silo, SILO_COOLDOWN);
    event_emit(g, OFE_EV_NUKE_LAUNCHED, id, type, p, target, 0);
    return id;
}

/* ---- Detonation ----------------------------------------------------------------------------------------- */

/* Troops killed per tile of blast (Config.nukeDeathFactor). */
static double death_factor(int type, double humans, double tiles_left, double max_troops) {
    if (type != OFE_UNIT_MIRV_WARHEAD) return 5.0 * humans / OFE_MAX(1.0, tiles_left);
    if (max_troops <= 0) return 0;
    double excess = OFE_MAX(0.0, humans - 0.03 * max_troops);
    return 500.0 * (1.0 - exp(-2.0 * excess / max_troops));
}

static double pool_after_blast(int type, double pool, int32_t tiles_before, int32_t hit, double max_troops) {
    for (int32_t i = 0; i < hit && pool > 0; i++) {
        pool -= death_factor(type, pool, (double)(tiles_before - i), max_troops);
        if (pool < 0) pool = 0;
    }
    return pool;
}

/* Tiles of the blast: breadth-first from the target, inner circle always,
 * outer ring each tile with probability 1/2 (tiles stay connected). */
static void blast_area(OfeGame *g, NukesState *st, TileRef c, int32_t inner, int32_t outer, uint64_t *rng) {
    const OfeMap *m = g->map;
    int64_t i2 = (int64_t)inner * inner, o2 = (int64_t)outer * outer;
    IntVec *q = &st->tiles;
    q->len = 0;
    uint32_t gen = tiles_new_visit_gen(g);
    uint32_t *seen = g->visit_stamp;
    if (map_is_impassable(m, c)) return;
    seen[c] = gen;
    IntVec_push(q, c);
    for (int32_t head = 0; head < q->len; head++) {
        TileRef nb[4];
        int n = map_neighbors4(m, q->data[head], nb);
        for (int k = 0; k < n; k++) {
            TileRef t = nb[k];
            if (seen[t] == gen) continue;
            seen[t] = gen;
            int64_t d2 = map_dist2(m, c, t);
            if (d2 > o2 || map_is_impassable(m, t)) continue;
            if (d2 > i2 && !rng_chance(rng, 2)) continue;
            IntVec_push(q, t);
        }
    }
}

static void detonate(OfeGame *g, NukesState *st, int32_t slot) {
    UnitTable *u = &g->units;
    UnitId id = unit_id_of_slot(g, slot);
    int type = u->type[slot];
    PlayerId nuker = u->owner[slot];
    TileRef dst = u->target_tile[slot];
    int32_t inner, outer;
    magnitude(type, &inner, &outer);
    uint64_t rng = event_rng(g, id, 0xB10C7);

    blast_area(g, st, dst, inner, outer, &rng);
    int32_t *cnt = pcount_get(g, st);
    st->touched.len = 0;
    int32_t land = 0;
    for (int32_t i = 0; i < st->tiles.len; i++) {
        TileRef t = st->tiles.data[i];
        PlayerId o = tile_owner(g, t);
        if (o != 0) {
            tile_set_owner(g, t, 0);
            if (cnt[o] == 0) IntVec_push(&st->touched, o);
            cnt[o]++;
        }
        if (map_is_land(g->map, t)) {
            tile_set_fallout(g, t, true);
            land++;
        }
    }

    /* Troops of every hit player, their attacks in progress and their boats. */
    for (int32_t i = 0; i < st->touched.len; i++) {
        PlayerId p = st->touched.data[i];
        int32_t hit = cnt[p];
        int32_t before = g->players.num_tiles[p] + hit;
        double max_troops = player_max_troops(g, p);
        for (int32_t k = 0; k < hit && g->players.troops[p] > 0; k++) {
            double d = death_factor(type, (double)g->players.troops[p], (double)(before - k), max_troops);
            player_remove_troops(g, p, d);
        }
        AttackTable *at = &g->attacks;
        for (int32_t a = 0; a < at->count; a++) {
            if (at->active[a] && at->attacker[a] == p) at->troops[a] = pool_after_blast(type, at->troops[a], before, hit, max_troops);
        }
        for (int32_t s = 0; s < u->count; s++) {
            if (u->alive[s] && u->type[s] == OFE_UNIT_TRANSPORT_SHIP && u->owner[s] == p) {
                u->troops[s] = pool_after_blast(type, u->troops[s], before, hit, max_troops);
            }
        }
    }
    touched_reset(st);

    /* Everything but nukes and SAM missiles inside the outer radius dies. */
    int64_t o2 = (int64_t)outer * outer;
    for (int32_t s = 0; s < u->count; s++) {
        if (!u->alive[s] || is_any_nuke(u->type[s]) || u->type[s] == OFE_UNIT_SAM_MISSILE) continue;
        if (map_dist2(g->map, dst, u->tile[s]) >= o2) continue;
        PlayerId o = u->owner[s];
        bool friendly = player_same_team(g, nuker, o) || player_allied(g, nuker, o);
        unit_destroy(g, unit_id_of_slot(g, s), friendly ? 0 : nuker);
    }

    event_emit(g, OFE_EV_NUKE_DETONATED, id, type, nuker, dst, land);
    unit_destroy(g, id, 0);
}

/* ---- Flying nukes ------------------------------------------------------------------------------------------- */

/* A nuke added without a trajectory (state edits) gets one on its first tick. */
static bool ensure_path(OfeGame *g, int32_t slot) {
    UnitTable *u = &g->units;
    if (u->path[slot].len > 0) return true;
    if (u->target_tile[slot] < 0) return false;
    double speed = u->auxf[slot] > 0 ? u->auxf[slot] : nuke_speed(u->type[slot]);
    u->auxf[slot] = speed;
    parabola_path(g->map, &u->path[slot], u->tile[slot], u->target_tile[slot], speed, true);
    u->path_pos[slot] = 0;
    return true;
}

/* Is a SAM missile about to hit this nuke? Then it must not detonate. */
static bool sam_hit_imminent(const OfeGame *g, int32_t slot) {
    int32_t m = unit_lookup(g, g->units.target_unit[slot]);
    return m >= 0 && g->units.timer[m] <= g->tick + 1;
}

static void nuke_tick(OfeGame *g, NukesState *st, UnitId id) {
    int32_t slot = unit_lookup(g, id);
    if (slot < 0) return;
    UnitTable *u = &g->units;
    if (u->aux0[slot] > 0) {
        u->aux0[slot]--;
        return;
    }
    if (!ensure_path(g, slot)) {
        unit_destroy(g, id, 0);
        return;
    }
    int32_t n = u->path[slot].len;
    if (u->path_pos[slot] < n - 1) {
        u->path_pos[slot]++;
        u->tile[slot] = u->path[slot].data[u->path_pos[slot]];
    }
    if (u->path_pos[slot] >= n - 1 && !sam_hit_imminent(g, slot)) detonate(g, st, slot);
}

/* ---- MIRV ------------------------------------------------------------------------------------------------------ */

typedef struct MirvGrid {
    TileRef cells[MIRV_GRID_W * MIRV_GRID_W * 2];
} MirvGrid;

static int32_t grid_cell(const OfeMap *m, TileRef base, TileRef t, int32_t *cx, int32_t *cy) {
    *cx = (map_x(m, t) - map_x(m, base) + MIRV_RANGE) / MIRV_MIN_SPREAD;
    *cy = (map_y(m, t) - map_y(m, base) + MIRV_RANGE) / MIRV_MIN_SPREAD;
    return *cx >= 0 && *cx < MIRV_GRID_W && *cy >= 0 && *cy < MIRV_GRID_W;
}

static bool grid_overlaps(const OfeMap *m, const MirvGrid *gr, TileRef base, TileRef t) {
    int32_t cx, cy;
    if (!grid_cell(m, base, t, &cx, &cy)) return true;
    if (gr->cells[(cy * MIRV_GRID_W + cx) * 2 + 1] != -1) return true; /* cell full */
    int32_t x = map_x(m, t), y = map_y(m, t);
    for (int32_t gy = OFE_MAX(0, cy - 1); gy <= OFE_MIN(MIRV_GRID_W - 1, cy + 1); gy++) {
        for (int32_t gx = OFE_MAX(0, cx - 1); gx <= OFE_MIN(MIRV_GRID_W - 1, cx + 1); gx++) {
            for (int k = 0; k < 2; k++) {
                TileRef o = gr->cells[(gy * MIRV_GRID_W + gx) * 2 + k];
                if (o == -1) break;
                int32_t dx = abs(x - map_x(m, o)), dy = abs(y - map_y(m, o));
                if (dx < MIRV_MIN_SPREAD && dx + dy < MIRV_MIN_SPREAD) return true;
            }
        }
    }
    return false;
}

static void grid_add(const OfeMap *m, MirvGrid *gr, TileRef base, TileRef t) {
    int32_t cx, cy;
    if (!grid_cell(m, base, t, &cx, &cy)) return;
    for (int k = 0; k < 2; k++) {
        TileRef *c = &gr->cells[(cy * MIRV_GRID_W + cx) * 2 + k];
        if (*c == -1) {
            *c = t;
            return;
        }
    }
}

static int cmp_targets(const void *pa, const void *pb, void *ctx) {
    const OfeMap *m = ((void **)ctx)[0];
    TileRef base = *(TileRef *)((void **)ctx)[1];
    TileRef a = *(const TileRef *)pa, b = *(const TileRef *)pb;
    int32_t da = map_manhattan(m, a, base), db = map_manhattan(m, b, base);
    if (da != db) return da < db ? 1 : -1; /* farthest first */
    return (a > b) - (a < b);
}

/* Picks up to 350 warhead targets on the target player's land within 1500
 * tiles, at least 55 tiles (manhattan) apart. The impact tile comes first. */
static void mirv_pick_targets(OfeGame *g, NukesState *st, PlayerId victim, TileRef dst, uint64_t *rng, IntVec *out) {
    const OfeMap *m = g->map;
    out->len = 0;
    IntVec_push(out, dst);
    if (victim <= 0 || !player_alive(g, victim)) return;
    tiles_collect_player(g, victim, &st->tiles);
    /* Keep tiles in range; the list doubles as the candidate pool. */
    int64_t r2 = (int64_t)MIRV_RANGE * MIRV_RANGE;
    int32_t n = 0;
    for (int32_t i = 0; i < st->tiles.len; i++) {
        TileRef t = st->tiles.data[i];
        if (map_dist2(m, dst, t) <= r2 && !map_is_impassable(m, t)) st->tiles.data[n++] = t;
    }
    st->tiles.len = n;
    if (n == 0) return;
    /* Tile order out of the flood fill depends on border order; sorting
     * keeps picks independent of it. */
    qsort(st->tiles.data, (size_t)n, sizeof(int32_t), cmp_int);

    MirvGrid *gr = (MirvGrid *)ofe_xmalloc(sizeof(MirvGrid));
    for (size_t i = 0; i < sizeof(gr->cells) / sizeof(gr->cells[0]); i++) gr->cells[i] = -1;
    grid_add(m, gr, dst, dst);
    int32_t failures = 0;
    for (int32_t attempts = 0; attempts < 40 * MIRV_WARHEAD_COUNT + 2 * MIRV_SATURATION; attempts++) {
        if (out->len >= MIRV_WARHEAD_COUNT || failures >= MIRV_SATURATION) break;
        TileRef t = st->tiles.data[rng_int(rng, 0, n)];
        if (grid_overlaps(m, gr, dst, t)) {
            failures++;
            continue;
        }
        failures = 0;
        IntVec_push(out, t);
        grid_add(m, gr, dst, t);
    }
    free(gr);
}

/* Ten ticks before the separation point the warheads appear there, each
 * waiting to take off around separation time. */
static void mirv_release_warheads(OfeGame *g, NukesState *st, int32_t slot, int32_t remaining) {
    UnitTable *u = &g->units;
    UnitId mirv = unit_id_of_slot(g, slot);
    PlayerId owner = u->owner[slot];
    TileRef dst = u->target_tile[slot];
    TileRef sep = u->path[slot].data[u->path[slot].len - 1];
    uint64_t rng = event_rng(g, mirv, 0x31AF);
    IntVec targets = {0};
    mirv_pick_targets(g, st, u->aux2[slot], dst, &rng, &targets);

    void *ctx[2];
    ctx[0] = (void *)g->map;
    ctx[1] = &dst;
    /* qsort_r is not C99; targets are few hundred, so insertion sort. */
    for (int32_t i = 1; i < targets.len; i++) {
        int32_t v = targets.data[i];
        int32_t j = i - 1;
        while (j >= 0 && cmp_targets(&targets.data[j], &v, ctx) > 0) {
            targets.data[j + 1] = targets.data[j];
            j--;
        }
        targets.data[j + 1] = v;
    }
    int32_t base_wait = OFE_MAX(0, remaining);
    for (int32_t i = 0; i < targets.len; i++) {
        int32_t bonus = i < 70 ? 0 : i < 140 ? 1 : i < 210 ? 2 : i < 280 ? 3 : 4;
        double speed = WARHEAD_SPEED + bonus;
        UnitId w = unit_create(g, OFE_UNIT_MIRV_WARHEAD, owner, sep);
        if (w < 0) break;
        int32_t ws = unit_slot(w);
        u->target_tile[ws] = targets.data[i];
        u->src_tile[ws] = sep;
        u->aux0[ws] = base_wait + rng_int(&rng, 0, 15);
        u->auxf[ws] = speed;
        parabola_path(g->map, &u->path[ws], sep, targets.data[i], speed, true);
    }
    IntVec_free(&targets);
}

static void mirv_tick(OfeGame *g, NukesState *st, UnitId id) {
    int32_t slot = unit_lookup(g, id);
    if (slot < 0) return;
    UnitTable *u = &g->units;
    int32_t n = u->path[slot].len;
    if (n == 0) {
        unit_destroy(g, id, 0);
        return;
    }
    if (u->aux0[slot] > 0) {
        u->aux0[slot]--;
        return;
    }
    int32_t remaining = (n - 2) - u->path_pos[slot];
    if (remaining <= MIRV_SPAWN_LEAD && !u->aux1[slot]) {
        u->aux1[slot] = 1;
        mirv_release_warheads(g, st, slot, remaining);
    }
    if (remaining > 0) {
        u->path_pos[slot]++;
        u->tile[slot] = u->path[slot].data[u->path_pos[slot]];
    } else {
        unit_destroy(g, id, 0); /* separated */
    }
}

/* ---- SAMs ---------------------------------------------------------------------------------------------------------- */

static SamCacheEntry *cache_find(const SamCache *c, uint64_t key) {
    if (!c->cap) return NULL;
    uint32_t mask = (uint32_t)c->cap - 1;
    uint32_t i = (uint32_t)((key * 0x9E3779B97F4A7C15ull) >> 32) & mask;
    while (c->e[i].key != 0) {
        if (c->e[i].key == key) return &c->e[i];
        i = (i + 1) & mask;
    }
    return NULL;
}

static void cache_insert(SamCache *c, const SamCacheEntry *entry) {
    if ((c->len + 1) * 2 > c->cap) {
        SamCacheEntry *old = c->e;
        int32_t oldcap = c->cap;
        c->cap = c->cap ? c->cap * 2 : 256;
        c->e = (SamCacheEntry *)ofe_xcalloc((size_t)c->cap, sizeof(SamCacheEntry));
        c->len = 0;
        for (int32_t i = 0; i < oldcap; i++) {
            if (old[i].key) cache_insert(c, &old[i]);
        }
        free(old);
    }
    uint32_t mask = (uint32_t)c->cap - 1;
    uint32_t i = (uint32_t)((entry->key * 0x9E3779B97F4A7C15ull) >> 32) & mask;
    while (c->e[i].key != 0) i = (i + 1) & mask;
    c->e[i] = *entry;
    c->len++;
}

static void cache_clear(SamCache *c) {
    if (c->len > 0) memset(c->e, 0, (size_t)c->cap * sizeof(SamCacheEntry));
    c->len = 0;
}

typedef struct Intercept {
    int32_t tick; /* ticks from now until the SAM must fire, or -1 / -2 */
    int32_t tile;
} Intercept;

static bool targetable_at(const OfeMap *m, TileRef t, TileRef src, TileRef dst) {
    int64_t r2 = (int64_t)NUKE_TARGETABLE_RANGE * NUKE_TARGETABLE_RANGE;
    return map_dist2(m, t, dst) < r2 || map_dist2(m, t, src) < r2;
}

/* Where along its remaining trajectory can this SAM catch the nuke? Returns
 * the first point it can reach in range and in time (SAMTargetingSystem). */
static Intercept compute_intercept(const OfeGame *g, int32_t sam, int32_t nuke) {
    const UnitTable *u = &g->units;
    const OfeMap *m = g->map;
    const IntVec *path = &u->path[nuke];
    TileRef sam_tile = u->tile[sam];
    int32_t n = path->len, cur = u->path_pos[nuke], wait = u->aux0[nuke];
    int32_t last = n - 2;
    double step = u->auxf[nuke] + 2.0; /* max distance between points */
    Intercept none = {-2, sam_tile};
    if (n < 2) return none;
    TileRef src = path->data[0], dst = path->data[n - 1];
    double max_sq = MAX_SAM_RANGE * MAX_SAM_RANGE;
    bool ever_close = false;

    for (int32_t i = cur; i <= last;) {
        TileRef t = path->data[i];
        double d2 = (double)map_dist2(m, sam_tile, t);
        if (d2 <= max_sq) ever_close = true;
        int32_t nuke_ticks = i - cur + wait;
        int32_t sam_ticks = ceil_div(map_manhattan(m, sam_tile, t), SAM_MISSILE_SPEED);
        double allowed = sam_range_at(g, sam, g->tick + nuke_ticks);
        if (d2 <= allowed * allowed && nuke_ticks >= sam_ticks && targetable_at(m, t, src, dst)) {
            Intercept r = {nuke_ticks - sam_ticks, t};
            return r;
        }
        /* The nuke needs at least (dist - range) / step ticks to get in
         * range, so intermediate points can be skipped. */
        double d = sqrt(d2);
        int32_t skip = d > MAX_SAM_RANGE ? (int32_t)((d - MAX_SAM_RANGE) / step) : 0;
        i += OFE_MAX(1, skip);
    }

    /* Out of reach along the way: maybe the blast point itself is in range,
     * in which case the missile meets the nuke at its last flight tile. */
    int32_t exp_ticks = (n - 1) - cur + wait;
    double range = sam_range_at(g, sam, g->tick + exp_ticks);
    if ((double)map_dist2(m, sam_tile, dst) <= range * range && last >= 0) {
        TileRef flight = path->data[last];
        int32_t nuke_ticks = last - cur + wait;
        int32_t before = nuke_ticks - ceil_div(map_manhattan(m, sam_tile, flight), SAM_MISSILE_SPEED);
        if (before >= 0) {
            Intercept r = {before, flight};
            return r;
        }
    }
    Intercept r = {ever_close ? -1 : -2, sam_tile};
    return r;
}

static bool sam_may_target(const OfeGame *g, PlayerId sam_owner, int32_t nuke) {
    const UnitTable *u = &g->units;
    if (u->target_unit[nuke] >= 0 && unit_lookup(g, u->target_unit[nuke]) >= 0) return false; /* already chased */
    PlayerId o = u->owner[nuke];
    if (o == sam_owner) return false;
    if (player_friendly(g, sam_owner, o)) return game_decided(g) && player_same_team(g, sam_owner, o);
    return true;
}

static int32_t target_score(const OfeGame *g, int32_t sam, int32_t nuke) {
    const UnitTable *u = &g->units;
    int32_t left = OFE_MAX(1, u->path[nuke].len - u->path_pos[nuke]);
    TileRef dst = u->target_tile[nuke] >= 0 ? u->target_tile[nuke] : u->tile[sam];
    int32_t dist = map_manhattan(g->map, u->tile[sam], dst);
    int32_t score = u->type[nuke] == OFE_UNIT_HYDROGEN_BOMB ? 70001 : 0;
    score += OFE_MAX(0, 200000 - dist * 1000);
    score += OFE_MAX(0, 10000 - left * 100);
    return score;
}

static void sam_fire(OfeGame *g, NukesState *st, int32_t sam, UnitId nuke_id, TileRef tile) {
    UnitTable *u = &g->units;
    int32_t nuke = unit_lookup(g, nuke_id);
    if (nuke < 0) return;
    UnitId mid = unit_create(g, OFE_UNIT_SAM_MISSILE, u->owner[sam], u->tile[sam]);
    if (mid < 0) return;
    int32_t ms = unit_slot(mid);
    u->src_tile[ms] = u->tile[sam];
    u->target_tile[ms] = tile;
    u->target_unit[ms] = nuke_id;
    u->aux0[ms] = unit_id_of_slot(g, sam);
    u->timer[ms] = g->tick + ceil_div(map_manhattan(g->map, u->tile[sam], tile), SAM_MISSILE_SPEED);
    u->target_unit[nuke] = mid;
    launcher_fire(g, st, sam, SAM_COOLDOWN);
    IntVec_push(&st->missiles, mid);
}

/* One SAM launcher: find the nukes it must shoot at now, strongest threat
 * first, and fire as many missiles as it holds. */
static void sam_tick(OfeGame *g, NukesState *st, int32_t sam) {
    UnitTable *u = &g->units;
    if (!launcher_ready(g, sam)) return;
    PlayerId owner = u->owner[sam];
    UnitId sam_id = unit_id_of_slot(g, sam);
    int64_t detect2 = (int64_t)SAM_DETECTION_RANGE * SAM_DETECTION_RANGE;
    st->targets.len = 0;

    for (int32_t k = 0; k < st->nukes.len; k++) {
        UnitId nid = st->nukes.data[k];
        int32_t nuke = unit_lookup(g, nid);
        if (nuke < 0 || !sam_may_target(g, owner, nuke)) continue;
        if (map_dist2(g->map, u->tile[sam], u->tile[nuke]) > detect2) continue;

        uint64_t key = ((uint64_t)(uint32_t)(sam_id + 1) << 32) | (uint32_t)(nid + 1);
        int32_t level = u->level[sam];
        SamCacheEntry *hit = cache_find(&st->prev, key);
        SamCacheEntry e;
        bool fire = false;
        if (hit && !(hit->tick == -1 && hit->level != level) && !(hit->tick >= 0 && hit->tick < g->tick)) {
            e = *hit;
            if (e.tick >= 0 && e.tick <= g->tick + 1) fire = true; /* consumed either way */
        } else {
            Intercept r = compute_intercept(g, sam, nuke);
            e.key = key;
            e.level = level;
            e.pad = 0;
            e.tile = r.tile;
            e.tick = r.tick >= 0 ? r.tick + g->tick : r.tick;
            if (r.tick >= 0 && r.tick <= 1) fire = true;
        }
        if (fire) {
            IntVec_push(&st->targets, nid);
            IntVec_push(&st->targets, e.tile);
            IntVec_push(&st->targets, target_score(g, sam, nuke));
        } else {
            cache_insert(&st->cur, &e);
        }
    }

    /* Highest score first; ties keep scan order. */
    IntVec *t = &st->targets;
    for (int32_t i = 3; i < t->len; i += 3) {
        int32_t a = t->data[i], b = t->data[i + 1], c = t->data[i + 2];
        int32_t j = i - 3;
        while (j >= 0 && t->data[j + 2] < c) {
            t->data[j + 3] = t->data[j];
            t->data[j + 4] = t->data[j + 1];
            t->data[j + 5] = t->data[j + 2];
            j -= 3;
        }
        t->data[j + 3] = a;
        t->data[j + 4] = b;
        t->data[j + 5] = c;
    }
    for (int32_t i = 0; i < t->len; i += 3) {
        if (!launcher_ready(g, sam)) break;
        sam_fire(g, st, sam, t->data[i], t->data[i + 1]);
    }
}

/* SAM missile: fly towards the interception tile; on arrival the target
 * dies. It vanishes if its launcher or target is gone. */
static void missile_tick(OfeGame *g, UnitId id) {
    int32_t slot = unit_lookup(g, id);
    if (slot < 0) return;
    UnitTable *u = &g->units;
    int32_t nuke = unit_lookup(g, u->target_unit[slot]);
    int32_t launcher = unit_lookup(g, u->aux0[slot]);
    bool valid = nuke >= 0 && launcher >= 0 && is_flying_nuke(u->type[nuke]) && u->owner[nuke] != u->owner[slot];
    if (!valid) {
        if (nuke >= 0) u->target_unit[nuke] = -1; /* other SAMs may try again */
        unit_destroy(g, id, 0);
        return;
    }
    if (g->tick >= u->timer[slot]) {
        PlayerId owner = u->owner[slot];
        unit_destroy(g, unit_id_of_slot(g, nuke), owner);
        unit_destroy(g, id, 0);
        return;
    }
    /* Interpolate the position along the straight line, for observers. */
    int32_t total = OFE_MAX(1, u->timer[slot] - u->created_tick[slot]);
    int32_t done = g->tick - u->created_tick[slot];
    const OfeMap *m = g->map;
    TileRef a = u->src_tile[slot], b = u->target_tile[slot];
    double f = (double)done / total;
    u->tile[slot] = tile_at(m, map_x(m, a) + (map_x(m, b) - map_x(m, a)) * f, map_y(m, a) + (map_y(m, b) - map_y(m, a)) * f);
}

/* ---- Tick ------------------------------------------------------------------------------------------------------------ */

void nukes_tick(OfeGame *g) {
    NukesState *st = g->sub->nukes;
    UnitTable *u = &g->units;
    reloads_tick(g, st);

    st->nukes.len = st->mirvs.len = st->missiles.len = st->sams.len = 0;
    for (int32_t i = 0; i < u->count; i++) {
        if (!u->alive[i]) continue;
        switch (u->type[i]) {
        case OFE_UNIT_ATOM_BOMB:
        case OFE_UNIT_HYDROGEN_BOMB:
        case OFE_UNIT_MIRV_WARHEAD: IntVec_push(&st->nukes, unit_id_of_slot(g, i)); break;
        case OFE_UNIT_MIRV: IntVec_push(&st->mirvs, unit_id_of_slot(g, i)); break;
        case OFE_UNIT_SAM_MISSILE: IntVec_push(&st->missiles, unit_id_of_slot(g, i)); break;
        default: break;
        }
    }
    if (st->nukes.len == 0 && st->mirvs.len == 0 && st->missiles.len == 0) {
        cache_clear(&st->prev);
        cache_clear(&st->cur);
        return;
    }

    if (st->nukes.len > 0) {
        for (int32_t i = 0; i < u->count; i++) {
            if (u->alive[i] && u->type[i] == OFE_UNIT_SAM_LAUNCHER) IntVec_push(&st->sams, unit_id_of_slot(g, i));
        }
        for (int32_t i = 0; i < st->sams.len; i++) {
            int32_t sam = unit_lookup(g, st->sams.data[i]);
            if (sam >= 0) sam_tick(g, st, sam);
        }
    }
    /* This tick's results become the next tick's lookup table. */
    SamCache tmp = st->prev;
    st->prev = st->cur;
    st->cur = tmp;
    cache_clear(&st->cur);

    for (int32_t i = 0; i < st->missiles.len; i++) missile_tick(g, st->missiles.data[i]);
    for (int32_t i = 0; i < st->nukes.len; i++) nuke_tick(g, st, st->nukes.data[i]);
    for (int32_t i = 0; i < st->mirvs.len; i++) mirv_tick(g, st, st->mirvs.data[i]);
}

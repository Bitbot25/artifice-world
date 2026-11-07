/*
 * units_ts.c - OpenFront's unit executions for the TS-parity build (compiled
 * only with -DOFE_TS_PARITY=1; the default build approximates all of this in
 * structures.c and nukes.c).
 *
 * Each TS execution that builds, upgrades, deletes or flies a unit is a
 * UnitExec here. exec_ts.c keeps them in TS's execution list: it creates one
 * per build_unit / upgrade_structure / delete_unit intent, calls init at the
 * end of the tick that created it and tick on every later tick, and the
 * executions create each other the way TS does (Construction -> Nuke / Mirv /
 * Silo / SAM, Mirv -> warhead Nukes, SAM -> SAM missile). So unit ids (TS's
 * global counter), gold timing, cooldown queues and per-player list order are
 * exactly TS's.
 *
 *   ConstructionExecution  structures are built the tick after the intent and
 *                          finish `duration + 1` ticks later (instant build: at
 *                          once); nukes are only paid for and placed by their own
 *                          execution two ticks after the intent.
 *   UpgradeStructure       at init: up to `amount` levels while gold lasts; a silo
 *                          or SAM gets a missile that starts reloading.
 *   DeleteUnit             at init: marks the unit, destroyed once 30 s have passed.
 *   MissileSilo / SAM      missile reload queues (tick each missile was fired).
 *   NukeExecution          atom, hydrogen bomb and MIRV warhead: parabola flight from
 *                          paths_ts.c, alliance breaking at launch, blast shape drawn
 *                          from PseudoRandom(tick), troop and unit losses, fallout.
 *   MirvExecution          carrier flight, warhead target staging and release.
 *   SAMLauncherExecution   interception planning with TS's cache, scoring and range
 *                          ramp-up after upgrades; SAMMissileExecution flies and hits.
 *
 * How TS state maps onto the shared unit columns (see internal.h):
 *   silo / SAM   path = missile timer queue (tick each missile was fired);
 *                SAM: auxf = startRange, aux1 = targetLevel, aux2 = upgradeStartTick (-1 none)
 *   nuke         path = trajectory, target_tile = destination, aux0 = trajectory index,
 *                aux1 = waitTicks, aux2 = flags (UF_NUKE_*), aux3 = MIRV target player
 *   SAM missile  target_unit = the nuke it chases
 *
 * Not modelled (later waves): PortExecution, FactoryExecution, CityExecution's train
 * station and WarshipExecution, and DefensePostExecution (which does nothing observable:
 * its shooting code is commented out in TS). waterNukes is unsupported.
 */
#include "units_ts.h"

#include "paths_ts.h"

#if OFE_TS_PARITY

#define SAM_COOLDOWN 90
#define SILO_COOLDOWN 90
#define SAM_UPGRADE_TICKS 45 /* samUpgradeDuration: SAMCooldown / 2 */
#define SAM_MISSILE_SPEED 12
#define SAM_DETECTION_FACTOR 4
#define MAX_SAM_RANGE 150
#define NUKE_TARGETABLE_RANGE 150
#define DELETION_MARK_TICKS 300
#define ALLIANCE_BREAK_THRESHOLD 100

#define UF_NUKE_TARGETED_BY_SAM 1 /* aux2 bits of a nuke */
#define UF_NUKE_REACHED 2

enum UxKind { UX_CONSTRUCTION, UX_UPGRADE, UX_DELETE, UX_SILO, UX_SAM, UX_SAM_MISSILE, UX_NUKE, UX_MIRV };

typedef struct NukeFlight {
    TsGrid grid;
    TsParabola pf;
} NukeFlight;

typedef struct AirFlight {
    TsGrid grid;
    TsAir pf;
} AirFlight;

/* SAMTargetingSystem's precomputedNukes entry. tick: >= 0 scheduled launch tick,
 * -1 unreachable at the current level, -2 permanently out of reach. */
typedef struct SamCache {
    int32_t id, tick, tile, last_seen;
    double min_dist2;
} SamCache;
OFE_VEC(SamCacheVec, SamCache)

#define MIRV_RANGE 1500
#define MIRV_MIN_SPREAD 55
#define MIRV_WARHEADS 350
#define MIRV_SATURATION 1500
#define MIRV_GRID_W ((MIRV_RANGE * 2 + MIRV_MIN_SPREAD - 1) / MIRV_MIN_SPREAD + 2)

typedef struct MirvData {
    TsRandom random;
    TsGrid grid;
    IntVec full_path, staged, warheads; /* warheads: eids of the NukeExecutions */
    int32_t path_index;
    PlayerId target_player;
    TileRef separate_dst, spawn_tile;
    int32_t base_x, base_y, min_x, max_x, min_y, max_y;
    int32_t overlap_failures;
    int32_t *cells; /* 2 targets per MIRV_MIN_SPREAD cell, -1 = free; NULL once finalised */
    bool warheads_spawned;
} MirvData;

struct UnitExec {
    uint8_t kind, active;
    bool up; /* rocketDirectionUp */
    PlayerId player;
    int32_t eid;
    int32_t type; /* unit type built / launched */
    TileRef tile; /* build tile, nuke or MIRV destination */
    int32_t amount;
    int32_t uid; /* the unit this execution built (0 = not yet) */
    int32_t unit_arg; /* upgrade / delete: the unit id of the intent */
    int32_t ticks_left; /* construction */
    TileRef src; /* nuke: launch tile, -1 = the silo's */
    int32_t speed, wait; /* nuke */
    int32_t target_unit, owner_unit; /* SAM missile: the nuke and the SAM; SAM: last_level in owner_unit */
    TileRef target_tile; /* SAM missile: where it aims */
    NukeFlight *nk;
    AirFlight *air;
    SamCacheVec cache;
    MirvData *mv;
};

/* ---- Small helpers -------------------------------------------------------------- */

static inline bool valid_tile(const OfeGame *g, TileRef t) { return t >= 0 && t < g->map->num_tiles; }

static bool is_nuke_type(int t) { return t == OFE_UNIT_ATOM_BOMB || t == OFE_UNIT_HYDROGEN_BOMB || t == OFE_UNIT_MIRV_WARHEAD; }

static double sam_range(int32_t level) { return MAX_SAM_RANGE - 480.0 / (level + 5); }

static void nuke_magnitude(int type, int32_t *inner, int32_t *outer) {
    switch (type) {
    case OFE_UNIT_MIRV_WARHEAD: *inner = 12, *outer = 18; break;
    case OFE_UNIT_HYDROGEN_BOMB: *inner = 80, *outer = 100; break;
    default: *inner = 12, *outer = 30; break;
    }
}

/* Slots of p's units whose type is in `mask`, in the order of OpenFront's
 * player.units() array (build order; a captured unit goes to the end). */
static void player_units(const OfeGame *g, PlayerId p, uint32_t mask, IntVec *out) {
    const UnitTable *t = &g->units;
    out->len = 0;
    for (int32_t i = 0; i < t->count; i++) {
        if (!t->alive[i] || t->owner[i] != p || !(mask & (1u << t->type[i]))) continue;
        int32_t j = out->len;
        IntVec_push(out, i);
        while (j > 0 && t->own_seq[out->data[j - 1]] > t->own_seq[i]) {
            out->data[j] = out->data[j - 1];
            j--;
        }
        out->data[j] = i;
    }
}

/* ---- Unit-level state ------------------------------------------------------------ */

static int32_t queue_len(const OfeGame *g, int32_t slot) { return g->units.path[slot].len; }
static bool in_cooldown(const OfeGame *g, int32_t slot) { return queue_len(g, slot) == g->units.level[slot]; }
static void launch_missile(OfeGame *g, int32_t slot) { IntVec_push(&g->units.path[slot], g->tick); }
static void reload_missile(OfeGame *g, int32_t slot) {
    IntVec *q = &g->units.path[slot];
    if (q->len == 0) return;
    memmove(q->data, q->data + 1, (size_t)(q->len - 1) * sizeof(int32_t));
    q->len--;
}

/* UnitImpl.dynamicSamRange (EngineConfig.dynamicSamRange). */
static double dynamic_sam_range(const OfeGame *g, int32_t slot, int32_t tick) {
    const UnitTable *u = &g->units;
    if (u->aux2[slot] < 0) return sam_range(u->level[slot]);
    int32_t elapsed = tick - u->aux2[slot];
    if (elapsed >= SAM_UPGRADE_TICKS) return sam_range(u->aux1[slot]);
    double diff = sam_range(u->aux1[slot]) - u->auxf[slot];
    return u->auxf[slot] + (diff * elapsed) / SAM_UPGRADE_TICKS;
}

/* UnitImpl.increaseLevel. */
static void increase_level(OfeGame *g, int32_t slot) {
    UnitTable *u = &g->units;
    int type = u->type[slot];
    if (type == OFE_UNIT_SAM_LAUNCHER) {
        u->auxf[slot] = dynamic_sam_range(g, slot, g->tick);
        u->aux1[slot] = u->level[slot] + 1;
        u->aux2[slot] = g->tick;
    }
    unit_set_level(g, slot, u->level[slot] + 1);
    if (type == OFE_UNIT_MISSILE_SILO || type == OFE_UNIT_SAM_LAUNCHER) launch_missile(g, slot);
}

static bool unit_alive_id(const OfeGame *g, int32_t uid) { return uid > 0 && unit_lookup(g, uid) >= 0; }

/* ---- Building ------------------------------------------------------------------------ */

/* PlayerImpl.canBuildUnitType: not disabled, affordable, builder alive (warheads need no owner). */
static bool can_build_unit_type(const OfeGame *g, PlayerId p, int type) {
    if (g->cfg.disabled_units & (1u << type)) return false;
    if (g->players.gold[p] < unit_cost(g, p, type)) return false;
    return type == OFE_UNIT_MIRV_WARHEAD || player_alive(g, p);
}

/* PlayerImpl.buildUnit: the cost is read before the unit exists, paid after. */
static int32_t build_unit(OfeGame *g, PlayerId p, int type, TileRef tile) {
    int64_t cost = unit_cost(g, p, type);
    UnitId id = unit_create(g, type, p, tile);
    if (id < 0) return -1;
    int32_t slot = unit_lookup(g, id);
    g->players.units_built[p][type]++;
    player_remove_gold(g, p, cost);
    UnitTable *u = &g->units;
    if (type == OFE_UNIT_SAM_LAUNCHER) {
        u->auxf[slot] = sam_range(1);
        u->aux1[slot] = 1;
        u->aux2[slot] = -1;
    }
    return slot;
}

/* PlayerImpl.nukeSpawn: the tile of the nearest ready silo, or -1. */
static TileRef nuke_spawn(const OfeGame *g, PlayerId p, int type, TileRef tile) {
    if (g->start_tick < 0 || g->tick - g->start_tick < g->cfg.spawn_immunity_ticks) return -1;
    if (map_is_impassable(g->map, tile)) return -1;
    PlayerId owner = tile_owner(g, tile);
    if (owner != 0 && owner != p && !g->game_over && player_same_team(g, p, owner)) return -1;
    int32_t inner, outer;
    nuke_magnitude(type, &inner, &outer);
    if (g->cfg.game_mode == OFE_MODE_TEAM && type != OFE_UNIT_MIRV && !g->game_over) {
        /* Never launch at something that would blow up a teammate's structure. */
        const UnitTable *u = &g->units;
        int64_t r2 = (int64_t)outer * outer;
        for (int32_t i = 0; i < u->count; i++) {
            if (!u->alive[i] || !unit_is_structure(u->type[i]) || (u->flags[i] & OFE_UF_UNDER_CONSTRUCTION)) continue;
            if (u->owner[i] != p && player_same_team(g, p, u->owner[i]) && map_dist2(g->map, tile, u->tile[i]) <= r2) return -1;
        }
    }
    IntVec silos = {0};
    player_units(g, p, 1u << OFE_UNIT_MISSILE_SILO, &silos);
    TileRef best = -1;
    int32_t best_d = 0;
    for (int32_t i = 0; i < silos.len; i++) {
        int32_t s = silos.data[i];
        if (in_cooldown(g, s) || (g->units.flags[s] & OFE_UF_UNDER_CONSTRUCTION)) continue;
        int32_t d = map_manhattan(g->map, g->units.tile[s], tile);
        if (best < 0 || d < best_d) {
            best = g->units.tile[s];
            best_d = d;
        }
    }
    IntVec_free(&silos);
    return best;
}

/* The first silo of p standing on `tile` (OpenFront: units(MissileSilo).find(tile === spawn)). */
static int32_t silo_at(const OfeGame *g, PlayerId p, TileRef tile) {
    IntVec silos = {0};
    player_units(g, p, 1u << OFE_UNIT_MISSILE_SILO, &silos);
    int32_t found = -1;
    for (int32_t i = 0; i < silos.len && found < 0; i++) {
        if (g->units.tile[silos.data[i]] == tile) found = silos.data[i];
    }
    IntVec_free(&silos);
    return found;
}

/* ---- Execution objects ------------------------------------------------------------------- */

static UnitExec *uexec_new(int kind, PlayerId player) {
    UnitExec *x = (UnitExec *)ofe_xcalloc(1, sizeof(*x));
    x->kind = (uint8_t)kind;
    x->active = 1;
    x->player = player;
    x->src = -1;
    x->speed = -1;
    return x;
}

UnitExec *uexec_from_intent(const OfeIntent *in) {
    UnitExec *x = NULL;
    switch (in->type) {
    case OFE_INTENT_BUILD_UNIT:
        x = uexec_new(UX_CONSTRUCTION, in->player);
        x->type = in->unit_type;
        x->tile = in->tile;
        x->amount = OFE_MAX(1, in->amount);
        x->up = (in->flags & 1) != 0;
        break;
    case OFE_INTENT_UPGRADE_STRUCTURE:
        x = uexec_new(UX_UPGRADE, in->player);
        x->unit_arg = in->target;
        x->amount = OFE_MAX(1, in->amount);
        break;
    case OFE_INTENT_DELETE_UNIT:
        x = uexec_new(UX_DELETE, in->player);
        x->unit_arg = in->target;
        break;
    default: break;
    }
    return x;
}

static UnitExec *new_nuke(PlayerId p, int type, TileRef dst, TileRef src, int32_t speed, int32_t wait, bool up) {
    UnitExec *x = uexec_new(UX_NUKE, p);
    x->type = type;
    x->tile = dst;
    x->src = src;
    x->speed = speed;
    x->wait = wait;
    x->up = up;
    return x;
}

static MirvData *mirv_clone(const MirvData *m) {
    MirvData *c = (MirvData *)ofe_xcalloc(1, sizeof(*c));
    *c = *m;
    memset(&c->full_path, 0, sizeof(IntVec) * 3);
    IntVec_copy(&c->full_path, &m->full_path);
    IntVec_copy(&c->staged, &m->staged);
    IntVec_copy(&c->warheads, &m->warheads);
    c->cells = NULL;
    if (m->cells) {
        c->cells = (int32_t *)ofe_xmalloc((size_t)MIRV_GRID_W * MIRV_GRID_W * 2 * sizeof(int32_t));
        memcpy(c->cells, m->cells, (size_t)MIRV_GRID_W * MIRV_GRID_W * 2 * sizeof(int32_t));
    }
    return c;
}

UnitExec *uexec_clone(const UnitExec *x) {
    UnitExec *c = (UnitExec *)ofe_xmalloc(sizeof(*c));
    *c = *x;
    c->nk = NULL;
    c->air = NULL;
    memset(&c->cache, 0, sizeof(c->cache));
    c->mv = NULL;
    if (x->nk) {
        c->nk = (NukeFlight *)ofe_xmalloc(sizeof(NukeFlight));
        *c->nk = *x->nk;
        c->nk->pf.map = &c->nk->grid;
        memset(&c->nk->pf.px, 0, sizeof(IntVec) * 2);
        IntVec_copy(&c->nk->pf.px, &x->nk->pf.px);
        IntVec_copy(&c->nk->pf.py, &x->nk->pf.py);
    }
    if (x->air) {
        c->air = (AirFlight *)ofe_xmalloc(sizeof(AirFlight));
        *c->air = *x->air;
        c->air->pf.map = &c->air->grid;
        memset(&c->air->pf.path, 0, sizeof(IntVec));
        IntVec_copy(&c->air->pf.path, &x->air->pf.path);
    }
    SamCacheVec_copy(&c->cache, &x->cache);
    if (x->mv) c->mv = mirv_clone(x->mv);
    return c;
}

void uexec_free(UnitExec *x) {
    if (!x) return;
    if (x->nk) {
        ts_parabola_free(&x->nk->pf);
        free(x->nk);
    }
    if (x->air) {
        ts_air_free(&x->air->pf);
        free(x->air);
    }
    SamCacheVec_free(&x->cache);
    if (x->mv) {
        IntVec_free(&x->mv->full_path);
        IntVec_free(&x->mv->staged);
        IntVec_free(&x->mv->warheads);
        free(x->mv->cells);
        free(x->mv);
    }
    free(x);
}

int32_t uexec_eid(const UnitExec *x) { return x->eid; }
void uexec_set_eid(UnitExec *x, int32_t eid) { x->eid = eid; }
bool uexec_active(const UnitExec *x) { return x->active; }

/* ---- Construction ---------------------------------------------------------------------------- */

/* ConstructionExecution.completeConstruction: the structure is finished and its own
 * execution starts (nukes and MIRVs start theirs without a structure). */
static void complete_construction(OfeGame *g, UnitExec *x) {
    int32_t slot = x->uid ? unit_lookup(g, x->uid) : -1;
    if (slot >= 0) unit_finish_construction(g, slot);
    switch (x->type) {
    case OFE_UNIT_ATOM_BOMB:
    case OFE_UNIT_HYDROGEN_BOMB:
        for (int32_t i = 0; i < x->amount; i++) ts_exec_add_unit(g, new_nuke(x->player, x->type, x->tile, -1, -1, 0, x->up));
        break;
    case OFE_UNIT_MIRV: {
        UnitExec *m = uexec_new(UX_MIRV, x->player);
        m->tile = x->tile;
        ts_exec_add_unit(g, m);
        break;
    }
    case OFE_UNIT_MISSILE_SILO: {
        UnitExec *s = uexec_new(UX_SILO, x->player);
        s->uid = x->uid;
        ts_exec_add_unit(g, s);
        break;
    }
    case OFE_UNIT_SAM_LAUNCHER: {
        UnitExec *s = uexec_new(UX_SAM, x->player);
        s->uid = x->uid;
        s->owner_unit = 1; /* lastLevel */
        ts_exec_add_unit(g, s);
        break;
    }
    default: break; /* defense post, city, port, factory: nothing observable (see header) */
    }
}

static bool construction_tick(OfeGame *g, UnitExec *x) {
    if (x->uid == 0) {
        if (!unit_is_structure(x->type)) {
            complete_construction(g, x); /* gold is taken by the nuke / MIRV execution */
            return false;
        }
        if (!can_build_unit_type(g, x->player, x->type)) return false;
        TileRef at = structures_can_build(g, x->player, x->type, x->tile);
        if (at < 0) return false;
        int32_t slot = build_unit(g, x->player, x->type, at);
        if (slot < 0) return false;
        x->uid = g->units.uid[slot];
        int32_t duration = structure_construction_ticks(g, x->type);
        if (duration > 0) {
            unit_begin_construction(g, slot, 0);
            x->ticks_left = duration;
            return true;
        }
        complete_construction(g, x);
        return false;
    }
    int32_t slot = unit_lookup(g, x->uid);
    if (slot < 0) return false;
    x->player = g->units.owner[slot];
    if (x->ticks_left == 0) {
        complete_construction(g, x);
        return false;
    }
    x->ticks_left--;
    return true;
}

/* ---- Upgrade and delete ------------------------------------------------------------------------- */

static bool can_upgrade_unit(const OfeGame *g, PlayerId p, int32_t slot) {
    const UnitTable *u = &g->units;
    int type = u->type[slot];
    if (!structure_upgradable(type) || !can_build_unit_type(g, p, type)) return false;
    return !(u->flags[slot] & (OFE_UF_UNDER_CONSTRUCTION | OFE_UF_MARKED_FOR_DELETION)) && u->owner[slot] == p;
}

static void upgrade_init(OfeGame *g, UnitExec *x) {
    int32_t slot = unit_lookup(g, x->unit_arg);
    if (slot < 0 || g->units.owner[slot] != x->player) return;
    for (int32_t i = 0; i < x->amount; i++) {
        if (!can_upgrade_unit(g, x->player, slot)) break;
        int type = g->units.type[slot];
        player_remove_gold(g, x->player, unit_cost(g, x->player, type));
        increase_level(g, slot);
        g->players.units_built[x->player][type]++;
    }
}

static bool delete_init(OfeGame *g, UnitExec *x) {
    int32_t slot = unit_lookup(g, x->unit_arg);
    if (slot < 0 || g->units.owner[slot] != x->player) return false;
    TileRef tile = g->units.tile[slot];
    if (tile_owner(g, tile) != x->player || !map_is_land(g->map, tile)) return false;
    if (g->start_tick < 0 || !structures_delete_ready(g, x->player)) return false;
    structures_delete_record(g, x->player);
    g->units.flags[slot] |= OFE_UF_MARKED_FOR_DELETION;
    g->units.deletion_at[slot] = g->tick + DELETION_MARK_TICKS;
    x->uid = g->units.uid[slot];
    return true;
}

static bool delete_tick(OfeGame *g, UnitExec *x) {
    int32_t slot = unit_lookup(g, x->uid);
    if (slot < 0) return false;
    if (g->units.deletion_at[slot] >= 0 && g->tick - g->units.deletion_at[slot] > 0) {
        unit_destroy(g, x->uid, 0);
        return false;
    }
    return true;
}

/* ---- Silo ------------------------------------------------------------------------------------------- */

static bool silo_tick(OfeGame *g, UnitExec *x) {
    int32_t slot = unit_lookup(g, x->uid);
    if (slot < 0) return false;
    if (g->units.flags[slot] & OFE_UF_UNDER_CONSTRUCTION) return true;
    const IntVec *q = &g->units.path[slot];
    if (q->len == 0) return true;
    if (SILO_COOLDOWN - (g->tick - q->data[0]) <= 0) reload_missile(g, slot);
    return true;
}

/* ---- Nukes: breaking alliances, blast, detonation -------------------------------------------------- */

typedef struct BlastWeight {
    PlayerId p;
    double w;
} BlastWeight;
OFE_VEC(BlastWeightVec, BlastWeight)

/* NukeAlliance.computeNukeBlastCounts: tiles of each owner in the blast circle
 * (inner counts 1, outer 0.5), in order of first appearance. */
static void blast_counts(const OfeGame *g, TileRef target, int32_t inner, int32_t outer, BlastWeightVec *out) {
    const OfeMap *m = g->map;
    int32_t cx = map_x(m, target), cy = map_y(m, target);
    int64_t inner2 = (int64_t)inner * inner, outer2 = (int64_t)outer * outer;
    for (int32_t x = OFE_MAX(0, cx - outer); x <= OFE_MIN(m->width - 1, cx + outer); x++) {
        for (int32_t y = OFE_MAX(0, cy - outer); y <= OFE_MIN(m->height - 1, cy + outer); y++) {
            int64_t d2 = (int64_t)(x - cx) * (x - cx) + (int64_t)(y - cy) * (y - cy);
            if (d2 > outer2) continue;
            PlayerId o = tile_owner(g, map_ref(m, x, y));
            if (o == 0) continue;
            double w = d2 <= inner2 ? 1.0 : 0.5;
            int32_t i = 0;
            while (i < out->len && out->data[i].p != o) i++;
            if (i == out->len) {
                BlastWeight b = {o, 0};
                BlastWeightVec_push(out, b);
            }
            out->data[i].w += w;
        }
    }
}

/* Any finished structure of `owner` (0 = anyone) within `range` of tile; with `owners`,
 * every such structure's owner is appended once instead. */
static bool structures_in_range(const OfeGame *g, TileRef tile, int32_t range, PlayerId owner, IntVec *owners) {
    const UnitTable *u = &g->units;
    int64_t r2 = (int64_t)range * range;
    bool any = false;
    for (int32_t i = 0; i < u->count; i++) {
        if (!u->alive[i] || !unit_is_structure(u->type[i]) || (u->flags[i] & OFE_UF_UNDER_CONSTRUCTION)) continue;
        if (owner != 0 && u->owner[i] != owner) continue;
        if (map_dist2(g->map, tile, u->tile[i]) > r2) continue;
        any = true;
        if (!owners) return true;
        bool seen = false;
        for (int32_t k = 0; k < owners->len; k++) seen |= owners->data[k] == u->owner[i];
        if (!seen) IntVec_push(owners, u->owner[i]);
    }
    return any;
}

/* NukeExecution.maybeBreakAlliances: everyone with a lot of land or any structure in the
 * blast loses its alliance with the launcher (and its pending requests), and likes it less. */
static void nuke_break_alliances(OfeGame *g, PlayerId p, int type, TileRef dst) {
    int32_t inner, outer;
    nuke_magnitude(type, &inner, &outer);
    IntVec set = {0};
    BlastWeightVec counts = {0};
    blast_counts(g, dst, inner, outer, &counts);
    for (int32_t i = 0; i < counts.len; i++) {
        if (counts.data[i].w > ALLIANCE_BREAK_THRESHOLD) IntVec_push(&set, counts.data[i].p);
    }
    IntVec owners = {0};
    structures_in_range(g, dst, outer, 0, &owners);
    for (int32_t i = 0; i < owners.len; i++) {
        bool seen = false;
        for (int32_t k = 0; k < set.len; k++) seen |= set.data[k] == owners.data[i];
        if (!seen) IntVec_push(&set, owners.data[i]);
    }

    /* The launcher's incoming requests from affected players are rejected. */
    for (int32_t i = g->alliance_requests.len - 1; i >= 0; i--) {
        AllianceRequest r = g->alliance_requests.data[i];
        if (r.recipient != p) continue;
        for (int32_t k = 0; k < set.len; k++) {
            if (set.data[k] == r.requestor) {
                alliance_reject(g, r.requestor, p);
                break;
            }
        }
    }
    for (int32_t k = 0; k < set.len; k++) {
        PlayerId a = set.data[k];
        if (!player_valid(g, a)) continue;
        bool pending = false;
        for (int32_t i = 0; i < g->alliance_requests.len; i++) {
            pending |= g->alliance_requests.data[i].requestor == p && g->alliance_requests.data[i].recipient == a;
        }
        if (pending) {
            alliance_reject(g, p, a);
            continue;
        }
        if (alliance_find(g, p, a) >= 0) alliance_break_raw(g, p, a);
        if (a != p) player_update_relation(g, a, p, -100);
    }
    IntVec_free(&set);
    IntVec_free(&owners);
    BlastWeightVec_free(&counts);
}

/* EngineConfig.nukeDeathFactor. */
static double nuke_death_factor(int type, double humans, double tiles_owned, double max_troops) {
    if (type != OFE_UNIT_MIRV_WARHEAD) return (5 * humans) / OFE_MAX(1.0, tiles_owned);
    double target = 0.03 * max_troops;
    double excess = OFE_MAX(0.0, humans - target);
    double normalized = excess / max_troops;
    return 500 * (1 - ofe_exp(-2 * normalized));
}

/* NukeExecution.tilesToDestroy: breadth-first from the target through every tile within
 * the outer radius that is not impassable; beyond the inner radius each candidate is
 * accepted with probability 1/2, redrawn every time a neighbour reaches it. Water
 * counts, it is only the owned land that matters. */
static void tiles_to_destroy(OfeGame *g, TileRef dst, int type, IntVec *out) {
    int32_t inner, outer;
    nuke_magnitude(type, &inner, &outer);
    int64_t inner2 = (int64_t)inner * inner, outer2 = (int64_t)outer * outer;
    TsRandom rand;
    ts_rand_init(&rand, g->tick);
    const OfeMap *m = g->map;
    out->len = 0;
    uint32_t gen = tiles_new_visit_gen(g);
    g->visit_stamp[dst] = gen;
    IntVec_push(out, dst);
    for (int32_t head = 0; head < out->len; head++) {
        TileRef nb[4];
        int k = map_neighbors4(m, out->data[head], nb);
        for (int i = 0; i < k; i++) {
            TileRef n = nb[i];
            if (g->visit_stamp[n] == gen) continue;
            int64_t d2 = map_dist2(m, dst, n);
            if (d2 <= outer2 && (d2 <= inner2 || ts_rand_chance(&rand, 2)) && !map_is_impassable(m, n)) {
                g->visit_stamp[n] = gen;
                IntVec_push(out, n);
            }
        }
    }
}

/* NukeExecution.detonate. */
static void detonate(OfeGame *g, UnitExec *x, int32_t slot) {
    UnitTable *u = &g->units;
    int type = x->type;
    int32_t inner, outer;
    nuke_magnitude(type, &inner, &outer);
    IntVec tiles = {0};
    tiles_to_destroy(g, x->tile, type, &tiles);

    /* Owned tiles are relinquished, land tiles get fallout. */
    BlastWeightVec hit = {0}; /* impacted players and their tile counts, in order */
    for (int32_t i = 0; i < tiles.len; i++) {
        TileRef t = tiles.data[i];
        PlayerId o = tile_owner(g, t);
        if (o != 0) {
            tile_set_owner(g, t, 0);
            int32_t k = 0;
            while (k < hit.len && hit.data[k].p != o) k++;
            if (k == hit.len) {
                BlastWeight b = {o, 0};
                BlastWeightVec_push(&hit, b);
            }
            hit.data[k].w += 1;
        }
        if (map_is_land(g->map, t)) tile_set_fallout(g, t, true);
    }

    for (int32_t h = 0; h < hit.len; h++) {
        PlayerId p = hit.data[h].p;
        int32_t impacted = (int32_t)hit.data[h].w;
        double tiles_before = g->players.num_tiles[p] + impacted;
        double max_troops = player_max_troops(g, p);
        IntVec ships = {0};
        for (int32_t i = 0; i < u->count; i++) {
            if (u->alive[i] && u->owner[i] == p && u->type[i] == OFE_UNIT_TRANSPORT_SHIP) IntVec_push(&ships, i);
        }
        for (int32_t i = 0; i < impacted; i++) {
            double left = tiles_before - i;
            player_remove_troops(g, p, nuke_death_factor(type, (double)g->players.troops[p], left, max_troops));
            for (int32_t a = 0; a < g->attacks.count; a++) {
                if (!g->attacks.active[a] || g->attacks.attacker[a] != p) continue;
                double tr = g->attacks.troops[a];
                tr -= nuke_death_factor(type, tr, left, max_troops);
                g->attacks.troops[a] = OFE_MAX(0.0, tr);
            }
            for (int32_t s = 0; s < ships.len; s++) {
                double tr = u->troops[ships.data[s]];
                tr -= nuke_death_factor(type, tr, left, max_troops);
                u->troops[ships.data[s]] = OFE_MAX(0.0, tr);
            }
        }
        IntVec_free(&ships);
    }

    /* Every unit but the flying ones inside the outer radius is destroyed. */
    int64_t outer2 = (int64_t)outer * outer;
    for (int32_t i = 0; i < u->count; i++) {
        if (!u->alive[i] || is_nuke_type(u->type[i]) || u->type[i] == OFE_UNIT_MIRV || u->type[i] == OFE_UNIT_SAM_MISSILE) continue;
        if (map_dist2(g->map, x->tile, u->tile[i]) < outer2) unit_destroy(g, unit_id_of_slot(g, i), 0);
    }
    x->active = 0;
    u->aux2[slot] |= UF_NUKE_REACHED;
    unit_destroy(g, unit_id_of_slot(g, slot), 0);
    IntVec_free(&tiles);
    BlastWeightVec_free(&hit);
}

static TsParabolaOpts flight_opts(int32_t speed, bool up) {
    TsParabolaOpts o = ts_parabola_default_opts();
    o.increment = speed;
    o.direction_up = up;
    return o;
}

static void nuke_init(OfeGame *g, UnitExec *x) {
    if (x->speed == -1) x->speed = x->type == OFE_UNIT_MIRV_WARHEAD ? 22 : 10;
    x->nk = (NukeFlight *)ofe_xcalloc(1, sizeof(NukeFlight));
    x->nk->grid = ts_grid_from_map(g->map);
    TsParabolaOpts o = flight_opts(x->speed, x->up);
    ts_parabola_init(&x->nk->pf, &x->nk->grid, &o);
}

/* NukeExecution.tick. */
static bool nuke_tick(OfeGame *g, UnitExec *x) {
    UnitTable *u = &g->units;
    PlayerId p = x->player;
    if (x->uid == 0) {
        TileRef spawn;
        if (!can_build_unit_type(g, p, x->type)) return false;
        spawn = x->type == OFE_UNIT_MIRV_WARHEAD ? x->tile : nuke_spawn(g, p, x->type, x->tile);
        if (spawn < 0) return false;
        if (x->src < 0) x->src = spawn;
        int32_t silo = silo_at(g, p, spawn);
        if (silo >= 0) {
            /* Launches from one silo trail each other: wait behind the last queued missile. */
            int32_t last = 0;
            for (int32_t i = 0; i < u->path[silo].len; i++) last = OFE_MAX(u->path[silo].data[i] + 1, last + 1);
            if (last > g->tick) x->wait += last - g->tick;
        }
        IntVec traj = {0};
        TsParabolaOpts o = flight_opts(x->speed, x->up);
        ts_parabola_find_path(&x->nk->grid, &o, x->src, x->tile, &traj);
        int32_t slot = build_unit(g, p, x->type, x->src);
        if (slot < 0) {
            IntVec_free(&traj);
            return false;
        }
        x->uid = u->uid[slot];
        u->target_tile[slot] = x->tile;
        IntVec_copy(&u->path[slot], &traj);
        IntVec_free(&traj);
        u->aux0[slot] = 0;
        u->aux1[slot] = x->wait;
        if (x->type != OFE_UNIT_MIRV_WARHEAD) nuke_break_alliances(g, p, x->type, x->tile);
        if (silo >= 0) launch_missile(g, silo);
        return true;
    }
    int32_t slot = unit_lookup(g, x->uid);
    if (slot < 0) return false; /* intercepted or cancelled */
    if (x->wait > 0) {
        u->aux1[slot] = --x->wait;
        return true;
    }
    TsPathResult r = ts_parabola_next(&x->nk->pf, x->src, x->tile, x->speed);
    if (r.status == TS_PATH_COMPLETE) {
        unit_move(g, slot, r.node);
        /* A SAM missile about to hit it counts as an interception, not a detonation. */
        bool doomed = false;
        int64_t r2 = (int64_t)SAM_MISSILE_SPEED * SAM_MISSILE_SPEED;
        for (int32_t i = 0; i < u->count && !doomed; i++) {
            if (!u->alive[i] || u->type[i] != OFE_UNIT_SAM_MISSILE || u->owner[i] == u->owner[slot]) continue;
            doomed = map_dist2(g->map, x->tile, u->tile[i]) <= r2 && u->target_unit[i] == x->uid;
        }
        if (!doomed) detonate(g, x, slot);
    } else if (r.status == TS_PATH_NEXT) {
        unit_move(g, slot, r.node);
        int32_t idx = ts_parabola_current_index(&x->nk->pf), max = u->path[slot].len - 1;
        u->aux0[slot] = idx < 0 ? 0 : idx > max ? max : idx;
    }
    return x->active;
}

void uexec_cancel(OfeGame *g, UnitExec *x) {
    x->active = 0;
    if (x->uid > 0 && unit_alive_id(g, x->uid)) unit_destroy(g, x->uid, 0);
}

/* ---- SAM launchers ------------------------------------------------------------------------------------ */

typedef struct SamTarget {
    int32_t slot, tile;
    double score;
} SamTarget;

static int32_t tick_to_reach(const OfeGame *g, TileRef from, TileRef to) {
    return (int32_t)ceil((double)map_manhattan(g->map, from, to) / SAM_MISSILE_SPEED);
}

static bool targetable_at(const OfeGame *g, TileRef tile, TileRef src, TileRef dst) {
    int64_t r2 = (int64_t)NUKE_TARGETABLE_RANGE * NUKE_TARGETABLE_RANGE;
    return map_dist2(g->map, tile, dst) < r2 || map_dist2(g->map, tile, src) < r2;
}

/* SAMTargetingSystem.checkDetonationInterception: can the SAM hit the nuke's last flight tile? */
static bool detonation_interception(const OfeGame *g, int32_t sam, int32_t nuke, int32_t ticks, int32_t *out_tick, TileRef *out_tile) {
    const UnitTable *u = &g->units;
    const IntVec *traj = &u->path[nuke];
    int32_t max_idx = traj->len - 2;
    if (traj->len == 0) return false;
    TileRef final_tile = traj->data[traj->len - 1];
    int32_t cur = u->aux0[nuke], wait = u->aux1[nuke];
    int32_t exp_ticks = traj->len - 1 - cur + wait;
    double range = dynamic_sam_range(g, sam, ticks + exp_ticks);
    if ((double)map_dist2(g->map, u->tile[sam], final_tile) > range * range) return false;
    if (max_idx < 0) return false;
    TileRef flight = traj->data[max_idx];
    int32_t nuke_ticks = max_idx - cur + wait;
    int32_t sam_ticks = tick_to_reach(g, u->tile[sam], flight);
    int32_t before = nuke_ticks - sam_ticks;
    if (before < 0) return false;
    *out_tick = before;
    *out_tile = flight;
    return true;
}

/* SAMTargetingSystem.computeInterceptionTile. */
static SamCache compute_interception(const OfeGame *g, int32_t sam, int32_t nuke, int32_t ticks) {
    const UnitTable *u = &g->units;
    const IntVec *traj = &u->path[nuke];
    TileRef sam_tile = u->tile[sam];
    int32_t cur = u->aux0[nuke], wait = u->aux1[nuke];
    int32_t max_idx = traj->len - 2;
    double max_range2 = (double)MAX_SAM_RANGE * MAX_SAM_RANGE;
    double min_d2 = INFINITY;
    TileRef closest = sam_tile;
    int32_t inc_steps = 0;
    double last_d2 = -1;
    TileRef src = traj->len > 0 ? traj->data[0] : -1, dst = traj->len > 0 ? traj->data[traj->len - 1] : -1;
    SamCache res = {u->uid[nuke], -1, sam_tile, ticks, 0};
    for (int32_t i = cur; i <= max_idx; i++) {
        TileRef tile = traj->data[i];
        double d2 = (double)map_dist2(g->map, sam_tile, tile);
        if (d2 < min_d2) {
            min_d2 = d2;
            closest = tile;
        }
        inc_steps = last_d2 != -1 && d2 > last_d2 ? inc_steps + 1 : 0;
        last_d2 = d2;
        int32_t nuke_ticks = i - cur + wait;
        int32_t sam_ticks = tick_to_reach(g, sam_tile, tile);
        double allowed = dynamic_sam_range(g, sam, ticks + nuke_ticks);
        if (d2 <= allowed * allowed && nuke_ticks >= sam_ticks && src >= 0 && dst >= 0 && targetable_at(g, tile, src, dst)) {
            res.tick = nuke_ticks - sam_ticks;
            res.tile = tile;
            res.min_dist2 = min_d2;
            return res;
        }
        if (inc_steps > 3 && d2 > max_range2) break;
    }
    int32_t det_tick;
    TileRef det_tile;
    if (detonation_interception(g, sam, nuke, ticks, &det_tick, &det_tile)) {
        res.tick = det_tick;
        res.tile = det_tile;
        res.min_dist2 = min_d2;
        return res;
    }
    res.tick = min_d2 > max_range2 ? -2 : -1;
    res.tile = closest;
    res.min_dist2 = min_d2;
    return res;
}

static SamCache *cache_find(SamCacheVec *c, int32_t id) {
    for (int32_t i = 0; i < c->len; i++) {
        if (c->data[i].id == id) return &c->data[i];
    }
    return NULL;
}

static void cache_erase(SamCacheVec *c, SamCache *e) {
    int32_t i = (int32_t)(e - c->data);
    memmove(c->data + i, c->data + i + 1, (size_t)(c->len - i - 1) * sizeof(SamCache));
    c->len--;
}

/* SAMTargetingSystem.isValidNukeTarget (the nuke is not already targeted, not ours, not a friend's). */
static bool valid_nuke_target(const OfeGame *g, int32_t sam, int32_t nuke) {
    const UnitTable *u = &g->units;
    if ((u->aux2[nuke] & UF_NUKE_TARGETED_BY_SAM) || u->owner[nuke] == u->owner[sam]) return false;
    if (player_friendly(g, u->owner[sam], u->owner[nuke])) {
        /* Aftergame fun between teammates once the game is over (not in singleplayer). */
        return g->game_over && player_same_team(g, u->owner[sam], u->owner[nuke]);
    }
    return true;
}

/* UnitGrid.nearbyUnits order for [atom, hydrogen, warhead] queries: grid cells row by row, within a cell by
 * type in query order, then by the order units entered the cell. */
static bool nuke_before(const OfeGame *g, int32_t a, int32_t b) {
    const UnitTable *u = &g->units;
    const OfeMap *m = g->map;
    int32_t cols = (m->width + 99) / 100;
    int32_t ca = (map_y(m, u->tile[a]) / 100) * cols + map_x(m, u->tile[a]) / 100;
    int32_t cb = (map_y(m, u->tile[b]) / 100) * cols + map_x(m, u->tile[b]) / 100;
    if (ca != cb) return ca < cb;
    static const int order[] = {OFE_UNIT_ATOM_BOMB, OFE_UNIT_HYDROGEN_BOMB, OFE_UNIT_MIRV_WARHEAD};
    int32_t ta = 0, tb = 0;
    for (int32_t i = 0; i < 3; i++) {
        if (order[i] == u->type[a]) ta = i;
        if (order[i] == u->type[b]) tb = i;
    }
    if (ta != tb) return ta < tb;
    return u->grid_seq[a] < u->grid_seq[b];
}

static double target_score(const OfeGame *g, int32_t sam, int32_t nuke) {
    const UnitTable *u = &g->units;
    const IntVec *traj = &u->path[nuke];
    TileRef sam_tile = u->tile[sam];
    int32_t time_to_explode = OFE_MAX(1, traj->len - u->aux0[nuke]);
    TileRef target = u->target_tile[nuke] >= 0 ? u->target_tile[nuke] : (traj->len > 0 ? traj->data[traj->len - 1] : sam_tile);
    int32_t dist = map_manhattan(g->map, sam_tile, target);
    double type_bonus = u->type[nuke] == OFE_UNIT_HYDROGEN_BOMB ? 70001 : 0;
    double distance_bonus = OFE_MAX(0.0, 200000.0 - dist * 1000.0);
    double urgency = OFE_MAX(0.0, 10000.0 - time_to_explode * 100.0);
    return type_bonus + distance_bonus + urgency;
}

/* SAMLauncherExecution.tick (the targeting system included). */
static bool sam_tick(OfeGame *g, UnitExec *x) {
    UnitTable *u = &g->units;
    int32_t slot = unit_lookup(g, x->uid);
    if (slot < 0) return false;
    /* lastLevel: a level-up forgets the "unreachable at this level" verdicts. */
    if (x->owner_unit != u->level[slot]) {
        x->owner_unit = u->level[slot];
        for (int32_t i = x->cache.len - 1; i >= 0; i--) {
            if (x->cache.data[i].tick == -1) cache_erase(&x->cache, &x->cache.data[i]);
        }
    }
    if (u->flags[slot] & OFE_UF_UNDER_CONSTRUCTION) return true;
    x->player = u->owner[slot];
    while (u->path[slot].len > 0) {
        if (SAM_COOLDOWN - (g->tick - u->path[slot].data[0]) > 0) break;
        reload_missile(g, slot);
    }
    if (in_cooldown(g, slot)) return true;

    int32_t nukes = 0;
    for (PlayerId p = 1; p < g->players.count; p++) {
        nukes += g->players.unit_count[p][OFE_UNIT_ATOM_BOMB] + g->players.unit_count[p][OFE_UNIT_HYDROGEN_BOMB] +
                 g->players.unit_count[p][OFE_UNIT_MIRV_WARHEAD];
    }
    if (nukes == 0) return true;

    /* getValidTargets */
    int32_t ticks = g->tick;
    int64_t detect = (int64_t)MAX_SAM_RANGE * SAM_DETECTION_FACTOR;
    IntVec near = {0};
    for (int32_t i = 0; i < u->count; i++) {
        if (!u->alive[i] || !is_nuke_type(u->type[i])) continue;
        if (map_dist2(g->map, u->tile[slot], u->tile[i]) > detect * detect || !valid_nuke_target(g, slot, i)) continue;
        int32_t j = near.len;
        IntVec_push(&near, i);
        while (j > 0 && nuke_before(g, i, near.data[j - 1])) {
            near.data[j] = near.data[j - 1];
            j--;
        }
        near.data[j] = i;
    }
    SamTarget *targets = (SamTarget *)ofe_xmalloc((size_t)(near.len + 1) * sizeof(SamTarget));
    int32_t nt = 0;
    for (int32_t k = 0; k < near.len; k++) {
        int32_t nuke = near.data[k], id = u->uid[nuke];
        SamCache *c = cache_find(&x->cache, id);
        if (c) {
            c->last_seen = ticks;
            if (c->tick == -2 || c->tick == -1) continue;
            if (c->tick == ticks || c->tick == ticks + 1) {
                targets[nt].slot = nuke;
                targets[nt++].tile = c->tile;
                cache_erase(&x->cache, c);
                continue;
            }
            if (c->tick > ticks) continue;
            cache_erase(&x->cache, c);
        }
        SamCache res = compute_interception(g, slot, nuke, ticks);
        if (res.tick >= 0 && res.tick <= 1) {
            targets[nt].slot = nuke;
            targets[nt++].tile = res.tile;
        } else {
            res.tick = res.tick >= 0 ? res.tick + ticks : res.tick;
            SamCacheVec_push(&x->cache, res);
        }
    }
    for (int32_t i = x->cache.len - 1; i >= 0; i--) { /* updateUnreachableNukes */
        if (x->cache.data[i].last_seen != ticks) cache_erase(&x->cache, &x->cache.data[i]);
    }
    if (nt > 1) { /* stable sort by score, best first */
        for (int32_t i = 0; i < nt; i++) targets[i].score = target_score(g, slot, targets[i].slot);
        for (int32_t i = 1; i < nt; i++) {
            SamTarget t = targets[i];
            int32_t j = i - 1;
            while (j >= 0 && targets[j].score < t.score) {
                targets[j + 1] = targets[j];
                j--;
            }
            targets[j + 1] = t;
        }
    }
    for (int32_t i = 0; i < nt; i++) {
        if (in_cooldown(g, slot)) break;
        launch_missile(g, slot);
        u->aux2[targets[i].slot] |= UF_NUKE_TARGETED_BY_SAM;
        UnitExec *m = uexec_new(UX_SAM_MISSILE, u->owner[slot]);
        m->tile = u->tile[slot]; /* spawn */
        m->owner_unit = u->uid[slot];
        m->target_unit = u->uid[targets[i].slot];
        m->target_tile = targets[i].tile;
        ts_exec_add_unit(g, m);
    }
    free(targets);
    IntVec_free(&near);
    return true;
}

/* SAMMissileExecution.tick (also run once from init). */
static bool sam_missile_tick(OfeGame *g, UnitExec *x) {
    UnitTable *u = &g->units;
    int32_t slot;
    if (x->uid == 0) {
        slot = build_unit(g, x->player, OFE_UNIT_SAM_MISSILE, x->tile);
        if (slot < 0) return false;
        x->uid = u->uid[slot];
        u->target_unit[slot] = x->target_unit;
    } else {
        slot = unit_lookup(g, x->uid);
        if (slot < 0) return false;
    }
    int32_t tgt = unit_lookup(g, x->target_unit);
    if (tgt < 0 || !unit_alive_id(g, x->owner_unit) || u->owner[tgt] == u->owner[slot] || !is_nuke_type(u->type[tgt])) {
        if (tgt >= 0) u->aux2[tgt] &= ~UF_NUKE_TARGETED_BY_SAM; /* other SAMs may retarget it */
        unit_destroy(g, x->uid, 0);
        return false;
    }
    for (int32_t i = 0; i < SAM_MISSILE_SPEED; i++) {
        TsPathResult r = ts_air_next(&x->air->pf, u->tile[slot], x->target_tile);
        if (r.status == TS_PATH_COMPLETE) {
            unit_destroy(g, x->target_unit, x->player);
            unit_destroy(g, x->uid, 0);
            return false;
        }
        if (r.status == TS_PATH_NEXT) unit_move(g, slot, r.node);
    }
    return true;
}

/* ---- MIRV ----------------------------------------------------------------------------------------------- */

static int32_t mirv_cell(int32_t rel) { return (int32_t)floor((double)rel / MIRV_MIN_SPREAD); }

static bool mirv_cell_of(const MirvData *m, const OfeMap *map, TileRef t, int32_t *cx, int32_t *cy) {
    *cx = mirv_cell(map_x(map, t) - m->base_x + MIRV_RANGE);
    *cy = mirv_cell(map_y(map, t) - m->base_y + MIRV_RANGE);
    return *cx >= 0 && *cx < MIRV_GRID_W && *cy >= 0 && *cy < MIRV_GRID_W;
}

static void mirv_add_to_grid(MirvData *m, const OfeMap *map, TileRef t) {
    int32_t cx, cy;
    if (!mirv_cell_of(m, map, t, &cx, &cy)) return;
    int32_t base = (cy * MIRV_GRID_W + cx) * 2;
    for (int32_t i = 0; i < 2; i++) {
        if (m->cells[base + i] == -1) {
            m->cells[base + i] = t;
            return;
        }
    }
}

static void mirv_remove_from_grid(MirvData *m, const OfeMap *map, TileRef t) {
    if (!m->cells) return;
    int32_t cx, cy;
    if (!mirv_cell_of(m, map, t, &cx, &cy)) return;
    int32_t base = (cy * MIRV_GRID_W + cx) * 2;
    if (m->cells[base] == t) {
        m->cells[base] = m->cells[base + 1];
        m->cells[base + 1] = -1;
    } else if (m->cells[base + 1] == t) {
        m->cells[base + 1] = -1;
    }
}

static bool mirv_overlapping(const MirvData *m, const OfeMap *map, int32_t x, int32_t y) {
    int32_t cx = mirv_cell(x - m->base_x + MIRV_RANGE), cy = mirv_cell(y - m->base_y + MIRV_RANGE);
    if (cx < 0 || cx >= MIRV_GRID_W || cy < 0 || cy >= MIRV_GRID_W) return true;
    if (m->cells[(cy * MIRV_GRID_W + cx) * 2 + 1] != -1) return true;
    for (int32_t gy = cy - 1; gy <= cy + 1; gy++) {
        if (gy < 0 || gy >= MIRV_GRID_W) continue;
        for (int32_t gx = cx - 1; gx <= cx + 1; gx++) {
            if (gx < 0 || gx >= MIRV_GRID_W) continue;
            for (int32_t i = 0; i < 2; i++) {
                int32_t t = m->cells[(gy * MIRV_GRID_W + gx) * 2 + i];
                if (t == -1) break;
                int32_t dx = abs(x - map_x(map, t));
                if (dx < MIRV_MIN_SPREAD && dx + abs(y - map_y(map, t)) < MIRV_MIN_SPREAD) return true;
            }
        }
    }
    return false;
}

/* MirvExecution.tryGenerateTarget: a random tile of the victim near the target, at least 55 from
 * every other warhead target; -1 when none is found (or the area is saturated). */
static TileRef mirv_try_generate(OfeGame *g, MirvData *m) {
    const OfeMap *map = g->map;
    if (m->overlap_failures >= MIRV_SATURATION) return -1;
    for (int32_t attempt = 0; attempt < 100; attempt++) {
        double r1 = ts_rand_next(&m->random);
        double r2 = fmod(r1 * 15485863, 1.0);
        int32_t x = m->min_x + (int32_t)floor(r1 * (m->max_x - m->min_x + 1));
        int32_t y = m->min_y + (int32_t)floor(r2 * (m->max_y - m->min_y + 1));
        int64_t dx = x - m->base_x, dy = y - m->base_y;
        if (dx * dx + dy * dy > (int64_t)MIRV_RANGE * MIRV_RANGE) continue;
        TileRef tile = map_ref(map, x, y);
        if (tile_owner(g, tile) != m->target_player) continue;
        if (!map_is_land(map, tile) || map_is_impassable(map, tile)) continue;
        if (mirv_overlapping(m, map, x, y)) {
            if (++m->overlap_failures >= MIRV_SATURATION) return -1;
            continue;
        }
        m->overlap_failures = 0;
        return tile;
    }
    return -1;
}

static void mirv_top_up(OfeGame *g, MirvData *m, int32_t attempts) {
    for (int32_t attempt = 0; attempt < attempts; attempt++) {
        if (m->staged.len >= MIRV_WARHEADS) break;
        TileRef t = mirv_try_generate(g, m);
        if (t >= 0) {
            IntVec_push(&m->staged, t);
            mirv_add_to_grid(m, g->map, t);
        }
        if (m->overlap_failures >= MIRV_SATURATION) break;
    }
}

/* MirvExecution.finalizeDestinations: drop targets whose owner changed, top up, sort farthest first. */
static void mirv_finalize(OfeGame *g, MirvData *m, TileRef dst, int32_t extra_attempts) {
    int32_t before = m->staged.len, w = 0;
    for (int32_t i = 0; i < m->staged.len; i++) {
        TileRef t = m->staged.data[i];
        if (t == dst || tile_owner(g, t) == m->target_player) m->staged.data[w++] = t;
        else mirv_remove_from_grid(m, g->map, t);
    }
    m->staged.len = w;
    if (w < before) m->overlap_failures = 0;
    if (m->overlap_failures < MIRV_SATURATION) mirv_top_up(g, m, extra_attempts);
    /* stable insertion sort, farthest from the target first */
    for (int32_t i = 1; i < m->staged.len; i++) {
        TileRef t = m->staged.data[i];
        int32_t d = map_manhattan(g->map, t, dst), j = i - 1;
        while (j >= 0 && map_manhattan(g->map, m->staged.data[j], dst) < d) {
            m->staged.data[j + 1] = m->staged.data[j];
            j--;
        }
        m->staged.data[j + 1] = t;
    }
    free(m->cells);
    m->cells = NULL;
}

static void mirv_init(OfeGame *g, UnitExec *x) {
    MirvData *m = (MirvData *)ofe_xcalloc(1, sizeof(*m));
    x->mv = m;
    const OfeMap *map = g->map;
    ts_rand_init(&m->random, (int64_t)g->tick + (int64_t)g->players.id_hash[x->player]);
    m->grid = ts_grid_from_map(map);
    m->target_player = tile_owner(g, x->tile);
    m->base_x = map_x(map, x->tile);
    m->base_y = map_y(map, x->tile);
    m->min_x = OFE_MAX(0, m->base_x - MIRV_RANGE);
    m->max_x = OFE_MIN(map->width - 1, m->base_x + MIRV_RANGE);
    m->min_y = OFE_MAX(0, m->base_y - MIRV_RANGE);
    m->max_y = OFE_MIN(map->height - 1, m->base_y + MIRV_RANGE);
    IntVec_push(&m->staged, x->tile);
    m->cells = (int32_t *)ofe_xmalloc((size_t)MIRV_GRID_W * MIRV_GRID_W * 2 * sizeof(int32_t));
    for (int32_t i = 0; i < MIRV_GRID_W * MIRV_GRID_W * 2; i++) m->cells[i] = -1;
    mirv_add_to_grid(m, map, x->tile);
}

static void mirv_spawn_warheads(OfeGame *g, UnitExec *x, int32_t remaining) {
    MirvData *m = x->mv;
    int32_t wait_base = OFE_MAX(0, remaining);
    for (int32_t i = 0; i < m->staged.len; i++) {
        int32_t offset = 4;
        if (i < 70) offset = 0;
        else if (i < 140) offset = 1;
        else if (i < 210) offset = 2;
        else if (i < 280) offset = 3;
        int32_t wait = wait_base + (int32_t)ts_rand_int(&m->random, 0, 15);
        UnitExec *w = new_nuke(x->player, OFE_UNIT_MIRV_WARHEAD, m->staged.data[i], m->separate_dst, 22 + offset, wait, true);
        IntVec_push(&m->warheads, ts_exec_add_unit(g, w));
    }
}

/* MirvExecution.tick. */
static bool mirv_tick(OfeGame *g, UnitExec *x) {
    UnitTable *u = &g->units;
    MirvData *m = x->mv;
    PlayerId p = x->player;
    int32_t slot;
    if (x->uid == 0) {
        if (!can_build_unit_type(g, p, OFE_UNIT_MIRV) || !tile_has_owner(g, x->tile)) return false;
        TileRef spawn = nuke_spawn(g, p, OFE_UNIT_MIRV, x->tile);
        if (spawn < 0) return false;
        m->target_player = tile_owner(g, x->tile);
        m->spawn_tile = spawn;
        slot = build_unit(g, p, OFE_UNIT_MIRV, spawn);
        if (slot < 0) return false;
        x->uid = u->uid[slot];
        u->target_tile[slot] = x->tile;
        u->aux3[slot] = m->target_player;
        g->mirvs_launched++;
        /* Betrayal on launch, only once the missile exists. */
        if (m->target_player != 0) {
            if (alliance_find(g, p, m->target_player) >= 0) alliance_break_raw(g, p, m->target_player);
            if (m->target_player != p) {
                player_update_relation(g, m->target_player, p, -100);
                player_update_relation(g, p, m->target_player, -100);
            }
        }
        int32_t sx = (int32_t)floor((m->base_x + map_x(g->map, u->tile[slot])) / 2.0);
        int32_t sy = OFE_MAX(0, m->base_y - 500) + 50;
        m->separate_dst = map_ref(g->map, sx, sy);
        double speed = ts_mirv_speed(&m->grid, m->spawn_tile, m->separate_dst);
        TsParabolaOpts o = ts_parabola_default_opts();
        o.increment = speed;
        TsParabola pf;
        ts_parabola_init(&pf, &m->grid, &o);
        TsPathResult r = ts_parabola_next(&pf, m->spawn_tile, m->separate_dst, speed);
        while (r.status == TS_PATH_NEXT) {
            IntVec_push(&m->full_path, r.node);
            r = ts_parabola_next(&pf, m->spawn_tile, m->separate_dst, speed);
        }
        ts_parabola_free(&pf);
        int32_t silo = silo_at(g, p, spawn);
        if (silo >= 0) launch_missile(g, silo);
    } else {
        slot = unit_lookup(g, x->uid);
    }
    if (slot < 0) { /* destroyed or cancelled: its warheads are called off */
        for (int32_t i = 0; i < m->warheads.len; i++) {
            UnitExec *w = ts_exec_find_unit(g, m->warheads.data[i]);
            if (w) uexec_cancel(g, w);
        }
        m->warheads.len = 0;
        return false;
    }
    int32_t remaining = m->full_path.len - m->path_index;
    if (remaining <= 20 && remaining > 10) { /* stage destinations over ticks 20 .. 11 before separation */
        if (m->overlap_failures < MIRV_SATURATION && m->staged.len < MIRV_WARHEADS) {
            for (int32_t attempt = 0; attempt < 100; attempt++) {
                if (m->staged.len >= MIRV_WARHEADS) break;
                TileRef t = mirv_try_generate(g, m);
                if (t >= 0) {
                    IntVec_push(&m->staged, t);
                    mirv_add_to_grid(m, g->map, t);
                }
                if (m->overlap_failures >= MIRV_SATURATION) break;
            }
        }
    }
    if (remaining <= 10 && !m->warheads_spawned) {
        m->warheads_spawned = true;
        mirv_finalize(g, m, x->tile, 500 + (10 - remaining) * 50);
        mirv_spawn_warheads(g, x, remaining);
    }
    if (m->path_index < m->full_path.len) {
        unit_move(g, slot, m->full_path.data[m->path_index++]);
        return true;
    }
    unit_destroy(g, x->uid, 0); /* separate */
    return false;
}

/* ---- Entry points ------------------------------------------------------------------------------------------ */

bool uexec_init(OfeGame *g, UnitExec *x) {
    switch (x->kind) {
    case UX_CONSTRUCTION:
        if ((x->type < 0 || x->type >= OFE_UNIT_TYPE_COUNT) || (g->cfg.disabled_units & (1u << x->type)) || !valid_tile(g, x->tile)) x->active = 0;
        break;
    case UX_UPGRADE:
        upgrade_init(g, x);
        x->active = 0;
        break;
    case UX_DELETE: x->active = delete_init(g, x); break;
    case UX_NUKE: nuke_init(g, x); break;
    case UX_MIRV: mirv_init(g, x); break;
    case UX_SAM_MISSILE:
        x->air = (AirFlight *)ofe_xcalloc(1, sizeof(AirFlight));
        x->air->grid = ts_grid_from_map(g->map);
        ts_air_init(&x->air->pf, &x->air->grid, g->tick);
        if (!sam_missile_tick(g, x)) x->active = 0; /* init runs the first tick */
        break;
    default: break;
    }
    return x->active;
}

bool uexec_tick(OfeGame *g, UnitExec *x) {
    bool on = true;
    switch (x->kind) {
    case UX_CONSTRUCTION: on = construction_tick(g, x); break;
    case UX_DELETE: on = delete_tick(g, x); break;
    case UX_SILO: on = silo_tick(g, x); break;
    case UX_SAM: on = sam_tick(g, x); break;
    case UX_SAM_MISSILE: on = sam_missile_tick(g, x); break;
    case UX_NUKE: on = nuke_tick(g, x); break;
    case UX_MIRV: on = mirv_tick(g, x); break;
    default: on = false; break;
    }
    if (!on) x->active = 0;
    return x->active;
}

/* ---- PlayerExecution's unit loop, alliance cancel ---------------------------------------------------------------- */

void player_structures_tick_ts(OfeGame *g, PlayerId p) {
    UnitTable *u = &g->units;
    IntVec lost = {0};
    for (int32_t i = 0; i < u->count; i++) {
        if (u->alive[i] && u->owner[i] == p && unit_is_structure(u->type[i]) && tile_owner(g, u->tile[i]) != p) IntVec_push(&lost, i);
    }
    /* Process in the order of the player's unit list. */
    for (int32_t i = 1; i < lost.len; i++) {
        int32_t s = lost.data[i], j = i - 1;
        while (j >= 0 && u->own_seq[lost.data[j]] > u->own_seq[s]) {
            lost.data[j + 1] = lost.data[j];
            j--;
        }
        lost.data[j + 1] = s;
    }
    for (int32_t i = 0; i < lost.len; i++) {
        int32_t s = lost.data[i];
        PlayerId owner = tile_owner(g, u->tile[s]);
        UnitId id = unit_id_of_slot(g, s);
        if (owner == 0) unit_destroy(g, id, 0);
        else if (u->type[s] == OFE_UNIT_DEFENSE_POST) unit_destroy(g, id, owner);
        else unit_capture(g, id, owner);
    }
    IntVec_free(&lost);
}

/* wouldNukeBreakAlliance for a single ally. */
static bool would_break_alliance(const OfeGame *g, TileRef target, int type, PlayerId ally) {
    int32_t inner, outer;
    nuke_magnitude(type, &inner, &outer);
    if (structures_in_range(g, target, outer, ally, NULL)) return true;
    BlastWeightVec counts = {0};
    blast_counts(g, target, inner, outer, &counts);
    bool result = false;
    for (int32_t i = 0; i < counts.len; i++) result |= counts.data[i].p == ally && counts.data[i].w > ALLIANCE_BREAK_THRESHOLD;
    BlastWeightVec_free(&counts);
    return result;
}

void nukes_ts_cancel_between(OfeGame *g, PlayerId a, PlayerId b) {
    static const uint32_t kinds = (1u << OFE_UNIT_ATOM_BOMB) | (1u << OFE_UNIT_HYDROGEN_BOMB) | (1u << OFE_UNIT_MIRV) |
                                  (1u << OFE_UNIT_MIRV_WARHEAD);
    PlayerId launchers[2] = {a, b};
    for (int32_t l = 0; l < 2; l++) {
        PlayerId launcher = launchers[l], other = l == 0 ? b : a;
        IntVec units = {0};
        player_units(g, launcher, kinds, &units);
        for (int32_t i = 0; i < units.len; i++) {
            int32_t s = units.data[i];
            const UnitTable *u = &g->units;
            if (!u->alive[s] || (u->aux2[s] & UF_NUKE_REACHED)) continue;
            TileRef target = u->target_tile[s];
            int type = u->type[s];
            if (type == OFE_UNIT_MIRV) {
                /* The target player captured at launch, not whoever owns the tile now. */
                PlayerId t = u->aux3[s] >= 0 ? u->aux3[s] : (target >= 0 ? tile_owner(g, target) : -1);
                if (t != other) continue;
            } else if (type == OFE_UNIT_MIRV_WARHEAD) {
                if (target < 0 || tile_owner(g, target) != other) continue;
            } else if (target < 0 || !would_break_alliance(g, target, type, other)) {
                continue;
            }
            unit_destroy(g, unit_id_of_slot(g, s), 0);
        }
        IntVec_free(&units);
    }
}

#endif /* OFE_TS_PARITY */

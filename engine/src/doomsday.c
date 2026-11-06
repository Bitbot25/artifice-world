/*
 * doomsday.c - the doomsday clock (anti-stall), off unless cfg.doomsday_enabled.
 *
 * Rules follow OpenFront's DoomsdayClockExecution / DoomsdayClock.ts. Once per
 * second every side (a player in FFA, a whole team in team games; tribes are
 * exempt) must hold a rising share of the clean land. The bar climbs in seven
 * linear waves after a 10 minute grace (slow / normal / fast pacing, higher
 * rungs in team games). The side with the most land is never doomed. A side
 * below the bar:
 *   - is flagged (players.doomsday_tick = tick it entered, OFE_EV_DOOMSDAY);
 *   - after 30 s bleeds troops toward a floor that decays 40% -> 5% of max over
 *     90 s (the drain ramps 2% -> 5% of max per second over 90 s);
 *   - once past 120 s and at the floor, loses territory (rot) so that it holds
 *     nothing at 150 s: pinholes first (15% of the land over the first 10 s),
 *     then growing holes, at least ceil(tiles left / seconds left) per second.
 *     Rotted tiles become unowned fallout;
 *   - loses warship health on its own steeper ramp, down to 5% of max.
 * Climbing back over the bar clears the flag and any rot in progress.
 *
 * Simplifications versus the TypeScript engine:
 *   - Rot does not rank every tile of the player each second. Pinholes are
 *     drawn by sampling: a random border tile, then a short random walk inward
 *     (so holes land in the interior, spread out, not clotted on the coast).
 *     Growth takes random tiles from the rot front (neighbours of holes); the
 *     TS "tips first" ranking is dropped, so holes grow as compact blobs. Work
 *     per second is proportional to the tiles rotted, never to the territory.
 *   - Rot tile choice uses a splitmix stream in the state, not tile hashes.
 *   - Warship max health is taken as 1000 (no veterancy). Ships must not heal
 *     while players.doomsday_tick >= 0 for the drain to land (ships.c).
 */
#include "internal.h"

#define CLOCK_PERIOD_TICKS 10
#define WARN_SECONDS 30
#define DRAIN_START_PERCENT 2
#define DRAIN_MAX_PERCENT 5
#define DRAIN_RAMP_SECONDS 90
#define DRAIN_FLOOR_PERCENT 5
#define FLOOR_START_PERCENT 40
#define FLOOR_DECAY_SECONDS 90
#define ROT_DEATH_SECONDS 150
#define ROT_GRAIN_SECONDS 10
#define ROT_SPECKLE_PERCENT 15
#define WARSHIP_DRAIN_START_PERCENT 1
#define WARSHIP_DRAIN_MAX_PERCENT 50
#define WARSHIP_DRAIN_CURVE_EXPONENT 8
#define WARSHIP_MAX_HEALTH 1000
#define DRAIN_CURVE_SCALE 1000000
#define MAX_TEAMS 16

/* ---- Schedule --------------------------------------------------------------------- */

#define NUM_WAVES 7
static const int32_t LEVELS[NUM_WAVES] = {200, 400, 700, 1100, 1700, 2500, 3500}; /* basis points */
static const int32_t LEVELS_TEAM[NUM_WAVES] = {300, 600, 1000, 1500, 2100, 2800, 3500};

typedef struct Schedule {
    int32_t grace, ramp, pause; /* seconds; the last wave has no pause */
} Schedule;

static const Schedule SCHEDULES[3] = {
    {600, 240, 70}, /* slow: 35% at 45:00 */
    {600, 168, 54}, /* normal: 35% at 35:00 */
    {600, 102, 31}, /* fast: 35% at 25:00 */
};

/* Required share in basis points at `elapsed` seconds. */
static int32_t required_basis_points(const OfeGame *g, int32_t elapsed) {
    const Schedule *s = &SCHEDULES[OFE_CLAMP(g->cfg.doomsday_speed, 0, 2)];
    const int32_t *levels = g->cfg.game_mode == OFE_MODE_TEAM ? LEVELS_TEAM : LEVELS;
    if (elapsed <= s->grace) return 0;
    int32_t t = elapsed - s->grace, prev = 0;
    for (int32_t i = 0; i < NUM_WAVES; i++) {
        int32_t target = levels[i];
        if (t < s->ramp) return prev + (target - prev) * t / s->ramp;
        t -= s->ramp;
        int32_t pause = i == NUM_WAVES - 1 ? 0 : s->pause;
        if (t < pause) return target;
        t -= pause;
        prev = target;
    }
    return levels[NUM_WAVES - 1];
}

int32_t doomsday_bar_tenths(const OfeGame *g) {
    if (!g->cfg.doomsday_enabled || g->start_tick < 0) return 0;
    return required_basis_points(g, (int32_t)floor(game_elapsed_seconds(g))) / 10;
}

/* ---- Drain and floor (integer math, as in TS) ---------------------------------------- */

static int64_t troop_floor(int64_t max_troops, int32_t seconds_past_warn) {
    int32_t span = FLOOR_START_PERCENT - DRAIN_FLOOR_PERCENT;
    int32_t t = OFE_MAX(0, seconds_past_warn);
    int32_t pct = t < FLOOR_DECAY_SECONDS ? FLOOR_START_PERCENT - span * t / FLOOR_DECAY_SECONDS : DRAIN_FLOOR_PERCENT;
    return max_troops * pct / 100;
}

/* (t/r)^exponent as a fraction of DRAIN_CURVE_SCALE. */
static int64_t drain_curve(int64_t t, int64_t r, int32_t exponent) {
    int64_t ratio = t * DRAIN_CURVE_SCALE / r, acc = DRAIN_CURVE_SCALE;
    for (int32_t i = 0; i < exponent; i++) acc = acc * ratio / DRAIN_CURVE_SCALE;
    return acc;
}

/* Loss this second as a percentage of `max` that ramps from start to top over
 * DRAIN_RAMP_SECONDS (convex when exponent > 1). */
static int64_t drain(int64_t max, int32_t seconds_past_warn, int32_t start, int32_t top, int32_t exponent) {
    int64_t t = OFE_MAX(0, seconds_past_warn), pct = top;
    if (t < DRAIN_RAMP_SECONDS) {
        int64_t span = top - start;
        int64_t grown = exponent <= 1 ? span * t / DRAIN_RAMP_SECONDS : span * drain_curve(t, DRAIN_RAMP_SECONDS, exponent) / DRAIN_CURVE_SCALE;
        pct = start + grown;
    }
    return OFE_MAX((int64_t)1, max * pct / 100);
}

/* ---- State: rot progress of doomed players -------------------------------------------- */

typedef struct RotEntry {
    PlayerId player;
    int32_t since; /* tick rot began */
    int32_t held; /* tiles held then: sets the pinhole density */
    IntVec front; /* tiles next to holes, candidates to rot next */
} RotEntry;

struct DoomsdayState {
    RotEntry *rot;
    int32_t num_rot, cap_rot;
    IntSet in_front; /* every tile in some front: keeps the fronts duplicate-free */
    uint64_t rng;
};

DoomsdayState *doomsday_state_new(OfeGame *g) {
    DoomsdayState *ds = (DoomsdayState *)ofe_xcalloc(1, sizeof(DoomsdayState));
    ds->rng = rng_derive(g->seed, 0x646f6f6dull); /* "doom" */
    return ds;
}

void doomsday_state_free(OfeGame *g) {
    DoomsdayState *ds = g->sub->doomsday;
    if (!ds) return;
    for (int32_t i = 0; i < ds->cap_rot; i++) IntVec_free(&ds->rot[i].front);
    free(ds->rot);
    intset_free(&ds->in_front);
    free(ds);
    g->sub->doomsday = NULL;
}

void doomsday_state_copy(OfeGame *dst, const OfeGame *src) {
    const DoomsdayState *s = src->sub->doomsday;
    if (!dst->sub->doomsday) dst->sub->doomsday = doomsday_state_new(dst);
    DoomsdayState *d = dst->sub->doomsday;
    if (d->cap_rot < s->num_rot) {
        d->rot = (RotEntry *)ofe_xrealloc(d->rot, (size_t)s->num_rot * sizeof(RotEntry));
        memset(d->rot + d->cap_rot, 0, (size_t)(s->num_rot - d->cap_rot) * sizeof(RotEntry));
        d->cap_rot = s->num_rot;
    }
    for (int32_t i = 0; i < s->num_rot; i++) {
        d->rot[i].player = s->rot[i].player;
        d->rot[i].since = s->rot[i].since;
        d->rot[i].held = s->rot[i].held;
        IntVec_copy(&d->rot[i].front, &s->rot[i].front);
    }
    d->num_rot = s->num_rot;
    intset_copy(&d->in_front, &s->in_front);
    d->rng = s->rng;
}

static RotEntry *rot_find(DoomsdayState *ds, PlayerId p) {
    for (int32_t i = 0; i < ds->num_rot; i++) {
        if (ds->rot[i].player == p) return &ds->rot[i];
    }
    return NULL;
}

static RotEntry *rot_start(DoomsdayState *ds, PlayerId p, int32_t tick, int32_t held) {
    if (ds->num_rot == ds->cap_rot) {
        int32_t cap = ds->cap_rot ? ds->cap_rot * 2 : 4;
        ds->rot = (RotEntry *)ofe_xrealloc(ds->rot, (size_t)cap * sizeof(RotEntry));
        memset(ds->rot + ds->cap_rot, 0, (size_t)(cap - ds->cap_rot) * sizeof(RotEntry));
        ds->cap_rot = cap;
    }
    /* Entries past num_rot keep their IntVec buffers for reuse. */
    RotEntry *e = &ds->rot[ds->num_rot++];
    e->player = p;
    e->since = tick;
    e->held = held;
    e->front.len = 0;
    return e;
}

/* Forgets p's rot (recovered or dead). */
static void rot_stop(DoomsdayState *ds, PlayerId p) {
    for (int32_t i = 0; i < ds->num_rot; i++) {
        if (ds->rot[i].player != p) continue;
        for (int32_t k = 0; k < ds->rot[i].front.len; k++) intset_remove(&ds->in_front, ds->rot[i].front.data[k]);
        /* Swap with the last live entry, keeping both buffers. */
        RotEntry tmp = ds->rot[i];
        ds->rot[i] = ds->rot[ds->num_rot - 1];
        ds->rot[ds->num_rot - 1] = tmp;
        ds->num_rot--;
        return;
    }
}

/* ---- Rot ------------------------------------------------------------------------------- */

static void front_push(DoomsdayState *ds, RotEntry *e, TileRef t) {
    if (intset_add(&ds->in_front, t)) IntVec_push(&e->front, t);
}

/* Takes tile t from p: unowned fallout, its neighbours join the front.
 * False if p no longer holds it. */
static bool rot_tile(OfeGame *g, DoomsdayState *ds, RotEntry *e, TileRef t) {
    if (tile_owner(g, t) != e->player) return false;
    tile_set_owner(g, t, 0);
    tile_set_fallout(g, t, true);
    TileRef nb[4];
    int n = map_neighbors4(g->map, t, nb);
    for (int i = 0; i < n; i++) {
        if (tile_owner(g, nb[i]) == e->player) front_push(ds, e, nb[i]);
    }
    return true;
}

/* A tile of p away from the coast: random border tile, then a random walk
 * inward through p's own land. */
static TileRef sample_interior(OfeGame *g, DoomsdayState *ds, PlayerId p) {
    const IntVec *border = &g->players.border[p];
    TileRef t = border->data[rng_int(&ds->rng, 0, border->len)];
    int32_t steps = rng_int(&ds->rng, 1, 40);
    int32_t dir = rng_int(&ds->rng, 0, 4);
    static const int32_t DX[4] = {1, -1, 0, 0}, DY[4] = {0, 0, 1, -1};
    int32_t x = map_x(g->map, t), y = map_y(g->map, t);
    for (int32_t i = 0; i < steps; i++) {
        int32_t nx = x + DX[dir], ny = y + DY[dir];
        if (!map_in_bounds(g->map, nx, ny) || tile_owner(g, map_ref(g->map, nx, ny)) != p) break;
        x = nx;
        y = ny;
    }
    return map_ref(g->map, x, y);
}

static int32_t speckle(OfeGame *g, DoomsdayState *ds, RotEntry *e, int32_t count) {
    int32_t opened = 0;
    for (int32_t tries = 0; opened < count && tries < count * 4 + 16 && g->players.border[e->player].len > 0; tries++) {
        if (rot_tile(g, ds, e, sample_interior(g, ds, e->player))) opened++;
    }
    /* Sampling keeps hitting rotted ground (small remainder): take border tiles. */
    while (opened < count && g->players.border[e->player].len > 0) {
        const IntVec *b = &g->players.border[e->player];
        if (rot_tile(g, ds, e, b->data[rng_int(&ds->rng, 0, b->len)])) opened++;
    }
    return opened;
}

static int32_t spread(OfeGame *g, DoomsdayState *ds, RotEntry *e, int32_t count) {
    int32_t taken = 0;
    while (taken < count && e->front.len > 0) {
        int32_t i = rng_int(&ds->rng, 0, e->front.len);
        TileRef t = e->front.data[i];
        e->front.data[i] = e->front.data[--e->front.len];
        intset_remove(&ds->in_front, t);
        if (rot_tile(g, ds, e, t)) taken++;
    }
    return taken;
}

/* Takes tiles so that p holds none ROT_DEATH_SECONDS after its flag appeared. */
static void rot_player(OfeGame *g, DoomsdayState *ds, PlayerId p, int32_t seconds_under) {
    int32_t owned = g->players.num_tiles[p];
    if (owned <= 0) return;
    RotEntry *e = rot_find(ds, p);
    if (!e) e = rot_start(ds, p, g->tick, owned);

    int32_t seconds_left = OFE_MAX(1, ROT_DEATH_SECONDS - seconds_under);
    int32_t quota = (owned + seconds_left - 1) / seconds_left;
    int32_t specks = 0;
    if (g->tick - e->since < ROT_GRAIN_SECONDS * TICKS_PER_SECOND) {
        int64_t per_second = ((int64_t)e->held * ROT_SPECKLE_PERCENT + 99) / 100;
        specks = (int32_t)OFE_MAX((int64_t)1, (per_second + ROT_GRAIN_SECONDS - 1) / ROT_GRAIN_SECONDS);
    }
    int32_t budget = OFE_MIN(owned, OFE_MAX(quota, specks));
    budget -= speckle(g, ds, e, OFE_MIN(specks, budget));
    budget -= spread(g, ds, e, budget);
    if (budget > 0) speckle(g, ds, e, budget); /* front exhausted */
}

/* ---- Flags ------------------------------------------------------------------------------ */

static void enter_clock(OfeGame *g, PlayerId p) {
    if (g->players.doomsday_tick[p] >= 0) return;
    g->players.doomsday_tick[p] = g->tick;
    event_emit(g, OFE_EV_DOOMSDAY, 0, p, 1, 0, 0);
}

static void clear_clock(OfeGame *g, DoomsdayState *ds, PlayerId p) {
    if (g->players.doomsday_tick[p] >= 0) {
        g->players.doomsday_tick[p] = -1;
        event_emit(g, OFE_EV_DOOMSDAY, 0, p, 0, 0, 0);
    }
    if (ds->num_rot > 0) rot_stop(ds, p);
}

/* Bleeds a doomed player's warships toward the floor. */
static void drain_warships(OfeGame *g, PlayerId p, int32_t seconds_past_warn) {
    UnitTable *t = &g->units;
    if (g->players.unit_count[p][OFE_UNIT_WARSHIP] == 0) return;
    int32_t ship_floor = WARSHIP_MAX_HEALTH * DRAIN_FLOOR_PERCENT / 100;
    for (int32_t s = 0; s < t->count; s++) {
        if (!t->alive[s] || t->type[s] != OFE_UNIT_WARSHIP || t->owner[s] != p) continue;
        int32_t removable = t->health[s] - ship_floor;
        if (removable <= 0) continue;
        int64_t dmg = drain(WARSHIP_MAX_HEALTH, seconds_past_warn, WARSHIP_DRAIN_START_PERCENT, WARSHIP_DRAIN_MAX_PERCENT,
                            WARSHIP_DRAIN_CURVE_EXPONENT);
        t->health[s] -= (int32_t)OFE_MIN(dmg, (int64_t)removable);
    }
}

static void punish(OfeGame *g, DoomsdayState *ds, PlayerId p) {
    enter_clock(g, p);
    int32_t seconds_under = (g->tick - g->players.doomsday_tick[p]) / TICKS_PER_SECOND;
    if (seconds_under < WARN_SECONDS) return;
    int32_t past = seconds_under - WARN_SECONDS;
    int64_t max_troops = (int64_t)player_max_troops(g, p);
    int64_t floor_troops = troop_floor(max_troops, past);
    int64_t chunk = drain(max_troops, past, DRAIN_START_PERCENT, DRAIN_MAX_PERCENT, 1);
    int64_t above = OFE_MAX((int64_t)0, g->players.troops[p] - floor_troops);
    player_remove_troops(g, p, (double)OFE_MIN(chunk, above));
    if (past >= FLOOR_DECAY_SECONDS && g->players.troops[p] <= floor_troops) rot_player(g, ds, p, seconds_under);
    drain_warships(g, p, past);
}

/* ---- Tick --------------------------------------------------------------------------------- */

void doomsday_tick(OfeGame *g) {
    if (!g->cfg.doomsday_enabled || g->tick % CLOCK_PERIOD_TICKS != 0) return;
    DoomsdayState *ds = g->sub->doomsday;
    const PlayerTable *pl = &g->players;
    bool team_game = g->cfg.game_mode == OFE_MODE_TEAM;
    int32_t num_teams = OFE_CLAMP(g->cfg.num_teams, 1, MAX_TEAMS);

    /* Sides: living non-tribe players (FFA) or teams with a living member. */
    int64_t team_tiles[MAX_TEAMS] = {0};
    bool team_alive[MAX_TEAMS] = {false};
    int32_t sides = 0;
    PlayerId leader = 0;
    int32_t leader_team = -1;
    for (PlayerId p = 1; p < pl->count; p++) {
        if (pl->type[p] == OFE_PLAYER_BOT || !player_alive(g, p)) continue;
        if (!team_game) {
            sides++;
            if (leader == 0 || pl->num_tiles[p] > pl->num_tiles[leader]) leader = p;
        } else if (pl->team[p] >= 0 && pl->team[p] < num_teams) {
            team_tiles[pl->team[p]] += pl->num_tiles[p];
            team_alive[pl->team[p]] = true;
        }
    }
    if (team_game) {
        for (int32_t t = 0; t < num_teams; t++) {
            if (!team_alive[t]) continue;
            sides++;
            if (leader_team < 0 || team_tiles[t] > team_tiles[leader_team]) leader_team = t;
        }
    }

    /* Nobody is judged once the game is decided or only one side is left. */
    bool idle = g->game_over || sides < 2;
    int64_t land = (int64_t)g->num_land_tiles - g->num_fallout_tiles;
    int64_t required = land > 0 ? (int64_t)required_basis_points(g, (int32_t)floor(game_elapsed_seconds(g))) * land / 10000 : 0;

    for (PlayerId p = 1; p < pl->count; p++) {
        if (pl->type[p] == OFE_PLAYER_BOT) continue;
        if (!player_alive(g, p) || idle) {
            clear_clock(g, ds, p);
            continue;
        }
        int32_t team = pl->team[p];
        if (team_game && (team < 0 || team >= num_teams)) continue;
        bool is_leader = team_game ? team == leader_team : p == leader;
        int64_t side_tiles = team_game ? team_tiles[team] : pl->num_tiles[p];
        if (!is_leader && side_tiles < required) punish(g, ds, p);
        else clear_clock(g, ds, p);
    }
}

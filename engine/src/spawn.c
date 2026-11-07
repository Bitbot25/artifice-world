/*
 * spawn.c - the spawn phase and spawn placement.
 *
 * A spawn claims every free land tile within euclidean distance 4 of the
 * chosen tile, measured from the tile's corner (OpenFront's getSpawnTiles
 * with center=true), which gives a 8x8-ish disc.
 *
 * Random spawns (tribes, random-spawn humans, nations whose spot is taken)
 * try random land tiles at least 30 tiles (manhattan) from other spawns,
 * relaxing that after 750 tries, and require the whole disc to be free land.
 *
 * The disc is walked breadth-first like getSpawnTiles, which fixes the order
 * the tiles join the border list. The parity build never falls back to a random
 * spot when a chosen tile is unusable (TS just does not spawn).
 */
#include "internal.h"

#define SPAWN_RADIUS 4
#define MIN_DISTANCE_BETWEEN_PLAYERS 30
#define MAX_SPAWN_TRIES 1000
#define RELAX_MIN_DIST_AT 750

static bool tile_is_free_land(const OfeGame *g, TileRef t) { return map_is_ownable(g->map, t) && !tile_has_owner(g, t); }

/* Disc tiles around center, in the breadth-first order OpenFront's
 * getSpawnTiles visits them (up, down, left, right neighbours): the order
 * decides the owner's border list. require_all: fail if any disc tile is not
 * free land. Returns the count (0 = failure). */
static int32_t spawn_disc(OfeGame *g, TileRef center, bool require_all, IntVec *out) {
    const OfeMap *m = g->map;
    IntVec *queue = &g->scratch_b;
    queue->len = 0;
    out->len = 0;
    double cx = map_x(m, center) - 0.5, cy = map_y(m, center) - 0.5;
    uint32_t gen = tiles_new_visit_gen(g);
    g->visit_stamp[center] = gen;
    IntVec_push(queue, center);
    for (int32_t head = 0; head < queue->len; head++) {
        TileRef t = queue->data[head];
        if (!tile_is_free_land(g, t)) {
            if (require_all) return 0;
        } else {
            IntVec_push(out, t);
        }
        TileRef nb[4];
        int n = map_neighbors4(m, t, nb);
        for (int i = 0; i < n; i++) {
            double dx = map_x(m, nb[i]) - cx, dy = map_y(m, nb[i]) - cy;
            if (g->visit_stamp[nb[i]] == gen || dx * dx + dy * dy > SPAWN_RADIUS * SPAWN_RADIUS) continue;
            g->visit_stamp[nb[i]] = gen;
            IntVec_push(queue, nb[i]);
        }
    }
    return out->len;
}

TileRef spawn_random_tile(OfeGame *g, PlayerId p, uint64_t *rng) {
    const OfeMap *m = g->map;
    IntVec disc = {0};
    TileRef found = -1;
    for (int tries = 1; tries <= MAX_SPAWN_TRIES && found < 0; tries++) {
        TileRef c = rng_int(rng, 0, m->num_tiles);
        if (!tile_is_free_land(g, c)) continue;
        if (tries <= RELAX_MIN_DIST_AT) {
            bool too_close = false;
            for (PlayerId o = 1; o < g->players.count && !too_close; o++) {
                TileRef st = g->players.spawn_tile[o];
                if (o != p && st >= 0 && map_manhattan(m, st, c) < MIN_DISTANCE_BETWEEN_PLAYERS) too_close = true;
            }
            if (too_close) continue;
        }
        if (spawn_disc(g, c, true, &disc) > 0) found = c;
    }
    IntVec_free(&disc);
    return found;
}

bool spawn_player(OfeGame *g, PlayerId p, TileRef center) {
    PlayerTable *pl = &g->players;
    IntVec disc = {0};

    /* Respawning during the spawn phase gives up the old spot first. */
    IntVec *old = &g->scratch_a;
    tiles_collect_player(g, p, old);
    IntVec old_copy = {0};
    IntVec_copy(&old_copy, old);
    for (int32_t i = 0; i < old_copy.len; i++) tile_set_owner(g, old_copy.data[i], 0);

    bool ok = false;
    if (center >= 0 && center < g->map->num_tiles) ok = spawn_disc(g, center, false, &disc) > 0;
#if OFE_TS_PARITY
    /* TS: a spawn on a chosen tile never falls back to a random spot. */
    if (!ok && center < 0) {
#else
    if (!ok) {
#endif
        center = spawn_random_tile(g, p, &g->rng);
        ok = center >= 0 && spawn_disc(g, center, true, &disc) > 0;
    }
    if (!ok) {
        /* Couldn't place: restore the old spot. */
        for (int32_t i = 0; i < old_copy.len; i++) tile_set_owner(g, old_copy.data[i], p);
        IntVec_free(&old_copy);
        IntVec_free(&disc);
        return false;
    }
    for (int32_t i = 0; i < disc.len; i++) tile_set_owner(g, disc.data[i], p);
    bool first = !pl->spawned[p];
    if (first) {
        /* Stagger enclave checks across players like OpenFront, which
         * offsets each player's 20-tick cycle by simpleHash(player id). */
        pl->cluster_calc_tick[p] = g->tick + (int32_t)(pl->id_hash[p] % 20);
    }
    pl->spawned[p] = 1;
    pl->dead[p] = 0;
    pl->spawn_tile[p] = center;
    event_emit(g, OFE_EV_PLAYER_SPAWNED, 0, p, 0, center, 0);
    if (first && pl->type[p] != OFE_PLAYER_HUMAN) ai_on_spawn(g, p);
    IntVec_free(&old_copy);
    IntVec_free(&disc);
    return true;
}

void spawn_phase_end(OfeGame *g) {
    if (g->start_tick >= 0) return;
    PlayerTable *pl = &g->players;
    for (PlayerId p = 1; p < pl->count; p++) {
        if (pl->spawned[p]) continue;
        bool auto_place = pl->type[p] != OFE_PLAYER_HUMAN || g->cfg.auto_spawn_humans || g->cfg.random_spawn;
        if (auto_place) spawn_player(g, p, -1);
    }
    g->start_tick = g->tick;
    event_emit(g, OFE_EV_SPAWN_PHASE_END, g->tick, 0, 0, 0, 0);
}

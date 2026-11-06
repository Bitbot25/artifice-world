/*
 * cluster.c - annexing surrounded territory (OpenFront's removeClusters).
 *
 * A player's border tiles fall into clusters (8-connected). A cluster with no
 * way out - no shore, no map edge, no clean unowned neighbour - and whose
 * enemy neighbours box it in on every side is an enclave. Its territory goes
 * to the neighbour attacking hardest (or the most common neighbour).
 *
 * The main body (the cluster with the biggest bounding box) is only annexed
 * when a single enemy surrounds it completely.
 *
 * Simplification: OpenFront's diagonal-pinch pass for nuke craters is not
 * modelled; pinched territory falls back to the same enclave rule.
 */
#include "internal.h"

typedef struct Box {
    int32_t min_x, min_y, max_x, max_y;
} Box;

static void box_add(Box *b, int32_t x, int32_t y) {
    if (x < b->min_x) b->min_x = x;
    if (y < b->min_y) b->min_y = y;
    if (x > b->max_x) b->max_x = x;
    if (y > b->max_y) b->max_y = y;
}

static bool box_covers(const Box *outer, const Box *inner) {
    return outer->min_x <= inner->min_x && outer->min_y <= inner->min_y && outer->max_x >= inner->max_x &&
           outer->max_y >= inner->max_y;
}

static int64_t box_area(const Box *b) { return (int64_t)(b->max_x - b->min_x + 1) * (b->max_y - b->min_y + 1); }

enum { CLASSIFY_OPEN = 0, CLASSIFY_SURROUNDED = 1 };

/* Is the cluster boxed in by enemies? single_enemy is set to the only enemy
 * neighbour if exactly one exists (main-body rule), else 0. */
static int classify(const OfeGame *g, PlayerId p, const int32_t *tiles, int32_t n, const Box *cbox, bool main_body,
                    PlayerId *single_enemy) {
    const OfeMap *m = g->map;
    Box enemy = {INT32_MAX, INT32_MAX, INT32_MIN, INT32_MIN};
    bool has_enemy = false;
    PlayerId only = 0;
    bool multi = false;
    for (int32_t i = 0; i < n; i++) {
        TileRef t = tiles[i];
        TileRef nb[4];
        int k = map_neighbors4(m, t, nb);
        /* Enclaves escape through any shore; the main body only through the
         * ocean (a lake inside the territory doesn't save it). */
        bool shore = false;
        if (main_body) {
            for (int j = 0; j < k; j++) shore |= map_is_ocean(m, nb[j]);
        } else {
            shore = map_is_shore(m, t);
        }
        if (shore || map_on_edge(m, t)) return CLASSIFY_OPEN;
        for (int j = 0; j < k; j++) {
            TileRef q = nb[j];
            PlayerId o = tile_owner(g, q);
            if (o == p) continue;
            if (o == 0) {
                if (main_body || !(map_is_land(m, q) && tile_has_fallout(g, q))) return CLASSIFY_OPEN;
                box_add(&enemy, map_x(m, q), map_y(m, q));
                continue;
            }
            has_enemy = true;
            if (only == 0) only = o;
            else if (only != o) multi = true;
            box_add(&enemy, map_x(m, q), map_y(m, q));
        }
    }
    *single_enemy = (has_enemy && !multi) ? only : 0;
    if (!has_enemy) return CLASSIFY_OPEN;
    if (main_body && multi) return CLASSIFY_OPEN;
    return box_covers(&enemy, cbox) ? CLASSIFY_SURROUNDED : CLASSIFY_OPEN;
}

/* 4-way flood of p's tiles from start, staying inside the fence. Fails (returns
 * false) if it reaches the map edge, leaves the fence, or touches water / clean
 * unowned land. On success the territory is in out. */
static bool flood_enclosed(OfeGame *g, PlayerId p, TileRef start, const Box *cbox, IntVec *out) {
    const OfeMap *m = g->map;
    out->len = 0;
    uint32_t gen = tiles_new_visit_gen(g);
    uint32_t *seen = g->visit_stamp;
    seen[start] = gen;
    IntVec_push(out, start);
    for (int32_t head = 0; head < out->len; head++) {
        TileRef t = out->data[head];
        int32_t x = map_x(m, t), y = map_y(m, t);
        if (x < cbox->min_x - 1 || x > cbox->max_x + 1 || y < cbox->min_y - 1 || y > cbox->max_y + 1) return false;
        if (map_on_edge(m, t)) return false;
        TileRef nb[4];
        int k = map_neighbors4(m, t, nb);
        for (int j = 0; j < k; j++) {
            TileRef q = nb[j];
            if (seen[q] == gen) continue;
            PlayerId o = tile_owner(g, q);
            if (o == p) {
                seen[q] = gen;
                IntVec_push(out, q);
            } else if (o == 0 && !(map_is_land(m, q) && tile_has_fallout(g, q))) {
                return false;
            }
        }
    }
    return true;
}

/* Who takes an enclave: the neighbour with the biggest attack on p, else
 * the most frequent non-friendly neighbour. */
static PlayerId capturing_player(OfeGame *g, PlayerId p, const IntVec *tiles) {
    const OfeMap *m = g->map;
    int32_t ncount = g->players.count;
    int32_t *counts = (int32_t *)ofe_xcalloc((size_t)ncount, sizeof(int32_t));
    for (int32_t i = 0; i < tiles->len; i++) {
        TileRef nb[4];
        int k = map_neighbors4(m, tiles->data[i], nb);
        for (int j = 0; j < k; j++) {
            PlayerId o = tile_owner(g, nb[j]);
            if (o != 0 && o != p && !player_friendly(g, o, p)) counts[o]++;
        }
    }
    PlayerId best_attacker = 0, best_mode = 0;
    double best_troops = 0;
    int32_t best_count = 0;
    for (PlayerId o = 1; o < ncount; o++) {
        if (!counts[o]) continue;
        double tr = attack_troops_against(g, o, p);
        if (tr > best_troops) {
            best_troops = tr;
            best_attacker = o;
        }
        if (counts[o] > best_count) {
            best_count = counts[o];
            best_mode = o;
        }
    }
    free(counts);
    return best_attacker ? best_attacker : best_mode;
}

static void annex(OfeGame *g, PlayerId p, PlayerId captor, const IntVec *tiles) {
    if (g->players.num_tiles[p] == tiles->len) player_conquered(g, captor, p);
    for (int32_t i = 0; i < tiles->len; i++) tile_set_owner(g, tiles->data[i], captor);
}

void cluster_check_player(OfeGame *g, PlayerId p) {
    const OfeMap *m = g->map;
    const IntVec *border = &g->players.border[p];
    if (border->len == 0) return;

    /* 1. Cluster the border tiles (8-connected). Clusters are stored back to
     *    back in scratch_a; starts[] marks where each begins. */
    IntVec *ctiles = &g->scratch_a;
    ctiles->len = 0;
    IntVec starts = {0};
    uint32_t gen = tiles_new_visit_gen(g);
    uint32_t *seen = g->visit_stamp;
    for (int32_t i = 0; i < border->len; i++) {
        TileRef s = border->data[i];
        if (seen[s] == gen) continue;
        IntVec_push(&starts, ctiles->len);
        seen[s] = gen;
        IntVec_push(ctiles, s);
        for (int32_t head = starts.data[starts.len - 1]; head < ctiles->len; head++) {
            TileRef nb[8];
            int k = map_neighbors8(m, ctiles->data[head], nb);
            for (int j = 0; j < k; j++) {
                TileRef q = nb[j];
                if (seen[q] != gen && tile_owner(g, q) == p && tile_is_border(g, q)) {
                    seen[q] = gen;
                    IntVec_push(ctiles, q);
                }
            }
        }
    }
    IntVec_push(&starts, ctiles->len);
    int32_t nclusters = starts.len - 1;

    /* 2. Boxes and the main body (largest box, then most tiles). */
    Box *boxes = (Box *)ofe_xmalloc((size_t)nclusters * sizeof(Box));
    int32_t main_idx = 0;
    for (int32_t c = 0; c < nclusters; c++) {
        Box b = {INT32_MAX, INT32_MAX, INT32_MIN, INT32_MIN};
        for (int32_t i = starts.data[c]; i < starts.data[c + 1]; i++) box_add(&b, map_x(m, ctiles->data[i]), map_y(m, ctiles->data[i]));
        boxes[c] = b;
        int64_t area = box_area(&b), best = box_area(&boxes[main_idx]);
        int32_t size = starts.data[c + 1] - starts.data[c];
        int32_t best_size = starts.data[main_idx + 1] - starts.data[main_idx];
        if (area > best || (area == best && size > best_size)) main_idx = c;
    }

    /* Classification reads the tiles before anything changes hands. */
    int32_t total = ctiles->len;
    int32_t *snapshot = (int32_t *)ofe_xmalloc((size_t)OFE_MAX(total, 1) * sizeof(int32_t));
    memcpy(snapshot, ctiles->data, (size_t)total * sizeof(int32_t));
    int32_t *cstart = (int32_t *)ofe_xmalloc((size_t)(nclusters + 1) * sizeof(int32_t));
    memcpy(cstart, starts.data, (size_t)(nclusters + 1) * sizeof(int32_t));
    IntVec_free(&starts);

    IntVec territory = {0};
    for (int32_t pass = 0; pass < 2; pass++) {
        /* pass 0: the main body; pass 1: the other clusters. */
        for (int32_t c = 0; c < nclusters; c++) {
            if ((pass == 0) != (c == main_idx)) continue;
            if (!player_alive(g, p)) break;
            const int32_t *tiles = snapshot + cstart[c];
            int32_t n = cstart[c + 1] - cstart[c];
            if (tile_owner(g, tiles[0]) != p) continue;
            PlayerId single = 0;
            int cls = classify(g, p, tiles, n, &boxes[c], c == main_idx, &single);
            if (cls != CLASSIFY_SURROUNDED) continue;
            if (c == main_idx && (single == 0 || player_friendly(g, single, p))) continue;
            if (!flood_enclosed(g, p, tiles[0], &boxes[c], &territory)) continue;
            PlayerId captor = c == main_idx ? single : capturing_player(g, p, &territory);
            if (captor == 0) continue;
            annex(g, p, captor, &territory);
        }
    }
    IntVec_free(&territory);
    free(snapshot);
    free(cstart);
    free(boxes);
}

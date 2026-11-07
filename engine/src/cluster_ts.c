/*
 * cluster_ts.c - OpenFront's removeClusters (PlayerExecution.ts), step for
 * step, for the TS-parity build. Compiled only with -DOFE_TS_PARITY=1; the
 * default build's cluster.c is a simpler approximation of the same rule.
 *
 * What matters for exactness, beyond the rule itself:
 *   - clusters come from the player's border set in insertion order, each as
 *     a DFS (stack, 8-connected, neighbours in TS order) mark order;
 *   - an annexed territory is flooded with the same 4-connected DFS and its
 *     tiles change hands in that order, which fixes the captor's border set
 *     order and so later attack behaviour;
 *   - the "main body" is the cluster touching the biggest 4-connected
 *     territory (box area, then border tile count, break ties);
 *   - the captor is the neighbour with the biggest attack on the player (first
 *     in neighbour order on ties), else the first most frequent neighbour;
 *   - "edge of the map" also counts tiles next to impassable terrain.
 */
#include "internal.h"

#if OFE_TS_PARITY

/* Scratch shared by all calls of one game (lazily allocated, kept zeroed). */
typedef struct ClusterScratch {
    int32_t *tid; /* per tile: id of its 4-connected territory (0 = none) */
    int32_t *cidx; /* per tile: cluster index + 1 (0 = none) */
} ClusterScratch;

typedef struct Clusters {
    IntVec tiles; /* all clusters back to back, DFS mark order */
    IntVec starts; /* n + 1 offsets into tiles */
    int32_t *box; /* 4 ints per cluster: minX minY maxX maxY */
    int32_t n;
} Clusters;

static const int32_t *cl_tiles(const Clusters *c, int32_t i) { return c->tiles.data + c->starts.data[i]; }
static int32_t cl_len(const Clusters *c, int32_t i) { return c->starts.data[i + 1] - c->starts.data[i]; }

/* TS neighbors8 order: west column (NW, W, SW), N, S, east column. */
static int neighbors8_ts(const OfeMap *m, TileRef t, TileRef out[8]) {
    int32_t w = m->width, x = t % w;
    bool has_n = t >= w, has_s = t < (m->height - 1) * w;
    int n = 0;
    if (x != 0) {
        if (has_n) out[n++] = t - 1 - w;
        out[n++] = t - 1;
        if (has_s) out[n++] = t - 1 + w;
    }
    if (has_n) out[n++] = t - w;
    if (has_s) out[n++] = t + w;
    if (x != w - 1) {
        if (has_n) out[n++] = t + 1 - w;
        out[n++] = t + 1;
        if (has_s) out[n++] = t + 1 + w;
    }
    return n;
}

/* GameMap.isOnEdgeOfMap: the map edge, or next to impassable terrain. */
static bool on_edge_ts(const OfeMap *m, TileRef t) {
    int32_t w = m->width, x = t % w;
    if (x == 0 || x == w - 1 || t < w || t >= (m->height - 1) * w) return true;
    return map_is_impassable(m, t - 1) || map_is_impassable(m, t + 1) || map_is_impassable(m, t - w) ||
           map_is_impassable(m, t + w);
}

static bool ocean_shore(const OfeMap *m, TileRef t) {
    if (!map_is_land(m, t)) return false;
    TileRef nb[4];
    int n = map_neighbors4(m, t, nb);
    for (int i = 0; i < n; i++) {
        if (map_is_ocean(m, nb[i])) return true;
    }
    return false;
}

static void box_init(int32_t *b) { b[0] = b[1] = 1000000000, b[2] = b[3] = -1000000000; }
static void box_add(const OfeMap *m, int32_t *b, TileRef t) {
    int32_t x = map_x(m, t), y = map_y(m, t);
    if (x < b[0]) b[0] = x;
    if (y < b[1]) b[1] = y;
    if (x > b[2]) b[2] = x;
    if (y > b[3]) b[3] = y;
}
static bool box_covers(const int32_t *outer, const int32_t *inner) {
    return outer[0] <= inner[0] && outer[1] <= inner[1] && outer[2] >= inner[2] && outer[3] >= inner[3];
}
static int64_t box_area(const int32_t *b) { return (int64_t)(b[2] - b[0] + 1) * (b[3] - b[1] + 1); }

/* ---- Clusters ------------------------------------------------------------- */

/* calculateClusters: DFS over the border set, 8-connected, border tiles only. */
static void calculate_clusters(OfeGame *g, PlayerId p, Clusters *c, IntVec *stack) {
    const OfeMap *m = g->map;
    const TileSeq *border = &g->players.border_seq[p];
    uint32_t border_gen = tiles_new_visit_gen(g);
    uint32_t cur_gen = tiles_new_visit_gen(g);
    uint32_t *vis = g->visit_stamp;
    for (int32_t i = 0; i < border->len; i++) {
        if (border->data[i] >= 0) vis[border->data[i]] = border_gen;
    }
    memset(c, 0, sizeof(*c));
    IntVec_push(&c->starts, 0);
    int32_t cap = 16;
    c->box = (int32_t *)ofe_xmalloc((size_t)cap * 4 * sizeof(int32_t));
    for (int32_t i = 0; i < border->len; i++) {
        TileRef start = border->data[i];
        if (start < 0 || vis[start] == cur_gen) continue;
        if (c->n == cap) {
            cap *= 2;
            c->box = (int32_t *)ofe_xrealloc(c->box, (size_t)cap * 4 * sizeof(int32_t));
        }
        int32_t *box = c->box + 4 * c->n;
        box_init(box);
        vis[start] = cur_gen;
        IntVec_push(&c->tiles, start);
        box_add(m, box, start);
        stack->len = 0;
        IntVec_push(stack, start);
        while (stack->len > 0) {
            TileRef t = stack->data[--stack->len];
            TileRef nb[8];
            int k = neighbors8_ts(m, t, nb);
            for (int j = 0; j < k; j++) {
                if (vis[nb[j]] != border_gen) continue; /* not a border tile, or already in a cluster */
                vis[nb[j]] = cur_gen;
                IntVec_push(&c->tiles, nb[j]);
                IntVec_push(stack, nb[j]);
                box_add(m, box, nb[j]);
            }
        }
        IntVec_push(&c->starts, c->tiles.len);
        c->n++;
    }
}

static void clusters_free(Clusters *c) {
    IntVec_free(&c->tiles);
    IntVec_free(&c->starts);
    free(c->box);
}

/* hasDiagonalPinch: a diagonal pair of own tiles with the other two of the 2x2 not own. */
static bool has_diagonal_pinch(const OfeGame *g, PlayerId p, const Clusters *c, int32_t ci) {
    const OfeMap *m = g->map;
    int32_t w = m->width;
    const int32_t *tiles = cl_tiles(c, ci);
    for (int32_t j = 0; j < cl_len(c, ci); j++) {
        TileRef t = tiles[j];
        if (tile_owner(g, t) != p || t >= (m->height - 1) * w) continue;
        TileRef below = t + w;
        if (tile_owner(g, below) == p) continue;
        int32_t x = t % w;
        if ((x < w - 1 && tile_owner(g, below + 1) == p && tile_owner(g, t + 1) != p) ||
            (x > 0 && tile_owner(g, below - 1) == p && tile_owner(g, t - 1) != p)) {
            return true;
        }
    }
    return false;
}

/* 4-connected DFS from `start` over tiles owned by p; marks via tid (when
 * assign_id > 0) and appends visited tiles in mark order. Returns the count. */
static int32_t flood4(OfeGame *g, PlayerId p, TileRef start, IntVec *out, IntVec *stack, int32_t *mark, int32_t id) {
    const OfeMap *m = g->map;
    int32_t first = out->len;
    mark[start] = id;
    IntVec_push(out, start);
    stack->len = 0;
    IntVec_push(stack, start);
    while (stack->len > 0) {
        TileRef t = stack->data[--stack->len];
        TileRef nb[4];
        int k = map_neighbors4(m, t, nb);
        for (int j = 0; j < k; j++) {
            if (mark[nb[j]] != 0 || tile_owner(g, nb[j]) != p) continue;
            mark[nb[j]] = id;
            IntVec_push(out, nb[j]);
            IntVec_push(stack, nb[j]);
        }
    }
    return out->len - first;
}

typedef struct Areas {
    IntVec tile_tid; /* per cluster tile (flattened like Clusters.tiles): territory id or 0 */
    IntVec area; /* territory areas indexed by id (area[0] unused) */
    int32_t *size; /* per cluster: biggest territory area it touches */
    int32_t max_area;
} Areas;

/* floodTerritoryAreas: the 4-connected territory each cluster tile sits on. */
static void flood_territory_areas(OfeGame *g, PlayerId p, const Clusters *c, ClusterScratch *sc, IntVec *stack,
                                  IntVec *touched, Areas *a) {
    memset(a, 0, sizeof(*a));
    a->size = (int32_t *)ofe_xcalloc((size_t)c->n, sizeof(int32_t));
    IntVec_push(&a->area, 0);
    for (int32_t i = 0; i < c->tiles.len; i++) IntVec_push(&a->tile_tid, 0);
    int32_t next_id = 1;
    for (int32_t i = 0; i < c->n; i++) {
        int32_t best = 0;
        for (int32_t j = c->starts.data[i]; j < c->starts.data[i + 1]; j++) {
            TileRef t = c->tiles.data[j];
            int32_t id = sc->tid[t];
            if (id == 0) {
                if (tile_owner(g, t) != p) continue;
                id = next_id++;
                int32_t size = flood4(g, p, t, touched, stack, sc->tid, id);
                IntVec_push(&a->area, size);
            }
            a->tile_tid.data[j] = id;
            if (a->area.data[id] > best) best = a->area.data[id];
        }
        a->size[i] = best;
        if (best > a->max_area) a->max_area = best;
    }
}

static int32_t largest_cluster(const Clusters *c, const Areas *a) {
    int32_t best = 0;
    for (int32_t i = 1; i < c->n; i++) {
        if (a->size[i] > a->size[best]) {
            best = i;
        } else if (a->size[i] == a->size[best]) {
            int64_t cur = box_area(c->box + 4 * i), top = box_area(c->box + 4 * best);
            if (cur > top || (cur == top && cl_len(c, i) > cl_len(c, best))) best = i;
        }
    }
    return best;
}

/* ---- Classification --------------------------------------------------------- */

/* surroundedBySamePlayer: the one enemy that walls in the whole cluster, or 0. */
static PlayerId surrounded_by_same_player(const OfeGame *g, PlayerId p, const Clusters *c, int32_t ci) {
    const OfeMap *m = g->map;
    PlayerId single = 0;
    int32_t eb[4];
    box_init(eb);
    const int32_t *tiles = cl_tiles(c, ci);
    for (int32_t j = 0; j < cl_len(c, ci); j++) {
        TileRef t = tiles[j];
        if (ocean_shore(m, t) || on_edge_ts(m, t)) return 0;
        TileRef nb[4];
        int k = map_neighbors4(m, t, nb);
        for (int i = 0; i < k; i++) {
            PlayerId o = tile_owner(g, nb[i]);
            if (o == p) continue;
            if (o == 0) return 0; /* unowned neighbour: an escape route */
            if (single == 0) single = o;
            else if (single != o) return 0;
            box_add(m, eb, nb[i]);
        }
    }
    if (single != 0 && box_covers(eb, c->box + 4 * ci)) return single;
    return 0;
}

/* classifyCluster: true if enemies (and fallout) box in a cluster with no shore. */
static bool cluster_surrounded(const OfeGame *g, PlayerId p, const Clusters *c, int32_t ci) {
    const OfeMap *m = g->map;
    bool has_enemy = false;
    int32_t eb[4];
    box_init(eb);
    const int32_t *tiles = cl_tiles(c, ci);
    for (int32_t j = 0; j < cl_len(c, ci); j++) {
        TileRef t = tiles[j];
        if (map_is_shore(m, t) || on_edge_ts(m, t)) return false;
        TileRef nb[4];
        int k = map_neighbors4(m, t, nb);
        for (int i = 0; i < k; i++) {
            PlayerId o = tile_owner(g, nb[i]);
            if (o != 0 && o != p) {
                has_enemy = true;
                box_add(m, eb, nb[i]);
            } else if (o == 0) {
                if (!tile_has_fallout(g, nb[i])) return false;
                box_add(m, eb, nb[i]);
            }
        }
    }
    return has_enemy && box_covers(eb, c->box + 4 * ci);
}

/* isEnclosed: walking only over own tiles from start never reaches water,
 * clean unowned land, the map edge or leaves the fence around the box. */
static bool is_enclosed(OfeGame *g, PlayerId p, TileRef start, const int32_t *box, IntVec *stack) {
    const OfeMap *m = g->map;
    uint32_t gen = tiles_new_visit_gen(g);
    uint32_t *vis = g->visit_stamp;
    stack->len = 0;
    vis[start] = gen;
    IntVec_push(stack, start);
    while (stack->len > 0) {
        TileRef t = stack->data[--stack->len];
        int32_t x = t % m->width, y = t / m->width;
        if (x < box[0] - 1 || x > box[2] + 1 || y < box[1] - 1 || y > box[3] + 1) return false;
        if (on_edge_ts(m, t)) return false;
        TileRef nb[4];
        int k = map_neighbors4(m, t, nb);
        for (int i = 0; i < k; i++) {
            if (vis[nb[i]] == gen) continue;
            PlayerId o = tile_owner(g, nb[i]);
            if (o == p) {
                vis[nb[i]] = gen;
                IntVec_push(stack, nb[i]);
            } else if (o == 0 && (!map_is_land(m, nb[i]) || !tile_has_fallout(g, nb[i]))) {
                return false;
            }
        }
    }
    return true;
}

/* getCapturingPlayer. Returns 0 if no non-friendly neighbour. */
static PlayerId capturing_player(const OfeGame *g, PlayerId p, const int32_t *tiles, int32_t n) {
    const OfeMap *m = g->map;
    IntVec order = {0}; /* neighbours in first-seen order (JS Map insertion order) */
    int32_t *count = (int32_t *)ofe_xcalloc((size_t)g->players.count, sizeof(int32_t));
    for (int32_t j = 0; j < n; j++) {
        TileRef nb[4];
        int k = map_neighbors4(m, tiles[j], nb);
        for (int i = 0; i < k; i++) {
            PlayerId o = tile_owner(g, nb[i]);
            if (o == 0 || o == p || player_friendly(g, o, p)) continue;
            if (count[o]++ == 0) IntVec_push(&order, o);
        }
    }
    PlayerId result = 0;
    double largest = 0;
    for (int32_t i = 0; i < order.len && order.len > 0; i++) {
        PlayerId nbp = order.data[i];
        /* The neighbour's first biggest attack on p (creation order = lowest id on ties). */
        double best = -1;
        int32_t best_id = 0;
        const AttackTable *at = &g->attacks;
        for (int32_t r = 0; r < at->count; r++) {
            if (!at->active[r] || at->attacker[r] != nbp || at->target[r] != p) continue;
            if (at->troops[r] > best || (at->troops[r] == best && at->id[r] < best_id)) best = at->troops[r], best_id = at->id[r];
        }
        if (best > largest) {
            largest = best;
            result = nbp;
        }
    }
    if (!result) {
        int32_t top = 0;
        for (int32_t i = 0; i < order.len; i++) {
            if (count[order.data[i]] > top) top = count[order.data[i]], result = order.data[i];
        }
    }
    IntVec_free(&order);
    free(count);
    return result;
}

/* ---- Removal ------------------------------------------------------------------ */

typedef struct Ctx {
    OfeGame *g;
    PlayerId p;
    ClusterScratch *sc;
    IntVec stack, flood, touched;
} Ctx;

/* Hands every tile of `tiles` to the captor, conquering the player first if it is all it has. */
static void annex(Ctx *x, PlayerId captor, const int32_t *tiles, int32_t n) {
    OfeGame *g = x->g;
    if (g->players.num_tiles[x->p] == n) player_conquered(g, captor, x->p);
    for (int32_t i = 0; i < n; i++) tile_set_owner(g, tiles[i], captor);
}

/* The 4-connected own territory from `start` in DFS order, using a throwaway mark. */
static int32_t territory_from(Ctx *x, TileRef start, IntVec *out) {
    OfeGame *g = x->g;
    out->len = 0;
    int32_t n = flood4(g, x->p, start, out, &x->stack, x->sc->tid, -1);
    for (int32_t i = 0; i < out->len; i++) x->sc->tid[out->data[i]] = 0;
    return n;
}

/* removeCluster. Returns true if something was annexed. */
static bool remove_cluster(Ctx *x, const Clusters *c, int32_t ci, PlayerId known_captor, bool pinched) {
    OfeGame *g = x->g;
    const int32_t *tiles = cl_tiles(c, ci);
    int32_t n = cl_len(c, ci);
    const int32_t *box = c->box + 4 * ci;
    for (int32_t i = 0; i < n; i++) {
        if (tile_owner(g, tiles[i]) != x->p) return false;
    }
    TileRef first = tiles[0];
    if (!pinched) {
        PlayerId captor = known_captor ? known_captor : capturing_player(g, x->p, tiles, n);
        if (!captor || !is_enclosed(g, x->p, first, box, &x->stack)) return false;
        territory_from(x, first, &x->flood);
        annex(x, captor, x->flood.data, x->flood.len);
        return true;
    }
    /* Pinched: every 4-connected component is judged on its own; cidx marks
     * the components already looked at (TS: a floodGen stamp). */
    bool any = false;
    IntVec comp = {0}, marked = {0};
    int32_t *seen = x->sc->cidx;
    for (int32_t i = 0; i < n; i++) {
        if (!player_alive(g, x->p)) break;
        TileRef t = tiles[i];
        if (seen[t] || tile_owner(g, t) != x->p) continue;
        bool enclosed = is_enclosed(g, x->p, t, box, &x->stack);
        territory_from(x, t, &comp);
        for (int32_t k = 0; k < comp.len; k++) {
            seen[comp.data[k]] = 1;
            IntVec_push(&marked, comp.data[k]);
        }
        if (!enclosed) continue;
        PlayerId captor = known_captor ? known_captor : capturing_player(g, x->p, comp.data, comp.len);
        if (!captor) continue;
        any = true;
        annex(x, captor, comp.data, comp.len);
    }
    for (int32_t k = 0; k < marked.len; k++) seen[marked.data[k]] = 0;
    IntVec_free(&comp);
    IntVec_free(&marked);
    return any;
}

/* ---- Pass 2: pinched clusters severed by fallout ------------------------------ */

typedef struct Holes {
    int32_t *ids; /* territory id per cluster, -1 unknown */
    int32_t next_id;
    bool ready;
} Holes;

/* 8-connected flood of own tiles from the start of cluster `ci`, tagging every
 * cluster it reaches with one territory id (checkAndAssignTerritory). */
static void assign_territory(Ctx *x, const Clusters *c, Holes *h, int32_t a, int32_t b) {
    OfeGame *g = x->g;
    if (h->ids[a] != -1 && h->ids[a] == h->ids[b]) return;
    if (h->ids[a] != -1 && h->ids[b] != -1) return;
    int32_t start_idx = h->ids[b] == -1 ? b : a;
    int32_t id = h->next_id++;
    TileRef start = cl_tiles(c, start_idx)[0];
    if (tile_owner(g, start) != x->p) return;
    uint32_t gen = tiles_new_visit_gen(g);
    uint32_t *vis = g->visit_stamp;
    vis[start] = gen;
    x->stack.len = 0;
    IntVec_push(&x->stack, start);
    h->ids[start_idx] = id;
    while (x->stack.len > 0) {
        TileRef t = x->stack.data[--x->stack.len];
        TileRef nb[8];
        int k = neighbors8_ts(g->map, t, nb);
        for (int j = 0; j < k; j++) {
            if (vis[nb[j]] == gen || tile_owner(g, nb[j]) != x->p) continue;
            vis[nb[j]] = gen;
            IntVec_push(&x->stack, nb[j]);
            if (x->sc->cidx[nb[j]] > 0) h->ids[x->sc->cidx[nb[j]] - 1] = id;
        }
    }
}

/* isHoleCluster: enclosed by another cluster of the same 8-connected territory. */
static bool is_hole_cluster(Ctx *x, const Clusters *c, Holes *h, int32_t i) {
    for (int32_t j = 0; j < c->n; j++) {
        if (j == i || !box_covers(c->box + 4 * j, c->box + 4 * i)) continue;
        if (!h->ready) {
            for (int32_t k = 0; k < c->n; k++) h->ids[k] = -1;
            /* clusterIndexMap: tile -> cluster index + 1 */
            for (int32_t cl = 0; cl < c->n; cl++) {
                for (int32_t k = 0; k < cl_len(c, cl); k++) x->sc->cidx[cl_tiles(c, cl)[k]] = cl + 1;
            }
            h->ready = true;
        }
        assign_territory(x, c, h, i, j);
        if (h->ids[i] != -1 && h->ids[i] == h->ids[j]) return true;
    }
    return false;
}

/* evaluatePinchCandidates: severed components (smaller than the main territory,
 * touching fallout, enclosed) go to their capturing neighbour. */
static void evaluate_pinch(Ctx *x, const Clusters *c, const Areas *areas, int32_t ci) {
    OfeGame *g = x->g;
    const OfeMap *m = g->map;
    if (!player_alive(g, x->p)) return;
    uint32_t pinch_gen = tiles_new_visit_gen(g);
    IntVec comp = {0};
    for (int32_t j = c->starts.data[ci]; j < c->starts.data[ci + 1]; j++) {
        if (!player_alive(g, x->p)) break;
        int32_t tid = areas->tile_tid.data[j];
        if (tid != 0 && areas->area.data[tid] >= areas->max_area) continue;
        TileRef start = c->tiles.data[j];
        uint32_t *vis = g->visit_stamp;
        if (tile_owner(g, start) != x->p || vis[start] >= pinch_gen) continue;
        bool fallout = false;
        int32_t box[4];
        box_init(box);
        comp.len = 0;
        vis[start] = pinch_gen;
        x->stack.len = 0;
        IntVec_push(&x->stack, start);
        IntVec_push(&comp, start);
        while (x->stack.len > 0) {
            TileRef t = x->stack.data[--x->stack.len];
            box_add(m, box, t);
            TileRef nb[4];
            int k = map_neighbors4(m, t, nb);
            for (int i = 0; i < k; i++) {
                if (!fallout && tile_has_fallout(g, nb[i])) fallout = true;
                if (vis[nb[i]] < pinch_gen && tile_owner(g, nb[i]) == x->p) {
                    vis[nb[i]] = pinch_gen;
                    IntVec_push(&comp, nb[i]);
                    IntVec_push(&x->stack, nb[i]);
                }
            }
        }
        if (comp.len >= areas->max_area || !fallout) continue;
        if (!is_enclosed(g, x->p, start, box, &x->stack)) continue;
        PlayerId captor = capturing_player(g, x->p, comp.data, comp.len);
        if (captor) annex(x, captor, comp.data, comp.len);
    }
    IntVec_free(&comp);
}

static void cluster_pinch_pass_ts(Ctx *x, const Clusters *c, const Areas *areas, const uint8_t *pinched,
                                  const uint8_t *removed) {
    Holes h = {(int32_t *)ofe_xcalloc((size_t)c->n, sizeof(int32_t)), 1, false};
    for (int32_t i = 0; i < c->n; i++) {
        if (!player_alive(x->g, x->p)) break;
        if (removed[i] || !pinched[i]) continue;
        if (is_hole_cluster(x, c, &h, i)) continue;
        evaluate_pinch(x, c, areas, i);
    }
    if (h.ready) {
        for (int32_t cl = 0; cl < c->n; cl++) {
            for (int32_t k = 0; k < cl_len(c, cl); k++) x->sc->cidx[cl_tiles(c, cl)[k]] = 0;
        }
    }
    free(h.ids);
}

void cluster_check_player_ts(OfeGame *g, PlayerId p) {
    if (g->players.border_seq[p].live == 0) return;
    if (!g->cluster_scratch) g->cluster_scratch = ofe_xcalloc(1, sizeof(ClusterScratch));
    ClusterScratch *sc = (ClusterScratch *)g->cluster_scratch;
    if (!sc->tid) {
        sc->tid = (int32_t *)ofe_xcalloc((size_t)g->map->num_tiles, sizeof(int32_t));
        sc->cidx = (int32_t *)ofe_xcalloc((size_t)g->map->num_tiles, sizeof(int32_t));
    }
    Ctx x = {g, p, sc, {0}, {0}, {0}};
    Clusters c;
    calculate_clusters(g, p, &c, &x.stack);
    if (c.n == 0) goto done;

    uint8_t *pinched = (uint8_t *)ofe_xcalloc((size_t)c.n, 1);
    uint8_t *removed = (uint8_t *)ofe_xcalloc((size_t)c.n, 1);
    for (int32_t i = 0; i < c.n; i++) pinched[i] = has_diagonal_pinch(g, p, &c, i);

    Areas areas;
    flood_territory_areas(g, p, &c, sc, &x.stack, &x.touched, &areas);
    for (int32_t i = 0; i < x.touched.len; i++) sc->tid[x.touched.data[i]] = 0;
    int32_t largest = largest_cluster(&c, &areas);

    /* Pass 0: the main body, if one enemy walls it in completely. */
    PlayerId by = surrounded_by_same_player(g, p, &c, largest);
    if (by && !player_friendly(g, by, p)) removed[largest] = remove_cluster(&x, &c, largest, by, pinched[largest]);
    /* Pass 1: whole secondary clusters. */
    for (int32_t i = 0; i < c.n; i++) {
        if (!player_alive(g, p)) break;
        if (i == largest) continue;
        if (cluster_surrounded(g, p, &c, i)) removed[i] = remove_cluster(&x, &c, i, 0, pinched[i]);
    }
    /* Pass 2: pinched clusters severed by fallout. */
    cluster_pinch_pass_ts(&x, &c, &areas, pinched, removed);

    free(areas.size);
    IntVec_free(&areas.tile_tid);
    IntVec_free(&areas.area);
    free(pinched);
    free(removed);
done:
    clusters_free(&c);
    IntVec_free(&x.stack);
    IntVec_free(&x.flood);
    IntVec_free(&x.touched);
}

void cluster_ts_free(OfeGame *g) {
    ClusterScratch *sc = (ClusterScratch *)g->cluster_scratch;
    if (!sc) return;
    free(sc->tid);
    free(sc->cidx);
    free(sc);
    g->cluster_scratch = NULL;
}

#endif /* OFE_TS_PARITY */

/*
 * pathfind.c - water bodies and ship routing.
 *
 * Built once per map and shared by all games on it (immutable, thread-safe):
 *   - comp:      4-connected water bodies of the full-resolution map
 *                (ocean vs lakes), for "can a boat get there at all".
 *   - mini_comp: the same on the half-resolution mini map, which is what
 *                routes are searched on (a mini tile is water only when all
 *                four of its tiles are), so a failed reachability test never
 *                triggers a pointless exhaustive search.
 *   - clear:     distance of each mini water tile from land, to keep routes
 *                off the coast.
 *
 * A route is found in four steps:
 *   1. "Access": a short breadth-first search at full resolution from the
 *      endpoint (a shore tile or a water tile near the coast) to the nearest
 *      tile that belongs to a mini water tile.
 *   2. Weighted A* over the mini map (8 moves, each costing one step, because
 *      a ship covers one tile per tick, diagonals included; no corner
 *      cutting; a small penalty next to land).
 *   3. String pulling: the mini route is replaced by the fewest straight
 *      segments that stay in water at least one tile off the coast.
 *   4. Expansion to one tile per step (Chebyshev lines), so route length in
 *      tiles is exactly the number of ticks the ship needs.
 *
 * Routes between the same two tiles are memoised per map as key points (a
 * route is a pure function of the terrain, so the memo only affects speed).
 * Search scratch buffers are pooled per map and borrowed under a lock.
 */
#include <pthread.h>

#include "pathfind.h"

#define ACCESS_RADIUS 16
#define ACCESS_MAX 40 /* longest access chain */
#define ASTAR_WEIGHT 1.5f
#define ASTAR_MAX_POPS 2000000
#define MEMO_SLOTS 16384
#define MEMO_MAX_LOAD (MEMO_SLOTS * 3 / 5)

/* ---- Data -------------------------------------------------------------- */

typedef struct Scratch {
    float *g;
    uint32_t *seen, *closed;
    uint8_t *dir;
    uint32_t gen;
    TileHeap heap;
    IntVec cells; /* A* result */
    IntVec poly, kept, pts; /* route building */
    struct Scratch *next;
} Scratch;

typedef struct MemoEntry {
    TileRef src, dst; /* src = -1: empty */
    int32_t len;
    int32_t *pts;
} MemoEntry;

struct WaterGraph {
    int32_t width, height, mw, mh;
    int32_t num_comps;
    uint16_t *comp; /* full resolution: 0 land, else body id + 1 */
    uint16_t *mini_comp; /* mini map: 0 land, else body id + 1 */
    uint8_t *clear; /* mini map: steps from land (water cells), 255 = far */
    const uint8_t *mini; /* the map's mini terrain */
    pthread_mutex_t lock;
    Scratch *free_scratch;
    MemoEntry *memo;
    int32_t memo_count;
};

/* Directions: N, S, W, E, then the diagonals. */
static const int DX[8] = {0, 0, -1, 1, -1, 1, -1, 1};
static const int DY[8] = {-1, 1, 0, 0, -1, -1, 1, 1};

static inline bool tile_land(const OfeMap *m, int x, int y) {
    return (m->terrain[y * m->width + x] & TERRAIN_LAND_BIT) != 0;
}
static inline bool mini_water(const WaterGraph *wg, int mx, int my) {
    return (wg->mini[my * wg->mw + mx] & TERRAIN_LAND_BIT) == 0;
}

/* ---- Build ------------------------------------------------------------- */

/* Labels 4-connected water (terrain bit 7 clear) with ids 1..65534; past
 * that, remaining bodies share the id 65535. Returns the number of ids. */
static int32_t label_water(const uint8_t *terr, int32_t w, int32_t h, uint16_t *out) {
    int32_t n = w * h, ids = 0;
    int32_t *queue = (int32_t *)ofe_xmalloc((size_t)n * sizeof(int32_t));
    for (int32_t start = 0; start < n; start++) {
        if ((terr[start] & TERRAIN_LAND_BIT) || out[start]) continue;
        uint16_t id = ids < 65534 ? (uint16_t)(++ids) : 65535;
        if (id == 65535 && ids == 65534) ids = 65535;
        int32_t qh = 0, qt = 0;
        queue[qt++] = start;
        out[start] = id;
        while (qh < qt) {
            int32_t c = queue[qh++], x = c % w;
            int32_t nb[4], k = 0;
            if (c >= w) nb[k++] = c - w;
            if (c < n - w) nb[k++] = c + w;
            if (x > 0) nb[k++] = c - 1;
            if (x < w - 1) nb[k++] = c + 1;
            for (int i = 0; i < k; i++) {
                if ((terr[nb[i]] & TERRAIN_LAND_BIT) || out[nb[i]]) continue;
                out[nb[i]] = id;
                queue[qt++] = nb[i];
            }
        }
    }
    free(queue);
    return ids;
}

static void compute_clearance(const OfeMap *m, uint8_t *clear) {
    int32_t mw = m->mini_width, mh = m->mini_height, n = mw * mh;
    int32_t *queue = (int32_t *)ofe_xmalloc((size_t)n * sizeof(int32_t));
    int32_t qh = 0, qt = 0;
    memset(clear, 255, (size_t)n);
    for (int32_t c = 0; c < n; c++) {
        if (m->mini_terrain[c] & TERRAIN_LAND_BIT) {
            clear[c] = 0;
            queue[qt++] = c;
        }
    }
    while (qh < qt) {
        int32_t c = queue[qh++], x = c % mw, y = c / mw;
        for (int d = 0; d < 4; d++) {
            int nx = x + DX[d], ny = y + DY[d];
            if (nx < 0 || ny < 0 || nx >= mw || ny >= mh) continue;
            int32_t nc = ny * mw + nx;
            if (clear[nc] != 255) continue;
            clear[nc] = (uint8_t)OFE_MIN(clear[c] + 1, 254);
            queue[qt++] = nc;
        }
    }
    free(queue);
}

static void *graph_build(const OfeMap *m) {
    WaterGraph *wg = (WaterGraph *)ofe_xcalloc(1, sizeof(WaterGraph));
    wg->width = m->width;
    wg->height = m->height;
    wg->mw = m->mini_width;
    wg->mh = m->mini_height;
    wg->mini = m->mini_terrain;
    wg->comp = (uint16_t *)ofe_xcalloc((size_t)m->num_tiles, sizeof(uint16_t));
    wg->num_comps = label_water(m->terrain, m->width, m->height, wg->comp);
    wg->mini_comp = (uint16_t *)ofe_xcalloc((size_t)wg->mw * wg->mh, sizeof(uint16_t));
    label_water(m->mini_terrain, wg->mw, wg->mh, wg->mini_comp);
    wg->clear = (uint8_t *)ofe_xmalloc((size_t)wg->mw * wg->mh);
    compute_clearance(m, wg->clear);
    pthread_mutex_init(&wg->lock, NULL);
    wg->memo = (MemoEntry *)ofe_xmalloc(MEMO_SLOTS * sizeof(MemoEntry));
    for (int i = 0; i < MEMO_SLOTS; i++) wg->memo[i].src = -1;
    return wg;
}

static void memo_clear(WaterGraph *wg) {
    for (int i = 0; i < MEMO_SLOTS; i++) {
        if (wg->memo[i].src >= 0) free(wg->memo[i].pts);
        wg->memo[i].src = -1;
        wg->memo[i].pts = NULL;
    }
    wg->memo_count = 0;
}

static void graph_free(void *p) {
    WaterGraph *wg = (WaterGraph *)p;
    memo_clear(wg);
    free(wg->memo);
    for (Scratch *s = wg->free_scratch; s;) {
        Scratch *next = s->next;
        free(s->g);
        free(s->seen);
        free(s->closed);
        free(s->dir);
        heap_free(&s->heap);
        IntVec_free(&s->cells);
        IntVec_free(&s->poly);
        IntVec_free(&s->kept);
        IntVec_free(&s->pts);
        free(s);
        s = next;
    }
    pthread_mutex_destroy(&wg->lock);
    free(wg->comp);
    free(wg->mini_comp);
    free(wg->clear);
    free(wg);
}

const WaterGraph *water_graph(OfeMap *map) {
    return (const WaterGraph *)map_cache_get(map, MAP_CACHE_SHIPS, graph_build, graph_free);
}

/* ---- Components -------------------------------------------------------- */

int32_t water_graph_num_components(const WaterGraph *wg) { return wg->num_comps; }

int water_tile_components(const WaterGraph *wg, const OfeMap *m, TileRef t, int32_t out[4]) {
    if (map_is_water(m, t)) {
        out[0] = (int32_t)wg->comp[t] - 1;
        return 1;
    }
    TileRef nb[4];
    int k = map_neighbors4(m, t, nb), n = 0;
    for (int i = 0; i < k; i++) {
        int32_t c = (int32_t)wg->comp[nb[i]] - 1;
        if (c < 0) continue;
        bool dup = false;
        for (int j = 0; j < n; j++) dup |= out[j] == c;
        if (!dup) out[n++] = c;
    }
    return n;
}

int32_t water_tile_component(const WaterGraph *wg, const OfeMap *m, TileRef t) {
    if (map_is_water(m, t)) return (int32_t)wg->comp[t] - 1;
    TileRef nb[4];
    int k = map_neighbors4(m, t, nb);
    for (int i = 0; i < k; i++) {
        if (wg->comp[nb[i]]) return (int32_t)wg->comp[nb[i]] - 1;
    }
    return -1;
}

/* ---- Scratch pool ------------------------------------------------------- */

static Scratch *scratch_acquire(WaterGraph *wg) {
    pthread_mutex_lock(&wg->lock);
    Scratch *s = wg->free_scratch;
    if (s) wg->free_scratch = s->next;
    pthread_mutex_unlock(&wg->lock);
    if (!s) {
        size_t n = (size_t)wg->mw * wg->mh;
        s = (Scratch *)ofe_xcalloc(1, sizeof(Scratch));
        s->g = (float *)ofe_xmalloc(n * sizeof(float));
        s->seen = (uint32_t *)ofe_xcalloc(n, sizeof(uint32_t));
        s->closed = (uint32_t *)ofe_xcalloc(n, sizeof(uint32_t));
        s->dir = (uint8_t *)ofe_xmalloc(n);
    }
    return s;
}

static void scratch_release(WaterGraph *wg, Scratch *s) {
    pthread_mutex_lock(&wg->lock);
    s->next = wg->free_scratch;
    wg->free_scratch = s;
    pthread_mutex_unlock(&wg->lock);
}

/* ---- Step 1: access chains ---------------------------------------------- */

/* Writes chain[0] = start, ..., chain[k-1] = the nearest tile (in water
 * steps) that lies in a mini water tile, and returns k; 0 if there is none
 * within reach. A water tile in open water is its own access. */
static int find_access(const WaterGraph *wg, const OfeMap *m, TileRef start, TileRef *chain) {
    const int w = m->width, h = m->height;
    const int sx = start % w, sy = start / w;
    const bool start_water = !tile_land(m, sx, sy);
    if (start_water && mini_water(wg, sx / 2, sy / 2)) {
        chain[0] = start;
        return 1;
    }
    enum { R = ACCESS_RADIUS, W = 2 * R + 1 };
    int16_t parent[W * W], queue[W * W];
    for (int i = 0; i < W * W; i++) parent[i] = -2;
    const int x0 = sx - R, y0 = sy - R, root = R * W + R;
    int qh = 0, qt = 0;
    parent[root] = -1;
    queue[qt++] = (int16_t)root;
    while (qh < qt) {
        int cur = queue[qh++];
        int cx = cur % W + x0, cy = cur / W + y0;
        for (int d = 0; d < 8; d++) {
            if (cur == root && !start_water && d >= 4) continue; /* leave land orthogonally */
            int nx = cx + DX[d], ny = cy + DY[d];
            int lx = nx - x0, ly = ny - y0;
            if (nx < 0 || ny < 0 || nx >= w || ny >= h || lx < 0 || ly < 0 || lx >= W || ly >= W) continue;
            int idx = ly * W + lx;
            if (parent[idx] != -2 || tile_land(m, nx, ny)) continue;
            if (d >= 4 && tile_land(m, cx + DX[d], cy) && tile_land(m, cx, cy + DY[d])) continue;
            parent[idx] = (int16_t)cur;
            if (mini_water(wg, nx / 2, ny / 2)) {
                int steps = 0;
                for (int i = idx; i != root; i = parent[i]) steps++;
                if (steps + 1 > ACCESS_MAX) return 0;
                int k = steps;
                for (int i = idx; i != -1; i = parent[i]) {
                    chain[k--] = (y0 + i / W) * w + (x0 + i % W);
                }
                return steps + 1;
            }
            queue[qt++] = (int16_t)idx;
        }
    }
    return 0;
}

/* ---- Step 2: A* on the mini map ------------------------------------------ */

static inline float coast_penalty(uint8_t clear) { return clear <= 1 ? 2.0f : (clear == 2 ? 0.5f : 0.0f); }

/* Cells of the cheapest route from any of src_cells to goal, in order. */
static bool astar(const WaterGraph *wg, Scratch *sc, const int32_t *src_cells, int ns, int32_t goal) {
    const int mw = wg->mw, mh = wg->mh;
    if (++sc->gen == 0) {
        size_t n = (size_t)mw * mh;
        memset(sc->seen, 0, n * sizeof(uint32_t));
        memset(sc->closed, 0, n * sizeof(uint32_t));
        sc->gen = 1;
    }
    const uint32_t gen = sc->gen;
    const int gx = goal % mw, gy = goal / mw;
    sc->heap.len = 0;
    sc->cells.len = 0;
#define H(cx, cy) (ASTAR_WEIGHT * (float)OFE_MAX(abs((cx) - gx), abs((cy) - gy)) + 0.01f * (float)(abs((cx) - gx) + abs((cy) - gy)))
    for (int i = 0; i < ns; i++) {
        int32_t c = src_cells[i];
        if (sc->seen[c] == gen) continue;
        sc->seen[c] = gen;
        sc->g[c] = 0;
        sc->dir[c] = 255;
        heap_push(&sc->heap, c, H(c % mw, c / mw));
    }
    int pops = 0;
    while (sc->heap.len > 0) {
        int32_t c = heap_pop(&sc->heap);
        if (sc->closed[c] == gen) continue;
        sc->closed[c] = gen;
        if (c == goal) {
            for (int32_t i = c;;) {
                IntVec_push(&sc->cells, i);
                uint8_t d = sc->dir[i];
                if (d == 255) break;
                i = (i % mw - DX[d]) + (i / mw - DY[d]) * mw;
            }
            for (int32_t a = 0, b = sc->cells.len - 1; a < b; a++, b--) {
                int32_t t = sc->cells.data[a];
                sc->cells.data[a] = sc->cells.data[b];
                sc->cells.data[b] = t;
            }
            return true;
        }
        if (++pops > ASTAR_MAX_POPS) return false;
        const int cx = c % mw, cy = c / mw;
        const float cg = sc->g[c];
        for (int d = 0; d < 8; d++) {
            int nx = cx + DX[d], ny = cy + DY[d];
            if (nx < 0 || ny < 0 || nx >= mw || ny >= mh || !mini_water(wg, nx, ny)) continue;
            int32_t n = ny * mw + nx;
            if (sc->closed[n] == gen) continue;
            if (d >= 4 && !(mini_water(wg, cx + DX[d], cy) && mini_water(wg, cx, cy + DY[d]))) continue;
            float ng = cg + 1.0f + coast_penalty(wg->clear[n]);
            if (sc->seen[n] != gen || ng < sc->g[n]) {
                sc->seen[n] = gen;
                sc->g[n] = ng;
                sc->dir[n] = (uint8_t)d;
                heap_push(&sc->heap, n, ng + H(nx, ny));
            }
        }
    }
#undef H
    return false;
}

/* ---- Steps 3 and 4: smoothing and expansion ---------------------------------- */

/* Floor division by a positive n of (v * 2 + n) / (2n): rounds v / n. */
static inline int round_div(int v, int n) {
    int num = 2 * v + n, den = 2 * n;
    int q = num / den;
    if ((num % den) < 0) q--;
    return q;
}

/* The straight line from a to b one tile per step, as in the expansion. */
static bool segment_clear(const OfeMap *m, TileRef a, TileRef b) {
    const int w = m->width;
    int ax = a % w, ay = a / w, bx = b % w, by = b / w;
    int dx = bx - ax, dy = by - ay, n = OFE_MAX(abs(dx), abs(dy));
    for (int k = 1; k <= n; k++) {
        TileRef t = (ay + round_div(dy * k, n)) * w + (ax + round_div(dx * k, n));
        uint8_t b8 = m->terrain[t];
        if (b8 & TERRAIN_LAND_BIT) return false;
        if (k < n && (b8 & TERRAIN_SHORELINE_BIT)) return false; /* stay off the coast */
    }
    return true;
}

/* Keeps the fewest waypoints of poly such that consecutive kept points see
 * each other. Neighbouring points are always accepted (they are adjacent
 * mini-water points), so the result stays valid even where the strict
 * coast-clearance test fails. */
static void smooth(const OfeMap *m, const IntVec *poly, IntVec *kept) {
    kept->len = 0;
    int n = poly->len, cur = 0;
    IntVec_push(kept, poly->data[0]);
    while (cur < n - 1) {
        int best = cur + 1;
        if (n - 1 > cur + 1) {
            if (segment_clear(m, poly->data[cur], poly->data[n - 1])) {
                best = n - 1;
            } else {
                int lo = cur + 2, hi = n - 2;
                while (lo <= hi) {
                    int mid = (lo + hi) / 2;
                    if (segment_clear(m, poly->data[cur], poly->data[mid])) {
                        best = mid;
                        lo = mid + 1;
                    } else {
                        hi = mid - 1;
                    }
                }
            }
        }
        cur = best;
        IntVec_push(kept, poly->data[cur]);
    }
}

/* One tile per step along straight lines between consecutive key points. */
static void expand(const OfeMap *m, const int32_t *pts, int32_t n, IntVec *out) {
    const int w = m->width;
    out->len = 0;
    if (n == 0) return;
    IntVec_push(out, pts[0]);
    for (int32_t i = 0; i + 1 < n; i++) {
        int ax = pts[i] % w, ay = pts[i] / w, bx = pts[i + 1] % w, by = pts[i + 1] / w;
        int dx = bx - ax, dy = by - ay, steps = OFE_MAX(abs(dx), abs(dy));
        for (int k = 1; k <= steps; k++) {
            IntVec_push(out, (ay + round_div(dy * k, steps)) * w + (ax + round_div(dx * k, steps)));
        }
    }
}

/* ---- Memo ------------------------------------------------------------------- */

static inline uint32_t memo_hash(TileRef a, TileRef b) {
    uint32_t h = (uint32_t)a * 0x9E3779B1u ^ ((uint32_t)b * 0x85EBCA6Bu + 0x7F4A7C15u);
    return (h ^ (h >> 15)) & (MEMO_SLOTS - 1);
}

static bool memo_get(WaterGraph *wg, TileRef src, TileRef dst, IntVec *pts) {
    bool found = false;
    pthread_mutex_lock(&wg->lock);
    for (uint32_t i = memo_hash(src, dst); wg->memo[i].src >= 0; i = (i + 1) & (MEMO_SLOTS - 1)) {
        if (wg->memo[i].src == src && wg->memo[i].dst == dst) {
            pts->len = 0;
            IntVec_reserve(pts, wg->memo[i].len);
            memcpy(pts->data, wg->memo[i].pts, (size_t)wg->memo[i].len * sizeof(int32_t));
            pts->len = wg->memo[i].len;
            found = true;
            break;
        }
    }
    pthread_mutex_unlock(&wg->lock);
    return found;
}

static void memo_put(WaterGraph *wg, TileRef src, TileRef dst, const IntVec *pts) {
    pthread_mutex_lock(&wg->lock);
    if (wg->memo_count >= MEMO_MAX_LOAD) memo_clear(wg);
    uint32_t i = memo_hash(src, dst);
    while (wg->memo[i].src >= 0) {
        if (wg->memo[i].src == src && wg->memo[i].dst == dst) goto done; /* another thread got there first */
        i = (i + 1) & (MEMO_SLOTS - 1);
    }
    wg->memo[i].src = src;
    wg->memo[i].dst = dst;
    wg->memo[i].len = pts->len;
    wg->memo[i].pts = (int32_t *)ofe_xmalloc((size_t)pts->len * sizeof(int32_t));
    memcpy(wg->memo[i].pts, pts->data, (size_t)pts->len * sizeof(int32_t));
    wg->memo_count++;
done:
    pthread_mutex_unlock(&wg->lock);
}

/* ---- Route ------------------------------------------------------------------- */

static inline TileRef mini_corner(const OfeMap *m, int32_t cell) {
    return (cell / m->mini_width) * 2 * m->width + (cell % m->mini_width) * 2;
}

/* Key points for the best route from srcs to dst, in sc->pts. Returns the
 * chosen source index or -1. */
static int32_t route_points(WaterGraph *wg, const OfeMap *m, Scratch *sc, const TileRef *srcs, int nsrc, TileRef dst) {
    TileRef dchain[ACCESS_MAX];
    int nd = find_access(wg, m, dst, dchain);
    if (nd == 0) return -1;
    const int32_t goal_cell = (dchain[nd - 1] / m->width / 2) * wg->mw + (dchain[nd - 1] % m->width) / 2;

    TileRef schain[WATER_MAX_SOURCES][ACCESS_MAX];
    int slen[WATER_MAX_SOURCES];
    int32_t scell[WATER_MAX_SOURCES];
    int usable = 0;
    for (int i = 0; i < nsrc; i++) {
        slen[i] = find_access(wg, m, srcs[i], schain[i]);
        if (slen[i] == 0) continue;
        TileRef a = schain[i][slen[i] - 1];
        scell[i] = (a / m->width / 2) * wg->mw + (a % m->width) / 2;
        if (wg->mini_comp[scell[i]] != wg->mini_comp[goal_cell]) {
            slen[i] = 0;
            continue;
        }
        usable++;
    }
    if (usable == 0) return -1;

    int32_t cells[WATER_MAX_SOURCES];
    int ncells = 0;
    for (int i = 0; i < nsrc; i++) {
        if (slen[i]) cells[ncells++] = scell[i];
    }
    if (!astar(wg, sc, cells, ncells, goal_cell)) return -1;
    int win = -1;
    for (int i = 0; i < nsrc; i++) {
        if (slen[i] && scell[i] == sc->cells.data[0]) {
            win = i;
            break;
        }
    }
    if (win < 0) return -1;

    /* Polyline over the mini route: access tile, cell corners, access tile. */
    IntVec *poly = &sc->poly;
    poly->len = 0;
    IntVec_push(poly, schain[win][slen[win] - 1]);
    for (int32_t i = 1; i + 1 < sc->cells.len; i++) IntVec_push(poly, mini_corner(m, sc->cells.data[i]));
    if (sc->cells.len > 1 || dchain[nd - 1] != poly->data[0]) IntVec_push(poly, dchain[nd - 1]);
    smooth(m, poly, &sc->kept);

    IntVec *pts = &sc->pts;
    pts->len = 0;
    for (int i = 0; i < slen[win]; i++) IntVec_push(pts, schain[win][i]);
    for (int32_t i = 1; i < sc->kept.len; i++) IntVec_push(pts, sc->kept.data[i]);
    for (int i = nd - 2; i >= 0; i--) IntVec_push(pts, dchain[i]);
    return win;
}

int32_t water_find_path(const WaterGraph *wg_const, const OfeMap *m, const TileRef *srcs, int nsrc, TileRef dst, IntVec *out) {
    WaterGraph *wg = (WaterGraph *)wg_const; /* mutable parts (memo, scratch pool) are lock-protected */
    out->len = 0;
    if (nsrc <= 0 || nsrc > WATER_MAX_SOURCES) return -1;
    if (nsrc == 1 && srcs[0] == dst) {
        IntVec_push(out, dst);
        return 0;
    }
    Scratch *sc = scratch_acquire(wg);
    int32_t win;
    if (nsrc == 1 && memo_get(wg, srcs[0], dst, &sc->pts)) {
        win = 0;
    } else {
        win = route_points(wg, m, sc, srcs, nsrc, dst);
        if (win >= 0 && nsrc == 1) memo_put(wg, srcs[0], dst, &sc->pts);
    }
    if (win >= 0) expand(m, sc->pts.data, sc->pts.len, out);
    scratch_release(wg, sc);
    return win;
}

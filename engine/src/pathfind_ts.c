/*
 * pathfind_ts.c - OpenFront's water pathfinder, reproduced tile for tile.
 *
 * The one ship router, in both builds: a ship's tile on every tick is the one
 * OpenFront's TypeScript engine gives, at 0.1-0.3 ms per cold query. This
 * file follows
 * packages/engine/src/pathfinding/ closely (names in brackets are the TS
 * classes), and exactness comes before speed:
 *
 *   WaterPathFinder.next / PathFinderStepper   ts_water_next            [stepper]
 *   MiniMapTransformer     full-map tiles -> half-resolution "mini map"
 *                          tiles, search there, upscale the route back,
 *                          fix its ends                                  [mini_find]
 *   ShoreCoercingTransformer  a shore endpoint becomes its best water
 *                          neighbour, the shore tile is added back       [shore_find]
 *   SmoothingWaterTransformer line-of-sight smoothing (magnitude >= 2),
 *                          local A* on the first and last 50 tiles,
 *                          smoothing again (magnitude >= 3)              [smooth]
 *   ComponentCheckTransformer  fail fast across water bodies             [comp check]
 *   AStarWaterHierarchical HPA*: 32x32 clusters, gateway nodes on the
 *                          cluster borders, abstract A* between nodes,
 *                          bounded tile A* inside clusters               [hpa_*]
 *   AbstractGraphBuilder / ConnectedComponents                            [graph_build]
 *   AStarWaterBounded / AStarWater / AbstractGraphAStar / MinHeap         [bounded_search, abs_search, MinHeap]
 *
 * Everything runs on the mini map (map4x.bin for normal games, map16x.bin
 * for compact ones), as in TS: the full map only supplies coordinates. The
 * A* searches use OpenFront's exact binary heap with float32 priorities, its
 * neighbour order (N, S, W, E), integer costs and cross-product tie breaker,
 * because ties decide which of several equally cheap routes a ship takes.
 *
 * What may differ from TS without changing output:
 *   - The per-edge path cache TS keeps on the graph (it caches pure results).
 *   - Search scratch memory: stamp arrays here are per borrowed context.
 *   - The graph builder's BFS queue order (only distances are used).
 *
 * Immutable after construction except for a pool of search contexts, which
 * is borrowed under a lock, and the route memo; any number of games and
 * threads may query. Water does not change during a game (no water nukes),
 * so the graph is never rebuilt.
 *
 * Route memo: findPath(from, to) with one source is a pure function of the
 * map, and trade ships repeat the same port-to-port routes all game, so
 * single-source results are cached per map in a fixed-size, sharded,
 * direct-mapped table (see MEMO_*). A hit costs one copy of the route; the
 * entries are never shared with callers, so a game cannot see another
 * game's state through it and results do not depend on thread timing.
 *
 * Water bodies (ts_water_component) are OpenFront's WaterManager
 * .getWaterComponent: connected components of the mini map, looked up from a
 * full-map tile (the same answer TS gives, including its 1- and 2-hop
 * search for shore tiles whose mini tile is land).
 */
#include "pathfind_ts.h"

#include <pthread.h>

#include "subsystems.h"

#define CLUSTER_SIZE 32 /* AbstractGraphBuilder.CLUSTER_SIZE */
#define COST_SCALE 100
#define BASE_COST COST_SCALE
#define MAGNITUDE_MASK 0x1f
#define LAND_BIT 0x80
#define MIN_HPA_NODES 100 /* below this TS uses plain A* on the mini map */

/* ---- Mini map ----------------------------------------------------------- */

typedef struct Mini {
    int32_t w, h;
    const uint8_t *terr;
} Mini;

static inline bool mini_water(const Mini *m, int32_t t) { return !(m->terr[t] & LAND_BIT); }
static inline int32_t iabs(int32_t v) { return v < 0 ? -v : v; }
static inline int32_t mini_manhattan(const Mini *m, int32_t a, int32_t b) {
    return iabs(a % m->w - b % m->w) + iabs(a / m->w - b / m->w);
}

/* ---- MinHeap (PriorityQueue.ts): binary heap, float32 priorities --------- */

typedef struct MinHeap {
    int32_t *node;
    float *pri;
    int32_t size, cap;
} MinHeap;

static void heap_clear(MinHeap *h) { h->size = 0; }
static void heap_destroy(MinHeap *h) {
    free(h->node);
    free(h->pri);
    memset(h, 0, sizeof(*h));
}

static void mh_push(MinHeap *h, int32_t node, double priority) {
    if (h->size >= h->cap) {
        h->cap = h->cap ? h->cap * 2 : 256;
        h->node = (int32_t *)ofe_xrealloc(h->node, (size_t)h->cap * sizeof(int32_t));
        h->pri = (float *)ofe_xrealloc(h->pri, (size_t)h->cap * sizeof(float));
    }
    int32_t i = h->size++;
    h->node[i] = node;
    h->pri[i] = (float)priority; /* Float32Array store */
    while (i > 0) {
        int32_t parent = (i - 1) >> 1;
        if (h->pri[parent] <= h->pri[i]) break;
        int32_t tn = h->node[parent];
        float tp = h->pri[parent];
        h->node[parent] = h->node[i];
        h->pri[parent] = h->pri[i];
        h->node[i] = tn;
        h->pri[i] = tp;
        i = parent;
    }
}

static int32_t mh_pop(MinHeap *h) {
    int32_t result = h->node[0];
    h->size--;
    if (h->size > 0) {
        h->node[0] = h->node[h->size];
        h->pri[0] = h->pri[h->size];
        int32_t i = 0;
        for (;;) {
            int32_t left = (i << 1) + 1, right = left + 1, smallest = i;
            if (left < h->size && h->pri[left] < h->pri[smallest]) smallest = left;
            if (right < h->size && h->pri[right] < h->pri[smallest]) smallest = right;
            if (smallest == i) break;
            int32_t tn = h->node[smallest];
            float tp = h->pri[smallest];
            h->node[smallest] = h->node[i];
            h->pri[smallest] = h->pri[i];
            h->node[i] = tn;
            h->pri[i] = tp;
            i = smallest;
        }
    }
    return result;
}

/* ---- AStarWaterBounded / AStarWater --------------------------------------- */

typedef struct Bounded {
    int32_t cap; /* maxSearchArea: larger windows are refused */
    uint32_t stamp;
    uint32_t *closed, *gstamp, *g;
    int32_t *came;
    MinHeap heap;
    double weight; /* heuristicWeight: 3 (bounded) or 5 (AStarWater) */
    int32_t max_iter; /* 100000 or 1000000 */
    int32_t close_penalty; /* cost of magnitude < 3 water: 3 or 10 base steps */
} Bounded;

static void bounded_init(Bounded *b, int32_t cap, double weight, int32_t max_iter, int32_t close_penalty) {
    memset(b, 0, sizeof(*b));
    b->cap = cap;
    b->closed = (uint32_t *)ofe_xcalloc((size_t)cap, sizeof(uint32_t));
    b->gstamp = (uint32_t *)ofe_xcalloc((size_t)cap, sizeof(uint32_t));
    b->g = (uint32_t *)ofe_xmalloc((size_t)cap * sizeof(uint32_t));
    b->came = (int32_t *)ofe_xmalloc((size_t)cap * sizeof(int32_t));
    b->stamp = 1;
    b->weight = weight;
    b->max_iter = max_iter;
    b->close_penalty = close_penalty;
}

static void bounded_destroy(Bounded *b) {
    free(b->closed);
    free(b->gstamp);
    free(b->g);
    free(b->came);
    heap_destroy(&b->heap);
}

/* Prefer water 3-10 tiles from shore. */
static inline int32_t magnitude_penalty(const Bounded *b, int32_t magnitude) {
    if (magnitude < 3) return b->close_penalty * COST_SCALE;
    if (magnitude <= 10) return 0;
    return 1 * COST_SCALE;
}

typedef struct Bounds {
    int32_t min_x, max_x, min_y, max_y;
} Bounds;

/* AStarWaterBounded.searchBounded. Tiles are mini map tiles; the window
 * bounds the search (the goal may be land: it is always enterable). Fills
 * out with start..goal and returns true, or returns false (no route, window
 * too big, or the iteration limit). */
static bool bounded_search(Bounded *b, const Mini *mn, const int32_t *starts, int ns, int32_t goal, Bounds bd,
                           IntVec *out) {
    b->stamp++;
    const uint32_t stamp = b->stamp;
    const int32_t W = mn->w;
    const int32_t bw = bd.max_x - bd.min_x + 1, bh = bd.max_y - bd.min_y + 1;
    const int32_t nlocal = bw * bh;
    const int32_t goal_x = goal % W, goal_y = goal / W;
    if (nlocal > b->cap) return false;

#define TO_LOCAL(tile, clampit, outv)                                                   \
    do {                                                                                \
        int32_t tx_ = (tile) % W, ty_ = (tile) / W;                                     \
        if (clampit) {                                                                  \
            tx_ = OFE_MAX(bd.min_x, OFE_MIN(bd.max_x, tx_));                            \
            ty_ = OFE_MAX(bd.min_y, OFE_MIN(bd.max_y, ty_));                            \
        }                                                                               \
        (outv) = (ty_ - bd.min_y) * bw + (tx_ - bd.min_x);                              \
    } while (0)

    int32_t goal_local;
    TO_LOCAL(goal, 1, goal_local);
    if (goal_local < 0 || goal_local >= nlocal) return false;

    heap_clear(&b->heap);

    /* Cross-product tie breaker (prefers the straight start-goal line). */
    const int32_t s0x = starts[0] % W, s0y = starts[0] / W;
    const int32_t dx_goal = goal_x - s0x, dy_goal = goal_y - s0y;
    const double cross_norm = (double)OFE_MAX(1, iabs(dx_goal) + iabs(dy_goal));

    for (int i = 0; i < ns; i++) {
        int32_t s = starts[i], sl;
        TO_LOCAL(s, 1, sl);
        if (sl < 0 || sl >= nlocal) continue;
        b->g[sl] = 0;
        b->gstamp[sl] = stamp;
        b->came[sl] = -1;
        double h = b->weight * BASE_COST * (double)(iabs(s % W - goal_x) + iabs(s / W - goal_y));
        mh_push(&b->heap, sl, h);
    }

    static const int32_t DX4[4] = {0, 0, -1, 1}, DY4[4] = {-1, 1, 0, 0}; /* N, S, W, E */
    int32_t iterations = b->max_iter;
    while (b->heap.size > 0) {
        if (--iterations <= 0) return false;
        int32_t cur = mh_pop(&b->heap);
        if (b->closed[cur] == stamp) continue;
        b->closed[cur] = stamp;

        if (cur == goal_local) {
            IntVec_reserve(out, 16);
            out->len = 0;
            int32_t c = cur, guard = 0;
            while (c != -1 && guard < nlocal) {
                IntVec_push(out, (bd.min_y + c / bw) * W + (bd.min_x + c % bw));
                c = b->came[c];
                guard++;
            }
            for (int32_t i = 0, j = out->len - 1; i < j; i++, j--) {
                int32_t t = out->data[i];
                out->data[i] = out->data[j];
                out->data[j] = t;
            }
            return true;
        }

        const uint32_t cur_g = b->g[cur];
        const int32_t cx = bd.min_x + cur % bw, cy = bd.min_y + cur / bw;
        const int32_t current = cy * W + cx;
        for (int d = 0; d < 4; d++) {
            if (d == 0 && !(cy > bd.min_y)) continue;
            if (d == 1 && !(cy < bd.max_y)) continue;
            if (d == 2 && !(cx > bd.min_x)) continue;
            if (d == 3 && !(cx < bd.max_x)) continue;
            const int32_t nx = cx + DX4[d], ny = cy + DY4[d];
            const int32_t neighbor = current + DY4[d] * W + DX4[d];
            const int32_t nl = cur + DY4[d] * bw + DX4[d];
            const uint8_t nt = mn->terr[neighbor];
            if (b->closed[nl] == stamp || !(neighbor == goal || (nt & LAND_BIT) == 0)) continue;
            uint32_t tentative = cur_g + (uint32_t)(BASE_COST + magnitude_penalty(b, nt & MAGNITUDE_MASK));
            if (b->gstamp[nl] != stamp || tentative < b->g[nl]) {
                b->came[nl] = cur;
                b->g[nl] = tentative;
                b->gstamp[nl] = stamp;
                double h = b->weight * BASE_COST * (double)(iabs(nx - goal_x) + iabs(ny - goal_y));
                double cross = (double)iabs(dx_goal * (ny - goal_y) - dy_goal * (nx - goal_x));
                double tie = floor(cross * (COST_SCALE - 1) / cross_norm / cross_norm);
                mh_push(&b->heap, nl, (double)tentative + h + tie);
            }
        }
    }
    return false;
#undef TO_LOCAL
}

/* ---- Abstract graph (AbstractGraph.ts) ------------------------------------ */

typedef struct Node {
    int32_t x, y, tile, comp;
} Node;

typedef struct Edge {
    int32_t a, b; /* a < b */
    int32_t cost;
    int32_t cx, cy; /* cluster whose search found the cost */
} Edge;

typedef struct Graph {
    int32_t cxn, cyn; /* clusters across and down */
    int32_t nnodes, nedges;
    Node *nodes;
    Edge *edges; /* sorted by (a, b); the index is the edge id */
    int32_t *adj_off, *adj; /* per node, its edge ids in increasing order */
    int32_t *cl_off, *cl_nodes; /* per cluster, its node ids in discovery order */
    uint16_t *comp; /* per mini tile: water body, 0xFFFF for land (ConnectedComponents) */
} Graph;

/* ConnectedComponents.initialize: bodies numbered in order of their first
 * tile; past 65534 bodies the rest stay 0. */
static void label_components(const Mini *mn, uint16_t *comp) {
    const int32_t n = mn->w * mn->h, W = mn->w;
    int32_t *queue = (int32_t *)ofe_xmalloc((size_t)n * sizeof(int32_t));
    for (int32_t i = 0; i < n; i++) comp[i] = mini_water(mn, i) ? 0 : 0xFFFF;
    int32_t next_id = 0;
    for (int32_t start = 0; start < n; start++) {
        if (comp[start] != 0) continue;
        next_id++;
        if (next_id == 0xFFFF) break;
        int32_t head = 0, tail = 0;
        queue[tail++] = start;
        comp[start] = (uint16_t)next_id;
        while (head < tail) {
            int32_t c = queue[head++], x = c % W;
            int32_t nb[4], k = 0;
            if (c >= W) nb[k++] = c - W;
            if (c < n - W) nb[k++] = c + W;
            if (x > 0) nb[k++] = c - 1;
            if (x < W - 1) nb[k++] = c + 1;
            for (int i = 0; i < k; i++) {
                if (comp[nb[i]] != 0) continue;
                comp[nb[i]] = (uint16_t)next_id;
                queue[tail++] = nb[i];
            }
        }
    }
    free(queue);
}

typedef struct BuildEdge {
    int32_t a, b, cost, cx, cy;
    int32_t seq; /* creation order, to keep qsort stable */
} BuildEdge;
OFE_VEC(BuildEdgeVec, BuildEdge)

static int build_edge_cmp(const void *pa, const void *pb) {
    const BuildEdge *a = (const BuildEdge *)pa, *b = (const BuildEdge *)pb;
    if (a->a != b->a) return a->a < b->a ? -1 : 1;
    if (a->b != b->b) return a->b < b->b ? -1 : 1;
    return a->seq < b->seq ? -1 : (a->seq > b->seq);
}

typedef struct Builder {
    const Mini *mn;
    Graph *g;
    IntVec *cluster_nodes; /* per cluster, while building */
    int32_t *tile_node; /* tile -> node id or -1 */
    /* BFS scratch (BFSGrid): stamp-based visited, queue, distance */
    uint32_t stamp;
    uint32_t *vis;
    int32_t *queue;
    uint16_t *dist;
    uint32_t *tmark; /* tile is a target of the current search */
    int32_t *tidx;
    int32_t nodes_cap;
} Builder;

static int32_t builder_node(Builder *b, int32_t x, int32_t y) {
    Graph *g = b->g;
    int32_t tile = y * b->mn->w + x;
    if (b->tile_node[tile] >= 0) return b->tile_node[tile];
    if (g->nnodes == b->nodes_cap) {
        b->nodes_cap = b->nodes_cap ? b->nodes_cap * 2 : 256;
        g->nodes = (Node *)ofe_xrealloc(g->nodes, (size_t)b->nodes_cap * sizeof(Node));
    }
    int32_t id = g->nnodes++;
    g->nodes[id] = (Node){x, y, tile, g->comp[tile]};
    b->tile_node[tile] = id;
    return id;
}

static void builder_add_to_cluster(Builder *b, int32_t cx, int32_t cy, int32_t node) {
    IntVec *v = &b->cluster_nodes[cy * b->g->cxn + cx];
    for (int32_t i = 0; i < v->len; i++) {
        if (v->data[i] == node) return;
    }
    IntVec_push(v, node);
}

/* Gateways on the border between cluster (cx, cy) and its right (vertical
 * border) or lower (horizontal) neighbour: the middle of every run of tiles
 * that are water on both sides of the border. */
static void builder_border(Builder *b, int32_t cx, int32_t cy, bool vertical) {
    const Mini *mn = b->mn;
    const int32_t W = mn->w, H = mn->h;
    const int32_t base_x = cx * CLUSTER_SIZE, base_y = cy * CLUSTER_SIZE;
    int32_t fixed, from, limit;
    if (vertical) {
        fixed = OFE_MIN(base_x + CLUSTER_SIZE - 1, W - 1);
        from = base_y;
        limit = OFE_MIN(base_y + CLUSTER_SIZE, H);
    } else {
        fixed = OFE_MIN(base_y + CLUSTER_SIZE - 1, H - 1);
        from = base_x;
        limit = OFE_MIN(base_x + CLUSTER_SIZE, W);
    }
    int32_t span = -1;
    for (int32_t v = from; v <= limit; v++) {
        bool entrance = false;
        if (v < limit) {
            int32_t x = vertical ? fixed : v, y = vertical ? v : fixed;
            int32_t nx = vertical ? x + 1 : x, ny = vertical ? y : y + 1;
            entrance = mini_water(mn, y * W + x) && nx < W && ny < H && mini_water(mn, ny * W + nx);
        }
        if (entrance) {
            if (span == -1) span = v;
        } else if (span != -1) {
            int32_t mid = span + (v - span) / 2;
            span = -1;
            int32_t node = vertical ? builder_node(b, fixed, mid) : builder_node(b, mid, fixed);
            builder_add_to_cluster(b, cx, cy, node);
            if (vertical) builder_add_to_cluster(b, cx + 1, cy, node);
            else builder_add_to_cluster(b, cx, cy + 1, node);
        }
    }
}

/* findAllReachableNodesInBounds: BFS from a node over water within the
 * cluster window (a target outside it still counts); cost[k] = BFS distance
 * to target k or -1. */
static void builder_reach(Builder *b, int32_t from, const int32_t *targets, int nt, Bounds bd, int32_t *cost) {
    const Mini *mn = b->mn;
    const int32_t W = mn->w, H = mn->h, last_row = (H - 1) * W;
    const Node *nodes = b->g->nodes;
    b->stamp++;
    const uint32_t stamp = b->stamp;
    const int32_t fx = from % W, fy = from / W;
    int32_t max_man = 0;
    for (int k = 0; k < nt; k++) {
        const Node *t = &nodes[targets[k]];
        b->tmark[t->tile] = stamp;
        b->tidx[t->tile] = k;
        cost[k] = -1;
        max_man = OFE_MAX(max_man, iabs(t->x - fx) + iabs(t->y - fy));
    }
    const int32_t max_dist = max_man * 4; /* allow path deviation */
    int32_t found = 0, head = 0, tail = 0;
    b->vis[from] = stamp;
    b->dist[from] = 0;
    b->queue[tail++] = from;
    while (head < tail) {
        int32_t node = b->queue[head++];
        int32_t d = b->dist[node], x = node % W, y = node / W;
        bool is_target = b->tmark[node] == stamp;
        if (!(node == from || is_target) && (x < bd.min_x || x > bd.max_x || y < bd.min_y || y > bd.max_y)) continue;
        if (is_target) {
            cost[b->tidx[node]] = d;
            if (++found == nt) return;
        }
        int32_t nd = d + 1;
        if (nd > max_dist) continue;
        int32_t nb[4], k = 0;
        if (node >= W) nb[k++] = node - W;
        if (node < last_row) nb[k++] = node + W;
        if (x != 0) nb[k++] = node - 1;
        if (x != W - 1) nb[k++] = node + 1;
        for (int i = 0; i < k; i++) {
            if (b->vis[nb[i]] != stamp && mini_water(mn, nb[i])) {
                b->vis[nb[i]] = stamp;
                b->dist[nb[i]] = (uint16_t)nd;
                b->queue[tail++] = nb[i];
            }
        }
    }
}

static Graph *graph_build(const Mini *mn) {
    Graph *g = (Graph *)ofe_xcalloc(1, sizeof(Graph));
    const int32_t W = mn->w, H = mn->h, n = W * H;
    g->cxn = (W + CLUSTER_SIZE - 1) / CLUSTER_SIZE;
    g->cyn = (H + CLUSTER_SIZE - 1) / CLUSTER_SIZE;
    g->comp = (uint16_t *)ofe_xmalloc((size_t)n * sizeof(uint16_t));
    label_components(mn, g->comp);

    Builder b;
    memset(&b, 0, sizeof(b));
    b.mn = mn;
    b.g = g;
    const int32_t ncl = g->cxn * g->cyn;
    b.cluster_nodes = (IntVec *)ofe_xcalloc((size_t)ncl, sizeof(IntVec));
    b.tile_node = (int32_t *)ofe_xmalloc((size_t)n * sizeof(int32_t));
    for (int32_t i = 0; i < n; i++) b.tile_node[i] = -1;
    b.vis = (uint32_t *)ofe_xcalloc((size_t)n, sizeof(uint32_t));
    b.tmark = (uint32_t *)ofe_xcalloc((size_t)n, sizeof(uint32_t));
    b.queue = (int32_t *)ofe_xmalloc((size_t)n * sizeof(int32_t));
    b.tidx = (int32_t *)ofe_xmalloc((size_t)n * sizeof(int32_t));
    b.dist = (uint16_t *)ofe_xmalloc((size_t)n * sizeof(uint16_t));

    /* Nodes: gateways on cluster borders. */
    for (int32_t cy = 0; cy < g->cyn; cy++) {
        for (int32_t cx = 0; cx < g->cxn; cx++) {
            if (cx < g->cxn - 1) builder_border(&b, cx, cy, true);
            if (cy < g->cyn - 1) builder_border(&b, cx, cy, false);
        }
    }

    /* Edges: BFS distance between nodes of one cluster, cheapest cluster wins. */
    BuildEdgeVec edges = {0};
    int32_t seq = 0;
    int32_t *targets = NULL, *cost = NULL;
    int32_t tcap = 0;
    for (int32_t cy = 0; cy < g->cyn; cy++) {
        for (int32_t cx = 0; cx < g->cxn; cx++) {
            const IntVec *ids = &b.cluster_nodes[cy * g->cxn + cx];
            if (ids->len == 0) continue;
            if (ids->len > tcap) {
                tcap = ids->len;
                targets = (int32_t *)ofe_xrealloc(targets, (size_t)tcap * sizeof(int32_t));
                cost = (int32_t *)ofe_xrealloc(cost, (size_t)tcap * sizeof(int32_t));
            }
            Bounds bd = {cx * CLUSTER_SIZE, OFE_MIN(W - 1, cx * CLUSTER_SIZE + CLUSTER_SIZE - 1), cy * CLUSTER_SIZE,
                         OFE_MIN(H - 1, cy * CLUSTER_SIZE + CLUSTER_SIZE - 1)};
            for (int32_t i = 0; i < ids->len; i++) {
                const Node *from = &g->nodes[ids->data[i]];
                int nt = 0;
                for (int32_t j = i + 1; j < ids->len; j++) {
                    if (from->comp != g->nodes[ids->data[j]].comp) continue;
                    targets[nt++] = ids->data[j];
                }
                if (nt == 0) continue;
                builder_reach(&b, from->tile, targets, nt, bd, cost);
                for (int k = 0; k < nt; k++) {
                    if (cost[k] < 0) continue;
                    int32_t lo = OFE_MIN(ids->data[i], targets[k]), hi = OFE_MAX(ids->data[i], targets[k]);
                    BuildEdgeVec_push(&edges, (BuildEdge){lo, hi, cost[k], cx, cy, seq++});
                }
            }
        }
    }
    free(targets);
    free(cost);
    qsort(edges.data, (size_t)edges.len, sizeof(BuildEdge), build_edge_cmp);
    g->edges = (Edge *)ofe_xmalloc((size_t)(edges.len ? edges.len : 1) * sizeof(Edge));
    for (int32_t i = 0; i < edges.len; i++) {
        const BuildEdge *e = &edges.data[i];
        if (g->nedges > 0 && g->edges[g->nedges - 1].a == e->a && g->edges[g->nedges - 1].b == e->b) {
            Edge *old = &g->edges[g->nedges - 1];
            if (e->cost < old->cost) { /* existing edge, update if cheaper */
                old->cost = e->cost;
                old->cx = e->cx;
                old->cy = e->cy;
            }
            continue;
        }
        g->edges[g->nedges++] = (Edge){e->a, e->b, e->cost, e->cx, e->cy};
    }
    BuildEdgeVec_free(&edges);

    /* Per-node edge lists (increasing edge id, as AbstractGraph._sortEdges). */
    g->adj_off = (int32_t *)ofe_xcalloc((size_t)g->nnodes + 1, sizeof(int32_t));
    for (int32_t i = 0; i < g->nedges; i++) {
        g->adj_off[g->edges[i].a + 1]++;
        g->adj_off[g->edges[i].b + 1]++;
    }
    for (int32_t i = 0; i < g->nnodes; i++) g->adj_off[i + 1] += g->adj_off[i];
    g->adj = (int32_t *)ofe_xmalloc((size_t)(g->adj_off[g->nnodes] ? g->adj_off[g->nnodes] : 1) * sizeof(int32_t));
    int32_t *fill = (int32_t *)ofe_xcalloc((size_t)g->nnodes + 1, sizeof(int32_t));
    for (int32_t i = 0; i < g->nedges; i++) {
        g->adj[g->adj_off[g->edges[i].a] + fill[g->edges[i].a]++] = i;
        g->adj[g->adj_off[g->edges[i].b] + fill[g->edges[i].b]++] = i;
    }
    free(fill);

    /* Cluster node lists, flattened. */
    g->cl_off = (int32_t *)ofe_xmalloc((size_t)(ncl + 1) * sizeof(int32_t));
    int32_t total = 0;
    for (int32_t i = 0; i < ncl; i++) {
        g->cl_off[i] = total;
        total += b.cluster_nodes[i].len;
    }
    g->cl_off[ncl] = total;
    g->cl_nodes = (int32_t *)ofe_xmalloc((size_t)(total ? total : 1) * sizeof(int32_t));
    for (int32_t i = 0; i < ncl; i++) {
        if (b.cluster_nodes[i].len) memcpy(g->cl_nodes + g->cl_off[i], b.cluster_nodes[i].data, (size_t)b.cluster_nodes[i].len * sizeof(int32_t));
        IntVec_free(&b.cluster_nodes[i]);
    }
    free(b.cluster_nodes);
    free(b.tile_node);
    free(b.vis);
    free(b.tmark);
    free(b.queue);
    free(b.tidx);
    free(b.dist);
    return g;
}

static void graph_destroy(Graph *g) {
    if (!g) return;
    free(g->nodes);
    free(g->edges);
    free(g->adj_off);
    free(g->adj);
    free(g->cl_off);
    free(g->cl_nodes);
    free(g->comp);
    free(g);
}

/* ---- AbstractGraphAStar -------------------------------------------------- */

typedef struct AbsSearch {
    uint32_t stamp;
    uint32_t *closed, *gstamp;
    float *g;
    int32_t *came;
    MinHeap heap;
} AbsSearch;

static void abs_init(AbsSearch *s, int32_t nnodes) {
    memset(s, 0, sizeof(*s));
    s->closed = (uint32_t *)ofe_xcalloc((size_t)nnodes + 1, sizeof(uint32_t));
    s->gstamp = (uint32_t *)ofe_xcalloc((size_t)nnodes + 1, sizeof(uint32_t));
    s->g = (float *)ofe_xmalloc(((size_t)nnodes + 1) * sizeof(float));
    s->came = (int32_t *)ofe_xmalloc(((size_t)nnodes + 1) * sizeof(int32_t));
    s->stamp = 1;
}

static void abs_destroy(AbsSearch *s) {
    free(s->closed);
    free(s->gstamp);
    free(s->g);
    free(s->came);
    heap_destroy(&s->heap);
}

/* A* over the abstract graph from the best of starts (all start at cost 0;
 * heuristic weight 1, Manhattan). out = node ids from the winning start to goal. */
static bool abs_search(AbsSearch *s, const Graph *g, const int32_t *starts, int ns, int32_t goal, IntVec *out) {
    s->stamp++;
    const uint32_t stamp = s->stamp;
    const Node *gn = &g->nodes[goal];
    heap_clear(&s->heap);
    for (int i = 0; i < ns; i++) {
        const Node *n = &g->nodes[starts[i]];
        s->g[starts[i]] = 0;
        s->gstamp[starts[i]] = stamp;
        s->came[starts[i]] = -1;
        mh_push(&s->heap, starts[i], (double)(iabs(n->x - gn->x) + iabs(n->y - gn->y)));
    }
    int32_t iterations = 100000;
    while (s->heap.size > 0) {
        if (--iterations <= 0) return false;
        int32_t cur = mh_pop(&s->heap);
        if (s->closed[cur] == stamp) continue;
        s->closed[cur] = stamp;
        if (cur == goal) {
            out->len = 0;
            for (int32_t c = cur; c != -1; c = s->came[c]) IntVec_push(out, c);
            for (int32_t i = 0, j = out->len - 1; i < j; i++, j--) {
                int32_t t = out->data[i];
                out->data[i] = out->data[j];
                out->data[j] = t;
            }
            return true;
        }
        const double cur_g = (double)s->g[cur];
        for (int32_t k = g->adj_off[cur]; k < g->adj_off[cur + 1]; k++) {
            const Edge *e = &g->edges[g->adj[k]];
            int32_t nb = e->a == cur ? e->b : e->a;
            if (s->closed[nb] == stamp) continue;
            double tentative = cur_g + e->cost;
            if (s->gstamp[nb] != stamp || tentative < (double)s->g[nb]) {
                s->came[nb] = cur;
                s->g[nb] = (float)tentative;
                s->gstamp[nb] = stamp;
                const Node *n = &g->nodes[nb];
                mh_push(&s->heap, nb, tentative + (double)(iabs(n->x - gn->x) + iabs(n->y - gn->y)));
            }
        }
    }
    return false;
}

/* ---- Search context (scratch for one query at a time) ---------------------- */

#define MEMO_SHARDS 64
#define MEMO_WAYS 96 /* direct-mapped slots per shard: 64 * 96 = 6144 routes per map */
#define MEMO_MAX_LEN 2048 /* longer routes are not cached */

typedef struct MemoEntry {
    int64_t key; /* (from << 32) | to, -1 = empty */
    int32_t len;
    int32_t *tiles;
} MemoEntry;

typedef struct MemoShard {
    pthread_mutex_t lock;
    MemoEntry slot[MEMO_WAYS];
} MemoShard;

typedef struct Ctx {
    Bounded cluster; /* one cluster: 32 x 32 */
    Bounded multi; /* 3 x 3 clusters */
    Bounded shortp; /* short multi-source searches: 260 x 260 */
    Bounded smooth; /* smoothing's local A*: 100 x 100 */
    Bounded plain; /* whole mini map, only when the graph is tiny */
    AbsSearch abs;
    IntVec t0, t1, t2, t3, t4; /* temporaries: smoothing, route, abstract path, segment, mini route */
    IntVec sc[9]; /* source lists, see scratch_ints */
    struct Ctx *next;
} Ctx;

struct TsWater {
    const OfeMap *map;
    Mini mini;
    Graph *graph; /* NULL: plain A* on the mini map */
    uint16_t *comp; /* per mini tile water body (the graph's, or its own when graph is NULL) */
    bool own_comp;
    int32_t ncomp; /* highest body id */
    int32_t nodes;
    pthread_mutex_t lock;
    Ctx *free_ctx;
    MemoShard *memo; /* MEMO_SHARDS shards */
};

static Ctx *ctx_new(const TsWater *w) {
    Ctx *c = (Ctx *)ofe_xcalloc(1, sizeof(Ctx));
    bounded_init(&c->cluster, CLUSTER_SIZE * CLUSTER_SIZE, 3, 100000, 3);
    bounded_init(&c->multi, 9 * CLUSTER_SIZE * CLUSTER_SIZE, 3, 100000, 3);
    bounded_init(&c->shortp, 260 * 260, 3, 100000, 3);
    bounded_init(&c->smooth, 100 * 100, 3, 100000, 3);
    if (w->graph) abs_init(&c->abs, w->graph->nnodes);
    else bounded_init(&c->plain, w->mini.w * w->mini.h, 5, 1000000, 10);
    return c;
}

static void ctx_destroy(Ctx *c) {
    bounded_destroy(&c->cluster);
    bounded_destroy(&c->multi);
    bounded_destroy(&c->shortp);
    bounded_destroy(&c->smooth);
    if (c->plain.cap) bounded_destroy(&c->plain);
    abs_destroy(&c->abs);
    IntVec_free(&c->t0);
    IntVec_free(&c->t1);
    IntVec_free(&c->t2);
    IntVec_free(&c->t3);
    IntVec_free(&c->t4);
    for (int i = 0; i < 9; i++) IntVec_free(&c->sc[i]);
    free(c);
}

static Ctx *ctx_get(const TsWater *cw) {
    TsWater *w = (TsWater *)cw;
    pthread_mutex_lock(&w->lock);
    Ctx *c = w->free_ctx;
    if (c) w->free_ctx = c->next;
    pthread_mutex_unlock(&w->lock);
    return c ? c : ctx_new(cw);
}

static void ctx_put(const TsWater *cw, Ctx *c) {
    TsWater *w = (TsWater *)cw;
    pthread_mutex_lock(&w->lock);
    c->next = w->free_ctx;
    w->free_ctx = c;
    pthread_mutex_unlock(&w->lock);
}

/* Scratch int arrays for source lists (any number of sources). */
static int32_t *scratch_ints(Ctx *c, int k, int32_t n) {
    IntVec_reserve(&c->sc[k], n > 0 ? n : 1);
    return c->sc[k].data;
}

/* ---- AStarWaterHierarchical ------------------------------------------------ */

typedef struct Hpa {
    const TsWater *w;
    const Graph *g;
    const Mini *mn;
    Ctx *c;
} Hpa;

static inline Bounds cluster_bounds(const Hpa *h, int32_t cx, int32_t cy) {
    int32_t min_x = cx * CLUSTER_SIZE, min_y = cy * CLUSTER_SIZE;
    return (Bounds){min_x, OFE_MIN(h->mn->w - 1, min_x + CLUSTER_SIZE - 1), min_y,
                    OFE_MIN(h->mn->h - 1, min_y + CLUSTER_SIZE - 1)};
}

/* findLocalPath: bounded A* inside one cluster (or the 3x3 block around it),
 * with the endpoints forced to be the requested tiles. */
static bool hpa_local(Hpa *h, int32_t from, int32_t to, int32_t cx, int32_t cy, bool multi, IntVec *out) {
    Bounds bd;
    Bounded *b;
    if (multi) {
        bd.min_x = OFE_MAX(0, (cx - 1) * CLUSTER_SIZE);
        bd.min_y = OFE_MAX(0, (cy - 1) * CLUSTER_SIZE);
        bd.max_x = OFE_MIN(h->mn->w - 1, (cx + 2) * CLUSTER_SIZE - 1);
        bd.max_y = OFE_MIN(h->mn->h - 1, (cy + 2) * CLUSTER_SIZE - 1);
        b = &h->c->multi;
    } else {
        bd = cluster_bounds(h, cx, cy);
        b = &h->c->cluster;
    }
    if (!bounded_search(b, h->mn, &from, 1, to, bd, out) || out->len == 0) return false;
    if (out->data[0] != from) {
        IntVec_push(out, 0);
        memmove(out->data + 1, out->data, (size_t)(out->len - 1) * sizeof(int32_t));
        out->data[0] = from;
    }
    if (out->data[out->len - 1] != to) IntVec_push(out, to);
    return true;
}

/* findNearestNode: BFS over water from tile, inside its cluster, for the
 * first (in BFS order) abstract node. Returns the node id or -1. */
static int32_t hpa_nearest_node(const Hpa *h, int32_t tile) {
    const Graph *g = h->g;
    const int32_t W = h->mn->w, H = h->mn->h;
    const int32_t x = tile % W, y = tile / W;
    const int32_t cx = x / CLUSTER_SIZE, cy = y / CLUSTER_SIZE;
    const int32_t cl = cy * g->cxn + cx;
    if (g->cl_off[cl + 1] == g->cl_off[cl]) return -1;
    const Bounds bd = cluster_bounds(h, cx, cy);
    /* The search never leaves the cluster plus a ring: tiles outside are
     * visited but not expanded. A window of that size replaces BFSGrid's
     * map-sized arrays. */
    enum { WIN = CLUSTER_SIZE + 2 };
    uint8_t seen[WIN * WIN];
    uint16_t dist[WIN * WIN];
    int32_t queue[WIN * WIN];
    memset(seen, 0, sizeof(seen));
    const int32_t ox = bd.min_x - 1, oy = bd.min_y - 1;
#define WIDX(tx, ty) (((ty) - oy) * WIN + ((tx) - ox))
    int32_t head = 0, tail = 0;
    seen[WIDX(x, y)] = 1;
    dist[WIDX(x, y)] = 0;
    queue[tail++] = tile;
    const int32_t max_dist = CLUSTER_SIZE * CLUSTER_SIZE;
    while (head < tail) {
        int32_t node = queue[head++];
        int32_t nx = node % W, ny = node / W;
        int32_t d = dist[WIDX(nx, ny)];
        for (int32_t k = g->cl_off[cl]; k < g->cl_off[cl + 1]; k++) {
            const Node *cand = &g->nodes[g->cl_nodes[k]];
            if (cand->x == nx && cand->y == ny) return g->cl_nodes[k];
        }
        if (nx < bd.min_x || nx > bd.max_x || ny < bd.min_y || ny > bd.max_y) continue;
        if (d + 1 > max_dist) continue;
        int32_t nb[4], cnt = 0;
        if (node >= W) nb[cnt++] = node - W;
        if (node < (H - 1) * W) nb[cnt++] = node + W;
        if (nx != 0) nb[cnt++] = node - 1;
        if (nx != W - 1) nb[cnt++] = node + 1;
        for (int i = 0; i < cnt; i++) {
            int32_t mx = nb[i] % W, my = nb[i] / W;
            if (mx < ox || mx >= ox + WIN || my < oy || my >= oy + WIN) continue; /* cannot happen */
            if (!seen[WIDX(mx, my)] && mini_water(h->mn, nb[i])) {
                seen[WIDX(mx, my)] = 1;
                dist[WIDX(mx, my)] = (uint16_t)(d + 1);
                queue[tail++] = nb[i];
            }
        }
    }
    return -1;
#undef WIDX
}

/* getClusterNode: the cluster's node closest (Manhattan) to the tile, first on ties. */
static int32_t hpa_cluster_node(const Hpa *h, int32_t tile) {
    const Graph *g = h->g;
    int32_t x = tile % h->mn->w, y = tile / h->mn->w;
    int32_t cl = (y / CLUSTER_SIZE) * g->cxn + (x / CLUSTER_SIZE);
    int32_t best = -1, best_d = INT32_MAX;
    for (int32_t k = g->cl_off[cl]; k < g->cl_off[cl + 1]; k++) {
        const Node *n = &g->nodes[g->cl_nodes[k]];
        int32_t d = iabs(n->x - x) + iabs(n->y - y);
        if (d < best_d) {
            best_d = d;
            best = g->cl_nodes[k];
        }
    }
    return best;
}

static const Edge *graph_edge_between(const Graph *g, int32_t a, int32_t b) {
    for (int32_t k = g->adj_off[a]; k < g->adj_off[a + 1]; k++) {
        const Edge *e = &g->edges[g->adj[k]];
        if (e->a == b || e->b == b) return e;
    }
    return NULL;
}

/* findPathSingle. out gets the mini map route from..to. */
static bool hpa_single(Hpa *h, int32_t from, int32_t to, IntVec *out) {
    const Graph *g = h->g;
    const int32_t W = h->mn->w;
    IntVec *seg = &h->c->t3;
    out->len = 0;

    if (mini_manhattan(h->mn, from, to) <= CLUSTER_SIZE) {
        if (hpa_local(h, from, to, (from % W) / CLUSTER_SIZE, (from / W) / CLUSTER_SIZE, true, out)) return true;
        out->len = 0;
    }

    int32_t start_node = hpa_nearest_node(h, from);
    int32_t end_node = hpa_nearest_node(h, to);
    if (start_node < 0 || end_node < 0) return false;

    if (start_node == end_node) {
        const Node *sn = &g->nodes[start_node];
        return hpa_local(h, from, to, sn->x / CLUSTER_SIZE, sn->y / CLUSTER_SIZE, true, out);
    }

    IntVec *np = &h->c->t2;
    np->len = 0;
    if (!abs_search(&h->c->abs, g, &start_node, 1, end_node, np)) return false;

    /* 1. from -> first node */
    const Node *first = &g->nodes[np->data[0]];
    if (!hpa_local(h, from, first->tile, (from % W) / CLUSTER_SIZE, (from / W) / CLUSTER_SIZE, false, seg)) return false;
    for (int32_t i = 0; i < seg->len; i++) IntVec_push(out, seg->data[i]);

    /* 2. through the abstract nodes */
    for (int32_t i = 0; i + 1 < np->len; i++) {
        const Edge *e = graph_edge_between(g, np->data[i], np->data[i + 1]);
        if (!e) return false;
        if (!hpa_local(h, g->nodes[np->data[i]].tile, g->nodes[np->data[i + 1]].tile, e->cx, e->cy, false, seg)) return false;
        for (int32_t k = 1; k < seg->len; k++) IntVec_push(out, seg->data[k]);
    }

    /* 3. last node -> to */
    const Node *last = &g->nodes[np->data[np->len - 1]];
    if (!hpa_local(h, last->tile, to, (to % W) / CLUSTER_SIZE, (to / W) / CLUSTER_SIZE, false, seg)) return false;
    for (int32_t k = 1; k < seg->len; k++) IntVec_push(out, seg->data[k]);
    return true;
}

/* findPathMultiSource (two or more sources). */
static bool hpa_multi(Hpa *h, const int32_t *src, int ns, int32_t target, IntVec *out) {
    const Mini *mn = h->mn;
    /* Short routes first: one bounded A* from every source within 120 tiles. */
    int32_t *cand = scratch_ints(h->c, 5, ns);
    int nc = 0;
    for (int i = 0; i < ns; i++) {
        if (mini_manhattan(mn, src[i], target) <= 120) cand[nc++] = src[i];
    }
    if (nc > 0) {
        Bounds bd = {target % mn->w, target % mn->w, target / mn->w, target / mn->w};
        for (int i = 0; i < nc; i++) {
            bd.min_x = OFE_MIN(bd.min_x, cand[i] % mn->w);
            bd.max_x = OFE_MAX(bd.max_x, cand[i] % mn->w);
            bd.min_y = OFE_MIN(bd.min_y, cand[i] / mn->w);
            bd.max_y = OFE_MAX(bd.max_y, cand[i] / mn->w);
        }
        bd.min_x = OFE_MAX(0, bd.min_x - 10);
        bd.max_x = OFE_MIN(mn->w - 1, bd.max_x + 10);
        bd.min_y = OFE_MAX(0, bd.min_y - 10);
        bd.max_y = OFE_MIN(mn->h - 1, bd.max_y + 10);
        if (bounded_search(&h->c->shortp, mn, cand, nc, target, bd, out) && out->len > 0) return true;
    }

    int32_t target_node = hpa_cluster_node(h, target);
    if (target_node < 0) return false;

    /* Sources -> abstract nodes, keeping the closest source per node, in first-seen order. */
    int32_t *node_ids = scratch_ints(h->c, 6, ns), *node_src = scratch_ints(h->c, 7, ns),
            *node_dist = scratch_ints(h->c, 8, ns);
    int nn = 0;
    for (int i = 0; i < ns; i++) {
        int32_t node = hpa_cluster_node(h, src[i]);
        if (node < 0) continue;
        int32_t d = iabs(h->g->nodes[node].x - src[i] % mn->w) + iabs(h->g->nodes[node].y - src[i] / mn->w);
        int k;
        for (k = 0; k < nn; k++) {
            if (node_ids[k] == node) break;
        }
        if (k == nn) {
            node_ids[nn] = node;
            node_src[nn] = src[i];
            node_dist[nn++] = d;
        } else if (d < node_dist[k]) {
            node_src[k] = src[i];
            node_dist[k] = d;
        }
    }
    if (nn == 0) return false;

    IntVec *np = &h->c->t2;
    np->len = 0;
    if (!abs_search(&h->c->abs, h->g, node_ids, nn, target_node, np)) return false;
    int32_t winner = -1;
    for (int k = 0; k < nn; k++) {
        if (node_ids[k] == np->data[0]) winner = node_src[k];
    }
    return hpa_single(h, winner, target, out);
}

/* ---- SmoothingWaterTransformer ----------------------------------------------- */

#define ENDPOINT_REFINEMENT_TILES 50
#define LOS_MIN_MAGNITUDE_PASS1 2
#define LOS_MIN_MAGNITUDE_PASS2 3
#define LOS_MAX_TILES 100000

static inline bool smooth_ok(const Mini *mn, int32_t tile, int32_t min_mag) {
    return mini_water(mn, tile) && (mn->terr[tile] & MAGNITUDE_MASK) >= min_mag;
}

/* canSee: Bresenham line over water that stays min_mag tiles from shore. */
static bool smooth_can_see(const Mini *mn, int32_t from, int32_t to, int32_t min_mag) {
    const int32_t W = mn->w;
    int32_t x0 = from % W, y0 = from / W, x1 = to % W, y1 = to / W;
    int32_t dx = iabs(x1 - x0), dy = iabs(y1 - y0), sx = x0 < x1 ? 1 : -1, sy = y0 < y1 ? 1 : -1;
    int32_t err = dx - dy, x = x0, y = y0, iterations = 0;
    for (;;) {
        if (iterations++ > LOS_MAX_TILES) return false;
        if (!smooth_ok(mn, y * W + x, min_mag)) return false;
        if (x == x1 && y == y1) return true;
        int32_t e2 = 2 * err;
        bool move_x = e2 > -dy, move_y = e2 < dx;
        if (move_x && move_y) {
            x += sx;
            err -= dy;
            if (!smooth_ok(mn, y * W + x, min_mag)) { /* try the other corner */
                x -= sx;
                err += dy;
                y += sy;
                err += dx;
                if (!smooth_ok(mn, y * W + x, min_mag)) return false;
                x += sx;
                err -= dy;
            } else {
                y += sy;
                err += dx;
            }
        } else {
            if (move_x) {
                x += sx;
                err -= dy;
            }
            if (move_y) {
                y += sy;
                err += dx;
            }
        }
    }
}

/* tracePath: the tiles of that line (every tile, water only). false = blocked. */
static bool smooth_trace(const Mini *mn, int32_t from, int32_t to, IntVec *tiles) {
    const int32_t W = mn->w;
    int32_t x0 = from % W, y0 = from / W, x1 = to % W, y1 = to / W;
    int32_t dx = iabs(x1 - x0), dy = iabs(y1 - y0), sx = x0 < x1 ? 1 : -1, sy = y0 < y1 ? 1 : -1;
    int32_t err = dx - dy, x = x0, y = y0, iterations = 0;
    tiles->len = 0;
    for (;;) {
        if (iterations++ > LOS_MAX_TILES) return false;
        int32_t tile = y * W + x;
        if (!mini_water(mn, tile)) return false;
        IntVec_push(tiles, tile);
        if (x == x1 && y == y1) break;
        int32_t e2 = 2 * err;
        bool move_x = e2 > -dy, move_y = e2 < dx;
        if (move_x && move_y) {
            x += sx;
            err -= dy;
            int32_t inter = y * W + x;
            if (!mini_water(mn, inter)) {
                x -= sx;
                err += dy;
                y += sy;
                err += dx;
                int32_t alt = y * W + x;
                if (!mini_water(mn, alt)) return false;
                IntVec_push(tiles, alt);
                x += sx;
                err -= dy;
            } else {
                IntVec_push(tiles, inter);
                y += sy;
                err += dx;
            }
        } else {
            if (move_x) {
                x += sx;
                err -= dy;
            }
            if (move_y) {
                y += sy;
                err += dx;
            }
        }
    }
    return true;
}

/* losSmooth: from each waypoint jump to the farthest one it can see
 * (binary search), filling in the straight line between. */
static void smooth_los(const Mini *mn, const IntVec *path, int32_t min_mag, IntVec *result, IntVec *trace) {
    result->len = 0;
    IntVec_push(result, path->data[0]);
    int32_t current = 0;
    while (current < path->len - 1) {
        int32_t lo = current + 1, hi = path->len - 1, farthest = lo;
        while (lo <= hi) {
            int32_t mid = (int32_t)((uint32_t)(lo + hi) >> 1);
            if (smooth_can_see(mn, path->data[current], path->data[mid], min_mag)) {
                farthest = mid;
                lo = mid + 1;
            } else {
                hi = mid - 1;
            }
        }
        if (farthest > current + 1) {
            if (smooth_trace(mn, path->data[current], path->data[farthest], trace)) {
                for (int32_t i = 1; i < trace->len - 1; i++) IntVec_push(result, trace->data[i]);
            }
        }
        current = farthest;
        if (current < path->len - 1) IntVec_push(result, path->data[current]);
    }
    IntVec_push(result, path->data[path->len - 1]);
}

static int32_t smooth_tile_at_distance(const Mini *mn, const IntVec *path, int32_t idx, int32_t distance, bool forward) {
    int32_t cum = 0;
    if (forward) {
        while (idx < path->len - 1 && cum < distance) {
            cum += mini_manhattan(mn, path->data[idx], path->data[idx + 1]);
            idx++;
        }
    } else {
        while (idx > 0 && cum < distance) {
            cum += mini_manhattan(mn, path->data[idx], path->data[idx - 1]);
            idx--;
        }
    }
    return idx;
}

static bool smooth_refine_segment(Ctx *c, const Mini *mn, int32_t from, int32_t to, IntVec *out) {
    const int32_t padding = 10;
    int32_t x0 = from % mn->w, y0 = from / mn->w, x1 = to % mn->w, y1 = to / mn->w;
    Bounds bd = {OFE_MAX(0, OFE_MIN(x0, x1) - padding), OFE_MIN(mn->w - 1, OFE_MAX(x0, x1) + padding),
                 OFE_MAX(0, OFE_MIN(y0, y1) - padding), OFE_MIN(mn->h - 1, OFE_MAX(y0, y1) + padding)};
    return bounded_search(&c->smooth, mn, &from, 1, to, bd, out);
}

/* refineEndpoints: re-search the first and last 50 tiles with bounded A*.
 * path is replaced (in place, using tmp as scratch). */
static void smooth_refine(Ctx *c, const Mini *mn, IntVec *path, IntVec *tmp, IntVec *seg) {
    if (path->len <= 2) return;
    int32_t start_end = smooth_tile_at_distance(mn, path, 0, ENDPOINT_REFINEMENT_TILES, true);
    if (start_end > 1 && smooth_refine_segment(c, mn, path->data[0], path->data[start_end], seg) && seg->len > 0) {
        tmp->len = 0;
        for (int32_t i = 0; i < seg->len - 1; i++) IntVec_push(tmp, seg->data[i]);
        for (int32_t i = start_end; i < path->len; i++) IntVec_push(tmp, path->data[i]);
        IntVec_copy(path, tmp);
    }
    int32_t end_start = smooth_tile_at_distance(mn, path, path->len - 1, ENDPOINT_REFINEMENT_TILES, false);
    if (end_start < path->len - 2 &&
        smooth_refine_segment(c, mn, path->data[path->len - 1], path->data[end_start], seg) && seg->len > 0) {
        tmp->len = 0;
        for (int32_t i = 0; i < end_start; i++) IntVec_push(tmp, path->data[i]);
        for (int32_t i = seg->len - 1; i >= 0; i--) IntVec_push(tmp, seg->data[i]); /* reversed */
        IntVec_copy(path, tmp);
    }
}

/* SmoothingWaterTransformer.smooth, in place. */
static void smooth_path(Ctx *c, const Mini *mn, IntVec *path) {
    if (path->len <= 2) return;
    IntVec *a = &c->t0, *b = &c->t1, *trace = &c->t3;
    /* Pass 1: line of sight, keeping 2 tiles off the shore. */
    smooth_los(mn, path, LOS_MIN_MAGNITUDE_PASS1, a, trace);
    /* Pass 2: local A* on both ends. */
    smooth_refine(c, mn, a, b, trace);
    /* Pass 3: line of sight again, farther from the shore. */
    smooth_los(mn, a, LOS_MIN_MAGNITUDE_PASS2, path, trace);
}

/* ---- Component check, shore coercing, mini-map transformer ------------------- */

/* Water search on the mini map for sources (already water, same-body filtered)
 * to a water destination: ComponentCheck + Smoothing + HPA (or plain A*). */
static bool water_core(const TsWater *w, Ctx *c, const int32_t *src, int ns, int32_t to, IntVec *out) {
    const Mini *mn = &w->mini;
    if (!w->graph) { /* AStarWater: all sources, no component check, no smoothing */
        Bounds all = {0, mn->w - 1, 0, mn->h - 1};
        return bounded_search(&c->plain, mn, src, ns, to, all, out) && out->len > 0;
    }
    const uint16_t *comp = w->comp;
    int32_t *valid = scratch_ints(c, 4, ns);
    int nv = 0;
    for (int i = 0; i < ns; i++) {
        if (comp[src[i]] == comp[to]) valid[nv++] = src[i];
    }
    if (nv == 0) return false;
    Hpa h = {w, w->graph, mn, c};
    bool ok = nv == 1 ? hpa_single(&h, valid[0], to, out) : hpa_multi(&h, valid, nv, to, out);
    if (!ok) return false;
    smooth_path(c, mn, out);
    return out->len > 0;
}

/* bestWaterNeighbor: the water 4-neighbour with the most water neighbours (first on ties). */
static int32_t shore_best_neighbor(const Mini *mn, int32_t tile) {
    int32_t best = -1, max_score = -1;
    const int32_t W = mn->w, H = mn->h;
    int32_t nb[4], k = 0, x = tile % W;
    if (tile >= W) nb[k++] = tile - W;
    if (tile < (H - 1) * W) nb[k++] = tile + W;
    if (x != 0) nb[k++] = tile - 1;
    if (x != W - 1) nb[k++] = tile + 1;
    for (int i = 0; i < k; i++) {
        if (!mini_water(mn, nb[i])) continue;
        int32_t n2[4], k2 = 0, x2 = nb[i] % W, score = 0;
        if (nb[i] >= W) n2[k2++] = nb[i] - W;
        if (nb[i] < (H - 1) * W) n2[k2++] = nb[i] + W;
        if (x2 != 0) n2[k2++] = nb[i] - 1;
        if (x2 != W - 1) n2[k2++] = nb[i] + 1;
        for (int j = 0; j < k2; j++) score += mini_water(mn, n2[j]);
        if (score > max_score) {
            max_score = score;
            best = nb[i];
        }
    }
    return best;
}

/* ShoreCoercingTransformer.findPath on mini map tiles. out = mini route
 * with the original shore tiles restored at the ends. */
static bool shore_find(const TsWater *w, Ctx *c, const int32_t *from, int nfrom, int32_t to, IntVec *out) {
    const Mini *mn = &w->mini;
    int32_t *water_from = scratch_ints(c, 1, nfrom);
    int nw = 0;
    /* waterToOriginal: water tile -> shore tile it stood in for (last write wins, a raw water source erases). */
    int32_t *map_key = scratch_ints(c, 2, nfrom), *map_val = scratch_ints(c, 3, nfrom);
    int nm = 0;
    for (int i = 0; i < nfrom; i++) {
        int32_t f = from[i];
        if (mini_water(mn, f)) {
            water_from[nw++] = f;
            for (int k = 0; k < nm; k++) {
                if (map_key[k] == f) {
                    map_key[k] = map_key[nm - 1];
                    map_val[k] = map_val[nm - 1];
                    nm--;
                    break;
                }
            }
        } else {
            int32_t wt = shore_best_neighbor(mn, f);
            if (wt != -1) {
                water_from[nw++] = wt;
                int k;
                for (k = 0; k < nm; k++) {
                    if (map_key[k] == wt) break;
                }
                if (k == nm) map_key[nm++] = wt;
                map_val[k] = f;
            }
        }
    }
    if (nw == 0) return false;

    int32_t water_to = to, original_to = -1;
    if (!mini_water(mn, to)) {
        water_to = shore_best_neighbor(mn, to);
        if (water_to == -1) return false;
        original_to = to;
    }

    if (!water_core(w, c, water_from, nw, water_to, out) || out->len == 0) return false;

    for (int k = 0; k < nm; k++) {
        if (map_key[k] == out->data[0]) { /* restore the original start shore tile */
            IntVec_push(out, 0);
            memmove(out->data + 1, out->data, (size_t)(out->len - 1) * sizeof(int32_t));
            out->data[0] = map_val[k];
            break;
        }
    }
    if (original_to != -1 && out->data[out->len - 1] != original_to) IntVec_push(out, original_to);
    return true;
}

static int32_t index_of(const int32_t *a, int32_t n, int32_t v) {
    for (int32_t i = 0; i < n; i++) {
        if (a[i] == v) return i;
    }
    return -1;
}

/* MiniMapTransformer.findPath. from/to are full-map tiles. */
static bool mini_find(const TsWater *w, Ctx *c, const TileRef *from, int nfrom, TileRef to, IntVec *out) {
    const OfeMap *map = w->map;
    const Mini *mn = &w->mini;
    int32_t *mfrom = scratch_ints(c, 0, nfrom);
    for (int i = 0; i < nfrom; i++) {
        int32_t mx = (from[i] % map->width) / 2, my = (from[i] / map->width) / 2;
        if (mx >= mn->w || my >= mn->h) return false; /* TS throws here */
        mfrom[i] = my * mn->w + mx;
    }
    int32_t mx = (to % map->width) / 2, my = (to / map->width) / 2;
    if (mx >= mn->w || my >= mn->h) return false;
    int32_t mto = my * mn->w + mx;

    IntVec *mp = &c->t4; /* the route on the mini map */
    mp->len = 0;
    if (!shore_find(w, c, mfrom, nfrom, mto, mp) || mp->len == 0) return false;

    /* upscalePath: mini tile -> its top-left full-map tile, with interpolated tiles between. */
    IntVec *up = out;
    up->len = 0;
    const int32_t FW = map->width;
    for (int32_t i = 0; i + 1 < mp->len; i++) {
        int32_t cur_x = (mp->data[i] % mn->w) * 2, cur_y = (mp->data[i] / mn->w) * 2;
        int32_t next_x = (mp->data[i + 1] % mn->w) * 2, next_y = (mp->data[i + 1] / mn->w) * 2;
        IntVec_push(up, cur_y * FW + cur_x);
        int32_t dx = next_x - cur_x, dy = next_y - cur_y, steps = OFE_MAX(iabs(dx), iabs(dy));
        for (int32_t s = 1; s < steps; s++) {
            double rx = floor((double)cur_x + ((double)dx * s) / steps + 0.5); /* Math.round */
            double ry = floor((double)cur_y + ((double)dy * s) / steps + 0.5);
            IntVec_push(up, (int32_t)ry * FW + (int32_t)rx);
        }
    }
    {
        int32_t last = mp->data[mp->len - 1];
        IntVec_push(up, (last / mn->w) * 2 * FW + (last % mn->w) * 2);
    }

    /* The source whose position is closest to the route start (several sources),
     * or the only source. */
    int32_t src = from[0];
    if (nfrom > 1) {
        int32_t sx = up->data[0] % FW, sy = up->data[0] / FW, min_d = INT32_MAX;
        for (int i = 0; i < nfrom; i++) {
            int32_t d = iabs(from[i] % FW - sx) + iabs(from[i] / FW - sy);
            if (d < min_d) {
                min_d = d;
                src = from[i];
            }
        }
    }

    /* fixExtremes */
    int32_t si = index_of(up->data, up->len, src);
    if (si == -1) {
        IntVec_push(up, 0);
        memmove(up->data + 1, up->data, (size_t)(up->len - 1) * sizeof(int32_t));
        up->data[0] = src;
    } else if (si != 0) {
        memmove(up->data, up->data + si, (size_t)(up->len - si) * sizeof(int32_t));
        up->len -= si;
    }
    int32_t di = index_of(up->data, up->len, to);
    if (di == -1) IntVec_push(up, to);
    else if (di != up->len - 1) up->len = di + 1;
    return true;
}

/* ---- Public API --------------------------------------------------------------- */

static void *ts_water_build(const OfeMap *m) {
    TsWater *w = (TsWater *)ofe_xcalloc(1, sizeof(TsWater));
    w->map = m;
    w->mini = (Mini){m->mini_width, m->mini_height, m->mini_terrain};
    pthread_mutex_init(&w->lock, NULL);
    w->graph = graph_build(&w->mini);
    w->nodes = w->graph->nnodes;
    w->comp = w->graph->comp;
    for (int32_t i = 0; i < w->mini.w * w->mini.h; i++) {
        if (w->comp[i] != 0xFFFF && w->comp[i] > w->ncomp) w->ncomp = w->comp[i];
    }
    if (w->nodes < MIN_HPA_NODES) { /* TS: simple A* chain, no abstract graph */
        w->graph->comp = NULL; /* the components stay: water bodies are still queried */
        w->own_comp = true;
        graph_destroy(w->graph);
        w->graph = NULL;
    }
    w->memo = (MemoShard *)ofe_xcalloc(MEMO_SHARDS, sizeof(MemoShard));
    for (int i = 0; i < MEMO_SHARDS; i++) {
        pthread_mutex_init(&w->memo[i].lock, NULL);
        for (int k = 0; k < MEMO_WAYS; k++) w->memo[i].slot[k].key = -1;
    }
    return w;
}

static void ts_water_destroy(void *p) {
    TsWater *w = (TsWater *)p;
    for (Ctx *c = w->free_ctx; c;) {
        Ctx *next = c->next;
        ctx_destroy(c);
        c = next;
    }
    graph_destroy(w->graph);
    if (w->own_comp) free(w->comp);
    for (int i = 0; i < MEMO_SHARDS; i++) {
        for (int k = 0; k < MEMO_WAYS; k++) free(w->memo[i].slot[k].tiles);
        pthread_mutex_destroy(&w->memo[i].lock);
    }
    free(w->memo);
    pthread_mutex_destroy(&w->lock);
    free(w);
}

const TsWater *ts_water(OfeMap *map) {
    return (const TsWater *)map_cache_get(map, MAP_CACHE_TS_WATER, ts_water_build, ts_water_destroy);
}

int32_t ts_water_node_count(const TsWater *w) { return w->nodes; }
int32_t ts_water_num_components(const TsWater *w) { return w->ncomp; }

static inline bool valid_ref(const TsWater *w, int64_t t) { return t >= 0 && t < w->map->num_tiles; }

static inline MemoShard *memo_shard(const TsWater *w, int64_t key, int32_t *way) {
    uint64_t h = (uint64_t)key * 0x9E3779B97F4A7C15ull;
    *way = (int32_t)((h >> 20) % MEMO_WAYS);
    return &w->memo[(h >> 58) % MEMO_SHARDS];
}

static bool memo_get(const TsWater *w, TileRef from, TileRef to, IntVec *out) {
    int64_t key = ((int64_t)from << 32) | (uint32_t)to;
    int32_t way;
    MemoShard *sh = memo_shard(w, key, &way);
    bool hit = false;
    pthread_mutex_lock(&sh->lock);
    const MemoEntry *e = &sh->slot[way];
    if (e->key == key) {
        IntVec_reserve(out, e->len);
        memcpy(out->data, e->tiles, (size_t)e->len * sizeof(int32_t));
        out->len = e->len;
        hit = true;
    }
    pthread_mutex_unlock(&sh->lock);
    return hit;
}

static void memo_put(const TsWater *w, TileRef from, TileRef to, const IntVec *route) {
    if (route->len > MEMO_MAX_LEN) return;
    int64_t key = ((int64_t)from << 32) | (uint32_t)to;
    int32_t way;
    MemoShard *sh = memo_shard(w, key, &way);
    int32_t *copy = (int32_t *)ofe_xmalloc((size_t)route->len * sizeof(int32_t));
    memcpy(copy, route->data, (size_t)route->len * sizeof(int32_t));
    pthread_mutex_lock(&sh->lock);
    MemoEntry *e = &sh->slot[way];
    int32_t *old = e->tiles;
    e->key = key;
    e->len = route->len;
    e->tiles = copy;
    pthread_mutex_unlock(&sh->lock);
    free(old);
}

/* ---- Water bodies (WaterManager.getWaterComponent / hasWaterComponent) ------ */

/* Mini neighbours in TS's N, S, W, E order. */
static int mini_neighbors(const Mini *mn, int32_t t, int32_t out[4]) {
    int k = 0;
    const int32_t x = t % mn->w;
    if (t >= mn->w) out[k++] = t - mn->w;
    if (t < (mn->h - 1) * mn->w) out[k++] = t + mn->w;
    if (x > 0) out[k++] = t - 1;
    if (x < mn->w - 1) out[k++] = t + 1;
    return k;
}

static inline int32_t mini_of(const TsWater *w, TileRef t) {
    const int32_t FW = w->map->width;
    return ((t / FW) / 2) * w->mini.w + (t % FW) / 2;
}

int32_t ts_water_component(const TsWater *w, TileRef tile) {
    if (!valid_ref(w, tile)) return -1;
    const Mini *mn = &w->mini;
    int32_t mt = mini_of(w, tile);
    if (mini_water(mn, mt)) return w->comp[mt];
    int32_t nb[4];
    int n = mini_neighbors(mn, mt, nb);
    for (int i = 0; i < n; i++) {
        if (mini_water(mn, nb[i])) return w->comp[nb[i]];
    }
    for (int i = 0; i < n; i++) { /* narrow straits: 2 hops */
        int32_t n2[4];
        int k2 = mini_neighbors(mn, nb[i], n2);
        for (int j = 0; j < k2; j++) {
            if (mini_water(mn, n2[j])) return w->comp[n2[j]];
        }
    }
    return -1;
}

bool ts_water_has_component(const TsWater *w, TileRef tile, int32_t component) {
    if (!valid_ref(w, tile)) return false;
    const Mini *mn = &w->mini;
    int32_t mt = mini_of(w, tile);
    if (mini_water(mn, mt) && w->comp[mt] == component) return true;
    int32_t nb[4];
    int n = mini_neighbors(mn, mt, nb);
    for (int i = 0; i < n; i++) {
        if (mini_water(mn, nb[i]) && w->comp[nb[i]] == component) return true;
    }
    for (int i = 0; i < n; i++) {
        int32_t n2[4];
        int k2 = mini_neighbors(mn, nb[i], n2);
        for (int j = 0; j < k2; j++) {
            if (mini_water(mn, n2[j]) && w->comp[n2[j]] == component) return true;
        }
    }
    return false;
}

bool ts_water_find_path(const TsWater *w, const TileRef *from, int nfrom, TileRef to, IntVec *out) {
    out->len = 0;
    if (nfrom <= 0 || !valid_ref(w, to)) return false;
    bool any_valid = false;
    for (int i = 0; i < nfrom; i++) any_valid |= valid_ref(w, from[i]);
    if (!any_valid) return false; /* stepper preCheck: every source invalid */
    for (int i = 0; i < nfrom; i++) {
        if (!valid_ref(w, from[i])) return false; /* TS throws on a mix; treat as no route */
    }
    if (nfrom == 1 && memo_get(w, from[0], to, out)) return true;
    Ctx *c = ctx_get(w);
    bool ok = mini_find(w, c, from, nfrom, to, out);
    ctx_put(w, c);
    if (!ok) out->len = 0;
    else if (nfrom == 1) memo_put(w, from[0], to, out);
    return ok;
}

/* ---- closestShoreByWater (SpatialQuery) ------------------------------------------ */

/* refineStartTile: among the shores within 20 tiles of the route's start,
 * a bounded A* (full-resolution map, at most 100 x 100 tiles) towards a
 * waypoint 50..200 tiles along the route picks the start. */
static TileRef refine_start(const TsWater *w, const TileRef *shores, int n, const IntVec *route) {
    const int32_t CANDIDATE_RADIUS = 20, MIN_WAYPOINT = 50, MAX_WAYPOINT = 200, PADDING = 10, MAX_AREA = 100 * 100;
    const int32_t FW = w->map->width, FH = w->map->height;
    TileRef best = route->data[0];
    if (route->len <= MIN_WAYPOINT) return best;
    const int32_t bx = best % FW, by = best / FW;

    Ctx *c = ctx_get(w);
    int32_t *cand = scratch_ints(c, 0, n);
    int nc = 0;
    for (int i = 0; i < n; i++) {
        if (iabs(shores[i] % FW - bx) + iabs(shores[i] / FW - by) <= CANDIDATE_RADIUS) cand[nc++] = shores[i];
    }
    TileRef result = best;
    if (nc > 1) {
        int32_t cminx = cand[0] % FW, cmaxx = cminx, cminy = cand[0] / FW, cmaxy = cminy;
        for (int i = 1; i < nc; i++) {
            cminx = OFE_MIN(cminx, cand[i] % FW);
            cmaxx = OFE_MAX(cmaxx, cand[i] % FW);
            cminy = OFE_MIN(cminy, cand[i] / FW);
            cmaxy = OFE_MAX(cmaxy, cand[i] / FW);
        }
        /* Binary search for the furthest waypoint that keeps the window small. */
        int32_t lo = MIN_WAYPOINT, hi = OFE_MIN(MAX_WAYPOINT, route->len - 1), best_idx = lo;
        for (int i = 0; i < 5 && lo <= hi; i++) {
            int32_t mid = (lo + hi) >> 1, wx = route->data[mid] % FW, wy = route->data[mid] / FW;
            int32_t minx = OFE_MIN(cminx, wx) - PADDING, maxx = OFE_MAX(cmaxx, wx) + PADDING;
            int32_t miny = OFE_MIN(cminy, wy) - PADDING, maxy = OFE_MAX(cmaxy, wy) + PADDING;
            if ((maxx - minx + 1) * (maxy - miny + 1) <= MAX_AREA) {
                best_idx = mid;
                lo = mid + 1;
            } else {
                hi = mid - 1;
            }
        }
        TileRef wp = route->data[best_idx];
        int32_t wx = wp % FW, wy = wp / FW;
        Bounds bd = {OFE_MAX(0, OFE_MIN(cminx, wx) - PADDING), OFE_MIN(FW - 1, OFE_MAX(cmaxx, wx) + PADDING),
                     OFE_MAX(0, OFE_MIN(cminy, wy) - PADDING), OFE_MIN(FH - 1, OFE_MAX(cmaxy, wy) + PADDING)};
        if ((bd.max_x - bd.min_x + 1) * (bd.max_y - bd.min_y + 1) <= MAX_AREA) {
            Mini full = {FW, FH, w->map->terrain};
            if (bounded_search(&c->smooth, &full, cand, nc, wp, bd, &c->t0) && c->t0.len > 0) result = c->t0.data[0];
        }
    }
    ctx_put(w, c);
    return result;
}

TileRef ts_water_closest_shore(const TsWater *w, const TileRef *shores, int n, TileRef target, IntVec *route) {
    if (n <= 0 || !ts_water_find_path(w, shores, n, target, route) || route->len == 0) return -1;
    return refine_start(w, shores, n, route);
}

/* ---- PathFinderStepper --------------------------------------------------------- */

void ts_water_stepper_init(TsWaterStepper *s) { memset(s, 0, sizeof(*s)); }
void ts_water_stepper_free(TsWaterStepper *s) {
    IntVec_free(&s->path);
    memset(s, 0, sizeof(*s));
}
void ts_water_stepper_copy(TsWaterStepper *dst, const TsWaterStepper *src) {
    IntVec_copy(&dst->path, &src->path);
    dst->index = src->index;
    dst->last_to = src->last_to;
    dst->has_path = src->has_path;
    dst->has_last_to = src->has_last_to;
}
void ts_water_stepper_invalidate(TsWaterStepper *s) {
    s->has_path = false;
    s->index = 0;
    s->has_last_to = false;
}

TsPathStatus ts_water_next(const TsWater *w, TsWaterStepper *s, TileRef from, TileRef to, int32_t dist, TileRef *node) {
    *node = -1;
    if (!valid_ref(w, from) || !valid_ref(w, to)) return TS_PATH_NOT_FOUND; /* preCheck */
    if (from == to) {
        *node = to;
        return TS_PATH_COMPLETE;
    }
    const int32_t W = w->map->width;
    if (dist > 0 && iabs(from % W - to % W) + iabs(from / W - to / W) <= dist) {
        *node = from;
        return TS_PATH_COMPLETE;
    }
    for (;;) {
        if (!s->has_last_to || s->last_to != to) { /* destination changed: drop the route */
            s->has_path = false;
            s->index = 0;
            s->last_to = to;
            s->has_last_to = true;
        }
        if (!s->has_path) {
            if (!ts_water_find_path(w, &from, 1, to, &s->path)) return TS_PATH_NOT_FOUND;
            s->has_path = true;
            s->index = 0;
            if (s->path.len > 0 && s->path.data[0] == from) s->index = 1;
        }
        if (s->index > 0 && from != s->path.data[s->index - 1]) { /* off the route */
            ts_water_stepper_invalidate(s);
            s->last_to = to;
            s->has_last_to = true;
            continue;
        }
        break;
    }
    if (s->index >= s->path.len) {
        *node = to;
        return TS_PATH_COMPLETE;
    }
    *node = s->path.data[s->index++];
    return TS_PATH_NEXT;
}

const int32_t *ts_water_stepper_rest(const TsWaterStepper *s, int32_t *len) {
    if (!s->has_path || s->index == 0) {
        *len = 0;
        return NULL;
    }
    *len = s->path.len - (s->index - 1);
    return s->path.data + (s->index - 1);
}


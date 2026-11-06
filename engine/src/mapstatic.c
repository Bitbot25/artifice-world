/*
 * mapstatic.c - per-map static analysis for the critic's feature extractor.
 *
 * A pure function of the terrain bytes (see ofe_mapstatic.h and
 * docs/mapstatic.md for the exact definitions). Stages:
 *
 *   1. full-res labels: terrain class, landmass and water-body components
 *   2. coarse grid: tile counts, cell costs, majority labels, step passability
 *   3. exact Euclidean distance transforms (Felzenszwalb-Huttenlocher),
 *      pooled per coarse cell
 *   4. patches (OFE_PATCH_K x OFE_PATCH_K blocks of cells), representatives,
 *      coastal anchors
 *   5. all-pairs shortest paths between patch representatives (Dijkstra per
 *      source, OpenMP over sources; every row is independent so the result
 *      does not depend on the thread count)
 */
#include <math.h>
#include <string.h>

#include "../include/ofe_mapstatic.h"
#include "internal.h"

_Static_assert((int)OFE_TC_OCEAN == (int)TC_OCEAN && (int)OFE_TC_LAKE == (int)TC_LAKE && (int)OFE_TC_PLAINS == (int)TC_PLAINS &&
                   (int)OFE_TC_HIGHLAND == (int)TC_HIGHLAND && (int)OFE_TC_MOUNTAIN == (int)TC_MOUNTAIN &&
                   (int)OFE_TC_IMPASSABLE == (int)TC_IMPASSABLE,
               "terrain class numbering must match the engine");

#define F16_INF 0x7C00u

/* ---- float16 -------------------------------------------------------------- */

uint16_t ofe_mapstatic_f32_to_f16(float v) {
    union { float f; uint32_t u; } x = {v};
    uint32_t sign = (x.u >> 16) & 0x8000u;
    uint32_t abs = x.u & 0x7fffffffu;
    if (abs >= 0x7f800000u) return (uint16_t)(sign | (abs > 0x7f800000u ? 0x7e00u : F16_INF));
    if (abs >= 0x477fe000u) return (uint16_t)(sign | 0x7bffu); /* clamp to 65504 */
    if (abs < 0x38800000u) { /* subnormal half or zero */
        if (abs < 0x33000000u) return (uint16_t)sign;
        uint32_t mant = (abs & 0x7fffffu) | 0x800000u;
        int shift = 126 - (int)(abs >> 23);
        uint32_t half = mant >> shift, rem = mant & ((1u << shift) - 1), mid = 1u << (shift - 1);
        if (rem > mid || (rem == mid && (half & 1))) half++;
        return (uint16_t)(sign | half);
    }
    uint32_t half = ((abs >> 13) - (112u << 10)); /* rebias exponent */
    uint32_t rem = abs & 0x1fffu;
    if (rem > 0x1000u || (rem == 0x1000u && (half & 1))) half++;
    if (half > 0x7bffu) half = 0x7bffu;
    return (uint16_t)(sign | half);
}

float ofe_mapstatic_f16_to_f32(uint16_t h) {
    uint32_t sign = (uint32_t)(h & 0x8000u) << 16, exp = (h >> 10) & 0x1f, mant = h & 0x3ffu;
    union { uint32_t u; float f; } x;
    if (exp == 0) {
        float f = (float)mant * (1.0f / 16777216.0f); /* 2^-24 */
        return sign ? -f : f;
    }
    if (exp == 31) x.u = sign | 0x7f800000u | (mant << 13);
    else x.u = sign | ((exp + 112) << 23) | (mant << 13);
    return x.f;
}

float ofe_mapstatic_attack_cost(int tclass) {
    switch (tclass) {
    case OFE_TC_PLAINS: return OFE_COST_PLAINS;
    case OFE_TC_HIGHLAND: return OFE_COST_HIGHLAND;
    case OFE_TC_MOUNTAIN: return OFE_COST_MOUNTAIN;
    default: return INFINITY;
    }
}

static inline bool is_land(uint8_t c) { return c >= OFE_TC_PLAINS && c <= OFE_TC_MOUNTAIN; } /* ownable */
static inline bool is_water(uint8_t c) { return c <= OFE_TC_LAKE; }

/* ---- 1. components ------------------------------------------------------- */

static int32_t uf_find(int32_t *p, int32_t a) {
    while (p[a] != a) {
        p[a] = p[p[a]];
        a = p[a];
    }
    return a;
}

static void uf_union(int32_t *p, int32_t a, int32_t b) {
    a = uf_find(p, a);
    b = uf_find(p, b);
    if (a < b) p[b] = a;
    else if (b < a) p[a] = b;
}

/* 4-connected components of tiles whose group (group[tclass], 0 = none) is
 * equal. Labels 1..n are assigned in raster order of each component's first
 * tile. Returns n. */
static int32_t label_components(const uint8_t *tclass, int32_t w, int32_t h, const uint8_t group[OFE_TC_COUNT],
                                int32_t *out) {
    int32_t n = w * h;
    int32_t *p = (int32_t *)ofe_xmalloc((size_t)n * sizeof(int32_t));
    for (int32_t t = 0; t < n; t++) p[t] = t;
    for (int32_t y = 0; y < h; y++) {
        for (int32_t x = 0; x < w; x++) {
            int32_t t = y * w + x;
            uint8_t g = group[tclass[t]];
            if (!g) continue;
            if (x > 0 && group[tclass[t - 1]] == g) uf_union(p, t, t - 1);
            if (y > 0 && group[tclass[t - w]] == g) uf_union(p, t, t - w);
        }
    }
    int32_t *lab = (int32_t *)ofe_xcalloc((size_t)n, sizeof(int32_t));
    int32_t count = 0;
    for (int32_t t = 0; t < n; t++) {
        if (!group[tclass[t]]) { out[t] = 0; continue; }
        int32_t r = uf_find(p, t);
        if (!lab[r]) lab[r] = ++count;
        out[t] = lab[r];
    }
    free(lab);
    free(p);
    return count;
}

/* ---- 2. coarse grid ------------------------------------------------------ */

typedef struct Grid {
    int32_t w, h, s, cw, ch;
} Grid;

static inline void cell_range(const Grid *g, int32_t cx, int32_t cy, int32_t *x0, int32_t *x1, int32_t *y0,
                              int32_t *y1) {
    *x0 = cx * g->s;
    *x1 = OFE_MIN(g->w, *x0 + g->s);
    *y0 = cy * g->s;
    *y1 = OFE_MIN(g->h, *y0 + g->s);
}

static int cmp_i32(const void *a, const void *b) {
    int32_t x = *(const int32_t *)a, y = *(const int32_t *)b;
    return (x > y) - (x < y);
}

/* Majority label of labels[0..n) (all nonzero), ties -> smallest; *distinct set. */
static int32_t majority(int32_t *labels, int32_t n, int32_t *distinct) {
    *distinct = 0;
    if (n == 0) return 0;
    qsort(labels, (size_t)n, sizeof(int32_t), cmp_i32);
    int32_t best = labels[0], best_n = 0, run = 0;
    for (int32_t i = 0; i < n; i++) {
        if (i == 0 || labels[i] != labels[i - 1]) {
            (*distinct)++;
            run = 0;
        }
        run++;
        if (run > best_n) { best_n = run; best = labels[i]; }
    }
    return best;
}

/* Whether two 4-adjacent tiles are "the same type" for passability. */
static inline bool same_land(uint8_t a, uint8_t b) { return is_land(a) && is_land(b); }
static inline bool same_water(uint8_t a, uint8_t b) { return is_water(a) && a == b; }

static void coarse_stats(OfeMapStatic *ms, const Grid *g) {
    const int32_t w = g->w, ncell = g->cw * g->ch;
    const uint8_t *tc = ms->tclass;
#pragma omp parallel
    {
        int32_t *tmp = (int32_t *)ofe_xmalloc((size_t)g->s * g->s * sizeof(int32_t));
#pragma omp for schedule(static)
        for (int32_t c = 0; c < ncell; c++) {
            int32_t cx = c % g->cw, cy = c / g->cw, x0, x1, y0, y1;
            cell_range(g, cx, cy, &x0, &x1, &y0, &y1);
            int32_t area = (x1 - x0) * (y1 - y0), nl = 0, nw = 0;
            double cost_sum = 0;
            for (int32_t y = y0; y < y1; y++)
                for (int32_t x = x0; x < x1; x++) {
                    uint8_t k = tc[y * w + x];
                    if (is_land(k)) { nl++; cost_sum += ofe_mapstatic_attack_cost(k); }
                    else if (is_water(k)) nw++;
                }
            ms->cell_land_tiles[c] = nl;
            ms->cell_water_tiles[c] = nw;
            ms->land_cost[c] = nl ? (float)(cost_sum / nl) : INFINITY;
            ms->water_cost[c] = nw ? (float)area / (float)nw : INFINITY;

            int32_t n = 0, distinct;
            for (int32_t y = y0; y < y1; y++)
                for (int32_t x = x0; x < x1; x++)
                    if (ms->landmass[y * w + x]) tmp[n++] = ms->landmass[y * w + x];
            ms->cell_landmass[c] = majority(tmp, n, &distinct);
            ms->multi_landmass[c] = distinct > 1;
            n = 0;
            for (int32_t y = y0; y < y1; y++)
                for (int32_t x = x0; x < x1; x++)
                    if (ms->water_body[y * w + x]) tmp[n++] = ms->water_body[y * w + x];
            ms->cell_water_body[c] = majority(tmp, n, &distinct);
            ms->multi_water[c] = distinct > 1;

            /* Step passability: [0]=left [1]=right [2]=up [3]=down neighbour. */
            uint8_t ls[4] = {0, 0, 0, 0}, ws[4] = {0, 0, 0, 0};
            if (cx > 0)
                for (int32_t y = y0; y < y1; y++) {
                    uint8_t a = tc[y * w + x0], b = tc[y * w + x0 - 1];
                    ls[0] |= same_land(a, b); ws[0] |= same_water(a, b);
                }
            if (cx < g->cw - 1)
                for (int32_t y = y0; y < y1; y++) {
                    uint8_t a = tc[y * w + x1 - 1], b = tc[y * w + x1];
                    ls[1] |= same_land(a, b); ws[1] |= same_water(a, b);
                }
            if (cy > 0)
                for (int32_t x = x0; x < x1; x++) {
                    uint8_t a = tc[y0 * w + x], b = tc[(y0 - 1) * w + x];
                    ls[2] |= same_land(a, b); ws[2] |= same_water(a, b);
                }
            if (cy < g->ch - 1)
                for (int32_t x = x0; x < x1; x++) {
                    uint8_t a = tc[(y1 - 1) * w + x], b = tc[y1 * w + x];
                    ls[3] |= same_land(a, b); ws[3] |= same_water(a, b);
                }
            for (int k = 0; k < 4; k++) {
                ms->land_step[k][c] = ls[k];
                ms->water_step[k][c] = ws[k];
            }
        }
        free(tmp);
    }
}

/* ---- 3. distance transforms ---------------------------------------------- */

#define EDT_NONE 0x3fffffff /* "no source anywhere in the line / map" */

/* Squared Euclidean distance to the nearest source tile (source[tclass] != 0),
 * written to d2 (EDT_NONE if the map has no source). Exact: row pass by
 * sweeps, column pass by the Felzenszwalb-Huttenlocher lower envelope. */
static void edt_squared(const uint8_t *tclass, int32_t w, int32_t h, const uint8_t source[OFE_TC_COUNT],
                        int32_t *d2) {
#pragma omp parallel for schedule(static)
    for (int32_t y = 0; y < h; y++) {
        int32_t *row = d2 + (size_t)y * w;
        const uint8_t *tr = tclass + (size_t)y * w;
        int32_t last = -1;
        for (int32_t x = 0; x < w; x++) { /* forward: distance to source at or left of x */
            if (source[tr[x]]) last = x;
            row[x] = last < 0 ? EDT_NONE : x - last;
        }
        last = -1;
        for (int32_t x = w - 1; x >= 0; x--) { /* backward */
            if (source[tr[x]]) last = x;
            if (last >= 0 && last - x < row[x]) row[x] = last - x;
        }
        for (int32_t x = 0; x < w; x++) row[x] = row[x] == EDT_NONE ? EDT_NONE : row[x] * row[x];
    }
#pragma omp parallel
    {
        int32_t *v = (int32_t *)ofe_xmalloc((size_t)h * sizeof(int32_t));
        double *z = (double *)ofe_xmalloc((size_t)(h + 1) * sizeof(double));
        int32_t *f = (int32_t *)ofe_xmalloc((size_t)h * sizeof(int32_t));
#pragma omp for schedule(static)
        for (int32_t x = 0; x < w; x++) {
            int32_t k = -1;
            for (int32_t q = 0; q < h; q++) {
                f[q] = d2[(size_t)q * w + x];
                if (f[q] == EDT_NONE) continue;
                if (k < 0) { k = 0; v[0] = q; z[0] = -1e30; z[1] = 1e30; continue; }
                double s;
                for (;;) {
                    int32_t p = v[k];
                    s = (((double)f[q] + (double)q * q) - ((double)f[p] + (double)p * p)) / (2.0 * (q - p));
                    if (s <= z[k] && k > 0) k--;
                    else break;
                }
                k++;
                v[k] = q;
                z[k] = s;
                z[k + 1] = 1e30;
            }
            if (k < 0) continue; /* column stays EDT_NONE */
            int32_t j = 0;
            for (int32_t q = 0; q < h; q++) {
                while (z[j + 1] < q) j++;
                int64_t dq = q - v[j];
                d2[(size_t)q * w + x] = (int32_t)(dq * dq + f[v[j]]);
            }
        }
        free(v);
        free(z);
        free(f);
    }
}

/* Pools sqrt(d2) over tiles whose class is in `domain` into per-cell min/mean
 * (+inf where the cell has no domain tile or the map has no source). */
static void pool_distance(const Grid *g, const uint8_t *tclass, const int32_t *d2, const uint8_t domain[OFE_TC_COUNT],
                          float *out_min, float *out_mean) {
    const int32_t ncell = g->cw * g->ch;
#pragma omp parallel for schedule(static)
    for (int32_t c = 0; c < ncell; c++) {
        int32_t x0, x1, y0, y1;
        cell_range(g, c % g->cw, c / g->cw, &x0, &x1, &y0, &y1);
        double sum = 0;
        int32_t n = 0, mn = EDT_NONE;
        bool none = false;
        for (int32_t y = y0; y < y1 && !none; y++)
            for (int32_t x = x0; x < x1; x++) {
                if (!domain[tclass[(size_t)y * g->w + x]]) continue;
                int32_t d = d2[(size_t)y * g->w + x];
                if (d == EDT_NONE) { none = true; break; }
                sum += sqrt((double)d);
                if (d < mn) mn = d;
                n++;
            }
        if (none || n == 0) { out_min[c] = out_mean[c] = INFINITY; continue; }
        out_min[c] = (float)sqrt((double)mn);
        out_mean[c] = (float)(sum / n);
    }
}

static void distance_features(OfeMapStatic *ms, const Grid *g) {
    int32_t *d2 = (int32_t *)ofe_xmalloc((size_t)g->w * g->h * sizeof(int32_t));
    static const uint8_t src_water[OFE_TC_COUNT] = {1, 1, 0, 0, 0, 0};
    static const uint8_t src_land[OFE_TC_COUNT] = {0, 0, 1, 1, 1, 1}; /* any land incl. impassable */
    static const uint8_t src_imp[OFE_TC_COUNT] = {0, 0, 0, 0, 0, 1};
    static const uint8_t dom_land[OFE_TC_COUNT] = {0, 0, 1, 1, 1, 0};
    static const uint8_t dom_water[OFE_TC_COUNT] = {1, 1, 0, 0, 0, 0};
    edt_squared(ms->tclass, g->w, g->h, src_water, d2);
    pool_distance(g, ms->tclass, d2, dom_land, ms->coast_land_min, ms->coast_land_mean);
    edt_squared(ms->tclass, g->w, g->h, src_land, d2);
    pool_distance(g, ms->tclass, d2, dom_water, ms->coast_water_min, ms->coast_water_mean);
    edt_squared(ms->tclass, g->w, g->h, src_imp, d2);
    pool_distance(g, ms->tclass, d2, dom_land, ms->imp_min, ms->imp_mean);
    free(d2);
}

/* ---- 4. patches ---------------------------------------------------------- */

/* Representative of patch (bx, by) among cells with finite cost: nearest (in
 * cell coordinates) to the mean of those cells, ties -> lowest cell index. */
static int32_t patch_rep(const Grid *g, int32_t k, int32_t bx, int32_t by, const float *cost) {
    int32_t cx0 = bx * k, cx1 = OFE_MIN(g->cw, cx0 + k), cy0 = by * k, cy1 = OFE_MIN(g->ch, cy0 + k);
    double sx = 0, sy = 0;
    int32_t n = 0;
    for (int32_t cy = cy0; cy < cy1; cy++)
        for (int32_t cx = cx0; cx < cx1; cx++)
            if (isfinite(cost[cy * g->cw + cx])) { sx += cx; sy += cy; n++; }
    if (!n) return -1;
    sx /= n;
    sy /= n;
    int32_t best = -1;
    double bd = 0;
    for (int32_t cy = cy0; cy < cy1; cy++)
        for (int32_t cx = cx0; cx < cx1; cx++) {
            if (!isfinite(cost[cy * g->cw + cx])) continue;
            double d = (cx - sx) * (cx - sx) + (cy - sy) * (cy - sy);
            if (best < 0 || d < bd) { best = cy * g->cw + cx; bd = d; }
        }
    return best;
}

/* Coastal patch: contains an ownable land tile 4-adjacent to an ocean tile.
 * Anchor = among the cells holding those ocean neighbour tiles, the one whose
 * centre is nearest the centroid of the patch's coastal land tiles. */
static void coastal_patch_anchor(const OfeMapStatic *ms, const Grid *g, int32_t k, int32_t bx, int32_t by,
                                 uint8_t *coastal, int32_t *anchor) {
    const uint8_t *tc = ms->tclass;
    int32_t x0 = bx * k * g->s, x1 = OFE_MIN(g->w, (bx + 1) * k * g->s);
    int32_t y0 = by * k * g->s, y1 = OFE_MIN(g->h, (by + 1) * k * g->s);
    static const int dx[4] = {-1, 1, 0, 0}, dy[4] = {0, 0, -1, 1};
    double sx = 0, sy = 0;
    int32_t n = 0;
    *coastal = 0;
    *anchor = -1;
    for (int pass = 0; pass < 2; pass++) {
        double best_d = 0;
        for (int32_t y = y0; y < y1; y++)
            for (int32_t x = x0; x < x1; x++) {
                if (!is_land(tc[(size_t)y * g->w + x])) continue;
                bool shore = false;
                for (int d = 0; d < 4; d++) {
                    int32_t xx = x + dx[d], yy = y + dy[d];
                    if (xx < 0 || yy < 0 || xx >= g->w || yy >= g->h) continue;
                    if (tc[(size_t)yy * g->w + xx] != OFE_TC_OCEAN) continue;
                    shore = true;
                    if (pass == 1) {
                        int32_t cell = (yy / g->s) * g->cw + xx / g->s;
                        double ccx = ((cell % g->cw) + 0.5) * g->s, ccy = ((cell / g->cw) + 0.5) * g->s;
                        double dd = (ccx - sx) * (ccx - sx) + (ccy - sy) * (ccy - sy);
                        if (*anchor < 0 || dd < best_d || (dd == best_d && cell < *anchor)) {
                            *anchor = cell;
                            best_d = dd;
                        }
                    }
                }
                if (shore && pass == 0) { sx += x + 0.5; sy += y + 0.5; n++; }
            }
        if (pass == 0) {
            if (!n) return;
            sx /= n;
            sy /= n;
            *coastal = 1;
        }
    }
}

/* ---- 5. all-pairs shortest paths ----------------------------------------- */

typedef struct MsHeapItem {
    float d;
    int32_t c;
} MsHeapItem;

static inline bool ms_heap_less(MsHeapItem a, MsHeapItem b) { return a.d < b.d || (a.d == b.d && a.c < b.c); }

static void ms_heap_push(MsHeapItem *h, int32_t *n, MsHeapItem it) {
    int32_t i = (*n)++;
    while (i > 0) {
        int32_t p = (i - 1) / 2;
        if (!ms_heap_less(it, h[p])) break;
        h[i] = h[p];
        i = p;
    }
    h[i] = it;
}

static MsHeapItem ms_heap_pop(MsHeapItem *h, int32_t *n) {
    MsHeapItem top = h[0], last = h[--(*n)];
    int32_t i = 0;
    for (;;) {
        int32_t l = 2 * i + 1;
        if (l >= *n) break;
        if (l + 1 < *n && ms_heap_less(h[l + 1], h[l])) l++;
        if (!ms_heap_less(h[l], last)) break;
        h[i] = h[l];
        i = l;
    }
    if (*n > 0) h[i] = last;
    return top;
}

/* A coarse graph: node weight half[c] (edge a-b costs half[a] + half[b]),
 * edges where step[dir][c] is set (dir: left,right,up,down). */
typedef struct Graph {
    int32_t cw, ch;
    const float *half;
    uint8_t *const *step;
} Graph;

/* Shortest distances from src until every cell flagged in is_target is settled
 * (n_targets of them) or the component is exhausted. */
static void dijkstra(const Graph *G, int32_t src, const uint8_t *is_target, int32_t n_targets, float *dist,
                     MsHeapItem *heap) {
    const int32_t ncell = G->cw * G->ch;
    for (int32_t i = 0; i < ncell; i++) dist[i] = INFINITY;
    int32_t hn = 0, remaining = n_targets;
    dist[src] = 0;
    ms_heap_push(heap, &hn, (MsHeapItem){0.0f, src});
    while (hn > 0 && remaining > 0) {
        MsHeapItem it = ms_heap_pop(heap, &hn);
        if (it.d > dist[it.c]) continue;
        int32_t c = it.c;
        if (is_target[c]) remaining--;
        int32_t nb[4] = {c - 1, c + 1, c - G->cw, c + G->cw};
        for (int k = 0; k < 4; k++) {
            if (!G->step[k][c]) continue;
            float nd = it.d + (G->half[c] + G->half[nb[k]]);
            if (nd < dist[nb[k]]) {
                dist[nb[k]] = nd;
                ms_heap_push(heap, &hn, (MsHeapItem){nd, nb[k]});
            }
        }
    }
}

/* table[i*n + j] = half(dist(rep[i] -> rep[j])); rows/cols of absent patches inf. */
static uint16_t *all_pairs(const Graph *G, const int32_t *rep, int32_t n) {
    uint16_t *table = (uint16_t *)ofe_xmalloc((size_t)n * n * sizeof(uint16_t));
    for (size_t i = 0; i < (size_t)n * n; i++) table[i] = F16_INF;
    const int32_t ncell = G->cw * G->ch;
    uint8_t *is_target = (uint8_t *)ofe_xcalloc((size_t)ncell, 1);
    int32_t n_targets = 0;
    for (int32_t j = 0; j < n; j++)
        if (rep[j] >= 0 && !is_target[rep[j]]) { is_target[rep[j]] = 1; n_targets++; }
#pragma omp parallel
    {
        float *dist = (float *)ofe_xmalloc((size_t)ncell * sizeof(float));
        MsHeapItem *heap = (MsHeapItem *)ofe_xmalloc((size_t)(4 * ncell + 1) * sizeof(MsHeapItem));
#pragma omp for schedule(dynamic, 4)
        for (int32_t i = 0; i < n; i++) {
            if (rep[i] < 0) continue;
            dijkstra(G, rep[i], is_target, n_targets, dist, heap);
            for (int32_t j = 0; j < n; j++)
                if (rep[j] >= 0) table[(size_t)i * n + j] = ofe_mapstatic_f32_to_f16(dist[rep[j]]);
        }
        free(dist);
        free(heap);
    }
    free(is_target);
    return table;
}

/* ---- driver and registry ------------------------------------------------- */

static void reg(OfeMapStatic *ms, const char *name, int dtype, int ndim, int64_t d0, int64_t d1, int64_t d2,
                const void *data) {
    int32_t i = ms->num_arrays++;
    ms->arrays[i].name = name;
    ms->arrays[i].dtype = dtype;
    ms->arrays[i].ndim = ndim;
    ms->arrays[i].shape[0] = d0;
    ms->arrays[i].shape[1] = d1;
    ms->arrays[i].shape[2] = d2;
    ms->arrays[i].data = data;
}

static void register_all(OfeMapStatic *ms) {
    const int64_t H = ms->height, W = ms->width, CH = ms->ch, CW = ms->cw, P = ms->num_patches;
    reg(ms, "version", OFE_MS_I32, 0, 0, 0, 0, &ms->version);
    reg(ms, "width", OFE_MS_I32, 0, 0, 0, 0, &ms->width);
    reg(ms, "height", OFE_MS_I32, 0, 0, 0, 0, &ms->height);
    reg(ms, "stride", OFE_MS_I32, 0, 0, 0, 0, &ms->stride);
    reg(ms, "cw", OFE_MS_I32, 0, 0, 0, 0, &ms->cw);
    reg(ms, "ch", OFE_MS_I32, 0, 0, 0, 0, &ms->ch);
    reg(ms, "patch_k", OFE_MS_I32, 0, 0, 0, 0, &ms->patch_k);
    reg(ms, "pw", OFE_MS_I32, 0, 0, 0, 0, &ms->pw);
    reg(ms, "ph", OFE_MS_I32, 0, 0, 0, 0, &ms->ph);
    reg(ms, "num_patches", OFE_MS_I32, 0, 0, 0, 0, &ms->num_patches);
    reg(ms, "num_landmasses", OFE_MS_I32, 0, 0, 0, 0, &ms->num_landmasses);
    reg(ms, "num_water_bodies", OFE_MS_I32, 0, 0, 0, 0, &ms->num_water_bodies);
    reg(ms, "tclass", OFE_MS_U8, 2, H, W, 0, ms->tclass);
    reg(ms, "shoreline", OFE_MS_U8, 2, H, W, 0, ms->shoreline);
    reg(ms, "magnitude", OFE_MS_U8, 2, H, W, 0, ms->magnitude);
    reg(ms, "landmass", OFE_MS_I32, 2, H, W, 0, ms->landmass);
    reg(ms, "water_body", OFE_MS_I32, 2, H, W, 0, ms->water_body);
    reg(ms, "water_body_is_ocean", OFE_MS_U8, 1, ms->num_water_bodies + 1, 0, 0, ms->water_body_is_ocean);
    reg(ms, "cell_land_tiles", OFE_MS_I32, 2, CH, CW, 0, ms->cell_land_tiles);
    reg(ms, "cell_water_tiles", OFE_MS_I32, 2, CH, CW, 0, ms->cell_water_tiles);
    reg(ms, "land_cost", OFE_MS_F32, 2, CH, CW, 0, ms->land_cost);
    reg(ms, "water_cost", OFE_MS_F32, 2, CH, CW, 0, ms->water_cost);
    static const char *const dirs[4] = {"from_left", "from_right", "from_up", "from_down"};
    static const char *const names[2][4] = {{"land_from_left", "land_from_right", "land_from_up", "land_from_down"},
                                            {"water_from_left", "water_from_right", "water_from_up",
                                             "water_from_down"}};
    (void)dirs;
    for (int k = 0; k < 4; k++) {
        reg(ms, names[0][k], OFE_MS_U8, 2, CH, CW, 0, ms->land_step[k]);
        reg(ms, names[1][k], OFE_MS_U8, 2, CH, CW, 0, ms->water_step[k]);
    }
    reg(ms, "multi_landmass", OFE_MS_U8, 2, CH, CW, 0, ms->multi_landmass);
    reg(ms, "multi_water", OFE_MS_U8, 2, CH, CW, 0, ms->multi_water);
    reg(ms, "cell_landmass", OFE_MS_I32, 2, CH, CW, 0, ms->cell_landmass);
    reg(ms, "cell_water_body", OFE_MS_I32, 2, CH, CW, 0, ms->cell_water_body);
    reg(ms, "coast_land_min", OFE_MS_F32, 2, CH, CW, 0, ms->coast_land_min);
    reg(ms, "coast_land_mean", OFE_MS_F32, 2, CH, CW, 0, ms->coast_land_mean);
    reg(ms, "coast_water_min", OFE_MS_F32, 2, CH, CW, 0, ms->coast_water_min);
    reg(ms, "coast_water_mean", OFE_MS_F32, 2, CH, CW, 0, ms->coast_water_mean);
    reg(ms, "imp_min", OFE_MS_F32, 2, CH, CW, 0, ms->imp_min);
    reg(ms, "imp_mean", OFE_MS_F32, 2, CH, CW, 0, ms->imp_mean);
    reg(ms, "land_patch_rep", OFE_MS_I32, 1, P, 0, 0, ms->land_patch_rep);
    reg(ms, "water_patch_rep", OFE_MS_I32, 1, P, 0, 0, ms->water_patch_rep);
    reg(ms, "coastal_patch", OFE_MS_U8, 1, P, 0, 0, ms->coastal_patch);
    reg(ms, "coastal_anchor", OFE_MS_I32, 1, P, 0, 0, ms->coastal_anchor);
    reg(ms, "land_dist", OFE_MS_F16, 2, P, P, 0, ms->land_dist);
    reg(ms, "water_dist", OFE_MS_F16, 2, P, P, 0, ms->water_dist);
    reg(ms, "coastal_dist", OFE_MS_F16, 2, P, P, 0, ms->coastal_dist);
}

OfeMapStatic *ofe_mapstatic_compute(const OfeMap *map) {
    if (!map || map->width <= 0 || map->height <= 0) return NULL;
    OfeMapStatic *ms = (OfeMapStatic *)ofe_xcalloc(1, sizeof(OfeMapStatic));
    const int32_t W = map->width, H = map->height, N = map->num_tiles;
    ms->version = OFE_MAPSTATIC_VERSION;
    ms->width = W;
    ms->height = H;
    int32_t s = 1, longest = OFE_MAX(W, H);
    while ((longest + s - 1) / s > OFE_COARSE_MAX) s++;
    ms->stride = s;
    ms->cw = (W + s - 1) / s;
    ms->ch = (H + s - 1) / s;
    ms->patch_k = OFE_PATCH_K;
    ms->pw = (ms->cw + OFE_PATCH_K - 1) / OFE_PATCH_K;
    ms->ph = (ms->ch + OFE_PATCH_K - 1) / OFE_PATCH_K;
    ms->num_patches = ms->pw * ms->ph;
    const Grid g = {W, H, s, ms->cw, ms->ch};
    const int32_t ncell = ms->cw * ms->ch, P = ms->num_patches;

    /* Full resolution. */
    ms->tclass = (uint8_t *)ofe_xmalloc((size_t)N);
    ms->shoreline = (uint8_t *)ofe_xmalloc((size_t)N);
    ms->magnitude = (uint8_t *)ofe_xmalloc((size_t)N);
    ms->landmass = (int32_t *)ofe_xmalloc((size_t)N * sizeof(int32_t));
    ms->water_body = (int32_t *)ofe_xmalloc((size_t)N * sizeof(int32_t));
    for (int32_t t = 0; t < N; t++) {
        uint8_t b = map->terrain[t];
        ms->tclass[t] = map->tclass[t];
        ms->shoreline[t] = (b & TERRAIN_SHORELINE_BIT) != 0;
        ms->magnitude[t] = b & TERRAIN_MAGNITUDE_MASK;
    }
    static const uint8_t grp_land[OFE_TC_COUNT] = {0, 0, 1, 1, 1, 0};
    static const uint8_t grp_water[OFE_TC_COUNT] = {1, 2, 0, 0, 0, 0}; /* ocean / lake never merge */
    ms->num_landmasses = label_components(ms->tclass, W, H, grp_land, ms->landmass);
    ms->num_water_bodies = label_components(ms->tclass, W, H, grp_water, ms->water_body);
    ms->water_body_is_ocean = (uint8_t *)ofe_xcalloc((size_t)ms->num_water_bodies + 1, 1);
    for (int32_t t = 0; t < N; t++)
        if (ms->water_body[t]) ms->water_body_is_ocean[ms->water_body[t]] = ms->tclass[t] == OFE_TC_OCEAN;

    /* Coarse. */
    ms->cell_land_tiles = (int32_t *)ofe_xmalloc((size_t)ncell * 4);
    ms->cell_water_tiles = (int32_t *)ofe_xmalloc((size_t)ncell * 4);
    ms->land_cost = (float *)ofe_xmalloc((size_t)ncell * 4);
    ms->water_cost = (float *)ofe_xmalloc((size_t)ncell * 4);
    for (int k = 0; k < 4; k++) {
        ms->land_step[k] = (uint8_t *)ofe_xmalloc((size_t)ncell);
        ms->water_step[k] = (uint8_t *)ofe_xmalloc((size_t)ncell);
    }
    ms->multi_landmass = (uint8_t *)ofe_xmalloc((size_t)ncell);
    ms->multi_water = (uint8_t *)ofe_xmalloc((size_t)ncell);
    ms->cell_landmass = (int32_t *)ofe_xmalloc((size_t)ncell * 4);
    ms->cell_water_body = (int32_t *)ofe_xmalloc((size_t)ncell * 4);
    float **fl[6] = {&ms->coast_land_min, &ms->coast_land_mean, &ms->coast_water_min,
                     &ms->coast_water_mean, &ms->imp_min, &ms->imp_mean};
    for (int i = 0; i < 6; i++) *fl[i] = (float *)ofe_xmalloc((size_t)ncell * 4);
    coarse_stats(ms, &g);
    distance_features(ms, &g);

    /* Patches. */
    ms->land_patch_rep = (int32_t *)ofe_xmalloc((size_t)P * 4);
    ms->water_patch_rep = (int32_t *)ofe_xmalloc((size_t)P * 4);
    ms->coastal_patch = (uint8_t *)ofe_xmalloc((size_t)P);
    ms->coastal_anchor = (int32_t *)ofe_xmalloc((size_t)P * 4);
#pragma omp parallel for schedule(dynamic, 8)
    for (int32_t b = 0; b < P; b++) {
        int32_t bx = b % ms->pw, by = b / ms->pw;
        ms->land_patch_rep[b] = patch_rep(&g, OFE_PATCH_K, bx, by, ms->land_cost);
        ms->water_patch_rep[b] = patch_rep(&g, OFE_PATCH_K, bx, by, ms->water_cost);
        coastal_patch_anchor(ms, &g, OFE_PATCH_K, bx, by, &ms->coastal_patch[b], &ms->coastal_anchor[b]);
    }

    /* Distance tables. Node weight = half an edge cost, so an edge a-b costs the
     * mean of the two cell costs times the stride (land: in plains-tile units). */
    float *half_land = (float *)ofe_xmalloc((size_t)ncell * 4);
    float *half_water = (float *)ofe_xmalloc((size_t)ncell * 4);
    for (int32_t c = 0; c < ncell; c++) {
        half_land[c] = ms->land_cost[c] * (0.5f * (float)s / OFE_COST_PLAINS);
        half_water[c] = ms->water_cost[c] * (0.5f * (float)s);
    }
    Graph lg = {ms->cw, ms->ch, half_land, ms->land_step};
    Graph wg = {ms->cw, ms->ch, half_water, ms->water_step};
    ms->land_dist = all_pairs(&lg, ms->land_patch_rep, P);
    ms->water_dist = all_pairs(&wg, ms->water_patch_rep, P);
    ms->coastal_dist = all_pairs(&wg, ms->coastal_anchor, P);
    free(half_land);
    free(half_water);

    register_all(ms);
    return ms;
}

void ofe_mapstatic_free(OfeMapStatic *ms) {
    if (!ms) return;
    free(ms->tclass); free(ms->shoreline); free(ms->magnitude);
    free(ms->landmass); free(ms->water_body); free(ms->water_body_is_ocean);
    free(ms->cell_land_tiles); free(ms->cell_water_tiles);
    free(ms->land_cost); free(ms->water_cost);
    for (int k = 0; k < 4; k++) { free(ms->land_step[k]); free(ms->water_step[k]); }
    free(ms->multi_landmass); free(ms->multi_water);
    free(ms->cell_landmass); free(ms->cell_water_body);
    free(ms->coast_land_min); free(ms->coast_land_mean);
    free(ms->coast_water_min); free(ms->coast_water_mean);
    free(ms->imp_min); free(ms->imp_mean);
    free(ms->land_patch_rep); free(ms->water_patch_rep);
    free(ms->coastal_patch); free(ms->coastal_anchor);
    free(ms->land_dist); free(ms->water_dist); free(ms->coastal_dist);
    free(ms);
}

int32_t ofe_mapstatic_num_arrays(const OfeMapStatic *ms) { return ms->num_arrays; }

int ofe_mapstatic_array_info(const OfeMapStatic *ms, int32_t i, const char **name, int *dtype, int *ndim,
                             int64_t shape[3], const void **data) {
    if (!ms || i < 0 || i >= ms->num_arrays) return -1;
    *name = ms->arrays[i].name;
    *dtype = ms->arrays[i].dtype;
    *ndim = ms->arrays[i].ndim;
    for (int k = 0; k < 3; k++) shape[k] = ms->arrays[i].shape[k];
    *data = ms->arrays[i].data;
    return 0;
}

const void *ofe_mapstatic_array(const OfeMapStatic *ms, const char *name) {
    for (int32_t i = 0; i < ms->num_arrays; i++)
        if (strcmp(ms->arrays[i].name, name) == 0) return ms->arrays[i].data;
    return NULL;
}

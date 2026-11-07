/*
 * paths_ts.c - OpenFront's missile, air and rail pathfinding, ported to give
 * the same tiles on the same ticks (TS-parity build only). See paths_ts.h for
 * the map of TS files to C types, and docs/ts_parity.md for the rules.
 *
 * Exactness notes that apply to the whole file:
 *   - JS numbers are doubles; every double expression here is written in the
 *     same order as the TS one (and the build uses -std=c99, which keeps
 *     fused multiply-add contraction off, so results are bit-identical).
 *   - Math.round(x) is floor(x + 0.5); `>> n` on an int32 is an arithmetic
 *     shift; `x | 0` wraps to int32.
 *   - Any ordering TS gets from a Set / Map (insertion order, deletion keeps
 *     the rest) is kept with ordered vectors.
 */
#if OFE_TS_PARITY

#include "paths_ts.h"
#include "internal.h"

/* ======================================================================== */
/* Terrain                                                                   */
/* ======================================================================== */

TsGrid ts_grid_from_map(const OfeMap *m) {
    TsGrid g = {m->width, m->height, m->terrain};
    return g;
}

uint8_t *ts_mini_terrain_load(const char *map_dir, int compact, int32_t game_w, int32_t game_h) {
    char path[1024];
    snprintf(path, sizeof(path), "%s/%s", map_dir, compact ? "map16x.bin" : "map4x.bin");
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    size_t n = (size_t)((game_w + 1) / 2) * (size_t)((game_h + 1) / 2);
    uint8_t *buf = (uint8_t *)ofe_xmalloc(n + 1);
    size_t got = fread(buf, 1, n, f);
    int extra = fgetc(f);
    fclose(f);
    if (got != n || extra != EOF) {
        free(buf);
        return NULL;
    }
    return buf;
}

static inline bool g_is_land(const TsGrid *g, int32_t t) { return (g->terrain[t] & 0x80) != 0; }
static inline bool g_is_water(const TsGrid *g, int32_t t) { return !g_is_land(g, t); }
static inline bool g_is_shoreline(const TsGrid *g, int32_t t) { return (g->terrain[t] & 0x40) != 0; }
static inline bool g_is_impassable(const TsGrid *g, int32_t t) {
    return g_is_land(g, t) && (g->terrain[t] & 0x1f) == 31;
}

/* Math.round for the values that occur here. */
static inline double js_round(double v) { return floor(v + 0.5); }

/* ======================================================================== */
/* PseudoRandom (sfc32, splitmix32 seeding)                                   */
/* ======================================================================== */

static inline uint32_t rotl32(uint32_t v, int n) { return (v << n) | (v >> (32 - n)); }

double ts_path_rng_next(TsPathRng *r) {
    uint32_t t = r->s0 + r->s1 + r->s3;
    r->s3 = r->s3 + 1;
    r->s0 = r->s1 ^ (r->s1 >> 9);
    r->s1 = r->s2 + (r->s2 << 3);
    r->s2 = rotl32(r->s2, 21);
    r->s2 = r->s2 + t;
    return (double)t / 4294967296.0;
}

void ts_path_rng_seed(TsPathRng *r, int64_t seed) {
    uint32_t h = (uint32_t)seed; /* seed | 0: truncated to 32 bits */
    uint32_t out[4];
    for (int i = 0; i < 4; i++) {
        h += 0x9e3779b9u;
        uint32_t t = h ^ (h >> 16);
        t *= 0x21f0aaadu;
        t ^= t >> 15;
        t *= 0x735a2d97u;
        out[i] = t ^ (t >> 15);
    }
    r->s0 = out[0];
    r->s1 = out[1];
    r->s2 = out[2];
    r->s3 = out[3];
    for (int i = 0; i < 12; i++) ts_path_rng_next(r); /* warm up */
}

int32_t ts_path_rng_next_int(TsPathRng *r, int32_t lo, int32_t hi) {
    return (int32_t)floor(ts_path_rng_next(r) * (double)(hi - lo)) + lo;
}

bool ts_path_rng_chance(TsPathRng *r, int32_t odds) { return ts_path_rng_next_int(r, 0, odds) == 0; }

/* ======================================================================== */
/* Parabola                                                                   */
/*                                                                            */
/* Follows PathFinder.Parabola.ts and DistanceBasedBezierCurve (Line.ts): the */
/* cubic bezier is subdivided in 24.8 fixed point (de Casteljau with >> 1),   */
/* the arc length is accumulated as floor(sqrt(dx^2 + dy^2)) per leaf, and a  */
/* point is stored each time the accumulated length passes the spacing. A     */
/* projectile then advances `speed * 256` sub-units per tick along the stored */
/* points, so its position is cached_points[k] where k counts how many        */
/* spacings the accumulated distance has crossed.                             */
/* ======================================================================== */

#define PARABOLA_MIN_HEIGHT 50
#define SUB_SCALE 256

TsParabolaOpts ts_parabola_default_opts(void) {
    TsParabolaOpts o = {3.0, true, true, false};
    return o;
}

void ts_parabola_controls(const TsGrid *map, int32_t from, int32_t to, const TsParabolaOpts *o, TsPt out[4]) {
    TsPt p0 = {(double)ts_grid_x(map, from), (double)ts_grid_y(map, from)};
    TsPt p3 = {(double)ts_grid_x(map, to), (double)ts_grid_y(map, to)};
    double dx = p3.x - p0.x;
    double dy = p3.y - p0.y;
    double distance = sqrt(dx * dx + dy * dy);
    double max_height = 0;
    if (o->distance_based_height) {
        double d3 = distance / 3;
        max_height = d3 > PARABOLA_MIN_HEIGHT ? d3 : PARABOLA_MIN_HEIGHT; /* Math.max */
    }
    double height_mult = o->direction_up ? -1 : 1;
    double map_height = (double)map->h;

    double p1y = p0.y + dy / 4 + height_mult * max_height;
    double p2y = p0.y + (dy * 3) / 4 + height_mult * max_height;

    TsPt p1 = {p0.x + dx / 4, p1y};
    TsPt p2 = {p0.x + (dx * 3) / 4, p2y};
    if (!o->ignore_map_bounds) { /* within(v, 0, h - 1) = min(max(v, 0), h - 1) */
        double hi = map_height - 1;
        p1.y = fmin(fmax(p1y, 0), hi);
        p2.y = fmin(fmax(p2y, 0), hi);
    }
    out[0] = p0;
    out[1] = p1;
    out[2] = p2;
    out[3] = p3;
}

typedef struct SubdivState {
    int32_t last_x, last_y;
    int64_t accum;
    int64_t threshold;  /* 0 = only measure the length */
    IntVec *px, *py;
} SubdivState;

static void subdivide(int32_t ax, int32_t ay, int32_t bx, int32_t by, int32_t cx, int32_t cy, int32_t dx, int32_t dy,
                      int depth, SubdivState *st) {
    int32_t dist = abs(bx - ax) + abs(by - ay) + abs(cx - bx) + abs(cy - by) + abs(dx - cx) + abs(dy - cy);
    if (dist <= 256 || depth >= 10) {
        int64_t edx = (int64_t)ax - st->last_x;
        int64_t edy = (int64_t)ay - st->last_y;
        st->accum += (int64_t)floor(sqrt((double)(edx * edx + edy * edy)));
        st->last_x = ax;
        st->last_y = ay;
        if (st->threshold > 0) {
            while (st->accum >= st->threshold) {
                IntVec_push(st->px, (ax + 128) >> 8);
                IntVec_push(st->py, (ay + 128) >> 8);
                st->accum -= st->threshold;
            }
        }
        return;
    }
    int32_t m01x = (ax + bx) >> 1, m01y = (ay + by) >> 1;
    int32_t m12x = (bx + cx) >> 1, m12y = (by + cy) >> 1;
    int32_t m23x = (cx + dx) >> 1, m23y = (cy + dy) >> 1;
    int32_t m012x = (m01x + m12x) >> 1, m012y = (m01y + m12y) >> 1;
    int32_t m123x = (m12x + m23x) >> 1, m123y = (m12y + m23y) >> 1;
    int32_t mx = (m012x + m123x) >> 1, my = (m012y + m123y) >> 1;
    subdivide(ax, ay, m01x, m01y, m012x, m012y, mx, my, depth + 1, st);
    subdivide(mx, my, m123x, m123y, m23x, m23y, dx, dy, depth + 1, st);
}

static void fixed_points(const TsPt p[4], int32_t f[8]) {
    for (int i = 0; i < 4; i++) {
        f[2 * i] = (int32_t)js_round(p[i].x) * SUB_SCALE;
        f[2 * i + 1] = (int32_t)js_round(p[i].y) * SUB_SCALE;
    }
}

double ts_bezier_length(const TsPt p[4]) {
    int32_t f[8];
    fixed_points(p, f);
    SubdivState st = {f[0], f[1], 0, 0, NULL, NULL};
    subdivide(f[0], f[1], f[2], f[3], f[4], f[5], f[6], f[7], 0, &st);
    int64_t edx = (int64_t)f[6] - st.last_x;
    int64_t edy = (int64_t)f[7] - st.last_y;
    st.accum += (int64_t)floor(sqrt((double)(edx * edx + edy * edy)));
    return (double)st.accum / SUB_SCALE;
}

/* computeAllPoints: the cached points of a curve with the given control points. */
static void curve_compute(const TsPt p[4], double pixel_spacing, IntVec *px, IntVec *py, int32_t *spacing_scaled) {
    px->len = py->len = 0;
    double sp = js_round(pixel_spacing * SUB_SCALE);
    *spacing_scaled = (int32_t)(sp > SUB_SCALE ? sp : SUB_SCALE);
    int32_t f[8];
    fixed_points(p, f);
    SubdivState st = {f[0], f[1], 0, *spacing_scaled, px, py};
    IntVec_push(px, (f[0] + 128) >> 8);
    IntVec_push(py, (f[1] + 128) >> 8);
    subdivide(f[0], f[1], f[2], f[3], f[4], f[5], f[6], f[7], 0, &st);
    int32_t lx = (f[6] + 128) >> 8, ly = (f[7] + 128) >> 8;
    if (px->data[px->len - 1] != lx || py->data[py->len - 1] != ly) {
        IntVec_push(px, lx);
        IntVec_push(py, ly);
    }
}

void ts_parabola_init(TsParabola *pf, const TsGrid *map, const TsParabolaOpts *opts) {
    memset(pf, 0, sizeof(*pf));
    pf->map = map;
    pf->opts = *opts;
    pf->last_to = -1;
}

void ts_parabola_free(TsParabola *pf) {
    IntVec_free(&pf->px);
    IntVec_free(&pf->py);
}

static void parabola_create_curve(TsParabola *pf, int32_t from, int32_t to) {
    TsPt p[4];
    ts_parabola_controls(pf->map, from, to, &pf->opts, p);
    curve_compute(p, pf->opts.increment, &pf->px, &pf->py, &pf->spacing_scaled);
    pf->current_index = 0;
    pf->accumulated_scaled = 0;
}

void ts_parabola_find_path(const TsGrid *map, const TsParabolaOpts *opts, int32_t from, int32_t to, IntVec *out) {
    TsParabola pf;
    ts_parabola_init(&pf, map, opts);
    parabola_create_curve(&pf, from, to);
    out->len = 0;
    for (int32_t i = 0; i < pf.px.len; i++) IntVec_push(out, pf.py.data[i] * map->w + pf.px.data[i]);
    ts_parabola_free(&pf);
}

TsPathResult ts_parabola_next(TsParabola *pf, int32_t from, int32_t to, double speed) {
    if (pf->last_to != to) {
        parabola_create_curve(pf, from, to);
        pf->last_to = to;
    }
    double step = js_round(speed * SUB_SCALE);
    pf->accumulated_scaled += step > 1 ? (int64_t)step : 1;
    while (pf->current_index < pf->px.len - 1 && pf->accumulated_scaled >= pf->spacing_scaled) {
        pf->current_index++;
        pf->accumulated_scaled -= pf->spacing_scaled;
    }
    TsPathResult r;
    if (pf->current_index >= pf->px.len - 1) {
        r.status = TS_PATH_COMPLETE;
        r.node = to;
        return r;
    }
    r.status = TS_PATH_NEXT;
    r.node = pf->py.data[pf->current_index] * pf->map->w + pf->px.data[pf->current_index];
    return r;
}

void ts_parabola_invalidate(TsParabola *pf) {
    pf->last_to = -1;
    pf->px.len = pf->py.len = 0;
}

int32_t ts_parabola_current_index(const TsParabola *pf) { return pf->current_index; }

double ts_mirv_speed(const TsGrid *map, int32_t spawn, int32_t separate_dst) {
    const double MATH_SCALE = 100;
    const double long_mult = 14, long_linear_percent = 10, short_mult = 10;
    const double base_speed = 15, base_ticks = 14;

    TsParabolaOpts ideal_o = {3.0, true, true, true};
    TsPt ip[4];
    ts_parabola_controls(map, spawn, separate_dst, &ideal_o, ip);
    double ideal_len = ts_bezier_length(ip);
    double ideal_ticks = ideal_len / base_speed;

    double base_scaled = base_ticks * MATH_SCALE;
    double ideal_int = floor(ideal_ticks * MATH_SCALE);
    double target = base_scaled;
    if (ideal_int > base_scaled) {
        double diff = ideal_int - base_scaled;
        target = base_scaled + floor(sqrt(diff)) * long_mult + floor((diff * long_linear_percent) / 100);
    } else if (ideal_int < base_scaled) {
        double diff = base_scaled - ideal_int;
        target = base_scaled - floor(sqrt(diff)) * short_mult;
    }
    target = fmax(MATH_SCALE, target);

    TsParabolaOpts actual_o = {3.0, true, true, false};
    TsPt ap[4];
    ts_parabola_controls(map, spawn, separate_dst, &actual_o, ap);
    double actual_len = ts_bezier_length(ap);
    double actual_int = js_round(actual_len * 256);
    double num = actual_int * MATH_SCALE;
    double den = target;
    double scaled = floor((num + floor(den / 2)) / den);
    return fmax(1, scaled / 256);
}

/* ======================================================================== */
/* Air pathfinder + PathFinderStepper                                         */
/*                                                                            */
/* AirPathFinder walks from `from` to `to` one orthogonal step at a time. When */
/* both coordinates differ it draws from PseudoRandom(seed) (fresh for every  */
/* query, so the path is a pure function of (seed, from, to)) and steps in x  */
/* with chance 1/ratio, ratio = floor(1 + |dy| / (|dx| + 1)), else in y.      */
/* The stepper caches the path, hands out one tile per call, and recomputes   */
/* from the current tile when the target changes or the caller is not where   */
/* the path expects it to be.                                                 */
/* ======================================================================== */

void ts_air_find_path(const TsGrid *map, int64_t seed, int32_t from, int32_t to, IntVec *out) {
    TsPathRng rng;
    ts_path_rng_seed(&rng, seed);
    out->len = 0;
    IntVec_push(out, from);
    int32_t cur = from;
    int32_t dst_x = ts_grid_x(map, to), dst_y = ts_grid_y(map, to);
    while (cur != to) {
        int32_t x = ts_grid_x(map, cur), y = ts_grid_y(map, cur);
        int32_t next;
        if (x == dst_x && y == dst_y) {
            next = to;
        } else {
            int32_t nx = x, ny = y;
            double ratio_d = floor(1 + (double)abs(dst_y - y) / (double)(abs(dst_x - x) + 1));
            int32_t ratio = (int32_t)ratio_d;
            if (x == dst_x) {
                ny += y < dst_y ? 1 : -1;
            } else if (y == dst_y) {
                nx += x < dst_x ? 1 : -1;
            } else if (ts_path_rng_chance(&rng, ratio)) {
                nx += x < dst_x ? 1 : -1;
            } else {
                ny += y < dst_y ? 1 : -1;
            }
            next = ny * map->w + nx;
        }
        if (next == cur) break;
        cur = next;
        IntVec_push(out, cur);
    }
}

void ts_air_init(TsAir *a, const TsGrid *map, int64_t seed) {
    memset(a, 0, sizeof(*a));
    a->map = map;
    a->seed = seed;
    a->last_to = -1;
}

void ts_air_free(TsAir *a) { IntVec_free(&a->path); }

static void air_invalidate(TsAir *a) {
    a->has_path = false;
    a->path_index = 0;
    a->last_to = -1;
}

TsPathResult ts_air_next(TsAir *a, int32_t from, int32_t to) {
    TsPathResult r;
    for (;;) {
        if (from == to) {
            r.status = TS_PATH_COMPLETE;
            r.node = to;
            return r;
        }
        if (a->last_to == -1 || a->last_to != to) {
            a->has_path = false;
            a->path_index = 0;
            a->last_to = to;
        }
        if (!a->has_path) {
            ts_air_find_path(a->map, a->seed, from, to, &a->path);
            a->has_path = true;
            a->path_index = 0;
            if (a->path.len > 0 && a->path.data[0] == from) a->path_index = 1;
        }
        if (a->path_index > 0 && from != a->path.data[a->path_index - 1]) {
            air_invalidate(a);
            a->last_to = to;
            continue; /* the TS code recurses */
        }
        if (a->path_index >= a->path.len) {
            r.status = TS_PATH_COMPLETE;
            r.node = to;
            return r;
        }
        r.status = TS_PATH_NEXT;
        r.node = a->path.data[a->path_index++];
        return r;
    }
}

/* ======================================================================== */
/* A* with a bucket queue                                                     */
/*                                                                            */
/* AStar.ts + BucketQueue (PriorityQueue.ts). The queue is LIFO inside a      */
/* priority bucket and priorities are truncated integers capped at the last   */
/* bucket, so which of several equal-cost paths A* returns depends on exactly */
/* this behaviour; it is reproduced rather than approximated. Scratch arrays  */
/* are stamped so a query costs only what it touches.                         */
/* ======================================================================== */

struct TsAStar {
    TsAStarAdapter ad;
    uint32_t stamp;
    uint32_t *closed_stamp, *gscore_stamp, *gscore;
    int32_t *came_from;
    int32_t *nbuf;
    /* bucket queue */
    int32_t max_bucket;
    int32_t **buckets;
    int32_t *bucket_cap, *bucket_size;
    uint32_t *bucket_stamp;
    uint32_t qstamp;
    int32_t min_bucket, qsize;
};

TsAStar *ts_astar_new(const TsAStarAdapter *adapter) {
    TsAStar *a = (TsAStar *)ofe_xcalloc(1, sizeof(TsAStar));
    a->ad = *adapter;
    size_t n = (size_t)adapter->num_nodes;
    a->closed_stamp = (uint32_t *)ofe_xcalloc(n, 4);
    a->gscore_stamp = (uint32_t *)ofe_xcalloc(n, 4);
    a->gscore = (uint32_t *)ofe_xcalloc(n, 4);
    a->came_from = (int32_t *)ofe_xcalloc(n, 4);
    a->nbuf = (int32_t *)ofe_xcalloc((size_t)adapter->max_neighbors + 1, 4);
    a->max_bucket = adapter->max_priority + 1;
    a->buckets = (int32_t **)ofe_xcalloc((size_t)a->max_bucket, sizeof(int32_t *));
    a->bucket_cap = (int32_t *)ofe_xcalloc((size_t)a->max_bucket, 4);
    a->bucket_size = (int32_t *)ofe_xcalloc((size_t)a->max_bucket, 4);
    a->bucket_stamp = (uint32_t *)ofe_xcalloc((size_t)a->max_bucket, 4);
    a->min_bucket = a->max_bucket;
    return a;
}

void ts_astar_free(TsAStar *a) {
    if (!a) return;
    free(a->closed_stamp);
    free(a->gscore_stamp);
    free(a->gscore);
    free(a->came_from);
    free(a->nbuf);
    for (int32_t i = 0; i < a->max_bucket; i++) free(a->buckets[i]);
    free(a->buckets);
    free(a->bucket_cap);
    free(a->bucket_size);
    free(a->bucket_stamp);
    free(a);
}

static void q_clear(TsAStar *a) {
    a->qstamp++;
    a->min_bucket = a->max_bucket;
    a->qsize = 0;
}

static void q_push(TsAStar *a, int32_t node, int32_t priority) {
    int32_t b = priority < a->max_bucket - 1 ? priority : a->max_bucket - 1;
    if (!a->buckets[b]) {
        a->buckets[b] = (int32_t *)ofe_xmalloc(64 * sizeof(int32_t));
        a->bucket_cap[b] = 64;
    }
    int32_t size = a->bucket_stamp[b] == a->qstamp ? a->bucket_size[b] : 0;
    if (size >= a->bucket_cap[b]) {
        a->bucket_cap[b] *= 2;
        a->buckets[b] = (int32_t *)ofe_xrealloc(a->buckets[b], (size_t)a->bucket_cap[b] * sizeof(int32_t));
    }
    a->buckets[b][size] = node;
    a->bucket_size[b] = size + 1;
    a->bucket_stamp[b] = a->qstamp;
    a->qsize++;
    if (b < a->min_bucket) a->min_bucket = b;
}

static int32_t q_pop(TsAStar *a) {
    while (a->min_bucket < a->max_bucket) {
        if (a->bucket_stamp[a->min_bucket] == a->qstamp) {
            int32_t size = a->bucket_size[a->min_bucket];
            if (size > 0) {
                a->bucket_size[a->min_bucket]--;
                a->qsize--;
                return a->buckets[a->min_bucket][size - 1];
            }
        }
        a->min_bucket++;
    }
    return -1;
}

bool ts_astar_find_path(TsAStar *a, int32_t start, int32_t goal, IntVec *out) {
    a->stamp++;
    uint32_t stamp = a->stamp;
    q_clear(a);
    a->gscore[start] = 0;
    a->gscore_stamp[start] = stamp;
    a->came_from[start] = -1;
    q_push(a, start, a->ad.heuristic(a->ad.ctx, start, goal));

    int32_t iterations = 500000;
    while (a->qsize > 0) {
        if (--iterations <= 0) return false;
        int32_t current = q_pop(a);
        if (a->closed_stamp[current] == stamp) continue;
        a->closed_stamp[current] = stamp;
        if (current == goal) {
            out->len = 0;
            for (int32_t c = goal; c != -1; c = a->came_from[c]) IntVec_push(out, c);
            for (int32_t i = 0, j = out->len - 1; i < j; i++, j--) {
                int32_t t = out->data[i];
                out->data[i] = out->data[j];
                out->data[j] = t;
            }
            return true;
        }
        uint32_t current_g = a->gscore[current];
        int32_t prev = a->came_from[current];
        int32_t count = a->ad.neighbors(a->ad.ctx, current, a->nbuf);
        for (int32_t i = 0; i < count; i++) {
            int32_t nb = a->nbuf[i];
            if (a->closed_stamp[nb] == stamp) continue;
            uint32_t tentative = current_g + (uint32_t)a->ad.cost(a->ad.ctx, current, nb, prev);
            if (a->gscore_stamp[nb] != stamp || tentative < a->gscore[nb]) {
                a->came_from[nb] = current;
                a->gscore[nb] = tentative;
                a->gscore_stamp[nb] = stamp;
                q_push(a, nb, (int32_t)tentative + a->ad.heuristic(a->ad.ctx, nb, goal));
            }
        }
    }
    return false;
}

/* ======================================================================== */
/* Rail pathfinder (AStar.Rail.ts + MiniMapTransformer.ts)                    */
/*                                                                            */
/* A* over the half-resolution map. Moves are 4-connected; stepping onto      */
/* water is allowed only from or onto a shoreline tile; an impassable start   */
/* tile has no moves. A step costs 1, +5 on water or shoreline, +3 when the   */
/* direction changes; the heuristic is twice the Manhattan distance. The path */
/* is then scaled x2 with the in-between tiles interpolated (rounding half    */
/* up) and trimmed or extended so that it starts at `from` and ends at `to`. */
/* ======================================================================== */

#define RAIL_WATER_PENALTY 5
#define RAIL_HEURISTIC_WEIGHT 2
#define RAIL_TURN_PENALTY 3

struct TsRailPf {
    const TsGrid *game, *mini;
    TsAStar *astar;
    IntVec mini_path, up;
};

static bool rail_traversable(const TsGrid *m, int32_t to, bool from_shoreline, bool impassable) {
    if (impassable) return false;
    if (!g_is_water(m, to)) return true;
    return from_shoreline || g_is_shoreline(m, to);
}

static int32_t rail_neighbors(void *ctx, int32_t node, int32_t *buf) {
    const TsGrid *m = (const TsGrid *)ctx;
    int32_t count = 0, w = m->w, n = m->w * m->h;
    int32_t x = node % w;
    bool from_shore = g_is_shoreline(m, node);
    bool imp = g_is_impassable(m, node);
    if (node >= w && rail_traversable(m, node - w, from_shore, imp)) buf[count++] = node - w;
    if (node < n - w && rail_traversable(m, node + w, from_shore, imp)) buf[count++] = node + w;
    if (x != 0 && rail_traversable(m, node - 1, from_shore, imp)) buf[count++] = node - 1;
    if (x != w - 1 && rail_traversable(m, node + 1, from_shore, imp)) buf[count++] = node + 1;
    return count;
}

static int32_t rail_cost(void *ctx, int32_t from, int32_t to, int32_t prev) {
    const TsGrid *m = (const TsGrid *)ctx;
    bool penalized = g_is_water(m, to) || g_is_shoreline(m, to);
    int32_t c = penalized ? 1 + RAIL_WATER_PENALTY : 1;
    if (prev != -1 && (from - prev) != (to - from)) c += RAIL_TURN_PENALTY;
    return c;
}

static int32_t rail_heuristic(void *ctx, int32_t node, int32_t goal) {
    const TsGrid *m = (const TsGrid *)ctx;
    int32_t nx = node % m->w, ny = node / m->w, gx = goal % m->w, gy = goal / m->w;
    return RAIL_HEURISTIC_WEIGHT * (abs(nx - gx) + abs(ny - gy));
}

TsRailPf *ts_rail_pf_new(const TsGrid *game, const TsGrid *mini) {
    TsRailPf *pf = (TsRailPf *)ofe_xcalloc(1, sizeof(TsRailPf));
    pf->game = game;
    pf->mini = mini;
    TsAStarAdapter ad;
    memset(&ad, 0, sizeof(ad));
    ad.ctx = (void *)mini;
    ad.num_nodes = mini->w * mini->h;
    ad.max_neighbors = 4;
    int32_t max_cost = 1 + RAIL_WATER_PENALTY + RAIL_TURN_PENALTY;
    ad.max_priority = RAIL_HEURISTIC_WEIGHT * (mini->w + mini->h) * max_cost;
    ad.neighbors = rail_neighbors;
    ad.cost = rail_cost;
    ad.heuristic = rail_heuristic;
    pf->astar = ts_astar_new(&ad);
    return pf;
}

void ts_rail_pf_free(TsRailPf *pf) {
    if (!pf) return;
    ts_astar_free(pf->astar);
    IntVec_free(&pf->mini_path);
    IntVec_free(&pf->up);
    free(pf);
}

static int32_t index_of(const IntVec *v, int32_t x) {
    for (int32_t i = 0; i < v->len; i++)
        if (v->data[i] == x) return i;
    return -1;
}

bool ts_rail_pf_find_path(TsRailPf *pf, int32_t from, int32_t to, IntVec *out) {
    const TsGrid *game = pf->game, *mini = pf->mini;
    int32_t mfrom = (ts_grid_y(game, from) / 2) * mini->w + ts_grid_x(game, from) / 2;
    int32_t mto = (ts_grid_y(game, to) / 2) * mini->w + ts_grid_x(game, to) / 2;
    if (!ts_astar_find_path(pf->astar, mfrom, mto, &pf->mini_path) || pf->mini_path.len == 0) return false;

    /* upscalePath */
    IntVec *up = &pf->up;
    up->len = 0;
    const IntVec *p = &pf->mini_path;
    for (int32_t i = 0; i < p->len - 1; i++) {
        int32_t cx = (p->data[i] % mini->w) * 2, cy = (p->data[i] / mini->w) * 2;
        int32_t nx = (p->data[i + 1] % mini->w) * 2, ny = (p->data[i + 1] / mini->w) * 2;
        IntVec_push(up, cy * game->w + cx);
        int32_t dx = nx - cx, dy = ny - cy;
        int32_t steps = OFE_MAX(abs(dx), abs(dy));
        for (int32_t step = 1; step < steps; step++) {
            int32_t rx = (int32_t)js_round(cx + (double)(dx * step) / steps);
            int32_t ry = (int32_t)js_round(cy + (double)(dy * step) / steps);
            IntVec_push(up, ry * game->w + rx);
        }
    }
    if (p->len > 0) {
        int32_t last = p->data[p->len - 1];
        IntVec_push(up, ((last / mini->w) * 2) * game->w + (last % mini->w) * 2);
    }

    /* fixExtremes(upscaled, to, from) */
    int32_t start = 0, end;
    int32_t src_i = index_of(up, from);
    bool unshift = false;
    if (src_i == -1) unshift = true;
    else start = src_i;
    out->len = 0;
    if (unshift) IntVec_push(out, from);
    for (int32_t i = start; i < up->len; i++) IntVec_push(out, up->data[i]);
    int32_t dst_i = index_of(out, to);
    if (dst_i == -1) IntVec_push(out, to);
    else {
        end = dst_i + 1;
        out->len = end;
    }
    return out->len > 0;
}

/* ======================================================================== */
/* Rail network                                                               */
/*                                                                            */
/* Which stations get connected, and the tiles of each railroad. Follows      */
/* RailNetworkImpl.connectStation: a new station first tries to snap onto     */
/* passing railroads (connectToExistingRails: any railroad with a tile in a   */
/* 4x4-cell window around it is split at its tile closest to the station);    */
/* only if none is split does it connect to nearby stations, nearest first    */
/* (connectToNearbyStations), each over a fresh A* railroad unless that       */
/* station is already within 4 hops or closer than 15 tiles. Clusters are the */
/* TS Cluster sets (insertion ordered), kept because the order of a cluster's */
/* trade stations decides which destination a train draws.                    */
/*                                                                            */
/* The candidate order comes from UnitGrid.nearbyUnits: 100x100 cells row by  */
/* row, within a cell the requested types in the order given, within a type   */
/* the units in the order they were added; then a stable sort by distance.    */
/* Not supported: removing stations or railroads (recomputeClusters).         */
/* ======================================================================== */

#define STATION_MAX_RANGE 110
#define STATION_MIN_RANGE 15
#define STATION_RADIUS 3
#define MAX_CONNECTION_HOPS 4
#define RAIL_GRID_CELL 4
#define UNIT_GRID_CELL 100
#define RAILROAD_MAX_SIZE (STATION_MAX_RANGE * 1.4142)
#define NUM_STRUCTURE_TYPES 3

typedef struct NetUnit {
    int32_t type, tile;
    int32_t station; /* first station of this unit, -1 */
    bool has_flag;   /* setTrainStation(true) */
} NetUnit;
OFE_VEC(NetUnitVec, NetUnit)

typedef struct NetStation {
    int32_t unit, tile, type;
    IntVec rails;                  /* railroads in Set order */
    IntVec nb_station, nb_rail;    /* railroadByNeighbor entries */
    int32_t cluster;               /* -1 = null */
} NetStation;
OFE_VEC(NetStationVec, NetStation)

typedef struct NetRail {
    int32_t from, to, id;
    IntVec tiles;
    bool alive;
} NetRail;
OFE_VEC(NetRailVec, NetRail)

typedef struct NetCluster {
    IntVec stations; /* Set order */
} NetCluster;
OFE_VEC(NetClusterVec, NetCluster)

typedef struct RailCell {
    int32_t cx, cy;
    IntVec rails;
} RailCell;
OFE_VEC(RailCellVec, RailCell)

struct TsRailNet {
    const TsGrid *game, *mini;
    TsRailPf *pf;
    NetUnitVec units;
    NetStationVec stations;
    NetRailVec rails;
    NetClusterVec clusters;
    int32_t next_rail_id;
    /* unit grid: [cell][type] -> unit indexes in insertion order */
    int32_t grid_cols, grid_rows;
    IntVec *grid;
    /* rail spatial grid */
    RailCellVec cells;
    int32_t *cell_table; /* open addressing, cell index + 1 */
    int32_t cell_table_size;
    /* scratch */
    IntVec seen;
    TsAStar *station_astar;
    int32_t station_astar_nodes;
};

TsRailNet *ts_rail_net_new(const TsGrid *game, const TsGrid *mini) {
    TsRailNet *n = (TsRailNet *)ofe_xcalloc(1, sizeof(TsRailNet));
    n->game = game;
    n->mini = mini;
    n->pf = ts_rail_pf_new(game, mini);
    n->grid_cols = (game->w + UNIT_GRID_CELL - 1) / UNIT_GRID_CELL;
    n->grid_rows = (game->h + UNIT_GRID_CELL - 1) / UNIT_GRID_CELL;
    n->grid = (IntVec *)ofe_xcalloc((size_t)n->grid_cols * n->grid_rows * NUM_STRUCTURE_TYPES, sizeof(IntVec));
    n->cell_table_size = 1024;
    n->cell_table = (int32_t *)ofe_xcalloc((size_t)n->cell_table_size, 4);
    return n;
}

void ts_rail_net_free(TsRailNet *n) {
    if (!n) return;
    ts_rail_pf_free(n->pf);
    ts_astar_free(n->station_astar);
    for (int32_t i = 0; i < n->grid_cols * n->grid_rows * NUM_STRUCTURE_TYPES; i++) IntVec_free(&n->grid[i]);
    free(n->grid);
    for (int32_t i = 0; i < n->stations.len; i++) {
        IntVec_free(&n->stations.data[i].rails);
        IntVec_free(&n->stations.data[i].nb_station);
        IntVec_free(&n->stations.data[i].nb_rail);
    }
    for (int32_t i = 0; i < n->rails.len; i++) IntVec_free(&n->rails.data[i].tiles);
    for (int32_t i = 0; i < n->clusters.len; i++) IntVec_free(&n->clusters.data[i].stations);
    for (int32_t i = 0; i < n->cells.len; i++) IntVec_free(&n->cells.data[i].rails);
    NetUnitVec_free(&n->units);
    NetStationVec_free(&n->stations);
    NetRailVec_free(&n->rails);
    NetClusterVec_free(&n->clusters);
    RailCellVec_free(&n->cells);
    IntVec_free(&n->seen);
    free(n->cell_table);
    free(n);
}

/* ---- ordered-set helpers (JS Set semantics on int vectors) ---- */
static void set_add(IntVec *v, int32_t x) {
    for (int32_t i = 0; i < v->len; i++)
        if (v->data[i] == x) return;
    IntVec_push(v, x);
}
static void set_delete(IntVec *v, int32_t x) {
    for (int32_t i = 0; i < v->len; i++) {
        if (v->data[i] == x) {
            memmove(v->data + i, v->data + i + 1, (size_t)(v->len - i - 1) * sizeof(int32_t));
            v->len--;
            return;
        }
    }
}
static bool set_has(const IntVec *v, int32_t x) { return index_of(v, x) >= 0; }

/* ---- unit grid ---- */
int32_t ts_rail_net_add_structure(TsRailNet *n, TsStructure type, int32_t tile) {
    NetUnit u = {(int32_t)type, tile, -1, false};
    NetUnitVec_push(&n->units, u);
    int32_t idx = n->units.len - 1;
    int32_t cx = ts_grid_x(n->game, tile) / UNIT_GRID_CELL, cy = ts_grid_y(n->game, tile) / UNIT_GRID_CELL;
    IntVec_push(&n->grid[((size_t)cy * n->grid_cols + cx) * NUM_STRUCTURE_TYPES + type], idx);
    return idx;
}

typedef struct Nearby {
    int32_t unit;
    int64_t dist2;
} Nearby;
OFE_VEC(NearbyVec, Nearby)

static void cells_in_range(const TsRailNet *n, int32_t tile, int32_t range, int32_t *sx, int32_t *ex, int32_t *sy, int32_t *ey) {
    int32_t x = ts_grid_x(n->game, tile), y = ts_grid_y(n->game, tile);
    int32_t gx = x / UNIT_GRID_CELL, gy = y / UNIT_GRID_CELL;
    *sx = OFE_MAX(0, gx - (int32_t)ceil((double)(range - (x % UNIT_GRID_CELL)) / UNIT_GRID_CELL));
    *ex = OFE_MIN(n->grid_cols - 1, gx + (int32_t)ceil((double)(range - (UNIT_GRID_CELL - x % UNIT_GRID_CELL)) / UNIT_GRID_CELL));
    *sy = OFE_MAX(0, gy - (int32_t)ceil((double)(range - (y % UNIT_GRID_CELL)) / UNIT_GRID_CELL));
    *ey = OFE_MIN(n->grid_rows - 1, gy + (int32_t)ceil((double)(range - (UNIT_GRID_CELL - y % UNIT_GRID_CELL)) / UNIT_GRID_CELL));
}

/* UnitGrid.nearbyUnits, unsorted (grid order). */
static void nearby_units(const TsRailNet *n, int32_t tile, int32_t range, const int32_t *types, int32_t ntypes, NearbyVec *out) {
    out->len = 0;
    int32_t sx, ex, sy, ey;
    cells_in_range(n, tile, range, &sx, &ex, &sy, &ey);
    int32_t x = ts_grid_x(n->game, tile), y = ts_grid_y(n->game, tile);
    int64_t range2 = (int64_t)range * range;
    for (int32_t cy = sy; cy <= ey; cy++) {
        for (int32_t cx = sx; cx <= ex; cx++) {
            for (int32_t k = 0; k < ntypes; k++) {
                const IntVec *set = &n->grid[((size_t)cy * n->grid_cols + cx) * NUM_STRUCTURE_TYPES + types[k]];
                for (int32_t i = 0; i < set->len; i++) {
                    int32_t u = set->data[i];
                    int64_t dx = ts_grid_x(n->game, n->units.data[u].tile) - x;
                    int64_t dy = ts_grid_y(n->game, n->units.data[u].tile) - y;
                    int64_t d2 = dx * dx + dy * dy;
                    if (d2 > range2) continue;
                    Nearby nb = {u, d2};
                    NearbyVec_push(out, nb);
                }
            }
        }
    }
}

static bool has_unit_nearby(const TsRailNet *n, int32_t tile, int32_t range, int32_t type) {
    int32_t sx, ex, sy, ey;
    cells_in_range(n, tile, range, &sx, &ex, &sy, &ey);
    int32_t x = ts_grid_x(n->game, tile), y = ts_grid_y(n->game, tile);
    int64_t range2 = (int64_t)range * range;
    for (int32_t cy = sy; cy <= ey; cy++) {
        for (int32_t cx = sx; cx <= ex; cx++) {
            const IntVec *set = &n->grid[((size_t)cy * n->grid_cols + cx) * NUM_STRUCTURE_TYPES + type];
            for (int32_t i = 0; i < set->len; i++) {
                int32_t u = set->data[i];
                int64_t dx = ts_grid_x(n->game, n->units.data[u].tile) - x;
                int64_t dy = ts_grid_y(n->game, n->units.data[u].tile) - y;
                if (dx * dx + dy * dy <= range2) return true;
            }
        }
    }
    return false;
}

/* Array.prototype.sort (stable) by distance. */
static void sort_nearby(NearbyVec *v) {
    for (int32_t i = 1; i < v->len; i++) {
        Nearby x = v->data[i];
        int32_t j = i - 1;
        while (j >= 0 && v->data[j].dist2 > x.dist2) {
            v->data[j + 1] = v->data[j];
            j--;
        }
        v->data[j + 1] = x;
    }
}

/* ---- rail spatial grid (RailroadSpatialGrid.ts) ---- */
static uint32_t cell_hash(int32_t cx, int32_t cy) {
    uint32_t h = (uint32_t)cx * 0x9e3779b1u ^ ((uint32_t)cy * 0x85ebca77u);
    return h ^ (h >> 15);
}

static RailCell *cell_get(TsRailNet *n, int32_t cx, int32_t cy, bool create) {
    uint32_t mask = (uint32_t)n->cell_table_size - 1;
    uint32_t h = cell_hash(cx, cy) & mask;
    while (n->cell_table[h]) {
        RailCell *c = &n->cells.data[n->cell_table[h] - 1];
        if (c->cx == cx && c->cy == cy) return c;
        h = (h + 1) & mask;
    }
    if (!create) return NULL;
    if ((n->cells.len + 1) * 2 > n->cell_table_size) { /* grow and rehash */
        free(n->cell_table);
        n->cell_table_size *= 2;
        n->cell_table = (int32_t *)ofe_xcalloc((size_t)n->cell_table_size, 4);
        uint32_t m2 = (uint32_t)n->cell_table_size - 1;
        for (int32_t i = 0; i < n->cells.len; i++) {
            uint32_t k = cell_hash(n->cells.data[i].cx, n->cells.data[i].cy) & m2;
            while (n->cell_table[k]) k = (k + 1) & m2;
            n->cell_table[k] = i + 1;
        }
        return cell_get(n, cx, cy, true);
    }
    RailCell nc;
    memset(&nc, 0, sizeof(nc));
    nc.cx = cx;
    nc.cy = cy;
    RailCellVec_push(&n->cells, nc);
    n->cell_table[h] = n->cells.len;
    return &n->cells.data[n->cells.len - 1];
}

static inline int32_t floor_div(int32_t a, int32_t b) { return (int32_t)floor((double)a / b); }

static void grid_unregister(TsRailNet *n, int32_t rail) {
    const IntVec *tiles = &n->rails.data[rail].tiles;
    for (int32_t i = 0; i < tiles->len; i++) {
        RailCell *c = cell_get(n, floor_div(ts_grid_x(n->game, tiles->data[i]), RAIL_GRID_CELL),
                               floor_div(ts_grid_y(n->game, tiles->data[i]), RAIL_GRID_CELL), false);
        if (c) set_delete(&c->rails, rail);
    }
}

static void grid_register(TsRailNet *n, int32_t rail) {
    grid_unregister(n, rail);
    const IntVec *tiles = &n->rails.data[rail].tiles;
    for (int32_t i = 0; i < tiles->len; i++) {
        RailCell *c = cell_get(n, floor_div(ts_grid_x(n->game, tiles->data[i]), RAIL_GRID_CELL),
                               floor_div(ts_grid_y(n->game, tiles->data[i]), RAIL_GRID_CELL), true);
        set_add(&c->rails, rail);
    }
}

/* RailSpatialGrid.query: cells column by column (x outer, y inner), rails in Set order. */
static void grid_query(TsRailNet *n, int32_t tile, int32_t radius, IntVec *out) {
    out->len = 0;
    int32_t x = ts_grid_x(n->game, tile), y = ts_grid_y(n->game, tile);
    int32_t c0x = floor_div(x - radius, RAIL_GRID_CELL), c0y = floor_div(y - radius, RAIL_GRID_CELL);
    int32_t c1x = floor_div(x + radius, RAIL_GRID_CELL), c1y = floor_div(y + radius, RAIL_GRID_CELL);
    for (int32_t cx = c0x; cx <= c1x; cx++) {
        for (int32_t cy = c0y; cy <= c1y; cy++) {
            RailCell *c = cell_get(n, cx, cy, false);
            if (!c) continue;
            for (int32_t i = 0; i < c->rails.len; i++) set_add(out, c->rails.data[i]);
        }
    }
}

/* ---- stations, railroads, clusters ---- */
static void station_add_rail(TsRailNet *n, int32_t st, int32_t rail) {
    NetStation *s = &n->stations.data[st];
    set_add(&s->rails, rail);
    int32_t nb = n->rails.data[rail].from == st ? n->rails.data[rail].to : n->rails.data[rail].from;
    int32_t i = index_of(&s->nb_station, nb);
    if (i >= 0) s->nb_rail.data[i] = rail;
    else {
        IntVec_push(&s->nb_station, nb);
        IntVec_push(&s->nb_rail, rail);
    }
}

static void station_remove_rail(TsRailNet *n, int32_t st, int32_t rail) {
    NetStation *s = &n->stations.data[st];
    set_delete(&s->rails, rail);
    int32_t nb = n->rails.data[rail].from == st ? n->rails.data[rail].to : n->rails.data[rail].from;
    int32_t i = index_of(&s->nb_station, nb);
    if (i >= 0) {
        memmove(s->nb_station.data + i, s->nb_station.data + i + 1, (size_t)(s->nb_station.len - i - 1) * 4);
        memmove(s->nb_rail.data + i, s->nb_rail.data + i + 1, (size_t)(s->nb_rail.len - i - 1) * 4);
        s->nb_station.len--;
        s->nb_rail.len--;
    }
}

static int32_t new_rail(TsRailNet *n, int32_t from, int32_t to, const int32_t *tiles, int32_t ntiles) {
    NetRail r;
    memset(&r, 0, sizeof(r));
    r.from = from;
    r.to = to;
    r.id = n->next_rail_id++;
    r.alive = true;
    for (int32_t i = 0; i < ntiles; i++) IntVec_push(&r.tiles, tiles[i]);
    NetRailVec_push(&n->rails, r);
    return n->rails.len - 1;
}

static int32_t new_cluster(TsRailNet *n) {
    NetCluster c;
    memset(&c, 0, sizeof(c));
    NetClusterVec_push(&n->clusters, c);
    return n->clusters.len - 1;
}

static void cluster_add_station(TsRailNet *n, int32_t cl, int32_t st) {
    set_add(&n->clusters.data[cl].stations, st);
    NetStation *s = &n->stations.data[st];
    if (s->cluster != -1 && s->cluster != cl) set_delete(&n->clusters.data[s->cluster].stations, st);
    s->cluster = cl;
}

static void clusters_merge(TsRailNet *n, const IntVec *to_merge) {
    int32_t merged = new_cluster(n);
    for (int32_t i = 0; i < to_merge->len; i++) {
        int32_t other = to_merge->data[i];
        IntVec snapshot = {0};
        IntVec_copy(&snapshot, &n->clusters.data[other].stations);
        for (int32_t k = 0; k < snapshot.len; k++) cluster_add_station(n, merged, snapshot.data[k]);
        IntVec_free(&snapshot);
    }
}

static void stations_within(const TsRailNet *n, int32_t start, int32_t max_distance, IntVec *visited) {
    visited->len = 0;
    IntVec_push(visited, start);
    IntVec frontier = {0}, next = {0};
    IntVec_push(&frontier, start);
    for (int32_t hop = 0; hop < max_distance && frontier.len > 0; hop++) {
        next.len = 0;
        for (int32_t i = 0; i < frontier.len; i++) {
            const NetStation *s = &n->stations.data[frontier.data[i]];
            for (int32_t k = 0; k < s->rails.len; k++) {
                const NetRail *r = &n->rails.data[s->rails.data[k]];
                int32_t nb = r->from == frontier.data[i] ? r->to : r->from;
                if (set_has(visited, nb)) continue;
                IntVec_push(visited, nb);
                IntVec_push(&next, nb);
            }
        }
        IntVec t = frontier;
        frontier = next;
        next = t;
    }
    IntVec_free(&frontier);
    IntVec_free(&next);
}

/* validRailPath: the A* railroad, accepted if non-empty and shorter than railroadMaxSize. */
static bool valid_rail_path(TsRailNet *n, int32_t from, int32_t to, IntVec *path) {
    if (!ts_rail_pf_find_path(n->pf, from, to, path)) return false;
    return path->len > 0 && (double)path->len < RAILROAD_MAX_SIZE;
}

static bool connect_to_existing_rails(TsRailNet *n, int32_t st) {
    IntVec rails = {0}, edited = {0};
    int32_t tile = n->stations.data[st].tile;
    grid_query(n, tile, STATION_RADIUS, &rails);
    int32_t tx = ts_grid_x(n->game, tile), ty = ts_grid_y(n->game, tile);
    for (int32_t i = 0; i < rails.len; i++) {
        int32_t rail = rails.data[i];
        int32_t from = n->rails.data[rail].from, to = n->rails.data[rail].to;
        int32_t len = n->rails.data[rail].tiles.len;
        int32_t closest = len == 0 ? -1 : 0;
        int64_t best = INT64_MAX;
        for (int32_t k = 0; k < len; k++) {
            int64_t dx = ts_grid_x(n->game, n->rails.data[rail].tiles.data[k]) - tx;
            int64_t dy = ts_grid_y(n->game, n->rails.data[rail].tiles.data[k]) - ty;
            if (dx * dx + dy * dy < best) {
                best = dx * dx + dy * dy;
                closest = k;
            }
        }
        if (closest == 0 || closest >= len) continue;

        station_remove_rail(n, from, rail);
        station_remove_rail(n, to, rail);
        grid_unregister(n, rail);
        n->rails.data[rail].alive = false;

        /* NetRailVec may reallocate: copy tiles out first. */
        int32_t nt = len;
        int32_t *tiles = (int32_t *)ofe_xmalloc((size_t)nt * 4);
        memcpy(tiles, n->rails.data[rail].tiles.data, (size_t)nt * 4);
        int32_t rf = new_rail(n, from, st, tiles, closest);
        int32_t rt = new_rail(n, st, to, tiles + closest, nt - closest);
        free(tiles);

        station_add_rail(n, st, rf);
        station_add_rail(n, st, rt);
        station_add_rail(n, from, rf);
        station_add_rail(n, to, rt);
        grid_register(n, rt);
        grid_register(n, rf);
        int32_t cl = n->stations.data[from].cluster;
        if (cl != -1) {
            cluster_add_station(n, cl, st);
            set_add(&edited, cl);
        }
    }
    if (edited.len > 1) clusters_merge(n, &edited);
    bool snapped = edited.len != 0;
    IntVec_free(&rails);
    IntVec_free(&edited);
    return snapped;
}

static void connect_to_nearby_stations(TsRailNet *n, int32_t st) {
    static const int32_t types[3] = {TS_CITY, TS_FACTORY, TS_PORT};
    NearbyVec nbs = {0};
    nearby_units(n, n->stations.data[st].tile, STATION_MAX_RANGE, types, 3, &nbs);
    sort_nearby(&nbs);
    /* candidates: stations of nearby units that belong to a cluster */
    NearbyVec cands = {0};
    for (int32_t i = 0; i < nbs.len; i++) {
        if (nbs.data[i].unit == n->stations.data[st].unit) continue;
        int32_t ns = n->units.data[nbs.data[i].unit].station;
        if (ns < 0) continue;
        if (n->stations.data[ns].cluster == -1) continue;
        Nearby c = {ns, nbs.data[i].dist2};
        NearbyVec_push(&cands, c);
    }

    IntVec edited = {0}, reachable = {0}, path = {0};
    stations_within(n, st, MAX_CONNECTION_HOPS, &reachable);
    for (int32_t i = 0; i < cands.len; i++) {
        int32_t nb = cands.data[i].unit; /* station index */
        if (set_has(&reachable, nb) || cands.data[i].dist2 <= (int64_t)STATION_MIN_RANGE * STATION_MIN_RANGE) continue;
        if (!valid_rail_path(n, n->stations.data[st].tile, n->stations.data[nb].tile, &path)) continue;
        /* connectWithPath */
        int32_t r = new_rail(n, st, nb, path.data, path.len);
        station_add_rail(n, st, r);
        station_add_rail(n, nb, r);
        grid_register(n, r);
        int32_t ncl = n->stations.data[nb].cluster;
        if (ncl != -1) {
            cluster_add_station(n, ncl, st);
            set_add(&edited, ncl);
        }
        stations_within(n, st, MAX_CONNECTION_HOPS, &reachable);
    }
    if (edited.len > 1) clusters_merge(n, &edited);
    else if (edited.len == 0) {
        int32_t c = new_cluster(n);
        cluster_add_station(n, c, st);
    }
    NearbyVec_free(&nbs);
    NearbyVec_free(&cands);
    IntVec_free(&edited);
    IntVec_free(&reachable);
    IntVec_free(&path);
}

/* TrainStationExecution.tick: new TrainStation + railNetwork.connectStation. */
static void connect_station(TsRailNet *n, int32_t unit) {
    NetStation s;
    memset(&s, 0, sizeof(s));
    s.unit = unit;
    s.tile = n->units.data[unit].tile;
    s.type = n->units.data[unit].type;
    s.cluster = -1;
    NetStationVec_push(&n->stations, s);
    int32_t st = n->stations.len - 1;
    if (n->units.data[unit].station < 0) n->units.data[unit].station = st;
    if (!connect_to_existing_rails(n, st)) connect_to_nearby_stations(n, st);
}

void ts_rail_net_structure_done(TsRailNet *n, int32_t unit) {
    NetUnit *u = &n->units.data[unit];
    IntVec todo = {0};
    if (u->type == TS_FACTORY) {
        static const int32_t types[3] = {TS_CITY, TS_PORT, TS_FACTORY};
        NearbyVec nbs = {0};
        nearby_units(n, u->tile, STATION_MAX_RANGE, types, 3, &nbs);
        /* Ports run PortExecution every tick: the tick the factory appears, every
         * station-less port within range already sees it (its execution runs
         * before the new FactoryExecution's first tick) and starts its station
         * first, in port creation order. */
        for (int32_t p = 0; p < n->units.len; p++) {
            NetUnit *pu = &n->units.data[p];
            if (p == unit || pu->type != TS_PORT || pu->has_flag) continue;
            if (has_unit_nearby(n, pu->tile, STATION_MAX_RANGE, TS_FACTORY)) {
                pu->has_flag = true;
                IntVec_push(&todo, p);
            }
        }
        /* addExecution(new TrainStationExecution(factory, true)) sets the flag at once */
        u->has_flag = true;
        IntVec_push(&todo, unit);
        for (int32_t i = 0; i < nbs.len; i++) {
            NetUnit *o = &n->units.data[nbs.data[i].unit];
            if (!o->has_flag) {
                o->has_flag = true;
                IntVec_push(&todo, nbs.data[i].unit);
            }
        }
        NearbyVec_free(&nbs);
    } else if (!u->has_flag && has_unit_nearby(n, u->tile, STATION_MAX_RANGE, TS_FACTORY)) {
        u->has_flag = true;
        IntVec_push(&todo, unit);
    }
    for (int32_t i = 0; i < todo.len; i++) connect_station(n, todo.data[i]);
    IntVec_free(&todo);
}

int32_t ts_rail_net_num_stations(const TsRailNet *n) { return n->stations.len; }
int32_t ts_rail_net_num_units(const TsRailNet *n) { return n->units.len; }
int32_t ts_rail_net_station_of_unit(const TsRailNet *n, int32_t unit) { return n->units.data[unit].station; }
int32_t ts_rail_net_station_unit(const TsRailNet *n, int32_t station) { return n->stations.data[station].unit; }
int32_t ts_rail_net_station_tile(const TsRailNet *n, int32_t station) { return n->stations.data[station].tile; }

int32_t ts_rail_net_num_railroads(const TsRailNet *n) {
    int32_t c = 0;
    for (int32_t i = 0; i < n->rails.len; i++) c += n->rails.data[i].alive;
    return c;
}

bool ts_rail_net_railroad(const TsRailNet *n, int32_t i, int32_t *from, int32_t *to, int32_t *id, const int32_t **tiles, int32_t *num_tiles) {
    for (int32_t k = 0; k < n->rails.len; k++) {
        if (!n->rails.data[k].alive) continue;
        if (i-- == 0) {
            const NetRail *r = &n->rails.data[k];
            *from = r->from;
            *to = r->to;
            *id = r->id;
            *tiles = r->tiles.data;
            *num_tiles = r->tiles.len;
            return true;
        }
    }
    return false;
}

bool ts_rail_net_oriented(const TsRailNet *n, int32_t a, int32_t b, IntVec *tiles_out) {
    const NetStation *s = &n->stations.data[a];
    int32_t i = index_of(&s->nb_station, b);
    if (i < 0) return false;
    const NetRail *r = &n->rails.data[s->nb_rail.data[i]];
    tiles_out->len = 0;
    if (r->to == b) {
        for (int32_t k = 0; k < r->tiles.len; k++) IntVec_push(tiles_out, r->tiles.data[k]);
    } else {
        for (int32_t k = r->tiles.len - 1; k >= 0; k--) IntVec_push(tiles_out, r->tiles.data[k]);
    }
    return true;
}

int32_t ts_rail_net_cluster(const TsRailNet *n, int32_t station) { return n->stations.data[station].cluster; }

void ts_rail_net_cluster_trade_stations(const TsRailNet *n, int32_t cluster, IntVec *out) {
    out->len = 0;
    const IntVec *cs = &n->clusters.data[cluster].stations;
    for (int32_t i = 0; i < cs->len; i++) {
        int32_t t = n->stations.data[cs->data[i]].type;
        if (t == TS_CITY || t == TS_PORT) IntVec_push(out, cs->data[i]);
    }
}

void ts_rail_net_neighbors(const TsRailNet *n, int32_t station, IntVec *out) {
    out->len = 0;
    const NetStation *s = &n->stations.data[station];
    for (int32_t k = 0; k < s->rails.len; k++) {
        const NetRail *r = &n->rails.data[s->rails.data[k]];
        IntVec_push(out, r->from != station ? r->from : r->to);
    }
}

/* ---- PathFinder.Station.ts: A* on the station graph (node id = index + 1) ---- */
typedef struct StationAdapterCtx {
    TsRailNet *n;
} StationAdapterCtx;

static int32_t sg_neighbors(void *ctx, int32_t node, int32_t *buf) {
    TsRailNet *n = ((StationAdapterCtx *)ctx)->n;
    if (node < 1 || node > n->stations.len) return 0;
    const NetStation *s = &n->stations.data[node - 1];
    int32_t count = 0;
    for (int32_t k = 0; k < s->rails.len && count < 32; k++) {
        const NetRail *r = &n->rails.data[s->rails.data[k]];
        buf[count++] = (r->from != node - 1 ? r->from : r->to) + 1;
    }
    return count;
}

static int32_t sg_cost(void *ctx, int32_t from, int32_t to, int32_t prev) { return 1; }

static int32_t sg_heuristic(void *ctx, int32_t node, int32_t goal) {
    TsRailNet *n = ((StationAdapterCtx *)ctx)->n;
    if (node < 1 || node > n->stations.len || goal < 1 || goal > n->stations.len) return 0;
    int32_t a = n->stations.data[node - 1].tile, b = n->stations.data[goal - 1].tile;
    return abs(ts_grid_x(n->game, a) - ts_grid_x(n->game, b)) + abs(ts_grid_y(n->game, a) - ts_grid_y(n->game, b));
}

bool ts_rail_net_stations_path(TsRailNet *n, int32_t from, int32_t to, IntVec *out) {
    if (n->stations.data[from].cluster != n->stations.data[to].cluster) return false;
    /* The TS adapter sizes its arrays by manager.count() (= next id) when the
     * pathfinder is built; rebuild whenever stations were added. */
    int32_t nodes = n->stations.len + 1;
    if (!n->station_astar || n->station_astar_nodes != nodes) {
        ts_astar_free(n->station_astar);
        static StationAdapterCtx ctx;
        ctx.n = n;
        TsAStarAdapter ad;
        memset(&ad, 0, sizeof(ad));
        ad.ctx = &ctx;
        ad.num_nodes = nodes;
        ad.max_neighbors = 32;
        ad.max_priority = n->game->w + n->game->h;
        ad.neighbors = sg_neighbors;
        ad.cost = sg_cost;
        ad.heuristic = sg_heuristic;
        n->station_astar = ts_astar_new(&ad);
        n->station_astar_nodes = nodes;
    }
    IntVec ids = {0};
    bool ok = ts_astar_find_path(n->station_astar, from + 1, to + 1, &ids);
    out->len = 0;
    if (ok)
        for (int32_t i = 0; i < ids.len; i++) IntVec_push(out, ids.data[i] - 1);
    IntVec_free(&ids);
    return ok;
}

/* ======================================================================== */
/* Train motion (TrainExecution.ts)                                           */
/*                                                                            */
/* The engine advances `speed` (2) path indices per tick along the oriented   */
/* railroads of its station route; when it overshoots the end of a railroad   */
/* the excess carries into the next one and the station is "reached". The     */
/* cars follow along `usedTiles`, a window of the tiles the engine passed    */
/* (one entry per tile, so spacing does not depend on speed). Falsy tile      */
/* quirks of the TS code are kept: tile ref 0 or running off the railroad     */
/* ends the trip. Not supported: a railroad being split under a moving train  */
/* (reconcileCurrentRailroadSplit).                                           */
/* ======================================================================== */

#define TRAIN_SPEED 2
#define TRAIN_SPACING 2

struct TsTrain {
    const TsRailNet *net;
    IntVec stations; /* remaining route */
    IntVec rail;     /* oriented tiles of the current railroad */
    IntVec path;     /* all tiles of the route, for tile < 0 lookups */
    int32_t path_index, current_tile, num_cars;
    IntVec used;
    int32_t engine;
    int32_t cars[TS_TRAIN_MAX_UNITS];
    bool active, stuck;
};

TsTrain *ts_train_new(const TsRailNet *n, const int32_t *stations, int32_t ns, int32_t num_cars) {
    if (ns <= 1 || num_cars + 1 > TS_TRAIN_MAX_UNITS) return NULL;
    TsTrain *t = (TsTrain *)ofe_xcalloc(1, sizeof(TsTrain));
    t->net = n;
    t->num_cars = num_cars + 1; /* tail engine + carriages */
    for (int32_t i = 0; i < ns; i++) IntVec_push(&t->stations, stations[i]);
    t->active = true;
    IntVec seg = {0};
    for (int32_t i = 0; i + 1 < ns; i++) {
        if (!ts_rail_net_oriented(n, stations[i], stations[i + 1], &seg)) {
            IntVec_free(&seg);
            if (i == 0) { /* init deactivates before any unit exists */
                ts_train_free(t);
                return NULL;
            }
            /* TS builds the units first and then deactivates the execution on a missing
             * segment: the units stay on the start tile for good. */
            t->stuck = true;
            t->active = false;
            break;
        }
        if (i == 0) IntVec_copy(&t->rail, &seg);
        for (int32_t k = 0; k < seg.len; k++) IntVec_push(&t->path, seg.data[k]);
    }
    IntVec_free(&seg);
    int32_t start = ts_rail_net_station_tile(n, stations[0]);
    if (!t->stuck && (t->path.len == 0 || t->path.data[0] != start)) {
        IntVec_push(&t->path, 0);
        memmove(t->path.data + 1, t->path.data, (size_t)(t->path.len - 1) * 4);
        t->path.data[0] = start;
        t->path_index = 1;
        t->current_tile = -1;
    }
    t->engine = start;
    for (int32_t i = 0; i < t->num_cars; i++) t->cars[i] = start;
    return t;
}

void ts_train_free(TsTrain *t) {
    if (!t) return;
    IntVec_free(&t->stations);
    IntVec_free(&t->rail);
    IntVec_free(&t->path);
    IntVec_free(&t->used);
    free(t);
}

static void train_save_tile(TsTrain *t, int32_t tile) {
    IntVec_push(&t->used, tile);
    if (t->used.len > t->num_cars * TRAIN_SPACING + 3) {
        memmove(t->used.data, t->used.data + 1, (size_t)(t->used.len - 1) * 4);
        t->used.len--;
    }
}

static void train_save_traversed(TsTrain *t, int32_t from, int32_t speed) {
    int32_t cur = from;
    for (int32_t i = 0; i < speed && cur < t->rail.len; i++) {
        int32_t tile = cur < 0 ? t->path.data[cur + t->path_index] : t->rail.data[cur];
        train_save_tile(t, tile);
        cur++;
    }
}

bool ts_train_stuck(const TsTrain *t) { return t->stuck; }

void ts_train_tick(TsTrain *t, bool destination_tradable, TsTrainStep *out) {
    out->moved = false;
    if (t->stuck) { /* inactive execution: never ticked, units remain */
        out->active_after = true;
        out->stop_station = -1;
        out->num_cars = t->num_cars;
        return;
    }
    out->stop_station = -1;
    out->num_cars = t->num_cars;
    int32_t tile = 0;
    bool have_tile = false;
    if (destination_tradable) { /* getNextTile */
        train_save_traversed(t, t->current_tile, TRAIN_SPEED);
        t->current_tile += TRAIN_SPEED;
        int32_t left_over = t->current_tile - t->rail.len;
        bool ok = true;
        if (left_over >= 0) {
            out->stop_station = t->stations.data[1]; /* stationReached */
            if (t->stations.len > 2) { /* nextStation */
                t->path_index += t->rail.len;
                memmove(t->stations.data, t->stations.data + 1, (size_t)(t->stations.len - 1) * 4);
                t->stations.len--;
                IntVec nr = {0};
                if (ts_rail_net_oriented(t->net, t->stations.data[0], t->stations.data[1], &nr)) {
                    IntVec_copy(&t->rail, &nr);
                    t->current_tile = left_over;
                    train_save_traversed(t, 0, left_over);
                } else {
                    ok = false; /* a split railroad is not supported: the trip ends */
                }
                IntVec_free(&nr);
            } else {
                ok = false;
            }
        }
        if (ok && t->current_tile >= 0 && t->current_tile < t->rail.len) {
            tile = t->rail.data[t->current_tile];
            have_tile = tile != 0; /* `if (tile)`: ref 0 is falsy */
        }
    }
    if (!have_tile) {
        t->active = false; /* targetReached + deleteTrain */
        out->active_after = false;
        return;
    }
    for (int32_t i = t->num_cars - 1; i >= 0; --i) { /* updateCarsPositions */
        int32_t idx = (i + 1) * TRAIN_SPACING + 2;
        if (t->used.len > idx) t->cars[i] = t->used.data[idx];
    }
    t->engine = tile;
    out->moved = true;
    out->engine_tile = tile;
    for (int32_t i = 0; i < t->num_cars; i++) out->car_tile[i] = t->cars[i];
    out->active_after = true;
}

#endif /* OFE_TS_PARITY */

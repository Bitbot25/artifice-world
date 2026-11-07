/*
 * util.h - small shared helpers: growable vectors, RNG, a tile min-heap,
 * an int hash set, and math helpers. Header-only.
 */
#ifndef OFE_UTIL_H
#define OFE_UTIL_H

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define OFE_MIN(a, b) ((a) < (b) ? (a) : (b))
#define OFE_MAX(a, b) ((a) > (b) ? (a) : (b))
#define OFE_CLAMP(v, lo, hi) OFE_MIN(OFE_MAX((v), (lo)), (hi))
#define OFE_UNUSED(x) ((void)(x))

void *ofe_xmalloc(size_t n);
void *ofe_xcalloc(size_t count, size_t size);
void *ofe_xrealloc(void *p, size_t n);

/* ---- Growable vector of plain values ------------------------------------
 * Declared per element type with OFE_VEC(Name, Type). All functions are
 * static inline so every module gets its own copy.
 */
#define OFE_VEC(Name, T)                                                      \
    typedef struct Name {                                                     \
        T *data;                                                              \
        int32_t len, cap;                                                     \
    } Name;                                                                   \
    static inline void Name##_reserve(Name *v, int32_t cap) {                 \
        if (cap <= v->cap) return;                                            \
        int32_t nc = v->cap ? v->cap : 8;                                     \
        while (nc < cap) nc *= 2;                                             \
        v->data = (T *)ofe_xrealloc(v->data, (size_t)nc * sizeof(T));         \
        v->cap = nc;                                                          \
    }                                                                         \
    static inline void Name##_push(Name *v, T x) {                            \
        if (v->len == v->cap) Name##_reserve(v, v->len + 1);                  \
        v->data[v->len++] = x;                                                \
    }                                                                         \
    static inline void Name##_free(Name *v) {                                 \
        free(v->data);                                                        \
        v->data = NULL;                                                       \
        v->len = v->cap = 0;                                                  \
    }                                                                         \
    /* dst becomes a copy of src, reusing dst's buffer when possible. */      \
    static inline void Name##_copy(Name *dst, const Name *src) {              \
        dst->len = 0;                                                         \
        Name##_reserve(dst, src->len);                                        \
        if (src->len) memcpy(dst->data, src->data, (size_t)src->len * sizeof(T)); \
        dst->len = src->len;                                                  \
    }

OFE_VEC(IntVec, int32_t)

/* ---- RNG: splitmix64 ---------------------------------------------------
 * One 64-bit state per stream. Cheap to copy, so every subsystem that wants
 * an independent stream keeps its own state.
 */
static inline uint64_t rng_next(uint64_t *s) {
    uint64_t z = (*s += 0x9E3779B97F4A7C15ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}
/* Uniform double in [0, 1). */
static inline double rng_float(uint64_t *s) {
    return (double)(rng_next(s) >> 11) * (1.0 / 9007199254740992.0);
}
/* Uniform int in [lo, hi). Returns lo if hi <= lo. */
static inline int32_t rng_int(uint64_t *s, int32_t lo, int32_t hi) {
    if (hi <= lo) return lo;
    return lo + (int32_t)((rng_next(s) >> 32) % (uint64_t)(hi - lo));
}
static inline bool rng_chance(uint64_t *s, int32_t one_in) {
    return one_in <= 1 || rng_int(s, 0, one_in) == 0;
}
/* Derives an independent stream seed from a parent seed and a tag. */
static inline uint64_t rng_derive(uint64_t seed, uint64_t tag) {
    uint64_t s = seed ^ (tag * 0xD6E8FEB86659FD93ull);
    return rng_next(&s);
}

/* ---- TsRandom: OpenFront's PseudoRandom --------------------------------
 * sfc32 seeded through splitmix32 with 12 warm-up draws, exactly as
 * engine-lib/PseudoRandom.ts. All parity random streams use it (see
 * docs/ts_parity.md); it is cheap, so it is compiled into every build.
 */
typedef struct TsRandom {
    uint32_t s0, s1, s2, s3;
} TsRandom;

static inline double ts_rand_next(TsRandom *r) {
    uint32_t t = r->s0 + r->s1 + r->s3;
    r->s3 += 1;
    r->s0 = r->s1 ^ (r->s1 >> 9);
    r->s1 = r->s2 + (r->s2 << 3);
    r->s2 = (r->s2 << 21) | (r->s2 >> 11);
    r->s2 += t;
    return (double)t / 4294967296.0;
}

/* `seed` is a JS number: only its low 32 bits matter (seed | 0). */
static inline void ts_rand_init(TsRandom *r, int64_t seed) {
    uint32_t h = (uint32_t)seed;
    uint32_t w[4];
    for (int i = 0; i < 4; i++) {
        h += 0x9e3779b9u;
        uint32_t t = h ^ (h >> 16);
        t *= 0x21f0aaadu;
        t ^= t >> 15;
        t *= 0x735a2d97u;
        w[i] = t ^ (t >> 15);
    }
    r->s0 = w[0], r->s1 = w[1], r->s2 = w[2], r->s3 = w[3];
    for (int i = 0; i < 12; i++) ts_rand_next(r);
}

/* Integer in [lo, hi): floor(next() * (floor(hi) - floor(lo))) + floor(lo). */
static inline double ts_rand_int(TsRandom *r, double lo, double hi) {
    lo = floor(lo);
    hi = floor(hi);
    return floor(ts_rand_next(r) * (hi - lo)) + lo;
}
static inline double ts_rand_float(TsRandom *r, double lo, double hi) { return ts_rand_next(r) * (hi - lo) + lo; }
static inline bool ts_rand_chance(TsRandom *r, double odds) { return ts_rand_int(r, 0, odds) == 0; }
/* Index into an array of n elements (randElement). */
static inline int32_t ts_rand_index(TsRandom *r, int32_t n) { return (int32_t)ts_rand_int(r, 0, n); }
/* Fisher-Yates in place, like shuffleArray (which shuffles a copy). */
static inline void ts_rand_shuffle(TsRandom *r, int32_t *a, int32_t n) {
    for (int32_t i = n - 1; i > 0; i--) {
        int32_t j = (int32_t)ts_rand_int(r, 0, i + 1);
        int32_t t = a[i];
        a[i] = a[j];
        a[j] = t;
    }
}
/* nextID(): 8 base-36 characters of floor(next() * 36^8). out has room for 9. */
static inline void ts_rand_id(TsRandom *r, char out[9]) {
    uint64_t v = (uint64_t)floor(ts_rand_next(r) * 2821109907456.0);
    for (int i = 7; i >= 0; i--) {
        out[i] = "0123456789abcdefghijklmnopqrstuvwxyz"[v % 36];
        v /= 36;
    }
    out[8] = 0;
}
/* simpleHash(str) of engine-lib/Util.ts: 31-multiplier hash, absolute value. */
static inline int64_t ts_simple_hash(const char *s) {
    int32_t h = 0;
    for (; *s; s++) h = (int32_t)((uint32_t)h * 31u + (uint32_t)(uint8_t)*s);
    return h < 0 ? -(int64_t)h : (int64_t)h;
}

/* ---- DetMath: OpenFront's deterministic exp / log / pow -----------------
 * engine-lib/DetMath.ts. Plain + - * / only, so the result is bit-identical
 * to the TS engine (this must not be compiled with FMA contraction;
 * -std=c99 already turns it off).
 */
static inline double det_pow2(int n) {
    if (n > 1023) return INFINITY;
    if (n < -1022) return 0;
    uint64_t bits = (uint64_t)(n + 1023) << 52;
    double d;
    memcpy(&d, &bits, sizeof d);
    return d;
}

static inline double det_exp(double x) {
    if (x > 709) return INFINITY;
    if (x < -708) return 0;
    const double LN2 = 0.6931471805599453, LOG2E = 1.4426950408889634;
    double n = floor(x * LOG2E + 0.5);
    double r = x - n * LN2;
    double p = 1 + r * (1 + r * (1.0 / 2 + r * (1.0 / 6 + r * (1.0 / 24 + r * (1.0 / 120 + r * (1.0 / 720 +
                   r * (1.0 / 5040 + r * (1.0 / 40320))))))));
    return p * det_pow2((int)n);
}

static inline double det_log(double x) {
    if (x <= 0) return x == 0 ? -INFINITY : NAN;
    const double LN2 = 0.6931471805599453, SQRT2 = 1.4142135623730951;
    int e = 0;
    if (x < 2.2250738585072014e-308) {
        x *= 18014398509481984.0; /* 2^54 */
        e = -54;
    }
    uint64_t bits;
    memcpy(&bits, &x, sizeof bits);
    e += (int)((bits >> 52) & 0x7ff) - 1023;
    bits = (bits & 0x000fffffffffffffull) | 0x3ff0000000000000ull;
    double m;
    memcpy(&m, &bits, sizeof m);
    if (m > SQRT2) {
        m *= 0.5;
        e += 1;
    }
    double s = (m - 1) / (m + 1);
    double z = s * s;
    double series = 1 + z * (1.0 / 3 + z * (1.0 / 5 + z * (1.0 / 7 + z * (1.0 / 9 + z * (1.0 / 11 + z * (1.0 / 13))))));
    return e * LN2 + 2 * s * series;
}

/* x ** y for x >= 0. */
static inline double det_pow(double x, double y) {
    if (y == 0 || x == 1) return 1;
    if (x == 0) return y > 0 ? 0 : INFINITY;
    if (x < 0) return NAN;
    return det_exp(y * det_log(x));
}

/* exp / log / pow of the engine's formulas: OpenFront's DetMath in the parity
 * build (bit-exact), libm in the default build (faster, ~1e-8 apart). */
#if OFE_TS_PARITY
#define ofe_exp det_exp
#define ofe_log det_log
#define ofe_pow det_pow
#else
#define ofe_exp exp
#define ofe_log log
#define ofe_pow pow
#endif

/* ---- Tile min-heap (priority float, tile int32) ------------------------- */
typedef struct TileHeap {
    float *pri;
    int32_t *tile;
    int32_t len, cap;
} TileHeap;

static inline void heap_push(TileHeap *h, int32_t tile, float pri) {
    if (h->len == h->cap) {
        int32_t nc = h->cap ? h->cap * 2 : 64;
        h->pri = (float *)ofe_xrealloc(h->pri, (size_t)nc * sizeof(float));
        h->tile = (int32_t *)ofe_xrealloc(h->tile, (size_t)nc * sizeof(int32_t));
        h->cap = nc;
    }
    int32_t i = h->len++;
    while (i > 0) {
        int32_t parent = (i - 1) >> 1;
        if (pri >= h->pri[parent]) break;
        h->pri[i] = h->pri[parent];
        h->tile[i] = h->tile[parent];
        i = parent;
    }
    h->pri[i] = pri;
    h->tile[i] = tile;
}

static inline int32_t heap_pop(TileHeap *h) {
    int32_t top = h->tile[0];
    int32_t n = --h->len;
    if (n > 0) {
        float p = h->pri[n];
        int32_t t = h->tile[n];
        int32_t i = 0;
        for (;;) {
            int32_t c = 2 * i + 1;
            if (c >= n) break;
            if (c + 1 < n && h->pri[c + 1] < h->pri[c]) c++;
            if (p <= h->pri[c]) break;
            h->pri[i] = h->pri[c];
            h->tile[i] = h->tile[c];
            i = c;
        }
        h->pri[i] = p;
        h->tile[i] = t;
    }
    return top;
}

static inline void heap_free(TileHeap *h) {
    free(h->pri);
    free(h->tile);
    memset(h, 0, sizeof(*h));
}

static inline void heap_copy(TileHeap *dst, const TileHeap *src) {
    if (dst->cap < src->len) {
        heap_free(dst);
        dst->cap = src->cap;
        dst->pri = (float *)ofe_xmalloc((size_t)dst->cap * sizeof(float));
        dst->tile = (int32_t *)ofe_xmalloc((size_t)dst->cap * sizeof(int32_t));
    }
    dst->len = src->len;
    if (src->len) {
        memcpy(dst->pri, src->pri, (size_t)src->len * sizeof(float));
        memcpy(dst->tile, src->tile, (size_t)src->len * sizeof(int32_t));
    }
}

/* ---- Open-addressing set of non-negative int32 (tiles) ------------------ */
typedef struct IntSet {
    int32_t *slots; /* -1 empty, -2 deleted */
    int32_t cap; /* power of two or 0 */
    int32_t len, used; /* live entries, live + tombstones */
} IntSet;

static inline uint32_t intset_hash(int32_t x) {
    uint32_t h = (uint32_t)x * 0x9E3779B1u;
    return h ^ (h >> 16);
}

static inline void intset_clear(IntSet *s) {
    if (s->cap) memset(s->slots, 0xff, (size_t)s->cap * sizeof(int32_t));
    s->len = s->used = 0;
}

static inline void intset_rehash(IntSet *s, int32_t newcap) {
    int32_t *old = s->slots;
    int32_t oldcap = s->cap;
    s->slots = (int32_t *)ofe_xmalloc((size_t)newcap * sizeof(int32_t));
    memset(s->slots, 0xff, (size_t)newcap * sizeof(int32_t));
    s->cap = newcap;
    s->len = s->used = 0;
    for (int32_t i = 0; i < oldcap; i++) {
        int32_t x = old[i];
        if (x >= 0) {
            uint32_t j = intset_hash(x) & (uint32_t)(newcap - 1);
            while (s->slots[j] != -1) j = (j + 1) & (uint32_t)(newcap - 1);
            s->slots[j] = x;
            s->len++;
            s->used++;
        }
    }
    free(old);
}

/* Returns true if x was newly inserted. */
static inline bool intset_add(IntSet *s, int32_t x) {
    if ((s->used + 1) * 4 >= s->cap * 3) intset_rehash(s, s->cap ? (s->len * 2 >= s->cap ? s->cap * 2 : s->cap) : 64);
    uint32_t mask = (uint32_t)(s->cap - 1);
    uint32_t j = intset_hash(x) & mask;
    int32_t tomb = -1;
    while (s->slots[j] != -1) {
        if (s->slots[j] == x) return false;
        if (s->slots[j] == -2 && tomb < 0) tomb = (int32_t)j;
        j = (j + 1) & mask;
    }
    if (tomb >= 0) {
        s->slots[tomb] = x;
    } else {
        s->slots[j] = x;
        s->used++;
    }
    s->len++;
    return true;
}

static inline bool intset_has(const IntSet *s, int32_t x) {
    if (!s->cap) return false;
    uint32_t mask = (uint32_t)(s->cap - 1);
    uint32_t j = intset_hash(x) & mask;
    while (s->slots[j] != -1) {
        if (s->slots[j] == x) return true;
        j = (j + 1) & mask;
    }
    return false;
}

/* Returns true if x was present. */
static inline bool intset_remove(IntSet *s, int32_t x) {
    if (!s->cap) return false;
    uint32_t mask = (uint32_t)(s->cap - 1);
    uint32_t j = intset_hash(x) & mask;
    while (s->slots[j] != -1) {
        if (s->slots[j] == x) {
            s->slots[j] = -2;
            s->len--;
            return true;
        }
        j = (j + 1) & mask;
    }
    return false;
}

static inline void intset_free(IntSet *s) {
    free(s->slots);
    memset(s, 0, sizeof(*s));
}

static inline void intset_copy(IntSet *dst, const IntSet *src) {
    if (dst->cap != src->cap) {
        free(dst->slots);
        dst->slots = src->cap ? (int32_t *)ofe_xmalloc((size_t)src->cap * sizeof(int32_t)) : NULL;
        dst->cap = src->cap;
    }
    if (src->cap) memcpy(dst->slots, src->slots, (size_t)src->cap * sizeof(int32_t));
    dst->len = src->len;
    dst->used = src->used;
}

/* ---- Math ---------------------------------------------------------------- */
static inline double sigmoid(double value, double decay_rate, double midpoint) {
    return 1.0 / (1.0 + ofe_exp(-decay_rate * (value - midpoint)));
}

static inline uint64_t hash_mix(uint64_t h, uint64_t x) {
    h ^= x + 0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2);
    return h * 0xFF51AFD7ED558CCDull;
}

#endif /* OFE_UTIL_H */

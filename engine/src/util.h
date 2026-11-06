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
    return 1.0 / (1.0 + exp(-decay_rate * (value - midpoint)));
}

static inline uint64_t hash_mix(uint64_t h, uint64_t x) {
    h ^= x + 0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2);
    return h * 0xFF51AFD7ED558CCDull;
}

#endif /* OFE_UTIL_H */

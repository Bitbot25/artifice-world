/*
 * map.c - loading OpenFront maps and the shared, immutable map caches.
 *
 * A map directory holds manifest.json (sizes, nations) and raw terrain
 * buffers: map.bin (full size), map4x.bin (half width and height) and
 * map16x.bin (quarter). Each byte is a terrain byte (see internal.h).
 */
#include <pthread.h>

#include "internal.h"
#include "paths_ts.h"

/* Map caches live outside OfeMap's public fields so map.c owns locking. */
typedef struct MapCaches {
    pthread_mutex_t lock;
    void *data[MAP_CACHE_COUNT];
    MapCacheFree destroy[MAP_CACHE_COUNT];
} MapCaches;

/* One MapCaches per map, stored right after the struct allocation. */
typedef struct MapAlloc {
    OfeMap map;
    MapCaches caches;
} MapAlloc;

static MapCaches *map_caches(const OfeMap *m) { return &((MapAlloc *)m)->caches; }

void *ofe_xmalloc(size_t n) {
    void *p = malloc(n ? n : 1);
    if (!p) {
        fprintf(stderr, "ofe: out of memory (%zu bytes)\n", n);
        abort();
    }
    return p;
}

void *ofe_xcalloc(size_t count, size_t size) {
    void *p = calloc(count ? count : 1, size ? size : 1);
    if (!p) {
        fprintf(stderr, "ofe: out of memory (%zu x %zu bytes)\n", count, size);
        abort();
    }
    return p;
}

void *ofe_xrealloc(void *p, size_t n) {
    void *q = realloc(p, n ? n : 1);
    if (!q) {
        fprintf(stderr, "ofe: out of memory (%zu bytes)\n", n);
        abort();
    }
    return q;
}

static uint8_t terrain_class(uint8_t b, bool is_ocean) {
    if (!(b & TERRAIN_LAND_BIT)) return is_ocean ? TC_OCEAN : TC_LAKE;
    int mag = b & TERRAIN_MAGNITUDE_MASK;
    if (mag >= TERRAIN_IMPASSABLE_MAGNITUDE) return TC_IMPASSABLE;
    if (mag < 10) return TC_PLAINS;
    if (mag < 20) return TC_HIGHLAND;
    return TC_MOUNTAIN;
}

static OfeMap *map_alloc(int32_t width, int32_t height) {
    MapAlloc *a = (MapAlloc *)ofe_xcalloc(1, sizeof(MapAlloc));
    OfeMap *m = &a->map;
    m->refcount = 1;
    m->width = width;
    m->height = height;
    m->num_tiles = width * height;
    m->terrain = (uint8_t *)ofe_xmalloc((size_t)m->num_tiles);
    m->tclass = (uint8_t *)ofe_xmalloc((size_t)m->num_tiles);
    pthread_mutex_init(&a->caches.lock, NULL);
    return m;
}

/* Fills tclass and num_land_tiles from terrain. */
static void map_derive(OfeMap *m) {
    m->num_land_tiles = 0;
    for (int32_t t = 0; t < m->num_tiles; t++) {
        uint8_t b = m->terrain[t];
        m->tclass[t] = terrain_class(b, (b & TERRAIN_OCEAN_BIT) != 0);
        if (m->tclass[t] >= TC_PLAINS && m->tclass[t] <= TC_MOUNTAIN) m->num_land_tiles++;
    }
}

/* Half-resolution map: a mini tile is land if any of its 2x2 tiles is. */
static void map_derive_mini(OfeMap *m) {
    m->mini_width = (m->width + 1) / 2;
    m->mini_height = (m->height + 1) / 2;
    m->mini_terrain = (uint8_t *)ofe_xcalloc((size_t)m->mini_width * m->mini_height, 1);
    for (int32_t y = 0; y < m->height; y++) {
        for (int32_t x = 0; x < m->width; x++) {
            uint8_t b = m->terrain[y * m->width + x];
            uint8_t *mb = &m->mini_terrain[(y / 2) * m->mini_width + x / 2];
            if (b & TERRAIN_LAND_BIT) {
                *mb = (uint8_t)((*mb & ~TERRAIN_OCEAN_BIT) | TERRAIN_LAND_BIT | (b & TERRAIN_MAGNITUDE_MASK));
            } else if (!(*mb & TERRAIN_LAND_BIT)) {
                *mb |= (uint8_t)(b & TERRAIN_OCEAN_BIT);
            }
        }
    }
}

static uint8_t *read_file(const char *path, size_t *len_out) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *buf = (uint8_t *)ofe_xmalloc((size_t)len + 1);
    size_t got = fread(buf, 1, (size_t)len, f);
    fclose(f);
    buf[got] = 0;
    *len_out = got;
    return buf;
}

/* ---- A tiny reader for the parts of manifest.json we need ----------------
 * Not a general JSON parser: it finds `"key"` and reads the value after the
 * colon. Good enough for OpenFront's generated manifests.
 */
static const char *json_find_key(const char *s, const char *end, const char *key) {
    size_t klen = strlen(key);
    for (const char *p = s; p && p + klen + 2 < end; p++) {
        if (*p == '"' && strncmp(p + 1, key, klen) == 0 && p[klen + 1] == '"') {
            const char *q = p + klen + 2;
            while (q < end && (*q == ' ' || *q == '\n' || *q == '\r' || *q == '\t')) q++;
            if (q < end && *q == ':') return q + 1;
        }
    }
    return NULL;
}

/* End of the JSON value (object/array/string/scalar) starting at p. */
static const char *json_skip_value(const char *p, const char *end) {
    while (p < end && (*p == ' ' || *p == '\n' || *p == '\r' || *p == '\t')) p++;
    if (p >= end) return end;
    if (*p == '{' || *p == '[') {
        int depth = 0;
        bool in_str = false;
        for (; p < end; p++) {
            if (in_str) {
                if (*p == '\\') p++;
                else if (*p == '"') in_str = false;
            } else if (*p == '"') {
                in_str = true;
            } else if (*p == '{' || *p == '[') {
                depth++;
            } else if (*p == '}' || *p == ']') {
                if (--depth == 0) return p + 1;
            }
        }
        return end;
    }
    if (*p == '"') {
        for (p++; p < end; p++) {
            if (*p == '\\') p++;
            else if (*p == '"') return p + 1;
        }
        return end;
    }
    while (p < end && *p != ',' && *p != '}' && *p != ']') p++;
    return p;
}

static int32_t json_int(const char *s, const char *end, const char *key, int32_t def) {
    const char *v = json_find_key(s, end, key);
    return v ? (int32_t)strtol(v, NULL, 10) : def;
}

/* Reads "map" / "map4x" sizes and the nations list. */
static bool parse_manifest(const char *json, size_t len, const char *size_key, int32_t *w, int32_t *h,
                           char *name, Nation **nations, int32_t *num_nations) {
    const char *end = json + len;
    const char *obj = json_find_key(json, end, size_key);
    if (!obj) return false;
    const char *obj_end = json_skip_value(obj, end);
    *w = json_int(obj, obj_end, "width", -1);
    *h = json_int(obj, obj_end, "height", -1);
    if (*w <= 0 || *h <= 0) return false;

    name[0] = 0;
    /* Top-level "name": the last "name" key outside "nations". Take the first
     * "name" that appears after the nations array, or before it. */
    const char *nat = json_find_key(json, end, "nations");
    const char *nat_end = nat ? json_skip_value(nat, end) : NULL;
    for (const char *p = json; p < end;) {
        const char *v = json_find_key(p, end, "name");
        if (!v) break;
        if (nat && v > nat && v < nat_end) {
            p = nat_end;
            continue;
        }
        const char *q = v;
        while (q < end && *q != '"') q++;
        if (q < end) {
            q++;
            int i = 0;
            while (q < end && *q != '"' && i < 63) name[i++] = *q++;
            name[i] = 0;
        }
        break;
    }

    *nations = NULL;
    *num_nations = 0;
    if (!nat) return true;
    int32_t cap = 0;
    const char *p = nat;
    while (p < nat_end) {
        const char *item = strchr(p, '{');
        if (!item || item >= nat_end) break;
        const char *item_end = json_skip_value(item, nat_end);
        const char *coords = json_find_key(item, item_end, "coordinates");
        if (coords) {
            int32_t x = -1, y = -1;
            const char *c = strchr(coords, '[');
            if (c && c < item_end) {
                char *after;
                x = (int32_t)strtol(c + 1, &after, 10);
                while (*after == ',' || *after == ' ' || *after == '\n') after++;
                y = (int32_t)strtol(after, NULL, 10);
            }
            if (*num_nations == cap) {
                cap = cap ? cap * 2 : 32;
                *nations = (Nation *)ofe_xrealloc(*nations, (size_t)cap * sizeof(Nation));
            }
            Nation *n = &(*nations)[(*num_nations)++];
            memset(n, 0, sizeof(*n));
            n->tile = (x << 16) | (y & 0xffff); /* packed until the map size is known */
            const char *nm = json_find_key(item, item_end, "name");
            if (nm) {
                const char *q = strchr(nm, '"');
                if (q && q < item_end) {
                    q++;
                    int i = 0;
                    while (*q && *q != '"' && i < (int)sizeof(n->name) - 1) n->name[i++] = *q++;
                }
            }
        }
        p = item_end;
    }
    return true;
}

static bool load_bin(const char *dir, const char *file, int32_t w, int32_t h, uint8_t *out) {
    char path[1024];
    snprintf(path, sizeof(path), "%s/%s", dir, file);
    size_t len = 0;
    uint8_t *buf = read_file(path, &len);
    if (!buf) {
        fprintf(stderr, "ofe: cannot read %s\n", path);
        return false;
    }
    if (len != (size_t)w * (size_t)h) {
        fprintf(stderr, "ofe: %s has %zu bytes, expected %dx%d\n", path, len, w, h);
        free(buf);
        return false;
    }
    memcpy(out, buf, len);
    free(buf);
    return true;
}

OfeMap *ofe_map_load(const char *map_dir, int compact) {
    char path[1024];
    snprintf(path, sizeof(path), "%s/manifest.json", map_dir);
    size_t len = 0;
    char *json = (char *)read_file(path, &len);
    if (!json) {
        fprintf(stderr, "ofe: cannot read %s\n", path);
        return NULL;
    }
    int32_t w, h, num_nations;
    char name[64];
    Nation *nations;
    const char *size_key = compact ? "map4x" : "map";
    if (!parse_manifest(json, len, size_key, &w, &h, name, &nations, &num_nations)) {
        fprintf(stderr, "ofe: bad manifest %s\n", path);
        free(json);
        return NULL;
    }
    free(json);

    OfeMap *m = map_alloc(w, h);
    snprintf(m->name, sizeof(m->name), "%s", name);
    if (!load_bin(map_dir, compact ? "map4x.bin" : "map.bin", w, h, m->terrain)) {
        free(nations);
        ofe_map_release(m);
        return NULL;
    }
    map_derive(m);
    map_derive_mini(m);
#if OFE_TS_PARITY
    /* TS: the mini map is OpenFront's own map4x.bin / map16x.bin, not a 2x2 reduction. */
    {
        uint8_t *mt = ts_mini_terrain_load(map_dir, compact, w, h);
        if (mt) {
            free(m->mini_terrain);
            m->mini_terrain = mt;
        }
    }
#endif

    /* Nation coordinates are given at full size; compact maps halve them. */
    for (int32_t i = 0; i < num_nations; i++) {
        int32_t x = nations[i].tile >> 16, y = nations[i].tile & 0xffff;
        if (compact) {
            x /= 2;
            y /= 2;
        }
        x = OFE_CLAMP(x, 0, w - 1);
        y = OFE_CLAMP(y, 0, h - 1);
        nations[i].tile = y * w + x;
    }
    m->nations = nations;
    m->num_nations = num_nations;
    return m;
}

OfeMap *ofe_map_from_terrain(int32_t width, int32_t height, const uint8_t *terrain) {
    if (width <= 0 || height <= 0 || !terrain) return NULL;
    OfeMap *m = map_alloc(width, height);
    snprintf(m->name, sizeof(m->name), "custom");
    memcpy(m->terrain, terrain, (size_t)m->num_tiles);
    map_derive(m);
    map_derive_mini(m);
    return m;
}

void ofe_map_add_nation(OfeMap *m, const char *name, int32_t x, int32_t y) {
    if (!map_in_bounds(m, x, y)) return;
    m->nations = (Nation *)ofe_xrealloc(m->nations, (size_t)(m->num_nations + 1) * sizeof(Nation));
    Nation *n = &m->nations[m->num_nations++];
    memset(n, 0, sizeof(*n));
    snprintf(n->name, sizeof(n->name), "%s", name ? name : "Nation");
    n->tile = map_ref(m, x, y);
}

void ofe_map_retain(OfeMap *m) { __atomic_add_fetch(&m->refcount, 1, __ATOMIC_RELAXED); }

void ofe_map_release(OfeMap *m) {
    if (!m) return;
    if (__atomic_sub_fetch(&m->refcount, 1, __ATOMIC_ACQ_REL) != 0) return;
    MapCaches *c = map_caches(m);
    for (int i = 0; i < MAP_CACHE_COUNT; i++) {
        if (c->data[i] && c->destroy[i]) c->destroy[i](c->data[i]);
    }
    pthread_mutex_destroy(&c->lock);
    free(m->terrain);
    free(m->tclass);
    free(m->mini_terrain);
    free(m->nations);
    free((MapAlloc *)m);
}

void *map_cache_get(OfeMap *m, int slot, MapCacheBuild build, MapCacheFree destroy) {
    MapCaches *c = map_caches(m);
    void *data = __atomic_load_n(&c->data[slot], __ATOMIC_ACQUIRE);
    if (data) return data;
    pthread_mutex_lock(&c->lock);
    data = c->data[slot];
    if (!data) {
        data = build(m);
        c->destroy[slot] = destroy;
        __atomic_store_n(&c->data[slot], data, __ATOMIC_RELEASE);
    }
    pthread_mutex_unlock(&c->lock);
    return data;
}

int32_t ofe_map_width(const OfeMap *m) { return m->width; }
int32_t ofe_map_height(const OfeMap *m) { return m->height; }
int32_t ofe_map_num_land_tiles(const OfeMap *m) { return m->num_land_tiles; }
const uint8_t *ofe_map_terrain(const OfeMap *m) { return m->terrain; }
int32_t ofe_map_num_nations(const OfeMap *m) { return m->num_nations; }
const char *ofe_map_name(const OfeMap *m) { return m->name; }

int32_t ofe_map_nation(const OfeMap *m, int32_t i, char *name, int32_t cap) {
    if (i < 0 || i >= m->num_nations) return -1;
    if (name && cap > 0) snprintf(name, (size_t)cap, "%s", m->nations[i].name);
    return m->nations[i].tile;
}

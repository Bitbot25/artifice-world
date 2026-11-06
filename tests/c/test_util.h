/*
 * test_util.h - shared helpers for the C tests: a CHECK macro, a synthetic
 * map, and an invariant checker that recomputes derived tile state.
 *
 * Each test binary includes this once.
 */
#ifndef OFE_TEST_UTIL_H
#define OFE_TEST_UTIL_H

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

#include "../../engine/src/internal.h"

static int failures = 0;

#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);  \
            failures++;                                                      \
        }                                                                    \
    } while (0)

/* Island in an ocean, a lake in the middle, a mountain band. */
static OfeMap *make_map(int w, int h) {
    uint8_t *t = (uint8_t *)malloc((size_t)w * h);
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            int edge = x < 4 || y < 4 || x >= w - 4 || y >= h - 4;
            int lake = (x - w / 2) * (x - w / 2) + (y - h / 2) * (y - h / 2) < 36;
            uint8_t b;
            if (edge) b = TERRAIN_OCEAN_BIT;
            else if (lake) b = 0;
            else {
                int mag = (x > w / 3 && x < w / 3 + 6) ? 25 : (y % 17 == 0 ? 12 : 3);
                b = (uint8_t)(TERRAIN_LAND_BIT | mag);
            }
            t[y * w + x] = b;
        }
    }
    /* Shoreline bits: land next to water and water next to land. */
    uint8_t *s = (uint8_t *)malloc((size_t)w * h);
    memcpy(s, t, (size_t)w * h);
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            int land = t[y * w + x] & TERRAIN_LAND_BIT;
            int dx[4] = {1, -1, 0, 0}, dy[4] = {0, 0, 1, -1};
            for (int k = 0; k < 4; k++) {
                int xx = x + dx[k], yy = y + dy[k];
                if (xx < 0 || yy < 0 || xx >= w || yy >= h) continue;
                if ((t[yy * w + xx] & TERRAIN_LAND_BIT) != land) s[y * w + x] |= TERRAIN_SHORELINE_BIT;
            }
        }
    }
    OfeMap *m = ofe_map_from_terrain(w, h, s);
    ofe_map_add_nation(m, "Alpha", w / 4, h / 4);
    ofe_map_add_nation(m, "Beta", 3 * w / 4, 3 * h / 4);
    free(t);
    free(s);
    return m;
}

/* Recomputes tile counts and border lists from scratch and compares. */
static void check_invariants(const OfeGame *g) {
    const OfeMap *m = g->map;
    int32_t n = g->players.count;
    int32_t *count = (int32_t *)calloc((size_t)n, sizeof(int32_t));
    int32_t *borders = (int32_t *)calloc((size_t)n, sizeof(int32_t));
    for (TileRef t = 0; t < m->num_tiles; t++) {
        PlayerId o = tile_owner(g, t);
        CHECK(o >= 0 && o < n);
        if (o == 0) {
            CHECK(!tile_is_border(g, t));
            continue;
        }
        CHECK(map_is_ownable(m, t));
        count[o]++;
        TileRef nb[4];
        int k = map_neighbors4(m, t, nb);
        bool border = false;
        for (int i = 0; i < k; i++) border |= tile_owner(g, nb[i]) != o;
        CHECK(border == tile_is_border(g, t));
        if (border) {
            borders[o]++;
            int32_t pos = tile_border_pos_raw(&g->tiles, t);
            CHECK(pos >= 0 && pos < g->players.border[o].len && g->players.border[o].data[pos] == t);
        }
    }
    for (PlayerId p = 1; p < n; p++) {
        CHECK(count[p] == g->players.num_tiles[p]);
        CHECK(borders[p] == g->players.border[p].len);
        CHECK(g->players.troops[p] >= 0);
    }
    free(count);
    free(borders);
}

static OfeConfig base_config(void) {
    OfeConfig c;
    ofe_config_default(&c);
    c.num_humans = 2;
    c.num_bots = 12;
    c.spawn_phase_ticks = 10;
    c.event_mask = OFE_EVENTS_ALL;
    return c;
}


/* Real OpenFront map from $OFE_MAPS_DIR, or NULL (test should skip). */
static OfeMap *load_real_map(const char *name) {
    const char *dir = getenv("OFE_MAPS_DIR");
    if (!dir) return NULL;
    char path[1024];
    snprintf(path, sizeof(path), "%s/%s", dir, name);
    return ofe_map_load(path, 0);
}

static int test_summary(const char *name) {
    if (failures) {
        fprintf(stderr, "%s: %d check(s) failed\n", name, failures);
        return 1;
    }
    printf("%s: all tests passed\n", name);
    return 0;
}

#endif

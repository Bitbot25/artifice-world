/*
 * test_mapstatic.c - per-map static outputs on a synthetic map with known
 * answers (two islands, an impassable ridge, a lake), plus determinism across
 * thread counts, and timings on real maps when OFE_MAPS_DIR is set.
 */
#include <math.h>
#include <omp.h>

#include "../../engine/include/ofe_mapstatic.h"
#include "test_util.h"

#define W 600
#define H 300

static uint8_t T[W * H];

static void fill(int x0, int x1, int y0, int y1, uint8_t v) {
    for (int y = y0; y < y1; y++)
        for (int x = x0; x < x1; x++) T[y * W + x] = v;
}

/* Sets the shoreline bit on every tile next to a tile of the other kind. */
static void set_shorelines(void) {
    static uint8_t c[W * H];
    memcpy(c, T, sizeof(c));
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) {
            int land = c[y * W + x] & 0x80, dx[4] = {1, -1, 0, 0}, dy[4] = {0, 0, 1, -1};
            for (int k = 0; k < 4; k++) {
                int xx = x + dx[k], yy = y + dy[k];
                if (xx >= 0 && yy >= 0 && xx < W && yy < H && (c[yy * W + xx] & 0x80) != land)
                    T[y * W + x] |= TERRAIN_SHORELINE_BIT;
            }
        }
}

/*
 * 600x300 -> stride 3, coarse 200x100, patches 50x25 = 1250.
 * Island A: x 40..199, y 40..259 plains; a 6-wide impassable ridge x 100..105
 *   splits it into A1 (west) and A2 (east); highland strip y 200..209 on A1,
 *   mountain strip x 150..158 on A2.
 * Island B: x 360..539, y 40..259 plains, with a lake x 400..419, y 120..149 and
 *   a 1-wide impassable ridge at x 451 (B1 | B2): thinner than a cell.
 */
static OfeMap *build(void) {
    fill(0, W, 0, H, TERRAIN_OCEAN_BIT);
    fill(40, 200, 40, 260, TERRAIN_LAND_BIT | 3);
    fill(360, 540, 40, 260, TERRAIN_LAND_BIT | 3);
    fill(100, 106, 40, 260, TERRAIN_LAND_BIT | 31);
    fill(40, 100, 200, 210, TERRAIN_LAND_BIT | 15);
    fill(150, 159, 40, 260, TERRAIN_LAND_BIT | 25);
    fill(400, 420, 120, 150, 0); /* lake: water without the ocean bit */
    fill(451, 452, 40, 260, TERRAIN_LAND_BIT | 31);
    set_shorelines();
    return ofe_map_from_terrain(W, H, T);
}

static float half(const OfeMapStatic *ms, const uint16_t *tab, int i, int j) {
    return ofe_mapstatic_f16_to_f32(tab[(size_t)i * ms->num_patches + j]);
}

static int patch_of_tile(const OfeMapStatic *ms, int x, int y) {
    int cell_x = x / ms->stride, cell_y = y / ms->stride;
    return (cell_y / ms->patch_k) * ms->pw + cell_x / ms->patch_k;
}

/* Brute-force pooled distance. which: 0 land->water, 1 water->land, 2 land->impassable. */
static float brute_dist(const OfeMapStatic *ms, int cx, int cy, int which, int pool_mean) {
    int s = ms->stride;
    double best = 1e30, sum = 0;
    int n = 0;
    for (int y = cy * s; y < OFE_MIN(H, (cy + 1) * s); y++)
        for (int x = cx * s; x < OFE_MIN(W, (cx + 1) * s); x++) {
            int k = ms->tclass[y * W + x];
            int land = k >= 2 && k <= 4, water = k <= 1;
            if (which == 1 && !water) continue;
            if (which != 1 && !land) continue;
            double d = 1e30;
            for (int yy = 0; yy < H; yy++)
                for (int xx = 0; xx < W; xx++) {
                    int kk = ms->tclass[yy * W + xx];
                    int src = which == 0 ? kk <= 1 : which == 1 ? kk >= 2 : kk == 5;
                    if (!src) continue;
                    double dd = sqrt((double)((xx - x) * (xx - x) + (yy - y) * (yy - y)));
                    if (dd < d) d = dd;
                }
            if (d < best) best = d;
            sum += d;
            n++;
        }
    return pool_mean ? (float)(sum / n) : (float)best;
}

static void test_f16(void) {
    CHECK(ofe_mapstatic_f32_to_f16(0.0f) == 0);
    CHECK(ofe_mapstatic_f32_to_f16(1.0f) == 0x3c00);
    CHECK(ofe_mapstatic_f32_to_f16(INFINITY) == 0x7c00);
    CHECK(ofe_mapstatic_f32_to_f16(1e9f) == 0x7bff); /* finite values clamp to 65504 */
    CHECK(ofe_mapstatic_f16_to_f32(0x7bff) == 65504.0f);
    CHECK(isinf(ofe_mapstatic_f16_to_f32(0x7c00)));
    for (float v = 0; v < 1024; v += 0.5f) CHECK(ofe_mapstatic_f16_to_f32(ofe_mapstatic_f32_to_f16(v)) == v);
    CHECK(ofe_mapstatic_f16_to_f32(ofe_mapstatic_f32_to_f16(1000.2f)) == 1000.0f);
    CHECK(ofe_mapstatic_f16_to_f32(ofe_mapstatic_f32_to_f16(3000.0f)) == 3000.0f);
    CHECK(ofe_mapstatic_f16_to_f32(ofe_mapstatic_f32_to_f16(3001.0f)) == 3000.0f); /* ties to even */
}

static int same_results(const OfeMapStatic *a, const OfeMapStatic *b) {
    if (a->num_arrays != b->num_arrays) return 0;
    for (int i = 0; i < a->num_arrays; i++) {
        size_t bytes = a->arrays[i].dtype == OFE_MS_U8 ? 1 : a->arrays[i].dtype == OFE_MS_F16 ? 2 : 4;
        int64_t n = 1;
        for (int k = 0; k < a->arrays[i].ndim; k++) n *= a->arrays[i].shape[k];
        if (memcmp(a->arrays[i].data, b->arrays[i].data, (size_t)n * bytes) != 0) {
            fprintf(stderr, "array %s differs\n", a->arrays[i].name);
            return 0;
        }
    }
    return 1;
}

static void test_synthetic(void) {
    OfeMap *m = build();
    OfeMapStatic *ms = ofe_mapstatic_compute(m);
    CHECK(ms && ms->version == OFE_MAPSTATIC_VERSION);
    CHECK(ms->stride == 3 && ms->cw == 200 && ms->ch == 100);
    CHECK(ms->pw == 50 && ms->ph == 25 && ms->num_patches == 1250);
    const int cw = ms->cw;

    /* Terrain classes and labels. */
    CHECK(ms->tclass[10 * W + 10] == OFE_TC_OCEAN);
    CHECK(ms->tclass[60 * W + 60] == OFE_TC_PLAINS);
    CHECK(ms->tclass[205 * W + 60] == OFE_TC_HIGHLAND);
    CHECK(ms->tclass[100 * W + 152] == OFE_TC_MOUNTAIN);
    CHECK(ms->tclass[100 * W + 102] == OFE_TC_IMPASSABLE);
    CHECK(ms->tclass[130 * W + 410] == OFE_TC_LAKE);
    CHECK(ms->shoreline[40 * W + 60] == 1 && ms->shoreline[100 * W + 60] == 0);
    CHECK(ms->magnitude[205 * W + 60] == 15);
    CHECK(ofe_mapstatic_attack_cost(OFE_TC_HIGHLAND) == 20.0f && isinf(ofe_mapstatic_attack_cost(OFE_TC_LAKE)));
    CHECK(ms->num_landmasses == 4); /* A1 A2 B1 B2 */
    int a1 = ms->landmass[100 * W + 60], a2 = ms->landmass[100 * W + 180];
    int b1 = ms->landmass[100 * W + 380], b2 = ms->landmass[100 * W + 500];
    CHECK(a1 == 1 && a2 == 2 && b1 == 3 && b2 == 4); /* raster order of first tile */
    CHECK(ms->landmass[100 * W + 102] == 0 && ms->landmass[10 * W + 10] == 0);
    CHECK(ms->num_water_bodies == 2);
    int ocean = ms->water_body[10 * W + 10], lake = ms->water_body[130 * W + 410];
    CHECK(ocean == 1 && lake == 2);
    CHECK(ms->water_body_is_ocean[ocean] == 1 && ms->water_body_is_ocean[lake] == 0);
    CHECK(ms->water_body[100 * W + 60] == 0);

    /* Cell costs (cell = tiles 3cx..3cx+2). */
    int ca = (100 / 3) * cw + 60 / 3; /* plains interior */
    CHECK(ms->land_cost[ca] == 16.5f && isinf(ms->water_cost[ca]));
    CHECK(ms->cell_land_tiles[ca] == 9 && ms->cell_water_tiles[ca] == 0);
    int chi = (207 / 3) * cw + 60 / 3; /* highland strip y 200..209 */
    CHECK(ms->land_cost[chi] == 20.0f);
    int cmix = (198 / 3) * cw + 60 / 3; /* y 198,199 plains, 200 highland */
    CHECK(fabsf(ms->land_cost[cmix] - (6 * 16.5f + 3 * 20.0f) / 9) < 1e-5f);
    int cmt = (100 / 3) * cw + 153 / 3;
    CHECK(ms->land_cost[cmt] == 25.0f);
    int cridge = (100 / 3) * cw + 103 / 3; /* x 102..104: all impassable */
    CHECK(isinf(ms->land_cost[cridge]) && isinf(ms->water_cost[cridge]));
    int csea = (10 / 3) * cw + 10 / 3;
    CHECK(ms->water_cost[csea] == 1.0f && isinf(ms->land_cost[csea]));
    int ccoast = (39 / 3) * cw + 60 / 3; /* y 39 water, 40,41 land: 3 water tiles of 9 */
    CHECK(fabsf(ms->water_cost[ccoast] - 3.0f) < 1e-6f && ms->cell_water_tiles[ccoast] == 3);

    /* Step passability. */
    int c33 = (100 / 3) * cw + 33, c34 = c33 + 1, c35 = c33 + 2, c32 = c33 - 1;
    CHECK(ms->land_step[0][c33] && ms->land_step[1][c32]); /* 32|33 open, both views agree */
    CHECK(!ms->land_step[1][c33] && !ms->land_step[0][c34]); /* 33|34 blocked by the ridge */
    CHECK(!ms->land_step[1][c34] && !ms->land_step[0][c35]);
    CHECK(ms->land_step[2][c33] && ms->land_step[3][c33]);
    CHECK(!ms->land_step[0][(40 / 3) * cw + 0]); /* map edge */
    CHECK(ms->water_step[0][csea] && ms->water_step[3][csea] && !ms->land_step[0][csea]);
    int cl = (130 / 3) * cw + 410 / 3; /* inside the lake */
    CHECK(ms->water_cost[cl] == 1.0f && ms->water_step[0][cl] && ms->water_step[1][cl]);
    for (int c = 0; c < ms->cw * ms->ch; c++) { /* symmetry of the steps */
        if (c % cw + 1 < cw) {
            CHECK(ms->land_step[1][c] == ms->land_step[0][c + 1]);
            CHECK(ms->water_step[1][c] == ms->water_step[0][c + 1]);
        }
        if (c + cw < ms->cw * ms->ch) {
            CHECK(ms->land_step[3][c] == ms->land_step[2][c + cw]);
            CHECK(ms->water_step[3][c] == ms->water_step[2][c + cw]);
        }
    }

    /* Multi-landmass / majority labels. 1-wide ridge at x 451: cell 150 = x 450..452. */
    int cthin = (100 / 3) * cw + 150;
    CHECK(ms->multi_landmass[cthin] == 1 && ms->cell_landmass[cthin] == b1); /* 1 tile each: smaller label */
    CHECK(ms->multi_landmass[ca] == 0 && ms->cell_landmass[ca] == a1 && ms->cell_landmass[csea] == 0);
    CHECK(ms->multi_landmass[c33] == 0); /* x 99..101: A1 tiles + ridge only */
    CHECK(ms->multi_water[csea] == 0 && ms->cell_water_body[csea] == ocean && ms->cell_water_body[cl] == lake);
    CHECK(ms->cell_water_body[ca] == 0);
    int cshore = (39 / 3) * cw + 60 / 3;
    CHECK(ms->cell_landmass[cshore] == a1 && ms->cell_water_body[cshore] == ocean);

    /* Distance transforms vs brute force. */
    int probe[][2] = {{20, 14}, {33, 40}, {50, 60}, {70, 34}, {136, 45}, {150, 50}, {3, 3}, {85, 99}};
    for (unsigned i = 0; i < sizeof(probe) / sizeof(probe[0]); i++) {
        int cx = probe[i][0], cy = probe[i][1], c = cy * cw + cx;
        int k = ms->tclass[cy * 3 * W + cx * 3];
        if (k <= 1) {
            float mn = brute_dist(ms, cx, cy, 1, 0), mean = brute_dist(ms, cx, cy, 1, 1);
            CHECK(fabsf(ms->coast_water_min[c] - mn) < 1e-4f && fabsf(ms->coast_water_mean[c] - mean) < 1e-3f);
            CHECK(isinf(ms->coast_land_min[c]));
        } else {
            float mn = brute_dist(ms, cx, cy, 0, 0), mean = brute_dist(ms, cx, cy, 0, 1);
            CHECK(fabsf(ms->coast_land_min[c] - mn) < 1e-4f && fabsf(ms->coast_land_mean[c] - mean) < 1e-3f);
            mn = brute_dist(ms, cx, cy, 2, 0);
            mean = brute_dist(ms, cx, cy, 2, 1);
            CHECK(fabsf(ms->imp_min[c] - mn) < 1e-4f && fabsf(ms->imp_mean[c] - mean) < 1e-3f);
        }
    }

    /* Patches. */
    int pa1 = patch_of_tile(ms, 70, 100), pa2 = patch_of_tile(ms, 180, 100), pq = patch_of_tile(ms, 70, 44);
    int pb1 = patch_of_tile(ms, 380, 100), pb2 = patch_of_tile(ms, 500, 100);
    int psea = patch_of_tile(ms, 300, 100), plake = patch_of_tile(ms, 410, 135);
    CHECK(ms->land_patch_rep[pa1] >= 0 && ms->water_patch_rep[pa1] < 0); /* interior: land only */
    CHECK(ms->water_patch_rep[psea] >= 0 && ms->land_patch_rep[psea] < 0);
    int pmix = patch_of_tile(ms, 400, 135); /* lake edge: both */
    CHECK(ms->water_patch_rep[plake] >= 0 && ms->land_patch_rep[plake] < 0);
    CHECK(ms->water_patch_rep[pmix] >= 0 && ms->land_patch_rep[pmix] >= 0);
    CHECK((ms->land_patch_rep[pa1] % cw) / 4 == pa1 % ms->pw && (ms->land_patch_rep[pa1] / cw) / 4 == pa1 / ms->pw);

    /* Land table: diagonal 0, symmetric, exact plains distance, blocked by the ridge. */
    CHECK(half(ms, ms->land_dist, pa1, pa1) == 0.0f);
    CHECK(isinf(half(ms, ms->land_dist, psea, psea)));
    CHECK(isinf(half(ms, ms->land_dist, pa1, pa2)) && isinf(half(ms, ms->land_dist, pa1, pb1)));
    CHECK(half(ms, ms->land_dist, pa1, pq) == half(ms, ms->land_dist, pq, pa1));
    int r1 = ms->land_patch_rep[pa1], r2 = ms->land_patch_rep[pq];
    int manh = abs(r1 % cw - r2 % cw) + abs(r1 / cw - r2 / cw);
    CHECK(half(ms, ms->land_dist, pa1, pq) == 3.0f * manh); /* all plains: 1 plains-tile unit per tile */
    int phi = patch_of_tile(ms, 70, 205);
    int rh = ms->land_patch_rep[phi];
    int manh_h = abs(r1 % cw - rh % cw) + abs(r1 / cw - rh / cw);
    CHECK(half(ms, ms->land_dist, pa1, phi) > 3.0f * manh_h); /* costlier over highland */
    /* A ridge thinner than a cell is invisible to the coarse graph (documented aliasing). */
    CHECK(isfinite(half(ms, ms->land_dist, pb1, pb2)));

    /* Water table: ocean patches connect around the islands; the lake is its own body. */
    int psea2 = patch_of_tile(ms, 570, 20);
    float wd = half(ms, ms->water_dist, psea, psea2);
    CHECK(isfinite(wd) && wd >= 3.0f * abs(ms->water_patch_rep[psea] % cw - ms->water_patch_rep[psea2] % cw));
    CHECK(half(ms, ms->water_dist, psea, psea) == 0.0f);
    CHECK(isinf(half(ms, ms->water_dist, psea, plake)));

    /* Coastal. */
    int pcA = patch_of_tile(ms, 100, 40), pcB = patch_of_tile(ms, 400, 40);
    CHECK(ms->coastal_patch[pcA] && ms->coastal_patch[pcB]);
    CHECK(!ms->coastal_patch[pa1] && !ms->coastal_patch[psea] && !ms->coastal_patch[plake]);
    CHECK(ms->coastal_anchor[pcA] >= 0 && ms->cell_water_tiles[ms->coastal_anchor[pcA]] > 0);
    CHECK(ms->coastal_anchor[pa1] == -1);
    float cd = half(ms, ms->coastal_dist, pcA, pcB);
    CHECK(isfinite(cd) && cd > 3.0f * 50 && half(ms, ms->coastal_dist, pcA, pcA) == 0.0f);
    CHECK(cd == half(ms, ms->coastal_dist, pcB, pcA));
    CHECK(isinf(half(ms, ms->coastal_dist, pcA, pa1)));
    CHECK(!ms->coastal_patch[patch_of_tile(ms, 421, 135)]); /* lake shores are not coastal */

    /* Generic accessors. */
    CHECK(ofe_mapstatic_num_arrays(ms) == ms->num_arrays && ms->num_arrays > 40);
    CHECK(ofe_mapstatic_array(ms, "land_dist") == ms->land_dist && ofe_mapstatic_array(ms, "nope") == NULL);

    /* Determinism across thread counts. */
    omp_set_num_threads(1);
    OfeMapStatic *m1 = ofe_mapstatic_compute(m);
    omp_set_num_threads(4);
    OfeMapStatic *m4 = ofe_mapstatic_compute(m);
    CHECK(same_results(ms, m1) && same_results(ms, m4));
    ofe_mapstatic_free(m1);
    ofe_mapstatic_free(m4);
    ofe_mapstatic_free(ms);
    ofe_map_release(m);
}

/* All land / all water maps and a 1x1 map must not crash. */
static void test_degenerate(void) {
    uint8_t one = TERRAIN_LAND_BIT | 3;
    OfeMap *m = ofe_map_from_terrain(1, 1, &one);
    OfeMapStatic *ms = ofe_mapstatic_compute(m);
    CHECK(ms && ms->stride == 1 && ms->num_landmasses == 1 && ms->num_water_bodies == 0);
    CHECK(isinf(ms->coast_land_min[0]) && isinf(ms->imp_min[0]));
    ofe_mapstatic_free(ms);
    ofe_map_release(m);
    uint8_t sea[20 * 10];
    memset(sea, TERRAIN_OCEAN_BIT, sizeof(sea));
    m = ofe_map_from_terrain(20, 10, sea);
    ms = ofe_mapstatic_compute(m);
    CHECK(ms && ms->num_landmasses == 0 && ms->num_water_bodies == 1 && isinf(ms->coast_water_min[0]));
    ofe_mapstatic_free(ms);
    ofe_map_release(m);
}

static void time_real(const char *name) {
    OfeMap *m = load_real_map(name);
    if (!m) { printf("  (map %s not available, skipped)\n", name); return; }
    double t0 = omp_get_wtime();
    OfeMapStatic *ms = ofe_mapstatic_compute(m);
    double t = omp_get_wtime() - t0;
    CHECK(ms != NULL);
    printf("  %-14s %dx%d stride %d coarse %dx%d patches %d landmasses %d water bodies %d: %.2f s (%d threads)\n", name,
           ms->width, ms->height, ms->stride, ms->cw, ms->ch, ms->num_patches, ms->num_landmasses,
           ms->num_water_bodies, t, omp_get_max_threads());
    if (strcmp(name, "world") == 0) {
        int saved = omp_get_max_threads();
        omp_set_num_threads(1);
        t0 = omp_get_wtime();
        OfeMapStatic *m1 = ofe_mapstatic_compute(m);
        printf("  %-14s single thread: %.2f s\n", name, omp_get_wtime() - t0);
        omp_set_num_threads(3);
        OfeMapStatic *m3 = ofe_mapstatic_compute(m);
        omp_set_num_threads(saved);
        CHECK(same_results(ms, m1) && same_results(ms, m3));
        ofe_mapstatic_free(m1);
        ofe_mapstatic_free(m3);
    }
    ofe_mapstatic_free(ms);
    ofe_map_release(m);
}

int main(void) {
    test_f16();
    test_synthetic();
    test_degenerate();
    const char *maps_dir = getenv("OFE_MAPS_DIR");
    if (maps_dir && *maps_dir) {
        printf("mapstatic timings:\n");
        const char *maps[] = {"world", "europe", "giantworldmap", "britannia"};
        for (unsigned i = 0; i < 4; i++) time_real(maps[i]);
    }
    return test_summary("test_mapstatic");
}

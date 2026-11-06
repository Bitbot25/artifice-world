/*
 * ofe_mapstatic.h - per-map static analysis for the critic's feature
 * extractor. Reads only an OfeMap; independent of the game simulation.
 *
 * Everything here is a pure function of the terrain bytes. The exact
 * definitions below are a contract: the same features are recomputed for
 * states replayed in the TypeScript engine. Change a definition => bump
 * OFE_MAPSTATIC_VERSION (caches are keyed by it). See docs/mapstatic.md.
 *
 * Conventions: arrays are row-major, coarse arrays are (ch, cw) with
 * cell = cy * cw + cx, full-res arrays are (height, width). Labels start at
 * 1 and 0 means "none". Distance tables are (n, n) uint16 holding IEEE half
 * floats: +inf (0x7C00) = unreachable / no such patch, finite values are
 * clamped to 65504.
 */
#ifndef OFE_MAPSTATIC_H
#define OFE_MAPSTATIC_H

#include <stdint.h>

#include "ofe.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Bump whenever any definition, array, name or dtype below changes. */
#define OFE_MAPSTATIC_VERSION 1

/* Terrain classes (value of the `tclass` array). Same numbering as the engine. */
enum {
    OFE_TC_OCEAN = 0,      /* water with the ocean bit */
    OFE_TC_LAKE = 1,       /* water without the ocean bit */
    OFE_TC_PLAINS = 2,     /* land, magnitude < 10 */
    OFE_TC_HIGHLAND = 3,   /* land, 10 <= magnitude < 20 */
    OFE_TC_MOUNTAIN = 4,   /* land, 20 <= magnitude < 31 */
    OFE_TC_IMPASSABLE = 5, /* land, magnitude 31 */
    OFE_TC_COUNT = 6
};

/* Attack tile cost (EngineConfig.terrainAttackBase tileCost). Not ownable
 * classes (water, impassable) have cost +inf. */
#define OFE_COST_PLAINS 16.5f
#define OFE_COST_HIGHLAND 20.0f
#define OFE_COST_MOUNTAIN 25.0f

/* Side length of a patch, in coarse cells (a patch is patch_k x patch_k cells). */
#define OFE_PATCH_K 4
/* Longest coarse side allowed when choosing the stride. */
#define OFE_COARSE_MAX 256

/* dtype codes in ofe_mapstatic_array_info. */
enum { OFE_MS_U8 = 0, OFE_MS_I32 = 1, OFE_MS_F32 = 2, OFE_MS_F16 = 3 /* uint16 bits */ };

typedef struct OfeMapStatic OfeMapStatic;

/* Typed view of the results; every pointer is owned by the OfeMapStatic. */
struct OfeMapStatic {
    int32_t version;
    int32_t width, height;
    int32_t stride;      /* smallest s with ceil(max(W,H)/s) <= 256 */
    int32_t cw, ch;      /* coarse grid: ceil(W/s) x ceil(H/s) */
    int32_t patch_k;     /* OFE_PATCH_K */
    int32_t pw, ph;      /* patch grid: ceil(cw/k) x ceil(ch/k) */
    int32_t num_patches; /* pw * ph; tables are num_patches x num_patches */
    int32_t num_landmasses, num_water_bodies;

    /* Full resolution (H, W). */
    uint8_t *tclass;     /* terrain class */
    uint8_t *shoreline;  /* terrain byte bit 6 */
    uint8_t *magnitude;  /* terrain byte bits 0-4 */
    int32_t *landmass;   /* 4-connected component of ownable land, 0 elsewhere */
    int32_t *water_body; /* 4-connected component of equal-class water, 0 on land */
    uint8_t *water_body_is_ocean;       /* (num_water_bodies + 1): 1 ocean, 0 lake; index 0 = 0 */

    /* Coarse (ch, cw). */
    int32_t *cell_land_tiles;  /* ownable land tiles in the cell */
    int32_t *cell_water_tiles; /* water tiles in the cell */
    float *land_cost, *water_cost;
    uint8_t *land_step[4], *water_step[4]; /* from_left, from_right, from_up, from_down */
    uint8_t *multi_landmass, *multi_water;
    int32_t *cell_landmass, *cell_water_body; /* majority labels */
    float *coast_land_min, *coast_land_mean;
    float *coast_water_min, *coast_water_mean;
    float *imp_min, *imp_mean;

    /* Patches (num_patches) and tables (num_patches^2). */
    int32_t *land_patch_rep, *water_patch_rep; /* representative cell or -1 */
    uint8_t *coastal_patch;
    int32_t *coastal_anchor;                   /* anchor water cell or -1 */
    uint16_t *land_dist, *water_dist, *coastal_dist;

    /* Registry used by the generic accessors (filled by compute). */
    struct {
        const char *name;
        int dtype, ndim;
        int64_t shape[3];
        const void *data;
    } arrays[64];
    int32_t num_arrays;
};

/* Computes all outputs. NULL on bad input. Deterministic for any thread count. */
OfeMapStatic *ofe_mapstatic_compute(const OfeMap *map);
void ofe_mapstatic_free(OfeMapStatic *ms);

/* Generic accessors (what the Python bindings use). Index 0..num-1. */
int32_t ofe_mapstatic_num_arrays(const OfeMapStatic *ms);
/* Returns 0 on success. Scalars have ndim 0 and one int32 element. */
int ofe_mapstatic_array_info(const OfeMapStatic *ms, int32_t i, const char **name, int *dtype, int *ndim,
                             int64_t shape[3], const void **data);
/* Array by name, or NULL. */
const void *ofe_mapstatic_array(const OfeMapStatic *ms, const char *name);

/* Attack tile cost of a terrain class (+inf if not ownable land). */
float ofe_mapstatic_attack_cost(int tclass);
/* float32 -> IEEE half bits (round to nearest even, clamped to 65504, inf kept). */
uint16_t ofe_mapstatic_f32_to_f16(float v);
float ofe_mapstatic_f16_to_f32(uint16_t h);

#ifdef __cplusplus
}
#endif
#endif

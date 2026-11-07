/*
 * internal.h - the engine's data layout and the functions modules share.
 *
 * Data-oriented layout:
 *   - The map (terrain) is immutable and shared by every game and clone.
 *   - Tiles: one uint16 state word per tile (owner + flags) plus the tile's
 *     position in its owner's border list, stored in fixed-size chunks that
 *     clones share copy-on-write.
 *   - Players, units and attacks are struct-of-arrays tables. Their columns
 *     are listed once in an X-macro (PLAYER_COLUMNS etc.), which generates
 *     the struct, allocation, copy and free code. To add a column, add one
 *     line to the list.
 *   - Variable-length per-row data (border lists, attack heaps, unit paths)
 *     lives in per-row vectors next to the table.
 *
 * Module map (one .c file per subsystem):
 *   map.c       map loading, terrain helpers, the mini (half-res) pathfinding map
 *   tiles.c     copy-on-write tile store, conquer/relinquish, borders
 *   game.c      create/destroy/clone, the tick loop, queries
 *   player.c    troop/gold growth, alliances, relations, embargoes, donations
 *   attack.c    land attacks
 *   cluster.c   annexing surrounded enclaves
 *   spawn.c     spawn phase and spawn placement
 *   intent.c    intent queue and dispatch
 *   win.c       win check and overtime
 *   events.c    event log
 *   edit.c      state-edit API
 *   units.c     unit table, structures, construction, capture  (subsystem)
 *   ships.c     transport/trade ships, warships (subsystem); routes: pathfind_ts.c
 *   nukes.c     atom/hydrogen bombs, MIRVs, SAMs               (subsystem)
 *   rail.c      factories, railroads, trains                    (subsystem)
 *   doomsday.c  doomsday clock                                  (subsystem)
 *   ai_nation.c, ai_tribe.c   built-in AI                       (subsystem)
 *   mapstatic.c per-map static analysis for feature extraction  (subsystem)
 */
#ifndef OFE_INTERNAL_H
#define OFE_INTERNAL_H

#include "../include/ofe.h"
#include "util.h"

/* 1 in the TS-parity build (make parity), see docs/ts_parity.md. */
#ifndef OFE_TS_PARITY
#define OFE_TS_PARITY 0
#endif

typedef int32_t TileRef;
typedef int32_t PlayerId; /* 0 = terra nullius */
typedef int32_t UnitId; /* opaque: (generation << UNIT_SLOT_BITS) | slot */

#define TICKS_PER_SECOND 10
#define MAX_PLAYERS 4095 /* owner ids are 12 bits */

/* ======================================================================== */
/* Map (immutable terrain)                                                   */
/* ======================================================================== */

/* OpenFront terrain byte. */
#define TERRAIN_LAND_BIT 0x80
#define TERRAIN_SHORELINE_BIT 0x40
#define TERRAIN_OCEAN_BIT 0x20
#define TERRAIN_MAGNITUDE_MASK 0x1f
#define TERRAIN_IMPASSABLE_MAGNITUDE 31

enum TerrainClass {
    TC_OCEAN = 0,
    TC_LAKE = 1,
    TC_PLAINS = 2, /* magnitude < 10 */
    TC_HIGHLAND = 3, /* magnitude < 20 */
    TC_MOUNTAIN = 4,
    TC_IMPASSABLE = 5, /* magnitude 31 land */
};

typedef struct Nation {
    char name[48];
    TileRef tile;
} Nation;

struct OfeMap {
    int32_t refcount; /* atomic */
    char name[64];
    int32_t width, height, num_tiles, num_land_tiles;
    uint8_t *terrain; /* OpenFront terrain byte per tile */
    uint8_t *tclass; /* TerrainClass per tile */

    /* Half-resolution map for pathfinding (OpenFront's "mini map"). */
    int32_t mini_width, mini_height;
    uint8_t *mini_terrain;

    Nation *nations;
    int32_t num_nations;
};

static inline bool map_is_land(const OfeMap *m, TileRef t) { return (m->terrain[t] & TERRAIN_LAND_BIT) != 0; }
static inline bool map_is_water(const OfeMap *m, TileRef t) { return (m->terrain[t] & TERRAIN_LAND_BIT) == 0; }
static inline bool map_is_ocean(const OfeMap *m, TileRef t) { return (m->terrain[t] & TERRAIN_OCEAN_BIT) != 0; }
static inline bool map_is_shoreline(const OfeMap *m, TileRef t) { return (m->terrain[t] & TERRAIN_SHORELINE_BIT) != 0; }
/* Land tile next to water. */
static inline bool map_is_shore(const OfeMap *m, TileRef t) {
    return (m->terrain[t] & (TERRAIN_LAND_BIT | TERRAIN_SHORELINE_BIT)) == (TERRAIN_LAND_BIT | TERRAIN_SHORELINE_BIT);
}
static inline int map_magnitude(const OfeMap *m, TileRef t) { return m->terrain[t] & TERRAIN_MAGNITUDE_MASK; }
static inline bool map_is_impassable(const OfeMap *m, TileRef t) { return m->tclass[t] == TC_IMPASSABLE; }
/* Land a player can own. */
static inline bool map_is_ownable(const OfeMap *m, TileRef t) {
    return m->tclass[t] >= TC_PLAINS && m->tclass[t] <= TC_MOUNTAIN;
}
static inline int32_t map_x(const OfeMap *m, TileRef t) { return t % m->width; }
static inline int32_t map_y(const OfeMap *m, TileRef t) { return t / m->width; }
static inline TileRef map_ref(const OfeMap *m, int32_t x, int32_t y) { return y * m->width + x; }
static inline bool map_in_bounds(const OfeMap *m, int32_t x, int32_t y) {
    return x >= 0 && y >= 0 && x < m->width && y < m->height;
}
static inline bool map_on_edge(const OfeMap *m, TileRef t) {
    int32_t x = map_x(m, t), y = map_y(m, t);
    return x == 0 || y == 0 || x == m->width - 1 || y == m->height - 1;
}
/* Writes the 4-neighbours of t into out (up to 4) and returns how many. */
static inline int map_neighbors4(const OfeMap *m, TileRef t, TileRef out[4]) {
    int n = 0;
    int32_t w = m->width, x = t % w;
    if (t >= w) out[n++] = t - w;
    if (t < m->num_tiles - w) out[n++] = t + w;
    if (x != 0) out[n++] = t - 1;
    if (x != w - 1) out[n++] = t + 1;
    return n;
}
static inline int map_neighbors8(const OfeMap *m, TileRef t, TileRef out[8]) {
    int n = 0;
    int32_t w = m->width, x = t % w, y = t / w;
    for (int dy = -1; dy <= 1; dy++) {
        int32_t yy = y + dy;
        if (yy < 0 || yy >= m->height) continue;
        for (int dx = -1; dx <= 1; dx++) {
            int32_t xx = x + dx;
            if ((dx | dy) == 0 || xx < 0 || xx >= w) continue;
            out[n++] = yy * w + xx;
        }
    }
    return n;
}
static inline int32_t map_manhattan(const OfeMap *m, TileRef a, TileRef b) {
    return abs(map_x(m, a) - map_x(m, b)) + abs(map_y(m, a) - map_y(m, b));
}
static inline int64_t map_dist2(const OfeMap *m, TileRef a, TileRef b) {
    int64_t dx = map_x(m, a) - map_x(m, b), dy = map_y(m, a) - map_y(m, b);
    return dx * dx + dy * dy;
}

/* ======================================================================== */
/* Tiles (copy-on-write chunks)                                              */
/* ======================================================================== */

#define TILE_CHUNK_BITS 12
#define TILE_CHUNK_SIZE (1 << TILE_CHUNK_BITS)
#define TILE_CHUNK_MASK (TILE_CHUNK_SIZE - 1)

/* Tile state word, same layout as OpenFront's. */
#define TS_OWNER_MASK 0x0fff
#define TS_FALLOUT_BIT 0x2000

typedef struct TileChunk {
    int32_t refcount; /* atomic; >1 means shared with a clone */
    uint16_t state[TILE_CHUNK_SIZE];
    /* Index of the tile in its owner's border list, -1 if not a border
     * tile. A border tile is an owned tile with a 4-neighbour that has a
     * different owner (or that is water / off-map-edge counts as no). */
    int32_t border_pos[TILE_CHUNK_SIZE];
#if OFE_TS_PARITY
    /* Position of the tile in its owner's insertion-ordered tile / border
     * sequence (TileSeq), -1 if absent. */
    int32_t tiles_seq_pos[TILE_CHUNK_SIZE];
    int32_t border_seq_pos[TILE_CHUNK_SIZE];
#endif
} TileChunk;

#if OFE_TS_PARITY
/* Insertion-ordered set of tiles with the semantics of OpenFront's TileSet
 * (a JS Set): iteration in insertion order, delete + add moves to the end,
 * deleted slots are tombstones (-1) skipped by iteration. Iterate with
 * `for (i = 0; i < s.len; i++) if (s.data[i] >= 0)` re-reading len each
 * step, and bracket loops that mutate with seq_iter_begin/end. */
typedef struct TileSeq {
    int32_t *data;
    int32_t len, cap, live;
} TileSeq;
#endif

typedef struct TileStore {
    int32_t num_chunks;
    TileChunk **chunks;
} TileStore;

static inline uint16_t tile_state(const TileStore *ts, TileRef t) {
    return ts->chunks[t >> TILE_CHUNK_BITS]->state[t & TILE_CHUNK_MASK];
}
static inline PlayerId tile_owner_raw(const TileStore *ts, TileRef t) { return tile_state(ts, t) & TS_OWNER_MASK; }
static inline int32_t tile_border_pos_raw(const TileStore *ts, TileRef t) {
    return ts->chunks[t >> TILE_CHUNK_BITS]->border_pos[t & TILE_CHUNK_MASK];
}
/* Makes the chunk holding t private to this store (copy-on-write). */
TileChunk *tiles_chunk_mut(TileStore *ts, TileRef t);

void tiles_init(TileStore *ts, int32_t num_tiles);
void tiles_free(TileStore *ts);
void tiles_share(TileStore *dst, const TileStore *src); /* dst must be empty */

/* ======================================================================== */
/* Tables (struct-of-arrays via X-macros)                                    */
/* ======================================================================== */

#define DECLARE_COLUMN(type, name) type *name;
#define ALLOC_COLUMN(type, name) t->name = (type *)ofe_xcalloc((size_t)cap, sizeof(type));
#define GROW_COLUMN(type, name)                                                     \
    t->name = (type *)ofe_xrealloc(t->name, (size_t)cap * sizeof(type));             \
    memset(t->name + t->cap, 0, (size_t)(cap - t->cap) * sizeof(type));
#define COPY_COLUMN(type, name) \
    if (src->cap) memcpy(dst->name, src->name, (size_t)src->cap * sizeof(type));
#define FREE_COLUMN(type, name) free(t->name);
#define HASH_COLUMN(type, name) h = hash_bytes(h, t->name, (size_t)t->count * sizeof(type));

/* ---- Players ------------------------------------------------------------
 * Row 0 is terra nullius and stays zero. Rows 1..count-1 are players.
 */
/* Array-typed columns need a typedef so the X-macros stay uniform. */
typedef int32_t UnitTypeCounts[OFE_UNIT_TYPE_COUNT];
typedef int32_t AiState[10];

#define PLAYER_COLUMNS(X)                                                           \
    X(uint8_t, type) /* OfePlayerType */                                            \
    X(uint8_t, spawned)                                                             \
    X(uint32_t, id_hash) /* simpleHash of the TS player id (see game_assign_ids) */  \
    X(uint8_t, disconnected)                                                        \
    X(uint8_t, dead) /* was alive, now has no tiles */                              \
    X(int16_t, team) /* -1 = none */                                                \
    X(int16_t, nation_index) /* index into map nations, -1 if not a nation */       \
    X(int32_t, relation_row) /* row in the relation matrix, -1 if none */           \
    X(int64_t, troops)                                                              \
    X(int64_t, gold)                                                                \
    X(int32_t, num_tiles)                                                           \
    X(TileRef, spawn_tile) /* -1 before spawning */                                 \
    X(int32_t, last_tile_change) /* tick */                                         \
    X(int32_t, cluster_calc_tick) /* last enclave check */                          \
    X(int32_t, traitor_tick) /* -1 = never */                                       \
    X(int32_t, betrayals)                                                           \
    X(int32_t, embargo_all_tick) /* last embargo-all, for cooldown */               \
    X(int32_t, last_donate_tick)                                                    \
    X(int32_t, last_target_tick)                                                    \
    X(PlayerId, target) /* last player this one targeted */                         \
    X(int32_t, doomsday_tick) /* tick entered the danger zone, -1 = safe */         \
    X(int32_t, attacks_sent)                                                        \
    X(int32_t, trade_spawn_rejections) /* trade ship pity timer */                  \
    X(int64_t, gold_earned)                                                         \
    X(int64_t, trade_gold)                                                          \
    X(int64_t, train_gold)                                                          \
    X(UnitTypeCounts, unit_count) /* alive units by type, incl. construction */     \
    X(UnitTypeCounts, unit_levels) /* summed levels of finished structures */       \
    X(UnitTypeCounts, units_built) /* units/levels bought, for costs */             \
    X(UnitTypeCounts, units_constructing) /* structures under construction */      \
    X(uint64_t, ai_rng) /* per-player AI random stream */                           \
    X(int32_t, ai_next_tick) /* next tick the AI acts */                            \
    X(AiState, ai_state) /* AI scratch; ai_*.c owns the meaning */

typedef struct PlayerTable {
    int32_t count, cap; /* count includes row 0 */
    PLAYER_COLUMNS(DECLARE_COLUMN)
    /* Per-player variable-length data. */
    IntVec *border; /* border tiles; tile's index lives in TileChunk.border_pos */
    char (*name)[48];
#if OFE_TS_PARITY
    TileSeq *tiles_seq; /* player.tiles() in TS insertion order */
    TileSeq *border_seq; /* player.borderTiles() in TS insertion order */
#endif
} PlayerTable;

/* ---- Attacks ------------------------------------------------------------- */
#define ATTACK_COLUMNS(X)                                                           \
    X(uint8_t, active)                                                              \
    X(int32_t, retreat_at) /* tick the ordered retreat executes, -1 = none */       \
    X(int32_t, id) /* stable id for cancel intents */                               \
    X(PlayerId, attacker)                                                           \
    X(PlayerId, target)                                                             \
    X(double, troops)                                                               \
    X(TileRef, source_tile) /* -1 = land attack; boat landing tile otherwise */     \
    X(TsRandom, rng) /* AttackExecution's PseudoRandom(123) */

typedef struct AttackTable {
    int32_t count, cap;
    ATTACK_COLUMNS(DECLARE_COLUMN)
    TileHeap *to_conquer; /* candidate tiles, lowest priority first */
    IntSet *front; /* target tiles adjacent to the attacker ("border size") */
} AttackTable;

/* ---- Units ----------------------------------------------------------------
 * Slots are reused through a free list; ids carry a generation so stale ids
 * are detected. Column meanings for the aux fields are documented per unit
 * type in units.c / ships.c / nukes.c / rail.c.
 */
#define UNIT_SLOT_BITS 20
#define UNIT_SLOT_MASK ((1 << UNIT_SLOT_BITS) - 1)

#include "pathfind_ts.h" /* TsWaterStepper, held per unit */

#define UNIT_COLUMNS(X)                                                             \
    X(uint8_t, alive)                                                               \
    X(uint8_t, type) /* OfeUnitType */                                              \
    X(uint8_t, flags) /* OFE_UF_* */                                                \
    X(uint8_t, level)                                                               \
    X(uint16_t, generation)                                                         \
    X(PlayerId, owner)                                                              \
    X(TileRef, tile)                                                                \
    X(TileRef, target_tile) /* -1 if none */                                        \
    X(UnitId, target_unit) /* -1 if none */                                         \
    X(TileRef, src_tile) /* where it came from (ships, nukes, trains) */            \
    X(double, troops) /* transport ship troops */                                   \
    X(int32_t, health)                                                              \
    X(int32_t, created_tick)                                                        \
    X(int32_t, timer) /* construction end / cooldown end / generic, in ticks */     \
    X(int32_t, path_pos) /* index into the unit's path */                           \
    X(PlayerId, last_owner) /* previous owner (capture, trade origin) */            \
    X(int32_t, aux0)                                                                \
    X(int32_t, aux1)                                                                \
    X(int32_t, aux2)                                                                \
    X(int32_t, aux3)                                                                \
    X(double, auxf)

typedef struct UnitTable {
    int32_t count, cap; /* count = high-water mark of used slots */
    UNIT_COLUMNS(DECLARE_COLUMN)
    IntVec *path; /* per-unit path (tiles), may be empty */
    TsWaterStepper *pf; /* per-ship route state (OpenFront's WaterPathFinder); plain data, deep-copied */
    IntVec free_slots;
    int32_t num_alive;
} UnitTable;

static inline int32_t unit_slot(UnitId id) { return id & UNIT_SLOT_MASK; }

/* ---- Alliances and alliance requests -------------------------------------- */
typedef struct Alliance {
    PlayerId a, b;
    int32_t created_tick;
    int32_t expires_tick;
    uint8_t extend_a, extend_b; /* each side asked to extend */
} Alliance;
OFE_VEC(AllianceVec, Alliance)

typedef struct AllianceRequest {
    PlayerId requestor, recipient;
    int32_t created_tick;
} AllianceRequest;
OFE_VEC(AllianceRequestVec, AllianceRequest)

typedef struct Embargo {
    PlayerId from, to;
    int32_t created_tick;
    uint8_t temporary;
} Embargo;
OFE_VEC(EmbargoVec, Embargo)

/* A donation sent, kept for the 10 s cooldown per (donor, recipient). */
typedef struct Donation {
    PlayerId from, to;
    int32_t tick;
} Donation;
OFE_VEC(DonationVec, Donation)

OFE_VEC(IntentVec, OfeIntent)
OFE_VEC(EventVec, OfeEvent)

/* ======================================================================== */
/* Game                                                                      */
/* ======================================================================== */

typedef struct Subsystems Subsystems; /* defined in subsystems.h */

struct OfeGame {
    OfeMap *map;
    OfeConfig cfg;
    uint64_t seed;
    uint64_t rng; /* game-level stream (spawns, misc) */

    int32_t tick;
    int64_t id_seed; /* simpleHash(game id): seeds every OpenFront random stream */
    int32_t spawn_phase_ticks; /* resolved from cfg */
    int32_t start_tick; /* tick the spawn phase ended, -1 while spawning */
    int32_t winner; /* player id, 0 = none */
    int32_t winning_team; /* -1 = none */
    int32_t game_over;
    int32_t num_land_tiles;
    int32_t num_fallout_tiles;
    int32_t next_attack_id;
    int32_t mirvs_launched;

    TileStore tiles;
    PlayerTable players;
    AttackTable attacks;
    UnitTable units;

    AllianceVec alliances;
    AllianceRequestVec alliance_requests; /* pending */
    AllianceRequestVec past_requests; /* recent, for the request cooldown */
    EmbargoVec embargoes; /* (from, to) pairs, in creation order */
    /* Lookup cache over embargoes: bit `to` of row `from` is set iff (from, to)
     * is in the list. Attacks create temporary embargoes, so the list gets
     * long and trade/AI checks would otherwise scan it. */
    uint64_t *embargo_bits;
    int32_t embargo_rows, embargo_words; /* rows x 64-bit words per row */
    DonationVec donations; /* recent donations (cooldown per recipient) */

    /* Nation relations: relation[row * players.cap + other], in [-100, 100]. */
    float *relations;
    int32_t num_relation_rows;

    IntentVec pending_intents; /* applied at the start of the next tick */
    EventVec events;

    /* Scratch buffers (not part of the logical state; never cloned). */
    uint32_t *visit_stamp; /* per tile; flood fills compare against visit_gen */
    uint32_t visit_gen;
    IntVec scratch_a, scratch_b;
#if OFE_TS_PARITY
    int32_t seq_iter; /* >0 while a TileSeq is iterated: no compaction */
    struct TsExecs *ts; /* TS-order execution list (exec_ts.c) */
    void *cluster_scratch; /* cluster_ts.c */
#endif

    Subsystems *sub; /* per-subsystem state owned by units/ships/nukes/rail/ai */
};

/* ======================================================================== */
/* Shared functions                                                           */
/* ======================================================================== */

/* ---- tiles.c ---- */
static inline PlayerId tile_owner(const OfeGame *g, TileRef t) { return tile_owner_raw(&g->tiles, t); }
static inline bool tile_has_owner(const OfeGame *g, TileRef t) { return tile_owner_raw(&g->tiles, t) != 0; }
static inline bool tile_has_fallout(const OfeGame *g, TileRef t) { return (tile_state(&g->tiles, t) & TS_FALLOUT_BIT) != 0; }
static inline bool tile_is_border(const OfeGame *g, TileRef t) { return tile_border_pos_raw(&g->tiles, t) >= 0; }
/* Gives tile t to `owner` (0 = relinquish). Updates tile counts, border
 * lists, fallout, events. No-op if already owned by `owner`. */
void tile_set_owner(OfeGame *g, TileRef t, PlayerId owner);
void tile_set_fallout(OfeGame *g, TileRef t, bool fallout);
#if OFE_TS_PARITY
void tileseq_free(TileSeq *s);
void tileseq_copy(TileSeq *dst, const TileSeq *src);
static inline void tile_seq_iter_begin(OfeGame *g) { g->seq_iter++; }
static inline void tile_seq_iter_end(OfeGame *g) { g->seq_iter--; }
#endif
/* Does t have a 4-neighbour owned by p? */
bool tile_borders_player(const OfeGame *g, TileRef t, PlayerId p);
/* Fresh flood-fill stamp. visit_stamp[t] == gen marks t visited. */
uint32_t tiles_new_visit_gen(OfeGame *g);
/* All tiles of player p (flood from its borders); result in out. */
void tiles_collect_player(OfeGame *g, PlayerId p, IntVec *out);

/* ---- player.c ---- */
static inline bool player_valid(const OfeGame *g, PlayerId p) { return p > 0 && p < g->players.count; }
static inline bool player_alive(const OfeGame *g, PlayerId p) { return g->players.num_tiles[p] > 0; }
bool player_is_traitor(const OfeGame *g, PlayerId p);
bool player_same_team(const OfeGame *g, PlayerId a, PlayerId b);
bool player_allied(const OfeGame *g, PlayerId a, PlayerId b);
/* Same team or allied (and `other` connected). */
bool player_friendly(const OfeGame *g, PlayerId self, PlayerId other);
bool player_is_immune(const OfeGame *g, PlayerId p);
bool player_can_attack_player(const OfeGame *g, PlayerId attacker, PlayerId target);
bool player_shares_border(const OfeGame *g, PlayerId a, PlayerId b);
double player_max_troops(const OfeGame *g, PlayerId p);
double player_troop_increase(const OfeGame *g, PlayerId p);
int64_t player_gold_rate(const OfeGame *g, PlayerId p);
void player_add_troops(OfeGame *g, PlayerId p, double troops);
/* Removes floor(troops), at most what the player has. Returns the amount removed. */
double player_remove_troops(OfeGame *g, PlayerId p, double troops);
void player_add_gold(OfeGame *g, PlayerId p, int64_t gold);
int64_t player_remove_gold(OfeGame *g, PlayerId p, int64_t gold);
double player_relation(const OfeGame *g, PlayerId self, PlayerId other);
void player_update_relation(OfeGame *g, PlayerId self, PlayerId other, double delta);
bool player_embargoed(const OfeGame *g, PlayerId from, PlayerId to); /* from embargoes to; O(1) */
void embargo_bits_copy(OfeGame *dst, const OfeGame *src);
bool player_can_trade(const OfeGame *g, PlayerId a, PlayerId b);
void player_add_embargo(OfeGame *g, PlayerId from, PlayerId to, bool temporary);
void player_stop_embargo(OfeGame *g, PlayerId from, PlayerId to);
/* Conquest bookkeeping when `conquered` is wiped out (gold transfer etc). */
void player_conquered(OfeGame *g, PlayerId conqueror, PlayerId conquered);
void players_tick(OfeGame *g); /* growth, expiry, death checks */
#if OFE_TS_PARITY
bool player_exec_tick_ts(OfeGame *g, PlayerId p);
void player_decay_relations(OfeGame *g, PlayerId p);
void alliances_expire_of(OfeGame *g, PlayerId p);
void alliance_requests_tick(OfeGame *g);
#endif

/* Alliances */
int32_t alliance_find(const OfeGame *g, PlayerId a, PlayerId b); /* index or -1 */
bool alliance_can_request(const OfeGame *g, PlayerId from, PlayerId to);
void alliance_request(OfeGame *g, PlayerId from, PlayerId to);
void alliance_accept(OfeGame *g, PlayerId requestor, PlayerId recipient);
void alliance_reject(OfeGame *g, PlayerId requestor, PlayerId recipient);
void alliance_form(OfeGame *g, PlayerId a, PlayerId b, int32_t duration);
void alliance_break(OfeGame *g, PlayerId breaker, PlayerId other);
void alliance_request_extension(OfeGame *g, PlayerId from, PlayerId to);
void alliances_tick(OfeGame *g);
void player_donate_troops(OfeGame *g, PlayerId from, PlayerId to, double troops);
void player_donate_gold(OfeGame *g, PlayerId from, PlayerId to, double gold);

/* ---- attack.c ---- */
/* Starts a land attack (or boat landing when source_tile >= 0). troops < 0
 * means the default amount. remove_troops=false when the troops were already
 * taken (boats). Returns the attack id or -1. */
int32_t attack_start(OfeGame *g, PlayerId attacker, PlayerId target, double troops, TileRef source_tile, bool remove_troops);
void attack_order_retreat(OfeGame *g, PlayerId owner, int32_t attack_id);
void attacks_tick(OfeGame *g);
void attack_tick_row(OfeGame *g, int32_t row); /* one attack's tick */
int32_t attack_row_by_id(const OfeGame *g, int32_t id); /* -1 if gone */
/* Sum of troops in outgoing attacks of p. */
double attack_outgoing_troops(const OfeGame *g, PlayerId p);
int32_t attack_count_outgoing(const OfeGame *g, PlayerId p);
/* Troops in the largest attack from `attacker` on `target`, 0 if none. */
double attack_troops_against(const OfeGame *g, PlayerId attacker, PlayerId target);

/* ---- cluster.c ---- */
void cluster_check_player(OfeGame *g, PlayerId p);
#if OFE_TS_PARITY
void cluster_check_player_ts(OfeGame *g, PlayerId p); /* cluster_ts.c */
void cluster_ts_free(OfeGame *g);
void game_tick_ts(OfeGame *g); /* exec_ts.c: one tick of the TS execution list */
void ts_execs_free(OfeGame *g);
void ts_execs_copy(OfeGame *dst, const OfeGame *src);
#endif

/* ---- spawn.c ---- */
/* Spawns (or respawns, during the spawn phase) p around center. Returns true
 * on success. center < 0 picks a random spot. */
bool spawn_player(OfeGame *g, PlayerId p, TileRef center);
TileRef spawn_random_tile(OfeGame *g, PlayerId p, uint64_t *rng);
void spawn_phase_end(OfeGame *g);

/* ---- intent.c ---- */
void intents_apply(OfeGame *g);
void intent_apply_one(OfeGame *g, const OfeIntent *in);
/* AI modules issue actions through this (same validation as agents). */
void game_issue(OfeGame *g, const OfeIntent *in);

/* ---- win.c ---- */
int32_t win_threshold_tenths(const OfeGame *g);
void win_check(OfeGame *g);
double game_elapsed_seconds(const OfeGame *g);
bool game_in_spawn_phase(const OfeGame *g);

/* ---- events.c ---- */
void event_emit(OfeGame *g, int32_t type, int32_t a, int32_t b, int32_t c, int32_t d, double v);

/* ---- game.c ---- */
uint64_t hash_bytes(uint64_t h, const void *data, size_t n);

#include "subsystems.h"

#endif /* OFE_INTERNAL_H */

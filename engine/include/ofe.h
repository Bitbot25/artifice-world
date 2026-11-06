/*
 * ofe.h - public C API of the Artifice OpenFront engine (OFE).
 *
 * An approximation of OpenFront.io's simulation for RL training. It follows
 * the live game's rules and constants (combat, troop and gold formulas, unit
 * ranges, win threshold, overtime, doomsday clock) but is NOT bit-exact with
 * the TypeScript engine: it is only deterministic against itself. The same
 * map, config, seed and intents always give the same game, on any thread
 * count.
 *
 * Conventions
 *   - A tile is an int32 index: tile = y * width + x.
 *   - Player ids are small integers. 0 is "terra nullius" (nobody).
 *     Real players are 1..ofe_game_num_players(g).
 *   - Unit ids are opaque int32 handles (slot + generation); a stale id never
 *     aliases a newer unit.
 *   - 1 tick = 100 ms of game time.
 *   - Every struct here is plain data so Python (ctypes / numpy) can mirror it.
 *     If you change a struct, update python/artifice_engine/_lib.py too.
 */
#ifndef OFE_H
#define OFE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define OFE_API_VERSION 1

/* Bumped whenever a rule constant or formula changes in a way that shifts
 * game outcomes. Feature extractors record it next to each training state. */
#define OFE_RULES_VERSION 1

typedef struct OfeMap OfeMap;
typedef struct OfeGame OfeGame;

/* ------------------------------------------------------------------------ */
/* Enums                                                                     */
/* ------------------------------------------------------------------------ */

enum OfePlayerType {
    OFE_PLAYER_HUMAN = 0, /* agent-controlled (an RL policy or a script) */
    OFE_PLAYER_NATION = 1, /* built-in nation AI (named, from the map) */
    OFE_PLAYER_BOT = 2, /* built-in tribe AI ("bots" in OpenFront) */
};

enum OfeDifficulty {
    OFE_DIFFICULTY_EASY = 0,
    OFE_DIFFICULTY_MEDIUM = 1,
    OFE_DIFFICULTY_HARD = 2,
    OFE_DIFFICULTY_IMPOSSIBLE = 3,
};

enum OfeGameMode {
    OFE_MODE_FFA = 0,
    OFE_MODE_TEAM = 1,
};

enum OfeUnitType {
    OFE_UNIT_TRANSPORT_SHIP = 0,
    OFE_UNIT_WARSHIP,
    OFE_UNIT_SHELL,
    OFE_UNIT_SAM_MISSILE,
    OFE_UNIT_PORT,
    OFE_UNIT_ATOM_BOMB,
    OFE_UNIT_HYDROGEN_BOMB,
    OFE_UNIT_TRADE_SHIP,
    OFE_UNIT_MISSILE_SILO,
    OFE_UNIT_DEFENSE_POST,
    OFE_UNIT_SAM_LAUNCHER,
    OFE_UNIT_CITY,
    OFE_UNIT_MIRV,
    OFE_UNIT_MIRV_WARHEAD,
    OFE_UNIT_TRAIN,
    OFE_UNIT_FACTORY,
    OFE_UNIT_TYPE_COUNT
};

enum OfeIntentType {
    OFE_INTENT_NONE = 0,
    OFE_INTENT_SPAWN, /* tile */
    OFE_INTENT_ATTACK, /* target (0 = terra nullius), troops (<0: default 1/5) */
    OFE_INTENT_CANCEL_ATTACK, /* target = attack id */
    OFE_INTENT_BOAT_ATTACK, /* tile = destination, troops */
    OFE_INTENT_CANCEL_BOAT, /* target = unit id */
    OFE_INTENT_ALLIANCE_REQUEST, /* target = recipient */
    OFE_INTENT_ALLIANCE_REJECT, /* target = requestor */
    OFE_INTENT_ALLIANCE_EXTENSION, /* target = ally */
    OFE_INTENT_BREAK_ALLIANCE, /* target = ally */
    OFE_INTENT_TARGET_PLAYER, /* target */
    OFE_INTENT_DONATE_GOLD, /* target, troops = gold amount (<0: all/default) */
    OFE_INTENT_DONATE_TROOPS, /* target, troops (<0: default 1/3) */
    OFE_INTENT_BUILD_UNIT, /* unit_type, tile, amount, flags bit0 = rocket up */
    OFE_INTENT_EMBARGO, /* target, flags bit0: 1 = start, 0 = stop */
    OFE_INTENT_EMBARGO_ALL, /* flags bit0: 1 = start, 0 = stop */
    OFE_INTENT_MOVE_WARSHIP, /* target = unit id, tile */
    OFE_INTENT_MARK_DISCONNECTED, /* flags bit0: 1 = disconnected */
    OFE_INTENT_UPGRADE_STRUCTURE, /* target = unit id, amount */
    OFE_INTENT_DELETE_UNIT, /* target = unit id */
    OFE_INTENT_TYPE_COUNT
};

/* Event types written to the per-tick event log. Field meanings per type are
 * listed in OfeEvent below. */
enum OfeEventType {
    OFE_EV_TILE_OWNER = 0, /* a=tile b=old owner c=new owner */
    OFE_EV_UNIT_CREATED, /* a=unit b=unit type c=owner d=tile */
    OFE_EV_UNIT_DESTROYED, /* a=unit b=unit type c=owner d=destroyer (0 = none) */
    OFE_EV_UNIT_CAPTURED, /* a=unit b=unit type c=old owner d=new owner */
    OFE_EV_ATTACK_STARTED, /* a=attack b=attacker c=target d=source tile (-1 land) v=troops */
    OFE_EV_ATTACK_ENDED, /* a=attack b=attacker c=target d=reason (OfeAttackEnd) v=troops left */
    OFE_EV_ALLIANCE_REQUEST, /* b=requestor c=recipient */
    OFE_EV_ALLIANCE_FORMED, /* b, c = the two players */
    OFE_EV_ALLIANCE_BROKEN, /* b=breaker c=other (b becomes traitor) */
    OFE_EV_ALLIANCE_EXPIRED, /* b, c */
    OFE_EV_ALLIANCE_REJECTED, /* b=requestor c=recipient */
    OFE_EV_PLAYER_SPAWNED, /* b=player d=spawn tile */
    OFE_EV_PLAYER_DIED, /* b=player c=conqueror (0 = none) */
    OFE_EV_PLAYER_CONQUERED, /* b=conqueror c=conquered v=gold taken */
    OFE_EV_PLAYER_DISCONNECTED, /* b=player c=1 disconnected / 0 reconnected */
    OFE_EV_NUKE_LAUNCHED, /* a=unit b=unit type c=owner d=target tile */
    OFE_EV_NUKE_DETONATED, /* a=unit b=unit type c=owner d=tile v=tiles destroyed */
    OFE_EV_EMBARGO, /* b=player c=target d=1 start / 0 stop */
    OFE_EV_DONATION, /* b=from c=to d=0 troops / 1 gold v=amount */
    OFE_EV_TARGET, /* b=player c=target */
    OFE_EV_SPAWN_PHASE_END, /* a=tick */
    OFE_EV_GAME_OVER, /* b=winner player (0 if team) c=winning team (-1 if FFA) */
    OFE_EV_DOOMSDAY, /* b=player c=1 entered / 0 left the danger zone */
    OFE_EV_TYPE_COUNT
};

enum OfeAttackEnd {
    OFE_ATTACK_END_FINISHED = 0, /* nothing left to conquer */
    OFE_ATTACK_END_RETREAT = 1, /* cancelled by its owner (25% malus vs players) */
    OFE_ATTACK_END_DEPLETED = 2, /* troops ran out */
    OFE_ATTACK_END_MERGED = 3, /* absorbed into / cancelled by another attack */
    OFE_ATTACK_END_FRIENDLY = 4, /* target became an ally or teammate */
};

/* OfePlayerInfo.flags */
#define OFE_PF_ALIVE (1u << 0)
#define OFE_PF_SPAWNED (1u << 1)
#define OFE_PF_DISCONNECTED (1u << 2)
#define OFE_PF_TRAITOR (1u << 3)
#define OFE_PF_IN_DOOMSDAY (1u << 4)
#define OFE_PF_IMMUNE (1u << 5)

/* OfeUnitInfo.flags */
#define OFE_UF_UNDER_CONSTRUCTION (1u << 0)
#define OFE_UF_RETREATING (1u << 1)
#define OFE_UF_MARKED_FOR_DELETION (1u << 2)
#define OFE_UF_COOLDOWN (1u << 3)

/* Event mask bits (OfeConfig.event_mask): 1 << OfeEventType. */
#define OFE_EVENTS_ALL 0xffffffffu
#define OFE_EVENTS_NO_TILES (OFE_EVENTS_ALL & ~(1u << OFE_EV_TILE_OWNER))

/* ------------------------------------------------------------------------ */
/* Plain-data structs                                                        */
/* ------------------------------------------------------------------------ */

typedef struct OfeConfig {
    int32_t difficulty; /* OfeDifficulty, scales nation strength */
    int32_t game_mode; /* OfeGameMode */
    int32_t num_teams; /* team games only (2..7) */
    int32_t num_humans; /* agent players: ids 1..num_humans */
    int32_t num_nations; /* -1 = every nation in the manifest, 0 = none */
    int32_t num_bots; /* tribes */
    int32_t spawn_phase_ticks; /* -1 = OpenFront default (200, 150 random spawn) */
    int32_t random_spawn; /* humans are placed randomly, spawn intents ignored */
    int32_t auto_spawn_humans; /* place humans that didn't pick a tile when spawn ends */
    int32_t instant_build;
    int32_t infinite_gold;
    int32_t infinite_troops;
    int32_t donate_gold;
    int32_t donate_troops;
    int32_t disable_alliances;
    int32_t alliance_duration_minutes; /* 0 = default (5 min) */
    uint32_t disabled_units; /* bit mask of 1 << OfeUnitType */
    int32_t win_threshold_tenths; /* 0 = default 800 (80.0% of land) */
    int32_t overtime_enabled;
    int32_t overtime_start_minutes; /* default 30 */
    int32_t doomsday_enabled;
    int32_t doomsday_speed; /* 0 slow, 1 normal, 2 fast */
    int32_t max_timer_minutes; /* 0 = no timer (170 min hard cap still applies) */
    int32_t spawn_immunity_ticks; /* -1 = default 50 */
    int64_t starting_gold;
    double gold_multiplier; /* 0 = default 1.0 */
    uint32_t event_mask; /* which OfeEventType to log, see OFE_EVENTS_* */
    int32_t humans_use_ai; /* 1 = built-in nation AI also drives human players (testing) */
    int32_t reserved[8];
} OfeConfig;

typedef struct OfeIntent {
    int32_t type; /* OfeIntentType */
    int32_t player; /* issuing player id */
    int32_t target; /* player id, unit id or attack id (see OfeIntentType) */
    int32_t tile;
    int32_t unit_type; /* OfeUnitType for BUILD_UNIT */
    int32_t amount; /* upgrade / build count, default 1 */
    int32_t flags;
    int32_t reserved;
    double troops; /* troops or gold amount; < 0 means "use the default" */
} OfeIntent;

typedef struct OfePlayerInfo {
    int32_t id;
    int32_t type; /* OfePlayerType */
    int32_t team; /* -1 = no team */
    uint32_t flags; /* OFE_PF_* */
    int32_t num_tiles;
    int32_t num_border_tiles;
    int32_t spawn_tile; /* -1 before spawning */
    int32_t num_outgoing_attacks;
    int64_t troops;
    int64_t gold;
    double max_troops;
    double troop_rate; /* troops added next tick */
    double outgoing_attack_troops; /* troops committed to land attacks */
    double boat_troops; /* troops at sea on transport ships */
    int32_t units[OFE_UNIT_TYPE_COUNT]; /* units owned, by type (incl. under construction) */
    int32_t unit_levels[OFE_UNIT_TYPE_COUNT]; /* summed structure levels, by type */
    int32_t traitor_ticks_left;
    int32_t doomsday_ticks; /* ticks below the doomsday bar (0 = safe) */
    int32_t reserved[4];
} OfePlayerInfo;

typedef struct OfeUnitInfo {
    int32_t id;
    int32_t type; /* OfeUnitType */
    int32_t owner;
    int32_t tile;
    int32_t target_tile; /* -1 if none */
    int32_t level;
    uint32_t flags; /* OFE_UF_* */
    int32_t health; /* warships */
    double troops; /* transport ships */
    int32_t ticks_left; /* construction / cooldown ticks remaining */
    int32_t reserved;
} OfeUnitInfo;

typedef struct OfeAttackInfo {
    int32_t id;
    int32_t attacker;
    int32_t target; /* 0 = terra nullius */
    int32_t source_tile; /* -1 = land attack, else boat landing tile */
    int32_t border_size; /* target tiles currently on the attack front */
    int32_t retreating;
    double troops;
} OfeAttackInfo;

typedef struct OfeAllianceInfo {
    int32_t a, b;
    int32_t created_tick;
    int32_t expires_tick;
} OfeAllianceInfo;

typedef struct OfeEvent {
    int32_t tick;
    int32_t type; /* OfeEventType */
    int32_t a, b, c, d;
    double v;
} OfeEvent;

/* ------------------------------------------------------------------------ */
/* Maps                                                                      */
/* ------------------------------------------------------------------------ */

/* Loads an OpenFront map directory (manifest.json, map.bin, map4x.bin,
 * map16x.bin). compact=1 uses the 4x map as the game map, like the
 * "compact" map size. Returns NULL on error (message on stderr). */
OfeMap *ofe_map_load(const char *map_dir, int compact);

/* Builds a map from a raw terrain buffer in OpenFront's byte format
 * (bit 7 land, bit 6 shoreline, bit 5 ocean, bits 0-4 magnitude). The
 * half-resolution pathfinding map is derived. The buffer is copied. */
OfeMap *ofe_map_from_terrain(int32_t width, int32_t height, const uint8_t *terrain);

/* Adds a nation spawn point (maps from ofe_map_load already have theirs). */
void ofe_map_add_nation(OfeMap *map, const char *name, int32_t x, int32_t y);

void ofe_map_retain(OfeMap *map);
void ofe_map_release(OfeMap *map);

int32_t ofe_map_width(const OfeMap *map);
int32_t ofe_map_height(const OfeMap *map);
int32_t ofe_map_num_land_tiles(const OfeMap *map);
const uint8_t *ofe_map_terrain(const OfeMap *map);
int32_t ofe_map_num_nations(const OfeMap *map);
/* Writes the nation's spawn tile and name (name may be NULL). */
int32_t ofe_map_nation(const OfeMap *map, int32_t index, char *name, int32_t name_cap);
const char *ofe_map_name(const OfeMap *map);

/* ------------------------------------------------------------------------ */
/* Games                                                                     */
/* ------------------------------------------------------------------------ */

void ofe_config_default(OfeConfig *config);

/* Creates a game. The game holds a reference to the map. */
OfeGame *ofe_game_create(OfeMap *map, const OfeConfig *config, uint64_t seed);
void ofe_game_destroy(OfeGame *game);

/* Full independent copy. Tile arrays are shared copy-on-write, so a clone
 * costs O(players + units + attacks), not O(tiles). */
OfeGame *ofe_game_clone(const OfeGame *game);
/* Makes dst an exact copy of src (both must use the same map). Reuses dst's
 * buffers, so restoring a saved state in a search loop doesn't allocate
 * much. */
void ofe_game_copy_into(OfeGame *dst, const OfeGame *src);

/* Queues intents. They take effect at the end of the next tick (like
 * OpenFront's executions), so e.g. an attack first conquers one tick later.
 * Returns the number accepted (malformed intents are dropped). */
int32_t ofe_game_push_intents(OfeGame *game, const OfeIntent *intents, int32_t count);

/* Runs up to `ticks` ticks; stops early when the game ends. Returns the number
 * of ticks run. */
int32_t ofe_game_step(OfeGame *game, int32_t ticks);

/* Steps many independent games. Uses up to num_threads threads (0 = all
 * cores). Results don't depend on the thread count. */
void ofe_step_batch(OfeGame **games, int32_t count, int32_t ticks, int32_t num_threads);

/* ---- State queries ---- */
const OfeMap *ofe_game_map(const OfeGame *game);
int32_t ofe_game_tick(const OfeGame *game);
int32_t ofe_game_in_spawn_phase(const OfeGame *game);
int32_t ofe_game_is_over(const OfeGame *game);
/* Winner player id (FFA), or 0. For team games see ofe_game_winning_team. */
int32_t ofe_game_winner(const OfeGame *game);
int32_t ofe_game_winning_team(const OfeGame *game);
int32_t ofe_game_num_players(const OfeGame *game);
int32_t ofe_game_num_land_tiles(const OfeGame *game);
int32_t ofe_game_num_fallout_tiles(const OfeGame *game);
/* Current win bar in tenths of a percent of non-fallout land. */
int32_t ofe_game_win_threshold_tenths(const OfeGame *game);
uint64_t ofe_game_hash(const OfeGame *game); /* full-state checksum */

int32_t ofe_game_player_info(const OfeGame *game, int32_t player, OfePlayerInfo *out);
/* Fills out[0..n) for players 1..n; returns n. */
int32_t ofe_game_players(const OfeGame *game, OfePlayerInfo *out, int32_t cap);
/* Owner id per tile (width * height uint16). */
void ofe_game_owner_map(const OfeGame *game, uint16_t *out);
/* Raw tile state (bits 0-11 owner, bit 13 fallout). */
void ofe_game_tile_state(const OfeGame *game, uint16_t *out);
int32_t ofe_game_tile_owner(const OfeGame *game, int32_t tile);
int32_t ofe_game_units(const OfeGame *game, OfeUnitInfo *out, int32_t cap);
int32_t ofe_game_num_units(const OfeGame *game);
int32_t ofe_game_attacks(const OfeGame *game, OfeAttackInfo *out, int32_t cap);
int32_t ofe_game_alliances(const OfeGame *game, OfeAllianceInfo *out, int32_t cap);
/* Relation of nation `a` toward player `b` in [-100, 100]; 0 for non-nations. */
double ofe_game_relation(const OfeGame *game, int32_t a, int32_t b);
int32_t ofe_game_is_embargoed(const OfeGame *game, int32_t a, int32_t b);
int32_t ofe_game_player_name(const OfeGame *game, int32_t player, char *out, int32_t cap);
/* Gold cost of the next unit of this type for this player. */
int64_t ofe_game_unit_cost(const OfeGame *game, int32_t player, int32_t unit_type);
/* 1 if the player could build this unit type at this tile right now. */
int32_t ofe_game_can_build(const OfeGame *game, int32_t player, int32_t unit_type, int32_t tile);

/* ---- Event log ----
 * Events accumulate until drained. ofe_game_events returns a pointer valid
 * until the next step / drain / clear. */
int32_t ofe_game_events(const OfeGame *game, const OfeEvent **out);
int32_t ofe_game_drain_events(OfeGame *game, OfeEvent *out, int32_t cap);
void ofe_game_clear_events(OfeGame *game);

/* ---- State edits (sensitivity tests, scenario setup, snapshot import) ----
 * All return 0 on success, <0 on error, unless stated otherwise. Edits apply
 * immediately and keep every derived structure (borders, tile counts, unit
 * indexes) consistent. */
int32_t ofe_edit_set_owner(OfeGame *game, int32_t tile, int32_t owner);
int32_t ofe_edit_set_owner_map(OfeGame *game, const uint16_t *owners); /* whole map */
int32_t ofe_edit_set_fallout(OfeGame *game, int32_t tile, int32_t fallout);
int32_t ofe_edit_set_troops(OfeGame *game, int32_t player, double troops);
int32_t ofe_edit_set_gold(OfeGame *game, int32_t player, int64_t gold);
/* Returns the new unit id, or <0. */
int32_t ofe_edit_add_unit(OfeGame *game, int32_t unit_type, int32_t owner, int32_t tile, int32_t level);
int32_t ofe_edit_remove_unit(OfeGame *game, int32_t unit_id);
int32_t ofe_edit_set_alliance(OfeGame *game, int32_t a, int32_t b, int32_t allied, int32_t duration_ticks);
/* Launches a nuke (atom, hydrogen or MIRV) from src_tile to dst_tile
 * without the usual checks (silo, gold). Returns the unit id, or <0. */
int32_t ofe_edit_launch_nuke(OfeGame *game, int32_t unit_type, int32_t owner, int32_t src_tile, int32_t dst_tile);
int32_t ofe_edit_set_relation(OfeGame *game, int32_t nation, int32_t other, double value);
int32_t ofe_edit_set_disconnected(OfeGame *game, int32_t player, int32_t disconnected);
/* Ends the spawn phase now (spawning anyone who hasn't spawned). */
int32_t ofe_edit_end_spawn_phase(OfeGame *game);

#ifdef __cplusplus
}
#endif

#endif /* OFE_H */

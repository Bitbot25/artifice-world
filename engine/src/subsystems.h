/*
 * subsystems.h - the contract between the core (game loop, tiles, players,
 * attacks) and the unit / AI subsystems.
 *
 * Each subsystem owns:
 *   - an opaque per-game state struct (may be empty), created, copied and
 *     freed through the *_state_* functions below. Copies must be deep: a
 *     clone and its parent never share mutable memory (map-derived caches
 *     may be shared through map_cache_get, they are immutable).
 *   - a tick function the core calls once per tick, in the order listed in
 *     game.c (game_tick).
 *   - the actions the intent dispatcher (intent.c) calls.
 *
 * Sections are owned by one module each, keep additions inside your section.
 */
#ifndef OFE_SUBSYSTEMS_H
#define OFE_SUBSYSTEMS_H

/* ------------------------------------------------------------------------ */
/* Map caches (map.c): lazily built, immutable data shared by all games on a */
/* map, e.g. water components or pathfinding graphs. Thread-safe.            */
/* ------------------------------------------------------------------------ */
enum MapCacheSlot {
    MAP_CACHE_SHIPS = 0, /* ships.c: water components, pathfinding graph */
    MAP_CACHE_STRUCTURES = 1,
    MAP_CACHE_NUKES = 2,
    MAP_CACHE_RAIL = 3,
    MAP_CACHE_AI = 4,
    MAP_CACHE_STATIC = 5, /* mapstatic.c */
    MAP_CACHE_COUNT = 8
};
typedef void *(*MapCacheBuild)(const OfeMap *map);
typedef void (*MapCacheFree)(void *cache);
void *map_cache_get(OfeMap *map, int slot, MapCacheBuild build, MapCacheFree destroy);

/* ------------------------------------------------------------------------ */
/* Unit table (units.c, core)                                               */
/* ------------------------------------------------------------------------ */
void units_table_init(UnitTable *t);
void units_table_free(UnitTable *t);
void units_table_copy(UnitTable *dst, const UnitTable *src);

/* Creates a unit and logs OFE_EV_UNIT_CREATED. Returns its id. Fields not
 * given here start at 0 / -1; set them on the returned slot. */
UnitId unit_create(OfeGame *g, int type, PlayerId owner, TileRef tile);
/* Removes a unit and logs OFE_EV_UNIT_DESTROYED (destroyer 0 = none). */
void unit_destroy(OfeGame *g, UnitId id, PlayerId destroyer);
/* Hands a unit to a new owner (counts, events). */
void unit_capture(OfeGame *g, UnitId id, PlayerId new_owner);
/* Slot of a live unit, or -1 if the id is stale / invalid. */
int32_t unit_lookup(const OfeGame *g, UnitId id);
static inline UnitId unit_id_of_slot(const OfeGame *g, int32_t slot) {
    return ((int32_t)g->units.generation[slot] << UNIT_SLOT_BITS) | slot;
}
/* Marks a structure as finished (counts its level toward unit_levels). */
void unit_finish_construction(OfeGame *g, int32_t slot);
void unit_set_level(OfeGame *g, int32_t slot, int level);
void unit_move(OfeGame *g, int32_t slot, TileRef tile);

bool unit_is_structure(int type);
/* Gold cost of the next unit (extra = units already being paid for). */
int64_t unit_cost(const OfeGame *g, PlayerId p, int type);
/* Is there a finished unit of `type` owned by `owner` (0 = anyone) within
 * euclidean `range` of tile? */
bool units_has_nearby(const OfeGame *g, TileRef tile, int32_t range, int type, PlayerId owner);
/* Up to cap slots of units of `type` (owner 0 = any) within range, nearest
 * first. Returns the count. */
int32_t units_nearby(const OfeGame *g, TileRef tile, int32_t range, int type, PlayerId owner, int32_t *out_slots, int32_t cap);
/* Calls every subsystem tick in order (see units.c). */
void units_tick(OfeGame *g);
/* Hands over / destroys the structures of a dead player, captures structures
 * whose tile changed owner. Called from units_tick. */
void units_check_structure_owners(OfeGame *g);

/* ------------------------------------------------------------------------ */
/* Structures (structures.c): construction, upgrades, deletion, placement    */
/* ------------------------------------------------------------------------ */
typedef struct StructuresState StructuresState;
StructuresState *structures_state_new(OfeGame *g);
void structures_state_copy(OfeGame *dst, const OfeGame *src);
void structures_state_free(OfeGame *g);
void structures_tick(OfeGame *g); /* construction timers, silo cooldowns, defense posts */

/* Tile where the unit would actually be placed (e.g. nearest shore for a
 * port), or -1 if the player can't build it there now. Checks gold,
 * disabled units, ownership, spacing, silos for nukes, ports for warships. */
TileRef structures_can_build(const OfeGame *g, PlayerId p, int type, TileRef tile);
/* BUILD_UNIT intent. Nukes and warships are dispatched to their modules.
 * flags bit0 = rocket direction up (nukes). Returns a unit id or -1. */
UnitId structures_build(OfeGame *g, PlayerId p, int type, TileRef tile, int32_t amount, int32_t flags);
bool structures_upgrade(OfeGame *g, PlayerId p, UnitId id, int32_t amount);
bool structures_delete(OfeGame *g, PlayerId p, UnitId id);

/* Spatial index of structures. unit_create / unit_destroy call the first two
 * (they ignore other unit types); units_has_nearby / units_nearby use the
 * queries for structure types. Same semantics as the units.c versions. */
void structures_index_add(OfeGame *g, int32_t slot);
void structures_index_remove(OfeGame *g, int32_t slot);
bool structures_has_nearby(const OfeGame *g, TileRef tile, int32_t range, int type, PlayerId owner);
int32_t structures_nearby(const OfeGame *g, TileRef tile, int32_t range, int type, PlayerId owner, int32_t *out_slots, int32_t cap);

/* ------------------------------------------------------------------------ */
/* Ships (ships.c): water pathfinding, transport ships, trade ships,         */
/* warships and their shells                                                 */
/* ------------------------------------------------------------------------ */
typedef struct ShipsState ShipsState;
ShipsState *ships_state_new(OfeGame *g);
void ships_state_copy(OfeGame *dst, const OfeGame *src);
void ships_state_free(OfeGame *g);
void ships_tick(OfeGame *g);

/* BOAT_ATTACK intent. Returns the ship id or -1. */
UnitId ships_launch_transport(OfeGame *g, PlayerId p, TileRef dst, double troops);
bool ships_retreat_transport(OfeGame *g, PlayerId p, UnitId id);
/* Builds a warship from the port nearest to tile. Returns id or -1. */
UnitId ships_build_warship(OfeGame *g, PlayerId p, TileRef tile);
bool ships_move_warship(OfeGame *g, PlayerId p, UnitId id, TileRef tile);
/* Water body id of a water tile (same id = connected by water), -1 for land. */
int32_t ships_water_component(OfeGame *g, TileRef tile);
/* Shore tile of p from which a boat could reach dst, or -1. */
TileRef ships_transport_source(OfeGame *g, PlayerId p, TileRef dst);
/* Fires a shell from `from` at the unit in target_slot (warships and
 * defense posts use it). Returns the shell id or -1. */
UnitId ships_fire_shell(OfeGame *g, PlayerId owner, TileRef from, int32_t target_slot, int32_t damage);

/* ------------------------------------------------------------------------ */
/* Nukes (nukes.c): atom / hydrogen bombs, MIRVs and warheads, SAMs          */
/* ------------------------------------------------------------------------ */
typedef struct NukesState NukesState;
NukesState *nukes_state_new(OfeGame *g);
void nukes_state_copy(OfeGame *dst, const OfeGame *src);
void nukes_state_free(OfeGame *g);
void nukes_tick(OfeGame *g);
/* Launches from silo src_slot (or -1 = pick a ready silo). Charges gold
 * unless free=true. Returns the nuke id or -1. */
UnitId nukes_launch(OfeGame *g, PlayerId p, int type, TileRef target, int32_t src_slot, bool rocket_up, bool free_launch);

/* ------------------------------------------------------------------------ */
/* Rail (rail.c): factories, train stations, railroads, trains               */
/* ------------------------------------------------------------------------ */
typedef struct RailState RailState;
RailState *rail_state_new(OfeGame *g);
void rail_state_copy(OfeGame *dst, const OfeGame *src);
void rail_state_free(OfeGame *g);
void rail_tick(OfeGame *g);
/* A city, port or factory finished construction / was destroyed / changed
 * owner: (re)connect it to the rail network. */
void rail_on_structure_changed(OfeGame *g, int32_t slot);

/* ------------------------------------------------------------------------ */
/* Doomsday clock (doomsday.c)                                               */
/* ------------------------------------------------------------------------ */
typedef struct DoomsdayState DoomsdayState;
DoomsdayState *doomsday_state_new(OfeGame *g);
void doomsday_state_copy(OfeGame *dst, const OfeGame *src);
void doomsday_state_free(OfeGame *g);
void doomsday_tick(OfeGame *g);
/* Current required share of land, in tenths of a percent (0 = off). */
int32_t doomsday_bar_tenths(const OfeGame *g);

/* ------------------------------------------------------------------------ */
/* Built-in AI (ai_nation.c, ai_tribe.c)                                     */
/* ------------------------------------------------------------------------ */
typedef struct AiSysState AiSysState;
AiSysState *ai_state_new(OfeGame *g);
void ai_state_copy(OfeGame *dst, const OfeGame *src);
void ai_state_free(OfeGame *g);
/* Runs every nation and tribe due this tick. AIs act through game_issue(). */
void ai_tick(OfeGame *g);
/* Called when an AI player spawns (seeds per-player AI state). */
void ai_on_spawn(OfeGame *g, PlayerId p);
/* Called when a player receives an alliance request (nations decide). */
void ai_on_alliance_request(OfeGame *g, PlayerId requestor, PlayerId recipient);
void ai_on_alliance_extension_prompt(OfeGame *g, PlayerId ai, PlayerId ally);
/* Spawns tribes during the spawn phase (ai_tribe.c). */
void ai_spawn_tribes(OfeGame *g);

/* ------------------------------------------------------------------------ */
/* Subsystem state container                                                 */
/* ------------------------------------------------------------------------ */
struct Subsystems {
    StructuresState *structures;
    ShipsState *ships;
    NukesState *nukes;
    RailState *rail;
    DoomsdayState *doomsday;
    AiSysState *ai;
};

#endif /* OFE_SUBSYSTEMS_H */

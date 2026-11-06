/*
 * ai_common.h - what the built-in AIs (nations in ai_nation.c / ai_build.c,
 * tribes in ai_tribe.c) share: per-game AI state, the per-decision context
 * (who borders us, who attacks us), troop budgeting and boat helpers.
 *
 * Per-player scratch lives in players.ai_state[p][AI_*]; anything bigger is in
 * AiSysState below, which ai_state_copy deep-copies.
 */
#ifndef OFE_AI_COMMON_H
#define OFE_AI_COMMON_H

#include "internal.h"

/* ---- players.ai_state slots -------------------------------------------- */
enum {
    AI_RATE = 0, /* ticks between full decisions (0 = not initialised) */
    AI_TRIGGER, /* troops / max troops needed before attacking players (percent) */
    AI_RESERVE, /* troops kept home when attacking players (percent of max) */
    AI_EXPAND, /* troops kept home when expanding or hitting bots (percent) */
    AI_PLACEMENTS, /* structures placed so far (nations) */
    AI_LAST_STRUCT, /* tick of the last placement (nations) */
    AI_PHASE, /* nations: 0 = full decision, 1 and 2 = structures only */
    AI_FLAGS,
};
enum {
    AIF_FIRST_DONE = 1, /* the opening land grab was sent */
    AIF_NO_FREE_LAND = 2, /* tribes: stopped looking for free land */
    AIF_FIRST_BUILT = 4, /* nations: the "port first" opening is over */
};

#define AI_MAX_FIGHTS 24
#define MAX_TRANSPORT_SHIPS 3 /* boatMaxNumber */

/* One side of an attack: who is on the other end and with how many troops. */
typedef struct AiFight {
    PlayerId who;
    double troops; /* all attacks */
    double land; /* land attacks only (boat landings excluded) */
} AiFight;

typedef struct AiFights {
    AiFight in[AI_MAX_FIGHTS]; /* attacks on us, summed per attacker */
    AiFight out[AI_MAX_FIGHTS]; /* our attacks, summed per target (0 = free land) */
    int32_t n_in, n_out;
    double in_total, out_total;
} AiFights;

/* What the border scan finds. Mirrors OpenFront's player.nearby(). */
typedef struct AiNeighborInfo {
    bool tn_clean; /* free, un-nuked land touches our border */
    bool tn_land; /* free land (nuked or not) touches our border */
    bool tn_fallout; /* nuked free land touches our border */
    bool tn_shore; /* free land across a narrow strait */
    int32_t shore_tiles; /* border tiles on the shore */
    int32_t minx, miny, maxx, maxy; /* bounding box of the border */
} AiNeighborInfo;

typedef struct AiBox {
    int32_t minx, miny, maxx, maxy;
} AiBox;

/* A boat crossing worth trying. */
typedef struct AiRoute {
    PlayerId target;
    bool ok;
    TileRef landing;
    int32_t length;
} AiRoute;
#define AI_MAX_ROUTES 16

/* Scratch for one border scan. */
typedef struct AiScan {
    uint32_t *stamp; /* per player: == gen when seen in the current scan */
    uint8_t *flag; /* bit0 land neighbour, bit1 across a strait */
    uint32_t gen;
    IntVec nb; /* distinct neighbour players, in discovery order */
} AiScan;

struct AiSysState {
    int32_t cap; /* players.cap this state was sized for */

    /* Neighbour-scan scratch (not logical state, never copied). `main` backs
     * the AiCtx of the AI that is deciding; `side` serves the alliance
     * callbacks, which run inside another AI's decision. */
    AiScan main, side;
    IntVec tmp;

    /* Caches (copied: they influence decisions). */
    AiBox *box; /* per player border bounding box */
    int32_t *box_tick;
    int32_t leader_tick, leader; /* runaway leader cache */
    int32_t top_tick, top_player, top_tiles; /* land leader cache */
    int32_t seen_attack_id; /* boat landings up to this attack id were followed up */
    int32_t mirv_tick[8]; /* recent MIRV targets (ring) */
    int32_t mirv_who[8];
    int32_t mirv_pos;
    /* Memo of "is a MIRV worth saving for" (valid within one tick; not copied). */
    int32_t worth_tick, worth_p;
    bool worth;
};

/* Everything a decision needs, filled by ai_ctx_begin. */
typedef struct AiCtx {
    OfeGame *g;
    AiSysState *ai;
    PlayerId p;
    uint64_t *rng;
    int diff; /* OfeDifficulty */
    bool scanned; /* nbi and the neighbour list are valid (see ai_ctx_scan) */
    AiNeighborInfo nbi;
    AiFights fights;
    double bot_troops_sent; /* nations: troops already committed to bots this decision */
    AiRoute routes[AI_MAX_ROUTES];
    int32_t n_routes;
} AiCtx;

/* ---- state (ai_common.c) ---------------------------------------------- */
static inline AiSysState *ai_sys(const OfeGame *g) { return g->sub->ai; }
/* Fills the attack summary; the border scan (neighbours, nbi) is done by
 * ai_ctx_scan when a decision needs it. */
void ai_ctx_begin(AiCtx *c, OfeGame *g, PlayerId p);
void ai_ctx_scan(AiCtx *c);
/* Non-tribe neighbours of p (land or across a strait) into out; safe to call
 * from inside another AI's decision. Returns how many. */
int32_t ai_side_neighbors(OfeGame *g, PlayerId p, PlayerId *out, int32_t cap);
/* Is q next to our border (by land or across a strait)? Valid until the next scan. */
bool ai_is_neighbor(const AiCtx *c, PlayerId q);
bool ai_borders_by_land(const AiCtx *c, PlayerId q);
/* Cached bounding box of q's border (refreshed every 100 ticks). */
const AiBox *ai_player_box(OfeGame *g, AiSysState *ai, PlayerId q);

/* ---- relations -------------------------------------------------------- */
enum { REL_HOSTILE = 0, REL_DISTRUSTFUL = 1, REL_NEUTRAL = 2, REL_FRIENDLY = 3 };
int ai_relation_class(double value);
int ai_relation(const OfeGame *g, PlayerId self, PlayerId other);

/* ---- troops ----------------------------------------------------------- */
bool ai_high_start_gold(const OfeGame *g);
double ai_max_troops(const OfeGame *g, PlayerId p, bool expansion);
double ai_reserve_troops(const AiCtx *c, PlayerId target);
bool ai_has_ratio_troops(const AiCtx *c, int ratio_percent, bool expansion);
/* Largest single incoming attack from someone not friendly (and, for
 * nations, not a tribe). 0 if none. */
PlayerId ai_find_incoming_attacker(const AiCtx *c, bool ignore_bots);
double ai_incoming_from(const AiCtx *c, PlayerId who);
double ai_incoming_on(const OfeGame *g, PlayerId target);
bool ai_player_has_structures(const OfeGame *g, PlayerId p);

/* ---- issuing actions (they go through game_issue) ---------------------- */
/* Land attack on target (0 = free land). Returns true if an attack started. */
bool ai_issue_attack(AiCtx *c, PlayerId target, double troops);
bool ai_issue_boat(AiCtx *c, TileRef dst, double troops);

/* ---- boats ------------------------------------------------------------ */
bool ai_boats_available(const AiCtx *c);
/* Plans a crossing to a shore of target. */
bool ai_boat_route(AiCtx *c, PlayerId target, TileRef *landing, int32_t *length);
/* A free-land shore within 5 tiles across the water that a boat can reach. */
TileRef ai_find_free_land_landing(AiCtx *c);

/* ---- tribes (ai_tribe.c) ---------------------------------------------- */
void ai_tribe_init(OfeGame *g, PlayerId p);
void ai_tribe_decide(OfeGame *g, PlayerId p);
bool ai_tribe_alliance_extension(OfeGame *g, PlayerId tribe, PlayerId ally);

/* ---- nations (ai_nation.c, ai_build.c) -------------------------------- */
void ai_nation_init(OfeGame *g, PlayerId p);
void ai_nation_decide(OfeGame *g, PlayerId p);
void ai_nation_structures_only(OfeGame *g, PlayerId p);
void ai_nation_follow_up_landings(OfeGame *g);
bool ai_nation_alliance_decision(OfeGame *g, PlayerId self, PlayerId other, bool is_response);
bool ai_nation_should_attack(AiCtx *c, PlayerId other);

/* ai_build.c: building, nukes, MIRVs, warships */
bool ai_build_handle_structures(AiCtx *c);
void ai_build_maybe_warship(AiCtx *c);
void ai_build_maybe_nuke(AiCtx *c);
bool ai_build_consider_mirv(AiCtx *c);
/* Strongest player by land, cached (excludes tribes). */
PlayerId ai_land_leader(OfeGame *g, AiSysState *ai, int32_t *tiles);
PlayerId ai_runaway_leader(OfeGame *g, AiSysState *ai);
double ai_save_up_target(const AiCtx *c);

#endif

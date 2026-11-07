/*
 * tribe_ts.h - OpenFront's TribeExecution for the TS-parity build
 * (ai_tribe_ts.c), driven by the execution list in exec_ts.c.
 */
#ifndef OFE_TRIBE_TS_H
#define OFE_TRIBE_TS_H

#include "internal.h"

#if OFE_TS_PARITY

/* TribeExecution's fields: its PseudoRandom(simpleHash(player id)) is shared
 * with the attack behaviour, the beat, and the three troop ratios. */
typedef struct TribeTs {
    TsRandom rng;
    int32_t attack_rate; /* 40..79 ticks */
    int32_t attack_tick; /* the beat: ticks % attack_rate == attack_tick */
    double trigger_ratio, reserve_ratio, expand_ratio;
    uint8_t has_behaviour; /* the first beat creates the attack behaviour and only grabs free land */
    uint8_t neighbors_terra_nullius;
} TribeTs;

/* The constructor: seeds the stream and draws rate, beat and ratios. */
void tribe_ts_init(OfeGame *g, PlayerId p, TribeTs *t);
/* One TribeExecution.tick. Returns false when the execution ends. */
bool tribe_ts_tick(OfeGame *g, PlayerId p, TribeTs *t);

/* exec_ts.c: queue an execution created by an AI during its tick; it is
 * initialised at the end of the tick, like Game.addExecution. */
void ts_exec_add_attack(OfeGame *g, PlayerId attacker, PlayerId target, double troops);
void ts_exec_add_intent(OfeGame *g, const OfeIntent *in);

/* attack.c: active attack rows in creation order. */
void attacks_in_creation_order(const OfeGame *g, IntVec *out);

#endif
#endif

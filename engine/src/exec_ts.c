/*
 * exec_ts.c - OpenFront's tick structure, for the TS-parity build. Compiled
 * only with -DOFE_TS_PARITY=1; the default build runs per-system batches
 * (game.c: game_tick).
 *
 * TS keeps a list of executions. Each tick (GameImpl.executeNextTick):
 *   1. the turn's intents become new executions (queued, not yet running);
 *   2. every running execution ticks, in the order it was initialised;
 *   3. queued executions are initialised, in creation order, and join the
 *      running list; during the spawn phase only spawn executions run;
 *   4. finished executions are dropped.
 * So an intent pushed before C tick t is turn t, does its init work at the end
 * of tick t (attack start, alliance requests, ...) and its tick work during
 * tick t+1 (break alliance, embargo, donation, retreat order, spawn), and a
 * new PlayerExecution first ticks the tick after it is created. Per-player
 * work (growth, expiry, enclaves) and attacks interleave in that order.
 *
 * Executions modelled here: spawn, player, attack, the intent kinds below and the
 * unit executions of units_ts.c (construction, upgrade, delete, silo, SAM, SAM
 * missile, nuke, MIRV). Ships and rail still tick after the list, in the
 * default order, and so do nations and tribes (they join the TS order in later
 * waves).
 */
#include "internal.h"
#include "units_ts.h"

#if OFE_TS_PARITY

enum TsKind {
    TX_SPAWN, /* SpawnExecution: places the player at its tick */
    TX_PLAYER, /* PlayerExecution */
    TX_ATTACK, /* AttackExecution; arg = attack id once started */
    TX_AT_TICK, /* intent whose work TS does in tick(): retreat, break alliance, embargo, target */
    TX_DONATE_GOLD, /* init computes the amount, tick donates */
    TX_DONATE_TROOPS,
    TX_AT_INIT, /* intent whose work TS does in init(): alliance request, reject, extension, ... */
    TX_UNIT, /* a unit execution (units_ts.c), including those made by build / upgrade / delete intents */
};

typedef struct TsExec {
    uint8_t kind, active, queued_in_spawn;
    PlayerId player;
    int32_t arg;
    OfeIntent in;
    double amount; /* donations: the amount fixed at init */
    UnitExec *ux; /* TX_UNIT */
} TsExec;
OFE_VEC(TsExecVec, TsExec)

struct TsExecs {
    TsExecVec run; /* initialised, in tick order */
    TsExecVec pend; /* created, not yet initialised, in creation order */
    bool win_check_on; /* WinCheckExecution is inactive during the spawn phase */
    int32_t next_eid; /* identity of unit executions */
};

static struct TsExecs *execs_of(OfeGame *g) {
    if (!g->ts) g->ts = (struct TsExecs *)ofe_xcalloc(1, sizeof(struct TsExecs));
    return g->ts;
}

static void execs_release(TsExecVec *v) {
    for (int32_t i = 0; i < v->len; i++) uexec_free(v->data[i].ux);
    TsExecVec_free(v);
}

void ts_execs_free(OfeGame *g) {
    if (!g->ts) return;
    execs_release(&g->ts->run);
    execs_release(&g->ts->pend);
    free(g->ts);
    g->ts = NULL;
}

void ts_execs_copy(OfeGame *dst, const OfeGame *src) {
    if (!src->ts) {
        ts_execs_free(dst);
        return;
    }
    struct TsExecs *d = execs_of(dst);
    execs_release(&d->run);
    execs_release(&d->pend);
    TsExecVec_copy(&d->run, &src->ts->run);
    TsExecVec_copy(&d->pend, &src->ts->pend);
    for (int32_t i = 0; i < d->run.len; i++) {
        if (d->run.data[i].ux) d->run.data[i].ux = uexec_clone(d->run.data[i].ux);
    }
    for (int32_t i = 0; i < d->pend.len; i++) {
        if (d->pend.data[i].ux) d->pend.data[i].ux = uexec_clone(d->pend.data[i].ux);
    }
    d->win_check_on = src->ts->win_check_on;
    d->next_eid = src->ts->next_eid;
}

/* ---- Unit executions (units_ts.c) ------------------------------------------------ */

/* Game.addExecution from inside an execution: joins the queue, initialised at the end of this tick. */
int32_t ts_exec_add_unit(OfeGame *g, UnitExec *x) {
    struct TsExecs *ts = execs_of(g);
    int32_t eid = ++ts->next_eid;
    uexec_set_eid(x, eid);
    TsExec ex = {0};
    ex.kind = TX_UNIT;
    ex.active = 1;
    ex.arg = -1;
    ex.ux = x;
    TsExecVec_push(&ts->pend, ex);
    return eid;
}

UnitExec *ts_exec_find_unit(OfeGame *g, int32_t eid) {
    struct TsExecs *ts = execs_of(g);
    for (int32_t i = 0; i < ts->run.len; i++) {
        if (ts->run.data[i].ux && uexec_eid(ts->run.data[i].ux) == eid) return ts->run.data[i].ux;
    }
    for (int32_t i = 0; i < ts->pend.len; i++) {
        if (ts->pend.data[i].ux && uexec_eid(ts->pend.data[i].ux) == eid) return ts->pend.data[i].ux;
    }
    return NULL;
}

/* ---- Creation (Executor.createExec) ------------------------------------------ */

static uint8_t kind_of_intent(int type) {
    switch (type) {
    case OFE_INTENT_SPAWN: return TX_SPAWN;
    case OFE_INTENT_ATTACK: return TX_ATTACK;
    case OFE_INTENT_CANCEL_ATTACK:
    case OFE_INTENT_BREAK_ALLIANCE:
    case OFE_INTENT_TARGET_PLAYER:
    case OFE_INTENT_EMBARGO: return TX_AT_TICK;
    case OFE_INTENT_DONATE_GOLD: return TX_DONATE_GOLD;
    case OFE_INTENT_DONATE_TROOPS: return TX_DONATE_TROOPS;
    case OFE_INTENT_BUILD_UNIT:
    case OFE_INTENT_UPGRADE_STRUCTURE:
    case OFE_INTENT_DELETE_UNIT: return TX_UNIT;
    default: return TX_AT_INIT;
    }
}

static void create_execs(OfeGame *g) {
    struct TsExecs *ts = execs_of(g);
    IntentVec *q = &g->pending_intents;
    for (int32_t i = 0; i < q->len; i++) {
        TsExec ex = {0};
        ex.kind = kind_of_intent(q->data[i].type);
        ex.active = 1;
        ex.player = q->data[i].player;
        ex.arg = -1;
        ex.in = q->data[i];
        if (ex.kind == TX_UNIT) {
            ex.ux = uexec_from_intent(&ex.in);
            uexec_set_eid(ex.ux, ++ts->next_eid);
        }
        TsExecVec_push(&ts->pend, ex);
    }
    q->len = 0;
}

/* ---- Init ----------------------------------------------------------------------- */

static void init_donate_gold(OfeGame *g, TsExec *ex) {
    PlayerId to = ex->in.target;
    if (!player_valid(g, to)) {
        ex->active = 0;
        return;
    }
    /* gold ??= sender.gold() / 3n, fixed now */
    ex->amount = ex->in.troops < 0 ? (double)(g->players.gold[ex->player] / 3) : floor(ex->in.troops);
}

static void init_donate_troops(OfeGame *g, TsExec *ex) {
    PlayerId to = ex->in.target;
    if (!player_valid(g, to)) {
        ex->active = 0;
        return;
    }
    double troops = ex->in.troops < 0 ? floor((double)g->players.troops[ex->player] / 3.0) : ex->in.troops;
    double room = player_max_troops(g, to) - (double)g->players.troops[to];
    ex->amount = OFE_MIN(troops, room);
    if (ex->amount <= 0) ex->active = 0;
}

static void init_attack(OfeGame *g, TsExec *ex) {
    const OfeIntent *in = &ex->in;
    ex->active = 0;
    if (in->target != 0 && !player_valid(g, in->target)) return;
    int32_t id = attack_start(g, ex->player, in->target, in->troops, -1, true);
    if (id >= 0) {
        ex->arg = id;
        ex->active = 1;
    }
}

static void init_exec(OfeGame *g, TsExec *ex) {
    switch (ex->kind) {
    case TX_SPAWN: ex->queued_in_spawn = g->start_tick < 0; break;
    case TX_PLAYER: g->players.cluster_calc_tick[ex->player] = g->tick + (int32_t)(g->players.id_hash[ex->player] % 20); break;
    case TX_ATTACK: init_attack(g, ex); break;
    case TX_DONATE_GOLD: init_donate_gold(g, ex); break;
    case TX_DONATE_TROOPS: init_donate_troops(g, ex); break;
    case TX_AT_INIT:
        intent_apply_one(g, &ex->in);
        ex->active = 0;
        break;
    case TX_UNIT: ex->active = uexec_init(g, ex->ux); break;
    default: break;
    }
}

/* ---- Tick ------------------------------------------------------------------------ */

/* SpawnExecution.tick: only a spawn issued during the spawn phase counts. */
static void tick_spawn(OfeGame *g, TsExec *ex) {
    ex->active = 0;
    PlayerId p = ex->player;
    int32_t tile = ex->in.tile;
    if (tile < 0 || tile >= g->map->num_tiles || !ex->queued_in_spawn) return;
    bool first = !g->players.spawned[p];
    TileRef center = g->cfg.random_spawn ? -1 : tile;
    if (g->cfg.random_spawn && !first) return;
    if (spawn_player(g, p, center) && first) {
        TsExec pe = {0};
        pe.kind = TX_PLAYER;
        pe.active = 1;
        pe.player = p;
        pe.arg = -1;
        TsExecVec_push(&g->ts->pend, pe);
    }
}

static void tick_donate(OfeGame *g, TsExec *ex) {
    ex->active = 0;
    if (ex->kind == TX_DONATE_GOLD) player_donate_gold(g, ex->player, ex->in.target, ex->amount);
    else player_donate_troops(g, ex->player, ex->in.target, ex->amount);
}

static void tick_exec(OfeGame *g, TsExec *ex) {
    switch (ex->kind) {
    case TX_SPAWN: tick_spawn(g, ex); break;
    case TX_PLAYER:
        player_decay_relations(g, ex->player);
        if (!player_exec_tick_ts(g, ex->player)) ex->active = 0;
        break;
    case TX_ATTACK: {
        int32_t row = attack_row_by_id(g, ex->arg);
        if (row < 0) {
            ex->active = 0; /* merged into another attack or ended */
            break;
        }
        attack_tick_row(g, row);
        if (attack_row_by_id(g, ex->arg) < 0) ex->active = 0;
        break;
    }
    case TX_AT_TICK:
        intent_apply_one(g, &ex->in);
        ex->active = 0;
        break;
    case TX_DONATE_GOLD:
    case TX_DONATE_TROOPS: tick_donate(g, ex); break;
    case TX_UNIT: ex->active = uexec_tick(g, ex->ux); break;
    default: ex->active = 0; break;
    }
}

/* Runs one tick of the TS execution list. */
void game_tick_ts(OfeGame *g) {
    struct TsExecs *ts = execs_of(g);
    create_execs(g);

    /* SpawnTimerExecution is first in the list. */
    if (g->start_tick < 0 && g->tick > g->spawn_phase_ticks) spawn_phase_end(g);
    /* WinCheckExecution is second: it sees the state at the end of the last tick. */
    if (ts->win_check_on) win_check(g);
    alliance_requests_tick(g);

    for (int32_t i = 0; i < ts->run.len; i++) {
        TsExec *ex = &ts->run.data[i];
        if (!ex->active || (ex->ux && !uexec_active(ex->ux))) continue; /* a nuke may be called off by an earlier one */
        if (g->start_tick < 0 && ex->kind != TX_SPAWN) continue;
        tick_exec(g, ex);
    }

    /* Initialise what was created, in creation order; only spawns during the spawn phase. */
    TsExecVec keep = {0}, inited = {0};
    for (int32_t i = 0; i < ts->pend.len; i++) {
        TsExec ex = ts->pend.data[i];
        if (g->start_tick < 0 && ex.kind != TX_SPAWN) {
            TsExecVec_push(&keep, ex);
            continue;
        }
        init_exec(g, &ex);
        if (ex.active) TsExecVec_push(&inited, ex);
        else uexec_free(ex.ux);
    }
    TsExecVec_free(&ts->pend);
    ts->pend = keep;
    if (g->start_tick >= 0) ts->win_check_on = true; /* inited with the first batch after the phase */
    int32_t w = 0;
    for (int32_t i = 0; i < ts->run.len; i++) {
        TsExec *e = &ts->run.data[i];
        if (e->active && (!e->ux || uexec_active(e->ux))) ts->run.data[w++] = *e;
        else uexec_free(e->ux);
    }
    ts->run.len = w;
    for (int32_t i = 0; i < inited.len; i++) TsExecVec_push(&ts->run, inited.data[i]);
    TsExecVec_free(&inited);

    if (g->start_tick >= 0) {
        ships_tick(g); /* ships and rail are not in TS order yet (later waves) */
        rail_tick(g);
        doomsday_tick(g);
        ai_tick(g);
    }
    g->tick++;
}

#endif /* OFE_TS_PARITY */

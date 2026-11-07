/*
 * units_ts.h - the unit executions of OpenFront's TS execution list (parity
 * build only, see units_ts.c and docs/ts_parity.md).
 */
#ifndef OFE_UNITS_TS_H
#define OFE_UNITS_TS_H

#include "internal.h"

#if OFE_TS_PARITY

/* One unit-related TS execution: ConstructionExecution, UpgradeStructureExecution,
 * DeleteUnitExecution, MissileSiloExecution, SAMLauncherExecution,
 * SAMMissileExecution, NukeExecution or MirvExecution. Opaque outside units_ts.c. */
typedef struct UnitExec UnitExec;

/* The execution Executor.createExec makes for a build_unit / upgrade_structure /
 * delete_unit intent; NULL for any other intent. */
UnitExec *uexec_from_intent(const OfeIntent *in);
/* Execution.init / Execution.tick; both return whether the execution stays active. */
bool uexec_init(OfeGame *g, UnitExec *x);
bool uexec_tick(OfeGame *g, UnitExec *x);
UnitExec *uexec_clone(const UnitExec *x);
void uexec_free(UnitExec *x);
bool uexec_active(const UnitExec *x);
/* Identity used to find an execution again (MIRV cancels its warheads). */
int32_t uexec_eid(const UnitExec *x);
void uexec_set_eid(UnitExec *x, int32_t eid);
/* NukeExecution.cancel(). */
void uexec_cancel(OfeGame *g, UnitExec *x);

/* Implemented in exec_ts.c: Game.addExecution for a unit execution, and lookup by eid. */
int32_t ts_exec_add_unit(OfeGame *g, UnitExec *x); /* returns the eid it was given */
UnitExec *ts_exec_find_unit(OfeGame *g, int32_t eid);

/* The structure part of PlayerExecution.tick: structures on land that changed hands are
 * captured (defense posts destroyed), those on unowned land deleted. */
void player_structures_tick_ts(OfeGame *g, PlayerId p);
/* AllianceRequestExecution.cancelNukesBetweenAlliedPlayers. */
void nukes_ts_cancel_between(OfeGame *g, PlayerId a, PlayerId b);

#endif /* OFE_TS_PARITY */
#endif /* OFE_UNITS_TS_H */

# TS-parity build

`make parity` builds the engine with `-DOFE_TS_PARITY=1` into `build-parity/`.
In that build a game reproduces OpenFront's TypeScript engine exactly: the same
map, game id, config, players and intents give the same observable state on
every tick. The default build stays the fast approximation (same rules,
self-deterministic only).

## What must match

Only observable output. Concretely, after every tick (`tools/parity/digest.py`):

- tile owners and fallout;
- per player: troops, gold, tile count;
- per unit: id, type, owner, tile, troops, health, level, under construction;
- per attack: attacker, target, troops, retreating;
- alliances and their expiry tick;
- ids that intents refer to (unit ids, which attack a cancel refers to), so a
  recorded TS game can be replayed through the C API.

Internal data structures, caches, statistics, UI updates, names, emojis and
quick chat do not need to match, unless they change one of the above (a nation
reacting to an emoji does; the emoji itself does not).

## How parity code is organised

1. **One engine.** Parity lives in the existing modules, not in a second engine.
2. **Free exactness is unconditional.** When TS's exact behaviour costs nothing
   (a formula, a cooldown, a tick offset, the order of two calls), it replaces
   the default behaviour in both builds, without `#if`.
3. **`#if OFE_TS_PARITY` only where exactness costs.** For example: emulating
   JS `Set` insertion order instead of swap-remove lists, drawing from OpenFront's
   random streams instead of ours, executing per-entity work in TS's execution
   order instead of per-system batches.
4. **Big divergent algorithms get their own file**: `<module>_ts.c`, wrapped
   whole in `#if OFE_TS_PARITY` and called from the module at one switch point
   (for example the nation AI). The default module stays readable on its own.
   An exact algorithm that is also fast is not divergent: the water pathfinder
   (`pathfind_ts.c`, 0.1-0.3 ms per cold query, route memo per map) is the only
   ship router of both builds, and `ofe_map_load` reads OpenFront's real mini
   map (`map4x.bin` / `map16x.bin`) in both.
5. Each `#if OFE_TS_PARITY` block says in one line what TS does that the
   default build approximates.

## Rules of exactness

- **Randomness.** `TsRandom` in `util.h` is OpenFront's `PseudoRandom` (sfc32,
  splitmix32 seeding, `nextInt`, `nextFloat`, `chance`, `shuffleArray`,
  `randElement`, `nextID`). Every TS `PseudoRandom` instance whose draws reach
  observable state has a C stream with the same seed (from `simpleHash(gameID)`,
  a player id, a tick, ...), and the C code draws in the same order, including
  draws whose result is discarded.
- **Iteration order.** Wherever TS iterates a `Set`, `Map` or array and the order
  decides something (random draws, ties, first match, which unit acts first),
  C iterates in the same order. JS `Set`/`Map` order is insertion order with
  deletions removed; OpenFront's `TileSet` behaves the same.
- **Numbers.** Troops and gold are `bigint` in TS: `int64_t` in C, with truncating
  division. Everything else is a double: same operations in the same order.
  `Math.floor` is `floor`, `x | 0` is ToInt32 (wraps modulo 2^32),
  `Math.round(x)` is `floor(x + 0.5)`. `Math.pow` and `**` must give V8's
  result (check against node; glibc's `pow` may differ in the last bit).
- **Ids.** Player ids are TS small ids. In the parity build unit ids are TS unit
  ids (OpenFront's global counter).
- **Tick structure.** TS ticks every active execution in insertion order, then
  initialises the executions created this tick (intents of this turn, and
  executions other executions added), then drops inactive ones. Intents pushed
  before tick `t` are TS's turn `t`. Where the default build's per-system order
  differs observably, the parity build follows TS's.

## Workflow

```bash
make parity
export OPENFRONT_DIR=<openfront checkout> OFE_MAPS_DIR=$OPENFRONT_DIR/resources/maps
python3 tools/parity/diff.py tools/parity/scenarios/core_humans.json
```

`diff.py` runs the scenario on both engines, prints the first tick whose digest
differs and the rows behind it (players, units, attacks, alliances, tiles that
changed). Fix, rebuild, repeat. `make parity-check` runs every scenario in
`tools/parity/scenarios/`; each scenario there must match on all its ticks.
Add a scenario for every feature made exact. The default build's tests and
benchmark must keep passing.

## Licence note

The parity code paths follow OpenFront's code closely, so they are a derivative
of OpenFront, which is AGPL-3.0. Distributing builds of this repository has to
respect that.

## Status and facts for the next waves

Scenarios that match on every tick (`make parity-check`): `core_humans` (1500
ticks), `war_12humans` (6000: wars, cancelled attacks, alliances that expire,
are extended or broken, wiped-out players), `social_donations` (3000:
donations, embargoes, targets, disconnects), `win_check` (900), and with
tribes (`bots`): `tribes_humans` (4000: 4 humans among 40 tribes, alliances,
extensions, betrayals, retaliation), `tribes_only` (3000, 80 tribes),
`tribes_europe` (3000, 2 humans and 60 tribes, hard, another map) and
`tribes_first_boat` (650, exact up to the first transport ship). Still out:
nations, units, ships, nukes. The tribe scenarios switch transport ships off
(`disabledUnits: ["Transport"]` / `disabled_units`) because ships are not
exact yet; the tribe's decision to sail is ported only roughly (see below).

Where the parity code lives:

| File | What |
|---|---|
| `exec_ts.c` | TS's execution list and tick (`game_tick_ts`); the one switch is in `game.c` |
| `cluster_ts.c` | `removeClusters` of PlayerExecution, step for step |
| `tiles.c` (`TileSeq`) | insertion-ordered `player.tiles()` and `borderTiles()` (JS Set order) |
| `ai_tribe_ts.c`, `tribe_ts.h` | `TribeExecution` + `TribeAttackBehavior`, step for step (the default `ai_tribe.c` stays the cheap tribe) |
| `attack.c`, `spawn.c`, `player.c`, `intent.c` | small `#if OFE_TS_PARITY` blocks, each says what TS does |

Facts a new module needs:

- **Seeds.** Player ids are `PseudoRandom(simpleHash(gameID)).nextID()`, humans
  first, then the nations in manifest (or shuffled) order. Tribe ids come from
  `PseudoRandom(simpleHash(gameID) + 2)`: per tribe two draws for its name
  (prefix, suffix) and then `nextID()`; `game_assign_ids` does all of this
  (maps with OpenFront `customTribes` names, such as ChannelIslands, Germany,
  LosAngeles, draw differently and are not covered).
  `Executor.random` is `PseudoRandom(simpleHash(gameID) + 1)` (nations' map). Each
  AttackExecution has its own `PseudoRandom(123)`. A SpawnExecution seeds
  `simpleHash(playerId) + simpleHash(gameID)`. Donation executions seed
  `PseudoRandom(ticks)`. `game.id_seed` and `players.id_hash` hold the hashes.
- **Execution order.** `PlayerExecution`s tick in the order the players first
  spawned (not by id), interleaved with attacks in the order they were
  initialised. A nation execution is created at game start, before
  any PlayerExecution, so it ticks first. A tribe's executions are created by
  its SpawnExecution (every tribe has one from game start; they tick on tick 1
  in tribe order and add the tribe as a player, so tribe small ids follow the
  humans and nations): PlayerExecution first, then TribeExecution, so a tribe
  ticks right after its own PlayerExecution. Tribes spawn before any human's
  spawn intent ticks, and a human's chosen spot may overlap a tribe's disc
  (the human gets what is free). TribeExecution does not run in the spawn phase. WinCheck ticks right after the spawn
  timer, before every player, on the state of the previous tick.
- **init vs tick work.** At the end of tick t (init): attack start, alliance
  request / reject / extension, embargo-all, mark disconnected, donation amount.
  During tick t+1 (tick): spawn, break alliance, embargo, target, retreat order
  (the retreat executes 20 ticks after the intent, one tick after the attack
  sees `retreated`), donation. Anything not active during the spawn phase waits
  for it to end and is initialised in creation order at the end of its last tick.
- **Ids.** Attack ids are C ids in the parity build (cancel intents resolve the
  attack by target in the harness). Unit ids are still C handles: wire up TS's
  global `nextUnitID` when units join the parity tick (`units.c`).
- **Numbers.** `ofe_pow/exp/log` are OpenFront's DetMath in the parity build
  and libm in the default build; `sigmoid` uses them too. Everything else uses
  doubles in TS order. `TsRandom` is unconditional in `util.h`.
- **Tribes.** `TribeExecution`: `PseudoRandom(simpleHash(tribe id))`, drawn in
  order: rate `nextInt(40,80)`, beat `nextInt(0,rate)`, trigger, reserve and
  expand ratios (`nextInt(50,60)`, `(30,40)`, `(10,20)` / 100); the same stream
  serves the attack behaviour. A beat is `ticks % rate == beat`; a dead tribe's
  execution ends on its beat. First beat: send everything above the reserve
  at free land, nothing else. Later beats: accept every incoming alliance
  request directly (no relation update, no embargo change), queue an
  extension execution for every alliance where exactly one side agreed,
  queue one structure deletion, then `maybeAttack`: betray a neighbouring
  traitor (`randElement` always draws, then `chance(6 if friendly else 3)`),
  expand into free land while `nearby()` holds free land, else
  `attackRandomTarget` (trigger ratio, biggest single incoming attack, traitor
  `chance(3)`, then `shuffleArray(nearby())` including the terra nullius
  entry, `chance(2)` skip for humans and nations). `nearby()` order is the JS
  Set order: border tiles in `border_seq` order, then every tenth shore tile's
  strait scan. Attacks go in as `AttackExecution`s (initialised at the end of
  the tick). Nations share `AiAttackBehavior` with tribes (sendAttack,
  reserve/expand troops, boat route planning), the alliance helpers
  (`alliance_form`, `alliance_break`, extension executions) and
  `ts_exec_add_attack` / `ts_exec_add_intent` (exec_ts.c) for creating
  executions from an AI. Tribes have no starting gold, so their troop
  ratios always use the full `maxTroops`; nations with high starting gold use
  `maxTroopsWithoutCities` for expansion.
- **Tribe boats.** The decision to cross water (`sendBoatAttack`,
  `sendBoatAttackToNearbyTerraNullius`) calls the default engine's boat
  helpers and launches a ship at once; the TS decision needs `SpatialQuery`
  and the ship executions are not exact, so a game is exact only until the
  first transport ship (see `tribes_first_boat`). The tick of the first boat
  matched TS in the runs made, so the decision itself is probably right.
- **Digest harness.** TS adds tribe players on tick 1, so `trace_c.py` hides
  tribe rows after tick 0; embargoes are read for pairs with a non-tribe.
- **Not in TS order yet:** `units_tick`, `doomsday_tick` and (nation) `ai_tick` run after
  the execution list; the alliance-extension prompt for AIs is not called from
  `alliances_expire_of`; relations decay per player row only for players with
  a relation row.

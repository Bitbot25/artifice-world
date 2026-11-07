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
donations, embargoes, targets, disconnects), `win_check` (900). Units: `units_structures` (4500), `units_nukes` (6200), `units_sam` (4200),
made by `make_unit_scenarios.py`. Humans only so far: no nations, tribes, ships.

Where the parity code lives:

| File | What |
|---|---|
| `exec_ts.c` | TS's execution list and tick (`game_tick_ts`); the one switch is in `game.c` |
| `cluster_ts.c` | `removeClusters` of PlayerExecution, step for step |
| `tiles.c` (`TileSeq`) | insertion-ordered `player.tiles()` and `borderTiles()` (JS Set order) |
| `attack.c`, `spawn.c`, `player.c`, `intent.c` | small `#if OFE_TS_PARITY` blocks, each says what TS does |

Facts a new module needs:

- **Seeds.** Player ids are `PseudoRandom(simpleHash(gameID)).nextID()`, humans
  first, then the nations in manifest (or shuffled) order; `game_assign_ids`
  draws humans and nations, tribes still need their own draws (TribeSpawner).
  `Executor.random` is `PseudoRandom(simpleHash(gameID) + 1)`. Each
  AttackExecution has its own `PseudoRandom(123)`. A SpawnExecution seeds
  `simpleHash(playerId) + simpleHash(gameID)`. Donation executions seed
  `PseudoRandom(ticks)`. `game.id_seed` and `players.id_hash` hold the hashes.
- **Execution order.** `PlayerExecution`s tick in the order the players first
  spawned (not by id), interleaved with attacks in the order they were
  initialised. A nation or tribe execution is created at game start, before
  any PlayerExecution, so it ticks first. WinCheck ticks right after the spawn
  timer, before every player, on the state of the previous tick.
- **init vs tick work.** At the end of tick t (init): attack start, alliance
  request / reject / extension, embargo-all, mark disconnected, donation amount.
  During tick t+1 (tick): spawn, break alliance, embargo, target, retreat order
  (the retreat executes 20 ticks after the intent, one tick after the attack
  sees `retreated`), donation. Anything not active during the spawn phase waits
  for it to end and is initialised in creation order at the end of its last tick.
- **Ids.** Attack ids are C ids in the parity build (cancel intents resolve the
  attack by target in the harness). Unit ids are TS's global `nextUnitID` in the
  parity build (`uid` column, `unit_create` assigns them in TS build order; ships
  and trains consume ids too, but are not in TS order yet).
- **Numbers.** `ofe_pow/exp/log` are OpenFront's DetMath in the parity build
  and libm in the default build; `sigmoid` uses them too. Everything else uses
  doubles in TS order. `TsRandom` is unconditional in `util.h`.
- **Units (units_ts.c).** Construction, upgrade, delete, silo, SAM, SAM missile,
  nuke and MIRV executions are in the exec list with TS's init/tick split (a nuke
  is placed 2 ticks after its intent, a structure 1 tick after and finishes
  duration+1 later; upgrades and delete marks happen at init). Per-player unit
  order is `own_seq`, UnitGrid query order is `grid_seq`. Structure capture is
  PlayerExecution's loop (`player_structures_tick_ts`). Free exactness made
  unconditional: structure placement tie-break (DFS order), delete cooldown
  counted from tick -1, `alliance_break_raw` (nukes break alliances without
  BreakAllianceExecution's relation updates). Skipped: PortExecution,
  FactoryExecution, CityExecution station, warships (ships/rail waves),
  waterNukes. Silo/SAM queues live in the unit's `path`, nuke trajectory too.
  `paths_ts.c` still has its own RNG (`TsPathRng`).
- **Not in TS order yet:** ships and rail ticks, `doomsday_tick` and `ai_tick` run after
  the execution list; the alliance-extension prompt for AIs is not called from
  `alliances_expire_of`; relations decay per player row only for players with
  a relation row.

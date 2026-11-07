# Parity harness

Lockstep comparison of the C engine's TS-parity build (`make parity`, see
`docs/ts_parity.md`) with the OpenFront TypeScript engine.

| File | Role |
|---|---|
| `digest.py` | per-tick digest of the observable state (definition) |
| `trace_ts.ts` | runs a scenario on the TS engine, writes the digest per tick |
| `trace_c.py` | the same on the C engine (`OFE_LIB`, default `build-parity/libofe.so`) |
| `diff.py` | runs both, reports the first diverging tick and the rows that differ |
| `scenarios/` | scenarios that must match on every tick (`make parity-check`) |

```bash
make parity
export OPENFRONT_DIR=<openfront checkout with npm ci> OFE_MAPS_DIR=$OPENFRONT_DIR/resources/maps
python3 tools/parity/diff.py tools/parity/scenarios/core_humans.json [--ticks N] [--refresh]
```

TS traces are cached in `out/` by scenario contents and OpenFront commit;
`--refresh` reruns TS.

## Scenario format

```json
{
  "name": "core_humans",
  "map": "world", "map_size": "compact",
  "game_id": "PARITY01",
  "difficulty": "medium", "nations": "disabled", "bots": 0, "humans": 4,
  "ts_config": {}, "c_config": {},
  "ticks": 1500,
  "intents": [{"tick": 0, "player": 1, "type": "spawn", "x": 495, "y": 97}, ...]
}
```

- `nations`: `"disabled"`, `"default"` or a number (TS `GameConfig.nations`).
- `ts_config` / `c_config`: extra fields for TS `GameConfig` / C `OfeConfig`.
- Players are small ids: humans are `1..humans` in order.
- An intent with `"tick": t` is in TS turn `t`, and is pushed before C's tick `t`.
- Intent types are the C names (`attack`, `boat_attack`, `build_unit`, ...).
  Fields: `player`, `target` (player small id, 0 = terra nullius), `x`/`y`,
  `troops` (absolute; omitted = the engine default), `unit` (TS unit id),
  `unit_type` (C unit name), `amount`, `flag` (start/stop, rocket up,
  disconnected). `cancel_attack` names the attack by its `target`.

## Digest extras and generated scenarios

Besides players, units, attacks and alliances the digest holds `ex` (per player:
disconnected, traitor ticks left, active target; first row = the winner) and `em`
(embargoes), so the social intents are checked too. `make_scenarios.py`
regenerates `war_12humans.json` and `social_donations.json` by playing them on
the C parity build (so attacks hit real neighbours); `win_check.json` is
`core_humans` with a 2% win bar.

## Path code (nukes, SAM missiles, shells, rails, trains)

`air_rail_paths.py` compares `engine/src/paths_ts.c` with the TS pathfinders
query by query (`air_rail_paths.ts` runs the TS side on a real game,
`air_rail_paths_c.c` the C side; both print the same text lines):

```bash
make parity && export OPENFRONT_DIR=<openfront checkout>
python3 tools/parity/air_rail_paths.py --maps world:1,world:0,iceland:1 --n 3000   # name:compact
```

Kinds: parabola (full path, stepped tiles per tick, current index), MIRV speed,
air (path and stepper calls with fixed or moving targets, any seed), rail tiles,
and rail networks (structures placed one by one through the real
Factory/City/Port executions: railroads and their tiles, station neighbours,
cluster trade-station order, station paths, train and car tiles per tick).

## Water pathfinder (`engine/src/pathfind_ts.c`)

`water_paths.ts` runs OpenFront's real `WaterPathFinder` (path queries,
multi-source queries and whole voyages of `next()` calls, with destination
changes, teleports and invalidations) on a map and writes queries plus TS
results; `water_paths_c.c` replays them on `pathfind_ts.c` and compares tile for
tile (also reports the cost per query; repeated queries come from the route memo).
This is the engine's only ship router in both builds.

```bash
cd $OPENFRONT_DIR && node_modules/.bin/tsx <repo>/tools/parity/water_paths.ts --ref . \
    --map world --size compact --queries 3000 --seed 1 --out /tmp/world_compact.txt
cd <repo> && make build-parity/water_paths_c
build-parity/water_paths_c $OPENFRONT_DIR/resources/maps/world 1 /tmp/world_compact.txt   # 1 = compact
```

## Unit scenarios

`make_unit_scenarios.py` plays builds, upgrades (amount > 1), deletes, captures, atom/hydrogen/MIRV launches (both rocket
directions, stacked, at allies, late alliances) with and without SAM cover on the C parity build and records them.

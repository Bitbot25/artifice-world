# Fidelity harness

Statistical comparison of the C engine with the real OpenFront TypeScript engine, plus a TS state-dump to C import
adapter. The OpenFront checkout (AGPL) is never copied here; it is loaded at runtime.

## Setup

```bash
tools/fetch_openfront.sh                 # into third_party/openfront (or point OPENFRONT_DIR elsewhere)
(cd third_party/openfront && npm ci --ignore-scripts)   # add --engine-strict=false on Node < 24
export OFE_MAPS_DIR=$PWD/third_party/openfront/resources/maps
make
```

## Compare

```bash
python3 tools/fidelity/compare.py --seeds 20 --jobs 4            # writes tools/fidelity/REPORT.md
python3 tools/fidelity/compare.py --only pvp,struct --refresh
```

Sections: `idle expand tn pvp conquer struct enclave spawn ai boats`. Runs are cached in `tools/fidelity/out/`
(git-ignored; keyed by scenario and engine versions). `findings.md` is spliced into the report; plots go to `img/`
(needs matplotlib). Missing C features show up as empty/`n/a` sections.

## Pieces

| File | What |
| --- | --- |
| `ts_runner.ts` | Scenario on the TS engine (run from inside the checkout with its `tsx`), JSON lines out |
| `c_runner.py` | Same scenario on the C engine, same output |
| `scenarios.py`, `stats.py`, `common.py` | Scenario generators (arena search on the map), KS/descriptive stats, helpers |
| `ts_dump_state.ts` | Dumps a TS game (from a game record replay, a TS snapshot, or a scenario) to `*.ofs` |
| `python/artifice_engine/ts_import.py` | Rebuilds a C game from a dump through the state-edit API |

Single runs:

```bash
cd third_party/openfront && npx tsx ../../tools/fidelity/ts_runner.ts --ref . --scenario s.json --out ts.jsonl
python3 tools/fidelity/c_runner.py --scenario s.json --out c.jsonl
```

## Scenario JSON

`name, map, map_size (compact|normal), difficulty, humans, nations (n|"default"|"disabled"), bots, seeds, ticks,
sample_every, sample_players ("all"|"humans"), starting_gold, spawns {human: [x,y]}, jitter [[dx,dy] per seed],
setup_delay (default 25 when setup is present), setup [...], intents [...]`.

Players are referenced by human index (1..humans; 0 = terra nullius; `"p:<id>"` for others, ids match in both
engines for humans). Times are ticks after the spawn phase (plus the setup delay). Setup ops (applied once, both
engines): `clear` (player or "all"), `claim` (shape rect x0,y0,x1,y1 | disc cx,cy,r; land only), `troops`, `gold`.
Intents: `attack {target, troops|frac}`, `boat {x,y,troops}`, `build {unit: City|DefensePost|..., x, y}`,
`cancel_attack`; each with `tick`, optional `every`/`until`.

Output lines: `meta`, `sample {t, p: [[id,type,tiles,troops,gold,alive]]}`, `attack` (start/end tick, troops sent
and left, tiles taken, tile counts before/after), `attack_open`, `boat` (launch, landing, distance, arrived), `end`.

## State dump and import

```bash
cd third_party/openfront
npx tsx ../../tools/fidelity/ts_dump_state.ts --ref . --record game.json --at 3000,6000 --out /tmp/g   # replay a record
npx tsx ../../tools/fidelity/ts_dump_state.ts --ref . --snapshot snap.bin --out /tmp/s
```

```python
from artifice_engine import ts_import
res = ts_import.import_state(ts_import.load_dump("/tmp/g_3000.ofs"), maps_dir="third_party/openfront/resources/maps")
game, ids, warnings = res.game, res.player_map, res.warnings   # ids: TS smallID -> C player id
```

TS smallIDs map to C ids by type (humans, nations, tribes, in order; nations by name when possible). Limits (also
listed in `warnings`): C tick restarts at 0, ships/missiles/trains/nukes are not imported, structures under
construction come in finished, running attacks and embargoes are dropped, team games match partitions only.

`tests/python/test_ts_import.py` runs on a synthetic dump; the real-TS round trip skips when the checkout is absent.

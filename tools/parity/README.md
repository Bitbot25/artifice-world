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

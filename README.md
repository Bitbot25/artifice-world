# Artifice World

A C99 approximation of the [OpenFront.io](https://github.com/openfrontio/OpenFrontIO)
game engine for reinforcement-learning training, with a Python API.

It implements the live game's rules and constants (combat, troop and gold
formulas, unit costs and ranges, win threshold, overtime, doomsday clock) but
is **not** bit-exact with the TypeScript engine. It is deterministic against
itself: the same map, config, seed and intents always give the same game, on
any thread count. Bit-exact replays of downloaded human games are meant to go
through the TypeScript engine, with the resulting state imported through the
state-edit API.

## Build

```bash
make                       # build/libofe.so, build/libofe.a
tools/fetch_openfront.sh   # OpenFront maps + TS engine into third_party/ (not vendored)
export OFE_MAPS_DIR=$PWD/third_party/openfront/resources/maps
make test                  # C and Python tests
make bench                 # throughput on the World map
```

Requires a C99 compiler with OpenMP (gcc or clang), Python 3.9+ and numpy.

## Python

```python
import sys; sys.path.insert(0, "python")
import artifice_engine as ae

world = ae.Map.load(f"{maps}/world")
cfg = ae.make_config(num_humans=1, num_bots=100, difficulty=ae.DIFFICULTY_HARD)
game = ae.Game(world, cfg, seed=1)

game.push(game.intent("spawn", player=1, tile=world.tile(500, 300)))
game.step(250)                                   # 1 tick = 100 ms
game.push(game.intent("attack", player=1, target=0, troops=5000))
game.step(10)                                    # decision cadence is up to you

owners = game.owner_map()        # (H, W) uint16 owner ids
players = game.players()         # structured array (PLAYER_DTYPE)
units = game.units()             # structured array (UNIT_DTYPE)
events = game.drain_events()     # tile flips, units, attacks, alliances, deaths...

branch = game.clone()            # copy-on-write tiles: ~tens of microseconds
game.restore(branch)

games = [ae.Game(world, cfg, seed=s) for s in range(64)]
ae.step_batch(games, ticks=10)   # C threads, GIL released
```

State edits for sensitivity tests and scenario setup: `set_owner`,
`set_owner_map`, `set_troops`, `set_gold`, `add_unit`, `remove_unit`,
`set_alliance`, `launch_nuke`, `set_fallout`, `set_relation`,
`set_disconnected`, `end_spawn_phase`.

## Layout

| Path | What |
| --- | --- |
| `engine/include/ofe.h` | Public C API (plain-data structs, mirrored in Python) |
| `engine/src/internal.h` | Data layout: map, copy-on-write tiles, struct-of-arrays tables |
| `engine/src/subsystems.h` | Contract between the core and the unit / AI modules |
| `engine/src/*.c` | One file per subsystem (see the module map in `internal.h`) |
| `python/artifice_engine/` | ctypes bindings and the numpy-facing API |
| `tests/c`, `tests/python` | Tests (synthetic maps; real maps when `OFE_MAPS_DIR` is set) |
| `tools/` | Benchmark, OpenFront fetch script |

## Design

* **Data-oriented.** Players, units and attacks are struct-of-arrays tables.
  Each table's columns are declared once in an X-macro (`PLAYER_COLUMNS`,
  `UNIT_COLUMNS`, `ATTACK_COLUMNS`), which generates allocation, cloning and
  freeing; adding a field is one line.
* **Copy-on-write tiles.** Tile state (owner, fallout) and each tile's slot in
  its owner's border list live in 4096-tile chunks shared between clones and
  copied on first write, so cloning a game costs O(players + units), not
  O(tiles).
* **Fixed tick order.** intents → players (growth, alliances, enclaves) →
  land attacks → units (structures, ships, nukes, rail) → doomsday clock →
  built-in AI → win check.
* **Own RNG and math.** splitmix64 streams per subsystem, libm math. No
  attempt at reproducing OpenFront's PRNG draws, iteration order or DetMath.

## License

MIT for this repository's code. OpenFront.io's code is AGPL-3.0 and its maps
are CC BY-SA 4.0; neither is included here (`tools/fetch_openfront.sh`
downloads them separately).

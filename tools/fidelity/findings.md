Scope: world map (compact, 1000x500). The single-player scenarios (idle, expand, land attacks, structures, enclaves,
boats) start both engines from one scenario script (same spawn tile, then explicit tile / troop / gold setup), 8-20
seeds each, with a per-seed offset of up to 6 tiles so both engines have a comparable spread (the TS engine is
deterministic for a given geometry). The AI section runs 60 tribes and the map's 72 nations with no humans for 6000
ticks (Medium difficulty, 6 seeds; spawn positions are random in both engines, so only distributions compare).
Nukes, rail, doomsday, embargoes and team games exist in C but are not scripted in the comparison.

**What matches**

| Area | Result |
|---|---|
| Idle player troop and gold growth, 6000 ticks | bit-identical |
| Spawn: tiles, troops, gold for 12 humans; spawn phase length (tick 202) | identical |
| Expanding player (attack on terra nullius every 10 s), t=100..4000 | tiles within 1%, troops +0.2% |
| Land attack on terra nullius, 9 cells | tiles conquered: median delta 0.0%, p90 0.3% |
| Land attack on a player, 36 cells | tiles conquered: median +0.6%, p90 1.9%, worst cell +2.8% |
| Wiping out an 80-tile player; enclave annexation; city and defense post | identical or within 0.2% |
| Boat travel time, 10 routes (25 to 350 tiles) | all land in both except boat_d350_0 (neither within 700 ticks); per-route C/TS ratio mean 0.98 (min 0.69, max 1.12); boat_d350_1: 696 vs 697 ticks |
| Nation spawn size at t=0 | 46.6 vs 47.2 tiles per nation (C used to give a fixed 49.9) |
| Tribes alive at t=600 | 14.5 vs 12.5 (was 23 vs 12.5); all gone by t=2400 in both |
| Nation land at t=600 / t=6000 | 139k vs 140k / 156k vs 158k tiles; nations alive at t=6000: 13.5 vs 12.5 |
| Leader's land share at t=6000 | 0.33 vs 0.41 (n=6, std 0.13: not distinguishable) |

**What changed since the first report (and why it mattered)**

* Tick ordering (the old discrepancies 1 to 4: attack start, regeneration vs attack, spawn phase length, enclave
  latency) is fixed and the idle/expand/attack numbers above are exact or within a percent.
* Nation timing was the cause of the "tribes die slower in C" gap. OpenFront builds a nation's behaviours and sends its
  opening attack on the very first tick after the spawn phase, and from then on its full decisions fall on ticks where
  `tick % rate == offset` (structure-only turns at 1/3 and 2/3 of the period). C started the opening on a random
  tick in the first period and its first real decision one period after that, so nations began taking tribe land about
  60 ticks late. Both now follow the TS schedule.
* Nation spawns: tribes are placed first, nations then pick a random free land tile within 25 tiles of their map spot
  (half of the mountain tiles are skipped) and hop to a new spot on every beat during the spawn phase, so their discs
  are cut short by tribes that were already there. An explicit spawn tile is never swapped for a random one.
* AI alliance answers are given on the AI's own turn (OpenFront's AIs look at their inbox only when they wake up):
  nations reject everything asked during the spawn phase, tribes accept everything, and neither asks to extend an
  alliance on its own (only an agent's extension ask is answered).
* Ship routing: both routing grids cut straits one or two tiles wide, which closed the Mediterranean, Black Sea and
  Baltic and sent Arctic to Pacific boats round the Atlantic (1250 ticks instead of about 700 in boat_d350_1). A full-
  resolution grid is now searched when both grids fail, or when a grid route is over three times the straight line
  (greedy A*, built once per map, memoised). All 297 random shore pairs of the world test are routable (9 failed before).
  AI "could a boat go there" probes no longer search at all (same water body is enough).
* A cancelled transport ship sails to the own shore nearest to where it is (25% of the troops lost on arrival), not
  back along its route.

**Remaining differences (not fixed)**

1. Pathfinding is not OpenFront's hierarchical A*: routes come from weighted A* on routing grids plus string pulling.
   Straight stretches take the same number of ticks, detours can differ by tens of percent (boat_d50_1: 74 vs 107 ticks,
   boat_d200_1: 414 vs 371).
2. The retreat shore is chosen by straight-line distance, OpenFront's closestShoreByWater measures by water.
3. Warship and trade-ship choices (target selection, port capacity) are simpler; only boats are compared here.
4. Nations do not use OpenFront's walled-in-bot annexation planner (Hard and Impossible only), their boat targets are
   not checked against hostile warships, and "largest cluster" boxes are the border bounding box. Medium is what the
   comparison runs.
5. MIRV targeting and nuke trajectories are sampled and SAM interception is judged by range of the blast, not by the
   trajectory; no nuke price memory or strike follow-ups for nations. A 20-minute Impossible world game launches about
   250 nukes in C; there is no TS number to compare it with yet.
6. Enclave annexation uses no diagonal-pinch pass (nuke craters).
7. No emojis, no tutorial logic, no team-spawn areas.
8. Random draws are not reproduced: only distributions can be compared, and with 6 seeds the AI rows have wide
   confidence intervals (leader share, nations alive at t=6000).

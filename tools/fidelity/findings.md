## Findings (hand-written; the tables further down are generated)

Scope: world map (compact, 1000x500), identical starting states built by one scenario script for both engines (same
spawn tile, then explicit tile / troop / gold setup), 8-20 seeds per scenario. The TS engine is deterministic for a
given geometry, so every scenario is also translated by a per-seed offset (up to 6 tiles) to give both engines a
comparable spread. C features exercised: troop/gold growth, land attacks, spawn, enclave annexation, city and defense
post. Nukes, rail and doomsday exist in C but are not scripted here. Ships and the nation/tribe AI are stubs in C.

**Matches**

| Area | Result |
|---|---|
| Idle player troop and gold growth, 6000 ticks | bit-identical |
| Spawn: tiles, troops, gold for 12 humans (inland and coastal) | identical |
| Expanding player (attack on terra nullius every 10 s), t=100..4000 | tiles within 1% (final 55,419 identical), troops +0.2% at t=500..1000 |
| Land attack on terra nullius, 9 cells | tiles conquered: median delta 0.0%, p90 0.3% |
| Land attack on a player, 36 cells | tiles conquered: median +0.6%, p90 1.9%, worst cell +2.8% |
| Wiping out an 80-tile player | tiles, gold, dead flag identical |
| Enclave annexation (r=2..12 inside r=36) | annexed in 100% of games in both, same tiles |
| City (+250k max troops) and defense post | troops within 0.2% then exact; gold cost identical; 31.5 vs 31.4 tiles into a post |

**Discrepancies (small; tick ordering, not formulas)**

1. Attack start timing. In TS a new attack execution is initialised after the tick's executions have ticked, so its
   first conquest is on the next tick. In C (intents, players, attacks order in game.c) the attack also conquers on
   the tick it is issued. Attacks finish about 1 tick earlier in C (e.g. 7.4 vs 8.6 ticks for 3k-troop attacks) and C
   conquers +0.6% more tiles at a fixed horizon (up to +2.8% for 10k-troop attacks). Fix: let attacks created in
   intents_apply skip their first attacks_tick (engine/src/attack.c attacks_tick / attack_start).
2. Troop regeneration vs attack start: TS regenerates first and then deducts the attack troops; C deducts first
   (attack_start runs from intents_apply before players_tick). Visible as +0.2% troops one tick after a build or
   attack, and defender troops at attack start.
3. Spawn phase ends at tick 202 in TS (`ticks() > numSpawnPhaseTurns`) and 201 in C.
4. Enclave annexation latency: C annexes on the first tick; TS checks each player every ~20 ticks, staggered by
   `simpleHash(id) % 20`, and only after a territory change at or after `lastCalc`, so TS can lag by up to ~20 ticks
   (mean 2.3 to 7 ticks vs 2). The harness waits 25 ticks before applying setup edits for that reason.
5. Nation spawn size: nations average 47.2 tiles in TS vs 49.9 in C at game start (3,396 vs 3,591 for 72 nations);
   tribes 51.9 vs 52.0 (TS truncates spawn discs near coasts/overlaps).
6. Conquest loot: a conquered 80-tile player with 50,000 gold gave the attacker nothing in either engine
   (identical, but check TS conquerPlayer against the intended rule if loot matters).

**Not comparable yet**

* Boats: TS travel time is about 1.87 x straight-line distance (33 ticks at 25 tiles, 155 at 100, 317 at 200,
  697 at 350). C has no transport ships; the section fills in when ships land.
* Tribe/nation AI (C stub): after 6000 ticks (60 tribes + 72 nations) nations hold 158,029 tiles in TS vs 151,699 in
  C (-4%), but 12.5 vs 20.3 nations are alive, and TS tribes are all gone by tick 2400 (C: 1.2 left). Leader land
  share at t=2400: 11.4% vs 16.3%. No game reached the 80% bar in 6000 ticks in either. Re-run after the AI lands.
* Nukes, trade/trains, doomsday, alliances, embargoes and team games are not scripted in the comparison.

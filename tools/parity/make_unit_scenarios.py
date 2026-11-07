#!/usr/bin/env python3
"""Writes the unit parity scenarios (units_structures.json, units_nukes.json, ...).

    OFE_MAPS_DIR=... python3 tools/parity/make_unit_scenarios.py [name ...]

Like make_scenarios.py, the intents are chosen by playing the game on the C
parity build (so builds hit owned land, nukes hit real neighbours, upgrades
name real structures) and recorded as a neutral scenario; the scenario is then
checked against the TS engine with diff.py, which is what proves parity.
"""
from __future__ import annotations

import json
import os
import random
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
os.environ.setdefault("OFE_LIB", str(HERE.parents[1] / "build-parity" / "libofe.so"))
os.environ.setdefault("OFE_PARITY", "1")
sys.path.insert(0, str(HERE))

import numpy as np  # noqa: E402

import trace_c  # noqa: E402
from make_scenarios import SPOTS, neighbours  # noqa: E402

UNIT = ["transport_ship", "warship", "shell", "sam_missile", "port", "atom_bomb", "hydrogen_bomb", "trade_ship",
        "missile_silo", "defense_post", "sam_launcher", "city", "mirv", "mirv_warhead", "train", "factory"]
STRUCTURES = ["city", "defense_post", "missile_silo", "sam_launcher", "port", "factory"]


def play(sc: dict, decide, every: int = 1) -> dict:
    """Plays sc on the C engine; decide(t, st) returns the intents of turn t (any later turn too)."""
    m, g = trace_c.make_game(sc)
    sc["intents"] = []
    queue: dict[int, list] = {}
    for t in range(sc["ticks"]):
        st = None
        if t == 0 or (t >= 205 and t % every == 0):
            players = g.players()
            st = dict(players={int(p["id"]): p for p in players},
                      owners=g.owner_map().reshape(m.height, m.width), units=g.units(), attacks=g.attacks(),
                      alliances=g.alliances(), tick=t, m=m)
            st["nb"] = neighbours(st["owners"])
        for it in decide(t, st) or []:
            queue.setdefault(it["tick"], []).append(it)
        intents = queue.pop(t, [])
        sc["intents"] += intents
        conv = [x for x in (trace_c.to_c_intent(g, m, it) for it in intents) if x is not None]
        if conv:
            g.push(conv)
        g.step(1)
    return sc


def own_tiles(st, p):
    ys, xs = np.nonzero(st["owners"] == p)
    return list(zip(xs.tolist(), ys.tolist()))


def pick(rnd, seq):
    return seq[rnd.randrange(len(seq))] if seq else None


def my_units(st, p, types=None):
    u = st["units"]
    out = []
    for r in u:
        if int(r["owner"]) != p:
            continue
        name = UNIT[int(r["type"])]
        if types is None or name in types:
            out.append((int(r["id"]), name, int(r["tile"]), int(r["flags"]) & 1, int(r["level"])))
    return out


def expand(t, p, st, rnd):
    row = st["players"][p]
    if row["num_tiles"] <= 0:
        return []
    return [dict(tick=t, player=p, type="attack", target=0, troops=max(1, int(row["troops"] * rnd.uniform(0.2, 0.5))))]


def war_move(t, p, st, rnd, allies):
    """An attack on a neighbour that is not an ally."""
    foes = [q for q in sorted(st["nb"].get(p, ())) if q not in allies]
    row = st["players"][p]
    if not foes:
        return []
    q = rnd.choice(foes)
    return [dict(tick=t, player=p, type="attack", target=q, troops=max(1, int(row["troops"] * rnd.uniform(0.3, 0.8))))]


def allies_of(st, p):
    return {int(a["b"]) if int(a["a"]) == p else int(a["a"]) for a in st["alliances"] if p in (int(a["a"]), int(a["b"]))}


def build_move(t, p, st, rnd, kind=None, near_existing=0.3):
    tiles = own_tiles(st, p)
    if not tiles:
        return []
    kind = kind or rnd.choice(["city", "city", "defense_post", "missile_silo", "sam_launcher", "port", "factory"])
    mine = my_units(st, p, STRUCTURES)
    if mine and rnd.random() < near_existing:
        _, _, tile, _, _ = rnd.choice(mine)
        x, y = tile % st["m"].width + rnd.randint(-4, 4), tile // st["m"].width + rnd.randint(-4, 4)
    else:
        x, y = pick(rnd, tiles)
    return [dict(tick=t, player=p, type="build_unit", unit_type=kind, x=int(x), y=int(y))]


def upgrade_move(t, p, st, rnd):
    mine = my_units(st, p, ["city", "missile_silo", "sam_launcher", "port", "factory"])
    if not mine:
        return []
    uid, _, _, _, _ = rnd.choice(mine)
    return [dict(tick=t, player=p, type="upgrade_structure", unit=uid, amount=rnd.choice([1, 1, 2, 3, 5]))]


def delete_move(t, p, st, rnd):
    mine = my_units(st, p, STRUCTURES)
    if not mine:
        return []
    uid, _, _, _, _ = rnd.choice(mine)
    return [dict(tick=t, player=p, type="delete_unit", unit=uid)]


def base_scenario(name, game_id, humans, ticks, gold, description):
    return dict(map="world", map_size="compact", game_id=game_id, difficulty="medium", nations="disabled", bots=0,
                humans=humans, name=name, description=description, ticks=ticks,
                ts_config={"startingGold": gold, "customAllianceDuration": 1},
                c_config={"starting_gold": gold, "alliance_duration_minutes": 1})


# ---- structures: every structure, upgrades, deletes, captures ---------------------------------------

def structures(seed=11):
    rnd = random.Random(seed)
    # Port and Factory are left out: their trade ships and trains are a later wave.
    kinds = ["city", "city", "defense_post", "missile_silo", "sam_launcher"]

    def decide(t, st):
        if t == 0:
            return [dict(tick=0, player=i + 1, type="spawn", x=x, y=y) for i, (x, y) in enumerate(SPOTS[:8])]
        if st is None or t % 9 != 0:
            return []
        alive = [p for p, r in st["players"].items() if r["num_tiles"] > 0]
        if not alive:
            return []
        p = alive[(t // 9) % len(alive)]
        allies = allies_of(st, p)
        if t < 700:
            return expand(t, p, st, rnd)
        r = rnd.random()
        if r < 0.40:
            return build_move(t, p, st, rnd, rnd.choice(kinds))
        if r < 0.58:
            return upgrade_move(t, p, st, rnd)
        if r < 0.64:
            return delete_move(t, p, st, rnd)
        if r < 0.80:
            return war_move(t, p, st, rnd, allies)
        if r < 0.86:
            return expand(t, p, st, rnd)
        if r < 0.90:
            q = rnd.choice([q for q in alive if q != p] or [p])
            return [dict(tick=t, player=p, type="alliance_request", target=q),
                    dict(tick=t + 2, player=q, type="alliance_request", target=p)]
        return []

    sc = base_scenario("units_structures", "UNITS001", 8, 4500, 3_000_000,
                       "8 humans: expansion, every structure but port and factory built (costs scale with count), "
                       "upgrades with amount > 1, deletions, structures captured and destroyed in wars.")
    return play(sc, decide)


# ---- nukes -------------------------------------------------------------------------------------------

def launch_move(t, p, st, rnd, alive, allies, kinds, near_sam=0.5, amounts=(1, 1, 1, 2, 3)):
    """A nuke at another player: near one of their SAM launchers, an ally's land, or anywhere on their land."""
    foes = [q for q in alive if q != p]
    if not foes:
        return []
    q = rnd.choice(foes)
    if allies and rnd.random() < 0.2:
        q = rnd.choice(sorted(allies))
    sams = [u for u in my_units(st, q, ["sam_launcher"])]
    w = st["m"].width
    tile = None
    if sams and rnd.random() < near_sam:
        _, _, st_tile, _, _ = rnd.choice(sams)
        x, y = st_tile % w + rnd.randint(-6, 6), st_tile // w + rnd.randint(-6, 6)
        if 0 <= y < st["m"].height and 0 <= x < w and st["owners"][y][x] == q:
            tile = (x, y)
    if tile is None:
        tiles = own_tiles(st, q)
        if not tiles:
            return []
        tile = pick(rnd, tiles)
    kind = rnd.choice(kinds)
    return [dict(tick=t, player=p, type="build_unit", unit_type=kind, x=int(tile[0]), y=int(tile[1]),
                 flag=rnd.choice([0, 1]), amount=rnd.choice(amounts))]


def nukes(seed=21, humans=8, name="units_nukes", ticks=6200, gold=600_000_000, kinds=None, game_id="UNITS002",
          description="", war=0.08, sam_cap=3, sam_cover_all=False, sam_from=3000, launch=0.22):
    rnd = random.Random(seed)
    kinds = kinds or ["atom_bomb"] * 5 + ["hydrogen_bomb", "mirv"]

    def decide(t, st):
        if t == 0:
            return [dict(tick=0, player=i + 1, type="spawn", x=x, y=y) for i, (x, y) in enumerate(SPOTS[:humans])]
        if st is None or t % 5 != 0:
            return []
        alive = [p for p, r in st["players"].items() if r["num_tiles"] > 0]
        if not alive:
            return []
        p = alive[(t // 5) % len(alive)]
        allies = allies_of(st, p)
        if t < 1200:
            return expand(t, p, st, rnd)
        silos = my_units(st, p, ["missile_silo"])
        sams = my_units(st, p, ["sam_launcher"])
        r = rnd.random()
        sam_n = sam_cap if (p % 2 == 1 or sam_cover_all) and t >= sam_from else 0  # SAM cover comes late, not for everybody
        if t < 1800:
            if len(silos) < 3 and r < 0.35:
                return build_move(t, p, st, rnd, "missile_silo", 0.0)
            if len(sams) < sam_n and r < 0.7:
                return build_move(t, p, st, rnd, "sam_launcher", 0.0)
            if (silos or sams) and r < 0.8:
                return upgrade_move_types(t, p, st, rnd, ["missile_silo", "sam_launcher"])
            if r < 0.86:
                q = rnd.choice([q for q in alive if q != p] or [p])
                return [dict(tick=t, player=p, type="alliance_request", target=q),
                        dict(tick=t + 2, player=q, type="alliance_request", target=p)]
            return []
        if silos and r < launch:
            out = launch_move(t, p, st, rnd, alive, allies, kinds)
            if out and rnd.random() < 0.25:
                # a late alliance between launcher and victim calls the nuke off
                q = st["owners"][out[0]["y"]][out[0]["x"]]
                if q and q != p:
                    out += [dict(tick=t + 12, player=p, type="alliance_request", target=int(q)),
                            dict(tick=t + 14, player=int(q), type="alliance_request", target=p)]
            return out
        if r < launch + 0.08:
            return upgrade_move_types(t, p, st, rnd, ["missile_silo", "sam_launcher"])
        if r < launch + 0.08 + war:
            return war_move(t, p, st, rnd, allies)
        if r < launch + 0.4:
            return expand(t, p, st, rnd)
        if r < launch + 0.47 and len(sams) < sam_n + 2:
            return build_move(t, p, st, rnd, "sam_launcher", 0.0)
        return []

    sc = base_scenario(name, game_id, humans, ticks, gold,
                       description or "8 humans with silos and SAMs launching atom bombs, hydrogen bombs and MIRVs (both rocket "
                       "directions, stacked launches) at each other and at allies; blasts, fallout, SAM interceptions.")
    return play(sc, decide)


def upgrade_move_types(t, p, st, rnd, types):
    mine = my_units(st, p, types)
    if not mine:
        return []
    uid, _, _, _, _ = rnd.choice(mine)
    return [dict(tick=t, player=p, type="upgrade_structure", unit=uid, amount=rnd.choice([1, 1, 2]))]


def nukes_sam():
    return nukes(seed=31, humans=4, name="units_sam", ticks=4200, gold=800_000_000, game_id="UNITS003", war=0.0, sam_cap=5, sam_cover_all=True, sam_from=1000,
                 description="4 humans with a ring of SAM launchers each: salvos of atom bombs, hydrogen bombs and MIRV "
                             "warheads against SAM cover, interception timing, range growth after upgrades.")


SCENARIOS = {"units_structures": structures, "units_nukes": nukes, "units_sam": nukes_sam}


def main():
    names = sys.argv[1:] or list(SCENARIOS)
    out = HERE / "scenarios"
    for n in names:
        sc = SCENARIOS[n]()
        (out / f"{n}.json").write_text(json.dumps(sc, indent=1) + "\n")
        kinds: dict[str, int] = {}
        for it in sc["intents"]:
            k = it["type"] + (":" + it["unit_type"] if "unit_type" in it else "")
            kinds[k] = kinds.get(k, 0) + 1
        print(n, len(sc["intents"]), "intents", dict(sorted(kinds.items())))


if __name__ == "__main__":
    main()

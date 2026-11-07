#!/usr/bin/env python3
"""Writes the busy parity scenarios (war_12humans.json, social_donations.json).

    OFE_MAPS_DIR=... python3 tools/parity/make_scenarios.py

The intents are chosen by playing the game on the C parity build (so every
attack hits a real neighbour, extensions fall into live alliances, ...) and
recorded as a neutral scenario. The scenario is only a script: it is then
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

# Compact-World nation spots in Europe and Western Asia (manifest / 2).
SPOTS = [(495, 97), (474, 110), (515, 96), (507, 68), (484, 66), (541, 63), (551, 91), (567, 105),
         (536, 115), (462, 93), (502, 125), (454, 132)]


def neighbours(owners: np.ndarray) -> dict[int, set[int]]:
    """Players that share a border (4-neighbourhood) with each player."""
    out: dict[int, set[int]] = {}
    for a, b in ((owners[:, :-1], owners[:, 1:]), (owners[:-1, :], owners[1:, :])):
        m = (a != b) & (a > 0) & (b > 0)
        for x, y in set(zip(a[m].tolist(), b[m].tolist())):
            out.setdefault(x, set()).add(y)
            out.setdefault(y, set()).add(x)
    return out


def play(sc: dict, decide) -> dict:
    """Plays sc on the C engine; decide(t, state) returns the intents of turn t."""
    m, g = trace_c.make_game(sc)
    sc["intents"] = []
    queue: dict[int, list] = {}
    for t in range(sc["ticks"]):
        state = None
        if t >= 215 or t == 0:
            players = g.players()
            owners = g.owner_map().reshape(m.height, m.width)
            state = dict(
                players={int(p["id"]): p for p in players},
                attacks=g.attacks(), alliances=g.alliances(),
                nb=neighbours(owners), tick=t)
        for it in decide(t, state) or []:
            queue.setdefault(it["tick"], []).append(it)
        intents = queue.pop(t, [])
        sc["intents"] += intents
        conv = [x for x in (trace_c.to_c_intent(g, m, it) for it in intents) if x is not None]
        if conv:
            g.push(conv)
        g.step(1)
    return sc


def war(seed: int, social: bool):
    rnd = random.Random(seed)

    def decide(t, st):
        if t == 0:
            return [dict(tick=0, player=i + 1, type="spawn", x=x, y=y) for i, (x, y) in enumerate(SPOTS)]
        if st is None or t % 7 != 0:
            return []
        pl = st["players"]
        alive = [p for p, r in pl.items() if r["num_tiles"] > 0]
        if not alive:
            return []
        p = alive[(t // 7) % len(alive)]
        row = pl[p]
        nb = sorted(st["nb"].get(p, ()))
        allies = {int(a["b"]) if int(a["a"]) == p else int(a["a"]) for a in st["alliances"] if p in (int(a["a"]), int(a["b"]))}
        foes = [q for q in nb if q not in allies]
        mine = [a for a in st["attacks"] if int(a["attacker"]) == p]
        out = []
        r = rnd.random()
        troops = int(row["troops"])
        if t < 700:
            out.append(dict(tick=t, player=p, type="attack", target=0, troops=max(1, int(troops * rnd.uniform(0.2, 0.6)))))
        elif r < 0.30 and foes:
            weakest = min(foes, key=lambda q: pl[q]["troops"])
            tgt = weakest if rnd.random() < 0.5 else rnd.choice(foes)
            out.append(dict(tick=t, player=p, type="attack", target=tgt, troops=max(1, int(troops * rnd.uniform(0.3, 0.9)))))
        elif r < 0.55 and mine:
            a = rnd.choice(mine)
            out.append(dict(tick=t, player=p, type="cancel_attack", target=int(a["target"])))
        elif r < 0.72 and foes:
            q = rnd.choice(foes)
            out.append(dict(tick=t, player=p, type="alliance_request", target=q))
            if rnd.random() < 0.7:
                out.append(dict(tick=t + 3, player=q, type="alliance_request", target=p))
        elif r < 0.8 and allies:
            q = rnd.choice(sorted(allies))
            if rnd.random() < 0.5:
                out.append(dict(tick=t, player=p, type="alliance_extension", target=q))
                out.append(dict(tick=t + 2, player=q, type="alliance_extension", target=p))
        elif r < 0.81 and allies:
            out.append(dict(tick=t, player=p, type="break_alliance", target=rnd.choice(sorted(allies))))
        elif r < 0.86 and foes:
            out.append(dict(tick=t, player=p, type="alliance_reject", target=rnd.choice(foes)))
        elif r < 0.95:
            out.append(dict(tick=t, player=p, type="attack", target=0, troops=max(1, int(troops * 0.4))))
        if social:
            out += social_intents(rnd, t, p, pl, st, allies, nb)
        # intents of a later turn were generated now: keep them in order
        return out

    return decide


def social_intents(rnd, t, p, pl, st, allies, nb):
    """Donations, embargoes, targets and disconnects on top of the war."""
    out = []
    r = rnd.random()
    others = [q for q in pl if q != p]
    if r < 0.2 and allies:
        q = rnd.choice(sorted(allies))
        if rnd.random() < 0.5:
            out.append(dict(tick=t + 1, player=p, type="donate_gold", target=q, troops=int(pl[p]["gold"] * rnd.uniform(0.1, 0.6))))
        else:
            out.append(dict(tick=t + 1, player=p, type="donate_troops", target=q, troops=int(pl[p]["troops"] * rnd.uniform(0.05, 0.4))))
    elif r < 0.3 and others:
        out.append(dict(tick=t + 1, player=p, type="embargo", target=rnd.choice(others), flag=rnd.choice([0, 1, 1])))
    elif r < 0.36:
        out.append(dict(tick=t + 1, player=p, type="embargo_all", flag=rnd.choice([0, 1])))
    elif r < 0.5 and others:
        out.append(dict(tick=t + 1, player=p, type="target_player", target=rnd.choice(others)))
    elif r < 0.56:
        out.append(dict(tick=t + 1, player=p, type="mark_disconnected", flag=rnd.choice([0, 1, 1])))
    return out


def land_spots(map_name: str, n: int) -> list[tuple[int, int]]:
    """n land tiles near the middle of a compact map, spread out."""
    m = trace_c.ae.Map.load(str(trace_c.maps_dir() / map_name), compact=True)
    land = (m.terrain() & 0x80) != 0
    out: list[tuple[int, int]] = []
    for k in range(n):
        cx, cy = m.width * (0.3 + 0.4 * (k % 2)), m.height * (0.35 + 0.3 * (k // 2 % 2))
        best = min(((x, y) for y in range(m.height) for x in range(m.width) if land[y, x] and land[max(0, y - 6):y + 7, max(0, x - 6):x + 7].all()),
                   key=lambda xy: (xy[0] - cx) ** 2 + (xy[1] - cy) ** 2)
        out.append(best)
    return out


def tribes_war(seed: int, spots: list[tuple[int, int]]):
    """Humans (one per spot) among tribes: expansion, attacks on neighbouring
    tribes, alliance requests (tribes accept on their beat), extensions,
    betrayals (tribes then hit the traitor) and cancelled attacks."""
    humans = len(spots)
    rnd = random.Random(seed)

    def decide(t, st):
        if t == 0:
            return [dict(tick=0, player=i + 1, type="spawn", x=x, y=y) for i, (x, y) in enumerate(spots)]
        if st is None or t % 5 != 0:
            return []
        pl = st["players"]
        p = 1 + (t // 5) % humans
        row = pl[p]
        if row["num_tiles"] == 0:
            return []
        nb = sorted(st["nb"].get(p, ()))
        allies = {int(a["b"]) if int(a["a"]) == p else int(a["a"]) for a in st["alliances"] if p in (int(a["a"]), int(a["b"]))}
        foes = [q for q in nb if q not in allies]
        mine = [a for a in st["attacks"] if int(a["attacker"]) == p]
        troops = int(row["troops"])
        r = rnd.random()
        if t < 500 or r < 0.12:
            return [dict(tick=t, player=p, type="attack", target=0, troops=max(1, int(troops * rnd.uniform(0.2, 0.5))))]
        if r < 0.45 and foes:
            q = min(foes, key=lambda q: pl[q]["troops"]) if rnd.random() < 0.5 else rnd.choice(foes)
            return [dict(tick=t, player=p, type="attack", target=q, troops=max(1, int(troops * rnd.uniform(0.3, 0.9))))]
        if r < 0.55 and mine:
            return [dict(tick=t, player=p, type="cancel_attack", target=int(rnd.choice(mine)["target"]))]
        if r < 0.72 and foes:
            return [dict(tick=t, player=p, type="alliance_request", target=rnd.choice(foes))]
        if r < 0.85 and allies:
            return [dict(tick=t, player=p, type="alliance_extension", target=rnd.choice(sorted(allies)))]
        if r < 0.9 and allies:
            return [dict(tick=t, player=p, type="break_alliance", target=rnd.choice(sorted(allies)))]
        return []

    return decide


NO_BOATS = dict(ts_config={"disabledUnits": ["Transport"]}, c_config={"disabled_units": ["transport_ship"]})


def tribe_scenarios():
    out = []
    # Transport ships are not exact yet: switched off where the run would go past the first boat.
    out.append((dict(name="tribes_humans", map="world", map_size="compact", game_id="PARITY56", difficulty="medium",
                     nations="disabled", bots=40, humans=4, ticks=4000,
                     description="4 humans among 40 tribes on compact World (boats off): humans and tribes expand, "
                                 "fight, ally, extend, betray; tribes retaliate and hit traitors.",
                     ts_config={"disabledUnits": ["Transport"], "customAllianceDuration": 1},
                     c_config={"disabled_units": ["transport_ship"], "alliance_duration_minutes": 1}), tribes_war(5, SPOTS[:4])))
    out.append((dict(name="tribes_only", map="world", map_size="compact", game_id="PARITY78", difficulty="medium",
                     nations="disabled", bots=80, humans=0, ticks=3000, intents=[],
                     description="80 tribes alone on compact World (boats off).", **NO_BOATS), None))
    out.append((dict(name="tribes_europe", map="europe", map_size="compact", game_id="PARITY90", difficulty="hard",
                     nations="disabled", bots=60, humans=2, ticks=3000,
                     description="2 humans among 60 tribes on compact Europe, hard difficulty (boats off).",
                     ts_config={"disabledUnits": ["Transport"], "customAllianceDuration": 1},
                     c_config={"disabled_units": ["transport_ship"], "alliance_duration_minutes": 1}), tribes_war(9, land_spots("europe", 2))))
    out.append((dict(name="tribes_first_boat", map="world", map_size="compact", game_id="PARITY02", difficulty="medium",
                     nations="disabled", bots=20, humans=0, ticks=650, intents=[],
                     description="20 tribes with boats on: exact up to the first transport ship (tick 663)."), None))
    return out


def main():
    if "tribes" in sys.argv[1:]:
        for sc, decide in tribe_scenarios():
            if decide is not None:
                play(sc, decide)
            (HERE / "scenarios" / f"{sc['name']}.json").write_text(json.dumps(sc, indent=1) + "\n")
            print(sc["name"], len(sc.get("intents", [])), "intents")
        return
    base = dict(map="world", map_size="compact", game_id="PARITY12", difficulty="medium", nations="disabled", bots=0,
                humans=12, ts_config={}, c_config={})
    war_sc = dict(base, name="war_12humans", game_id="PARITY12", ticks=6000,
                  description="12 humans on compact World: expansion, wars, cancelled attacks, alliances that expire, "
                              "are extended or broken, players wiped out.",
                  ts_config={"customAllianceDuration": 1}, c_config={"alliance_duration_minutes": 1})
    social_sc = dict(base, name="social_donations", game_id="PARITY34", ticks=3000,
                     description="12 humans with donations, embargoes, target_player and mark_disconnected on top of a war.",
                     ts_config={"donateGold": True, "donateTroops": True, "customAllianceDuration": 1},
                     c_config={"donate_gold": 1, "donate_troops": 1, "alliance_duration_minutes": 1})
    out = HERE / "scenarios"
    for sc, seed, social in ((war_sc, 1, False), (social_sc, 2, True)):
        play(sc, war(seed, social))
        (out / f"{sc['name']}.json").write_text(json.dumps(sc, indent=1) + "\n")
        print(sc["name"], len(sc["intents"]), "intents")


if __name__ == "__main__":
    main()

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


def main():
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

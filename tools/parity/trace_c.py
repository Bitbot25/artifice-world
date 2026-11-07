#!/usr/bin/env python3
"""Runs a parity scenario on the C engine and writes the per-tick digest.

    OFE_LIB=build-parity/libofe.so python3 tools/parity/trace_c.py scenario.json --out c.jsonl [--detail 120,121]

Each output line is {"t": tick, "h": {component: uint32}} for the state after
that tick; ticks listed in --detail also carry "d": the full component lists
and "tiles": [[tile, owner], ...] for tiles whose owner changed that tick.
The scenario format is described in README.md.
"""
from __future__ import annotations

import argparse
import json
import os
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parents[1]
if "OFE_LIB" not in os.environ:
    os.environ["OFE_PARITY"] = "1"  # the build-parity library
sys.path.insert(0, str(REPO / "python"))
sys.path.insert(0, str(HERE))

import numpy as np  # noqa: E402

import artifice_engine as ae  # noqa: E402
from digest import TileHasher, digest  # noqa: E402

DIFFICULTY = {"easy": 0, "medium": 1, "hard": 2, "impossible": 3}


def maps_dir() -> Path:
    d = os.environ.get("OFE_MAPS_DIR")
    if d:
        return Path(d)
    ref = os.environ.get("OPENFRONT_DIR")
    if ref:
        return Path(ref) / "resources" / "maps"
    return REPO / "third_party" / "openfront" / "resources" / "maps"


def make_game(sc: dict):
    m = ae.Map.load(str(maps_dir() / sc["map"].lower()), compact=sc.get("map_size", "compact") == "compact")
    nations = sc.get("nations", "disabled")
    cfg = dict(
        difficulty=DIFFICULTY[sc.get("difficulty", "medium").lower()],
        num_humans=sc.get("humans", 0),
        num_nations=0 if nations == "disabled" else (-1 if nations == "default" else int(nations)),
        num_bots=sc.get("bots", 0),
        auto_spawn_humans=0,
        game_id=sc["game_id"],
    )
    cfg.update(sc.get("c_config", {}))
    return m, ae.Game(m, ae.make_config(**cfg))


def to_c_intent(g, m, it: dict):
    """Neutral scenario intent -> engine intent dict (None if unresolvable)."""
    d = {"type": it["type"], "player": it["player"]}
    if "x" in it:
        d["tile"] = m.tile(it["x"], it["y"])
    for k in ("troops", "amount"):
        if it.get(k) is not None:
            d[k] = it[k]
    if "unit_type" in it:
        d["unit_type"] = it["unit_type"]
    if "flag" in it:
        d["flags"] = int(it["flag"])
    if it["type"] == "cancel_attack":
        at = g.attacks()
        sel = at[(at["attacker"] == it["player"]) & (at["target"] == it["target"])]
        if len(sel) == 0:
            return None
        d["target"] = int(sel[0]["id"])
    elif "target" in it:
        d["target"] = it["target"]
    elif "unit" in it:
        d["target"] = it["unit"]
    return d


def observe(g, hasher: TileHasher, tribes_exist: bool = True):
    players = g.players()
    if not tribes_exist:  # TS adds the tribe players when their spawn executions tick, on tick 1
        players = players[players["type"] != 2]
    pl = [[int(p["id"]), int(p["troops"]), int(p["gold"]), int(p["num_tiles"])] for p in players]
    pl.sort()
    units = g.units()
    un = [[int(u["id"]), int(u["type"]), int(u["owner"]), int(u["tile"]), int(np.floor(u["troops"])),
           int(u["health"]), int(u["level"]), int(bool(u["flags"] & 1))] for u in units]
    un.sort()
    at = sorted([int(a["attacker"]), int(a["target"]), int(np.floor(a["troops"])), int(a["retreating"])]
                for a in g.attacks())
    al = sorted([int(min(a["a"], a["b"])), int(max(a["a"], a["b"])), int(a["expires_tick"])] for a in g.alliances())
    owners = g.owner_map()
    fallout = (g.tile_state() & (1 << 13)) != 0
    ex = [[0, int(g.winner), 0, 0]] + sorted([int(p["id"]), int(bool(p["flags"] & 4)), int(p["traitor_ticks_left"]), int(p["target"])] for p in players)
    ids = [int(p["id"]) for p in players]
    em = [[a, b] for a in ids for b in ids if a != b and len(ids) <= 40 and g.is_embargoed(a, b)]
    lists = {"pl": pl, "un": un, "at": at, "al": al, "ex": ex, "em": em}
    return lists, owners, digest(lists, hasher.owners(owners), hasher.mask(fallout))


def run(sc: dict, out, detail: set[int]):
    m, g = make_game(sc)
    hasher = TileHasher(m.width * m.height)
    by_tick: dict[int, list] = {}
    for it in sc.get("intents", []):
        by_tick.setdefault(int(it["tick"]), []).append(it)
    prev_owners = g.owner_map().copy()
    for t in range(int(sc["ticks"])):
        intents = [x for x in (to_c_intent(g, m, it) for it in by_tick.get(t, [])) if x is not None]
        if intents:
            g.push(intents)
        g.step(1)
        lists, owners, h = observe(g, hasher, t > 0)
        rec = {"t": t, "h": h}
        if t in detail:
            rec["d"] = lists
            changed = np.nonzero(owners.ravel() != prev_owners.ravel())[0]
            rec["tiles"] = [[int(i), int(owners.ravel()[i])] for i in changed]
        prev_owners = owners.copy()
        out.write(json.dumps(rec, separators=(",", ":")) + "\n")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("scenario")
    ap.add_argument("--out", default="-")
    ap.add_argument("--detail", default="")
    a = ap.parse_args()
    sc = json.loads(Path(a.scenario).read_text())
    detail = {int(x) for x in a.detail.split(",") if x}
    out = sys.stdout if a.out == "-" else open(a.out, "w")
    run(sc, out, detail)


if __name__ == "__main__":
    main()

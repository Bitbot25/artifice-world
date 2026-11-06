#!/usr/bin/env python3
"""Runs a fidelity scenario on the C engine (python/artifice_engine) and
writes the same JSON lines as ts_runner.ts.

    python3 tools/fidelity/c_runner.py --scenario s.json [--out o.jsonl] [--maps DIR]

Features the C engine does not have yet (ships, ...) simply produce no
records; compare.py detects that from the results.
"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))
import common  # noqa: E402  (also puts python/ on sys.path)

import artifice_engine as ae  # noqa: E402

DIFFICULTY = {
    "easy": ae.DIFFICULTY_EASY,
    "medium": ae.DIFFICULTY_MEDIUM,
    "hard": ae.DIFFICULTY_HARD,
    "impossible": ae.DIFFICULTY_IMPOSSIBLE,
}
TYPE_CHAR = {ae.PLAYER_HUMAN: "h", ae.PLAYER_NATION: "n", ae.PLAYER_BOT: "b"}

_map_cache: dict = {}


def load_map(name: str, size: str, maps_dir: Path) -> "ae.Map":
    key = (name, size, str(maps_dir))
    if key not in _map_cache:
        _map_cache[key] = ae.Map.load(str(maps_dir / name.lower()), compact=(size == "compact"))
    return _map_cache[key]


def parse_target(spec):
    if isinstance(spec, str) and spec.startswith("p:"):
        return int(spec[2:])
    return int(spec or 0)


def run_one(sc: dict, seed: int, emit, maps_dir: Path) -> None:
    sc = common.apply_jitter(sc, sc["seeds"].index(seed))
    size = sc.get("map_size", "compact")
    m = load_map(sc["map"], size, maps_dir)
    W, H = m.width, m.height
    n_humans = sc.get("humans", 0)
    nations = sc.get("nations", "disabled")
    n_nations = 0 if nations == "disabled" else (-1 if nations == "default" else int(nations))
    track_attacks = sc.get("track_attacks", sc.get("bots", 0) <= 20)
    mask = ae.EVENTS_ALL if track_attacks else ae.EVENTS_NO_TILES
    cfg = ae.make_config(
        num_humans=n_humans,
        num_nations=n_nations,
        num_bots=sc.get("bots", 0),
        difficulty=DIFFICULTY[sc.get("difficulty", "medium").lower()],
        starting_gold=int(sc.get("starting_gold", 0)),
        event_mask=mask,
    )
    game = ae.Game(m, cfg, seed=seed)

    # ---- spawn phase -------------------------------------------------------
    for k, (x, y) in (sc.get("spawns") or {}).items():
        game.push(game.intent("spawn", player=int(k), tile=m.tile(x, y)))
    guard = 0
    game.step(1)
    while game.in_spawn_phase:
        game.step(1)
        guard += 1
        if guard > 5000:
            raise RuntimeError("spawn phase did not end")
    # Scenarios with setup edits wait setup_delay ticks first (see ts_runner.ts).
    delay = sc.get("setup_delay", 25 if sc.get("setup") else 0)
    if delay:
        game.step(delay)
    T0 = game.tick
    game.drain_events()

    # ---- setup -------------------------------------------------------------
    terrain = m.terrain()
    land = common.passable_land(terrain)
    owners = None
    for op in sc.get("setup", []):
        kind = op["op"]
        if kind in ("clear", "claim"):
            if owners is None:
                owners = game.owner_map().copy()
            if kind == "clear":
                if op["player"] == "all":
                    owners[(owners >= 1) & (owners <= n_humans)] = 0
                else:
                    owners[owners == parse_target(op["player"])] = 0
            else:
                sel = common.shape_mask(op, W, H) & land
                if op.get("only_free"):
                    sel &= owners == 0
                owners[sel] = parse_target(op["player"])
            continue
        if owners is not None:
            game.set_owner_map(owners)
            owners = None
        if kind == "troops":
            game.set_troops(parse_target(op["player"]), op["value"])
        elif kind == "gold":
            game.set_gold(parse_target(op["player"]), int(op["value"]))
        else:
            raise ValueError(f"unknown setup op {kind}")
    if owners is not None:
        game.set_owner_map(owners)
    game.drain_events()

    emit({
        "ev": "meta", "engine": "c", "scenario": sc["name"], "seed": seed,
        "width": W, "height": H, "t_spawn_end": T0, "num_players": game.num_players,
        "spawn_phase_turns": T0,
    })

    # ---- main loop ---------------------------------------------------------
    sample_every = sc.get("sample_every", 10)
    humans_only = sc.get("sample_players") == "humans"

    def sample(t: int, players=None):
        pl = game.players() if players is None else players
        rows = []
        for r in pl:
            if humans_only and r["type"] != ae.PLAYER_HUMAN:
                continue
            rows.append([int(r["id"]), TYPE_CHAR[int(r["type"])], int(r["num_tiles"]),
                         int(r["troops"]), int(r["gold"]), 1 if r["flags"] & ae.PF_ALIVE else 0])
        emit({"ev": "sample", "seed": seed, "t": t, "p": rows})

    active: dict[int, dict] = {}  # attack id -> record
    boats: dict[int, dict] = {}
    track_boats = sc.get("track_boats", True)
    EV_TILE, EV_AS, EV_AE = ae.EVENT["tile_owner"], ae.EVENT["attack_started"], ae.EVENT["attack_ended"]

    def process_events(t_rel: int, pre, post):
        ev = game.drain_events()
        for e in ev:
            ty = int(e["type"])
            if ty == EV_AS:
                att, tgt = int(e["b"]), int(e["c"])
                src = int(e["d"])
                a_row = pre[att - 1]
                active[int(e["a"])] = {
                    "t0": t_rel, "attacker": att, "target": tgt,
                    "kind": "land" if src < 0 else "boat", "troops0": float(e["v"]), "tiles": 0,
                    "att_tiles0": int(a_row["num_tiles"]),
                    "def_tiles0": int(pre[tgt - 1]["num_tiles"]) if tgt else 0,
                    "def_troops0": int(pre[tgt - 1]["troops"]) if tgt else 0,
                    "src": None if src < 0 else list(m.xy(src)),
                }
            elif ty == EV_TILE:
                old, new = int(e["b"]), int(e["c"])
                for rec in active.values():
                    if rec["attacker"] == new and rec["target"] == old:
                        rec["tiles"] += 1
                        break
            elif ty == EV_AE:
                rec = active.pop(int(e["a"]), None)
                if rec is None:
                    continue
                tgt = rec["target"]
                emit({
                    "ev": "attack", "seed": seed, "t0": rec["t0"], "t1": t_rel,
                    "attacker": rec["attacker"], "target": tgt, "kind": rec["kind"],
                    "troops0": rec["troops0"], "troops_end": float(e["v"]), "tiles": rec["tiles"],
                    "att_tiles0": rec["att_tiles0"], "att_tiles1": int(post[rec["attacker"] - 1]["num_tiles"]),
                    "def_tiles0": rec["def_tiles0"], "def_tiles1": int(post[tgt - 1]["num_tiles"]) if tgt else 0,
                    "def_troops0": rec["def_troops0"], "def_troops1": int(post[tgt - 1]["troops"]) if tgt else 0,
                    "src": rec["src"],
                })

    def track_boat_units(t_rel: int):
        seen = set()
        us = game.units()
        for u in us[us["type"] == ae.UNIT["transport_ship"]]:
            uid = int(u["id"])
            seen.add(uid)
            b = boats.get(uid)
            if b is None:
                dst = int(u["target_tile"])
                b = boats[uid] = {
                    "t0": t_rel, "player": int(u["owner"]), "troops": float(u["troops"]),
                    "src": list(m.xy(int(u["tile"]))), "dst": list(m.xy(dst)) if dst >= 0 else None,
                }
            b["last"] = list(m.xy(int(u["tile"])))
        for uid in [k for k in boats if k not in seen]:
            b = boats.pop(uid)
            d = None if b["dst"] is None else float(np.hypot(b["dst"][0] - b["src"][0], b["dst"][1] - b["src"][1]))
            dl = None if b["dst"] is None else abs(b["last"][0] - b["dst"][0]) + abs(b["last"][1] - b["dst"][1])
            emit({"ev": "boat", "seed": seed, "t0": b["t0"], "t1": t_rel, "player": b["player"],
                  "troops": b["troops"], "src": b["src"], "dst": b["dst"], "last": b["last"], "dist": d,
                  "arrived": dl is not None and dl <= 2})

    def intents_for(k: int):
        out = []
        for it in sc.get("intents", []):
            t0 = it.get("tick", 0)
            hit = k == t0
            if not hit and it.get("every") and k > t0 and (not it.get("until") or k <= it["until"]) \
                    and (k - t0) % it["every"] == 0:
                hit = True
            if not hit:
                continue
            me = int(it["player"])
            row = game.player(me)
            if not (row["flags"] & ae.PF_ALIVE):
                continue
            troops = it.get("troops")
            if it.get("frac") is not None:
                troops = float(int(row["troops"] * it["frac"]))
            troops = -1.0 if troops is None else float(troops)
            if it["type"] == "attack":
                out.append(game.intent("attack", player=me, target=parse_target(it.get("target", 0)), troops=troops))
            elif it["type"] == "boat":
                out.append(game.intent("boat_attack", player=me, tile=m.tile(it["x"], it["y"]), troops=troops))
            elif it["type"] == "build":
                import re
                name = re.sub(r"(?<!^)(?=[A-Z])", "_", it["unit"]).lower()
                out.append(game.intent("build_unit", player=me, unit_type=name, tile=m.tile(it["x"], it["y"])))
            elif it["type"] == "cancel_attack":
                atk = game.attacks()
                atk = atk[atk["attacker"] == me]
                if len(atk):
                    out.append(game.intent("cancel_attack", player=me, target=int(atk[0]["id"])))
            else:
                raise ValueError(f"unsupported intent {it['type']}")
        return out

    k = 0
    pre = game.players()
    while k < sc["ticks"]:
        if k % sample_every == 0:
            sample(k, pre)
        ins = intents_for(k)
        if ins:
            game.push(*ins)
        game.step(1)
        post = game.players()
        process_events(k, pre, post)
        if track_boats:
            track_boat_units(k + 1)
        pre = post
        k += 1
        if game.is_over:
            break
    sample(k, pre)
    for aid, rec in active.items():
        row = [a for a in game.attacks() if int(a["id"]) == aid]
        emit({"ev": "attack_open", "seed": seed, "t0": rec["t0"], "attacker": rec["attacker"],
              "target": rec["target"], "kind": rec["kind"], "troops0": rec["troops0"],
              "troops_now": float(row[0]["troops"]) if row else 0.0, "tiles": rec["tiles"]})
    over = game.is_over
    team = game.winning_team
    emit({"ev": "end", "seed": seed, "t": k, "over": bool(over),
          "winner": game.winner if over and game.winner else None,
          "winner_team": team if over and team >= 0 else None})


def run(sc: dict, emit, maps_dir: Path | None = None) -> None:
    maps_dir = maps_dir or common.maps_dir()
    if maps_dir is None:
        raise SystemExit("OpenFront maps not found: set OFE_MAPS_DIR or OPENFRONT_DIR")
    for seed in sc["seeds"]:
        run_one(sc, int(seed), emit, Path(maps_dir))


def c_run(scenario: dict) -> list[dict]:
    """In-process helper for compare.py: returns the records."""
    out: list[dict] = []
    run(scenario, out.append)
    return out


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--scenario", required=True)
    ap.add_argument("--out")
    ap.add_argument("--maps")
    args = ap.parse_args()
    sc = json.loads(Path(args.scenario).read_text())
    f = open(args.out, "w") if args.out else sys.stdout
    run(sc, lambda o: f.write(json.dumps(o) + "\n"), Path(args.maps) if args.maps else None)
    if args.out:
        f.close()


if __name__ == "__main__":
    main()

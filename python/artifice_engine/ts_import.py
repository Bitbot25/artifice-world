"""Rebuilds a C-engine game from a state dump of the OpenFront TypeScript
engine (tools/fidelity/ts_dump_state.ts), through the state-edit API.

    from artifice_engine import ts_import
    dump = ts_import.load_dump("g_3000.ofs")
    res = ts_import.import_state(dump, maps_dir="third_party/openfront/resources/maps")
    game, ids = res.game, res.player_map          # ids: TS smallID -> C player id
    game.step(100)

Dump format (``*.ofs``): one JSON header line, then raw blobs described by
``header["blobs"]``: ``owners`` (uint16 LE, TS smallID per tile, row-major) and
``fallout`` (uint8 per tile). The header carries tick, players (small_id, type,
team, alive, troops, gold, ...), units, alliances and attacks.

Known limits (reported in ``ImportResult.warnings``):
  * the C game's tick restarts at 0 (alliance expiry is converted to a remaining
    duration; spawn immunity is set from the dump);
  * units that cannot be created through the edit API (ships in flight, shells,
    missiles, trains, nukes) are skipped; structures under construction are
    imported as finished; running attacks are not imported;
  * team games: C deals teams itself; players are matched to C slots so the team
    partition is preserved when group sizes agree.
"""

from __future__ import annotations

import json
import re
from dataclasses import dataclass, field
from pathlib import Path

import numpy as np

from . import Game, Map, make_config
from . import _lib as L

MAGIC = "ofe-state-dump"

# TS UnitType string values -> C unit names. Names already in C form pass through.
TS_UNIT_NAMES = {
    "Transport": "transport_ship", "Warship": "warship", "Shell": "shell", "SAMMissile": "sam_missile",
    "Port": "port", "Atom Bomb": "atom_bomb", "Hydrogen Bomb": "hydrogen_bomb", "Trade Ship": "trade_ship",
    "Missile Silo": "missile_silo", "Defense Post": "defense_post", "SAM Launcher": "sam_launcher",
    "City": "city", "MIRV": "mirv", "MIRV Warhead": "mirv_warhead", "Train": "train", "Factory": "factory",
}
IMPORTABLE_UNITS = {"port", "city", "factory", "defense_post", "missile_silo", "sam_launcher", "warship"}
DIFFICULTY = {"easy": 0, "medium": 1, "hard": 2, "impossible": 3}


@dataclass
class Dump:
    header: dict
    owners: np.ndarray  # (h, w) uint16, TS smallIDs
    fallout: np.ndarray  # (h, w) bool

    @property
    def tick(self) -> int:
        return int(self.header["tick"])


@dataclass
class ImportResult:
    game: Game
    player_map: dict[int, int]  # TS smallID -> C player id
    unit_map: dict[int, int] = field(default_factory=dict)  # TS unit id -> C unit id
    warnings: list[str] = field(default_factory=list)
    ts_tick: int = 0


def load_dump(path) -> Dump:
    raw = Path(path).read_bytes()
    nl = raw.index(b"\n")
    header = json.loads(raw[:nl])
    return _decode(header, raw[nl + 1:])


def dumps(header: dict, owners: np.ndarray, fallout: np.ndarray) -> bytes:
    """Serializes a dump (used by tests and tools that fabricate states)."""
    h = dict(header)
    own = np.ascontiguousarray(owners, dtype="<u2")
    fal = np.ascontiguousarray(fallout, dtype="u1")
    h["format"], h["version"] = MAGIC, 1
    h["width"], h["height"] = int(own.shape[1]), int(own.shape[0])
    h["blobs"] = {"owners": {"dtype": "<u2", "bytes": own.nbytes}, "fallout": {"dtype": "u1", "bytes": fal.nbytes}}
    return (json.dumps(h) + "\n").encode() + own.tobytes() + fal.tobytes()


def _decode(header: dict, blob: bytes) -> Dump:
    if header.get("format") != MAGIC:
        raise ValueError("not an ofe-state-dump file")
    w, h = header["width"], header["height"]
    n = w * h
    ob = header["blobs"]["owners"]["bytes"]
    if len(blob) < ob + n:
        raise ValueError("truncated dump")
    owners = np.frombuffer(blob, dtype="<u2", count=n).reshape(h, w).astype(np.uint16)
    fallout = np.frombuffer(blob, dtype="u1", count=n, offset=ob).reshape(h, w).astype(bool)
    return Dump(header, owners, fallout)


def _norm_unit(name: str) -> str:
    return TS_UNIT_NAMES.get(name, name)


def _assign_ids(ts_players: list[dict], game: Game, warnings: list[str]) -> dict[int, int]:
    """TS smallID -> C id: humans, nations, tribes in order; nations by name first."""
    c = game.players()
    c_by_type = {t: [int(r["id"]) for r in c if int(r["type"]) == t] for t in (0, 1, 2)}
    tname = {"human": 0, "nation": 1, "bot": 2}
    mapping: dict[int, int] = {}
    for tname_, ctype in tname.items():
        ts = sorted((p for p in ts_players if p["type"] == tname_), key=lambda p: p["small_id"])
        slots = list(c_by_type[ctype])
        if len(ts) > len(slots):
            raise ValueError(f"dump has {len(ts)} {tname_} players, C game only {len(slots)}")
        if ctype == 1:  # match nations by name where possible
            names = {game.player_name(s): s for s in slots}
            rest = []
            for p in ts:
                s = names.get(p["name"])
                if s is not None and s in slots:
                    mapping[p["small_id"]] = s
                    slots.remove(s)
                else:
                    rest.append(p)
            ts = rest
            if rest:
                warnings.append(f"{len(rest)} nation(s) matched by order, not name")
        for p, s in zip(ts, slots):
            mapping[p["small_id"]] = s
    return mapping


def _match_teams(ts_players: list[dict], game: Game, mapping: dict[int, int], warnings: list[str]) -> None:
    """Permutes the mapping within each player type so TS teams = C teams."""
    c = game.players()
    cteam = {int(r["id"]): int(r["team"]) for r in c}
    for tname, ctype in (("human", 0), ("nation", 1)):
        ts = [p for p in ts_players if p["type"] == tname]
        groups: dict = {}
        for p in ts:
            groups.setdefault(p["team"], []).append(p)
        slots = [mapping[p["small_id"]] for p in ts]
        cgroups: dict = {}
        for s in slots:
            cgroups.setdefault(cteam[s], []).append(s)
        tg = sorted(groups.values(), key=len, reverse=True)
        cg = sorted(cgroups.values(), key=len, reverse=True)
        if [len(g) for g in tg] != [len(g) for g in cg]:
            warnings.append(f"team sizes differ between TS and C for {tname}s; teams not preserved")
            continue
        for g, cs in zip(tg, cg):
            for p, s in zip(g, cs):
                mapping[p["small_id"]] = s


def import_state(dump: Dump, map: Map | None = None, maps_dir=None, seed: int = 0, **config_overrides) -> ImportResult:
    """Creates a C game equivalent to the dumped TS state."""
    hd = dump.header
    warnings: list[str] = []
    if map is None:
        if maps_dir is None:
            raise ValueError("pass map= or maps_dir=")
        map = Map.load(str(Path(maps_dir) / hd["map"].lower().replace(" ", "")), compact=(hd["map_size"].lower() == "compact"))
    if (map.width, map.height) != (hd["width"], hd["height"]):
        raise ValueError(f"map size {map.width}x{map.height} != dump {hd['width']}x{hd['height']}")

    players = hd["players"]
    n_h = sum(p["type"] == "human" for p in players)
    n_n = sum(p["type"] == "nation" for p in players)
    n_b = sum(p["type"] == "bot" for p in players)
    cfg_in = hd.get("config", {})
    team_game = str(hd.get("game_mode", "")).lower().startswith("team")
    teams = sorted({p["team"] for p in players if p["type"] != "bot" and p["team"] is not None})
    kw = dict(
        num_humans=n_h, num_nations=n_n, num_bots=n_b,
        difficulty=DIFFICULTY.get(str(hd.get("difficulty", "medium")).lower(), 1),
        game_mode=1 if team_game else 0, num_teams=max(2, len(teams)),
        infinite_gold=int(cfg_in.get("infinite_gold", False)), infinite_troops=int(cfg_in.get("infinite_troops", False)),
        instant_build=int(cfg_in.get("instant_build", False)), donate_gold=int(cfg_in.get("donate_gold", False)),
        donate_troops=int(cfg_in.get("donate_troops", False)),
        disable_alliances=int(cfg_in.get("disable_alliances", False)),
        spawn_immunity_ticks=int(hd.get("spawn_immunity_remaining", 0)),
        spawn_phase_ticks=-1 if hd.get("in_spawn_phase") else 0,
    )
    kw.update(config_overrides)
    game = Game(map, make_config(**kw), seed=seed)

    mapping = _assign_ids(players, game, warnings)
    if team_game:
        _match_teams(players, game, mapping, warnings)

    # tiles: TS smallID -> C id through a lookup table
    lut = np.zeros(int(max([0, dump.owners.max()] + [p["small_id"] for p in players])) + 1, dtype=np.uint16)
    for ts_id, c_id in mapping.items():
        lut[ts_id] = c_id
    game.set_owner_map(lut[dump.owners])

    flat_fallout = np.flatnonzero(dump.fallout.ravel())
    owners_flat = lut[dump.owners].ravel()
    for t in flat_fallout:
        if owners_flat[t] == 0:
            game.set_fallout(int(t), True)

    for p in players:
        c_id = mapping.get(p["small_id"])
        if c_id is None:
            continue
        game.set_troops(c_id, float(p["troops"]))
        game.set_gold(c_id, int(p["gold"]))
        if p.get("disconnected"):
            game.set_disconnected(c_id, True)

    unit_map: dict[int, int] = {}
    skipped: dict[str, int] = {}
    for u in hd.get("units", []):
        name = _norm_unit(u["type"])
        owner = mapping.get(u["owner"])
        if owner is None or name not in IMPORTABLE_UNITS:
            skipped[name] = skipped.get(name, 0) + 1
            continue
        if u.get("under_construction"):
            skipped["(under construction, imported finished)"] = skipped.get("(under construction, imported finished)", 0) + 1
        unit_map[u["id"]] = game.add_unit(name, owner, int(u["tile"]), max(1, int(u.get("level", 1))))
    if skipped:
        warnings.append("units not imported / approximated: " + ", ".join(f"{k} x{v}" for k, v in sorted(skipped.items())))

    for a in hd.get("alliances", []):
        x, y = mapping.get(a["a"]), mapping.get(a["b"])
        if x and y:
            game.set_alliance(x, y, True, max(1, int(a["expires"]) - dump.tick))
    for p in players:
        for target in p.get("embargoes", []):
            if mapping.get(p["small_id"]) and mapping.get(target):
                warnings.append("embargoes are not importable (no edit API); ignored")
                break
    if hd.get("attacks"):
        warnings.append(f"{len(hd['attacks'])} running attack(s) not imported")
    if re.search(r"[^0-9]", str(hd["tick"])):
        warnings.append("odd tick value")
    return ImportResult(game, mapping, unit_map, warnings, dump.tick)

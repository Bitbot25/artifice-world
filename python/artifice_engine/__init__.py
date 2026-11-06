"""Python API for the Artifice OpenFront engine.

    from artifice_engine import Map, Game, make_config

    world = Map.load("third_party/openfront/resources/maps/world")
    game = Game(world, make_config(num_humans=1, num_bots=100), seed=1)
    game.push(game.intent("spawn", player=1, tile=world.tile(500, 300)))
    game.step(300)                         # 30 s of game time
    owners = game.owner_map()              # (height, width) uint16
    players = game.players()               # structured array, PLAYER_DTYPE
    events = game.drain_events()           # structured array, EVENT_DTYPE

Batched stepping across many games runs in C threads without the GIL:

    games = [Game(world, cfg, seed=s) for s in range(64)]
    step_batch(games, ticks=10, num_threads=0)
"""

from __future__ import annotations

import ctypes as C
from typing import Iterable, Sequence

import numpy as np

from . import _lib as L
from ._lib import (  # noqa: F401  (re-exported)
    ALLIANCE_DTYPE,
    API_VERSION,
    ATTACK_DTYPE,
    DIFFICULTY_EASY,
    DIFFICULTY_HARD,
    DIFFICULTY_IMPOSSIBLE,
    DIFFICULTY_MEDIUM,
    EVENT,
    EVENT_DTYPE,
    EVENT_TYPES,
    EVENTS_ALL,
    EVENTS_NO_TILES,
    INTENT,
    INTENT_DTYPE,
    INTENT_TYPES,
    MODE_FFA,
    MODE_TEAM,
    PF_ALIVE,
    PF_DISCONNECTED,
    PF_IMMUNE,
    PF_IN_DOOMSDAY,
    PF_SPAWNED,
    PF_TRAITOR,
    PLAYER_BOT,
    PLAYER_DTYPE,
    PLAYER_HUMAN,
    PLAYER_NATION,
    RULES_VERSION,
    UNIT,
    UNIT_DTYPE,
    UNIT_TYPE_COUNT,
    UNIT_TYPES,
    Config,
)

__all__ = [
    "Map",
    "Game",
    "make_config",
    "step_batch",
    "intents_array",
]


def make_config(**overrides) -> Config:
    """OpenFront defaults (ofe_config_default) with keyword overrides.

    Unit names are accepted for `disabled_units`, e.g.
    make_config(disabled_units=["mirv", "hydrogen_bomb"]).
    """
    cfg = Config()
    L.ofe_config_default(C.byref(cfg))
    for key, value in overrides.items():
        if key == "disabled_units" and not isinstance(value, int):
            value = sum(1 << UNIT[name] for name in value)
        if key == "game_id" and isinstance(value, str):
            value = value.encode()
        if not hasattr(cfg, key):
            raise KeyError(f"unknown config field {key!r}")
        setattr(cfg, key, value)
    return cfg


class Map:
    """An immutable terrain map. Shared by every game created on it."""

    def __init__(self, handle: int):
        if not handle:
            raise ValueError("map could not be loaded")
        self._h = handle
        self.width = L.ofe_map_width(handle)
        self.height = L.ofe_map_height(handle)

    @classmethod
    def load(cls, map_dir: str, compact: bool = False) -> "Map":
        return cls(L.ofe_map_load(str(map_dir).encode(), int(compact)))

    @classmethod
    def from_terrain(cls, terrain: np.ndarray, nations: Sequence[tuple] = ()) -> "Map":
        """Map from an OpenFront terrain byte array of shape (height, width).

        nations: iterable of (name, x, y).
        """
        t = np.ascontiguousarray(terrain, dtype=np.uint8)
        h, w = t.shape
        m = cls(L.ofe_map_from_terrain(w, h, t.ctypes.data))
        for name, x, y in nations:
            L.ofe_map_add_nation(m._h, name.encode(), int(x), int(y))
        return m

    def __del__(self):
        if getattr(self, "_h", None):
            L.ofe_map_release(self._h)
            self._h = None

    @property
    def name(self) -> str:
        return L.ofe_map_name(self._h).decode()

    @property
    def num_land_tiles(self) -> int:
        return L.ofe_map_num_land_tiles(self._h)

    def terrain(self) -> np.ndarray:
        """Terrain bytes, shape (height, width). A copy."""
        ptr = L.ofe_map_terrain(self._h)
        return np.ctypeslib.as_array(ptr, shape=(self.height, self.width)).copy()

    def nations(self) -> list[tuple[str, int]]:
        out = []
        buf = C.create_string_buffer(64)
        for i in range(L.ofe_map_num_nations(self._h)):
            tile = L.ofe_map_nation(self._h, i, buf, 64)
            out.append((buf.value.decode(), tile))
        return out

    def tile(self, x: int, y: int) -> int:
        return int(y) * self.width + int(x)

    def xy(self, tile: int) -> tuple[int, int]:
        return int(tile) % self.width, int(tile) // self.width


def intents_array(intents: Iterable) -> np.ndarray:
    """Builds an INTENT_DTYPE array from dicts / tuples / an existing array."""
    if isinstance(intents, np.ndarray) and intents.dtype == INTENT_DTYPE:
        return np.ascontiguousarray(intents)
    rows = list(intents)
    arr = np.zeros(len(rows), dtype=INTENT_DTYPE)
    arr["troops"] = -1.0
    arr["amount"] = 1
    for i, r in enumerate(rows):
        if isinstance(r, np.void):
            arr[i] = r
            continue
        for k, v in r.items():
            if k == "type" and isinstance(v, str):
                v = INTENT[v]
            if k == "unit_type" and isinstance(v, str):
                v = UNIT[v]
            arr[i][k] = v
    return arr


class Game:
    """One simulation. Not thread-safe; use one Game per thread or step_batch."""

    def __init__(self, map: Map, config: Config | None = None, seed: int = 0, _handle: int | None = None):
        self.map = map
        if _handle is None:
            cfg = config if config is not None else make_config()
            _handle = L.ofe_game_create(map._h, C.byref(cfg), int(seed) & 0xFFFFFFFFFFFFFFFF)
            if not _handle:
                raise RuntimeError("game creation failed")
        self._h = _handle
        self._owner_buf = np.empty((map.height, map.width), dtype=np.uint16)

    def __del__(self):
        if getattr(self, "_h", None):
            L.ofe_game_destroy(self._h)
            self._h = None

    # ---- lifecycle -------------------------------------------------------
    def clone(self) -> "Game":
        """Independent copy; tiles are shared copy-on-write."""
        return Game(self.map, _handle=L.ofe_game_clone(self._h))

    def restore(self, other: "Game") -> None:
        """Make this game an exact copy of `other` (reuses buffers)."""
        L.ofe_game_copy_into(self._h, other._h)

    def step(self, ticks: int = 1) -> int:
        return L.ofe_game_step(self._h, int(ticks))

    # ---- intents ---------------------------------------------------------
    @staticmethod
    def intent(type: str, player: int, **fields) -> dict:
        d = {"type": type, "player": player}
        d.update(fields)
        return d

    def push(self, *intents) -> int:
        """Queue intents (dicts, or one INTENT_DTYPE array) for the next tick."""
        if len(intents) == 1 and isinstance(intents[0], (np.ndarray, list, tuple)) and not isinstance(intents[0], dict):
            arr = intents_array(intents[0])
        else:
            arr = intents_array(intents)
        return L.ofe_game_push_intents(self._h, arr.ctypes.data, len(arr))

    # ---- scalar state ----------------------------------------------------
    @property
    def tick(self) -> int:
        return L.ofe_game_tick(self._h)

    @property
    def in_spawn_phase(self) -> bool:
        return bool(L.ofe_game_in_spawn_phase(self._h))

    @property
    def is_over(self) -> bool:
        return bool(L.ofe_game_is_over(self._h))

    @property
    def winner(self) -> int:
        return L.ofe_game_winner(self._h)

    @property
    def winning_team(self) -> int:
        return L.ofe_game_winning_team(self._h)

    @property
    def num_players(self) -> int:
        return L.ofe_game_num_players(self._h)

    @property
    def num_land_tiles(self) -> int:
        return L.ofe_game_num_land_tiles(self._h)

    @property
    def num_fallout_tiles(self) -> int:
        return L.ofe_game_num_fallout_tiles(self._h)

    @property
    def win_threshold_tenths(self) -> int:
        return L.ofe_game_win_threshold_tenths(self._h)

    def hash(self) -> int:
        return L.ofe_game_hash(self._h)

    # ---- arrays ----------------------------------------------------------
    def owner_map(self, out: np.ndarray | None = None) -> np.ndarray:
        """Owner id per tile, shape (height, width), uint16."""
        if out is None:
            out = np.empty((self.map.height, self.map.width), dtype=np.uint16)
        L.ofe_game_owner_map(self._h, out.ctypes.data)
        return out

    def tile_state(self, out: np.ndarray | None = None) -> np.ndarray:
        """Raw tile state (bits 0-11 owner, bit 13 fallout)."""
        if out is None:
            out = np.empty((self.map.height, self.map.width), dtype=np.uint16)
        L.ofe_game_tile_state(self._h, out.ctypes.data)
        return out

    def players(self) -> np.ndarray:
        n = self.num_players
        out = np.zeros(n, dtype=PLAYER_DTYPE)
        L.ofe_game_players(self._h, out.ctypes.data, n)
        return out

    def player(self, pid: int) -> np.void:
        out = np.zeros(1, dtype=PLAYER_DTYPE)
        if L.ofe_game_player_info(self._h, int(pid), out.ctypes.data) < 0:
            raise IndexError(pid)
        return out[0]

    def player_name(self, pid: int) -> str:
        buf = C.create_string_buffer(64)
        L.ofe_game_player_name(self._h, int(pid), buf, 64)
        return buf.value.decode(errors="replace")

    def units(self) -> np.ndarray:
        n = L.ofe_game_num_units(self._h)
        out = np.zeros(n, dtype=UNIT_DTYPE)
        got = L.ofe_game_units(self._h, out.ctypes.data, n)
        return out[:got]

    def attacks(self) -> np.ndarray:
        cap = 64
        while True:
            out = np.zeros(cap, dtype=ATTACK_DTYPE)
            n = L.ofe_game_attacks(self._h, out.ctypes.data, cap)
            if n < cap:
                return out[:n]
            cap *= 4

    def alliances(self) -> np.ndarray:
        cap = 64
        while True:
            out = np.zeros(cap, dtype=ALLIANCE_DTYPE)
            n = L.ofe_game_alliances(self._h, out.ctypes.data, cap)
            if n < cap:
                return out[:n]
            cap *= 4

    def relation(self, a: int, b: int) -> float:
        return L.ofe_game_relation(self._h, int(a), int(b))

    def is_embargoed(self, a: int, b: int) -> bool:
        return bool(L.ofe_game_is_embargoed(self._h, int(a), int(b)))

    def unit_cost(self, player: int, unit_type) -> int:
        ut = UNIT[unit_type] if isinstance(unit_type, str) else int(unit_type)
        return L.ofe_game_unit_cost(self._h, int(player), ut)

    def can_build(self, player: int, unit_type, tile: int) -> bool:
        ut = UNIT[unit_type] if isinstance(unit_type, str) else int(unit_type)
        return bool(L.ofe_game_can_build(self._h, int(player), ut, int(tile)))

    # ---- events ----------------------------------------------------------
    def events(self) -> np.ndarray:
        """Copy of the event log (not cleared)."""
        ptr = C.c_void_p()
        n = L.ofe_game_events(self._h, C.byref(ptr))
        if n == 0:
            return np.zeros(0, dtype=EVENT_DTYPE)
        buf = (C.c_char * (n * EVENT_DTYPE.itemsize)).from_address(ptr.value)
        return np.frombuffer(buf, dtype=EVENT_DTYPE, count=n).copy()

    def drain_events(self) -> np.ndarray:
        """Event log since the last drain, then clears it."""
        ev = self.events()
        L.ofe_game_clear_events(self._h)
        return ev

    # ---- edits -----------------------------------------------------------
    def set_owner(self, tile: int, owner: int) -> None:
        _check(L.ofe_edit_set_owner(self._h, int(tile), int(owner)), "set_owner")

    def set_owner_map(self, owners: np.ndarray) -> None:
        arr = np.ascontiguousarray(owners, dtype=np.uint16)
        _check(L.ofe_edit_set_owner_map(self._h, arr.ctypes.data), "set_owner_map")

    def set_fallout(self, tile: int, fallout: bool = True) -> None:
        _check(L.ofe_edit_set_fallout(self._h, int(tile), int(fallout)), "set_fallout")

    def set_troops(self, player: int, troops: float) -> None:
        _check(L.ofe_edit_set_troops(self._h, int(player), float(troops)), "set_troops")

    def set_gold(self, player: int, gold: int) -> None:
        _check(L.ofe_edit_set_gold(self._h, int(player), int(gold)), "set_gold")

    def add_unit(self, unit_type, owner: int, tile: int, level: int = 1) -> int:
        ut = UNIT[unit_type] if isinstance(unit_type, str) else int(unit_type)
        uid = L.ofe_edit_add_unit(self._h, ut, int(owner), int(tile), int(level))
        _check(uid, "add_unit")
        return uid

    def remove_unit(self, unit_id: int) -> None:
        _check(L.ofe_edit_remove_unit(self._h, int(unit_id)), "remove_unit")

    def set_alliance(self, a: int, b: int, allied: bool = True, duration_ticks: int = 0) -> None:
        _check(L.ofe_edit_set_alliance(self._h, int(a), int(b), int(allied), int(duration_ticks)), "set_alliance")

    def launch_nuke(self, unit_type, owner: int, src_tile: int, dst_tile: int) -> int:
        ut = UNIT[unit_type] if isinstance(unit_type, str) else int(unit_type)
        uid = L.ofe_edit_launch_nuke(self._h, ut, int(owner), int(src_tile), int(dst_tile))
        _check(uid, "launch_nuke")
        return uid

    def set_relation(self, nation: int, other: int, value: float) -> None:
        _check(L.ofe_edit_set_relation(self._h, int(nation), int(other), float(value)), "set_relation")

    def set_disconnected(self, player: int, disconnected: bool = True) -> None:
        _check(L.ofe_edit_set_disconnected(self._h, int(player), int(disconnected)), "set_disconnected")

    def end_spawn_phase(self) -> None:
        L.ofe_edit_end_spawn_phase(self._h)


def _check(code: int, what: str) -> None:
    if code < 0:
        raise ValueError(f"{what} failed (code {code})")


def step_batch(games: Sequence[Game], ticks: int, num_threads: int = 0) -> None:
    """Steps every game `ticks` ticks in parallel C threads (GIL released)."""
    arr = (C.c_void_p * len(games))(*[g._h for g in games])
    L.ofe_step_batch(arr, len(games), int(ticks), int(num_threads))

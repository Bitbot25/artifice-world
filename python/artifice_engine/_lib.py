"""ctypes bindings for libofe (engine/include/ofe.h).

Every struct here mirrors one in ofe.h field for field; keep them in sync.
numpy dtypes with the same layout are built from the ctypes structs so
arrays of records can be passed to and from C without copying.
"""

from __future__ import annotations

import ctypes as C
import os
from pathlib import Path

import numpy as np

# ---------------------------------------------------------------------------
# Library loading
# ---------------------------------------------------------------------------


def _find_library() -> str:
    """OFE_LIB if set; else the TS-parity build if OFE_PARITY=1 (`make parity`),
    else the default build."""
    env = os.environ.get("OFE_LIB")
    if env:
        return env
    here = Path(__file__).resolve().parent
    parity = os.environ.get("OFE_PARITY") == "1"
    build = "build-parity" if parity else "build"
    installed = here / ("libofe_parity.so" if parity else "libofe.so")
    for cand in (installed, here.parents[1] / build / "libofe.so"):
        if cand.exists():
            return str(cand)
    raise OSError(
        f"libofe.so not found in {build}/: run `make` (or `make parity`) in the repository root or set OFE_LIB"
    )


lib = C.CDLL(_find_library())

# ---------------------------------------------------------------------------
# Constants (ofe.h enums)
# ---------------------------------------------------------------------------

API_VERSION = 1
RULES_VERSION = 1

PLAYER_HUMAN, PLAYER_NATION, PLAYER_BOT = 0, 1, 2
DIFFICULTY_EASY, DIFFICULTY_MEDIUM, DIFFICULTY_HARD, DIFFICULTY_IMPOSSIBLE = range(4)
MODE_FFA, MODE_TEAM = 0, 1

UNIT_TYPES = [
    "transport_ship",
    "warship",
    "shell",
    "sam_missile",
    "port",
    "atom_bomb",
    "hydrogen_bomb",
    "trade_ship",
    "missile_silo",
    "defense_post",
    "sam_launcher",
    "city",
    "mirv",
    "mirv_warhead",
    "train",
    "factory",
]
UNIT = {name: i for i, name in enumerate(UNIT_TYPES)}
UNIT_TYPE_COUNT = len(UNIT_TYPES)

INTENT_TYPES = [
    "none",
    "spawn",
    "attack",
    "cancel_attack",
    "boat_attack",
    "cancel_boat",
    "alliance_request",
    "alliance_reject",
    "alliance_extension",
    "break_alliance",
    "target_player",
    "donate_gold",
    "donate_troops",
    "build_unit",
    "embargo",
    "embargo_all",
    "move_warship",
    "mark_disconnected",
    "upgrade_structure",
    "delete_unit",
]
INTENT = {name: i for i, name in enumerate(INTENT_TYPES)}

EVENT_TYPES = [
    "tile_owner",
    "unit_created",
    "unit_destroyed",
    "unit_captured",
    "attack_started",
    "attack_ended",
    "alliance_request",
    "alliance_formed",
    "alliance_broken",
    "alliance_expired",
    "alliance_rejected",
    "player_spawned",
    "player_died",
    "player_conquered",
    "player_disconnected",
    "nuke_launched",
    "nuke_detonated",
    "embargo",
    "donation",
    "target",
    "spawn_phase_end",
    "game_over",
    "doomsday",
]
EVENT = {name: i for i, name in enumerate(EVENT_TYPES)}
EVENTS_ALL = 0xFFFFFFFF
EVENTS_NO_TILES = EVENTS_ALL & ~(1 << EVENT["tile_owner"])

PF_ALIVE, PF_SPAWNED, PF_DISCONNECTED, PF_TRAITOR, PF_IN_DOOMSDAY, PF_IMMUNE = (
    1 << i for i in range(6)
)
UF_UNDER_CONSTRUCTION, UF_RETREATING, UF_MARKED_FOR_DELETION, UF_COOLDOWN = (
    1 << i for i in range(4)
)

# ---------------------------------------------------------------------------
# Structs
# ---------------------------------------------------------------------------

i32, u32, i64, f64 = C.c_int32, C.c_uint32, C.c_int64, C.c_double


class Config(C.Structure):
    _fields_ = [
        ("difficulty", i32),
        ("game_mode", i32),
        ("num_teams", i32),
        ("num_humans", i32),
        ("num_nations", i32),
        ("num_bots", i32),
        ("spawn_phase_ticks", i32),
        ("random_spawn", i32),
        ("auto_spawn_humans", i32),
        ("instant_build", i32),
        ("infinite_gold", i32),
        ("infinite_troops", i32),
        ("donate_gold", i32),
        ("donate_troops", i32),
        ("disable_alliances", i32),
        ("alliance_duration_minutes", i32),
        ("disabled_units", u32),
        ("win_threshold_tenths", i32),
        ("overtime_enabled", i32),
        ("overtime_start_minutes", i32),
        ("doomsday_enabled", i32),
        ("doomsday_speed", i32),
        ("max_timer_minutes", i32),
        ("spawn_immunity_ticks", i32),
        ("starting_gold", i64),
        ("gold_multiplier", f64),
        ("event_mask", u32),
        ("humans_use_ai", i32),
        ("game_id", C.c_char * 24),
        ("reserved", i32 * 2),
    ]


class Intent(C.Structure):
    _fields_ = [
        ("type", i32),
        ("player", i32),
        ("target", i32),
        ("tile", i32),
        ("unit_type", i32),
        ("amount", i32),
        ("flags", i32),
        ("reserved", i32),
        ("troops", f64),
    ]


class PlayerInfo(C.Structure):
    _fields_ = [
        ("id", i32),
        ("type", i32),
        ("team", i32),
        ("flags", u32),
        ("num_tiles", i32),
        ("num_border_tiles", i32),
        ("spawn_tile", i32),
        ("num_outgoing_attacks", i32),
        ("troops", i64),
        ("gold", i64),
        ("max_troops", f64),
        ("troop_rate", f64),
        ("outgoing_attack_troops", f64),
        ("boat_troops", f64),
        ("units", i32 * UNIT_TYPE_COUNT),
        ("unit_levels", i32 * UNIT_TYPE_COUNT),
        ("traitor_ticks_left", i32),
        ("doomsday_ticks", i32),
        ("reserved", i32 * 4),
    ]


class UnitInfo(C.Structure):
    _fields_ = [
        ("id", i32),
        ("type", i32),
        ("owner", i32),
        ("tile", i32),
        ("target_tile", i32),
        ("level", i32),
        ("flags", u32),
        ("health", i32),
        ("troops", f64),
        ("ticks_left", i32),
        ("reserved", i32),
    ]


class AttackInfo(C.Structure):
    _fields_ = [
        ("id", i32),
        ("attacker", i32),
        ("target", i32),
        ("source_tile", i32),
        ("border_size", i32),
        ("retreating", i32),
        ("troops", f64),
    ]


class AllianceInfo(C.Structure):
    _fields_ = [("a", i32), ("b", i32), ("created_tick", i32), ("expires_tick", i32)]


class Event(C.Structure):
    _fields_ = [
        ("tick", i32),
        ("type", i32),
        ("a", i32),
        ("b", i32),
        ("c", i32),
        ("d", i32),
        ("v", f64),
    ]


def _dtype_of(struct) -> np.dtype:
    """numpy dtype with exactly the layout of a ctypes struct."""
    names, formats, offsets = [], [], []
    for name, ctype in struct._fields_:
        names.append(name)
        offsets.append(getattr(struct, name).offset)
        if issubclass(ctype, C.Array):
            formats.append((np.dtype(ctype._type_), (ctype._length_,)))
        else:
            formats.append(np.dtype(ctype))
    return np.dtype(
        {"names": names, "formats": formats, "offsets": offsets, "itemsize": C.sizeof(struct)}
    )


INTENT_DTYPE = _dtype_of(Intent)
PLAYER_DTYPE = _dtype_of(PlayerInfo)
UNIT_DTYPE = _dtype_of(UnitInfo)
ATTACK_DTYPE = _dtype_of(AttackInfo)
ALLIANCE_DTYPE = _dtype_of(AllianceInfo)
EVENT_DTYPE = _dtype_of(Event)

# ---------------------------------------------------------------------------
# Function prototypes
# ---------------------------------------------------------------------------

vp, cp = C.c_void_p, C.c_char_p
P = C.POINTER


def _fn(name, restype, *argtypes):
    f = getattr(lib, name)
    f.restype = restype
    f.argtypes = list(argtypes)
    return f


ofe_map_load = _fn("ofe_map_load", vp, cp, C.c_int)
ofe_map_from_terrain = _fn("ofe_map_from_terrain", vp, i32, i32, vp)
ofe_map_add_nation = _fn("ofe_map_add_nation", None, vp, cp, i32, i32)
ofe_map_retain = _fn("ofe_map_retain", None, vp)
ofe_map_release = _fn("ofe_map_release", None, vp)
ofe_map_width = _fn("ofe_map_width", i32, vp)
ofe_map_height = _fn("ofe_map_height", i32, vp)
ofe_map_num_land_tiles = _fn("ofe_map_num_land_tiles", i32, vp)
ofe_map_terrain = _fn("ofe_map_terrain", P(C.c_uint8), vp)
ofe_map_num_nations = _fn("ofe_map_num_nations", i32, vp)
ofe_map_nation = _fn("ofe_map_nation", i32, vp, i32, cp, i32)
ofe_map_name = _fn("ofe_map_name", cp, vp)

ofe_config_default = _fn("ofe_config_default", None, P(Config))
ofe_game_create = _fn("ofe_game_create", vp, vp, P(Config), C.c_uint64)
ofe_game_destroy = _fn("ofe_game_destroy", None, vp)
ofe_game_clone = _fn("ofe_game_clone", vp, vp)
ofe_game_copy_into = _fn("ofe_game_copy_into", None, vp, vp)
ofe_game_push_intents = _fn("ofe_game_push_intents", i32, vp, vp, i32)
ofe_game_step = _fn("ofe_game_step", i32, vp, i32)
ofe_step_batch = _fn("ofe_step_batch", None, P(vp), i32, i32, i32)

ofe_game_tick = _fn("ofe_game_tick", i32, vp)
ofe_game_in_spawn_phase = _fn("ofe_game_in_spawn_phase", i32, vp)
ofe_game_is_over = _fn("ofe_game_is_over", i32, vp)
ofe_game_winner = _fn("ofe_game_winner", i32, vp)
ofe_game_winning_team = _fn("ofe_game_winning_team", i32, vp)
ofe_game_num_players = _fn("ofe_game_num_players", i32, vp)
ofe_game_num_land_tiles = _fn("ofe_game_num_land_tiles", i32, vp)
ofe_game_num_fallout_tiles = _fn("ofe_game_num_fallout_tiles", i32, vp)
ofe_game_win_threshold_tenths = _fn("ofe_game_win_threshold_tenths", i32, vp)
ofe_game_hash = _fn("ofe_game_hash", C.c_uint64, vp)
ofe_game_player_info = _fn("ofe_game_player_info", i32, vp, i32, vp)
ofe_game_players = _fn("ofe_game_players", i32, vp, vp, i32)
ofe_game_owner_map = _fn("ofe_game_owner_map", None, vp, vp)
ofe_game_tile_state = _fn("ofe_game_tile_state", None, vp, vp)
ofe_game_tile_owner = _fn("ofe_game_tile_owner", i32, vp, i32)
ofe_game_units = _fn("ofe_game_units", i32, vp, vp, i32)
ofe_game_num_units = _fn("ofe_game_num_units", i32, vp)
ofe_game_attacks = _fn("ofe_game_attacks", i32, vp, vp, i32)
ofe_game_alliances = _fn("ofe_game_alliances", i32, vp, vp, i32)
ofe_game_relation = _fn("ofe_game_relation", f64, vp, i32, i32)
ofe_game_is_embargoed = _fn("ofe_game_is_embargoed", i32, vp, i32, i32)
ofe_game_player_name = _fn("ofe_game_player_name", i32, vp, i32, cp, i32)
ofe_game_unit_cost = _fn("ofe_game_unit_cost", i64, vp, i32, i32)
ofe_game_can_build = _fn("ofe_game_can_build", i32, vp, i32, i32, i32)

ofe_game_events = _fn("ofe_game_events", i32, vp, P(vp))
ofe_game_drain_events = _fn("ofe_game_drain_events", i32, vp, vp, i32)
ofe_game_clear_events = _fn("ofe_game_clear_events", None, vp)

ofe_edit_set_owner = _fn("ofe_edit_set_owner", i32, vp, i32, i32)
ofe_edit_set_owner_map = _fn("ofe_edit_set_owner_map", i32, vp, vp)
ofe_edit_set_fallout = _fn("ofe_edit_set_fallout", i32, vp, i32, i32)
ofe_edit_set_troops = _fn("ofe_edit_set_troops", i32, vp, i32, f64)
ofe_edit_set_gold = _fn("ofe_edit_set_gold", i32, vp, i32, i64)
ofe_edit_add_unit = _fn("ofe_edit_add_unit", i32, vp, i32, i32, i32, i32)
ofe_edit_remove_unit = _fn("ofe_edit_remove_unit", i32, vp, i32)
ofe_edit_set_alliance = _fn("ofe_edit_set_alliance", i32, vp, i32, i32, i32, i32)
ofe_edit_launch_nuke = _fn("ofe_edit_launch_nuke", i32, vp, i32, i32, i32, i32)
ofe_edit_set_relation = _fn("ofe_edit_set_relation", i32, vp, i32, i32, f64)
ofe_edit_set_disconnected = _fn("ofe_edit_set_disconnected", i32, vp, i32, i32)
ofe_edit_end_spawn_phase = _fn("ofe_edit_end_spawn_phase", i32, vp)

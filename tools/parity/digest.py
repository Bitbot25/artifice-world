"""Canonical per-tick digest of the observable game state.

The TS tracer (trace_ts.ts) computes exactly the same values; keep the two in
sync. A digest is a dict of component -> uint32:

  own   tile owners: sum over owned tiles of fmix32(t*0x9E3779B1 + owner*0x85EBCA77)
  fall  fallout tiles: the same with owner = 1
  pl    players  [[small_id, troops, gold, tiles], ...] by small id
  un    units    [[id, type, owner, tile, troops, health, level, constructing], ...] by id
  at    attacks  [[attacker, target, troops, retreating], ...] sorted
  al    alliances [[a, b, expires_tick], ...] with a < b, sorted

List components are hashed as crc32 of their compact JSON text. All values
are integers (troops and health are floored). Unit types use the shared
enum order (OfeUnitType == OpenFront's UnitType declaration order).
"""
from __future__ import annotations

import json
import zlib

import numpy as np

M32 = 0xFFFFFFFF
LIST_COMPONENTS = ("pl", "un", "at", "al")


def fmix32(h: np.ndarray) -> np.ndarray:
    h = h ^ (h >> np.uint32(16))
    h = h * np.uint32(0x85EBCA6B)
    h = h ^ (h >> np.uint32(13))
    h = h * np.uint32(0xC2B2AE35)
    return h ^ (h >> np.uint32(16))


class TileHasher:
    def __init__(self, num_tiles: int):
        with np.errstate(over="ignore"):
            self.tkey = np.arange(num_tiles, dtype=np.uint32) * np.uint32(0x9E3779B1)

    def owners(self, owners: np.ndarray) -> int:
        o = owners.ravel().astype(np.uint32)
        idx = np.nonzero(o)[0]
        with np.errstate(over="ignore"):
            k = self.tkey[idx] + o[idx] * np.uint32(0x85EBCA77)
            return int(fmix32(k).sum(dtype=np.uint64)) & M32

    def mask(self, mask: np.ndarray) -> int:
        idx = np.nonzero(mask.ravel())[0]
        with np.errstate(over="ignore"):
            k = self.tkey[idx] + np.uint32(0x85EBCA77)
            return int(fmix32(k).sum(dtype=np.uint64)) & M32


def list_hash(rows: list) -> int:
    return zlib.crc32(json.dumps(rows, separators=(",", ":")).encode()) & M32


def digest(lists: dict, own: int, fall: int) -> dict:
    d = {"own": own, "fall": fall}
    for k in LIST_COMPONENTS:
        d[k] = list_hash(lists[k])
    return d

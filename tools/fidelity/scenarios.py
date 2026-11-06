"""Scenario generators for the fidelity comparison.

A scenario is a plain dict (see README.md); the same dict drives ts_runner.ts
and c_runner.py, so both engines start from the same state: same map, same
players, same tiles / troops / gold (set explicitly after the spawn phase) and
the same scripted intents. Maps are located through common.maps_dir().
"""

from __future__ import annotations

import numpy as np

import common

MAP = "world"
MAP_SIZE = "compact"  # 1000x500, the 4x world map
J = 6  # spatial jitter (tiles): per-seed translation of the whole setup
ATTACK_T = 30  # humans are immune for the first 50 ticks after the spawn phase; setup waits 25 of them


class Arenas:
    """Finds land rectangles dominated by one terrain class."""

    def __init__(self, terrain: np.ndarray):
        self.h, self.w = terrain.shape
        self.cls = common.terrain_class(terrain)
        self.land = common.passable_land(terrain)
        self._ii = {}
        for name, mask in (("land", self.land), ("plains", self.cls == 1), ("highland", self.cls == 2),
                           ("mountain", self.cls == 3)):
            ii = np.zeros((self.h + 1, self.w + 1), dtype=np.int64)
            ii[1:, 1:] = mask.cumsum(0).cumsum(1)
            self._ii[name] = ii
        self.shore = (terrain & 0x40) != 0
        self.ocean = self.cls == 0  # any water

    def _count(self, name: str, w: int, h: int) -> np.ndarray:
        ii = self._ii[name]
        return ii[h:, w:] - ii[:-h, w:] - ii[h:, :-w] + ii[:-h, :-w]

    def find(self, kind: str, w: int, h: int, avoid: list | None = None):
        """Top-left (x, y) of the w*h rectangle that is all passable land and
        has the highest share of `kind` terrain. Returns (x, y, purity)."""
        land = self._count("land", w, h)
        pur = self._count(kind, w, h).astype(float) / (w * h)
        pur[land != w * h] = -1
        for (ax, ay, aw, ah) in avoid or []:
            y0, y1 = max(0, ay - h), min(pur.shape[0], ay + ah)
            x0, x1 = max(0, ax - w), min(pur.shape[1], ax + aw)
            pur[y0:y1, x0:x1] = -1
        idx = int(np.argmax(pur))
        y, x = divmod(idx, pur.shape[1])
        if pur[y, x] < 0:
            raise RuntimeError(f"no {w}x{h} land arena found")
        return int(x), int(y), float(pur[y, x])


def _base(name, seeds, ticks, **kw):
    sc = {
        "name": name, "map": MAP, "map_size": MAP_SIZE, "difficulty": "medium", "humans": 0,
        "nations": "disabled", "bots": 0, "seeds": list(seeds), "ticks": ticks, "sample_every": 10,
        "sample_players": "humans",
    }
    sc.update(kw)
    return sc


def jitter(seeds, j=J):
    rng = np.random.default_rng(12345)
    return [[int(rng.integers(-j, j + 1)), int(rng.integers(-j, j + 1))] for _ in seeds]


def rect(player, x0, y0, x1, y1):
    return {"op": "claim", "player": player, "shape": "rect", "x0": x0, "y0": y0, "x1": x1, "y1": y1}


# ---------------------------------------------------------------------------


def idle_human(arenas: Arenas, seeds):
    x, y, _ = arenas.find("plains", 30 + 2 * J, 30 + 2 * J)
    return _base("idle_human", seeds, 6000, humans=1, spawns={"1": [x + J + 15, y + J + 15]}, sample_every=100,
                 jitter=jitter(seeds))


def expand_human(arenas: Arenas, seeds):
    x, y, _ = arenas.find("plains", 100 + 2 * J, 80 + 2 * J)
    return _base("expand_human", seeds, 4000, humans=1, spawns={"1": [x + J + 50, y + J + 40]}, sample_every=100,
                 jitter=jitter(seeds),
                 intents=[{"tick": 0, "player": 1, "type": "attack", "target": 0, "every": 100, "until": 3900}])


def tn_cells(arenas: Arenas, seeds, terrains=("plains", "highland", "mountain")):
    """Attack on terra nullius: blob of 40x40 attacks the land around it."""
    cells = []
    for terr in terrains:
        ax, ay, pur = arenas.find(terr, 100 + 2 * J, 80 + 2 * J)
        ax, ay = ax + J, ay + J
        for x_troops in (1000, 5000, 20000):
            sc = _base(f"tn_{terr}_x{x_troops}", seeds, 400, humans=1, spawns={"1": [ax + 50, ay + 40]},
                       sample_every=10, jitter=jitter(seeds))
            sc["setup"] = [{"op": "clear", "player": "all"}, rect(1, ax + 30, ay + 20, ax + 70, ay + 60),
                           {"op": "troops", "player": 1, "value": 40000}]
            sc["intents"] = [{"tick": 0, "player": 1, "type": "attack", "target": 0, "troops": x_troops}]
            sc["meta"] = {"terrain": terr, "purity": pur, "attack_troops": x_troops, "att_tiles": 1600}
            cells.append(sc)
    return cells


def pvp_cells(arenas: Arenas, seeds, terrains=("plains", "highland", "mountain")):
    """Land attack on a player: 40x60 attacker block next to a defender block."""
    cells = []
    for terr in terrains:
        ax, ay, pur = arenas.find(terr, 100 + 2 * J, 70 + 2 * J)
        ax, ay = ax + J, ay + J
        for dw in (20, 40):
            for d_troops in (4000, 20000):
                for x_troops in (3000, 10000, 30000):
                    name = f"pvp_{terr}_d{dw * 60}_D{d_troops}_x{x_troops}"
                    sc = _base(name, seeds, 700, humans=2,
                               spawns={"1": [ax + 10, ay + 10], "2": [ax + 90, ay + 60]}, sample_every=10,
                               jitter=jitter(seeds))
                    x0 = ax + 20
                    sc["setup"] = [
                        {"op": "clear", "player": "all"},
                        rect(1, x0, ay + 5, x0 + 40, ay + 65),
                        rect(2, x0 + 40, ay + 5, x0 + 40 + dw, ay + 65),
                        {"op": "troops", "player": 1, "value": 40000},
                        {"op": "troops", "player": 2, "value": d_troops},
                    ]
                    sc["intents"] = [{"tick": ATTACK_T, "player": 1, "type": "attack", "target": 2, "troops": x_troops}]
                    sc["meta"] = {"terrain": terr, "purity": pur, "def_tiles": dw * 60, "def_troops": d_troops,
                                  "attack_troops": x_troops, "att_tiles": 2400}
                    cells.append(sc)
    return cells


def conquer_cells(arenas: Arenas, seeds):
    """Attack that wipes out a small player (<100 tiles): conquest, gold and
    leftover tiles."""
    ax, ay, pur = arenas.find("plains", 100 + 2 * J, 70 + 2 * J)
    ax, ay = ax + J, ay + J
    cells = []
    for d_gold in (0, 50000):
        for x_troops in (2000, 10000):
            sc = _base(f"conquer_g{d_gold}_x{x_troops}", seeds, 300, humans=2,
                       spawns={"1": [ax + 10, ay + 10], "2": [ax + 90, ay + 60]}, sample_every=10,
                       jitter=jitter(seeds))
            x0 = ax + 20
            sc["setup"] = [
                {"op": "clear", "player": "all"},
                rect(1, x0, ay + 5, x0 + 40, ay + 65),
                rect(2, x0 + 40, ay + 25, x0 + 48, ay + 35),
                {"op": "troops", "player": 1, "value": 40000},
                {"op": "troops", "player": 2, "value": 1500},
                {"op": "gold", "player": 2, "value": d_gold},
            ]
            sc["intents"] = [{"tick": ATTACK_T, "player": 1, "type": "attack", "target": 2, "troops": x_troops}]
            sc["meta"] = {"def_gold": d_gold, "attack_troops": x_troops}
            cells.append(sc)
    return cells


def structure_cells(arenas: Arenas, seeds):
    """City (max-troops bonus) and defense post (attack resistance)."""
    ax, ay, pur = arenas.find("plains", 100 + 2 * J, 70 + 2 * J)
    ax, ay = ax + J, ay + J
    cells = []
    sc = _base("struct_city", seeds, 1500, humans=1, spawns={"1": [ax + 50, ay + 35]}, sample_every=50,
               jitter=jitter(seeds))
    sc["setup"] = [{"op": "clear", "player": "all"}, {"op": "claim", "player": 1, "shape": "disc", "cx": ax + 50,
                                                      "cy": ay + 35, "r": 25},
                   {"op": "troops", "player": 1, "value": 200000}, {"op": "gold", "player": 1, "value": 2000000}]
    sc["intents"] = [{"tick": 0, "player": 1, "type": "build", "unit": "City", "x": ax + 50, "y": ay + 35}]
    cells.append(sc)
    base = dict(sc)
    base = _base("struct_nocity", seeds, 1500, humans=1, spawns=sc["spawns"], sample_every=50, jitter=sc["jitter"])
    base["setup"] = sc["setup"]
    cells.append(base)
    for post in (False, True):
        name = "struct_post" if post else "struct_nopost"
        sc = _base(name, seeds, 500, humans=2, spawns={"1": [ax + 10, ay + 10], "2": [ax + 90, ay + 60]},
                   sample_every=10, jitter=jitter(seeds))
        x0 = ax + 20
        sc["setup"] = [{"op": "clear", "player": "all"}, rect(1, x0, ay + 5, x0 + 40, ay + 65),
                       rect(2, x0 + 40, ay + 5, x0 + 80, ay + 65),
                       {"op": "troops", "player": 1, "value": 40000}, {"op": "troops", "player": 2, "value": 10000},
                       {"op": "gold", "player": 2, "value": 1000000}]
        sc["intents"] = ([{"tick": 0, "player": 2, "type": "build", "unit": "DefensePost", "x": x0 + 48, "y": ay + 35}]
                         if post else []) + [{"tick": 150, "player": 1, "type": "attack", "target": 2, "troops": 15000}]
        sc["meta"] = {"post": post}
        cells.append(sc)
    return cells


def enclave_cells(arenas: Arenas, seeds):
    """A small player completely surrounded by a bigger one."""
    ax, ay, pur = arenas.find("plains", 80 + 2 * J, 80 + 2 * J)
    ax, ay = ax + J, ay + J
    cx, cy = ax + 40, ay + 40
    cells = []
    for r2 in (2, 4, 7, 12):
        sc = _base(f"enclave_r{r2}", seeds, 400, humans=2, spawns={"1": [ax + 5, ay + 5], "2": [ax + 75, ay + 75]},
                   sample_every=2, jitter=jitter(seeds))
        sc["setup"] = [
            {"op": "clear", "player": "all"},
            {"op": "claim", "player": 1, "shape": "disc", "cx": cx, "cy": cy, "r": 36},
            {"op": "claim", "player": 2, "shape": "disc", "cx": cx, "cy": cy, "r": r2},
            {"op": "troops", "player": 1, "value": 30000},
            {"op": "troops", "player": 2, "value": 3000},
        ]
        sc["meta"] = {"r2": r2}
        cells.append(sc)
    return cells


def spawn_scenario(arenas: Arenas, seeds, n=12):
    rng = np.random.default_rng(7)
    spawns, pts = {}, []
    ys, xs = np.nonzero(arenas.land)
    tries = 0
    while len(pts) < n and tries < 100000:
        tries += 1
        i = int(rng.integers(len(xs)))
        x, y = int(xs[i]), int(ys[i])
        if all(abs(x - px) + abs(y - py) > 40 for px, py in pts):
            pts.append((x, y))
    for k, (x, y) in enumerate(pts):
        spawns[str(k + 1)] = [x, y]
    sc = _base("spawn_humans", seeds, 100, humans=len(pts), spawns=spawns, sample_every=50)
    return sc


def ai_mix(arenas: Arenas, seeds, ticks=6000, bots=60, nations="default"):
    return _base("ai_mix", seeds, ticks, humans=0, bots=bots, nations=nations, sample_every=200,
                 sample_players="all", track_boats=False, track_attacks=False, chunk=2)


def _water_components(ocean: np.ndarray) -> np.ndarray:
    """4-connected labels of water tiles (0 = land)."""
    h, w = ocean.shape
    lab = np.zeros((h, w), dtype=np.int32)
    flat = ocean.ravel()
    labf = lab.ravel()
    nxt = 0
    for start in np.flatnonzero(flat):
        if labf[start]:
            continue
        nxt += 1
        labf[start] = nxt
        stack = [int(start)]
        while stack:
            c = stack.pop()
            x = c % w
            for n, ok in ((c - w, c >= w), (c + w, c < (h - 1) * w), (c - 1, x > 0), (c + 1, x < w - 1)):
                if ok and flat[n] and not labf[n]:
                    labf[n] = nxt
                    stack.append(n)
    return lab


def boat_cells(arenas: Arenas, seeds, bins=(25, 50, 100, 200, 350), per_bin=2):
    """Source: a coastal disc of radius 5. Destination: a coast on the same sea at a
    given straight-line distance. Boat travel time is measured in ticks."""
    lab = _water_components(arenas.ocean)
    h, w = arenas.h, arenas.w
    # candidate coastal land tiles: passable land with a 9x9 land blob around them and ocean within 3 tiles
    cands = []
    ys, xs = np.nonzero(arenas.shore & arenas.land)
    rng = np.random.default_rng(11)
    order = rng.permutation(len(xs))
    xs, ys = xs[order], ys[order]
    comp_of = {}

    def sea_of(x, y):
        for dy in range(-2, 3):
            for dx in range(-2, 3):
                xx, yy = x + dx, y + dy
                if 0 <= xx < w and 0 <= yy < h and lab[yy, xx]:
                    return int(lab[yy, xx])
        return 0

    sizes = np.bincount(lab.ravel())
    cells = []
    used = set()
    for tgt in bins:
        got = 0
        for i in range(min(len(xs), 60000)):
            x, y = int(xs[i]), int(ys[i])
            if not (4 <= x < w - 5 and 4 <= y < h - 5):
                continue
            s = sea_of(x, y)
            if s == 0 or sizes[s] < 5000:
                continue
            # destination search: ring at distance ~tgt
            ang = rng.uniform(0, 2 * np.pi, 40)
            for a in ang:
                dx, dy = int(round(x + tgt * np.cos(a))), int(round(y + tgt * np.sin(a)))
                if not (2 <= dx < w - 2 and 2 <= dy < h - 2):
                    continue
                if not (arenas.shore[dy, dx] and arenas.land[dy, dx]) or sea_of(dx, dy) != s:
                    continue
                if (x, y) in used or arenas.ocean[y, x]:
                    continue
                # keep destination off the source landmass: no land path is checked cheaply; require some
                # water strictly between (sampled along the segment)
                tt = np.linspace(0, 1, 40)
                px = (x + (dx - x) * tt).astype(int)
                py = (y + (dy - y) * tt).astype(int)
                if arenas.ocean[py, px].sum() < 5:
                    continue
                sc = _base(f"boat_d{tgt}_{got}", seeds, 700, humans=1, spawns={"1": [x, y]}, sample_every=50)
                sc["setup"] = [{"op": "clear", "player": "all"},
                               {"op": "claim", "player": 1, "shape": "disc", "cx": x, "cy": y, "r": 5}]
                sc["intents"] = [{"tick": 0, "player": 1, "type": "boat", "x": dx, "y": dy}]
                sc["meta"] = {"src": [x, y], "dst": [dx, dy], "dist": float(np.hypot(dx - x, dy - y))}
                cells.append(sc)
                used.add((x, y))
                got += 1
                break
            if got >= per_bin:
                break
    return cells

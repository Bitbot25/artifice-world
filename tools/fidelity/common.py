"""Shared helpers for the fidelity harness: paths, scenario geometry, running
the TypeScript runner, reading JSON-lines results."""

from __future__ import annotations

import json
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

import numpy as np

HERE = Path(__file__).resolve().parent
REPO = HERE.parents[1]
sys.path.insert(0, str(REPO / "python"))

IMPASSABLE_MAGNITUDE = 31
LAND_BIT = 0x80


def openfront_dir() -> Path | None:
    """The OpenFront checkout: $OPENFRONT_DIR, else <repo>/third_party/openfront."""
    cand = Path(os.environ.get("OPENFRONT_DIR") or REPO / "third_party" / "openfront")
    return cand if (cand / "package.json").exists() else None


def maps_dir() -> Path | None:
    env = os.environ.get("OFE_MAPS_DIR")
    if env and Path(env).is_dir():
        return Path(env)
    of = openfront_dir()
    if of and (of / "resources" / "maps").is_dir():
        return of / "resources" / "maps"
    return None


def ts_available() -> bool:
    of = openfront_dir()
    return bool(of and (of / "node_modules" / ".bin" / "tsx").exists() and shutil.which("node"))


# ---------------------------------------------------------------------------
# Geometry shared by both runners (must match ts_runner.ts)
# ---------------------------------------------------------------------------


def passable_land(terrain: np.ndarray) -> np.ndarray:
    """Boolean (h, w): land that is not impassable (what can be owned)."""
    return ((terrain & LAND_BIT) != 0) & ((terrain & 31) < IMPASSABLE_MAGNITUDE)


def shape_mask(op: dict, w: int, h: int) -> np.ndarray:
    ys, xs = np.mgrid[0:h, 0:w]
    if op["shape"] == "rect":
        return (xs >= op["x0"]) & (xs < op["x1"]) & (ys >= op["y0"]) & (ys < op["y1"])
    if op["shape"] == "disc":
        return (xs - op["cx"]) ** 2 + (ys - op["cy"]) ** 2 <= op["r"] ** 2
    raise ValueError(op["shape"])


def terrain_class(terrain: np.ndarray) -> np.ndarray:
    """0 water, 1 plains, 2 highland, 3 mountain, 4 impassable (OpenFront's
    terrainType: magnitude <10 / <20 / else)."""
    mag = terrain & 31
    land = (terrain & LAND_BIT) != 0
    cls = np.zeros(terrain.shape, dtype=np.uint8)
    cls[land & (mag < 10)] = 1
    cls[land & (mag >= 10) & (mag < 20)] = 2
    cls[land & (mag >= 20)] = 3
    cls[land & (mag >= IMPASSABLE_MAGNITUDE)] = 4
    return cls


# ---------------------------------------------------------------------------
# Running the TypeScript runner
# ---------------------------------------------------------------------------


def ts_run(scenario: dict, openfront: Path | None = None, timeout: float = 900.0) -> list[dict]:
    """Runs a scenario (all its seeds) on the OpenFront TS engine. Returns the
    JSON-lines records. Raises RuntimeError if TS is not installed."""
    of = openfront or openfront_dir()
    if not ts_available() and of is None:
        raise RuntimeError("OpenFront checkout not found (run tools/fetch_openfront.sh and `npm ci` there)")
    with tempfile.TemporaryDirectory() as td:
        sp = Path(td) / "scenario.json"
        op = Path(td) / "out.jsonl"
        sp.write_text(json.dumps(scenario))
        cmd = [
            str(of / "node_modules" / ".bin" / "tsx"),
            str(HERE / "ts_runner.ts"),
            "--ref", str(of),
            "--scenario", str(sp),
            "--out", str(op),
        ]
        r = subprocess.run(cmd, cwd=of, capture_output=True, text=True, timeout=timeout)
        if r.returncode != 0:
            raise RuntimeError(f"ts_runner failed ({r.returncode}):\n{r.stderr[-3000:]}")
        return read_jsonl(op)


def read_jsonl(path) -> list[dict]:
    out = []
    with open(path) as f:
        for line in f:
            line = line.strip()
            if line.startswith("{"):
                out.append(json.loads(line))
    return out


def by_seed(records: list[dict]) -> dict[int, list[dict]]:
    out: dict[int, list[dict]] = {}
    for r in records:
        out.setdefault(r["seed"], []).append(r)
    return out


def apply_jitter(sc: dict, seed_index: int) -> dict:
    """Shifts every coordinate of a scenario by its per-seed offset
    (sc["jitter"][i] = [dx, dy]). Gives the TS engine, which is deterministic
    for a given geometry, a spatial distribution to compare against the C
    engine's seed-to-seed variation. Mirrored in ts_runner.ts."""
    jit = sc.get("jitter")
    if not jit:
        return sc
    dx, dy = jit[seed_index % len(jit)]
    out = json.loads(json.dumps(sc))
    out["spawns"] = {k: [x + dx, y + dy] for k, (x, y) in (sc.get("spawns") or {}).items()}
    for op in out.get("setup", []):
        if op.get("shape") == "rect":
            op["x0"] += dx; op["x1"] += dx; op["y0"] += dy; op["y1"] += dy
        elif op.get("shape") == "disc":
            op["cx"] += dx; op["cy"] += dy
    for it in out.get("intents", []):
        if "x" in it:
            it["x"] += dx; it["y"] += dy
    return out

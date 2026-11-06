#!/usr/bin/env python3
"""Lockstep parity check: runs a scenario on the TS engine and on the C parity
build, finds the first tick whose observable state differs and explains it.

    make parity
    export OPENFRONT_DIR=<openfront checkout> OFE_MAPS_DIR=$OPENFRONT_DIR/resources/maps
    python3 tools/parity/diff.py tools/parity/scenarios/core_humans.json [--ticks N] [--refresh]

Exit status 0 when every tick matches. TS traces are cached under
tools/parity/out/ keyed by the scenario contents (and OpenFront commit).
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parents[1]
OUT = HERE / "out"


def openfront_dir() -> Path:
    d = os.environ.get("OPENFRONT_DIR")
    return Path(d) if d else REPO / "third_party" / "openfront"


def of_commit(of: Path) -> str:
    try:
        return subprocess.run(["git", "rev-parse", "--short", "HEAD"], cwd=of, capture_output=True,
                              text=True).stdout.strip()
    except OSError:
        return "unknown"


def read(path: Path) -> list[dict]:
    return [json.loads(x) for x in path.read_text().splitlines() if x.startswith("{")]


def run_ts(sc: dict, scen_path: Path, out: Path, detail: list[int]):
    of = openfront_dir()
    cmd = [str(of / "node_modules" / ".bin" / "tsx"), str(HERE / "trace_ts.ts"), "--ref", str(of),
           "--scenario", str(scen_path), "--out", str(out)]
    if detail:
        cmd += ["--detail", ",".join(map(str, detail))]
    r = subprocess.run(cmd, cwd=of, capture_output=True, text=True)
    if r.returncode != 0:
        sys.exit(f"trace_ts failed:\n{r.stderr[-4000:]}")


def run_c(scen_path: Path, out: Path, detail: list[int]):
    cmd = [sys.executable, str(HERE / "trace_c.py"), str(scen_path), "--out", str(out)]
    if detail:
        cmd += ["--detail", ",".join(map(str, detail))]
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode != 0:
        sys.exit(f"trace_c failed:\n{r.stderr[-4000:]}")


def show_rows(name: str, ts_rows: list, c_rows: list, limit: int = 12):
    ts_set = {json.dumps(r) for r in ts_rows}
    c_set = {json.dumps(r) for r in c_rows}
    only_ts = [r for r in ts_rows if json.dumps(r) not in c_set]
    only_c = [r for r in c_rows if json.dumps(r) not in ts_set]
    print(f"  {name}: {len(ts_rows)} rows in TS, {len(c_rows)} in C; {len(only_ts)} only in TS, {len(only_c)} only in C")
    for r in only_ts[:limit]:
        print(f"    TS {r}")
    for r in only_c[:limit]:
        print(f"    C  {r}")


def explain(t: int, ts: dict, c: dict):
    for comp in ("pl", "un", "at", "al"):
        if ts["h"][comp] != c["h"][comp]:
            show_rows(comp, ts["d"][comp], c["d"][comp])
    if ts["h"]["own"] != c["h"]["own"]:
        tt = {a: b for a, b in ts["tiles"]}
        ct = {a: b for a, b in c["tiles"]}
        bad = sorted(k for k in set(tt) | set(ct) if tt.get(k) != ct.get(k))
        print(f"  own: {len(tt)} tiles changed in TS this tick, {len(ct)} in C; {len(bad)} disagree "
              f"(tile: TS new owner / C new owner, '-' = unchanged)")
        for k in bad[:20]:
            print(f"    {k}: {tt.get(k, '-')} / {ct.get(k, '-')}")
        if not bad:
            print("    (all of this tick's changes agree: the divergence is older than this tick's changes)")
    if ts["h"]["fall"] != c["h"]["fall"]:
        print("  fall: fallout differs")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("scenario")
    ap.add_argument("--ticks", type=int, help="override the scenario length")
    ap.add_argument("--refresh", action="store_true", help="ignore the cached TS trace")
    a = ap.parse_args()
    sc = json.loads(Path(a.scenario).read_text())
    if a.ticks:
        sc["ticks"] = a.ticks
    OUT.mkdir(exist_ok=True)
    key = hashlib.sha1((json.dumps(sc, sort_keys=True) + of_commit(openfront_dir())).encode()).hexdigest()[:12]
    name = sc.get("name", Path(a.scenario).stem)
    scen_path = OUT / f"{name}-{key}.json"
    scen_path.write_text(json.dumps(sc))
    ts_path, c_path = OUT / f"{name}-{key}.ts.jsonl", OUT / f"{name}-{key}.c.jsonl"
    if a.refresh or not ts_path.exists():
        run_ts(sc, scen_path, ts_path, [])
    run_c(scen_path, c_path, [])
    ts, c = read(ts_path), read(c_path)
    for i, (x, y) in enumerate(zip(ts, c)):
        if x["h"] != y["h"]:
            t = x["t"]
            comps = [k for k in x["h"] if x["h"][k] != y["h"][k]]
            print(f"{name}: first divergence at tick {t} in {', '.join(comps)} ({i} ticks matched)")
            td, cd = OUT / f"{name}-{key}.ts.detail.jsonl", OUT / f"{name}-{key}.c.detail.jsonl"
            short = OUT / f"{name}-{key}.detail.json"
            short.write_text(json.dumps(dict(sc, ticks=t + 1)))
            run_ts(sc, short, td, [t])
            run_c(short, cd, [t])
            explain(t, read(td)[t], read(cd)[t])
            sys.exit(1)
    if len(ts) != len(c):
        sys.exit(f"{name}: traces have different lengths (TS {len(ts)}, C {len(c)})")
    print(f"{name}: all {len(ts)} ticks match")


if __name__ == "__main__":
    main()

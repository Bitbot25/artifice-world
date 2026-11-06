#!/usr/bin/env python3
"""Statistical fidelity comparison of the C engine against the OpenFront
TypeScript engine, from identical starting states.

    python3 tools/fidelity/compare.py [--seeds 20] [--jobs 4] [--only idle,pvp,...]
                                      [--report tools/fidelity/REPORT.md] [--refresh]

Needs the OpenFront checkout with `npm ci` done (see README.md). Each section
runs the same scenarios on both engines over many seeds (the TS engine is
deterministic for a given geometry, so scenarios also carry a per-seed spatial
jitter) and compares distributions: mean, std, quantiles and a two-sample KS
test. Sections whose features are missing in the C engine (e.g. ships) say so.
"""

from __future__ import annotations

import argparse
import concurrent.futures as cf
import hashlib
import json
import sys
import time
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))
import common  # noqa: E402
import scenarios as S  # noqa: E402
from stats import compare, fmt, md_table  # noqa: E402

import artifice_engine as ae  # noqa: E402

HERE = Path(__file__).resolve().parent
OUT = HERE / "out"

SECTIONS = ["idle", "expand", "tn", "pvp", "conquer", "enclave", "spawn", "ai", "boats"]


# ---------------------------------------------------------------------------
# Running with a cache
# ---------------------------------------------------------------------------


def _tags() -> tuple[str, str]:
    of = common.openfront_dir()
    rev = "none"
    if of and (of / ".git").exists():
        import subprocess

        rev = subprocess.run(["git", "-C", str(of), "rev-parse", "HEAD"], capture_output=True, text=True).stdout.strip()
    lib = Path(ae._lib.lib._name)
    ts_tag = hashlib.sha1((rev + (HERE / "ts_runner.ts").read_text()).encode()).hexdigest()[:10]
    c_tag = hashlib.sha1((str(lib.stat().st_mtime_ns) + (HERE / "c_runner.py").read_text()).encode()).hexdigest()[:10]
    return ts_tag, c_tag


def _c_worker(sc: dict) -> list[dict]:
    import c_runner

    return c_runner.c_run(sc)


def _ts_worker(sc: dict) -> list[dict]:
    return common.ts_run(sc)


def _chunks(sc: dict) -> list[dict]:
    """Splits a long scenario by seeds so the chunks run in parallel."""
    n = sc.get("chunk")
    if not n or len(sc["seeds"]) <= n or sc.get("jitter"):
        return [sc]
    out = []
    for i in range(0, len(sc["seeds"]), n):
        c = dict(sc)
        c["seeds"] = sc["seeds"][i:i + n]
        out.append(c)
    return out


def run_all(scs: list[dict], jobs: int, refresh: bool, engines=("ts", "c")) -> dict[str, dict[str, list[dict]]]:
    """name -> {"ts": records, "c": records}."""
    (OUT / "cache").mkdir(parents=True, exist_ok=True)
    (OUT / ".gitignore").write_text("*\n")
    ts_tag, c_tag = _tags()
    res: dict[str, dict] = {sc["name"]: {e: [] for e in engines} for sc in scs}
    todo = []
    for sc in scs:
        for part in _chunks(sc):
            h = hashlib.sha1(json.dumps(part, sort_keys=True).encode()).hexdigest()[:12]
            for eng in engines:
                path = OUT / "cache" / f"{sc['name']}.{h}.{ts_tag if eng == 'ts' else c_tag}.{eng}.json"
                if path.exists() and not refresh:
                    res[sc["name"]][eng] += json.loads(path.read_text())
                else:
                    todo.append((part, sc["name"], eng, path))
    if not todo:
        return res
    t0 = time.time()
    done = 0
    with cf.ProcessPoolExecutor(max_workers=jobs) as pool:
        futs = {}
        for part, name, eng, path in todo:
            futs[pool.submit(_ts_worker if eng == "ts" else _c_worker, part)] = (name, eng, path)
        for f in cf.as_completed(futs):
            name, eng, path = futs[f]
            try:
                recs = f.result()
            except Exception as e:  # keep going: the section reports missing data
                print(f"  ! {eng} {name} failed: {str(e)[-400:]}", file=sys.stderr)
                recs = []
            else:
                path.write_text(json.dumps(recs))
            res[name][eng] += recs
            done += 1
            if done % 10 == 0 or done == len(todo):
                print(f"  ran {done}/{len(todo)} ({time.time() - t0:.0f}s)", file=sys.stderr)
    return res


# ---------------------------------------------------------------------------
# Record helpers
# ---------------------------------------------------------------------------


def samples(recs: list[dict]) -> dict[int, dict[int, list]]:
    """seed -> t -> list of player rows [id, type, tiles, troops, gold, alive]."""
    out: dict = {}
    for r in recs:
        if r["ev"] == "sample":
            out.setdefault(r["seed"], {})[r["t"]] = r["p"]
    return out


def player_at(smp, seed, t, pid):
    for row in smp.get(seed, {}).get(t, []):
        if row[0] == pid:
            return row
    return None


def per_seed(recs, ev):
    out: dict = {}
    for r in recs:
        if r["ev"] == ev:
            out.setdefault(r["seed"], []).append(r)
    return out


class Report:
    def __init__(self):
        self.parts: list[str] = []
        self.flags: list[tuple[str, str, dict]] = []  # (section, metric, cmp)
        self.images: list[str] = []

    def add(self, s: str):
        self.parts.append(s)

    def flag(self, section, metric, cmp_):
        self.flags.append((section, metric, cmp_))


def row_for(name: str, cmp_: dict, digits=4):
    t, c = cmp_["ts"], cmp_["c"]
    return [name, t["n"], fmt(t["mean"], digits), fmt(t["std"], digits), fmt(c["mean"], digits), fmt(c["std"], digits),
            f"{cmp_['rel'] * 100:+.1f}%" if np.isfinite(cmp_["rel"]) else "-",
            fmt(cmp_["ks_d"], 3), fmt(cmp_["ks_p"], 3), cmp_["verdict"]]


CMP_HEAD = ["metric", "n", "TS mean", "TS std", "C mean", "C std", "Δ mean", "KS D", "KS p", "verdict"]


def plt_or_none():
    try:
        import matplotlib

        matplotlib.use("Agg")
        import matplotlib.pyplot as plt

        return plt
    except Exception:
        return None


def save_fig(plt, fig, name, rep: Report):
    p = OUT / f"{name}.png"
    fig.savefig(p, dpi=110, bbox_inches="tight")
    plt.close(fig)
    rep.images.append(name)
    return f"![{name}](out/{name}.png)"


# ---------------------------------------------------------------------------
# Sections
# ---------------------------------------------------------------------------


def sec_idle(rep: Report, res, sc):
    r = res[sc["name"]]
    ts, c = samples(r["ts"]), samples(r["c"])
    rep.add("## 1. Idle player: troop and gold curves\n")
    rep.add("One human spawns on plains and does nothing for 6000 ticks (10 min). Both engines are deterministic "
            "here, so the curves should coincide exactly.\n")
    seeds = sc["seeds"]
    ts_t = sorted(next(iter(ts.values())).keys()) if ts else []
    rows = []
    worst = {"troops": 0.0, "gold": 0.0, "tiles": 0.0}
    series = {"ts": {"troops": [], "gold": []}, "c": {"troops": [], "gold": []}}
    for t in ts_t:
        tt = [player_at(ts, s, t, 1) for s in seeds if s in ts]
        cc = [player_at(c, s, t, 1) for s in seeds if s in c]
        if not tt or not cc or None in tt or None in cc:
            continue
        for key, idx in (("tiles", 2), ("troops", 3), ("gold", 4)):
            a, b = np.mean([x[idx] for x in tt]), np.mean([x[idx] for x in cc])
            if key != "tiles":
                series["ts"][key].append(a)
                series["c"][key].append(b)
            worst[key] = max(worst[key], abs(b - a) / max(abs(a), 1.0))
        if t in (0, 100, 600, 1200, 2400, 4200, 6000):
            rows.append([t, fmt(np.mean([x[3] for x in tt])), fmt(np.mean([x[3] for x in cc])),
                         fmt(np.mean([x[4] for x in tt])), fmt(np.mean([x[4] for x in cc])),
                         fmt(np.mean([x[2] for x in tt])), fmt(np.mean([x[2] for x in cc]))])
    rep.add(md_table(["tick", "TS troops", "C troops", "TS gold", "C gold", "TS tiles", "C tiles"], rows))
    rep.add(f"\nWorst relative difference over the whole curve (mean over seeds): troops {worst['troops']:.2e}, "
            f"gold {worst['gold']:.2e}, tiles {worst['tiles']:.2e}.\n")
    verdict = "ok" if max(worst.values()) < 1e-3 else "DIFF"
    rep.flag("idle", "idle troop/gold curve max rel diff", {"verdict": verdict, "rel": max(worst.values()),
                                                           "ts": {"mean": 0}, "c": {"mean": 0}})
    plt = plt_or_none()
    if plt and ts_t and series["ts"]["troops"]:
        fig, ax = plt.subplots(1, 2, figsize=(9, 3.2))
        tl = [t for t in ts_t][: len(series["ts"]["troops"])]
        for k, a in zip(("troops", "gold"), ax):
            a.plot(tl, series["ts"][k], label="TS", lw=2)
            a.plot(tl, series["c"][k], "--", label="C", lw=2)
            a.set_title(f"idle player {k}")
            a.set_xlabel("ticks since spawn phase end")
            a.legend()
        rep.add(save_fig(plt, fig, "idle_curves", rep) + "\n")


def _metric_at(smp, seeds, t, pid, idx):
    out = []
    for s in seeds:
        row = player_at(smp, s, t, pid)
        if row is not None:
            out.append(row[idx])
    return out


def sec_expand(rep: Report, res, sc):
    r = res[sc["name"]]
    ts, c = samples(r["ts"]), samples(r["c"])
    seeds = sc["seeds"]
    rep.add("## 2. Expanding player (attacks terra nullius every 10 s with the default 1/5 of troops)\n")
    rows = []
    for t in (100, 500, 1000, 2000, 4000):
        for name, idx in (("tiles", 2), ("troops", 3), ("gold", 4)):
            cm = compare(_metric_at(ts, seeds, t, 1, idx), _metric_at(c, seeds, t, 1, idx))
            rows.append(row_for(f"{name} @ t={t}", cm))
            rep.flag("expand", f"{name} @ t={t}", cm)
    rep.add(md_table(CMP_HEAD, rows) + "\n")
    plt = plt_or_none()
    if plt:
        fig, ax = plt.subplots(1, 3, figsize=(12, 3.2))
        for a, (name, idx) in zip(ax, (("tiles", 2), ("troops", 3), ("gold", 4))):
            for eng, smp, col in (("TS", ts, "C0"), ("C", c, "C1")):
                tl = sorted(next(iter(smp.values())).keys()) if smp else []
                m = np.array([np.mean(_metric_at(smp, seeds, t, 1, idx)) for t in tl])
                sd = np.array([np.std(_metric_at(smp, seeds, t, 1, idx)) for t in tl])
                a.plot(tl, m, color=col, label=eng)
                a.fill_between(tl, m - sd, m + sd, color=col, alpha=0.2)
            a.set_title(f"expanding player: {name}")
            a.set_xlabel("ticks")
            a.legend()
        rep.add(save_fig(plt, fig, "expand_curves", rep) + "\n")


def attack_of(recs_by_seed, seed, attacker=1, target=None):
    for a in recs_by_seed.get(seed, []):
        if a["attacker"] == attacker and (target is None or a["target"] == target):
            return a
    return None


def _cell_metrics(r_ts, r_c, sc, target, horizon_ticks=(30, 100)):
    seeds = sc["seeds"]
    t_start = sc["intents"][0]["tick"]
    out = {}
    for eng, recs in (("ts", r_ts), ("c", r_c)):
        smp = samples(recs)
        atk = per_seed(recs, "attack")
        opn = per_seed(recs, "attack_open")
        m = {"tiles_final": [], "loss": [], "duration": [], "done": [], "loss_per_tile": [], "def_lost": []}
        for h in horizon_ticks:
            m[f"tiles_t{h}"] = []
        for s in seeds:
            if s not in smp:
                continue
            tl = sorted(smp[s].keys())
            base = player_at(smp, s, 0, 1)
            last = smp[s][tl[-1]]
            me_last = [x for x in last if x[0] == 1][0]
            m["tiles_final"].append(me_last[2] - base[2])
            if target:
                m["def_lost"].append(player_at(smp, s, 0, target)[2] - [x for x in last if x[0] == target][0][2])
            for h in horizon_ticks:
                row = player_at(smp, s, t_start + h - (t_start + h) % sc["sample_every"], 1)
                m[f"tiles_t{h}"].append((row[2] - base[2]) if row else np.nan)
            a = attack_of(atk, s, 1, target)
            m["done"].append(1.0 if a else 0.0)
            if a:
                m["duration"].append(a["t1"] - a["t0"])
                loss = a["troops0"] - max(a["troops_end"], 0.0)
                m["loss"].append(loss)
                if a["tiles"] > 0:
                    m["loss_per_tile"].append(loss / a["tiles"])
        out[eng] = m
    return out


def sec_attacks(rep: Report, res, cells, title, key, target, intro):
    rep.add(f"## {title}\n")
    rep.add(intro + "\n")
    rows, summary = [], {}
    horizon = (30, 100)
    for sc in cells:
        r = res[sc["name"]]
        if not r.get("ts") or not r.get("c"):
            continue
        cm = _cell_metrics(r["ts"], r["c"], sc, target, horizon)
        meta = sc["meta"]
        for metric in ("tiles_t30", "tiles_t100", "tiles_final", "loss", "loss_per_tile", "duration", "done"):
            cvals = cm["c"][metric]
            if metric == "duration":
                # C's attack also ticks on the tick it starts, TS's first conquest is the next tick.
                cvals = [v + 1 for v in cvals]
            cmp_ = compare(cm["ts"][metric], cvals, rel_tol=0.08)
            summary.setdefault(metric, []).append((sc["name"], meta, cmp_))
            if metric in ("tiles_final", "loss"):
                pass
    # summary per metric across cells
    srows = []
    for metric, lst in summary.items():
        rels = np.array([x[2]["rel"] for x in lst if np.isfinite(x[2]["rel"])])
        nd = sum(1 for x in lst if x[2]["verdict"] == "DIFF")
        nw = sum(1 for x in lst if x[2]["verdict"] == "warn")
        srows.append([metric, len(lst), f"{np.median(rels) * 100:+.1f}%" if len(rels) else "-",
                      f"{np.percentile(np.abs(rels), 90) * 100:.1f}%" if len(rels) else "-", nd, nw])
        rep.flag(key, f"{metric}: cells with significant difference", {
            "verdict": "DIFF" if nd > len(lst) / 3 else ("warn" if nd + nw > 0 else "ok"),
            "rel": float(np.median(rels)) if len(rels) else float("nan"),
            "ts": {"mean": 0}, "c": {"mean": 0}, "note": f"{nd} DIFF / {nw} warn of {len(lst)} cells"})
    rep.add("`duration` compares C+1 tick: in C an attack also conquers on the tick it was issued, in TS the "
            "first conquest is the next tick (see findings).\n")
    rep.add("Per-metric summary over all cells (relative difference of the C mean to the TS mean):\n")
    rep.add(md_table(["metric", "cells", "median Δ", "p90 |Δ|", "cells DIFF", "cells warn"], srows) + "\n")

    # per-cell table for the headline metrics
    for metric in ("tiles_final", "loss", "duration"):
        rows = []
        for name, meta, cmp_ in summary[metric]:
            rows.append([name] + row_for(metric, cmp_)[1:])
        rep.add(f"<details><summary>per-cell `{metric}`</summary>\n")
        rep.add(md_table(["cell"] + CMP_HEAD[1:], rows) + "\n\n</details>\n")

    # losses vs sizes
    plt = plt_or_none()
    if plt:
        fig, ax = plt.subplots(1, 3, figsize=(12, 3.6))
        for a, metric in zip(ax, ("tiles_final", "loss", "duration")):
            xs = [x[2]["ts"]["mean"] for x in summary[metric]]
            ys = [x[2]["c"]["mean"] for x in summary[metric]]
            cols = [{"plains": "C2", "highland": "C1", "mountain": "C3"}.get(x[1]["terrain"], "C0")
                    for x in summary[metric]]
            a.scatter(xs, ys, c=cols, s=18)
            lim = [min(xs + ys), max(xs + ys)]
            a.plot(lim, lim, "k:", lw=1)
            a.set_xlabel(f"TS mean {metric}")
            a.set_ylabel(f"C mean {metric}")
            a.set_title(metric + " (green plains, orange highland, red mountain)")
        rep.add(save_fig(plt, fig, f"{key}_scatter", rep) + "\n")
    return summary


def sec_conquer(rep: Report, res, cells):
    rep.add("## 4b. Wiping out a small player\n")
    rep.add("Attacker (2400 tiles) hits an 80-tile defender (1500 troops), which dies as soon as it holds fewer "
            "than 100 tiles and is conquered. Compared at the end (300 ticks): attacker tiles gained, attacker "
            "gold and troops, and whether the defender is dead. The defender starts with 0 or 50k gold.\n")
    rows = []
    for sc in cells:
        r = res[sc["name"]]
        vals = {}
        for eng in ("ts", "c"):
            smp = samples(r[eng])
            m = {"tiles": [], "gold": [], "troops": [], "dead": []}
            for s in sc["seeds"]:
                if s not in smp:
                    continue
                tl = sorted(smp[s])
                a0 = player_at(smp, s, 0, 1)
                a1 = player_at(smp, s, tl[-1], 1)
                d1 = player_at(smp, s, tl[-1], 2)
                m["tiles"].append(a1[2] - a0[2])
                m["gold"].append(a1[4])
                m["troops"].append(a1[3])
                m["dead"].append(0.0 if d1[5] else 1.0)
            vals[eng] = m
        for metric in ("tiles", "gold", "troops", "dead"):
            cmp_ = compare(vals["ts"][metric], vals["c"][metric], 0.03)
            rows.append(row_for(f"{sc['name']} {metric}", cmp_))
            rep.flag("conquer", f"{sc['name']} {metric}", cmp_)
    rep.add(md_table(CMP_HEAD, rows) + "\n")


def sec_enclave(rep: Report, res, cells):
    rep.add("## 5. Enclave annexation\n")
    rep.add("Player 2 owns a disc completely surrounded by player 1 (radius 36). Reported: ticks until player 2 "
            "loses its tiles (sampled every 2 ticks), per enclave radius.\n")
    rows = []
    for sc in cells:
        r = res[sc["name"]]
        vals = {}
        for eng in ("ts", "c"):
            smp = samples(r[eng])
            t_ann, taken = [], []
            for s in sc["seeds"]:
                if s not in smp:
                    continue
                tl = sorted(smp[s])
                p2_0 = player_at(smp, s, 0, 2)[2]
                p1_0 = player_at(smp, s, 0, 1)[2]
                when = np.nan
                for t in tl:
                    row = player_at(smp, s, t, 2)
                    if row[2] < p2_0:
                        when = t
                        break
                t_ann.append(when)
                taken.append(player_at(smp, s, tl[-1], 1)[2] - p1_0)
            vals[eng] = (np.array(t_ann, float), np.array(taken, float))
        frac_ts = np.mean(np.isfinite(vals["ts"][0])) if len(vals["ts"][0]) else float("nan")
        frac_c = np.mean(np.isfinite(vals["c"][0])) if len(vals["c"][0]) else float("nan")
        cm_t = compare(vals["ts"][0][np.isfinite(vals["ts"][0])], vals["c"][0][np.isfinite(vals["c"][0])], 0.1)
        cm_n = compare(vals["ts"][1], vals["c"][1], 0.1)
        rows.append([sc["name"], sc["meta"]["r2"], f"{frac_ts:.0%}", f"{frac_c:.0%}",
                     fmt(cm_t["ts"]["mean"]), fmt(cm_t["c"]["mean"]), cm_t["verdict"],
                     fmt(cm_n["ts"]["mean"]), fmt(cm_n["c"]["mean"]), cm_n["verdict"]])
        rep.flag("enclave", f"{sc['name']} annexation tick", {**cm_t, "verdict": cm_t["verdict"] if frac_ts == frac_c else "DIFF",
                                                              "note": f"annexed in {frac_ts:.0%} TS / {frac_c:.0%} C of games"})
    rep.add(md_table(["scenario", "enclave r", "annexed TS", "annexed C", "TS tick", "C tick", "verdict",
                      "TS tiles gained", "C tiles gained", "verdict"], rows) + "\n")


def sec_spawn(rep: Report, res, sc):
    r = res[sc["name"]]
    rep.add("## 6. Spawn\n")
    rep.add("12 humans spawn at fixed tiles (inland and coastal, >40 tiles apart). Tiles claimed, initial troops and "
            "gold at the end of the spawn phase, paired per player.\n")
    ts, c = samples(r["ts"]), samples(r["c"])
    s0 = sc["seeds"][0]
    n = sc["humans"]
    rows, mism = [], 0
    for i in range(1, n + 1):
        a, b = player_at(ts, s0, 0, i), player_at(c, s0, 0, i)
        if a is None or b is None:
            continue
        if a[2] != b[2] or a[3] != b[3] or a[4] != b[4]:
            mism += 1
        rows.append([i, sc["spawns"][str(i)], a[2], b[2], a[3], b[3], a[4], b[4]])
    rep.add(md_table(["player", "spawn tile", "TS tiles", "C tiles", "TS troops", "C troops", "TS gold", "C gold"], rows) + "\n")
    rep.add(f"{mism} of {len(rows)} players differ.\n")
    rep.add(f"Spawn phase length: TS ends the phase at tick {next((x['t_spawn_end'] for x in r['ts'] if x['ev'] == 'meta'), '?')}, "
            f"C at tick {next((x['t_spawn_end'] for x in r['c'] if x['ev'] == 'meta'), '?')}.\n")
    rep.flag("spawn", "spawn tiles/troops/gold differ", {"verdict": "DIFF" if mism else "ok", "rel": float(mism),
                                                     "ts": {"mean": 0}, "c": {"mean": 0}})


def sec_ai(rep: Report, res, sc, land_tiles):
    r = res[sc["name"]]
    rep.add("## 7. Tribes and nations: expansion curves and game length proxies\n")
    rep.add(f"No humans; {sc['bots']} tribes and `{sc['nations']}` nations on the compact world map, "
            f"{sc['ticks']} ticks after the spawn phase. Compared over {len(sc['seeds'])} seeds. "
            "(Spawn positions are random in both engines, so only distributions are comparable.)\n")
    ts, c = samples(r["ts"]), samples(r["c"])
    seeds = sc["seeds"]
    if not ts or not c:
        rep.add("_no data_\n")
        return
    meta = {e: [x for x in r[e] if x["ev"] == "meta"] for e in ("ts", "c")}
    rep.add(f"Players per game: TS {np.mean([m['num_players'] for m in meta['ts']]):.0f}, "
            f"C {np.mean([m['num_players'] for m in meta['c']]):.0f}.\n")
    tl = sorted(next(iter(ts.values())).keys())
    checkpoints = [t for t in (0, 600, 1200, 2400, 4000, 6000, 9000) if t in tl]
    rows = []
    curves = {}
    for ptype, label in (("b", "tribe"), ("n", "nation")):
        for t in checkpoints:
            def agg(smp, fn):
                out = []
                for s in seeds:
                    if s in smp and t in smp[s]:
                        ps = [x for x in smp[s][t] if x[1] == ptype]
                        out.append(fn(ps))
                return out
            for name, fn in (("tiles total", lambda ps: sum(x[2] for x in ps)),
                             ("alive", lambda ps: sum(x[5] for x in ps)),
                             ("mean tiles/alive", lambda ps: (np.mean([x[2] for x in ps if x[5]]) if any(x[5] for x in ps) else 0))):
                cmp_ = compare(agg(ts, fn), agg(c, fn), 0.15)
                rows.append(row_for(f"{label} {name} @ t={t}", cmp_))
                if t in (checkpoints[-1],):
                    rep.flag("ai", f"{label} {name} @ t={t}", cmp_)
        for s_name, smp in (("ts", ts), ("c", c)):
            curves[(label, s_name)] = [np.mean([sum(x[2] for x in smp[s][t] if x[1] == ptype) for s in seeds if s in smp and t in smp[s]]) for t in tl]
    rep.add(md_table(CMP_HEAD, rows) + "\n")

    # pooled per-player tile distribution at the last checkpoint
    t_last = checkpoints[-1]
    rows2 = []
    for ptype, label in (("b", "tribe"), ("n", "nation")):
        pools = {}
        for eng, smp in (("ts", ts), ("c", c)):
            vals = []
            for s in seeds:
                if s in smp and t_last in smp[s]:
                    vals += [x[2] for x in smp[s][t_last] if x[1] == ptype and x[5]]
            pools[eng] = vals
        cmp_ = compare(pools["ts"], pools["c"], 0.15)
        rows2.append(row_for(f"{label} tiles per alive player @ t={t_last} (pooled)", cmp_))
        rep.flag("ai", f"{label} pooled tiles per alive player @ t={t_last}", cmp_)
    rep.add(md_table(CMP_HEAD, rows2) + "\n")

    # leader share / game length proxy
    def leader_share(smp, s, t):
        return max(x[2] for x in smp[s][t]) / land_tiles
    rows3 = []
    for t in checkpoints[1:]:
        a = [leader_share(ts, s, t) for s in seeds if s in ts and t in ts[s]]
        b = [leader_share(c, s, t) for s in seeds if s in c and t in c[s]]
        cmp_ = compare(a, b, 0.15)
        rows3.append(row_for(f"leader land share @ t={t}", cmp_))
    rep.add("Leader's share of all land (game-length proxy; the win bar is 80%):\n")
    rep.add(md_table(CMP_HEAD, rows3) + "\n")
    ends = {e: [x for x in r[e] if x["ev"] == "end"] for e in ("ts", "c")}
    rep.add(f"Games ended by a winner within {sc['ticks']} ticks: TS {sum(x['over'] for x in ends['ts'])}/{len(ends['ts'])}, "
            f"C {sum(x['over'] for x in ends['c'])}/{len(ends['c'])}.\n")
    plt = plt_or_none()
    if plt:
        fig, ax = plt.subplots(1, 2, figsize=(10, 3.2))
        for a, label in zip(ax, ("tribe", "nation")):
            a.plot(tl, curves[(label, "ts")], label="TS", lw=2)
            a.plot(tl, curves[(label, "c")], "--", label="C", lw=2)
            a.set_title(f"{label}s: total tiles owned")
            a.set_xlabel("ticks")
            a.legend()
        rep.add(save_fig(plt, fig, "ai_curves", rep) + "\n")


def sec_boats(rep: Report, res, cells):
    rep.add("## 8. Boats: travel time vs distance\n")
    rep.add("A human owning a coastal disc sends one transport (default 1/5 of troops) to a coast at a given "
            "straight-line distance on the same sea. Ticks from launch to landing.\n")
    rows, pts = [], {"ts": [], "c": []}
    c_has = False
    for sc in cells:
        r = res[sc["name"]]
        out = {}
        for eng in ("ts", "c"):
            bs = [x for x in r.get(eng, []) if x["ev"] == "boat" and x["arrived"]]
            out[eng] = bs
            for b in bs[:1]:
                pts[eng].append((b["dist"], b["t1"] - b["t0"]))
        c_has |= bool(out["c"])
        t = [b["t1"] - b["t0"] for b in out["ts"]]
        cc = [b["t1"] - b["t0"] for b in out["c"]]
        rows.append([sc["name"], fmt(sc["meta"]["dist"]), len(t), fmt(np.mean(t)) if t else "-",
                     len(cc), fmt(np.mean(cc)) if cc else "-"])
    rep.add(md_table(["scenario", "distance", "TS landings", "TS ticks", "C landings", "C ticks"], rows) + "\n")
    if pts["ts"]:
        d, t = np.array(pts["ts"]).T
        slope = float((d * t).sum() / (d * d).sum())
        rep.add(f"TS: travel ticks ≈ {slope:.2f} × straight-line distance (through-origin fit over {len(d)} routes).\n")
    if not c_has:
        rep.add("**C engine: no transport ship was launched or landed (ships subsystem not implemented yet), "
                "so there is nothing to compare. Re-run once ships land.**\n")
        rep.flag("boats", "boat travel time", {"verdict": "n/a", "rel": float("nan"), "ts": {"mean": 0}, "c": {"mean": 0},
                                              "note": "C has no ships yet"})
    else:
        d, t = np.array(pts["c"]).T
        slope = float((d * t).sum() / (d * d).sum())
        rep.add(f"C: travel ticks ≈ {slope:.2f} × straight-line distance.\n")
        rep.flag("boats", "boat travel time", {"verdict": "ok", "rel": 0.0, "ts": {"mean": 0}, "c": {"mean": 0}})
    plt = plt_or_none()
    if plt and pts["ts"]:
        fig, ax = plt.subplots(figsize=(4.5, 3.4))
        for eng, col in (("ts", "C0"), ("c", "C1")):
            if pts[eng]:
                d, t = np.array(pts[eng]).T
                ax.scatter(d, t, c=col, label=eng.upper())
        ax.set_xlabel("straight-line distance (tiles)")
        ax.set_ylabel("travel ticks")
        ax.legend()
        rep.add(save_fig(plt, fig, "boats", rep) + "\n")


# ---------------------------------------------------------------------------


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--seeds", type=int, default=20)
    ap.add_argument("--jobs", type=int, default=4)
    ap.add_argument("--only", default=",".join(SECTIONS), help=f"comma list of {SECTIONS}")
    ap.add_argument("--report", default=str(HERE / "REPORT.md"))
    ap.add_argument("--findings", default=str(HERE / "findings.md"),
                    help="hand-written text spliced into the report after the summary")
    ap.add_argument("--refresh", action="store_true", help="ignore cached runs")
    ap.add_argument("--ai-ticks", type=int, default=6000)
    ap.add_argument("--ai-bots", type=int, default=60)
    args = ap.parse_args()

    if not common.ts_available():
        sys.exit("OpenFront TS engine not available: run tools/fetch_openfront.sh, then `npm ci` in third_party/openfront "
                 "(or set OPENFRONT_DIR).")
    maps = common.maps_dir()
    m = ae.Map.load(str(maps / S.MAP), compact=(S.MAP_SIZE == "compact"))
    arenas = S.Arenas(m.terrain())
    seeds = list(range(1, args.seeds + 1))
    only = set(args.only.split(","))

    plan: dict[str, list[dict]] = {}
    if "idle" in only:
        plan["idle"] = [S.idle_human(arenas, seeds[:3])]
    if "expand" in only:
        plan["expand"] = [S.expand_human(arenas, seeds)]
    if "tn" in only:
        plan["tn"] = S.tn_cells(arenas, seeds)
    if "pvp" in only:
        plan["pvp"] = S.pvp_cells(arenas, seeds[: max(8, args.seeds // 2)])
    if "conquer" in only:
        plan["conquer"] = S.conquer_cells(arenas, seeds[:8])
    if "enclave" in only:
        plan["enclave"] = S.enclave_cells(arenas, seeds[:6])
    if "spawn" in only:
        plan["spawn"] = [S.spawn_scenario(arenas, seeds[:1])]
    if "ai" in only:
        plan["ai"] = [S.ai_mix(arenas, seeds[: max(6, args.seeds // 3)], ticks=args.ai_ticks, bots=args.ai_bots)]
    if "boats" in only:
        plan["boats"] = S.boat_cells(arenas, seeds[:2])

    allsc = [sc for lst in plan.values() for sc in lst]
    print(f"{len(allsc)} scenarios, running TS and C ({args.jobs} jobs)...", file=sys.stderr)
    res = run_all(allsc, args.jobs, args.refresh)

    rep = Report()
    if "idle" in plan:
        sec_idle(rep, res, plan["idle"][0])
    if "expand" in plan:
        sec_expand(rep, res, plan["expand"][0])
    if "tn" in plan:
        sec_attacks(rep, res, plan["tn"], "3. Land attacks on terra nullius", "tn", None,
                    "A 40x40 block (1600 tiles, 40k troops) attacks the free land around it with a fixed number of "
                    "troops, on plains / highland / mountain arenas. Tiles gained after 30 / 100 ticks and at the end "
                    "(400 ticks), attacker troops lost, attack duration, and the share of games in which the attack "
                    "finished (`done`).")
    if "pvp" in only and "pvp" in plan:
        sec_attacks(rep, res, plan["pvp"], "4. Land attacks on a player", "pvp", 2,
                    "A 2400-tile attacker (40k troops) hits an adjacent defender block (1200 or 2400 tiles, 4k or 20k "
                    "troops) with 3k / 10k / 30k troops, on three terrain types, after the 50-tick spawn immunity.")
    if "conquer" in plan:
        sec_conquer(rep, res, plan["conquer"])
    if "enclave" in plan:
        sec_enclave(rep, res, plan["enclave"])
    if "spawn" in plan:
        sec_spawn(rep, res, plan["spawn"][0])
    if "ai" in plan:
        sec_ai(rep, res, plan["ai"][0], m.num_land_tiles)
    if "boats" in plan:
        sec_boats(rep, res, plan["boats"])

    # summary table
    summ = []
    for sec, metric, cmp_ in rep.flags:
        if cmp_["verdict"] in ("DIFF", "warn", "n/a"):
            note = cmp_.get("note", "")
            rel = cmp_["rel"]
            summ.append([sec, metric, cmp_["verdict"], f"{rel * 100:+.1f}%" if np.isfinite(rel) else "-", note])
    head = ["# Fidelity report: C engine vs OpenFront TypeScript engine\n",
            f"Generated by `tools/fidelity/compare.py` ({time.strftime('%Y-%m-%d')}); OpenFront commit "
            f"`{_rev()}`; map `{S.MAP}` ({S.MAP_SIZE}), {args.seeds} seeds. C `OFE_RULES_VERSION={ae.RULES_VERSION}`.\n"]
    head.append("## Summary of flagged metrics\n")
    head.append(md_table(["section", "metric", "verdict", "Δ mean", "note"], summ) if summ else "Nothing flagged.")
    head.append("\n`verdict`: DIFF = |Δ mean| over the section's tolerance and z > 3; warn = z > 3 but small; "
                "ok = indistinguishable at this sample size.\n")
    findings = Path(args.findings)
    if findings.exists():
        head.append(findings.read_text())
    Path(args.report).write_text("\n".join(head) + "\n" + "\n".join(rep.parts))
    print(f"report written to {args.report}", file=sys.stderr)


def _rev():
    of = common.openfront_dir()
    import subprocess

    r = subprocess.run(["git", "-C", str(of), "rev-parse", "--short", "HEAD"], capture_output=True, text=True)
    return r.stdout.strip() or "unknown"


if __name__ == "__main__":
    main()

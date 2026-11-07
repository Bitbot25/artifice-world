#!/usr/bin/env python3
"""Compares the C port of OpenFront's missile/air/rail path code with the TS code.

    make parity
    export OPENFRONT_DIR=<openfront checkout with npm ci>
    python3 tools/parity/air_rail_paths.py [--maps world:1,world:0,iceland:1] [--n 1500] [--seed 1]

For every map (name:compact) it generates random queries, runs them through
air_rail_paths.ts (TS pathfinders on a real game) and air_rail_paths_c.c (the C
port, linked with build-parity/libofe.a) and compares the output lines exactly.
Kinds: parabola (path + stepped ticks + current index), mirv speed, air (path +
stepper calls with fixed and moving targets), rail tiles, rail network (which
stations connect, railroad tiles, cluster order, station paths, train motion).
Exit status 1 if anything differs.
"""
import argparse, json, os, random, subprocess, sys, tempfile
import numpy as np

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
HERE = os.path.dirname(os.path.abspath(__file__))


def load_map(ref, name, compact):
    d = os.path.join(ref, "resources/maps", name)
    m = json.load(open(os.path.join(d, "manifest.json")))
    key = "map4x" if compact else "map"
    w, h = m[key]["width"], m[key]["height"]
    t = np.fromfile(os.path.join(d, "map4x.bin" if compact else "map.bin"), dtype=np.uint8).reshape(h, w)
    return w, h, t


def gen_parabola(rng, w, h, land, n):
    qs = []
    for _ in range(n):
        kind = rng.random()
        if kind < 0.1:  # edge cases: map edges, tiny distances, same tile
            x0, y0 = rng.choice([0, w - 1, rng.randrange(w)]), rng.choice([0, h - 1, rng.randrange(h)])
            x1, y1 = min(w - 1, max(0, x0 + rng.randint(-3, 3))), min(h - 1, max(0, y0 + rng.randint(-3, 3)))
        elif kind < 0.5:  # short
            x0, y0 = rng.randrange(w), rng.randrange(h)
            x1, y1 = min(w - 1, max(0, x0 + rng.randint(-60, 60))), min(h - 1, max(0, y0 + rng.randint(-60, 60)))
        else:
            x0, y0, x1, y1 = rng.randrange(w), rng.randrange(h), rng.randrange(w), rng.randrange(h)
        inc = rng.choice([3, 10, 10, 15, 22, 22, 23, 24, 26, 15.3, 1, 2.5, round(rng.uniform(1, 40), 3)])
        speed = inc if rng.random() < 0.8 else rng.choice([1, 0, 5, 12.5])
        flags = rng.choice([1, 1, 0, 0, 2, 3])  # bit 4 (ignore bounds) throws in TS ref(): only MIRV length uses it
        qs.append(f"P {y0*w+x0} {y1*w+x1} {inc} {speed} {flags}")
    return qs


def gen_mirv(rng, w, h, n):
    return [f"M {rng.randrange(h)*w+rng.randrange(w)} {rng.randrange(h)*w+rng.randrange(w)}" for _ in range(n)]


def gen_air(rng, w, h, n):
    qs = []
    for _ in range(n):
        x0, y0 = rng.randrange(w), rng.randrange(h)
        r = rng.choice([3, 10, 40, 150, 400])
        x1 = min(w - 1, max(0, x0 + rng.randint(-r, r)))
        y1 = min(h - 1, max(0, y0 + rng.randint(-r, r)))
        seed = rng.choice([rng.randrange(0, 5000), rng.randrange(0, 2**32), rng.randrange(0, 2**33)])
        cnt = min(450, abs(x1 - x0) + abs(y1 - y0) + 4)
        mode = rng.random()
        tos = []
        tx, ty = x1, y1
        for _ in range(cnt):
            if mode < 0.4:  # moving target (shell chasing a ship)
                tx = min(w - 1, max(0, tx + rng.randint(-2, 2)))
                ty = min(h - 1, max(0, ty + rng.randint(-2, 2)))
            elif mode < 0.5 and rng.random() < 0.05:  # target jumps
                tx, ty = rng.randrange(w), rng.randrange(h)
            tos.append(ty * w + tx)
        qs.append(f"A {seed} {y0*w+x0} {cnt} " + " ".join(map(str, tos)))
    return qs


def gen_rail(rng, w, h, terr, n):
    land = np.argwhere((terr & 0x80) != 0)
    qs = []
    for _ in range(n):
        y0, x0 = land[rng.randrange(len(land))]
        r = rng.choice([8, 20, 60, 110, 160])
        for _ in range(50):
            y1, x1 = land[rng.randrange(len(land))]
            if abs(int(x1) - int(x0)) <= r and abs(int(y1) - int(y0)) <= r:
                break
        if rng.random() < 0.05:  # some water / arbitrary endpoints
            x1, y1 = rng.randrange(w), rng.randrange(h)
        qs.append(f"R {int(y0)*w+int(x0)} {int(y1)*w+int(x1)}")
    return qs


def gen_net(rng, w, h, terr, nstruct, nq):
    land = np.argwhere(((terr & 0x80) != 0) & ((terr & 0x1f) < 31))
    cy, cx = land[rng.randrange(len(land))]
    spread = rng.choice([40, 80, 150])
    qs = []
    placed = []
    tries = 0
    while len(placed) < nstruct and tries < 5000:
        tries += 1
        y, x = land[rng.randrange(len(land))]
        if abs(int(x) - int(cx)) > spread or abs(int(y) - int(cy)) > spread:
            continue
        t = rng.choice([0, 0, 1, 2, 2])
        if t == 1 and not (terr[y, x] & 0x40):
            t = 0
        placed.append((t, int(y) * w + int(x)))
    if not any(t == 2 for t, _ in placed) and placed:
        placed[rng.randrange(len(placed))] = (2, placed[0][1] if False else placed[0][1])
    for t, tile in placed:
        qs.append(f"S {t} {tile}")
    qs.append("D")
    n = len(placed)
    for _ in range(nq):
        a, b = rng.randrange(n), rng.randrange(n)
        qs.append(f"T {a} {b}")
    for _ in range(max(2, nq // 4)):
        a, b = rng.randrange(n), rng.randrange(n)
        qs.append(f"V {a} {b} {rng.choice([0, 1, 5])}")
    return qs


def run_ts(ref, qfile, ofile):
    exe = os.path.join(ref, "node_modules/.bin/tsx")
    r = subprocess.run([exe, os.path.join(HERE, "air_rail_paths.ts"), "--ref", ref, "--queries", qfile, "--out", ofile],
                       cwd=ref, capture_output=True, text=True)
    if r.returncode:
        print(r.stderr[-3000:])
        sys.exit(2)


def build_c(out):
    lib = os.path.join(ROOT, "build-parity/libofe.a")
    if not os.path.exists(lib):
        subprocess.check_call(["make", "-C", ROOT, "parity"])
    subprocess.check_call(["cc", "-O2", "-std=c99", "-D_DEFAULT_SOURCE", "-DOFE_TS_PARITY=1", "-fopenmp", "-o", out,
                           os.path.join(HERE, "air_rail_paths_c.c"), lib, "-lm", "-lpthread"])


def same(a, b):
    if a.split() == b.split():
        return True
    if a.startswith("M "):  # numbers printed differently
        ta, tb = a.split(), b.split()
        return ta[:2] == tb[:2] and float(ta[2]) == float(tb[2])
    return False


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--maps", default="world:1,world:0,iceland:1")
    ap.add_argument("--n", type=int, default=1500)
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--kinds", default="parabola,mirv,air,rail,net")
    ap.add_argument("--ref", default=os.environ.get("OPENFRONT_DIR"))
    args = ap.parse_args()
    ref = os.path.abspath(args.ref)
    maps_dir = os.path.join(ref, "resources/maps")
    tmp = tempfile.mkdtemp(prefix="airrail_")
    cexe = os.path.join(tmp, "paths_c")
    build_c(cexe)
    rng = random.Random(args.seed)
    bad_total = 0
    print(f"{'map':14} {'kind':9} {'queries':>8} {'match':>8}")
    for spec in args.maps.split(","):
        name, comp = spec.split(":")
        compact = int(comp)
        w, h, terr = load_map(ref, name, compact)
        for kind in args.kinds.split(","):
            if kind == "parabola": qs = gen_parabola(rng, w, h, None, args.n)
            elif kind == "mirv": qs = gen_mirv(rng, w, h, args.n)
            elif kind == "air": qs = gen_air(rng, w, h, args.n)
            elif kind == "rail": qs = gen_rail(rng, w, h, terr, args.n // 3)
            elif kind == "net":
                qs = None
            groups = []
            if kind == "net":  # several independent networks, one process each
                for _ in range(max(2, args.n // 150)):
                    groups.append(gen_net(rng, w, h, terr, rng.choice([8, 15, 25]), 12))
            else:
                groups.append(qs)
            total = bad = 0
            for gi, g in enumerate(groups):
                qf, tf, cf = (os.path.join(tmp, f"{name}{compact}_{kind}_{gi}.{e}") for e in ("q", "ts", "c"))
                with open(qf, "w") as f:
                    f.write(f"map {name} {compact}\n" + "\n".join(g) + "\n")
                run_ts(ref, qf, tf)
                with open(cf, "w") as f:
                    subprocess.run([cexe, maps_dir, qf], stdout=f, check=True)
                a, b = open(tf).read().split("\n"), open(cf).read().split("\n")
                # compare per query tag (kind + index); a query may print 2 lines
                from collections import OrderedDict
                def group(lines):
                    d = OrderedDict()
                    for l in lines:
                        if not l: continue
                        t = l.split()
                        key = (t[0], t[1]) if t[0] in "PMART" and t[0] != "R" or t[0] in ("R", "V") else (t[0], t[1])
                        d.setdefault(key, []).append(l)
                    return d
                ga, gb = group(a), group(b)
                keys = list(ga.keys()) + [k for k in gb if k not in ga]
                for k in keys:
                    total += 1
                    la, lb = ga.get(k, []), gb.get(k, [])
                    if len(la) == len(lb) and all(same(x, y) for x, y in zip(la, lb)):
                        continue
                    bad += 1
                    if bad <= 3:
                        print("  MISMATCH", name, compact, kind, k)
                        for x, y in zip(la, lb):
                            if not same(x, y):
                                print("    TS:", x[:300]); print("    C :", y[:300]); break
            bad_total += bad
            print(f"{name+':'+str(compact):14} {kind:9} {total:8} {100.0*(total-bad)/max(total,1):7.2f}%")
    sys.exit(1 if bad_total else 0)


if __name__ == "__main__":
    main()

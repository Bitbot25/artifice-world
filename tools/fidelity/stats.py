"""Small statistics helpers (numpy only): descriptive stats, two-sample
Kolmogorov-Smirnov, and a comparison row with a verdict."""

from __future__ import annotations

import math

import numpy as np


def ks_2samp(a, b) -> tuple[float, float]:
    """Two-sample KS statistic and asymptotic p-value."""
    a = np.sort(np.asarray(a, dtype=float))
    b = np.sort(np.asarray(b, dtype=float))
    n, m = len(a), len(b)
    if n == 0 or m == 0:
        return float("nan"), float("nan")
    allv = np.concatenate([a, b])
    cdf_a = np.searchsorted(a, allv, side="right") / n
    cdf_b = np.searchsorted(b, allv, side="right") / m
    d = float(np.max(np.abs(cdf_a - cdf_b)))
    en = math.sqrt(n * m / (n + m))
    lam = (en + 0.12 + 0.11 / en) * d
    if lam < 1e-9:
        return d, 1.0
    p = 0.0
    for j in range(1, 101):
        term = 2 * (-1) ** (j - 1) * math.exp(-2 * lam * lam * j * j)
        p += term
        if abs(term) < 1e-10:
            break
    return d, float(min(max(p, 0.0), 1.0))


def describe(x) -> dict:
    x = np.asarray(x, dtype=float)
    x = x[np.isfinite(x)]
    if len(x) == 0:
        return {"n": 0, "mean": float("nan"), "std": float("nan"), "q10": float("nan"), "q50": float("nan"),
                "q90": float("nan")}
    return {"n": len(x), "mean": float(x.mean()), "std": float(x.std(ddof=1)) if len(x) > 1 else 0.0,
            "q10": float(np.quantile(x, 0.1)), "q50": float(np.quantile(x, 0.5)), "q90": float(np.quantile(x, 0.9))}


def compare(ts, c, rel_tol: float = 0.05) -> dict:
    """One metric: TS sample vs C sample. verdict is 'ok', 'warn' or 'DIFF'."""
    ts = np.asarray(ts, dtype=float)
    c = np.asarray(c, dtype=float)
    ts = ts[np.isfinite(ts)]
    c = c[np.isfinite(c)]
    dt, dc = describe(ts), describe(c)
    out = {"ts": dt, "c": dc}
    if dt["n"] == 0 or dc["n"] == 0:
        out.update(ks_d=float("nan"), ks_p=float("nan"), rel=float("nan"), z=float("nan"), verdict="n/a")
        return out
    d, p = ks_2samp(ts, c)
    diff = dc["mean"] - dt["mean"]
    scale = max(abs(dt["mean"]), 1e-9)
    rel = diff / scale
    se = math.sqrt(dt["std"] ** 2 / dt["n"] + dc["std"] ** 2 / dc["n"])
    z = abs(diff) / se if se > 1e-12 else (math.inf if abs(diff) > 1e-9 * scale else 0.0)
    if z > 3 and abs(rel) > rel_tol:
        verdict = "DIFF"
    elif z > 3 and abs(rel) > rel_tol / 2.5:
        verdict = "warn"
    else:
        verdict = "ok"
    out.update(ks_d=d, ks_p=p, rel=rel, z=z, verdict=verdict)
    return out


def fmt(v, digits=4) -> str:
    if v is None or (isinstance(v, float) and not math.isfinite(v)):
        return "-"
    if abs(v) >= 1000:
        return f"{v:,.0f}"
    if abs(v) >= 100:
        return f"{v:.1f}"
    return f"{v:.{digits - 1}g}" if abs(v) < 1 else f"{v:.2f}"


def md_table(headers: list[str], rows: list[list]) -> str:
    out = ["| " + " | ".join(headers) + " |", "|" + "|".join("---" for _ in headers) + "|"]
    for r in rows:
        out.append("| " + " | ".join(str(x) for x in r) + " |")
    return "\n".join(out)

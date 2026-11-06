"""mapstatic tests: synthetic map with known answers, cache behaviour, real maps."""

import os
import sys
import time
from pathlib import Path

import numpy as np
import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "python"))

import artifice_engine as ae  # noqa: E402
from artifice_engine import mapstatic as ms  # noqa: E402

LAND, SHORE, OCEAN = 0x80, 0x40, 0x20


def with_shorelines(t):
    land = (t & LAND) != 0
    shore = np.zeros_like(land)
    shore[1:, :] |= land[1:, :] != land[:-1, :]
    shore[:-1, :] |= land[:-1, :] != land[1:, :]
    shore[:, 1:] |= land[:, 1:] != land[:, :-1]
    shore[:, :-1] |= land[:, :-1] != land[:, 1:]
    t[shore] |= SHORE
    return t


def synthetic(w=600, h=300):
    """Two islands; island A has a 6-wide impassable ridge, island B a lake."""
    t = np.full((h, w), OCEAN, dtype=np.uint8)
    t[40:260, 40:200] = LAND | 3
    t[40:260, 360:540] = LAND | 3
    t[40:260, 100:106] = LAND | 31  # ridge splits A
    t[200:210, 40:100] = LAND | 15  # highland
    t[40:260, 150:159] = LAND | 25  # mountain
    t[120:150, 400:420] = 0  # lake
    return with_shorelines(t)


@pytest.fixture(scope="module")
def smap():
    return ae.Map.from_terrain(synthetic())


@pytest.fixture(scope="module")
def out(smap):
    return ms.compute(smap)


def test_shapes_and_dtypes(out):
    assert int(out["stride"]) == 3 and int(out["cw"]) == 200 and int(out["ch"]) == 100
    assert int(out["num_patches"]) == 50 * 25
    P = int(out["num_patches"])
    for k in ("land_dist", "water_dist", "coastal_dist"):
        assert out[k].shape == (P, P) and out[k].dtype == np.float16
    assert out["tclass"].shape == (300, 600) and out["landmass"].dtype == np.int32
    assert out["land_cost"].shape == (100, 200) and out["land_cost"].dtype == np.float32
    assert out["attack_cost"].shape == (300, 600)
    assert int(out["version"]) == ms.MAPSTATIC_VERSION


def test_terrain_classes_and_costs(out):
    tc, ac = out["tclass"], out["attack_cost"]
    assert tc[10, 10] == ms.TC_OCEAN and tc[130, 410] == ms.TC_LAKE
    assert tc[60, 60] == ms.TC_PLAINS and tc[205, 60] == ms.TC_HIGHLAND
    assert tc[100, 152] == ms.TC_MOUNTAIN and tc[100, 102] == ms.TC_IMPASSABLE
    assert ac[60, 60] == 16.5 and ac[205, 60] == 20 and ac[100, 152] == 25
    assert np.isinf(ac[10, 10]) and np.isinf(ac[100, 102])
    assert out["impassable"][100, 102] == 1 and out["impassable"].sum() == (tc == 5).sum()
    assert out["land_cost"][33, 20] == 16.5
    assert np.isinf(out["land_cost"][3, 3]) and out["water_cost"][3, 3] == 1.0


def test_labels(out):
    assert int(out["num_landmasses"]) == 3 and int(out["num_water_bodies"]) == 2
    lm, wb = out["landmass"], out["water_body"]
    assert lm[100, 60] != lm[100, 180] and lm[100, 100] == 0 and lm[100, 60] > 0
    assert wb[10, 10] == 1 and wb[130, 410] == 2
    assert list(out["water_body_is_ocean"]) == [0, 1, 0]
    assert out["cell_water_body"][43, 136] == 2


def test_steps(out):
    row = 33
    assert out["land_from_left"][row, 33] == 1 and out["land_from_right"][row, 32] == 1
    assert out["land_from_right"][row, 33] == 0  # ridge between cells 33 and 34
    assert out["land_from_left"][row, 34] == 0
    assert out["water_from_left"][3, 3] == 1 and out["land_from_left"][3, 3] == 0
    for k in ("land", "water"):  # left/right and up/down views agree
        assert (out[f"{k}_from_right"][:, :-1] == out[f"{k}_from_left"][:, 1:]).all()
        assert (out[f"{k}_from_down"][:-1] == out[f"{k}_from_up"][1:]).all()


def test_distance_pooling_matches_brute_force(out):
    tc = out["tclass"]
    ys, xs = np.nonzero(tc >= 2)
    land_cell = (33, 50)  # cell row, col; tiles y 99..101, x 150..152
    y0, x0 = land_cell[0] * 3, land_cell[1] * 3
    # nearest water from tile (y, x): brute force over all water tiles
    wy, wx = np.nonzero(tc <= 1)
    ds = []
    for y in range(y0, y0 + 3):
        for x in range(x0, x0 + 3):
            ds.append(np.sqrt(((wy - y) ** 2 + (wx - x) ** 2).min()))
    assert np.isclose(out["coast_land_min"][land_cell], min(ds), atol=1e-4)
    assert np.isclose(out["coast_land_mean"][land_cell], np.mean(ds), atol=1e-3)
    assert np.isinf(out["coast_land_min"][3, 3]) and np.isinf(out["coast_water_min"][33, 50])


def test_patch_tables(out):
    P = int(out["num_patches"])
    pw = int(out["pw"])

    def patch(x, y):
        return (y // 3 // 4) * pw + (x // 3 // 4)

    a1, a2, q = patch(70, 100), patch(180, 100), patch(70, 44)
    L = out["land_dist"].astype(np.float32)
    assert L[a1, a1] == 0 and np.isinf(L[a1, a2]) and np.isfinite(L[a1, q]) and L[a1, q] == L[q, a1]
    ra, rq = out["land_patch_rep"][a1], out["land_patch_rep"][q]
    manh = abs(ra % 200 - rq % 200) + abs(ra // 200 - rq // 200)
    assert L[a1, q] == 3 * manh
    assert out["land_patch_rep"].shape == (P,)
    sea, sea2 = patch(300, 100), patch(570, 20)
    W = out["water_dist"].astype(np.float32)
    assert W[sea, sea] == 0 and np.isfinite(W[sea, sea2]) and np.isinf(W[sea, patch(410, 135)])
    cA, cB = patch(100, 40), patch(400, 40)
    C_ = out["coastal_dist"].astype(np.float32)
    assert out["coastal_patch"][cA] == 1 and out["coastal_patch"][a1] == 0
    assert np.isfinite(C_[cA, cB]) and C_[cA, cB] == C_[cB, cA] and np.isinf(C_[cA, a1])


def test_cache_roundtrip_and_invalidation(smap, out, tmp_path):
    first = ms.load_or_compute(smap, tmp_path)
    files = list(tmp_path.glob("mapstatic-*.npz"))
    assert len(files) == 1
    second = ms.load_or_compute(smap, tmp_path)  # served from the cache
    assert files[0].stat().st_size > 0 and len(list(tmp_path.glob("*"))) == 1
    assert set(first) == set(second) == set(out)
    for k in out:
        assert np.array_equal(out[k], second[k], equal_nan=True), k
        assert second[k].dtype == out[k].dtype, k
    # Different terrain => different key; same terrain => same key.
    t = synthetic()
    t[60, 60] = LAND | 12
    other = ae.Map.from_terrain(t)
    assert ms.cache_path(other, tmp_path) != ms.cache_path(smap, tmp_path)
    assert ms.cache_path(ae.Map.from_terrain(synthetic()), tmp_path) == ms.cache_path(smap, tmp_path)
    assert ms.load_or_compute(other, tmp_path)["tclass"][60, 60] == ms.TC_HIGHLAND
    assert len(list(tmp_path.glob("mapstatic-*.npz"))) == 2


def test_corrupt_cache_is_recomputed(smap, tmp_path):
    p = ms.cache_path(smap, tmp_path)
    p.write_bytes(b"not an npz")
    out = ms.load_or_compute(smap, tmp_path)
    assert int(out["stride"]) == 3
    assert ms.load_or_compute(smap, tmp_path)["tclass"].shape == (300, 600)


def test_deterministic(smap, out):
    again = ms.compute(smap)
    for k in out:
        assert np.array_equal(out[k], again[k], equal_nan=True), k


MAPS = os.environ.get("OFE_MAPS_DIR")


@pytest.mark.skipif(not MAPS, reason="OFE_MAPS_DIR not set")
@pytest.mark.parametrize("name", ["world", "europe", "giantworldmap"])
def test_real_maps(name, tmp_path, capsys):
    m = ae.Map.load(f"{MAPS}/{name}")
    t0 = time.time()
    out = ms.compute(m)
    t_compute = time.time() - t0
    t0 = time.time()
    ms.load_or_compute(m, tmp_path)  # compute + save
    t_save = time.time() - t0
    t0 = time.time()
    cached = ms.load_or_compute(m, tmp_path)
    t_load = time.time() - t0
    size = next(tmp_path.glob("*.npz")).stat().st_size / 1e6
    with capsys.disabled():
        print(
            f"\n[mapstatic] {name}: compute {t_compute:.2f}s, compute+save {t_save:.2f}s, "
            f"cache load {t_load:.2f}s, npz {size:.1f} MB, stride {int(out['stride'])}, "
            f"patches {int(out['num_patches'])}"
        )
    assert t_compute < 60
    assert max(m.width, m.height) <= 256 * int(out["stride"])
    assert np.array_equal(out["land_dist"], cached["land_dist"])
    assert np.array_equal(out["coastal_dist"], cached["coastal_dist"])
    if name == "world":
        assert int(out["num_patches"]) == 2016

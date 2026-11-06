"""ts_import on a synthetic dump (no TypeScript needed), plus an optional
end-to-end test against the real TS engine when it is installed."""

import json
import os
import shutil
import subprocess
import sys
from pathlib import Path

import numpy as np
import pytest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "python"))

import artifice_engine as ae  # noqa: E402
from artifice_engine import ts_import  # noqa: E402

LAND, SHORE, OCEAN = 0x80, 0x40, 0x20


def synthetic_map(w=120, h=90):
    t = np.full((h, w), LAND | 4, dtype=np.uint8)
    t[:3, :] = OCEAN
    t[-3:, :] = OCEAN
    t[:, :3] = OCEAN
    t[:, -3:] = OCEAN
    land = (t & LAND) != 0
    shore = np.zeros_like(land)
    shore[1:, :] |= land[1:, :] != land[:-1, :]
    shore[:-1, :] |= land[:-1, :] != land[1:, :]
    shore[:, 1:] |= land[:, 1:] != land[:, :-1]
    shore[:, :-1] |= land[:, :-1] != land[:, 1:]
    t[shore] |= SHORE
    return ae.Map.from_terrain(t, nations=[("North", 30, 20), ("South", 90, 70)])


def make_dump(m, tick=1234):
    """TS-style state: smallIDs 1,2 humans, 3,4 nations (South first), 5,6 tribes."""
    h, w = m.height, m.width
    owners = np.zeros((h, w), dtype=np.uint16)
    owners[10:20, 10:30] = 1
    owners[10:20, 30:40] = 2
    owners[40:50, 50:60] = 3  # nation "South"
    owners[40:50, 60:65] = 5
    fallout = np.zeros((h, w), dtype=np.uint8)
    fallout[60, 60:63] = 1
    players = [
        dict(small_id=1, type="human", name="a", team=None, alive=True, troops=11111, gold="500", embargoes=[]),
        dict(small_id=2, type="human", name="b", team=None, alive=True, troops=22222, gold="7000000000", embargoes=[]),
        dict(small_id=3, type="nation", name="South", team=None, alive=True, troops=33333, gold="0", embargoes=[]),
        dict(small_id=4, type="nation", name="North", team=None, alive=False, troops=0, gold="0", embargoes=[]),
        dict(small_id=5, type="bot", name="t1", team=None, alive=True, troops=4444, gold="0", embargoes=[]),
        dict(small_id=6, type="bot", name="t2", team=None, alive=False, troops=0, gold="0", embargoes=[]),
    ]
    header = dict(
        tick=tick, in_spawn_phase=False, map="synthetic", map_size="normal", game_mode="Free For All",
        difficulty="Medium", config={}, spawn_immunity_remaining=0, players=players,
        units=[
            dict(id=7, type="City", owner=1, tile=int(m.tile(12, 12)), level=2, under_construction=False),
            dict(id=8, type="Train", owner=1, tile=int(m.tile(13, 13)), level=1),
            dict(id=9, type="Defense Post", owner=2, tile=int(m.tile(35, 12)), level=1, under_construction=True),
        ],
        alliances=[dict(a=1, b=2, created=1000, expires=tick + 500)],
        attacks=[dict(attacker=1, target=0, troops=10, source_tile=None)],
    )
    return ts_import.dumps(header, owners, fallout)


def test_roundtrip_and_import(tmp_path):
    m = synthetic_map()
    f = tmp_path / "d.ofs"
    f.write_bytes(make_dump(m))
    dump = ts_import.load_dump(f)
    assert dump.tick == 1234 and dump.owners.shape == (m.height, m.width)
    res = ts_import.import_state(dump, map=m)
    g = res.game
    # C nations: 3 = North, 4 = South. TS nation "South" (small id 3) maps by name to C 4.
    assert res.player_map[1] == 1 and res.player_map[2] == 2
    assert res.player_map[3] == 4 and res.player_map[4] == 3
    assert res.player_map[5] == 5 and res.player_map[6] == 6
    p = g.players()
    assert p[0]["num_tiles"] == 200 and p[1]["num_tiles"] == 100
    assert p[3]["num_tiles"] == 100 and p[4]["num_tiles"] == 50
    assert p[2]["num_tiles"] == 0
    assert p[0]["troops"] == 11111 and p[1]["troops"] == 22222 and p[3]["troops"] == 33333
    assert p[0]["gold"] == 500 and p[1]["gold"] == 7000000000
    assert g.num_fallout_tiles == 3
    owners = g.owner_map()
    assert owners[15, 20] == 1 and owners[45, 52] == 4 and owners[45, 62] == 5
    units = g.units()
    assert sorted(units["type"].tolist()) == sorted([ae.UNIT["city"], ae.UNIT["defense_post"]])
    city = units[units["type"] == ae.UNIT["city"]][0]
    assert city["owner"] == 1 and city["level"] == 2
    al = g.alliances()
    assert len(al) == 1 and al[0]["expires_tick"] - g.tick == 500
    assert any("train" in w.lower() for w in res.warnings)
    assert any("attack" in w for w in res.warnings)
    g.step(5)  # the imported game runs


def test_bad_inputs(tmp_path):
    with pytest.raises(ValueError):
        ts_import._decode({"format": "nope"}, b"")
    m = synthetic_map()
    f = tmp_path / "x.ofs"
    f.write_bytes(make_dump(m))
    d = ts_import.load_dump(f)
    small = ae.Map.from_terrain(np.full((10, 10), LAND | 4, dtype=np.uint8))
    with pytest.raises(ValueError):
        ts_import.import_state(d, map=small)


def _ts_env():
    of = os.environ.get("OPENFRONT_DIR") or str(ROOT / "third_party" / "openfront")
    ok = (Path(of) / "node_modules" / ".bin" / "tsx").exists() and shutil.which("node")
    maps = os.environ.get("OFE_MAPS_DIR") or str(Path(of) / "resources" / "maps")
    return (of, maps) if ok and Path(maps).is_dir() else None


@pytest.mark.skipif(_ts_env() is None, reason="needs the OpenFront checkout with node_modules")
def test_real_ts_dump_import(tmp_path):
    of, maps = _ts_env()
    sc = {"name": "d", "map": "world", "map_size": "compact", "humans": 2, "nations": "disabled", "bots": 6,
          "spawns": {"1": [250, 280], "2": [600, 100]}, "ticks": 10, "seeds": [1],
          "intents": [{"tick": 0, "player": 1, "type": "attack", "target": 0, "every": 50, "troops": None}]}
    (tmp_path / "s.json").write_text(json.dumps(sc))
    r = subprocess.run([f"{of}/node_modules/.bin/tsx", str(ROOT / "tools/fidelity/ts_dump_state.ts"), "--ref", of,
                        "--scenario", str(tmp_path / "s.json"), "--at", "300", "--out", str(tmp_path / "d")],
                       cwd=of, capture_output=True, text=True, timeout=300)
    assert r.returncode == 0, r.stderr[-2000:]
    dump = ts_import.load_dump(tmp_path / "d_300.ofs")
    res = ts_import.import_state(dump, maps_dir=maps)
    c = res.game.players()
    for p in dump.header["players"]:
        row = c[res.player_map[p["small_id"]] - 1]
        assert row["num_tiles"] == p["tiles"]
        assert row["troops"] == p["troops"] and row["gold"] == int(p["gold"])

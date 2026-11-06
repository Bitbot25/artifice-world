"""Python API tests on a synthetic map (no map assets needed)."""

import os
import sys
from pathlib import Path

import numpy as np
import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "python"))

import artifice_engine as ae  # noqa: E402

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


@pytest.fixture(scope="module")
def world():
    return synthetic_map()


def cfg(**kw):
    base = dict(num_humans=2, num_bots=8, spawn_phase_ticks=5)
    base.update(kw)
    return ae.make_config(**base)


def test_create_and_step(world):
    g = ae.Game(world, cfg(), seed=1)
    assert g.num_players == 2 + 2 + 8
    g.push(g.intent("spawn", player=1, tile=world.tile(20, 60)))
    g.step(10)
    assert not g.in_spawn_phase
    p = g.player(1)
    assert p["flags"] & ae.PF_SPAWNED and p["num_tiles"] > 0
    owners = g.owner_map()
    assert owners.shape == (world.height, world.width)
    assert (owners == 1).sum() == p["num_tiles"]
    players = g.players()
    assert players["num_tiles"].sum() == (owners > 0).sum()


def test_attack_and_events(world):
    g = ae.Game(world, cfg(num_bots=0), seed=2)
    g.push(g.intent("spawn", player=1, tile=world.tile(60, 45)))
    g.step(10)
    g.drain_events()
    before = g.player(1)["num_tiles"]
    g.push(g.intent("attack", player=1, target=0, troops=-1))
    g.step(40)
    assert g.player(1)["num_tiles"] > before
    ev = g.drain_events()
    types = set(ev["type"].tolist())
    assert ae.EVENT["attack_started"] in types
    assert ae.EVENT["tile_owner"] in types
    flips = ev[ev["type"] == ae.EVENT["tile_owner"]]
    mine = flips[flips["c"] == 1]  # nations expand too
    assert len(mine) > 0 and (mine["b"] != 1).all()


def test_clone_restore_determinism(world):
    g = ae.Game(world, cfg(), seed=3)
    g.step(200)
    snap = g.clone()
    assert snap.hash() == g.hash()
    g.step(100)
    h_after = g.hash()
    g2 = snap.clone()
    g2.step(100)
    assert g2.hash() == h_after
    g.restore(snap)
    assert g.hash() == snap.hash()


def test_batch_matches_serial(world):
    games_a = [ae.Game(world, cfg(), seed=s) for s in range(5)]
    games_b = [ae.Game(world, cfg(), seed=s) for s in range(5)]
    ae.step_batch(games_a, 150, num_threads=4)
    for g in games_b:
        g.step(150)
    assert [g.hash() for g in games_a] == [g.hash() for g in games_b]


def test_edits(world):
    g = ae.Game(world, cfg(num_bots=0, num_nations=0, spawn_phase_ticks=0, auto_spawn_humans=0), seed=4)
    for x in range(10, 20):
        for y in range(10, 20):
            g.set_owner(world.tile(x, y), 1)
    assert g.player(1)["num_tiles"] == 100
    g.set_troops(1, 12345)
    g.set_gold(1, 999)
    p = g.player(1)
    assert p["troops"] == 12345 and p["gold"] == 999
    g.set_alliance(1, 2, True)
    assert len(g.alliances()) == 1
    g.set_alliance(1, 2, False)
    assert len(g.alliances()) == 0


def test_intent_array_roundtrip():
    arr = ae.intents_array([{"type": "attack", "player": 1, "target": 2, "troops": 10.0}])
    assert arr.dtype == ae.INTENT_DTYPE
    assert arr[0]["type"] == ae.INTENT["attack"] and arr[0]["troops"] == 10.0


@pytest.mark.skipif(not os.environ.get("OFE_MAPS_DIR"), reason="needs OpenFront maps")
def test_real_map():
    m = ae.Map.load(os.path.join(os.environ["OFE_MAPS_DIR"], "world"))
    assert (m.width, m.height) == (2000, 1000)
    g = ae.Game(m, ae.make_config(spawn_phase_ticks=10, event_mask=ae.EVENTS_NO_TILES), seed=0)
    g.step(300)
    assert g.players()["num_tiles"].sum() > 0


def test_vec_games(world):
    from artifice_engine.vec import VecGames

    envs = VecGames(world, cfg(), seeds=range(4), cadence=20)
    for _ in range(10):
        done = envs.step({0: [{"type": "attack", "player": 1, "target": 0, "troops": -1}]})
    assert done.shape == (4,)
    obs = envs.observe(owner_maps=True, events=True)
    assert obs["players"].shape[0] == 4
    assert obs["owners"].shape == (4, world.height, world.width)
    assert (obs["tick"] == 200).all()
    twin = envs.clone()
    envs.step()
    twin.step()
    assert [g.hash() for g in envs.games] == [g.hash() for g in twin.games]

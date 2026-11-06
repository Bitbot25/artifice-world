"""Vectorised environments: many independent games stepped together.

    from artifice_engine import Map, make_config
    from artifice_engine.vec import VecGames

    world = Map.load(".../maps/world")
    envs = VecGames(world, make_config(num_humans=1), seeds=range(32), cadence=10)
    obs = envs.observe(owner_maps=True)
    envs.step({0: [intent, ...], 5: [...]})     # intents per env index
    obs = envs.observe()                        # players (N, P), owners (N, H, W)

Stepping runs in C threads with the GIL released; results are identical for
any thread count. `cadence` is the number of ticks between decisions
(1 tick = 100 ms).
"""

from __future__ import annotations

from typing import Iterable, Mapping, Sequence

import numpy as np

from . import PLAYER_DTYPE, Config, Game, Map, intents_array, step_batch


class VecGames:
    def __init__(
        self,
        map: Map,
        config: Config | Sequence[Config],
        seeds: Iterable[int],
        cadence: int = 10,
        num_threads: int = 0,
    ):
        self.map = map
        self.seeds = list(seeds)
        n = len(self.seeds)
        self.configs = list(config) if isinstance(config, (list, tuple)) else [config] * n
        if len(self.configs) != n:
            raise ValueError("one config per seed (or a single shared config)")
        self.cadence = int(cadence)
        self.num_threads = int(num_threads)
        self.games = [Game(map, c, seed=s) for c, s in zip(self.configs, self.seeds)]
        self._owners = np.empty((n, map.height, map.width), dtype=np.uint16)

    def __len__(self) -> int:
        return len(self.games)

    def reset(self, index: int, seed: int | None = None) -> None:
        """Starts env `index` over (new seed optional)."""
        if seed is not None:
            self.seeds[index] = int(seed)
        self.games[index] = Game(self.map, self.configs[index], seed=self.seeds[index])

    def step(self, intents: Mapping[int, Iterable] | None = None, ticks: int | None = None) -> np.ndarray:
        """Queues intents per env, then steps every live game `ticks`
        (default: cadence). Returns a bool array: which games are over."""
        if intents:
            for i, batch in intents.items():
                arr = intents_array(batch)
                if len(arr):
                    self.games[i].push(arr)
        live = [g for g in self.games if not g.is_over]
        if live:
            step_batch(live, self.cadence if ticks is None else int(ticks), self.num_threads)
        return np.array([g.is_over for g in self.games])

    def observe(self, owner_maps: bool = False, events: bool = False) -> dict:
        """Snapshot of every env.

        players: (N, P) PLAYER_DTYPE, P = max player count (padded rows have id 0)
        owners:  (N, H, W) uint16, a view of an internal buffer (copy to keep)
        events:  list of EVENT_DTYPE arrays, drained from each game
        """
        n = len(self.games)
        p = max(g.num_players for g in self.games)
        players = np.zeros((n, p), dtype=PLAYER_DTYPE)
        for i, g in enumerate(self.games):
            arr = g.players()
            players[i, : len(arr)] = arr
        out = {
            "players": players,
            "tick": np.array([g.tick for g in self.games], dtype=np.int32),
            "done": np.array([g.is_over for g in self.games]),
            "winner": np.array([g.winner for g in self.games], dtype=np.int32),
        }
        if owner_maps:
            for i, g in enumerate(self.games):
                g.owner_map(out=self._owners[i])
            out["owners"] = self._owners
        if events:
            out["events"] = [g.drain_events() for g in self.games]
        return out

    def clone(self) -> "VecGames":
        """Copy of every env (copy-on-write tiles), e.g. for rollouts."""
        twin = object.__new__(VecGames)
        twin.map = self.map
        twin.seeds = list(self.seeds)
        twin.configs = list(self.configs)
        twin.cadence = self.cadence
        twin.num_threads = self.num_threads
        twin.games = [g.clone() for g in self.games]
        twin._owners = np.empty_like(self._owners)
        return twin

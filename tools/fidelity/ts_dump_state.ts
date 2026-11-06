/*
 * ts_dump_state.ts - dumps the state of an OpenFront TS game to the compact
 * "ofe-state-dump" format read by python/artifice_engine/ts_import.py.
 *
 *   cd <openfront checkout>
 *   npx tsx tools/fidelity/ts_dump_state.ts --ref . --record game.json --at 3000,6000 --out dumps/g
 *   npx tsx .../ts_dump_state.ts --ref . --snapshot snap.bin --out dumps/s
 *   npx tsx .../ts_dump_state.ts --ref . --scenario s.json --seed 1 --at 1500 --out dumps/x
 *
 * Writes <out>_<tick>.ofs for every --at tick (and the last tick if --at is
 * omitted). File layout: one JSON header line (UTF-8, '\n' terminated), then
 * the blobs described in header.blobs: owners (uint16 LE per tile, TS smallID,
 * 0 = nobody) and fallout (uint8 per tile).
 * The OpenFront code is loaded at runtime from --ref; nothing of it is copied here.
 */
import fs from "node:fs";
import path from "node:path";
import { pathToFileURL } from "node:url";

const arg = (n: string, d?: string) => {
  const i = process.argv.indexOf(n);
  return i >= 0 ? process.argv[i + 1] : d;
};
const REF = path.resolve(arg("--ref") ?? process.env.OPENFRONT_DIR ?? "third_party/openfront");
const outBase = arg("--out") ?? "state";
const ats = (arg("--at") ?? "").split(",").filter(Boolean).map(Number);
console.log = (...a: unknown[]) => console.error(...a);
console.debug = console.warn = console.info = () => {};
const imp = (rel: string): Promise<any> => import(pathToFileURL(path.join(REF, rel)).href);

function mapLoader(types: any) {
  return {
    getMapData: (map: any) => {
      const key = Object.keys(types.GameMapType).find((k) => types.GameMapType[k] === map)!.toLowerCase();
      const dir = path.join(REF, "resources/maps", key);
      const bin = (n: string) => async () => new Uint8Array(fs.readFileSync(path.join(dir, n)));
      return {
        mapBin: bin("map.bin"), map4xBin: bin("map4x.bin"), map16xBin: bin("map16x.bin"),
        manifest: async () => JSON.parse(fs.readFileSync(path.join(dir, "manifest.json"), "utf8")),
        webpPath: "", layerImage: async () => { throw new Error("no layers"); },
      };
    },
  };
}

function dumpGame(game: any, types: any, config: any, commit: string) {
  const { PlayerType, UnitType } = types;
  const W = game.width(), H = game.height(), n = W * H;
  const owners = new Uint16Array(n);
  const fallout = new Uint8Array(n);
  const map = game.map();
  for (let t = 0; t < n; t++) {
    owners[t] = map.ownerID(t);
    fallout[t] = game.hasFallout(t) ? 1 : 0;
  }
  const typeName = (t: any) => (t === PlayerType.Human ? "human" : t === PlayerType.Nation ? "nation" : "bot");
  const players = game.allPlayers().map((p: any) => ({
    small_id: p.smallID(), id: p.id(), client_id: p.clientID(), name: p.displayName?.() ?? p.name(),
    type: typeName(p.type()), team: p.team() ?? null, alive: p.isAlive(), tiles: p.numTilesOwned(),
    troops: p.troops(), gold: p.gold().toString(), spawn_tile: p.spawnTile() ?? null,
    disconnected: p.isDisconnected(), has_spawned: p.hasSpawned(),
    embargoes: [...game.allPlayers()].filter((o: any) => o !== p && p.hasEmbargoAgainst(o)).map((o: any) => o.smallID()),
  }));
  const units = game.units().map((u: any) => ({
    id: u.id(), type: u.type(), owner: u.owner().smallID(), tile: u.tile(), level: u.level(),
    under_construction: u.isUnderConstruction(), troops: u.troops?.() ?? 0, health: u.health?.() ?? 0,
    target_tile: u.targetTile?.() ?? null,
  }));
  const seen = new Set<string>();
  const alliances: any[] = [];
  for (const p of game.allPlayers()) {
    for (const a of p.alliances()) {
      const x = a.requestor().smallID(), y = a.recipient().smallID();
      const k = `${Math.min(x, y)}:${Math.max(x, y)}`;
      if (seen.has(k)) continue;
      seen.add(k);
      alliances.push({ a: x, b: y, created: a.createdAt(), expires: a.expiresAt() });
    }
  }
  const attacks: any[] = [];
  for (const p of game.allPlayers()) {
    for (const a of p.outgoingAttacks()) {
      const t = a.target();
      attacks.push({ attacker: p.smallID(), target: t.isPlayer() ? t.smallID() : 0, troops: a.troops(), source_tile: a.sourceTile() });
    }
  }
  const gc = game.config();
  const header = {
    format: "ofe-state-dump", version: 1, engine: "openfront-ts", commit,
    tick: game.ticks(), in_spawn_phase: game.inSpawnPhase(), width: W, height: H,
    map: config.gameMap, map_size: config.gameMapSize, game_mode: config.gameMode, difficulty: config.difficulty,
    config: { bots: config.bots, nations: config.nations, infinite_gold: !!config.infiniteGold,
      infinite_troops: !!config.infiniteTroops, instant_build: !!config.instantBuild, donate_gold: !!config.donateGold,
      donate_troops: !!config.donateTroops, disable_alliances: !!config.disableAlliances, random_spawn: !!config.randomSpawn },
    spawn_phase_turns: gc.numSpawnPhaseTurns(),
    spawn_immunity_remaining: Math.max(0, gc.numSpawnPhaseTurns() + gc.spawnImmunityDuration() - game.ticks()),
    players, units, alliances, attacks,
    blobs: { owners: { dtype: "<u2", bytes: owners.byteLength }, fallout: { dtype: "u1", bytes: fallout.byteLength } },
  };
  return Buffer.concat([Buffer.from(JSON.stringify(header) + "\n"), Buffer.from(owners.buffer), Buffer.from(fallout.buffer)]);
}

async function main() {
  const types = await imp("packages/engine-api/src/game/GameTypes.ts");
  const gr = await imp("packages/engine/src/GameRunner.ts");
  const shared = await imp("packages/shared/src/GameMapLoader.ts");
  let commit = "unknown";
  try { commit = fs.readFileSync(path.join(REF, ".git/HEAD"), "utf8").trim(); } catch {}
  const loader = mapLoader(types);
  let fatal: string | undefined;
  const cb = (gu: any) => { if ("errMsg" in gu) fatal = gu.errMsg; };
  let runner: any, config: any, turns: any[] = [];
  const write = (tick: number) => {
    const f = `${outBase}_${tick}.ofs`;
    fs.mkdirSync(path.dirname(path.resolve(f)), { recursive: true });
    fs.writeFileSync(f, dumpGame(runner.game, types, config, commit));
    console.error("wrote", f);
  };

  if (arg("--snapshot")) {
    const snap = new Uint8Array(fs.readFileSync(arg("--snapshot")!));
    const snapMod = await imp("packages/engine/src/snapshot/GameSnapshot.ts");
    config = snapMod.readSnapshotHeader(snap).gameConfig;
    const gs = { gameID: "snapshot", lobbyCreatedAt: 0, config, players: [] };
    runner = await gr.createGameRunnerFromSnapshot(gs, snap, undefined, await shared.loadMapFiles(loader, config.gameMap, config.gameMapSize), cb);
    write(runner.game.ticks());
    return;
  }
  if (arg("--record")) {
    const util = await imp("packages/shared/src/SharedUtil.ts");
    const rec = util.decompressGameRecord(JSON.parse(fs.readFileSync(arg("--record")!, "utf8")));
    const info = rec.info;
    const gs = util.toWireGameStartInfo({ gameID: info.gameID, lobbyCreatedAt: info.lobbyCreatedAt, config: info.config, players: info.players, tribes: info.tribes });
    config = info.config;
    runner = await gr.createGameRunner(gs, undefined, await shared.loadMapFiles(loader, config.gameMap, config.gameMapSize), cb);
    turns = rec.turns;
  } else if (arg("--scenario")) {
    // minimal: humans spawn at sc.spawns, optional attack-on-terra-nullius intents; runs idle otherwise
    const sc = JSON.parse(fs.readFileSync(arg("--scenario")!, "utf8"));
    const seed = Number(arg("--seed", "1"));
    const mk = Object.keys(types.GameMapType).find((k) => k.toLowerCase() === sc.map.toLowerCase())!;
    const dk = Object.keys(types.Difficulty).find((k) => k.toLowerCase() === (sc.difficulty ?? "medium").toLowerCase())!;
    config = { gameMap: types.GameMapType[mk], gameMapSize: (sc.map_size ?? "compact") === "normal" ? types.GameMapSize.Normal : types.GameMapSize.Compact,
      gameMode: types.GameMode.FFA, gameType: types.GameType.Private, difficulty: types.Difficulty[dk], nations: sc.nations ?? "disabled",
      donateGold: false, donateTroops: false, bots: sc.bots ?? 0, infiniteGold: false, infiniteTroops: false, instantBuild: false, randomSpawn: false };
    const ids = Array.from({ length: sc.humans ?? 0 }, (_, i) => `HUMAN${String(i + 1).padStart(3, "0")}`);
    const gs = { gameID: `dump-${seed}`, lobbyCreatedAt: 0, config, players: ids.map((c, i) => ({ clientID: c, username: `p${i + 1}`, clanTag: null, isLobbyCreator: i === 0 })) };
    runner = await gr.createGameRunner(gs, undefined, await shared.loadMapFiles(loader, config.gameMap, config.gameMapSize), cb);
    const W = runner.game.width();
    const total = Math.max(...ats, 0) + 1;
    for (let t = 0; t < total + 2000; t++) {
      const intents: any[] = [];
      if (t === 1) for (const [k, xy] of Object.entries(sc.spawns ?? {}) as any) intents.push({ type: "spawn", tile: xy[1] * W + xy[0], clientID: ids[Number(k) - 1] });
      if (!runner.game.inSpawnPhase()) for (const it of sc.intents ?? []) {
        const k = runner.game.ticks();
        if (it.type === "attack" && it.every && k % it.every === 0) intents.push({ type: "attack", targetID: null, troops: it.troops ?? null, clientID: ids[it.player - 1] });
      }
      turns.push({ turnNumber: t, intents });
    }
  } else {
    throw new Error("need --record, --snapshot or --scenario");
  }
  const last = ats.length ? Math.max(...ats) : turns.length;
  for (let i = 0; i < turns.length && runner.game.ticks() <= last; i++) {
    if (ats.includes(runner.game.ticks())) write(runner.game.ticks());
    runner.addTurn(turns[i]);
    if (!runner.executeNextTick() || fatal) throw new Error(`tick failed: ${fatal}`);
  }
  if (!ats.length || ats.includes(runner.game.ticks())) write(runner.game.ticks());
}
main().catch((e) => { console.error(e); process.exit(1); });

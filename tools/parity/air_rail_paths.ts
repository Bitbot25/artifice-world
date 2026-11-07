/*
 * air_rail_paths.ts - runs path queries on OpenFront's TS pathfinders and
 * prints the results as plain text lines, for comparison with the C port
 * (engine/src/paths_ts.c, via tools/parity/air_rail_paths_c.c). Driven by
 * tools/parity/air_rail_paths.py.
 *
 *   node_modules/.bin/tsx air_rail_paths.ts --ref <openfront> --queries q.txt
 *
 * Query file (one per line, integers):
 *   map <name> <compact 0|1>
 *   P from to increment speed flags     parabola: flags 1 up, 2 flat height, 4 ignore bounds
 *   A seed from n to_0 .. to_n-1        air: findPath + one stepper call per `to`
 *   R from to                           rail tiles
 *   M name seed                         mirv speed query: M from to  (see below)
 *   mode net: S type tile | T unitA unitB | V unitA unitB cars | D
 * Output lines are tagged with the query index.
 */
import fs from "node:fs";
import path from "node:path";
import { pathToFileURL } from "node:url";

const arg = (n: string, d?: string) => {
  const i = process.argv.indexOf(n);
  return i >= 0 ? process.argv[i + 1] : d;
};
const REF = path.resolve(arg("--ref") ?? process.env.OPENFRONT_DIR ?? "third_party/openfront");
const queriesPath = arg("--queries")!;
console.log = (...a: unknown[]) => console.error(...a);
console.debug = console.warn = console.info = () => {};
const imp = (rel: string): Promise<any> => import(pathToFileURL(path.join(REF, rel)).href);
const out: string[] = [];
const emit = (s: string) => out.push(s);

async function main() {
  const lines = fs.readFileSync(queriesPath, "utf8").split("\n").filter((l) => l.trim() && !l.startsWith("#"));
  const hdr = lines[0].split(/\s+/);
  if (hdr[0] !== "map") throw new Error("first line must be `map <name> <compact>`");
  const mapName = hdr[1];
  const compact = hdr[2] === "1";

  const types = await imp("packages/engine-api/src/game/GameTypes.ts");
  const runnerMod = await imp("packages/engine/src/GameRunner.ts");
  const shared = await imp("packages/shared/src/GameMapLoader.ts");
  const pfMod = await imp("packages/engine/src/pathfinding/PathFinder.ts");
  const airMod = await imp("packages/engine/src/pathfinding/PathFinder.Air.ts");
  const stepMod = await imp("packages/engine/src/pathfinding/PathFinderStepper.ts");
  const paraMod = await imp("packages/engine/src/pathfinding/PathFinder.Parabola.ts");
  const lineMod = await imp("packages/engine/src/utilities/Line.ts");
  const { GameType, GameMapType, GameMapSize, GameMode, Difficulty, UnitType } = types;
  const mapKey = Object.keys(GameMapType).find((k) => k.toLowerCase() === mapName.toLowerCase())!;
  const gameMap = GameMapType[mapKey];
  const gameMapSize = compact ? GameMapSize.Compact : GameMapSize.Normal;
  const gameStart = {
    gameID: "PATHS001",
    lobbyCreatedAt: 0,
    config: {
      gameMap, gameMapSize, gameMode: GameMode.FFA, gameType: GameType.Private, difficulty: Difficulty.Medium,
      nations: "disabled", donateGold: false, donateTroops: false, bots: 0,
      infiniteGold: true, infiniteTroops: true, instantBuild: true, randomSpawn: false,
    },
    players: [{ clientID: "CLIENT01", username: "player1", clanTag: null, isLobbyCreator: true }],
  };
  const loader = {
    getMapData: (map: any) => {
      const key = Object.keys(GameMapType).find((k) => GameMapType[k] === map)!.toLowerCase();
      const dir = path.join(REF, "resources/maps", key);
      const bin = (n: string) => async () => new Uint8Array(fs.readFileSync(path.join(dir, n)));
      return {
        mapBin: bin("map.bin"), map4xBin: bin("map4x.bin"), map16xBin: bin("map16x.bin"),
        manifest: async () => JSON.parse(fs.readFileSync(path.join(dir, "manifest.json"), "utf8")),
        webpPath: "", layerImage: async () => { throw new Error("no layers"); },
      };
    },
  };
  let fatal: string | undefined;
  const runner = await runnerMod.createGameRunner(
    gameStart, undefined, await shared.loadMapFiles(loader, gameMap, gameMapSize),
    (gu: any) => { if ("errMsg" in gu) fatal = `${gu.errMsg}\n${gu.stack ?? ""}`; },
  );
  const game = runner.game;
  const W = game.width();
  let mirvInst: any = null; // MirvExecution.calculateDeterministicSpeed is private: use a bare instance
  void W;

  const mirvMod = await imp("packages/engine/src/execution/MIRVExecution.ts");
  mirvInst = Object.create(mirvMod.MirvExecution.prototype);
  mirvInst.mg = game;
  void lineMod;

  const mode = lines[1]?.split(/\s+/)[0];
  if (mode === "S") {
    await runNet(lines.slice(1));
  } else {
    runPaths(lines.slice(1));
  }

  // ---------------------------------------------------------------- path queries
  function runPaths(qs: string[]) {
    const rail = pfMod.PathFinding.Rail(game);
    qs.forEach((line, qi) => {
      const t = line.split(/\s+/);
      const k = t[0];
      const n = t.slice(1).map(Number);
      if (k === "P") {
        const [from, to, inc, speed, flags] = n;
        const opts = { increment: inc, directionUp: !!(flags & 1), distanceBasedHeight: !(flags & 2), ignoreMapBounds: !!(flags & 4) };
        const pf = new paraMod.ParabolaUniversalPathFinder(game, opts);
        const full = pf.findPath(from, to) as number[];
        emit(`P ${qi} F ${full.length} ${full.join(" ")}`);
        const nodes: number[] = [], idxs: number[] = [];
        let status = -1, end = -1;
        for (let c = 0; c < 200000; c++) {
          const r = pf.next(from, to, speed);
          if (r.status !== 0) { status = r.status; end = r.node; break; }
          nodes.push(r.node);
          idxs.push(pf.currentIndex());
        }
        emit(`P ${qi} S ${nodes.length} ${nodes.join(" ")} I ${idxs.join(" ")} E ${status} ${end}`);
      } else if (k === "M") {
        const [from, to] = n;
        const pts = paraMod.getParabolaControlPoints(game, from, to, { distanceBasedHeight: true, directionUp: true, ignoreMapBounds: true });
        void pts;
        // calculateDeterministicSpeed is private to MirvExecution: call it on a bare instance
        // (async import done lazily below)
        emit(`M ${qi} ${mirvSpeed(from, to)}`);
      } else if (k === "A") {
        const [seed, from, cnt, ...tos] = n;
        const af = new airMod.AirPathFinder(game);
        af.setSeed(seed);
        const full = af.findPath(from, tos[0]) as number[];
        emit(`A ${qi} F ${full.length} ${full.join(" ")}`);
        const st = new stepMod.PathFinderStepper(af, { equals: (a: number, b: number) => a === b });
        let cur = from;
        const res: string[] = [];
        for (let i = 0; i < cnt; i++) {
          const r = st.next(cur, tos[i]);
          res.push(`${r.status}:${r.node ?? -1}`);
          if (r.status === 0) cur = r.node;
        }
        emit(`A ${qi} C ${res.join(" ")}`);
      } else if (k === "R") {
        const p = rail.findPath(n[0], n[1]);
        emit(p === null || p.length === 0 ? `R ${qi} -1` : `R ${qi} ${p.length} ${Array.from(p).join(" ")}`);
      } else throw new Error("bad query " + line);
    });
  }

  function mirvSpeed(from: number, to: number): string {
    if (!mirvInst) throw new Error("mirv module not loaded");
    return String(mirvInst.calculateDeterministicSpeed(from, to));
  }

  // ---------------------------------------------------------------- rail network
  async function runNet(qs: string[]) {
    const player = game.allPlayers()[0];
    const facMod = await imp("packages/engine/src/execution/FactoryExecution.ts");
    const cityMod = await imp("packages/engine/src/execution/CityExecution.ts");
    const portMod = await imp("packages/engine/src/execution/PortExecution.ts");
    const trainMod = await imp("packages/engine/src/execution/TrainExecution.ts");
    (game.config() as any).trainSpawnRate = () => 1e12; // factories must not launch their own trains
    let spawnTile = 0;
    for (let t = Math.floor((W * game.height()) / 2); t < W * game.height(); t++) {
      if (game.isLand(t) && !game.isImpassable(t) && game.x(t) > 5 && game.x(t) < W - 5) { spawnTile = t; break; }
    }
    let turn = 0;
    const tick = () => {
      runner.addTurn({ turnNumber: turn, intents: turn === 0 ? [{ type: "spawn", clientID: "CLIENT01", tile: spawnTile }] : [] });
      turn++;
      if (!runner.executeNextTick() || fatal) throw new Error(`tick failed: ${fatal}`);
    };
    for (let i = 0; i < 400 && (i === 0 || game.inSpawnPhase()); i++) tick();
    if (game.inSpawnPhase()) throw new Error("still in spawn phase");
    const units: any[] = [];
    if (process.env.DEBUG_NET) {
      const rn: any = game.railNetwork();
      const orig = rn.connectStation.bind(rn);
      rn.connectStation = (st: any) => { console.error("connect unit", units.indexOf(st.unit)); orig(st); };
    }
    const unitIdx = (u: any) => units.indexOf(u);
    const stationUnits = () => units.map((u) => game.railNetwork().stationManager().findStation(u));
    const trainUnits = () =>
      game.units(UnitType.Train).slice().sort((a: any, b: any) => a.id() - b.id());
    qs.forEach((line, qi) => {
      const t = line.split(/\s+/);
      const n = t.slice(1).map(Number);
      if (t[0] === "S") {
        const type = [UnitType.City, UnitType.Port, UnitType.Factory][n[0]];
        if (game.owner(n[1]) !== player) player.conquer(n[1]); // structures on foreign land are removed by the game
        const u = player.buildUnit(type, n[1], {});
        units.push(u);
        game.addExecution(new [cityMod.CityExecution, portMod.PortExecution, facMod.FactoryExecution][n[0]](u));
        for (let i = 0; i < 4; i++) tick();
      } else if (t[0] === "T") {
        const sm = game.railNetwork().stationManager();
        const a = sm.findStation(units[n[0]]), b = sm.findStation(units[n[1]]);
        const p = a && b ? game.railNetwork().findStationsPath(a, b) : [];
        emit(`T ${qi} ${p.length} ${p.map((s: any) => unitIdx(s.unit)).join(" ")}`);
      } else if (t[0] === "V") {
        const sm = game.railNetwork().stationManager();
        const a = sm.findStation(units[n[0]]), b = sm.findStation(units[n[1]]);
        if (!a || !b) { emit(`V ${qi} NONE`); return; }
        if (process.env.DEBUG_TRAIN) console.error("alive", player.isAlive(), player.canBuild(UnitType.Train, a.tile()), a.tile(), game.isLand(a.tile()));
        game.addExecution(new trainMod.TrainExecution(game.railNetwork(), player, a, b, n[2]));
        const rows: string[] = [];
        for (let i = 0; i < 3000; i++) {
          tick();
          const tu = trainUnits();
          if (tu.length === 0) {
            if (rows.length === 0 && i < 3) continue; // execution is initialised at the end of the next tick
            rows.push(rows.length === 0 ? "NONE" : "END");
            break;
          }
          rows.push(tu.map((u: any) => u.tile()).join(","));
        }
        emit(`V ${qi} ${rows.join(" ")}`);
      } else if (t[0] === "D") {
        const sts = stationUnits();
        const rails = new Map<number, any>();
        for (const s of sts) if (s) for (const r of s.getRailroads()) rails.set(r.id, r);
        for (const id of [...rails.keys()].sort((x, y) => x - y)) {
          const r = rails.get(id);
          emit(`RAIL ${id} ${unitIdx(r.from.unit)} ${unitIdx(r.to.unit)} ${r.tiles.length} ${r.tiles.join(" ")}`);
        }
        units.forEach((u, i) => {
          const s = sts[i];
          if (!s) { emit(`ST ${i} none`); return; }
          const c = s.getCluster();
          const nb = s.neighbors().map((x: any) => unitIdx(x.unit));
          emit(`ST ${i} nb ${nb.join(" ")} trade ${c ? [...(c as any).tradeStations].map((x: any) => unitIdx(x.unit)).join(" ") : "null"}`);
        });
      } else throw new Error("bad query " + line);
    });
  }

  fs.writeFileSync(arg("--out") ?? "/dev/stdout", out.join("\n") + "\n");
}

main().catch((e) => {
  console.error(e);
  process.exit(1);
});

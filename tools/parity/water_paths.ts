/*
 * water_paths.ts - reference output for the C water pathfinder (pathfind_ts.c).
 *
 * Builds a real OpenFront game on a map (one idle player) and runs its water
 * pathfinder (WaterPathFinder: HPA* + smoothing + shore coercing + mini-map
 * upscaling, and the stepper's next()) on random queries. Writes the queries
 * and the tile sequences TS returns to a text file that
 * tools/parity/water_paths_c.c replays and compares.
 *
 *   cd <openfront checkout>
 *   node_modules/.bin/tsx <repo>/tools/parity/water_paths.ts --ref . --map world --size compact \
 *       --queries 3000 --seed 1 --out out/world_compact.txt
 *
 * File format (one record = query line, then result line starting with "="):
 *   M <w> <h> <mw> <mh> <nodes>          header (game and mini map size, abstract nodes)
 *   P <from> <to>                        findPath(from, to)
 *   Q <k> <f1..fk> <to>                  findPath([f1..fk], to)
 *   V <from> <to> <dist|-1> <max> <nev> [<tick> <D|T|I> <tile>]*   a voyage of next() calls
 *   = <n> <tiles...>                     path (n = -1: null); for V: <n> then n pairs <status> <node>
 *   statuses: 0 NEXT, 2 COMPLETE, 3 NOT_FOUND (node -1)
 * The OpenFront code is loaded at runtime from --ref; nothing of it is copied here.
 */
import fs from "node:fs";
import path from "node:path";
import { pathToFileURL } from "node:url";

const arg = (n: string, d?: string) => {
  const i = process.argv.indexOf(n);
  return i >= 0 ? process.argv[i + 1] : d;
};
const REF = path.resolve(arg("--ref") ?? process.env.OPENFRONT_DIR ?? ".");
const mapName = arg("--map", "world")!;
const sizeName = arg("--size", "compact")!;
const nQueries = Number(arg("--queries", "2000"));
const seed = Number(arg("--seed", "1"));
const outPath = arg("--out");
console.log = (...a: unknown[]) => console.error(...a);
console.debug = console.warn = console.info = () => {};
const imp = (rel: string): Promise<any> => import(pathToFileURL(path.join(REF, rel)).href);

function mulberry32(a: number) {
  return () => {
    a = (a + 0x6d2b79f5) | 0;
    let t = Math.imul(a ^ (a >>> 15), 1 | a);
    t = (t + Math.imul(t ^ (t >>> 7), 61 | t)) ^ t;
    return ((t ^ (t >>> 14)) >>> 0) / 4294967296;
  };
}

async function main() {
  const types = await imp("packages/engine-api/src/game/GameTypes.ts");
  const runnerMod = await imp("packages/engine/src/GameRunner.ts");
  const shared = await imp("packages/shared/src/GameMapLoader.ts");
  const pfMod = await imp("packages/engine/src/pathfinding/PathFinder.ts");
  const { GameType, GameMapType, GameMapSize, GameMode, Difficulty } = types;
  const mapKey = Object.keys(GameMapType).find((k) => k.toLowerCase() === mapName.toLowerCase())!;
  const gameMap = GameMapType[mapKey];
  const gameMapSize = sizeName === "normal" ? GameMapSize.Normal : GameMapSize.Compact;
  const gameStart = {
    gameID: "WATERPTH",
    lobbyCreatedAt: 0,
    config: {
      gameMap, gameMapSize, gameMode: GameMode.FFA, gameType: GameType.Private,
      difficulty: Difficulty.Medium, nations: "disabled", donateGold: false, donateTroops: false,
      bots: 0, infiniteGold: false, infiniteTroops: false, instantBuild: false, randomSpawn: false,
    },
    players: [{ clientID: "CLIENT01", username: "p1", clanTag: null, isLobbyCreator: true }],
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
  const runner = await runnerMod.createGameRunner(
    gameStart, undefined, await shared.loadMapFiles(loader, gameMap, gameMapSize), () => {},
  );
  const game = runner.game;
  const map = game.map();
  const W = map.width(), H = map.height(), N = W * H;
  const mini = game.miniMap();
  const nodes = game.miniWaterGraph()?.nodeCount ?? -1;

  const rnd = mulberry32(seed);
  const ri = (n: number) => Math.floor(rnd() * n);
  const shores: number[] = [], waters: number[] = [], lands: number[] = [];
  for (let t = 0; t < N; t++) {
    if (map.isWater(t)) waters.push(t);
    else {
      lands.push(t);
      if (map.isShore(t)) shores.push(t);
    }
  }
  const pick = (a: number[]) => a[ri(a.length)];
  const near = (a: number[], t: number, r: number) => {
    for (let i = 0; i < 400; i++) {
      const c = pick(a);
      if (c !== t && map.manhattanDist(c, t) <= r) return c;
    }
    return pick(a);
  };
  // A water, shore or (rarely) inland tile: the endpoints ships use are ports,
  // landing tiles and warship targets.
  const endpoint = () => {
    const r = rnd();
    return r < 0.55 ? pick(shores) : r < 0.95 ? pick(waters) : pick(lands);
  };

  const lines: string[] = [];
  lines.push(`M ${W} ${H} ${mini.width()} ${mini.height()} ${nodes}`);
  let tsMs = 0, tsPaths = 0;
  const wpf = new pfMod.WaterPathFinder(game);
  const runPath = (from: number | number[], to: number): number[] | null => {
    const t0 = performance.now();
    const p = wpf.findPath(from, to);
    tsMs += performance.now() - t0;
    tsPaths++;
    return p === null ? null : Array.from(p as ArrayLike<number>);
  };
  const fmt = (p: number[] | null) => (p === null ? "= -1" : `= ${p.length} ${p.join(" ")}`);

  for (let q = 0; q < nQueries; q++) {
    const kind = rnd();
    if (kind < 0.5) {
      // Point to point. Distance class: adjacent, short, medium, any.
      const dc = ri(4);
      const a = endpoint();
      let b: number;
      if (dc === 0) b = near(rnd() < 0.5 ? shores : waters, a, 4);
      else if (dc === 1) b = near(rnd() < 0.5 ? shores : waters, a, 40);
      else if (dc === 2) b = near(rnd() < 0.7 ? shores : waters, a, 250);
      else b = endpoint();
      lines.push(`P ${a} ${b}`);
      lines.push(fmt(runPath(a, b)));
    } else if (kind < 0.62) {
      // Multi-source (closest shore by water).
      const k = 2 + ri(7);
      const to = rnd() < 0.5 ? pick(waters) : pick(shores);
      const srcs: number[] = [];
      const radius = [60, 200, 100000][ri(3)];
      for (let i = 0; i < k; i++) srcs.push(near(shores, to, radius));
      lines.push(`Q ${k} ${srcs.join(" ")} ${to}`);
      lines.push(fmt(runPath(srcs, to)));
    } else if (kind < 0.64) {
      // Invalid refs.
      const a = rnd() < 0.5 ? N + ri(5) : -1 - ri(3);
      const b = pick(waters);
      lines.push(`P ${a} ${b}`);
      lines.push(fmt(runPath(a, b)));
    } else {
      // A voyage of next() calls, optionally disturbed.
      const a = rnd() < 0.7 ? pick(shores) : pick(waters);
      const b = rnd() < 0.5 ? near(shores, a, [60, 300, 100000][ri(3)]) : endpoint();
      const dist = rnd() < 0.3 ? 1 + ri(8) : -1;
      const max = 3000;
      const evs: string[] = [];
      let nev = 0;
      if (rnd() < 0.4) {
        nev = 1 + ri(3);
        let tick = 0;
        for (let i = 0; i < nev; i++) {
          tick += 1 + ri(60);
          const k = ri(3);
          if (k === 0) evs.push(`${tick} D ${endpoint()}`);
          else if (k === 1) evs.push(`${tick} T ${pick(waters)}`);
          else evs.push(`${tick} I 0`);
        }
      }
      lines.push(`V ${a} ${b} ${dist} ${max} ${nev}${nev ? " " + evs.join(" ") : ""}`);
      const pf = new pfMod.WaterPathFinder(game);
      let pos = a, dst = b;
      const evAt = new Map<number, [string, number]>();
      for (const e of evs) {
        const [t, k, v] = e.split(" ");
        evAt.set(Number(t), [k, Number(v)]);
      }
      const steps: number[] = [];
      for (let tick = 0; tick < max; tick++) {
        const e = evAt.get(tick);
        if (e) {
          if (e[0] === "D") dst = e[1];
          else if (e[0] === "T") pos = e[1];
          else pf.invalidate();
        }
        const t0 = performance.now();
        const r = pf.next(pos, dst, dist >= 0 ? dist : undefined);
        tsMs += performance.now() - t0;
        steps.push(r.status, r.node ?? -1);
        if (r.status === 0) pos = r.node;
        else break;
      }
      lines.push(`= ${steps.length / 2} ${steps.join(" ")}`);
    }
  }
  fs.writeFileSync(outPath ?? "/dev/stdout", lines.join("\n") + "\n");
  console.error(`${mapName}/${sizeName}: ${W}x${H} mini ${mini.width()}x${mini.height()} nodes=${nodes} ` +
    `queries=${nQueries} ts_path_calls=${tsPaths} ts_ms=${tsMs.toFixed(0)}`);
}
main().catch((e) => { console.error(e); process.exit(1); });

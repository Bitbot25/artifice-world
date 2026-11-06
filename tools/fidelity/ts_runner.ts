/*
 * ts_runner.ts - runs a scenario headlessly on the real OpenFront TypeScript
 * engine and writes JSON lines (see README.md for the schema).
 *
 * The OpenFront checkout (AGPL) is NOT part of this repository. It is loaded
 * at runtime from --ref <dir> (default: $OPENFRONT_DIR or third_party/openfront).
 * Run it with the checkout's own toolchain, from inside the checkout:
 *
 *   cd third_party/openfront
 *   npx tsx /path/to/tools/fidelity/ts_runner.ts --ref . --scenario s.json [--out o.jsonl]
 *   npx tsx /path/to/tools/fidelity/ts_runner.ts --ref . --dump-state d.json  ... (see ts_dump_state.ts)
 *
 * fidelity.py / compare.py do this for you (ts_run()).
 */
import fs from "node:fs";
import path from "node:path";
import { pathToFileURL } from "node:url";

// ---------------------------------------------------------------------------
// CLI
// ---------------------------------------------------------------------------

function arg(name: string, def?: string): string | undefined {
  const i = process.argv.indexOf(name);
  return i >= 0 ? process.argv[i + 1] : def;
}

const REF = path.resolve(
  arg("--ref") ?? process.env.OPENFRONT_DIR ?? "third_party/openfront",
);
const scenarioPath = arg("--scenario");
const outPath = arg("--out");
if (!scenarioPath) {
  console.error("usage: ts_runner.ts --ref <openfront dir> --scenario <file.json> [--out <file>]");
  process.exit(2);
}

// Engine chatter goes to stderr so stdout stays clean JSON lines.
console.log = (...a: unknown[]) => console.error(...a);
console.debug = () => {};
console.warn = () => {};
console.info = () => {};

const outStream: { write(s: string): unknown } = outPath
  ? { write: ((fd) => (s: string) => fs.writeSync(fd, s))(fs.openSync(outPath, "w")) }
  : { write: (s: string) => process.stdout.write(s) };
function emit(o: unknown) {
  outStream.write(JSON.stringify(o) + "\n");
}

async function imp(rel: string): Promise<any> {
  return import(pathToFileURL(path.join(REF, rel)).href);
}

// ---------------------------------------------------------------------------
// Scenario helpers (shared schema with c_runner.py)
// ---------------------------------------------------------------------------

type Scenario = {
  name: string;
  map: string;
  map_size?: "compact" | "normal";
  difficulty?: string;
  humans?: number;
  nations?: number | "default" | "disabled";
  bots?: number;
  spawns?: Record<string, [number, number]>;
  setup?: any[];
  intents?: any[];
  ticks: number;
  sample_every?: number;
  seeds: number[];
  sample_players?: "all" | "humans";
  starting_gold?: number;
  track_boats?: boolean;
  setup_delay?: number;
};

function inShape(op: any, x: number, y: number): boolean {
  if (op.shape === "rect") return x >= op.x0 && x < op.x1 && y >= op.y0 && y < op.y1;
  if (op.shape === "disc") return (x - op.cx) ** 2 + (y - op.cy) ** 2 <= op.r * op.r;
  throw new Error(`unknown shape ${op.shape}`);
}

function bbox(op: any): [number, number, number, number] {
  if (op.shape === "rect") return [op.x0, op.y0, op.x1 - 1, op.y1 - 1];
  return [op.cx - op.r, op.cy - op.r, op.cx + op.r, op.cy + op.r];
}

// ---------------------------------------------------------------------------
// One game
// ---------------------------------------------------------------------------

function applyJitter(sc: any, idx: number): any {
  const jit = sc.jitter;
  if (!jit || jit.length === 0) return sc;
  const [dx, dy] = jit[idx % jit.length];
  const out = JSON.parse(JSON.stringify(sc));
  out.spawns = {};
  for (const [k, xy] of Object.entries(sc.spawns ?? {}) as any) out.spawns[k] = [xy[0] + dx, xy[1] + dy];
  for (const op of out.setup ?? []) {
    if (op.shape === "rect") {
      op.x0 += dx; op.x1 += dx; op.y0 += dy; op.y1 += dy;
    } else if (op.shape === "disc") {
      op.cx += dx; op.cy += dy;
    }
  }
  for (const it of out.intents ?? []) if ("x" in it) { it.x += dx; it.y += dy; }
  return out;
}

async function runOne(sc0: Scenario, seed: number, mods: any) {
  const sc: Scenario = applyJitter(sc0, sc0.seeds.indexOf(seed));
  const { GameType, GameMapType, GameMapSize, GameMode, Difficulty, PlayerType, UnitType } = mods.types;
  const mapKey = Object.keys(GameMapType).find((k) => k.toLowerCase() === sc.map.toLowerCase());
  if (!mapKey) throw new Error(`unknown map ${sc.map}`);
  const gameMap = GameMapType[mapKey];
  const gameMapSize = (sc.map_size ?? "compact") === "normal" ? GameMapSize.Normal : GameMapSize.Compact;
  const diffKey = Object.keys(Difficulty).find((k) => k.toLowerCase() === (sc.difficulty ?? "medium").toLowerCase());
  const nHumans = sc.humans ?? 0;
  const gameID = `fid-${sc.name}-${seed}`;
  const clientIDs = Array.from({ length: nHumans }, (_, i) => `HUMAN${String(i + 1).padStart(3, "0")}`);

  const gameStart = {
    gameID,
    lobbyCreatedAt: 0,
    config: {
      gameMap,
      gameMapSize,
      gameMode: GameMode.FFA,
      gameType: GameType.Private,
      difficulty: Difficulty[diffKey!],
      nations: sc.nations ?? "disabled",
      donateGold: false,
      donateTroops: false,
      bots: sc.bots ?? 0,
      infiniteGold: false,
      infiniteTroops: false,
      instantBuild: false,
      randomSpawn: false,
      ...(sc.starting_gold ? { startingGold: sc.starting_gold } : {}),
    },
    players: clientIDs.map((clientID, i) => ({
      clientID,
      username: `fid${i + 1}`,
      clanTag: null,
      isLobbyCreator: i === 0,
    })),
  };

  const loader = {
    getMapData: (map: any) => {
      const key = Object.keys(GameMapType).find((k) => GameMapType[k] === map)!.toLowerCase();
      const dir = path.join(REF, "resources/maps", key);
      const bin = (n: string) => async () => new Uint8Array(fs.readFileSync(path.join(dir, n)));
      return {
        mapBin: bin("map.bin"),
        map4xBin: bin("map4x.bin"),
        map16xBin: bin("map16x.bin"),
        manifest: async () => JSON.parse(fs.readFileSync(path.join(dir, "manifest.json"), "utf8")),
        webpPath: "",
        layerImage: async () => {
          throw new Error("no layers");
        },
      };
    },
  };

  let fatal: string | undefined;
  const runner = await mods.createGameRunner(
    gameStart,
    undefined,
    await mods.loadMapFiles(loader, gameMap, gameMapSize),
    (gu: any) => {
      if ("errMsg" in gu) fatal = `${gu.errMsg}\n${gu.stack ?? ""}`;
    },
  );
  const game = runner.game;
  const W = game.width();
  const H = game.height();
  const tileOf = (x: number, y: number) => game.ref(x, y);

  // ---- instrumentation ----------------------------------------------------
  type AttackRec = {
    id: string;
    t0: number;
    attacker: number;
    target: number;
    kind: string;
    troops0: number;
    tiles: number;
    att_tiles0: number;
    def_tiles0: number;
    def_troops0: number;
    src: number | null;
    sx: number;
    sy: number;
  };
  const active = new Map<any, AttackRec>();
  const conquerCount = new Map<string, number>(); // "att:prev" -> n (this window)
  const origConquer = game.conquer.bind(game);
  game.conquer = (owner: any, tile: number) => {
    const prev = game.owner(tile);
    const key = `${owner.smallID()}:${prev.isPlayer() ? prev.smallID() : 0}`;
    for (const [, rec] of active) {
      if (`${rec.attacker}:${rec.target}` === key) {
        rec.tiles++;
        break;
      }
    }
    return origConquer(owner, tile);
  };
  const PlayerImpl = mods.PlayerImpl;
  const origCreate = PlayerImpl.prototype.createAttack;
  PlayerImpl.prototype.createAttack = function (target: any, troops: number, sourceTile: number | null, border: any) {
    const a = origCreate.call(this, target, troops, sourceTile, border);
    const tgt = target.isPlayer() ? target : null;
    const rec: AttackRec = {
      id: a.id(),
      t0: game.ticks(),
      attacker: this.smallID(),
      target: tgt ? tgt.smallID() : 0,
      kind: sourceTile === null ? "land" : "boat",
      troops0: troops,
      tiles: 0,
      att_tiles0: this.numTilesOwned(),
      def_tiles0: tgt ? tgt.numTilesOwned() : 0,
      def_troops0: tgt ? tgt.troops() : 0,
      src: sourceTile,
      sx: sourceTile === null ? -1 : game.x(sourceTile),
      sy: sourceTile === null ? -1 : game.y(sourceTile),
    };
    active.set(a, rec);
    const origDelete = a.delete.bind(a);
    const attacker = this;
    a.delete = () => {
      if (active.has(a)) {
        const r = active.get(a)!;
        active.delete(a);
        const tg = target.isPlayer() ? target : null;
        emit({
          ev: "attack",
          seed,
          t0: r.t0 - T0,
          t1: game.ticks() - T0,
          attacker: r.attacker,
          target: r.target,
          kind: r.kind,
          troops0: r.troops0,
          troops_end: a.troops(),
          tiles: r.tiles,
          att_tiles0: r.att_tiles0,
          att_tiles1: attacker.numTilesOwned(),
          def_tiles0: r.def_tiles0,
          def_tiles1: tg ? tg.numTilesOwned() : 0,
          def_troops0: r.def_troops0,
          def_troops1: tg ? tg.troops() : 0,
          src: r.src === null ? null : [r.sx, r.sy],
        });
      }
      return origDelete();
    };
    return a;
  };

  // ---- spawn phase ----------------------------------------------------------
  let turn = 0;
  const humanPlayer = (i: number) => game.playerByClientID(clientIDs[i - 1]);
  const stampSpawns = () => {
    const intents: any[] = [];
    for (const [k, xy] of Object.entries(sc.spawns ?? {})) {
      const i = Number(k);
      intents.push({ type: "spawn", tile: tileOf(xy[0], xy[1]), clientID: clientIDs[i - 1] });
    }
    return intents;
  };
  const tick = (intents: any[]) => {
    runner.addTurn({ turnNumber: turn++, intents });
    if (!runner.executeNextTick() || fatal) throw new Error(`tick failed: ${fatal}`);
  };
  tick(stampSpawns());
  let guard = 0;
  while (game.inSpawnPhase()) {
    tick([]);
    if (++guard > 5000) throw new Error("spawn phase did not end");
  }
  // PlayerExecution only starts its territory checks once a tile changed after
  // its own start tick (spawn end + up to 19), so scenarios with setup edits wait
  // `setup_delay` ticks before applying them (default 25).
  const delay = sc.setup_delay ?? ((sc.setup ?? []).length > 0 ? 25 : 0);
  for (let i = 0; i < delay; i++) tick([]);
  const T0 = game.ticks();

  // ---- setup ops --------------------------------------------------------------
  const resolvePlayer = (spec: number | string) => {
    if (typeof spec === "number") return humanPlayer(spec);
    if (spec.startsWith("p:")) return game.playerBySmallID(Number(spec.slice(2)));
    throw new Error(`bad player spec ${spec}`);
  };
  for (const op of sc.setup ?? []) {
    if (op.op === "clear") {
      const list = op.player === "all" ? clientIDs.map((_, i) => humanPlayer(i + 1)) : [resolvePlayer(op.player)];
      for (const p of list) for (const t of [...p.tiles()]) p.relinquish(t);
    } else if (op.op === "claim") {
      const p = resolvePlayer(op.player);
      const [bx0, by0, bx1, by1] = bbox(op);
      for (let y = Math.max(0, by0); y <= Math.min(H - 1, by1); y++) {
        for (let x = Math.max(0, bx0); x <= Math.min(W - 1, bx1); x++) {
          if (!inShape(op, x, y)) continue;
          const t = tileOf(x, y);
          if (!game.isLand(t) || game.isImpassable(t)) continue;
          if (op.only_free && game.hasOwner(t)) continue;
          p.conquer(t);
        }
      }
    } else if (op.op === "troops") {
      resolvePlayer(op.player).setTroops(op.value);
    } else if (op.op === "gold") {
      const p = resolvePlayer(op.player);
      p.removeGold(p.gold());
      p.addGold(BigInt(op.value));
    } else {
      throw new Error(`unknown setup op ${op.op}`);
    }
  }

  emit({
    ev: "meta",
    engine: "ts",
    scenario: sc.name,
    seed,
    width: W,
    height: H,
    t_spawn_end: T0,
    num_players: game.allPlayers().length,
    spawn_phase_turns: game.config().numSpawnPhaseTurns(),
  });

  // ---- main loop ----------------------------------------------------------------
  const typeChar = (t: any) => (t === PlayerType.Human ? "h" : t === PlayerType.Nation ? "n" : "b");
  const sampleEvery = sc.sample_every ?? 10;
  const sample = (t: number) => {
    const rows = game
      .allPlayers()
      .filter((p: any) => sc.sample_players !== "humans" || p.type() === PlayerType.Human)
      .map((p: any) => [p.smallID(), typeChar(p.type()), p.numTilesOwned(), p.troops(), Number(p.gold()), p.isAlive() ? 1 : 0]);
    emit({ ev: "sample", seed, t, p: rows });
  };
  const boats = new Map<number, any>();
  const boatTracker = (t: number) => {
    const seen = new Set<number>();
    for (const u of game.units(UnitType.TransportShip)) {
      seen.add(u.id());
      let b = boats.get(u.id());
      if (!b) {
        const dst = u.targetTile();
        b = {
          t0: t,
          player: u.owner().smallID(),
          troops: u.troops(),
          src: [game.x(u.tile()), game.y(u.tile())],
          dst: dst == null ? null : [game.x(dst), game.y(dst)],
        };
        boats.set(u.id(), b);
      }
      b.last = [game.x(u.tile()), game.y(u.tile())];
      b.t_last = t;
    }
    for (const [id, b] of boats) {
      if (seen.has(id)) continue;
      boats.delete(id);
      const d = b.dst ? Math.hypot(b.dst[0] - b.src[0], b.dst[1] - b.src[1]) : null;
      const dl = b.dst ? Math.abs(b.last[0] - b.dst[0]) + Math.abs(b.last[1] - b.dst[1]) : null;
      emit({
        ev: "boat",
        seed,
        t0: b.t0,
        t1: t,
        player: b.player,
        troops: b.troops,
        src: b.src,
        dst: b.dst,
        last: b.last,
        dist: d,
        arrived: dl !== null && dl <= 2,
      });
    }
  };

  const intentsFor = (k: number) => {
    const out: any[] = [];
    for (const it of sc.intents ?? []) {
      const t0 = it.tick ?? 0;
      let hit = k === t0;
      if (!hit && it.every && k > t0 && (!it.until || k <= it.until) && (k - t0) % it.every === 0) hit = true;
      if (!hit) continue;
      const clientID = clientIDs[it.player - 1];
      const me = humanPlayer(it.player);
      if (!me || !me.isAlive()) continue;
      if (it.type === "attack") {
        let targetID: string | null = null;
        if (it.target) targetID = resolvePlayer(it.target).id();
        let troops = it.troops ?? null;
        if (it.frac != null) troops = Math.floor(me.troops() * it.frac);
        out.push({ type: "attack", targetID, troops, clientID });
      } else if (it.type === "boat") {
        let troops = it.troops ?? null;
        if (it.frac != null) troops = Math.floor(me.troops() * it.frac);
        out.push({ type: "boat", dst: tileOf(it.x, it.y), troops, clientID });
      } else if (it.type === "build") {
        out.push({ type: "build_unit", unit: UnitType[it.unit], tile: tileOf(it.x, it.y), clientID });
      } else if (it.type === "cancel_attack") {
        const a = me.outgoingAttacks()[0];
        if (a) out.push({ type: "cancel_attack", attackID: a.id(), clientID });
      } else {
        throw new Error(`unsupported intent ${it.type}`);
      }
    }
    return out;
  };

  let k = 0;
  for (; k < sc.ticks; k++) {
    if (k % sampleEvery === 0) sample(k);
    tick(intentsFor(k));
    if (sc.track_boats !== false) boatTracker(k + 1);
    if (game.getWinner() !== null) {
      k++;
      break;
    }
  }
  sample(k);
  // Attacks still running at the end.
  for (const [a, r] of active) {
    emit({
      ev: "attack_open",
      seed,
      t0: r.t0 - T0,
      attacker: r.attacker,
      target: r.target,
      kind: r.kind,
      troops0: r.troops0,
      troops_now: a.troops(),
      tiles: r.tiles,
    });
  }
  const w = game.getWinner();
  emit({
    ev: "end",
    seed,
    t: k,
    over: w !== null,
    winner: w !== null && typeof w !== "string" ? w.smallID() : null,
    winner_team: w !== null && typeof w === "string" ? w : null,
  });
  PlayerImpl.prototype.createAttack = origCreate;
}

async function main() {
  const sc: Scenario = JSON.parse(fs.readFileSync(scenarioPath!, "utf8"));
  const types = await imp("packages/engine-api/src/game/GameTypes.ts");
  const runnerMod = await imp("packages/engine/src/GameRunner.ts");
  const shared = await imp("packages/shared/src/GameMapLoader.ts");
  const pimpl = await imp("packages/engine/src/game/PlayerImpl.ts");
  const mods = {
    types,
    createGameRunner: runnerMod.createGameRunner,
    loadMapFiles: shared.loadMapFiles,
    PlayerImpl: pimpl.PlayerImpl,
  };
  for (const seed of sc.seeds) {
    await runOne(sc, seed, mods);
  }
}

main().catch((e) => {
  console.error(e);
  process.exit(1);
});

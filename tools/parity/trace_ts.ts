/*
 * trace_ts.ts - runs a parity scenario on the OpenFront TS engine and writes
 * the per-tick digest of the observable state (same format as trace_c.py;
 * the digest is defined in digest.py and must stay identical to it).
 *
 *   cd <openfront checkout>
 *   node_modules/.bin/tsx <repo>/tools/parity/trace_ts.ts --ref . --scenario s.json --out ts.jsonl [--detail 120,121]
 *
 * The game is driven like a real one: GameRunner, one turn per tick, intents
 * stamped with the player's clientID. The OpenFront code is loaded at
 * runtime from --ref; nothing of it is copied here.
 */
import fs from "node:fs";
import path from "node:path";
import { pathToFileURL } from "node:url";

const arg = (n: string, d?: string) => {
  const i = process.argv.indexOf(n);
  return i >= 0 ? process.argv[i + 1] : d;
};
const REF = path.resolve(arg("--ref") ?? process.env.OPENFRONT_DIR ?? "third_party/openfront");
const scenarioPath = arg("--scenario");
const outPath = arg("--out");
const detail = new Set((arg("--detail") ?? "").split(",").filter(Boolean).map(Number));
if (!scenarioPath) {
  console.error("usage: trace_ts.ts --ref <openfront> --scenario <file.json> [--out f] [--detail t1,t2]");
  process.exit(2);
}
console.log = (...a: unknown[]) => console.error(...a);
console.debug = console.warn = console.info = () => {};
const imp = (rel: string): Promise<any> => import(pathToFileURL(path.join(REF, rel)).href);
const out = outPath
  ? ((fd) => (s: string) => fs.writeSync(fd, s))(fs.openSync(outPath, "w"))
  : (s: string) => process.stdout.write(s);

// ---- digest (mirror of digest.py) -------------------------------------------

function fmix32(h: number): number {
  h ^= h >>> 16;
  h = Math.imul(h, 0x85ebca6b);
  h ^= h >>> 13;
  h = Math.imul(h, 0xc2b2ae35);
  return (h ^ (h >>> 16)) >>> 0;
}

function tileHash(n: number, value: (t: number) => number): number {
  let sum = 0;
  for (let t = 0; t < n; t++) {
    const o = value(t);
    if (o === 0) continue;
    const k = (Math.imul(t, 0x9e3779b1) + Math.imul(o, 0x85ebca77)) | 0;
    sum = (sum + fmix32(k)) >>> 0;
  }
  return sum;
}

const CRC_TABLE = (() => {
  const t = new Uint32Array(256);
  for (let i = 0; i < 256; i++) {
    let c = i;
    for (let k = 0; k < 8; k++) c = c & 1 ? 0xedb88320 ^ (c >>> 1) : c >>> 1;
    t[i] = c >>> 0;
  }
  return t;
})();

function crc32(s: string): number {
  const b = Buffer.from(s, "utf8");
  let c = 0xffffffff;
  for (let i = 0; i < b.length; i++) c = CRC_TABLE[(c ^ b[i]) & 0xff] ^ (c >>> 8);
  return (c ^ 0xffffffff) >>> 0;
}

const cmpRows = (a: number[], b: number[]) => {
  for (let i = 0; i < Math.min(a.length, b.length); i++) if (a[i] !== b[i]) return a[i] - b[i];
  return a.length - b.length;
};

// ---- game setup ---------------------------------------------------------------

async function main() {
  const sc = JSON.parse(fs.readFileSync(scenarioPath!, "utf8"));
  const types = await imp("packages/engine-api/src/game/GameTypes.ts");
  const runnerMod = await imp("packages/engine/src/GameRunner.ts");
  const shared = await imp("packages/shared/src/GameMapLoader.ts");
  const { GameType, GameMapType, GameMapSize, GameMode, Difficulty, UnitType } = types;
  const UNIT_ORDER: string[] = Object.values(UnitType); // == OfeUnitType order
  const mapKey = Object.keys(GameMapType).find((k) => k.toLowerCase() === sc.map.toLowerCase())!;
  const gameMap = GameMapType[mapKey];
  const gameMapSize = (sc.map_size ?? "compact") === "normal" ? GameMapSize.Normal : GameMapSize.Compact;
  const diffKey = Object.keys(Difficulty).find((k) => k.toLowerCase() === (sc.difficulty ?? "medium").toLowerCase())!;
  const nHumans: number = sc.humans ?? 0;
  const clientIDs = Array.from({ length: nHumans }, (_, i) => `CLIENT${String(i + 1).padStart(2, "0")}`);

  const gameStart = {
    gameID: sc.game_id,
    lobbyCreatedAt: 0,
    config: {
      gameMap,
      gameMapSize,
      gameMode: GameMode.FFA,
      gameType: GameType.Private,
      difficulty: Difficulty[diffKey],
      nations: sc.nations ?? "disabled",
      donateGold: false,
      donateTroops: false,
      bots: sc.bots ?? 0,
      infiniteGold: false,
      infiniteTroops: false,
      instantBuild: false,
      randomSpawn: false,
      ...(sc.ts_config ?? {}),
    },
    players: clientIDs.map((clientID, i) => ({
      clientID,
      username: `player${i + 1}`,
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
  const runner = await runnerMod.createGameRunner(
    gameStart,
    undefined,
    await shared.loadMapFiles(loader, gameMap, gameMapSize),
    (gu: any) => {
      if ("errMsg" in gu) fatal = `${gu.errMsg}\n${gu.stack ?? ""}`;
    },
  );
  const game = runner.game;
  const n = game.width() * game.height();
  const map = game.map();

  // ---- intents -----------------------------------------------------------------
  const byTick = new Map<number, any[]>();
  for (const it of sc.intents ?? []) {
    if (!byTick.has(it.tick)) byTick.set(it.tick, []);
    byTick.get(it.tick)!.push(it);
  }
  const pid = (sid: number) => game.playerBySmallID(sid).id();
  const toTs = (it: any): any => {
    const clientID = clientIDs[it.player - 1];
    const tile = it.x !== undefined ? game.ref(it.x, it.y) : undefined;
    const troops = it.troops ?? null;
    switch (it.type) {
      case "spawn": return { type: "spawn", clientID, tile };
      case "attack": return { type: "attack", clientID, targetID: it.target ? pid(it.target) : null, troops };
      case "cancel_attack": {
        const me = game.playerBySmallID(it.player);
        const a = me.outgoingAttacks().find((x: any) => (x.target().isPlayer() ? x.target().smallID() : 0) === it.target);
        return a ? { type: "cancel_attack", clientID, attackID: a.id() } : null;
      }
      case "boat_attack": return { type: "boat", clientID, dst: tile, troops: troops ?? game.playerBySmallID(it.player).troops() / 5 };
      case "cancel_boat": return { type: "cancel_boat", clientID, unitID: it.unit };
      case "alliance_request": return { type: "allianceRequest", clientID, recipient: pid(it.target) };
      case "alliance_reject": return { type: "allianceReject", clientID, requestor: pid(it.target) };
      case "alliance_extension": return { type: "allianceExtension", clientID, recipient: pid(it.target) };
      case "break_alliance": return { type: "breakAlliance", clientID, recipient: pid(it.target) };
      case "target_player": return { type: "targetPlayer", clientID, target: pid(it.target) };
      case "donate_gold": return { type: "donate_gold", clientID, recipient: pid(it.target), gold: troops };
      case "donate_troops": return { type: "donate_troops", clientID, recipient: pid(it.target), troops };
      case "embargo": return { type: "embargo", clientID, targetID: pid(it.target), action: it.flag ? "start" : "stop" };
      case "embargo_all": return { type: "embargo_all", clientID, action: it.flag ? "start" : "stop" };
      case "build_unit": {
        const unit = UNIT_ORDER[Object.keys(UNIT_NAMES).indexOf(it.unit_type)];
        return { type: "build_unit", clientID, unit, tile, rocketDirectionUp: !!it.flag, amount: it.amount ?? 1 };
      }
      case "upgrade_structure": {
        const u = game.units().find((x: any) => x.id() === it.unit);
        return u ? { type: "upgrade_structure", clientID, unit: u.type(), unitId: it.unit, amount: it.amount ?? 1 } : null;
      }
      case "delete_unit": return { type: "delete_unit", clientID, unitId: it.unit };
      case "move_warship": return { type: "move_warship", clientID, unitIds: [it.unit], tile };
      case "mark_disconnected": return { type: "mark_disconnected", clientID, isDisconnected: !!it.flag };
      default: throw new Error(`unknown intent ${it.type}`);
    }
  };

  // ---- observation -----------------------------------------------------------------
  const observe = () => {
    const pl = game
      .allPlayers()
      .map((p: any) => [p.smallID(), Math.floor(p.troops()), Number(p.gold()), p.numTilesOwned()])
      .sort(cmpRows);
    const un = game
      .units()
      .map((u: any) => [
        u.id(), UNIT_ORDER.indexOf(u.type()), u.owner().smallID(), u.tile(), Math.floor(u.troops()),
        Math.floor(u.health()), u.level(), u.isUnderConstruction() ? 1 : 0,
      ])
      .sort(cmpRows);
    const at: number[][] = [];
    const al: number[][] = [];
    for (const p of game.allPlayers()) {
      for (const a of p.outgoingAttacks()) {
        at.push([p.smallID(), a.target().isPlayer() ? a.target().smallID() : 0, Math.floor(a.troops()), a.retreating() ? 1 : 0]);
      }
      for (const a of p.alliances()) {
        const x = a.requestor().smallID(), y = a.recipient().smallID();
        if (p.smallID() === Math.min(x, y)) al.push([Math.min(x, y), Math.max(x, y), a.expiresAt()]);
      }
    }
    at.sort(cmpRows);
    al.sort(cmpRows);
    const ex = game
      .allPlayers()
      .map((p: any) => {
        const tg = p.targets();
        return [p.smallID(), p.isDisconnected() ? 1 : 0, p.getTraitorRemainingTicks(), tg.length ? tg[tg.length - 1].smallID() : 0];
      })
      .sort(cmpRows);
    const w = game.getWinner();
    ex.unshift([0, w === null ? 0 : typeof w === "string" ? -1 : (w as any).smallID(), 0, 0]);
    const em: number[][] = [];
    for (const p of game.allPlayers()) for (const e of p.getEmbargoes()) em.push([p.smallID(), e.target.smallID()]);
    em.sort(cmpRows);
    const lists = { pl, un, at, al, ex, em };
    const h: Record<string, number> = {
      own: tileHash(n, (t) => map.ownerID(t)),
      fall: tileHash(n, (t) => (game.hasFallout(t) ? 1 : 0)),
    };
    for (const k of ["pl", "un", "at", "al", "ex", "em"]) h[k] = crc32(JSON.stringify((lists as any)[k]));
    return { lists, h };
  };

  let prevOwners = new Uint16Array(n);
  for (let t = 0; t < n; t++) prevOwners[t] = map.ownerID(t);
  for (let t = 0; t < sc.ticks; t++) {
    const intents = (byTick.get(t) ?? []).map(toTs).filter((x: any) => x !== null);
    runner.addTurn({ turnNumber: t, intents });
    if (!runner.executeNextTick() || fatal) throw new Error(`tick ${t} failed: ${fatal}`);
    const { lists, h } = observe();
    const rec: any = { t, h };
    if (detail.has(t)) {
      rec.d = lists;
      rec.tiles = [];
      for (let i = 0; i < n; i++) if (map.ownerID(i) !== prevOwners[i]) rec.tiles.push([i, map.ownerID(i)]);
    }
    if (detail.has(t) || detail.has(t + 1)) for (let i = 0; i < n; i++) prevOwners[i] = map.ownerID(i);
    out(JSON.stringify(rec) + "\n");
  }
}

// OfeUnitType names in enum order (python UNIT_TYPES).
const UNIT_NAMES: Record<string, number> = Object.fromEntries(
  [
    "transport_ship", "warship", "shell", "sam_missile", "port", "atom_bomb", "hydrogen_bomb", "trade_ship",
    "missile_silo", "defense_post", "sam_launcher", "city", "mirv", "mirv_warhead", "train", "factory",
  ].map((k, i) => [k, i]),
);

main().catch((e) => {
  console.error(e);
  process.exit(1);
});

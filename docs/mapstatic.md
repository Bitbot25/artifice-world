# Per-map static outputs (`mapstatic`)

Static, per-map features for the critic's feature extractor. They are a pure
function of the terrain bytes (`OfeMap`), independent of any game, computed
once at map load and cached on disk. The definitions below are a contract: the
same features are recomputed for states replayed in the TypeScript engine, so
any change to a definition bumps `OFE_MAPSTATIC_VERSION` (C) and
`MAPSTATIC_VERSION` (Python), which invalidates caches.

* C API: `engine/include/ofe_mapstatic.h`, implementation `engine/src/mapstatic.c`
* Python: `artifice_engine.mapstatic` (`compute`, `load_or_compute`)
* Version: **1**

Conventions: row-major; coarse arrays are `(ch, cw)` with `cell = cy * cw + cx`;
full-res arrays `(H, W)`; tile `(x, y)` = `y * W + x`. Labels start at 1, 0 means
"none". "Ownable land" = terrain class plains, highland or mountain.

## 1. Full-resolution terrain

OpenFront terrain byte: bit 7 land, bit 6 shoreline, bit 5 ocean, bits 0-4 magnitude.

| class | value | rule |
| --- | --- | --- |
| ocean | 0 | water (bit 7 clear) with the ocean bit |
| lake | 1 | water without the ocean bit |
| plains | 2 | land, magnitude < 10 |
| highland | 3 | land, 10 <= magnitude < 20 |
| mountain | 4 | land, 20 <= magnitude < 31 |
| impassable | 5 | land, magnitude 31 |

* `tclass` (uint8), `shoreline` (bit 6, 0/1), `magnitude` (bits 0-4).
* `attack_cost` (float32; Python only, derived from `tclass`): plains 16.5,
  highland 20, mountain 25 (`terrainAttackBase.tileCost` in `EngineConfig.ts`),
  +inf for water and impassable. `impassable` (uint8) = `tclass == 5`.
* `landmass` (int32): 4-connected components of **ownable** land (impassable
  tiles are not land here and split landmasses). 0 on non-ownable tiles.
* `water_body` (int32): 4-connected components of water of **equal class**
  (ocean and lake bodies never merge). 0 on land. `water_body_is_ocean[label]`
  (uint8, length `num_water_bodies + 1`) tells ocean from lake.
* Labels are numbered 1..n in raster order of each component's first tile.
  `num_landmasses`, `num_water_bodies`.

## 2. Coarse grid

* **Stride** `s` = smallest integer with `ceil(max(W, H) / s) <= 256`.
* Grid `cw = ceil(W / s)`, `ch = ceil(H / s)`. Cell `(cx, cy)` covers tiles
  `[cx*s, min(W, (cx+1)*s)) x [cy*s, min(H, (cy+1)*s))` (edge cells are smaller).
* `cell_land_tiles` / `cell_water_tiles` (int32): ownable-land and water tile counts.
* **`land_cost`** (float32) = mean attack tile cost over the cell's ownable land
  tiles; +inf if it has none.
* **`water_cost`** (float32) = `cell_tiles / water_tiles` = 1 / (water fraction)
  (1 for open water, larger for coastal cells); +inf if it has no water.
  Lakes and ocean both count as water.
* **Step passability** (uint8), separately for land and water, in four arrays
  each: `{land,water}_from_{left,right,up,down}`. `from_left[c] = 1` iff some
  full-res tile on the cell's left edge column and the tile directly left of it
  (in the left neighbour cell) are both ownable land (land graph) or both water of
  the same class (water graph). `from_right`, `from_up`, `from_down` likewise for
  the right edge, top edge, bottom edge. 0 on the map border. The relation is
  symmetric: `from_right[c] == from_left[c+1]`, `from_down[c] == from_up[c+cw]`.
  An edge exists in the coarse graph iff the flag is set (so both end cells have
  finite cost).
* **`multi_landmass`** (uint8): the cell contains ownable tiles of more than one
  landmass. `multi_water`: same for water bodies.
* **`cell_landmass` / `cell_water_body`** (int32): majority label among the cell's
  labelled tiles (ties -> smallest label), 0 if none.

## 3. Distances (exact, full resolution, pooled)

Metric: exact Euclidean distance between tile centres, in tiles (Felzenszwalb-
Huttenlocher; squared distances are integers, so the result is exact), float32.
Pooled per cell as the **min** and **mean** over the cell's tiles in the stated domain.
Cells without domain tiles, or maps without any source tile, get `+inf`.

| arrays | per-tile value | pooled over |
| --- | --- | --- |
| `coast_land_min/mean` | distance from a land tile to the nearest water tile | ownable land tiles |
| `coast_water_min/mean` | distance from a water tile to the nearest land tile (any land class) | water tiles |
| `imp_min/mean` | distance to the nearest impassable tile | ownable land tiles |

(The distance to a tile of the other type is 1 for tiles on the coast.)

## 4. Patches and distance tables

* **Patch** = a `k x k` block of coarse cells, `k = 4` (`OFE_PATCH_K`). Patch grid
  `pw = ceil(cw/4)`, `ph = ceil(ch/4)`, `num_patches = pw*ph`, patch id
  `by * pw + bx`. World (2000x1000): stride 8, 250x125 cells, 63x32 = **2016
  patches**; the spec's 16 MB = 2 tables x 2016^2 x 2 B. We store three
  such tables (land, water, coastal), 24 MB on World; other maps have 1.7k-4k
  patches (<= 4096, i.e. at most 33 MB per table).
* **Representatives**: `land_patch_rep[p]` = among the patch's cells with finite
  `land_cost`, the one nearest (cell coordinates) to the mean position of those
  cells, ties -> lowest cell index; -1 if none. `water_patch_rep` likewise with
  `water_cost`. A patch can have both, one or neither.
* **Graph and edge weights**: nodes are cells, edges are the step-passable
  4-adjacencies. Edge `a-b` weighs `s * (cost[a] + cost[b]) / 2` for water (tile
  units), and `s * (cost[a] + cost[b]) / 2 / 16.5` for land (plains-tile
  equivalents: a straight run of plains costs exactly its length in tiles).
* **`land_dist`, `water_dist`** (`(P, P)` float16, row = source patch): shortest
  path between the two representative cells (sum of edge weights), 0 on the
  diagonal for existing patches, +inf if unreachable or either patch has no
  representative. Values are float16 rounded to nearest even; finite values clamp
  at 65504 (never reached in practice: the longest World path is a few thousand).
* **Coastal patches** (boat attacks): `coastal_patch[p] = 1` iff the patch
  contains an ownable land tile 4-adjacent to an **ocean** tile (lake shores do not
  count). `coastal_anchor[p]` = among the cells holding those ocean neighbour
  tiles (of any such land tile), the one whose centre `((cx+.5)s, (cy+.5)s)` is
  nearest the centroid of the patch's coastal land tiles, ties -> lowest cell index;
  -1 for non-coastal patches. It may lie in a neighbouring patch.
* **`coastal_dist`** (`(P, P)` float16): shortest over-water path on the water
  graph between the anchors of two coastal patches (same weights as `water_dist`);
  +inf if either patch is not coastal or no ocean route exists; patches sharing an
  anchor have distance 0.

Search is Dijkstra per source patch (float32 accumulation), parallelised over
sources with OpenMP; each row is independent, so output is bit-identical for any
thread count.

## 5. Python and cache

```python
from artifice_engine import mapstatic
out = mapstatic.compute(map)                    # dict[str, np.ndarray]
out = mapstatic.load_or_compute(map, cache_dir)  # .npz cache
out["land_dist"]  # (P, P) float16, inf = unreachable
```

Cache file: `<cache_dir>/mapstatic-v<VERSION>-<sha256[:24]>.npz`, compressed
`np.savez_compressed`, written atomically (temp file + rename). The key hashes
`b"artifice-mapstatic\0"`, int32 `[VERSION, W, H]` and the raw terrain bytes. It
holds every array of `compute` except the derived `attack_cost` and `impassable`
(rebuilt from `tclass` on load). Unreadable or version-mismatched files are
recomputed. Scalars are stored as 0-d int32 arrays.

## 6. Known limitations

* Step passability is per edge, not per cell crossing: a cell is traversable if
  its left and right edges both open, even if the two open sides are separated
  inside the cell. A ridge or channel narrower than a cell can therefore be
  "crossed" in the coarse graph. `multi_landmass` / `multi_water` flag such cells.
* A patch's representative is one cell; a patch with several disconnected
  components of the same type is represented by one of them.
* Coarse distances are cell-centre to cell-centre along 4-neighbour moves
  (Manhattan-style, up to ~41% above Euclidean).
* `coastal_dist` counts only ocean routes; boats on lakes are not covered.

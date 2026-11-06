"""Per-map static outputs for the critic's feature extractor.

``compute(map)`` runs engine/src/mapstatic.c and returns a dict of numpy
arrays; ``load_or_compute(map, cache_dir)`` caches them as .npz keyed by a hash
of the terrain bytes and ``MAPSTATIC_VERSION``. The exact definitions of every
array are in engine/include/ofe_mapstatic.h and docs/mapstatic.md.

Array shapes (H, W full resolution; ch, cw coarse; P = num_patches):

    scalars (0-d int32)  version width height stride cw ch patch_k pw ph
                         num_patches num_landmasses num_water_bodies
    (H, W)               tclass shoreline magnitude landmass water_body
                         attack_cost (float32, inf if not ownable)
                         impassable (uint8)
    (ch, cw)             cell_land_tiles cell_water_tiles land_cost water_cost
                         {land,water}_from_{left,right,up,down}
                         multi_landmass multi_water cell_landmass cell_water_body
                         coast_land_{min,mean} coast_water_{min,mean} imp_{min,mean}
    (P,)                 land_patch_rep water_patch_rep coastal_patch coastal_anchor
    (P, P) float16       land_dist water_dist coastal_dist
    (num_water_bodies+1) water_body_is_ocean
"""

from __future__ import annotations

import ctypes as C
import hashlib
import os
from pathlib import Path
from typing import Dict

import numpy as np

from ._lib import lib as L

MAPSTATIC_VERSION = 1  # must equal OFE_MAPSTATIC_VERSION in ofe_mapstatic.h

TC_OCEAN, TC_LAKE, TC_PLAINS, TC_HIGHLAND, TC_MOUNTAIN, TC_IMPASSABLE = range(6)
#: attack tile cost per terrain class (inf where a tile cannot be owned)
ATTACK_COST = np.array([np.inf, np.inf, 16.5, 20.0, 25.0, np.inf], dtype=np.float32)

# Arrays recomputed from tclass instead of being stored in the cache.
_DERIVED = ("attack_cost", "impassable")

_DTYPES = {0: np.uint8, 1: np.int32, 2: np.float32, 3: np.float16}

L.ofe_mapstatic_compute.argtypes = [C.c_void_p]
L.ofe_mapstatic_compute.restype = C.c_void_p
L.ofe_mapstatic_free.argtypes = [C.c_void_p]
L.ofe_mapstatic_free.restype = None
L.ofe_mapstatic_num_arrays.argtypes = [C.c_void_p]
L.ofe_mapstatic_num_arrays.restype = C.c_int32
L.ofe_mapstatic_array_info.argtypes = [
    C.c_void_p,
    C.c_int32,
    C.POINTER(C.c_char_p),
    C.POINTER(C.c_int),
    C.POINTER(C.c_int),
    C.POINTER(C.c_int64),
    C.POINTER(C.c_void_p),
]
L.ofe_mapstatic_array_info.restype = C.c_int

_ITEMSIZE = {0: 1, 1: 4, 2: 4, 3: 2}


def _add_derived(out: Dict[str, np.ndarray]) -> None:
    tclass = out["tclass"]
    out["attack_cost"] = ATTACK_COST[tclass]
    out["impassable"] = (tclass == TC_IMPASSABLE).astype(np.uint8)


def compute(map) -> Dict[str, np.ndarray]:
    """Compute every static output for ``map`` (an ``artifice_engine.Map``)."""
    ms = L.ofe_mapstatic_compute(map._h)
    if not ms:
        raise ValueError("ofe_mapstatic_compute failed")
    out: Dict[str, np.ndarray] = {}
    try:
        for i in range(L.ofe_mapstatic_num_arrays(ms)):
            name = C.c_char_p()
            dtype, ndim = C.c_int(), C.c_int()
            shape = (C.c_int64 * 3)()
            data = C.c_void_p()
            if L.ofe_mapstatic_array_info(
                ms, i, C.byref(name), C.byref(dtype), C.byref(ndim), shape, C.byref(data)
            ):
                raise RuntimeError("bad mapstatic array index")
            dims = tuple(int(shape[k]) for k in range(ndim.value))
            dt = np.dtype(_DTYPES[dtype.value])
            count = int(np.prod(dims, dtype=np.int64)) if dims else 1
            buf = (C.c_char * (count * _ITEMSIZE[dtype.value])).from_address(data.value) if count else b""
            arr = np.frombuffer(buf, dtype=dt, count=count).reshape(dims).copy()
            out[name.value.decode()] = arr
    finally:
        L.ofe_mapstatic_free(ms)
    _add_derived(out)
    return out


def terrain_hash(map) -> str:
    """Cache key: sha256 over the mapstatic version, map size and terrain bytes."""
    t = np.ascontiguousarray(map.terrain(), dtype=np.uint8)
    h = hashlib.sha256()
    h.update(b"artifice-mapstatic\0")
    h.update(np.array([MAPSTATIC_VERSION, t.shape[1], t.shape[0]], dtype="<i4").tobytes())
    h.update(t.tobytes())
    return h.hexdigest()


def cache_path(map, cache_dir) -> Path:
    return Path(cache_dir) / f"mapstatic-v{MAPSTATIC_VERSION}-{terrain_hash(map)[:24]}.npz"


def load_or_compute(map, cache_dir) -> Dict[str, np.ndarray]:
    """Load the cached outputs for ``map`` from ``cache_dir`` or compute and store them.

    Unreadable, stale (version mismatch) or incomplete caches are recomputed.
    The file is written atomically, so concurrent writers are safe.
    """
    path = cache_path(map, cache_dir)
    if path.exists():
        try:
            with np.load(path) as z:
                out = {k: z[k] for k in z.files}
            if int(out["version"]) == MAPSTATIC_VERSION and int(out["width"]) == map.width:
                _add_derived(out)
                return out
        except Exception:
            pass
    out = compute(map)
    path.parent.mkdir(parents=True, exist_ok=True)
    tmp = path.with_name(f"{path.name}.{os.getpid()}.tmp.npz")
    np.savez_compressed(tmp, **{k: v for k, v in out.items() if k not in _DERIVED})
    os.replace(tmp, path)
    return out


def as_float32(table: np.ndarray) -> np.ndarray:
    """A float16 distance table as float32 (inf = unreachable / no such patch)."""
    return table.astype(np.float32)

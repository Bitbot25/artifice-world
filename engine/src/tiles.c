/*
 * tiles.c - the copy-on-write tile store, tile ownership and border lists.
 *
 * Tiles are stored in chunks of TILE_CHUNK_SIZE consecutive tiles. A clone
 * shares every chunk with its parent and bumps the chunk's refcount; the
 * first write to a shared chunk copies it (tiles_chunk_mut). Reads never
 * copy.
 *
 * Border lists: players.border[p] holds every border tile of p, and
 * TileChunk.border_pos holds each tile's index in that list, so adding and
 * removing is O(1) (swap-remove).
 */
#include "internal.h"

static TileChunk *chunk_new_blank(void) {
    TileChunk *c = (TileChunk *)ofe_xmalloc(sizeof(TileChunk));
    c->refcount = 1;
    memset(c->state, 0, sizeof(c->state));
    memset(c->border_pos, 0xff, sizeof(c->border_pos));
    return c;
}

static void chunk_release(TileChunk *c) {
    if (__atomic_sub_fetch(&c->refcount, 1, __ATOMIC_ACQ_REL) == 0) free(c);
}

void tiles_init(TileStore *ts, int32_t num_tiles) {
    ts->num_chunks = (num_tiles + TILE_CHUNK_SIZE - 1) >> TILE_CHUNK_BITS;
    ts->chunks = (TileChunk **)ofe_xmalloc((size_t)ts->num_chunks * sizeof(TileChunk *));
    for (int32_t i = 0; i < ts->num_chunks; i++) ts->chunks[i] = chunk_new_blank();
}

void tiles_free(TileStore *ts) {
    for (int32_t i = 0; i < ts->num_chunks; i++) chunk_release(ts->chunks[i]);
    free(ts->chunks);
    ts->chunks = NULL;
    ts->num_chunks = 0;
}

void tiles_share(TileStore *dst, const TileStore *src) {
    if (dst->num_chunks != src->num_chunks) {
        tiles_free(dst);
        dst->num_chunks = src->num_chunks;
        dst->chunks = (TileChunk **)ofe_xmalloc((size_t)dst->num_chunks * sizeof(TileChunk *));
    } else {
        for (int32_t i = 0; i < dst->num_chunks; i++) chunk_release(dst->chunks[i]);
    }
    for (int32_t i = 0; i < src->num_chunks; i++) {
        TileChunk *c = src->chunks[i];
        __atomic_add_fetch(&c->refcount, 1, __ATOMIC_RELAXED);
        dst->chunks[i] = c;
    }
}

TileChunk *tiles_chunk_mut(TileStore *ts, TileRef t) {
    TileChunk **slot = &ts->chunks[t >> TILE_CHUNK_BITS];
    TileChunk *c = *slot;
    if (__atomic_load_n(&c->refcount, __ATOMIC_ACQUIRE) == 1) return c;
    TileChunk *copy = (TileChunk *)ofe_xmalloc(sizeof(TileChunk));
    memcpy(copy, c, sizeof(TileChunk));
    copy->refcount = 1;
    chunk_release(c);
    *slot = copy;
    return copy;
}

static inline void set_border_pos(OfeGame *g, TileRef t, int32_t pos) {
    tiles_chunk_mut(&g->tiles, t)->border_pos[t & TILE_CHUNK_MASK] = pos;
}

static void border_add(OfeGame *g, PlayerId p, TileRef t) {
    IntVec *b = &g->players.border[p];
    set_border_pos(g, t, b->len);
    IntVec_push(b, t);
}

static void border_remove(OfeGame *g, PlayerId p, TileRef t) {
    IntVec *b = &g->players.border[p];
    int32_t pos = tile_border_pos_raw(&g->tiles, t);
    TileRef last = b->data[--b->len];
    if (last != t) {
        b->data[pos] = last;
        set_border_pos(g, last, pos);
    }
    set_border_pos(g, t, -1);
}

/* An owned tile is a border tile if a 4-neighbour has another owner. */
static bool compute_is_border(const OfeGame *g, TileRef t, PlayerId owner) {
    TileRef nb[4];
    int n = map_neighbors4(g->map, t, nb);
    for (int i = 0; i < n; i++) {
        if (tile_owner(g, nb[i]) != owner) return true;
    }
    return false;
}

static void refresh_border(OfeGame *g, TileRef t) {
    PlayerId owner = tile_owner(g, t);
    bool listed = tile_border_pos_raw(&g->tiles, t) >= 0;
    if (owner == 0) {
        /* Unowned tiles are never in a list (removal happens before the
         * owner changes, see tile_set_owner). */
        return;
    }
    bool border = compute_is_border(g, t, owner);
    if (border && !listed) border_add(g, owner, t);
    else if (!border && listed) border_remove(g, owner, t);
}

void tile_set_owner(OfeGame *g, TileRef t, PlayerId owner) {
    PlayerId old = tile_owner(g, t);
    if (old == owner) return;
    PlayerTable *pl = &g->players;

    if (old != 0) {
        if (tile_border_pos_raw(&g->tiles, t) >= 0) border_remove(g, old, t);
        pl->num_tiles[old]--;
        pl->last_tile_change[old] = g->tick;
    }

    TileChunk *c = tiles_chunk_mut(&g->tiles, t);
    uint16_t *st = &c->state[t & TILE_CHUNK_MASK];
    if (owner != 0 && (*st & TS_FALLOUT_BIT)) {
        /* Conquering a tile clears its fallout. */
        g->num_fallout_tiles--;
        *st &= (uint16_t)~TS_FALLOUT_BIT;
    }
    *st = (uint16_t)((*st & ~TS_OWNER_MASK) | (owner & TS_OWNER_MASK));

    if (owner != 0) {
        pl->num_tiles[owner]++;
        pl->last_tile_change[owner] = g->tick;
    }

    refresh_border(g, t);
    TileRef nb[4];
    int n = map_neighbors4(g->map, t, nb);
    for (int i = 0; i < n; i++) refresh_border(g, nb[i]);

    if (g->cfg.event_mask & (1u << OFE_EV_TILE_OWNER)) event_emit(g, OFE_EV_TILE_OWNER, t, old, owner, 0, 0);
}

void tile_set_fallout(OfeGame *g, TileRef t, bool fallout) {
    bool has = tile_has_fallout(g, t);
    if (has == fallout) return;
    uint16_t *st = &tiles_chunk_mut(&g->tiles, t)->state[t & TILE_CHUNK_MASK];
    if (fallout) {
        *st |= TS_FALLOUT_BIT;
        g->num_fallout_tiles++;
    } else {
        *st &= (uint16_t)~TS_FALLOUT_BIT;
        g->num_fallout_tiles--;
    }
}

bool tile_borders_player(const OfeGame *g, TileRef t, PlayerId p) {
    TileRef nb[4];
    int n = map_neighbors4(g->map, t, nb);
    for (int i = 0; i < n; i++) {
        if (tile_owner(g, nb[i]) == p) return true;
    }
    return false;
}

uint32_t tiles_new_visit_gen(OfeGame *g) {
    if (!g->visit_stamp) g->visit_stamp = (uint32_t *)ofe_xcalloc((size_t)g->map->num_tiles, sizeof(uint32_t));
    if (++g->visit_gen == 0) {
        memset(g->visit_stamp, 0, (size_t)g->map->num_tiles * sizeof(uint32_t));
        g->visit_gen = 1;
    }
    return g->visit_gen;
}

void tiles_collect_player(OfeGame *g, PlayerId p, IntVec *out) {
    out->len = 0;
    uint32_t gen = tiles_new_visit_gen(g);
    uint32_t *seen = g->visit_stamp;
    const IntVec *b = &g->players.border[p];
    for (int32_t i = 0; i < b->len; i++) {
        seen[b->data[i]] = gen;
        IntVec_push(out, b->data[i]);
    }
    /* Every component of a territory touches its border, so flooding from
     * the border reaches every tile. `out` doubles as the BFS queue. */
    for (int32_t head = 0; head < out->len; head++) {
        TileRef nb[4];
        int n = map_neighbors4(g->map, out->data[head], nb);
        for (int i = 0; i < n; i++) {
            TileRef q = nb[i];
            if (seen[q] != gen && tile_owner(g, q) == p) {
                seen[q] = gen;
                IntVec_push(out, q);
            }
        }
    }
}

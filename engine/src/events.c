/*
 * events.c - the per-tick event log.
 *
 * Every state change worth reacting to (tile flips, units, attacks,
 * alliances, deaths...) appends one fixed-size OfeEvent. The caller drains
 * the log between decisions, so features and labels can update
 * incrementally. cfg.event_mask filters event types at the source.
 */
#include "internal.h"

void event_emit(OfeGame *g, int32_t type, int32_t a, int32_t b, int32_t c, int32_t d, double v) {
    if (!(g->cfg.event_mask & (1u << type))) return;
    OfeEvent e;
    e.tick = g->tick;
    e.type = type;
    e.a = a;
    e.b = b;
    e.c = c;
    e.d = d;
    e.v = v;
    EventVec_push(&g->events, e);
}

int32_t ofe_game_events(const OfeGame *g, const OfeEvent **out) {
    *out = g->events.data;
    return g->events.len;
}

int32_t ofe_game_drain_events(OfeGame *g, OfeEvent *out, int32_t cap) {
    int32_t n = OFE_MIN(cap, g->events.len);
    if (n > 0) memcpy(out, g->events.data, (size_t)n * sizeof(OfeEvent));
    int32_t rest = g->events.len - n;
    if (rest > 0) memmove(g->events.data, g->events.data + n, (size_t)rest * sizeof(OfeEvent));
    g->events.len = rest;
    return n;
}

void ofe_game_clear_events(OfeGame *g) { g->events.len = 0; }

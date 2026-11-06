/*
 * win.c - win check and overtime.
 *
 * Checked every 10 ticks after the spawn phase. A side wins when it holds
 * more than the bar (default 80.0%) of the land without fallout. Overtime
 * lowers the bar by 2 points a minute after overtime_start_minutes. A game
 * timer (max_timer_minutes) or the 170 minute hard cap hands the win to the
 * biggest side.
 */
#include "internal.h"

#define HARD_TIME_LIMIT_SECONDS (170 * 60)
#define OVERTIME_DROP_PERCENT_PER_MINUTE 2

bool game_in_spawn_phase(const OfeGame *g) { return g->start_tick < 0; }

double game_elapsed_seconds(const OfeGame *g) {
    if (g->start_tick < 0) return 0.0;
    return (double)(g->tick - g->start_tick) / TICKS_PER_SECOND;
}

int32_t win_threshold_tenths(const OfeGame *g) {
    int32_t base = g->cfg.win_threshold_tenths > 0 ? g->cfg.win_threshold_tenths : 800;
    if (!g->cfg.overtime_enabled) return base;
    int32_t past = (int32_t)floor(game_elapsed_seconds(g)) - g->cfg.overtime_start_minutes * 60;
    if (past <= 0) return base;
    return OFE_MAX(0, base - (past * OVERTIME_DROP_PERCENT_PER_MINUTE / 60) * 10);
}

static bool has_won(const OfeGame *g, int64_t tiles) {
    double elapsed = game_elapsed_seconds(g);
    if (g->cfg.max_timer_minutes > 0 && elapsed >= g->cfg.max_timer_minutes * 60.0) return true;
    if (elapsed >= HARD_TIME_LIMIT_SECONDS) return true;
    int64_t clean_land = g->num_land_tiles - g->num_fallout_tiles;
    return tiles * 1000 > clean_land * win_threshold_tenths(g);
}

static void declare(OfeGame *g, PlayerId player, int32_t team) {
    g->game_over = 1;
    g->winner = player;
    g->winning_team = team;
    event_emit(g, OFE_EV_GAME_OVER, 0, player, team, 0, 0);
}

void win_check(OfeGame *g) {
    if (g->game_over || g->start_tick < 0 || g->tick % 10 != 0) return;
    const PlayerTable *pl = &g->players;

    if (g->cfg.game_mode == OFE_MODE_FFA) {
        PlayerId best = 0;
        for (PlayerId p = 1; p < pl->count; p++) {
            if (pl->num_tiles[p] > 0 && (best == 0 || pl->num_tiles[p] > pl->num_tiles[best])) best = p;
        }
        if (best && has_won(g, pl->num_tiles[best])) declare(g, best, -1);
        return;
    }

    /* Team games: sum tiles per team. Bots (tribes) can't win. */
    int32_t nteams = OFE_MAX(1, g->cfg.num_teams);
    int64_t *tiles = (int64_t *)ofe_xcalloc((size_t)nteams + 1, sizeof(int64_t));
    for (PlayerId p = 1; p < pl->count; p++) {
        int16_t t = pl->team[p];
        if (t >= 0 && t < nteams) tiles[t] += pl->num_tiles[p];
    }
    int32_t best = -1;
    for (int32_t t = 0; t < nteams; t++) {
        if (best < 0 || tiles[t] > tiles[best]) best = t;
    }
    if (best >= 0 && has_won(g, tiles[best])) declare(g, 0, best);
    free(tiles);
}

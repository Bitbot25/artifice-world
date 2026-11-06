/*
 * bench.c - engine throughput.
 *
 *   build/bench [map_dir] [ticks] [games] [threads]
 *
 * Defaults: $OFE_MAPS_DIR/world, 3000 ticks, 1 game, all threads. Reports
 * ticks per second per game and in aggregate, and clone cost.
 */
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#include "../engine/include/ofe.h"

static double now(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}

int main(int argc, char **argv) {
    char def[1024];
    const char *maps = getenv("OFE_MAPS_DIR");
    snprintf(def, sizeof(def), "%s/world", maps ? maps : "third_party/openfront/resources/maps");
    const char *dir = argc > 1 ? argv[1] : def;
    int ticks = argc > 2 ? atoi(argv[2]) : 3000;
    int ngames = argc > 3 ? atoi(argv[3]) : 1;
    int threads = argc > 4 ? atoi(argv[4]) : 0;

    OfeMap *map = ofe_map_load(dir, 0);
    if (!map) return 1;
    OfeConfig cfg;
    ofe_config_default(&cfg);
    cfg.event_mask = OFE_EVENTS_NO_TILES;
    cfg.humans_use_ai = 1;

    OfeGame **games = (OfeGame **)malloc(sizeof(OfeGame *) * ngames);
    for (int i = 0; i < ngames; i++) games[i] = ofe_game_create(map, &cfg, 1000 + i);

    double t0 = now();
    int chunk = 100;
    for (int done = 0; done < ticks; done += chunk) {
        ofe_step_batch(games, ngames, chunk, threads);
        for (int i = 0; i < ngames; i++) ofe_game_clear_events(games[i]);
    }
    double dt = now() - t0;
    printf("map %s %dx%d, %d players, %d game(s), %d ticks\n", ofe_map_name(map), ofe_map_width(map), ofe_map_height(map),
           ofe_game_num_players(games[0]), ngames, ticks);
    printf("  %.0f ticks/s per game, %.0f ticks/s total (%.1fx real time per game)\n", ticks / dt * ngames / ngames,
           ticks * (double)ngames / dt, ticks / dt / 10.0);

    t0 = now();
    int nclones = 200;
    for (int i = 0; i < nclones; i++) {
        OfeGame *c = ofe_game_clone(games[0]);
        ofe_game_destroy(c);
    }
    printf("  clone+destroy: %.1f us\n", (now() - t0) / nclones * 1e6);
    OfePlayerInfo info;
    ofe_game_player_info(games[0], 1, &info);
    printf("  agent 1: %d tiles, %lld troops, %d units total, tick %d\n", info.num_tiles, (long long)info.troops,
           ofe_game_num_units(games[0]), ofe_game_tick(games[0]));

    for (int i = 0; i < ngames; i++) ofe_game_destroy(games[i]);
    free(games);
    ofe_map_release(map);
    return 0;
}

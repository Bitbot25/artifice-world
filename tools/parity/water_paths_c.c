/*
 * water_paths_c.c - replays a water_paths.ts reference file on pathfind_ts.c
 * and compares tile for tile (see water_paths.ts for the file format).
 *
 *   build-parity/water_paths_c <map dir> <compact 0|1> <reference.txt>
 *
 * Prints the match rate per query kind, the first mismatches and the time per
 * query (repeated point queries come from the route memo). Exit status 1 if
 * anything differs.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "pathfind_ts.h"

static double now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1e3 + ts.tv_nsec / 1e6;
}

static char *line_buf;
static size_t line_cap;

static int read_line(FILE *f) { return getline(&line_buf, &line_cap, f) > 0; }
static long next_int(char **p) { return strtol(*p, p, 10); }

int main(int argc, char **argv) {
    if (argc < 4) {
        fprintf(stderr, "usage: %s <map dir> <compact 0|1> <reference.txt>\n", argv[0]);
        return 2;
    }
    OfeMap *m = ofe_map_load(argv[1], atoi(argv[2]));
    if (!m) return 2;
    FILE *f = fopen(argv[3], "r");
    if (!f) return 2;
    double tb = now_ms();
    const TsWater *w = ts_water(m);
    double build_ms = now_ms() - tb;
    if (!read_line(f) || line_buf[0] != 'M') return 2;
    printf("%s: map %dx%d mini %dx%d, abstract nodes %d, graph build %.0f ms\n", argv[3], m->width, m->height,
           m->mini_width, m->mini_height, ts_water_node_count(w), build_ms);

    long total[3] = {0}, ok[3] = {0}, nulls = 0, shown = 0;
    double ts_ms = 0, tsp_ms = 0;
    long ts_calls = 0, tsp_calls = 0;
    IntVec got = {0};
    char *qline = NULL;
    while (read_line(f)) {
        free(qline);
        qline = strdup(line_buf);
        if (!read_line(f) || line_buf[0] != '=') return 2;
        char *q = qline + 2, *r = line_buf + 2;
        int kind = qline[0] == 'P' ? 0 : qline[0] == 'Q' ? 1 : 2;
        bool match = true;
        if (kind < 2) {
            TileRef from[4096];
            int nf = 1;
            if (kind == 0) from[0] = (TileRef)next_int(&q);
            else {
                nf = (int)next_int(&q);
                for (int i = 0; i < nf; i++) from[i] = (TileRef)next_int(&q);
            }
            TileRef to = (TileRef)next_int(&q);
            double t0 = now_ms();
            bool found = ts_water_find_path(w, from, nf, to, &got);
            double dt = now_ms() - t0;
            ts_ms += dt;
            ts_calls++;
            long n = next_int(&r);
            if (n < 0) {
                match = !found;
                nulls++;
            } else {
                match = found && got.len == n;
                for (long i = 0; match && i < n; i++) match = got.data[i] == (TileRef)next_int(&r);
            }
            if (kind == 0 && from[0] >= 0 && from[0] < m->num_tiles && to >= 0 && to < m->num_tiles) {
                tsp_ms += dt;
                tsp_calls++;
            }
        } else {
            TileRef from = (TileRef)next_int(&q), dst = (TileRef)next_int(&q);
            int dist = (int)next_int(&q), max = (int)next_int(&q), nev = (int)next_int(&q);
            long et[8], ev[8];
            char ek[8];
            for (int i = 0; i < nev; i++) {
                et[i] = next_int(&q);
                while (*q == ' ') q++;
                ek[i] = *q++;
                ev[i] = next_int(&q);
            }
            TsWaterStepper st;
            ts_water_stepper_init(&st);
            TileRef pos = from;
            long n = next_int(&r);
            long steps = 0;
            for (int tick = 0; tick < max; tick++) {
                for (int i = 0; i < nev; i++) {
                    if (et[i] != tick) continue;
                    if (ek[i] == 'D') dst = (TileRef)ev[i];
                    else if (ek[i] == 'T') pos = (TileRef)ev[i];
                    else ts_water_stepper_invalidate(&st);
                }
                TileRef node;
                double t0 = now_ms();
                TsPathStatus s = ts_water_next(w, &st, pos, dst, dist, &node);
                ts_ms += now_ms() - t0;
                ts_calls++;
                long es = next_int(&r), en = next_int(&r);
                if (steps >= n || es != (long)s || en != (long)node) match = false;
                steps++;
                if (s == TS_PATH_NEXT) pos = node;
                else break;
            }
            if (steps != n) match = false;
            ts_water_stepper_free(&st);
        }
        total[kind]++;
        if (match) ok[kind]++;
        else if (shown++ < 5) printf("MISMATCH: %.200s\n", qline);
    }
    const char *names[3] = {"point-to-point", "multi-source", "voyages"};
    long t = 0, o = 0;
    for (int k = 0; k < 3; k++) {
        printf("  %-15s %ld/%ld match\n", names[k], ok[k], total[k]);
        t += total[k];
        o += ok[k];
    }
    printf("  total %ld/%ld (%.3f%%), %ld null results\n", o, t, t ? 100.0 * o / t : 100.0, nulls);
    printf("  cost per point query: %.3f ms (%ld queries)\n", tsp_calls ? tsp_ms / tsp_calls : 0, tsp_calls);
    printf("  all calls incl. voyage steps: %.4f ms/call over %ld calls\n", ts_calls ? ts_ms / ts_calls : 0, ts_calls);
    return o == t ? 0 : 1;
}

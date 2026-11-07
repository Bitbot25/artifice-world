/*
 * air_rail_paths_c.c - runs the query file of air_rail_paths.ts on the C port
 * (engine/src/paths_ts.c) and prints the same output lines.
 *
 *   air_rail_paths_c <maps_dir> <queries.txt>
 *
 * Build (parity library): see tools/parity/air_rail_paths.py.
 */
#include "../../engine/src/internal.h"
#include "../../engine/src/paths_ts.h"

#if !OFE_TS_PARITY
#error "build with -DOFE_TS_PARITY=1"
#endif

static void print_vec(const IntVec *v) {
    for (int32_t i = 0; i < v->len; i++) printf(" %d", v->data[i]);
}

static int read_ints(const char *s, double *out, int max) {
    int n = 0;
    char *end;
    while (n < max) {
        double v = strtod(s, &end);
        if (end == s) break;
        out[n++] = v;
        s = end;
    }
    return n;
}

int main(int argc, char **argv) {
    if (argc < 3) return 2;
    FILE *f = fopen(argv[2], "r");
    if (!f) return 2;
    char line[1 << 20];
    int qi = -1;
    OfeMap *map = NULL;
    TsGrid game = {0}, mini = {0};
    uint8_t *mini_t = NULL;
    TsRailPf *rail = NULL;
    TsRailNet *net = NULL;
    IntVec tmp = {0}, tmp2 = {0};
    int32_t *unit_of = NULL;
    (void)unit_of;

    while (fgets(line, sizeof(line), f)) {
        if (line[0] == '\n' || line[0] == '#') continue;
        char kind[16], name[128];
        int compact = 0;
        if (sscanf(line, "%15s", kind) != 1) continue;
        if (strcmp(kind, "map") == 0) {
            sscanf(line, "map %127s %d", name, &compact);
            char dir[1024];
            snprintf(dir, sizeof(dir), "%s/%s", argv[1], name);
            map = ofe_map_load(dir, compact);
            if (!map) {
                fprintf(stderr, "cannot load %s\n", dir);
                return 1;
            }
            game = ts_grid_from_map(map);
            mini_t = ts_mini_terrain_load(dir, compact, game.w, game.h);
            if (!mini_t) {
                fprintf(stderr, "cannot load mini map of %s\n", dir);
                return 1;
            }
            mini.w = (game.w + 1) / 2;
            mini.h = (game.h + 1) / 2;
            mini.terrain = mini_t;
            continue;
        }
        double a[8192];
        int na = read_ints(line + strlen(kind), a, 8192);
        if (kind[0] != 'S' && kind[0] != 'T' && kind[0] != 'V' && kind[0] != 'D') qi++;
        if (kind[0] == 'P') {
            TsParabolaOpts o = {a[2], !(((int)a[4]) & 2) ? true : true, !!(((int)a[4]) & 1), !!(((int)a[4]) & 4)};
            o.distance_based_height = !(((int)a[4]) & 2);
            int32_t from = (int32_t)a[0], to = (int32_t)a[1];
            ts_parabola_find_path(&game, &o, from, to, &tmp);
            printf("P %d F %d", qi, tmp.len);
            print_vec(&tmp);
            printf("\n");
            TsParabola pf;
            ts_parabola_init(&pf, &game, &o);
            IntVec idx = {0};
            tmp.len = 0;
            TsPathResult r;
            int status = -1, end_node = -1;
            for (int c = 0; c < 200000; c++) {
                r = ts_parabola_next(&pf, from, to, a[3]);
                if (r.status != TS_PATH_NEXT) {
                    status = (int)r.status;
                    end_node = r.node;
                    break;
                }
                IntVec_push(&tmp, r.node);
                IntVec_push(&idx, ts_parabola_current_index(&pf));
            }
            printf("P %d S %d", qi, tmp.len);
            print_vec(&tmp);
            printf(" I");
            print_vec(&idx);
            printf(" E %d %d\n", status, end_node);
            IntVec_free(&idx);
            ts_parabola_free(&pf);
        } else if (kind[0] == 'M') {
            double s = ts_mirv_speed(&game, (int32_t)a[0], (int32_t)a[1]);
            printf("M %d %.17g\n", qi, s);
        } else if (kind[0] == 'A') {
            int64_t seed = (int64_t)a[0];
            int32_t from = (int32_t)a[1];
            int cnt = (int)a[2];
            ts_air_find_path(&game, seed, from, (int32_t)a[3], &tmp);
            printf("A %d F %d", qi, tmp.len);
            print_vec(&tmp);
            printf("\nA %d C", qi);
            TsAir air;
            ts_air_init(&air, &game, seed);
            int32_t cur = from;
            for (int i = 0; i < cnt; i++) {
                TsPathResult r = ts_air_next(&air, cur, (int32_t)a[3 + i]);
                printf(" %d:%d", (int)r.status, r.status == TS_PATH_NOT_FOUND ? -1 : r.node);
                if (r.status == TS_PATH_NEXT) cur = r.node;
            }
            printf("\n");
            ts_air_free(&air);
        } else if (kind[0] == 'R') {
            if (!rail) rail = ts_rail_pf_new(&game, &mini);
            if (!ts_rail_pf_find_path(rail, (int32_t)a[0], (int32_t)a[1], &tmp)) printf("R %d -1\n", qi);
            else {
                printf("R %d %d", qi, tmp.len);
                print_vec(&tmp);
                printf("\n");
            }
        } else if (kind[0] == 'S') {
            if (!net) net = ts_rail_net_new(&game, &mini);
            int32_t u = ts_rail_net_add_structure(net, (TsStructure)(int)a[0], (int32_t)a[1]);
            ts_rail_net_structure_done(net, u);
            qi++;
        } else if (kind[0] == 'T') {
            int32_t sa = ts_rail_net_station_of_unit(net, (int32_t)a[0]), sb = ts_rail_net_station_of_unit(net, (int32_t)a[1]);
            tmp.len = 0;
            if (sa >= 0 && sb >= 0) ts_rail_net_stations_path(net, sa, sb, &tmp);
            printf("T %d %d", ++qi, tmp.len);
            for (int32_t i = 0; i < tmp.len; i++) printf(" %d", ts_rail_net_station_unit(net, tmp.data[i]));
            printf("\n");
        } else if (kind[0] == 'V') {
            int32_t sa = ts_rail_net_station_of_unit(net, (int32_t)a[0]), sb = ts_rail_net_station_of_unit(net, (int32_t)a[1]);
            tmp.len = 0;
            if (sa >= 0 && sb >= 0) ts_rail_net_stations_path(net, sa, sb, &tmp);
            TsTrain *t = ts_train_new(net, tmp.data, tmp.len, (int32_t)a[2]);
            printf("V %d", ++qi);
            if (!t) printf(" NONE\n");
            else {
                int32_t start = ts_rail_net_station_tile(net, tmp.data[0]);
                int ncars = (int)a[2] + 1;
                printf(" ");
                for (int i = 0; i < ncars + 1; i++) printf(i ? ",%d" : "%d", start);
                /* TS: init tick, then the first moving tick */
                TsTrainStep st;
                for (int tick = 0; tick < (ts_train_stuck(t) ? 2999 : 3000); tick++) {
                    ts_train_tick(t, true, &st);
                    if (ts_train_stuck(t)) {
                        printf(" ");
                        for (int i = 0; i < ncars + 1; i++) printf(i ? ",%d" : "%d", start);
                        continue;
                    }
                    if (!st.active_after) {
                        printf(" END");
                        break;
                    }
                    printf(" %d", st.engine_tile);
                    for (int i = 0; i < st.num_cars; i++) printf(",%d", st.car_tile[i]);
                }
                printf("\n");
                ts_train_free(t);
            }
        } else if (kind[0] == 'D') {
            qi++;
            int32_t nr = ts_rail_net_num_railroads(net);
            for (int32_t i = 0; i < nr; i++) {
                int32_t from, to, id, nt;
                const int32_t *tiles;
                ts_rail_net_railroad(net, i, &from, &to, &id, &tiles, &nt);
                printf("RAIL %d %d %d %d", id, ts_rail_net_station_unit(net, from), ts_rail_net_station_unit(net, to), nt);
                for (int32_t k = 0; k < nt; k++) printf(" %d", tiles[k]);
                printf("\n");
            }
            for (int32_t u = 0; u < ts_rail_net_num_units(net); u++) {
                int32_t s = ts_rail_net_station_of_unit(net, u);
                if (s < 0) {
                    printf("ST %d none\n", u);
                    continue;
                }
                ts_rail_net_neighbors(net, s, &tmp);
                printf("ST %d nb", u);
                for (int32_t k = 0; k < tmp.len; k++) printf(" %d", ts_rail_net_station_unit(net, tmp.data[k]));
                int32_t c = ts_rail_net_cluster(net, s);
                printf(" trade");
                if (c < 0) printf(" null");
                else {
                    ts_rail_net_cluster_trade_stations(net, c, &tmp2);
                    for (int32_t k = 0; k < tmp2.len; k++) printf(" %d", ts_rail_net_station_unit(net, tmp2.data[k]));
                }
                printf("\n");
            }
        }
    }
    ts_rail_pf_free(rail);
    ts_rail_net_free(net);
    return 0;
}

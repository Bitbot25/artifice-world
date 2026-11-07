/*
 * test_tsrandom.c - TsRandom (OpenFront's PseudoRandom), simpleHash and
 * DetMath against values produced by the TypeScript engine:
 *   node_modules/.bin/tsx -e "import {PseudoRandom} from '.../PseudoRandom'; ..."
 */
#include "test_util.h"

static int near_eq(double a, double b) { return a == b; }

int main(void) {
    TsRandom r;
    ts_rand_init(&r, 123);
    CHECK(near_eq(ts_rand_next(&r), 0.025040518259629607));
    CHECK(near_eq(ts_rand_next(&r), 0.77641140879131854));
    CHECK(near_eq(ts_rand_next(&r), 0.10681998985819519));
    CHECK(ts_rand_int(&r, 0, 7) == 1);
    CHECK(ts_rand_int(&r, 0, 5) == 0);
    CHECK(ts_rand_int(&r, 100, 200) == 145);
    CHECK(near_eq(ts_rand_float(&r, 1, 3), 1.7514003394171596));

    CHECK(ts_simple_hash("PARITY01") == 1961667374);
    CHECK(ts_simple_hash("") == 0);
    CHECK(ts_simple_hash("a") == 97);
    CHECK(ts_simple_hash("abcdefghij") == 634317659);
    CHECK(ts_simple_hash("zzzzzzzzzzzzzzzzzzzz") == 1718487168);

    TsRandom ids;
    char id[9];
    ts_rand_init(&ids, ts_simple_hash("PARITY01"));
    ts_rand_id(&ids, id);
    CHECK(strcmp(id, "eiwxj9jj") == 0);
    ts_rand_id(&ids, id);
    CHECK(strcmp(id, "fqb6w5fc") == 0);
    ts_rand_id(&ids, id);
    CHECK(strcmp(id, "2kjvmkav") == 0);

    TsRandom r3;
    ts_rand_init(&r3, ts_simple_hash("PARITY01") + 1);
    CHECK(ts_rand_int(&r3, 0, 1000) == 536);
    CHECK(ts_rand_int(&r3, 0, 1000) == 489);

    int32_t a[10] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9};
    TsRandom sh;
    ts_rand_init(&sh, 7);
    ts_rand_shuffle(&sh, a, 10);
    const int32_t want[10] = {7, 3, 1, 2, 5, 6, 0, 4, 8, 9};
    CHECK(memcmp(a, want, sizeof a) == 0);

    /* Only the low 32 bits of the seed count (seed | 0). */
    TsRandom big, neg;
    ts_rand_init(&big, 4294967296LL + 5);
    CHECK(ts_rand_int(&big, 0, 1000000) == 755292);
    ts_rand_init(&neg, -5);
    CHECK(ts_rand_int(&neg, 0, 1000000) == 937158);

    CHECK(near_eq(det_exp(1.5), 4.4816890703380299));
    CHECK(near_eq(det_log(7.25), 1.9810014688665833));
    CHECK(near_eq(det_pow(12345, 0.6), 285.03268020122704));
    CHECK(near_eq(det_pow(25000, 0.73), 1623.6641673243630));
    CHECK(near_eq(det_pow(0.5, 3), 0.125));

    if (failures) return 1;
    printf("test_tsrandom: ok\n");
    return 0;
}

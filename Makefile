# Builds the engine as a shared library (for Python) and a static library
# (for C tests and tools).
#
#   make            build/libofe.so and build/libofe.a
#   make test       C unit tests + Python tests
#   make bench      throughput benchmark
#   make debug      -O0 -g with sanitizers
#   make parity     TS-parity build (-DOFE_TS_PARITY=1) in build-parity/;
#                   see docs/ts_parity.md
#
# Every engine/src/*.c is compiled; adding a module needs no Makefile edit.

CC      ?= cc
BUILD   ?= build
CFLAGS  ?= -O3 -march=native
override CFLAGS += -std=c99 -D_DEFAULT_SOURCE -Wall -Wextra -Wno-unused-parameter -fPIC -fopenmp -Iengine/include
override LDFLAGS += -fopenmp -lm -lpthread

SRC  := $(wildcard engine/src/*.c)
OBJ  := $(patsubst engine/src/%.c,$(BUILD)/obj/%.o,$(SRC))
DEPS := $(OBJ:.o=.d)

.PHONY: all clean test bench debug ctest pytest parity parity-check parity-paths

all: $(BUILD)/libofe.so $(BUILD)/libofe.a

$(BUILD)/obj/%.o: engine/src/%.c | $(BUILD)/obj
	$(CC) $(CFLAGS) -MMD -MP -c $< -o $@

$(BUILD)/obj:
	mkdir -p $@

$(BUILD)/libofe.so: $(OBJ)
	$(CC) -shared -o $@ $^ $(LDFLAGS)

$(BUILD)/libofe.a: $(OBJ)
	rm -f $@ && ar rcs $@ $^

# Every tests/c/test_*.c is its own test binary.
CTESTS := $(patsubst tests/c/%.c,$(BUILD)/tests/%,$(wildcard tests/c/test_*.c))

$(BUILD)/tests/%: tests/c/%.c tests/c/test_util.h $(BUILD)/libofe.a
	@mkdir -p $(BUILD)/tests
	$(CC) $(CFLAGS) -o $@ $< $(BUILD)/libofe.a $(LDFLAGS)

$(BUILD)/bench: tools/bench.c $(BUILD)/libofe.a
	$(CC) $(CFLAGS) -o $@ $< $(BUILD)/libofe.a $(LDFLAGS)

ctest: $(CTESTS)
	@for t in $(CTESTS); do echo "== $$t"; $$t || exit 1; done

pytest: $(BUILD)/libofe.so
	python3 -m pytest -q tests/python

test: ctest pytest

bench: $(BUILD)/bench
	$(BUILD)/bench

debug:
	$(MAKE) BUILD=build-debug CFLAGS="-O0 -g -fsanitize=address,undefined -fno-omit-frame-pointer" \
	        LDFLAGS="-fsanitize=address,undefined" all ctest

# The TS-parity build: same sources, OFE_TS_PARITY switches on the code paths
# that reproduce OpenFront's observable behaviour exactly.
parity:
	$(MAKE) BUILD=build-parity CFLAGS="$(CFLAGS) -DOFE_TS_PARITY=1" all

# Lockstep comparison with the TS engine over every parity scenario
# (needs OPENFRONT_DIR, see tools/parity/README.md).
# Replays TS water-path references on pathfind_ts.c (tools/parity/water_paths.ts
# makes them); build-parity/water_paths_c <map dir> <compact> <reference>.
build-parity/water_paths_c: tools/parity/water_paths_c.c
	$(MAKE) parity
	$(CC) $(CFLAGS) -DOFE_TS_PARITY=1 -D_GNU_SOURCE -Iengine/src -o $@ $< build-parity/libofe.a $(LDFLAGS)

parity-check: parity
	@for s in tools/parity/scenarios/*.json; do python3 tools/parity/diff.py $$s || exit 1; done

# Missile / air / rail path code against the TS pathfinders (needs OPENFRONT_DIR).
parity-paths: parity
	python3 tools/parity/air_rail_paths.py

clean:
	rm -rf build build-debug build-parity

-include $(DEPS)

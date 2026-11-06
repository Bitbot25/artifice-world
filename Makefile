# Builds the engine as a shared library (for Python) and a static library
# (for C tests and tools).
#
#   make            build/libofe.so and build/libofe.a
#   make test       C unit tests + Python tests
#   make bench      throughput benchmark
#   make debug      -O0 -g with sanitizers
#
# Every engine/src/*.c is compiled; adding a module needs no Makefile edit.

CC      ?= cc
BUILD   ?= build
CFLAGS  ?= -O3 -march=native
CFLAGS  += -std=c99 -D_DEFAULT_SOURCE -Wall -Wextra -Wno-unused-parameter -fPIC -fopenmp -Iengine/include
LDFLAGS += -fopenmp -lm -lpthread

SRC  := $(wildcard engine/src/*.c)
OBJ  := $(patsubst engine/src/%.c,$(BUILD)/obj/%.o,$(SRC))
DEPS := $(OBJ:.o=.d)

.PHONY: all clean test bench debug ctest pytest

all: $(BUILD)/libofe.so $(BUILD)/libofe.a

$(BUILD)/obj/%.o: engine/src/%.c | $(BUILD)/obj
	$(CC) $(CFLAGS) -MMD -MP -c $< -o $@

$(BUILD)/obj:
	mkdir -p $@

$(BUILD)/libofe.so: $(OBJ)
	$(CC) -shared -o $@ $^ $(LDFLAGS)

$(BUILD)/libofe.a: $(OBJ)
	ar rcs $@ $^

$(BUILD)/test_engine: tests/c/test_engine.c $(BUILD)/libofe.a
	$(CC) $(CFLAGS) -o $@ $< $(BUILD)/libofe.a $(LDFLAGS)

$(BUILD)/bench: tools/bench.c $(BUILD)/libofe.a
	$(CC) $(CFLAGS) -o $@ $< $(BUILD)/libofe.a $(LDFLAGS)

ctest: $(BUILD)/test_engine
	$(BUILD)/test_engine

pytest: $(BUILD)/libofe.so
	python3 -m pytest -q tests/python

test: ctest pytest

bench: $(BUILD)/bench
	$(BUILD)/bench

debug:
	$(MAKE) BUILD=build-debug CFLAGS="-O0 -g -fsanitize=address,undefined -fno-omit-frame-pointer" \
	        LDFLAGS="-fsanitize=address,undefined" all build-debug/test_engine

clean:
	rm -rf build build-debug

-include $(DEPS)

CXX      := g++
CXXSTD   := -std=c++23
WARN     := -Wall -Wextra -Wpedantic
INCLUDE  := -Iinclude
OPT      := -O3 -DNDEBUG -march=native
DBG      := -O0 -g

BUILD := build

.PHONY: all test bench bench_nopool clean

all: test bench bench_nopool

$(BUILD):
	mkdir -p $(BUILD)

test: $(BUILD)
	$(CXX) $(CXXSTD) $(WARN) $(INCLUDE) $(DBG) tests/test_main.cpp -o $(BUILD)/test
	./$(BUILD)/test

bench: $(BUILD)
	$(CXX) $(CXXSTD) $(WARN) $(INCLUDE) $(OPT) bench/bench.cpp -o $(BUILD)/bench

bench_nopool: $(BUILD)
	$(CXX) $(CXXSTD) $(WARN) $(INCLUDE) $(OPT) -DLOB_NO_POOL bench/bench.cpp -o $(BUILD)/bench_nopool

clean:
	rm -rf $(BUILD)

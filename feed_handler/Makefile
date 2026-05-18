CXX        ?= g++
STD         := -std=c++23
WARN        := -Wall -Wextra -Wpedantic
OPT         := -O2 -g
THREADS     := -pthread

# point this at your matching-engine headers when you wire in the real book.
# example: INCLUDES := -I../matching_engine/include
INCLUDES    :=

BASE_FLAGS  := $(STD) $(WARN) $(OPT) $(THREADS) $(INCLUDES)

HDRS        := spsc_ring.h feed_protocol.h
BINS        := feed_handler feed_replay feed_handler_tsan

.PHONY: all clean

all: $(BINS)

feed_handler: feed_handler.cpp $(HDRS)
	$(CXX) $(BASE_FLAGS) feed_handler.cpp -o $@

feed_replay: feed_replay.cpp feed_protocol.h
	$(CXX) $(BASE_FLAGS) feed_replay.cpp -o $@

# tsan build — lower opt, full instrumentation, no -O2 (defeats some checks)
feed_handler_tsan: feed_handler.cpp $(HDRS)
	$(CXX) $(STD) $(WARN) -O1 -g $(THREADS) $(INCLUDES) -fsanitize=thread feed_handler.cpp -o $@

clean:
	rm -f $(BINS) *.o *.svg out.perf out.folded perf.data*

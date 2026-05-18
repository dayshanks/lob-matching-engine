#!/usr/bin/env bash
# flamegraph.sh — capture a flame graph of the handler under sustained load
# usage: ./flamegraph.sh [n_msgs] [port] [record_seconds]
set -euo pipefail

N_MSGS="${1:-100000000}"
PORT="${2:-9100}"
DURATION="${3:-20}"

# clone Brendan Gregg's FlameGraph scripts on first run
if [[ ! -d FlameGraph ]]; then
    git clone --depth 1 https://github.com/brendangregg/FlameGraph
fi

make feed_handler feed_replay

REPLAY_PID=""
HANDLER_PID=""
cleanup() {
    [[ -n "$REPLAY_PID"  ]] && kill "$REPLAY_PID"  2>/dev/null || true
    [[ -n "$HANDLER_PID" ]] && kill "$HANDLER_PID" 2>/dev/null || true
}
trap cleanup EXIT

./feed_replay "$PORT" "$N_MSGS" &
REPLAY_PID=$!
sleep 0.3

./feed_handler 127.0.0.1 "$PORT" &
HANDLER_PID=$!

echo "recording perf for ${DURATION}s on handler pid $HANDLER_PID"
perf record -F 999 -g -p "$HANDLER_PID" -- sleep "$DURATION"
perf script > out.perf
./FlameGraph/stackcollapse-perf.pl out.perf > out.folded
./FlameGraph/flamegraph.pl out.folded > flame.svg

echo "flame.svg ready — open in any browser"

wait "$REPLAY_PID"  2>/dev/null || true
wait "$HANDLER_PID" 2>/dev/null || true

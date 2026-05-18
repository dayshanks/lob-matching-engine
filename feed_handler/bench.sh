#!/usr/bin/env bash
# bench.sh — smoke test: build, start feed source, run handler
# usage: ./bench.sh [n_msgs] [port]
set -euo pipefail

N_MSGS="${1:-10000000}"
PORT="${2:-9100}"

make -j

REPLAY_PID=""
cleanup() {
    [[ -n "$REPLAY_PID" ]] && kill "$REPLAY_PID" 2>/dev/null || true
}
trap cleanup EXIT

./feed_replay "$PORT" "$N_MSGS" &
REPLAY_PID=$!
sleep 0.3

./feed_handler 127.0.0.1 "$PORT"

wait "$REPLAY_PID" 2>/dev/null || true

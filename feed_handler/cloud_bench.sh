#!/usr/bin/env bash
# cloud_bench.sh — provision a Hetzner Cloud VM, run bench + flame graph,
# fetch results, destroy the VM. zero leftover state, ~5-10 minutes end to end.
#
# one-time setup:
#   brew install hcloud
#   # generate a Hetzner Cloud API token at: https://console.hetzner.cloud → your project → Security → API Tokens
#   export HCLOUD_TOKEN=<your-token>               # add to ~/.zshrc to persist
#   # upload your ssh public key (one time)
#   hcloud ssh-key create --name default --public-key-from-file ~/.ssh/id_ed25519.pub
#
# usage:
#   ./cloud_bench.sh <repo-url> [n_msgs]
#
# example:
#   ./cloud_bench.sh https://github.com/dayshanks/your-lob-repo.git 100000000
#
# defaults: ccx23 in nbg1 (4 dedicated x86_64 cores, ~€0.034/hr). override:
#   SERVER_TYPE=ccx33 LOCATION=ash ./cloud_bench.sh <repo-url>
#
# private repo: pass an https url with a PAT, e.g.
#   https://\<token\>@github.com/dayshanks/your-private-repo.git

set -euo pipefail

: "${HCLOUD_TOKEN:?set with: export HCLOUD_TOKEN=...}"

REPO_URL="${1:?repo url required as first arg}"
N_MSGS="${2:-100000000}"

SERVER_TYPE="${SERVER_TYPE:-ccx23}"
LOCATION="${LOCATION:-nbg1}"
IMAGE="${IMAGE:-ubuntu-24.04}"
SSH_KEY_NAME="${SSH_KEY_NAME:-default}"
SERVER_NAME="feed-bench-$$"

cleanup() {
    echo ""
    echo "==> destroying $SERVER_NAME"
    hcloud server delete "$SERVER_NAME" 2>/dev/null || true
}
trap cleanup EXIT

echo "==> provisioning $SERVER_NAME ($SERVER_TYPE in $LOCATION)"
hcloud server create \
    --name "$SERVER_NAME" \
    --type "$SERVER_TYPE" \
    --image "$IMAGE" \
    --location "$LOCATION" \
    --ssh-key "$SSH_KEY_NAME" > /dev/null

IP=$(hcloud server ip "$SERVER_NAME")
echo "==> server up at $IP — waiting for ssh"

for i in {1..40}; do
    if ssh -o StrictHostKeyChecking=no -o ConnectTimeout=2 \
           -o UserKnownHostsFile=/dev/null \
           root@"$IP" "true" 2>/dev/null; then
        break
    fi
    sleep 3
done

SSH="ssh -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null root@$IP"
SCP="scp -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null"

echo "==> installing toolchain + cloning repo"
$SSH bash <<EOF
set -euo pipefail

# wait for cloud-init / unattended-upgrades to release apt locks
while fuser /var/lib/dpkg/lock-frontend > /dev/null 2>&1; do sleep 3; done

DEBIAN_FRONTEND=noninteractive apt-get update -qq
DEBIAN_FRONTEND=noninteractive apt-get install -y -qq \
    build-essential g++-14 git linux-tools-common linux-tools-generic
update-alternatives --install /usr/bin/g++ g++ /usr/bin/g++-14 100 > /dev/null

# perf needs paranoid disabled to record other procs
sysctl -w kernel.perf_event_paranoid=-1 > /dev/null

git clone --depth 1 "$REPO_URL" /root/proj
cd /root/proj/feed_handler
make -j
chmod +x bench.sh flamegraph.sh 2>/dev/null || true

echo ""
echo "------------------------------------------------------------"
echo "system info"
echo "------------------------------------------------------------"
uname -a
lscpu | grep -E '^(Architecture|Model name|CPU\(s\)|CPU MHz|L1d|L1i|L2|L3)' | head -10
EOF

echo ""
echo "==> running bench ($N_MSGS messages, producer pinned to cpu 2, consumer to cpu 3)"
$SSH bash <<EOF
set -euo pipefail
cd /root/proj/feed_handler
./feed_replay 9100 $N_MSGS > /tmp/replay.log 2>&1 &
REPLAY=\$!
sleep 0.3
./feed_handler 127.0.0.1 9100 2 3 | tee /root/bench_results.txt
wait \$REPLAY 2>/dev/null || true
echo ""
echo "------------------------------------------------------------"
echo "replay log"
echo "------------------------------------------------------------"
cat /tmp/replay.log
EOF

echo ""
echo "==> capturing flame graph (20s recording window)"
$SSH bash <<EOF
set -euo pipefail
cd /root/proj/feed_handler
[[ -d FlameGraph ]] || git clone --depth 1 -q https://github.com/brendangregg/FlameGraph

./feed_replay 9100 $((N_MSGS * 3)) > /tmp/replay.log 2>&1 &
REPLAY=\$!
sleep 0.3
./feed_handler 127.0.0.1 9100 2 3 > /tmp/handler.log 2>&1 &
HANDLER=\$!
sleep 1

perf record -F 999 -g -p \$HANDLER -- sleep 20 2>/dev/null
perf script > out.perf 2>/dev/null
./FlameGraph/stackcollapse-perf.pl out.perf > out.folded
./FlameGraph/flamegraph.pl out.folded > flame.svg

kill \$HANDLER \$REPLAY 2>/dev/null || true
wait 2>/dev/null || true
echo "flame.svg: \$(wc -c < flame.svg) bytes"
EOF

echo ""
echo "==> fetching results to ./bench_output/"
mkdir -p ./bench_output
$SCP root@"$IP":/root/bench_results.txt           ./bench_output/
$SCP root@"$IP":/root/proj/feed_handler/flame.svg ./bench_output/
$SCP root@"$IP":/root/proj/feed_handler/out.folded ./bench_output/ 2>/dev/null || true

echo ""
echo "============================================================"
echo "FINAL BENCH OUTPUT"
echo "============================================================"
cat ./bench_output/bench_results.txt
echo ""
echo "flame graph: ./bench_output/flame.svg (open in browser)"
echo "raw folded:  ./bench_output/out.folded (re-render with different colors etc)"

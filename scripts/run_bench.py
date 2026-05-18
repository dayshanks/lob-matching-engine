#!/usr/bin/env python3
import json
import re
import subprocess
import sys
import time
from pathlib import Path
from statistics import median

REPO = Path(__file__).resolve().parent.parent
RESULTS = REPO / "results"
RESULTS.mkdir(exist_ok=True)

CONFIGS = [
    ("build/bench",        "arena_pool", 2_000_000),
    ("build/bench_nopool", "malloc",     2_000_000),
]

RUNS_PER_CONFIG = 5

RX = {
    "throughput": re.compile(r"throughput:\s+([\d.]+)\s+ops/sec"),
    "p50_ns":     re.compile(r"p50=(\d+)\s+p90=\d+\s+p99=\d+"),
    "p90_ns":     re.compile(r"p50=\d+\s+p90=(\d+)\s+p99=\d+"),
    "p99_ns":     re.compile(r"p50=\d+\s+p90=\d+\s+p99=(\d+)"),
    "p999_ns":    re.compile(r"p999=(\d+)\s+max="),
    "max_ns":     re.compile(r"max=(\d+)"),
}

def run_once(binary: str, n: int) -> dict:
    path = REPO / binary
    if not path.exists():
        sys.exit(f"missing binary: {path} — run `make` first")
    out = subprocess.run([str(path), str(n)], capture_output=True, text=True, check=True).stdout
    parsed = {}
    for k, rx in RX.items():
        m = rx.search(out)
        if not m:
            sys.exit(f"failed to parse {k} from bench output:\n{out}")
        parsed[k] = float(m.group(1)) if k == "throughput" else int(m.group(1))
    return parsed

def aggregate(runs: list[dict]) -> dict:
    keys = runs[0].keys()
    return {k: median(r[k] for r in runs) for k in keys}

def main():
    all_results = {}
    for binary, label, n in CONFIGS:
        print(f"\n=== {label} ({binary}, n={n}) ===")
        runs = []
        for i in range(RUNS_PER_CONFIG):
            print(f"  run {i+1}/{RUNS_PER_CONFIG}...", end=" ", flush=True)
            r = run_once(binary, n)
            print(f"tput={r['throughput']/1e6:.2f}M/s  p99={r['p99_ns']}ns")
            runs.append(r)
            time.sleep(0.5)
        all_results[label] = {
            "binary": binary,
            "n_ops": n,
            "runs": runs,
            "median": aggregate(runs),
        }

    out_path = RESULTS / f"bench_{int(time.time())}.json"
    out_path.write_text(json.dumps(all_results, indent=2))
    (RESULTS / "latest.json").write_text(json.dumps(all_results, indent=2))

    print(f"\nresults written to {out_path}")
    print("\nsummary (medians):")
    for label, data in all_results.items():
        m = data["median"]
        print(f"  {label:15s}  tput={m['throughput']/1e6:6.2f}M/s  "
              f"p50={m['p50_ns']:>5}ns  p99={m['p99_ns']:>6}ns  p999={m['p999_ns']:>6}ns")

if __name__ == "__main__":
    main()

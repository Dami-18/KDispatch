#!/usr/bin/env python3
"""Turn results/*.json into the headline graphs.

  python3 scripts/plot.py results/ -o figs/
"""
import argparse, json, statistics as st
from collections import defaultdict
from pathlib import Path
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

NS_US = 1e3

def load(d):
    runs = []
    for p in sorted(Path(d).glob("*.json")):
        if p.name.endswith(".server.json"):
            continue
        with open(p) as f:
            r = json.load(f)
        srv = p.with_suffix("").with_suffix("")  # strip .json
        sp = Path(str(p)[:-5] + ".server.json")
        if sp.exists():
            with open(sp) as f:
                r["server"] = json.load(f)
        runs.append(r)
    return runs

AXES = {"large_pct": "large-message fraction (%)",
        "workers": "server worker threads",
        "conns": "connections",
        "offered_rate": "offered load (msg/s)"}

def axis_of(runs):
    """Pick the axis that actually varies across the loaded runs."""
    for k in ("large_pct", "workers", "conns", "offered_rate"):
        vals = {r.get(k) if k != "workers" else r.get("server", {}).get("workers")
                for r in runs}
        vals.discard(None)
        if len(vals) > 1:
            return k
    return "conns"

def group(runs, key, axis):
    """(arm, axis_value) -> median of key across repeats"""
    acc = defaultdict(list)
    for r in runs:
        x = r.get("server", {}).get("workers") if axis == "workers" else r.get(axis)
        v = key(r)
        if v is not None and x is not None:
            acc[(r["arm"], x)].append(v)
    return {k: st.median(v) for k, v in acc.items()}

def line_plot(data, ylabel, title, path, xlabel="connections", logy=True):
    arms = sorted({a for a, _ in data})
    plt.figure(figsize=(7, 4.5))
    for arm in arms:
        pts = sorted((c, v) for (a, c), v in data.items() if a == arm)
        if pts:
            plt.plot([c for c, _ in pts], [v for _, v in pts], marker="o", label=arm)
    plt.xscale("log")
    if logy:
        plt.yscale("log")
    plt.xlabel(xlabel)
    plt.ylabel(ylabel)
    plt.title(title)
    plt.grid(True, which="both", alpha=0.3)
    plt.legend()
    plt.tight_layout()
    plt.savefig(path, dpi=150)
    print("wrote", path)

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("results", nargs="?", default="results")
    ap.add_argument("-o", "--outdir", default="figs")
    a = ap.parse_args()
    Path(a.outdir).mkdir(exist_ok=True)

    runs = load(a.results)
    if not runs:
        raise SystemExit(f"no result json found in {a.results}/")
    print(f"loaded {len(runs)} runs")

    axis = axis_of(runs)
    xlabel = AXES[axis]
    print("sweep axis:", axis)

    for pct, name in ((0.50, "p50"), (0.99, "p99"), (0.999, "p999")):
        key = {"p50": "p50_ns", "p99": "p99_ns", "p999": "p999_ns"}[name]
        line_plot(group(runs, lambda r, k=key: r["latency"]["small_ol"][k] / NS_US, axis),
                  f"small-message {name} (us)",
                  f"Head-of-line blocking: small-message {name} vs {xlabel}",
                  f"{a.outdir}/{name}_small_by_{axis}.png", xlabel=xlabel)

    line_plot(group(runs, lambda r: r["server"]["worker_busy_pct_spread"]
                    if "server" in r else None, axis),
              "worker busy% spread (max-min)",
              "Work conservation: imbalance across workers",
              f"{a.outdir}/worker_spread_by_{axis}.png", xlabel=xlabel, logy=False)

if __name__ == "__main__":
    main()

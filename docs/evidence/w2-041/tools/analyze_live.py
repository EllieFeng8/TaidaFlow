#!/usr/bin/env python3
"""w2-041: summarise ui_ping.py / mem_sample.py CSVs over named time windows (taken from the
app log), so each claim in the report can be recomputed.

  python analyze_live.py --ping 30-desktop-export-ui-ping.csv --mem 31-desktop-export-mem.csv
         --window "idle=19:55:31.000-19:55:39.400" --window "export=19:55:44.455-19:55:49.466"

For each window: ping count, p50 / p99 / max latency (ms, 'inf' = no answer within 5 s),
number of pings above 20 ms; memory min/max of private bytes and working set (MB).
"""
import argparse
import csv
import math


def load(path):
    with open(path, encoding="utf-8") as f:
        return list(csv.DictReader(f))


ap = argparse.ArgumentParser()
ap.add_argument("--ping")
ap.add_argument("--mem")
ap.add_argument("--window", action="append", default=[])
a = ap.parse_args()
ping = load(a.ping) if a.ping else []
mem = load(a.mem) if a.mem else []

for w in a.window:
    name, span = w.split("=", 1)
    lo, hi = span.split("-")
    print(f"[{name}] {lo} .. {hi}")
    if ping:
        lat = sorted(float(r["latency_ms"]) for r in ping if lo <= r["time"] <= hi)
        if lat:
            p = lambda q: lat[min(len(lat) - 1, int(q * len(lat)))]
            fmt = lambda v: "inf" if math.isinf(v) else f"{v:.2f}"
            print(f"  ui ping: n={len(lat)} p50={fmt(p(0.5))} ms p99={fmt(p(0.99))} ms max={fmt(lat[-1])} ms "
                  f"above20ms={sum(1 for v in lat if v > 20)}")
        else:
            print("  ui ping: no samples")
    if mem:
        rows = [r for r in mem if lo <= r["time"] <= hi]
        if rows:
            pv = [float(r["private_mb"]) for r in rows]
            ws = [float(r["working_set_mb"]) for r in rows]
            print(f"  memory: n={len(rows)} private {min(pv):.1f}..{max(pv):.1f} MB, "
                  f"working set {min(ws):.1f}..{max(ws):.1f} MB")
        else:
            print("  memory: no samples")

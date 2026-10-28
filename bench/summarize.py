#!/usr/bin/env python3
"""Aggregate bench/results/results.jsonl into a per-label markdown table.

Values are medians over runs, normalised per minute of wall time. WindowServer
overhead is reported relative to the mean of the control runs taken in the same
file (no monitor running), so it estimates the extra compositor work each app causes.

usage: summarize.py [label-prefix ...]
"""
import json
import os
import statistics
import sys

here = os.path.dirname(os.path.abspath(__file__))
rows = [json.loads(l) for l in open(os.path.join(here, "results", "results.jsonl")) if l.strip()]
prefixes = sys.argv[1:]

by = {}
for r in rows:
    by.setdefault(r["label"], []).append(r)

ctrl = [r["ws_pct"] for r in by.get("control", [])]
ws_base = statistics.mean(ctrl) if ctrl else 0.0


def m(runs, key, scale=1.0):
    # median: robust to the occasional run disturbed by a system event
    vals = [r[key] * 60.0 / r["wall_s"] * scale for r in runs if key in r]
    return statistics.median(vals) if vals else float("nan")


print(f"WindowServer control baseline: {ws_base:.2f}% CPU over {len(ctrl)} runs\n")
print("| scenario | runs | CPU % | instr/min (M) | energy mJ/min | wakeups/min | syscalls/min | threads | footprint MB | WindowServer extra % |")
print("|---|---|---|---|---|---|---|---|---|---|")
for label, runs in by.items():
    if label.startswith("INVALID"):
        continue
    if label == "control" or (prefixes and not any(label.startswith(p) for p in prefixes)):
        continue
    cpu = statistics.median(r["cpu_pct"] for r in runs)
    ws = statistics.median(r["ws_pct"] for r in runs) - ws_base
    print(
        f"| {label} | {len(runs)} | {cpu:.3f} | {m(runs, 'instructions', 1e-6):.1f} | {m(runs, 'energy_mj'):.1f} | "
        f"{m(runs, 'wakeups'):.0f} | {m(runs, 'syscalls'):.0f} | {max(r['threads'] for r in runs)} | "
        f"{statistics.median(r['footprint_mb'] for r in runs):.1f} | {ws:+.2f} |"
    )

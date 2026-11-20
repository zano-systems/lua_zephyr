#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Turn tests/async_bench output into markdown tables.

    bench_table.py run.log                 # one run
    bench_table.py base.log new.log        # before/after with deltas
    bench_table.py --emit-baseline run.log > tests/async_bench/baselines/<board>.lua

Reads the BENCH (and STATS, when present) lines that benchlib.c prints; any
other line is ignored, so twister handler.log files or raw serial captures work.
"""

import argparse
import re
import sys

LINE = re.compile(r"(BENCH|STATS) (\S+) (.*)")
FIELD = re.compile(r"([\w/]+)=(\S+)")


def parse(path):
    bench, stats, order = {}, {}, []
    gc_of = {}
    with open(path, errors="replace") as f:
        for line in f:
            m = LINE.search(line)
            if not m or m.group(2) in ("FAIL", "NOTE"):
                continue
            kind, name, rest = m.groups()
            fields = dict(FIELD.findall(rest))
            if kind == "BENCH":
                key = (name, fields.get("gc", "?"))
                gc_of[name] = key[1]
                bench[key] = fields
                order.append(key)
            else:
                stats[(name, gc_of.get(name, "?"))] = fields
    return bench, stats, order


def num(v):
    return float(v.rstrip("%")) if v is not None else None


def delta(a, b):
    if a in (None, 0) or b is None:
        return ""
    return f"{(b - a) * 100.0 / a:+.1f}%"


def single(bench, stats, order):
    print("| bench | gc | cyc/op | allocs/op | bytes/op | peak B |")
    print("|---|---|---:|---:|---:|---:|")
    for key in order:
        f = bench[key]
        print(f"| {key[0]} | {key[1]} | {f['cyc/op']} | {f['allocs/op']} | "
              f"{f['bytes/op']} | {f['peak']} |")
    if stats:
        print()
        print("| bench | gc | resume | sched | drain | expire | locks | scans/op |")
        print("|---|---|---:|---:|---:|---:|---:|---:|")
        for key in order:
            s = stats.get(key)
            if not s:
                continue
            ops = int(bench[key]["ops"])
            print(f"| {key[0]} | {key[1]} | {s['resume']} | {s['sched']} | {s['drain']} | "
                  f"{s['expire']} | {s['locks']} | {int(s['scans']) / ops:.1f} |")


def compare(base, new, order):
    print("| bench | gc | cyc/op | Δ | allocs/op | Δ | peak B | Δ |")
    print("|---|---|---:|---:|---:|---:|---:|---:|")
    for key in order:
        a, b = base.get(key), new.get(key)
        if a is None or b is None:
            continue
        cols = []
        for k in ("cyc/op", "allocs/op", "peak"):
            x, y = num(a[k]), num(b[k])
            cols.append(f"{a[k]} → {b[k]} | {delta(x, y)}")
        print(f"| {key[0]} | {key[1]} | " + " | ".join(cols) + " |")


def emit_baseline(path):
    bench, _, order = parse(path)
    board = "?"
    with open(path, errors="replace") as f:
        for line in f:
            m = re.search(r"BENCH_INFO board=(\S+)", line)
            if m:
                board = m.group(1)
                break
    cases = {}
    for name, gc in order:
        if name.startswith("selftest/"):
            continue  # bench.lua's startup self-test, never checked against a baseline
        cases.setdefault(name, {})[gc] = int(bench[(name, gc)]["cyc/op"])
    print(f"-- cycles/op on {board}, from scripts/bench_table.py --emit-baseline.")
    print("-- tests/async_bench fails a case slower than this + CONFIG_BENCH_MARGIN_PCT.")
    print("return {")
    for name, modes in cases.items():
        fields = ", ".join(f"{gc} = {v}" for gc, v in modes.items())
        print(f'\t["{name}"] = {{ {fields} }},')
    print("}")


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("logs", nargs="+", help="one log, or base and new logs")
    ap.add_argument("--emit-baseline", action="store_true",
                    help="print the log's cycles/op as a tests/async_bench baseline file")
    args = ap.parse_args()
    if args.emit_baseline:
        if len(args.logs) != 1:
            ap.error("--emit-baseline takes one log")
        emit_baseline(args.logs[0])
    elif len(args.logs) == 1:
        single(*parse(args.logs[0]))
    elif len(args.logs) == 2:
        base, _, order = parse(args.logs[0])
        new, _, _ = parse(args.logs[1])
        compare(base, new, order)
    else:
        ap.error("give one or two logs")
    return 0


if __name__ == "__main__":
    sys.exit(main())

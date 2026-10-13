#!/usr/bin/env python3
# Copyright (c) 2026 Nurzaman
# SPDX-License-Identifier: Apache-2.0
"""A-to-B latency from a logic-analyser CSV export (hardware lab 1).

Dockmaster's GPIO probe points toggle: pin A when a link reports a
completion from interrupt context, pin B when an owner thread calls a
driver's tx. Every edge is one event. This script pairs each A edge with
the first B edge that follows it, provided that B edge comes before the
next A edge, and prints the latency distribution.

Input: a CSV file with a header row and at least three columns: time,
channel A, channel B. Both export styles work: one row per transition
(Saleae Logic 2 "digital.csv") or one row per sample. Channel values are
0/1 (or anything int() accepts). Time is in seconds unless --time-unit
says otherwise.

Usage:
    lab_latency.py capture.csv
    lab_latency.py capture.csv --a-col 2 --b-col 3 --time-unit ns

Standard library only.
"""

import argparse
import csv
import sys

UNIT_TO_US = {"s": 1e6, "ms": 1e3, "us": 1.0, "ns": 1e-3}


def edges(rows, t_col, a_col, b_col, scale):
    """Yield (time_us, channel) for every level change on A or B."""
    prev = None
    for row in rows:
        if not row or len(row) <= max(t_col, a_col, b_col):
            continue
        t = float(row[t_col]) * scale
        cur = (int(float(row[a_col])), int(float(row[b_col])))
        if prev is not None:
            if cur[0] != prev[0]:
                yield t, "A"
            if cur[1] != prev[1]:
                yield t, "B"
        prev = cur


def pair(events):
    """Latencies in us from each A edge to the next B edge, plus unpaired A count."""
    lat = []
    pending = None
    unpaired = 0
    for t, ch in events:
        if ch == "A":
            if pending is not None:
                unpaired += 1
            pending = t
        elif pending is not None:
            lat.append(t - pending)
            pending = None
    if pending is not None:
        unpaired += 1
    return lat, unpaired


def percentile(sorted_vals, p):
    """Nearest-rank percentile of an already sorted list."""
    k = max(1, -(-len(sorted_vals) * p // 100))  # ceil(n * p / 100), at least 1
    return sorted_vals[int(k) - 1]


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("csv", help="logic-analyser CSV export")
    ap.add_argument("--time-col", type=int, default=0, help="time column (default 0)")
    ap.add_argument("--a-col", type=int, default=1, help="pin A column (default 1)")
    ap.add_argument("--b-col", type=int, default=2, help="pin B column (default 2)")
    ap.add_argument("--time-unit", choices=sorted(UNIT_TO_US), default="s",
                    help="unit of the time column (default s)")
    args = ap.parse_args()

    with open(args.csv, newline="") as f:
        reader = csv.reader(f)
        next(reader, None)  # header
        lat, unpaired = pair(edges(reader, args.time_col, args.a_col, args.b_col,
                                   UNIT_TO_US[args.time_unit]))

    if not lat:
        print("no A edge followed by a B edge", file=sys.stderr)
        return 1
    lat.sort()
    print(f"pairs     {len(lat)} (A edges without a B: {unpaired})")
    print(f"min       {lat[0]:.2f} us")
    print(f"p50       {percentile(lat, 50):.2f} us")
    print(f"p99       {percentile(lat, 99):.2f} us")
    print(f"max       {lat[-1]:.2f} us")
    return 0


if __name__ == "__main__":
    sys.exit(main())

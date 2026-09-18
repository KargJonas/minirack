#!/usr/bin/env python3
"""Select the k battery cells (default 7) whose voltages are closest together.

The variance-minimizing subset of size k is always k consecutive entries in
sorted order, so we sort and slide a window of size k instead of enumerating
all C(n, k) combinations.

Usage: ./select_cells.py [csv_file] [k]
CSV format: cell_id,voltage  (one header line, then one cell per line)
"""

import csv
import sys


def read_cells(path):
    cells = []
    with open(path, newline="") as f:
        for row in csv.DictReader(f):
            cells.append((row["cell_id"].strip(), float(row["voltage"])))
    return cells


def best_group(cells, k):
    cells = sorted(cells, key=lambda c: c[1])  # sort by voltage
    if len(cells) < k:
        raise ValueError(f"need at least {k} cells, got {len(cells)}")
    best, best_var = None, float("inf")
    for i in range(len(cells) - k + 1):
        window = cells[i : i + k]
        voltages = [v for _, v in window]
        mean = sum(voltages) / k
        var = sum((v - mean) ** 2 for v in voltages) / k
        if var < best_var:
            best, best_var = window, var
    return best, best_var


def main():
    path = sys.argv[1] if len(sys.argv) > 1 else "cells.csv"
    k = int(sys.argv[2]) if len(sys.argv) > 2 else 7

    cells = read_cells(path)
    group, var = best_group(cells, k)

    voltages = [v for _, v in group]
    print(f"Selected {k} cells (out of {len(cells)}):")
    for cell_id, v in group:
        print(f"  {cell_id}: {v:.4f} V")
    print(f"spread (max-min): {max(voltages) - min(voltages):.4f} V")
    print(f"variance:         {var:.6f} V^2")


if __name__ == "__main__":
    main()

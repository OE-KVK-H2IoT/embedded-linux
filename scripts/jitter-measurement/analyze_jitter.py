#!/usr/bin/env python3
"""
analyze_jitter.py - Parse jitter CSV logs and produce comparison statistics.

Reads one or more CSV files produced by jitter_logger.h and computes
per-file statistics (min, avg, max, stddev, 99th percentile) for
sensor_dt, frame_dt, and latency.  Optionally generates histogram plots.

Usage:
    python3 analyze_jitter.py baseline.csv stress.csv [--plot]
    python3 analyze_jitter.py baseline.csv stress.csv rt_baseline.csv rt_stress.csv --plot

Requirements:
    pip install numpy matplotlib  (matplotlib only needed with --plot)
"""

import argparse
import csv
import os
import sys

import numpy as np


# ---------- data loading ----------

def load_csv(path):
    """
    Load a jitter CSV file.

    Expected columns:
        timestamp_ns, sensor_dt_ns, frame_dt_ns, latency_ns,
        roll_deg, pitch_deg

    Returns a dict of numpy arrays keyed by column name.
    """
    columns = {
        "timestamp_ns": [],
        "sensor_dt_ns": [],
        "frame_dt_ns": [],
        "latency_ns": [],
        "roll_deg": [],
        "pitch_deg": [],
    }

    with open(path, newline="") as f:
        reader = csv.DictReader(f)
        for row in reader:
            for key in columns:
                try:
                    columns[key].append(float(row[key]))
                except (KeyError, ValueError):
                    columns[key].append(0.0)

    return {k: np.array(v) for k, v in columns.items()}


# ---------- statistics ----------

def compute_stats(values_ns):
    """
    Compute summary statistics for a timing array (in nanoseconds).
    Returns a dict with values converted to milliseconds.
    """
    if len(values_ns) == 0:
        return {"min": 0, "avg": 0, "max": 0, "std": 0, "p99": 0, "n": 0}

    ms = values_ns / 1_000_000.0  # convert to milliseconds
    return {
        "min":  float(np.min(ms)),
        "avg":  float(np.mean(ms)),
        "max":  float(np.max(ms)),
        "std":  float(np.std(ms)),
        "p99":  float(np.percentile(ms, 99)),
        "n":    len(ms),
    }


def detect_dropped_frames(frame_dt_ns, threshold_ms=25.0):
    """
    Detect frames where frame_dt exceeds the threshold (default 25 ms,
    which corresponds to below 40 fps -- a clear miss at 60 Hz target).

    Returns (count, indices).
    """
    ms = frame_dt_ns / 1_000_000.0
    mask = ms > threshold_ms
    return int(np.sum(mask)), np.where(mask)[0]


# ---------- output formatting ----------

def print_table(results):
    """
    Print a comparison table to stdout.

    results: list of (filename, stats_dict) where stats_dict has keys
             'sensor_dt', 'frame_dt', 'latency', 'dropped'.
    """
    # Header
    header = f"{'File':<30} {'Metric':<12} {'Min':>8} {'Avg':>8} " \
             f"{'Max':>8} {'Std':>8} {'P99':>8} {'N':>8}"
    sep = "-" * len(header)

    print()
    print(sep)
    print(header)
    print(sep)

    for filename, stats in results:
        short = os.path.basename(filename)
        for metric_name in ("sensor_dt", "frame_dt", "latency"):
            s = stats[metric_name]
            print(f"{short:<30} {metric_name:<12} "
                  f"{s['min']:8.2f} {s['avg']:8.2f} "
                  f"{s['max']:8.2f} {s['std']:8.2f} "
                  f"{s['p99']:8.2f} {s['n']:8d}")
            short = ""  # only show filename on first row

        dropped = stats["dropped"]
        print(f"{'':30} {'dropped':12} {dropped:>8d}")
        print(sep)

    print("  (All timing values in milliseconds)")
    print()


# ---------- plotting ----------

def generate_plots(results, output_prefix="jitter"):
    """
    Generate histogram comparison plots and save as PNG.

    results: list of (filename, data_dict) where data_dict has raw arrays.
    """
    try:
        import matplotlib
        matplotlib.use("Agg")  # headless backend
        import matplotlib.pyplot as plt
    except ImportError:
        print("ERROR: matplotlib is required for --plot. "
              "Install with: pip install matplotlib", file=sys.stderr)
        return

    metrics = [
        ("sensor_dt_ns", "Sensor dt (ms)", "sensor_dt"),
        ("frame_dt_ns",  "Frame dt (ms)",  "frame_dt"),
        ("latency_ns",   "Latency (ms)",   "latency"),
    ]

    for col_key, xlabel, plot_name in metrics:
        fig, ax = plt.subplots(figsize=(10, 6))

        for filename, data in results:
            values_ms = data[col_key] / 1_000_000.0
            label = os.path.basename(filename)
            ax.hist(values_ms, bins=100, alpha=0.5, label=label,
                    edgecolor="none")

        ax.set_xlabel(xlabel)
        ax.set_ylabel("Count")
        ax.set_title(f"{xlabel} Distribution")
        ax.legend()
        ax.grid(True, alpha=0.3)

        out_path = f"{output_prefix}_{plot_name}.png"
        fig.savefig(out_path, dpi=150, bbox_inches="tight")
        plt.close(fig)
        print(f"Plot saved: {out_path}")


# ---------- main ----------

def main():
    parser = argparse.ArgumentParser(
        description="Analyze jitter/latency CSV logs and compare results.")
    parser.add_argument("files", nargs="+", metavar="CSV",
                        help="One or more CSV files from jitter_logger")
    parser.add_argument("--plot", action="store_true",
                        help="Generate PNG histogram plots")
    parser.add_argument("--plot-prefix", type=str, default="jitter",
                        help="Prefix for plot filenames (default: jitter)")
    parser.add_argument("--drop-threshold", type=float, default=25.0,
                        help="Frame dt threshold (ms) for dropped frame "
                             "detection (default: 25.0)")
    args = parser.parse_args()

    # Load all files
    all_data = []
    for path in args.files:
        if not os.path.isfile(path):
            print(f"WARNING: {path} not found, skipping.", file=sys.stderr)
            continue
        data = load_csv(path)
        all_data.append((path, data))

    if not all_data:
        print("ERROR: No valid CSV files to analyze.", file=sys.stderr)
        sys.exit(1)

    # Compute statistics
    stat_results = []
    for filename, data in all_data:
        stats = {
            "sensor_dt": compute_stats(data["sensor_dt_ns"]),
            "frame_dt":  compute_stats(data["frame_dt_ns"]),
            "latency":   compute_stats(data["latency_ns"]),
        }
        dropped_count, _ = detect_dropped_frames(
            data["frame_dt_ns"], threshold_ms=args.drop_threshold)
        stats["dropped"] = dropped_count
        stat_results.append((filename, stats))

    # Print comparison table
    print_table(stat_results)

    # Generate plots if requested
    if args.plot:
        generate_plots(all_data, output_prefix=args.plot_prefix)


if __name__ == "__main__":
    main()

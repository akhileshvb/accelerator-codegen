#!/usr/bin/env python3
"""Plot SEC EDGAR aux-state workload speedup across write-frequency sweep points."""

from __future__ import annotations

import argparse
import csv
import math
from pathlib import Path

import matplotlib.pyplot as plt

ROOT = Path(__file__).resolve().parents[3]
DEFAULT_BUILD_DIR = ROOT / "build-release"
DEFAULT_OUTPUT = Path(__file__).resolve().parent / "results" / "sec_edgar_aux_write_freq_speedup.png"

QUERIES = (1, 2, 3, 4, 6, 24)
WRITE_FREQS = (0, 1, 10, 50)
COLORS = {1: "#1f77b4", 2: "#ff7f0e", 3: "#2ca02c", 4: "#d62728", 6: "#9467bd", 24: "#8c564b"}
MARKERS = {1: "o", 2: "s", 3: "^", 4: "D", 6: "v", 24: "P"}
ANNOTATION_FONTSIZE = 11
MIN_LOG_LABEL_GAP = 0.29
FREQ_X_BASE = {0: 3.0, 1: 2.6, 10: 2.0, 50: 1.4}
X_COLUMN_STAGGER = 2.3
LOG_CLUSTER_GAP = 0.35


def cluster_points(points: list[tuple[int, float]]) -> list[list[tuple[int, float]]]:
	"""Split sorted points into log-scale clusters separated by large gaps."""
	if not points:
		return []
	clusters: list[list[tuple[int, float]]] = [[points[0]]]
	for query, speedup in points[1:]:
		prev_speedup = clusters[-1][-1][1]
		if math.log10(speedup) - math.log10(prev_speedup) > LOG_CLUSTER_GAP:
			clusters.append([(query, speedup)])
		else:
			clusters[-1].append((query, speedup))
	return clusters


def spread_log_positions(values: list[float], min_log_gap: float) -> list[float]:
	"""Evenly space label y-positions in log-space with a minimum gap between boxes."""
	if not values:
		return []
	n = len(values)
	if n == 1:
		return values[:]

	log_values = [math.log10(v) for v in values]
	lo, hi = min(log_values), max(log_values)
	min_span = min_log_gap * (n - 1)
	span = max(hi - lo, min_span)
	center = (lo + hi) / 2
	start = center - span / 2
	step = span / (n - 1)
	return [10 ** (start + i * step) for i in range(n)]


def label_x_position(freq: int, index: int, count: int) -> float:
	"""Stagger labels across multiple x-columns for crowded write-frequency buckets."""
	base = FREQ_X_BASE.get(freq, 1.5)
	if count <= 3:
		return freq + base + (index % 2) * (X_COLUMN_STAGGER * 0.55)
	columns = 3 if count >= 6 else 2
	column = index % columns
	lane = index // columns
	return freq + base + column * X_COLUMN_STAGGER + lane * 0.3


def results_path(build_dir: Path, query: int) -> Path:
	return build_dir / f"q{query}_aux_bench_sec_edgar_sf3" / "results.csv"


def load_speedups(results_csv: Path, metric_col: str) -> dict[int, float]:
	by_freq: dict[int, float] = {}
	with results_csv.open(newline="") as handle:
		reader = csv.DictReader(handle)
		for row in reader:
			if not row.get("write_frequency_pct"):
				continue
			freq = int(float(row["write_frequency_pct"]))
			by_freq[freq] = float(row[metric_col])
	return by_freq


def format_speedup(value: float) -> str:
	if value >= 100:
		return f"{value:.0f}×"
	if value >= 10:
		return f"{value:.1f}×"
	return f"{value:.2f}×"


def annotate_speedups(ax, query_data: dict[int, dict[int, float]]) -> None:
	"""Place boxed labels per write-frequency column with log-spaced y positions."""
	for freq in WRITE_FREQS:
		points = [(query, query_data[query][freq]) for query in QUERIES if freq in query_data[query]]
		if not points:
			continue

		points.sort(key=lambda item: item[1])
		placed: list[tuple[int, float, float, int]] = []
		label_index = 0
		for cluster in cluster_points(points):
			speedups = [speedup for _, speedup in cluster]
			label_ys = spread_log_positions(speedups, MIN_LOG_LABEL_GAP)
			for (query, speedup), label_y in zip(cluster, label_ys):
				placed.append((query, speedup, label_y, label_index))
				label_index += 1

		for query, speedup, label_y, index in placed:
			x_label = label_x_position(freq, index, len(points))
			ax.annotate(
				format_speedup(speedup),
				(freq, speedup),
				xytext=(x_label, label_y),
				textcoords="data",
				ha="left",
				va="center",
				fontsize=ANNOTATION_FONTSIZE,
				fontweight="medium",
				color=COLORS[query],
				bbox={
					"boxstyle": "round,pad=0.3",
					"facecolor": "white",
					"edgecolor": COLORS[query],
					"linewidth": 1.4,
					"alpha": 0.95,
				},
				arrowprops={
					"arrowstyle": "-",
					"color": COLORS[query],
					"linewidth": 0.9,
					"shrinkA": 4,
					"shrinkB": 4,
					"alpha": 0.75,
				},
				zorder=5,
			)


def main() -> None:
	parser = argparse.ArgumentParser(description=__doc__)
	parser.add_argument("--build-dir", type=Path, default=DEFAULT_BUILD_DIR)
	parser.add_argument("--output", type=Path, default=DEFAULT_OUTPUT)
	parser.add_argument(
		"--metric",
		choices=("workload", "query_only"),
		default="workload",
		help="Speedup column (default: workload_speedup)",
	)
	args = parser.parse_args()

	metric_col = "workload_speedup" if args.metric == "workload" else "query_only_speedup"
	metric_label = (
		"Workload speedup (queries + writes)"
		if args.metric == "workload"
		else "Query-only speedup (aux hit vs DuckDB ref)"
	)

	fig, ax = plt.subplots(figsize=(12, 7.5))
	query_data: dict[int, dict[int, float]] = {}

	for query in QUERIES:
		csv_path = results_path(args.build_dir, query)
		if not csv_path.is_file():
			raise SystemExit(f"missing results: {csv_path}")

		by_freq = load_speedups(csv_path, metric_col)
		query_data[query] = by_freq
		freqs = [f for f in WRITE_FREQS if f in by_freq]
		speedups = [by_freq[f] for f in freqs]

		ax.plot(
			freqs,
			speedups,
			label=f"Q{query}",
			color=COLORS[query],
			marker=MARKERS[query],
			linewidth=2,
			markersize=8,
			zorder=3,
		)

	annotate_speedups(ax, query_data)

	ax.set_xlabel("Write frequency (%)", fontsize=12)
	ax.set_ylabel(f"{metric_label} (×)", fontsize=12)
	ax.set_title("SEC EDGAR aux state speedup vs DuckDB (SF3)", fontsize=13)
	ax.set_xticks(list(WRITE_FREQS))
	ax.set_xticklabels([f"{f}%" for f in WRITE_FREQS])
	ax.set_xlim(-1, 76)
	ax.axhline(1.0, color="#888888", linestyle="--", linewidth=1, label="DuckDB baseline")
	ax.set_yscale("log")
	ax.grid(True, which="both", linestyle=":", alpha=0.5)
	ax.legend(loc="upper right", framealpha=0.9, ncol=2)
	fig.tight_layout()

	args.output.parent.mkdir(parents=True, exist_ok=True)
	fig.savefig(args.output, dpi=160)
	print(f"wrote {args.output}")


if __name__ == "__main__":
	main()

#!/usr/bin/env python3
"""Measure and plot SEC EDGAR aux-state memory overhead vs DuckDB query memory."""

from __future__ import annotations

import argparse
import csv
import os
import subprocess
from pathlib import Path

import matplotlib.pyplot as plt
import numpy as np

ROOT = Path(__file__).resolve().parents[2]
DEFAULT_BUILD_DIR = ROOT / "build-release"
DEFAULT_MEASURE_BIN = DEFAULT_BUILD_DIR / "measure_sec_edgar_aux_memory"
DEFAULT_OUTPUT = Path(__file__).resolve().parent / "results" / "sec_edgar_aux_memory_overhead.png"
DEFAULT_CSV = Path(__file__).resolve().parent / "results" / "sec_edgar_aux_memory.csv"
QUERIES = (1, 2, 3, 4, 6, 24)


def run_measurement(measure_bin: Path, mode: str, query: int, env: dict[str, str]) -> float:
	result = subprocess.run(
	    [str(measure_bin), mode, str(query)],
	    check=True,
	    capture_output=True,
	    text=True,
	    env=env,
	)
	lines = [line.strip() for line in result.stdout.splitlines() if line.strip()]
	if len(lines) < 2:
		raise RuntimeError(f"unexpected output for {mode} Q{query}: {result.stdout!r}")
	reader = csv.DictReader(lines)
	row = next(reader)
	return float(row["measured_mb"])


def collect_rows(measure_bin: Path, env: dict[str, str]) -> list[dict[str, float | str]]:
	rows = []
	for query in QUERIES:
		duckdb_mb = run_measurement(measure_bin, "duckdb", query, env)
		aux_mb = run_measurement(measure_bin, "aux", query, env)
		rows.append(
		    {
		        "query": f"Q{query}",
		        "duckdb_query_peak_mb": duckdb_mb,
		        "aux_state_mb": aux_mb,
		    }
		)
	return rows


def write_csv(rows: list[dict[str, float | str]], path: Path) -> None:
	path.parent.mkdir(parents=True, exist_ok=True)
	with path.open("w", newline="") as handle:
		writer = csv.DictWriter(
		    handle,
		    fieldnames=["query", "duckdb_query_peak_mb", "aux_state_mb"],
		)
		writer.writeheader()
		writer.writerows(rows)


def plot(rows: list[dict[str, float | str]], output: Path) -> None:
	labels = [str(row["query"]) for row in rows]
	duckdb_peak = np.array([float(row["duckdb_query_peak_mb"]) for row in rows], dtype=float)
	aux_state = np.array([float(row["aux_state_mb"]) for row in rows], dtype=float)

	x = np.arange(len(labels))
	width = 0.36
	fig, ax = plt.subplots(figsize=(11, 6.5))
	ax.bar(
	    x - width / 2,
	    duckdb_peak,
	    width,
	    label="DuckDB query peak (RSS delta)",
	    color="#DD8452",
	    edgecolor="white",
	    linewidth=0.6,
	)
	ax.bar(
	    x + width / 2,
	    aux_state,
	    width,
	    label="Aux state (RSS delta after build)",
	    color="#4C72B0",
	    edgecolor="white",
	    linewidth=0.6,
	)

	for idx, (duck, aux) in enumerate(zip(duckdb_peak, aux_state)):
		if duck > 0:
			ax.text(
			    x[idx] - width / 2,
			    duck,
			    f"{duck:.0f}",
			    ha="center",
			    va="bottom",
			    fontsize=10,
			    color="#8B4513",
			)
		if aux > 0:
			ax.text(
			    x[idx] + width / 2,
			    aux,
			    f"{aux:.0f}",
			    ha="center",
			    va="bottom",
			    fontsize=10,
			    color="#1F4E79",
			)

	ax.set_yscale("log")
	ax.set_ylabel("Memory (MB, log scale)", fontsize=12)
	ax.set_xlabel("Query", fontsize=12)
	ax.set_title("SEC EDGAR aux state memory vs DuckDB query memory (SF3, RSS)", fontsize=13)
	ax.set_xticks(x)
	ax.set_xticklabels(labels)
	ax.grid(axis="y", which="both", linestyle=":", alpha=0.5)
	ax.legend(loc="upper left", framealpha=0.95)
	fig.tight_layout()

	output.parent.mkdir(parents=True, exist_ok=True)
	fig.savefig(output, dpi=160)
	print(f"wrote {output}")


def main() -> None:
	parser = argparse.ArgumentParser(description=__doc__)
	parser.add_argument("--build-dir", type=Path, default=DEFAULT_BUILD_DIR)
	parser.add_argument("--measure-bin", type=Path, default=None)
	parser.add_argument("--output", type=Path, default=DEFAULT_OUTPUT)
	parser.add_argument("--csv", type=Path, default=DEFAULT_CSV)
	args = parser.parse_args()

	measure_bin = args.measure_bin or (args.build_dir / "measure_sec_edgar_aux_memory")
	if not measure_bin.is_file():
		raise SystemExit(f"missing measurement binary: {measure_bin}")

	rows = collect_rows(measure_bin, os.environ.copy())
	write_csv(rows, args.csv)
	print(f"wrote {args.csv}")
	plot(rows, args.output)


if __name__ == "__main__":
	main()

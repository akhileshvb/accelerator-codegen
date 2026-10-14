#!/usr/bin/env python3
"""Alias of plot_rf_multi_sweep.py, which plots both interval sweeps and write-rate sweeps (wps<rate>/ directories).

  python3 eval/compare/plot_wps_sweep.py <out_dir>

Kept so that older result scripts importing stat / errplot / key / workload_speedup from here keep working.
"""
from plot_rf_multi_sweep import main
from plotlib import errplot, stat  # noqa: F401
from workload import key, workload_speedup  # noqa: F401

if __name__ == "__main__":
    main()

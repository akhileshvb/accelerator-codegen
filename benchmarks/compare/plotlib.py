"""Helpers shared by the aux_compare plotters: repetition statistics, error bars, headless matplotlib.

(Workload speedup math lives in workload.py; load code stays with each plotter since the result layouts differ.)
"""
import math
import statistics

import matplotlib

matplotlib.use("Agg")

# the large-font style of the per-query / sweep figures
LARGE_FONTS = {"font.size": 15, "axes.titlesize": 17, "axes.labelsize": 15, "xtick.labelsize": 14,
               "ytick.labelsize": 14, "legend.fontsize": 14}


def stat(values):
    """(median, low, high) of the repetitions' values; None when there are none (None entries are skipped)."""
    v = sorted(x for x in values if x is not None)
    if not v:
        return None
    return statistics.median(v), v[0], v[-1]


def geomean(values):
    return math.exp(sum(math.log(x) for x in values) / len(values))


def errplot(ax, xs, stats, **kw):
    """Median points with min..max whiskers; stats[i] is a stat() tuple or None (point skipped)."""
    pts = [(x, s) for x, s in zip(xs, stats) if s]
    if not pts:
        return
    ax.errorbar([x for x, _ in pts], [s[0] for _, s in pts],
                yerr=[[s[0] - s[1] for _, s in pts], [s[2] - s[0] for _, s in pts]], capsize=3, **kw)

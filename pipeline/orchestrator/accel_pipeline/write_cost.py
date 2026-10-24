"""What the quick tier's per-write timings predict for the full aux_correctness gate.

The differential harness times, on the subset dataset (host_contract.md, ACCEL_SUITE_REPORT), the accelerator's
maintenance for each suite transaction (the engine's own cost of the SQL excluded; the whole commit and first read
when the engine does not report it), each adversarial probe's, and the state build.
project() scales them to the full dataset and sums what the full gate would spend on writes.

The model, deliberately conservative and linear where it cannot tell:
- scale of a table set = bytes of its .tbl files in the full dataset / in the subset;
- a write costing at least REBUILD_SHARE of the subset build is taken to rebuild the state: it scales with the
  tables the state depends on, linearly;
- other writes scale with the square root of their tables' scale (index probes, cache effects), linearly for bulk kinds
  (their statements touch a share of the table);
- the differential runs twice (N threads, then one), and the gate's time budget is a share of its timeout.
Builds are not projected: any design pays them, and a few-millisecond subset build is mostly fixed overhead, too
loose a base for a linear extrapolation to fail a gate on; a write that rebuilds is caught as a write.
It cannot see a write whose cost is constant on the subset but grows with data in some other way (a scan of a table
the subset keeps whole), nor the reference queries' and reads' own time, which the budget share leaves room for.
"""

from __future__ import annotations

import json
import math
from collections.abc import Callable, Iterable
from dataclasses import dataclass, field
from pathlib import Path

REBUILD_SHARE = 0.5
MIN_REBUILD_MS = 1.0
SUITE_REPORT_STEM = "suite_quick_report"


def report_paths(gates_dir: Path) -> list[Path]:
    return [gates_dir / f"{SUITE_REPORT_STEM}.json", gates_dir / f"{SUITE_REPORT_STEM}_threads1.json"]


def table_scale(full_dir: Path | None, subset_dir: Path | None) -> Callable[[Iterable[str]], float]:
    """scale(tables): full / subset bytes of those tables' .tbl files (all common tables when empty); 1.0 when unknown."""
    def size(directory: Path | None, table: str) -> int:
        path = directory / f"{table}.tbl" if directory else None
        return path.stat().st_size if path and path.is_file() else 0

    def scale(tables: Iterable[str]) -> float:
        names = [t.lower() for t in tables]
        if not names and full_dir and full_dir.is_dir():
            names = [f.stem for f in full_dir.glob("*.tbl")]
        full = sum(size(full_dir, t) for t in names)
        sub = sum(size(subset_dir, t) for t in names)
        return max(1.0, full / sub) if full and sub else 1.0
    return scale


@dataclass
class Projection:
    total_s: float = 0.0
    budget_s: float = 0.0
    lines: list[str] = field(default_factory=list)  # one per transaction kind, the costliest first
    worst: str = ""

    @property
    def over(self) -> bool:
        return self.total_s > self.budget_s

    def summary(self) -> str:
        return f"projected full-gate write time ~{self.total_s:.0f} s (budget {self.budget_s:.0f} s)"


def _maintenance_s(cost: dict) -> float:
    """The accelerator's own time for one write: what the engine reports, else the whole commit and first read."""
    return cost["maint_ms"] / 1000.0 if cost.get("maint_ms", -1) >= 0 else (cost["write_ms"] + cost["read_ms"]) / 1000.0


def _kind_line(kind: str, tables: tuple[str, ...], subset_ms: float, per_write_s: float, factor: float, rebuild: bool) -> str:
    on = f" on {','.join(tables)}" if tables else ""
    why = ": as costly as a rebuild; likely a full rebuild per write, make it incremental" if rebuild else ""
    return f"{kind}{on}: {subset_ms:.0f} ms on the subset, projected ~{per_write_s:.1f} s per write at full scale (x{factor:.0f}){why}"


def project(reports: list[dict], *, profiles: dict[str, tuple[str, tuple[str, ...]]], dependent: list[str],
            scale: Callable[[Iterable[str]], float], probes_full: int, budget_s: float) -> Projection:
    """The full gate's projected write and build seconds from the suite reports of the quick tier's differential runs."""
    r_dep = scale(dependent)
    out = Projection(budget_s=budget_s)
    worst: dict[tuple[str, tuple[str, ...]], tuple[float, str]] = {}
    for report in reports:
        build_s = float(report.get("build_seconds") or 0.0)
        costs = [(profiles.get(c["txn"], ("other", ())), _maintenance_s(c)) for c in report.get("write_costs") or []]
        probe_ms = [float(ms) for ms in report.get("probe_costs_ms") or []]
        if probe_ms:  # the adversarial probes: every table the state depends on, scaled to the full gate's probe count
            costs += [(("adversarial_probe", tuple(dependent)), sum(probe_ms) / len(probe_ms) / 1000.0)] * probes_full
        for (kind, tables), cost_s in costs:
            rebuild = cost_s * 1000 >= MIN_REBUILD_MS and cost_s >= REBUILD_SHARE * build_s
            r_touch = scale(tables) if tables else r_dep
            factor = max(r_dep if rebuild else 1.0, r_touch if kind.startswith("bulk") else math.sqrt(r_touch))
            out.total_s += cost_s * factor
            line = _kind_line(kind, tables, cost_s * 1000, cost_s * factor, factor, rebuild)
            if cost_s * factor > worst.get((kind, tables), (0.0, ""))[0]:
                worst[(kind, tables)] = (cost_s * factor, line)
    ranked = sorted(worst.values(), reverse=True)
    out.lines = [line for _, line in ranked]
    out.worst = out.lines[0] if out.lines else ""
    return out


def load_reports(gates_dir: Path) -> list[dict]:
    reports = []
    for path in report_paths(gates_dir):
        try:
            report = json.loads(path.read_text(encoding="utf-8"))
        except (OSError, ValueError):
            continue
        if "write_costs" in report:
            reports.append(report)
    return reports

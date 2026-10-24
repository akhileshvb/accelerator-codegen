"""What the quick tier's per-write timings predict for the full aux_correctness gate.

The differential harness times, on the subset dataset (host_contract.md, ACCEL_SUITE_REPORT), the accelerator's
maintenance for each suite transaction (the engine's own cost of the SQL excluded; the whole commit and first read
when the engine does not report it), each adversarial probe's, and the state build.
project() scales them to the full dataset and sums what the full gate would spend on writes.

The model, deliberately conservative and linear where it cannot tell:
- scale of a table set = rows of those tables in the engine's full dataset / in the subset (test_db.CutDatasets.scale);
- a write costing at least REBUILD_SHARE of the subset build is taken to rebuild the state: it scales with the
  tables the state depends on, linearly;
- other writes scale with the square root of their tables' scale (index probes, cache effects), linearly for bulk kinds
  (their statements touch a share of the table);
- the differential runs twice (N threads, then one), and the gate's time budget is a share of its timeout.
Builds are projected by check_report.py, which times them on a subset big enough to measure.
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


def is_rebuild(cost_s: float, build_s: float, action: str = "") -> bool:
    """A write the accelerator maintains by rebuilding: it says so (the report's `action`, when it has one), or it
    costs about as much as building the state from scratch."""
    return "rebuild" in action.lower() or (cost_s * 1000 >= MIN_REBUILD_MS and cost_s >= REBUILD_SHARE * build_s)


def unfaithful_rebuilds(reports: list[dict], txn_sql: dict[str, str], coverage: list[dict],
                        statements: Callable[[str], list[tuple[str, str]]]) -> list[str]:
    """Suite transactions that rebuild the state although analysis.json's mutation_coverage maintains every
    (table, mutation) they write as supported_delta: one line each, naming the transaction and those writes."""
    delta = {(str(c.get("table", "")).lower(), str(c.get("mutation", "")).lower())
             for c in coverage if c.get("behavior") == "supported_delta"}
    out: dict[str, str] = {}
    for report in reports:
        build_s = float(report.get("build_seconds") or 0.0)
        for cost in report.get("write_costs") or []:
            writes = sorted({(t, verb.lower()) for verb, t in statements(txn_sql.get(cost["txn"], ""))})
            if writes and all(w in delta for w in writes) and is_rebuild(_maintenance_s(cost), build_s, cost.get("action", "")):
                out[cost["txn"]] = (f"{cost['txn']} ({', '.join(f'{v} {t}' for t, v in writes)}): maintenance costs as much as "
                                    "a full build; analysis.json maintains these writes incrementally (supported_delta)")
    return list(out.values())


def _maintenance_s(cost: dict) -> float:
    """The accelerator's own time for one write: what the engine reports, else the whole commit and first read."""
    return cost["maint_ms"] / 1000.0 if cost.get("maint_ms", -1) >= 0 else (cost["write_ms"] + cost["read_ms"]) / 1000.0


def _kind_line(kind: str, tables: tuple[str, ...], action: str, subset_ms: float, per_write_s: float, factor: float,
               rebuild: bool) -> str:
    on = f" on {','.join(tables)}" if tables else ""
    how = f" via {action}" if action else ""
    why = ": as costly as a rebuild; likely a full rebuild per write, make it incremental" if rebuild else ""
    return f"{kind}{on}{how}: {subset_ms:.0f} ms on the subset, projected ~{per_write_s:.1f} s per write at full scale (x{factor:.0f}){why}"


def project(reports: list[dict], *, profiles: dict[str, tuple[str, tuple[str, ...]]], dependent: list[str],
            scale: Callable[[Iterable[str]], float], probes_full: int, budget_s: float) -> Projection:
    """The full gate's projected write seconds from the suite reports of the quick tier's differential runs."""
    r_dep = scale(dependent)
    out = Projection(budget_s=budget_s)
    worst: dict[tuple[str, tuple[str, ...]], tuple[float, str]] = {}
    for report in reports:
        build_s = float(report.get("build_seconds") or 0.0)
        costs = [(*profiles.get(c["txn"], ("other", ())), c.get("action", ""), _maintenance_s(c))
                 for c in report.get("write_costs") or []]
        probe_ms = [float(ms) for ms in report.get("probe_costs_ms") or []]
        if probe_ms:  # the adversarial probes: every table the state depends on, scaled to the full gate's probe count
            costs += [("adversarial_probe", tuple(dependent), "", sum(probe_ms) / len(probe_ms) / 1000.0)] * probes_full
        for kind, tables, action, cost_s in costs:
            rebuild = is_rebuild(cost_s, build_s, action)
            r_touch = scale(tables) if tables else r_dep
            factor = max(r_dep if rebuild else 1.0, r_touch if kind.startswith("bulk") else math.sqrt(r_touch))
            out.total_s += cost_s * factor
            line = _kind_line(kind, tables, action, cost_s * 1000, cost_s * factor, factor, rebuild)
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

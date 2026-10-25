"""The write-cost projection of quick_check: per-write timings on the subset scaled to the full gate."""

from __future__ import annotations

import json

import pytest

from accel_pipeline import quick, write_cost
from accel_pipeline.config import QuickTierConfig
from accel_pipeline.layout import suite_dir
from accel_pipeline.paths import gates_dir_for_query, query_artifacts_dir
from accel_pipeline.test_db import CutDatasets
from conftest import WORKLOAD, Gate

PROFILES = {"001.sql": ("point_update", ("orders",)), "002.sql": ("bulk_delete", ("orders",))}


def scale_of(**ratios):
    return lambda tables: max([ratios.get(t, 1.0) for t in tables] or list(ratios.values()))


def report(write_ms, read_ms=0.0, build_s=0.05, probes=(), maint_ms=-1):
    return {"build_seconds": build_s, "probe_costs_ms": list(probes),
            "write_costs": [{"txn": "001.sql", "write_ms": write_ms, "read_ms": read_ms, "maint_ms": maint_ms}]}


def project(reports, ratio=250.0, budget_s=30.0, probes_full=5):
    return write_cost.project(reports, profiles=PROFILES, dependent=["orders"], scale=scale_of(orders=ratio),
                              probes_full=probes_full, budget_s=budget_s)


def test_a_write_as_costly_as_a_rebuild_is_projected_linearly_and_named():
    out = project([report(write_ms=12, read_ms=168)])  # 180 ms against a 50 ms build
    assert out.over
    assert out.worst == ("point_update on orders: 180 ms on the subset, projected ~45.0 s per write at full scale (x250): "
                         "as costly as a rebuild; likely a full rebuild per write, make it incremental")


def test_the_engines_own_cost_of_the_sql_is_not_the_accelerators():
    slow_sql = report(write_ms=60, read_ms=0.3, build_s=0.08, maint_ms=0.05)  # a 60 ms commit, 50 us of maintenance
    out = project([slow_sql], ratio=1000.0)
    assert not out.over and "rebuild" not in out.worst and out.lines[0].startswith("point_update on orders: 0 ms on")
    assert project([report(write_ms=60, read_ms=0.3, build_s=0.08)], ratio=1000.0).over  # unreported: all of it counts


def test_an_incremental_write_grows_with_the_square_root_only():
    out = project([report(write_ms=0.4, read_ms=0.1, build_s=0.2)], ratio=10000.0, budget_s=100.0)
    assert not out.over and "rebuild" not in out.worst
    assert out.lines[0].startswith("point_update on orders: 0 ms on the subset, projected ~0.1 s per write at full scale (x100)")


def test_adversarial_probes_are_projected_over_the_full_gates_probe_count():
    out = project([report(0.1, build_s=1.0, probes=[30.0, 50.0])], ratio=100.0, probes_full=5)
    assert any(line.startswith("adversarial_probe on orders: 40 ms") for line in out.lines)


def test_the_projection_passes_within_the_budget_and_reports_the_costliest_kind_first():
    out = project([report(write_ms=1, read_ms=1, build_s=1.0)], ratio=10.0, budget_s=1000.0)
    assert not out.over and out.summary().startswith("projected full-gate write time")
    assert out.lines == sorted(out.lines, key=lambda line: -float(line.split("~")[1].split(" s")[0]))


def test_reports_are_read_from_both_thread_runs_and_ignore_a_report_without_costs(tmp_path):
    (tmp_path / "suite_quick_report.json").write_text(json.dumps(report(1)))
    (tmp_path / "suite_quick_report_threads1.json").write_text(json.dumps({"ok": True}))
    assert len(write_cost.load_reports(tmp_path)) == 1


# quick_check wiring ---------------------------------------------------------------------------------------

def stage_suite(config, subset_rows, full_rows, reports):
    """A suite, test databases of those row counts and the reports of a quick differential, for the real projection code."""
    root = config.repo_root
    suite = root / suite_dir("tpch", 6)
    (suite / "txns").mkdir(parents=True)
    (suite / "txns" / "001.sql").write_text("UPDATE orders SET o_flag = 'B' WHERE o_orderkey = 1;\n")
    (suite / "suite.yaml").write_text("txns:\n  001.sql: {kind: point_update}\npoint_writes: {max_probes: 5}\n")
    cuts = CutDatasets(root / "subset", root / "tiny", {"source_rows": {"orders": full_rows}, "subset_rows": {"orders": subset_rows}})
    art = query_artifacts_dir(root, 6, workload=WORKLOAD)
    art.mkdir(parents=True, exist_ok=True)
    (art / "analysis.json").write_text(json.dumps({"correctness_facts": {"dependent_tables": [{"name": "orders"}]}}))
    gates = gates_dir_for_query(art)
    gates.mkdir(parents=True, exist_ok=True)
    for path, rep in zip(write_cost.report_paths(gates), reports):
        path.write_text(json.dumps(rep))
    return WORKLOAD, cuts


class ReportingGate(Gate):
    """A passing gate that leaves the suite reports of a differential behind."""

    def __init__(self, monkeypatch, config, workload, reports):
        super().__init__(monkeypatch)
        self.reports = reports
        self.config, self.workload = config, workload

    def run_gate(self, config, name, query, workload, **kwargs):
        gates = gates_dir_for_query(query_artifacts_dir(config.repo_root, query, workload=workload))
        for path, rep in zip(write_cost.report_paths(gates), self.reports):
            path.write_text(json.dumps(rep))
        return super().run_gate(config, name, query, workload, **kwargs)


def check(config, monkeypatch, write_ms, timeout=30.0):
    reports = [report(write_ms=write_ms, build_s=0.05)] * 2
    workload, cuts = stage_suite(config, 40, 10_000, [])  # 250x the subset
    monkeypatch.setattr(quick, "GATE_RUN_TIMEOUT_S", timeout / config.quick_tier.write_cost_budget_share)
    gate = ReportingGate(monkeypatch, config, workload, reports)
    monkeypatch.setattr(quick, "ensure_cut_datasets", lambda *a: cuts)
    return quick.run_quick_check(config, workload, 6), gate


def test_a_write_that_rebuilds_fails_quick_check_with_an_actionable_message(config, monkeypatch):
    result, _ = check(config, monkeypatch, write_ms=180)
    assert result.status == "fail" and result.failure.kind == "code"
    assert "point_update on orders: 180 ms on the subset, projected ~45.0 s per write" in result.summary
    assert "make it incremental" in result.summary and "would time out" in result.summary
    assert quick.admission_refusal(config, WORKLOAD, 6, "aux_correctness")  # a failed check admits nothing


def test_an_incremental_write_passes_and_the_projection_is_reported(config, monkeypatch):
    result, _ = check(config, monkeypatch, write_ms=0.5)
    assert result.status == "pass" and "projected full-gate write time" in result.report["writes_total"]
    assert result.report["writes"] and json.loads(quick.render_quick_result(result))["writes"] == result.report["writes"]


def test_a_stale_report_from_an_earlier_run_is_not_projected(config, monkeypatch):
    workload, cuts = stage_suite(config, 40, 10_000, [report(write_ms=500)] * 2)
    Gate(monkeypatch)  # leaves no report
    monkeypatch.setattr(quick, "ensure_cut_datasets", lambda *a: cuts)
    assert quick.run_quick_check(config, workload, 6).status == "pass"


# gate admission -------------------------------------------------------------------------------------------

def test_heavy_gates_are_admitted_only_after_a_passing_quick_check_on_this_code(config, monkeypatch):
    refusal = quick.admission_refusal(config, WORKLOAD, 6, "aux_correctness")
    assert "run quick_check first" in refusal and "\n" not in refusal
    Gate(monkeypatch)
    assert quick.run_quick_check(config, WORKLOAD, 6).status == "pass"
    assert quick.admission_refusal(config, WORKLOAD, 6, "aux_correctness") == ""
    (config.repo_root / "accelerators" / "tpch" / "q6" / "module.cpp").write_text("// v2\n")
    assert "run quick_check first" in quick.admission_refusal(config, WORKLOAD, 6, "aux_bench_sweep")


@pytest.mark.parametrize("kwargs", [{"differential": False}, {"sanitizer": "thread"}])
def test_only_the_plain_check_with_the_differential_admits(config, monkeypatch, kwargs):
    Gate(monkeypatch)
    quick.run_quick_check(config, WORKLOAD, 6, **kwargs)
    assert quick.admission_refusal(config, WORKLOAD, 6, "aux_tsan")


def test_an_unchanged_passing_check_still_admits(config, monkeypatch):
    Gate(monkeypatch)
    quick.run_quick_check(config, WORKLOAD, 6)
    quick.run_quick_check(config, WORKLOAD, 6, differential=False)  # another check, same code
    assert quick.admission_refusal(config, WORKLOAD, 6, "aux_correctness") == ""


def test_other_gates_and_other_workloads_are_never_refused(config):
    assert quick.admission_refusal(config, WORKLOAD, 6, "aux_unit") == ""
    assert quick.admission_refusal(config, {**WORKLOAD, "workload_kind": "base"}, 6, "aux_correctness") == ""
    config.quick_tier = QuickTierConfig(enabled=False)
    assert quick.admission_refusal(config, WORKLOAD, 6, "aux_correctness") == ""


def test_rebuilds_of_supported_delta_writes_are_unfaithful():
    from accel_pipeline.test_suite import statements

    report = {"build_seconds": 0.020, "write_costs": [
        {"txn": "001.sql", "maint_ms": 15.0, "write_ms": 40.0, "read_ms": 1.0},   # rebuild-sized
        {"txn": "002.sql", "maint_ms": 0.2, "write_ms": 40.0, "read_ms": 1.0},    # incremental
        {"txn": "003.sql", "maint_ms": 15.0, "write_ms": 40.0, "read_ms": 1.0}]}  # rebuild, but the analysis allows it
    sql = {"001.sql": "UPDATE accounts SET status = 'x' WHERE id = 1;",
           "002.sql": "UPDATE accounts SET status = 'y' WHERE id = 2;",
           "003.sql": "DELETE FROM events WHERE id = 3;"}
    coverage = [{"table": "accounts", "mutation": "update", "behavior": "supported_delta"},
                {"table": "events", "mutation": "delete", "behavior": "bounded_rebuild"}]
    lines = write_cost.unfaithful_rebuilds([report], sql, coverage, statements)
    assert len(lines) == 1 and lines[0].startswith("001.sql (update accounts)")


def test_a_rebuild_shown_by_one_thread_run_only_is_noise():
    from accel_pipeline.test_suite import statements

    slow = {"build_seconds": 0.020, "write_costs": [{"txn": "001.sql", "maint_ms": 15.0, "write_ms": 40.0, "read_ms": 1.0}]}
    fast = {"build_seconds": 0.020, "write_costs": [{"txn": "001.sql", "maint_ms": 0.2, "write_ms": 40.0, "read_ms": 1.0}]}
    args = ({"001.sql": "UPDATE accounts SET status = 'x' WHERE id = 1;"}, [{"table": "accounts", "mutation": "update", "behavior": "supported_delta"}], statements)
    assert write_cost.unfaithful_rebuilds([slow, fast], *args) == []
    assert len(write_cost.unfaithful_rebuilds([slow, slow], *args)) == 1

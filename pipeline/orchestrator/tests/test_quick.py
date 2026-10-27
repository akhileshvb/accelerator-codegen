from __future__ import annotations

import json
from pathlib import Path


from accel_pipeline import check_report, quick, write_cost
from accel_pipeline.config import QuickTierConfig, gates_dir_for_query, query_artifacts_dir
from accel_pipeline.datasets import CutDatasets
from accel_pipeline.quick import render_quick_result, run_quick_check

from conftest import GIT, WORKLOAD, Gate  # noqa: F401


def edit_query(config):
    (config.repo_root / "accelerators" / "tpch" / "q6" / "module.cpp").write_text("// v2\n")


def test_a_passing_check_runs_the_quick_gate_with_the_configured_limits(config, monkeypatch):
    gate = Gate(monkeypatch)
    result = run_quick_check(config, WORKLOAD, 6, targets=["my_test"], differential=False)
    assert result.status == "pass" and result.phase_timings["host_build"] == 1.5
    call = gate.calls[0]
    assert call["name"] == "quick_check" and call["query"] == 6 and call["timeout_s"] == 120
    assert call["env"]["ACCEL_QUICK_TARGETS"] == "my_test" and call["env"]["ACCEL_QUICK_DIFFERENTIAL"] == "0"
    assert call["env"]["ACCEL_QUICK_PROBES"] == "3" and call["env"]["ACCEL_BUILD_STAMP_CACHE"] == "1"


def test_an_unchanged_tree_passes_without_running_anything_again(config, monkeypatch):
    gate = Gate(monkeypatch)
    run_quick_check(config, WORKLOAD, 6)
    again = run_quick_check(config, WORKLOAD, 6)
    assert again.status == "pass" and "unchanged" in again.summary
    assert len(gate.calls) == 1


def test_an_edit_runs_the_gate_again(config, monkeypatch):
    gate = Gate(monkeypatch)
    run_quick_check(config, WORKLOAD, 6)
    edit_query(config)
    run_quick_check(config, WORKLOAD, 6)
    assert len(gate.calls) == 2


def test_a_failing_differential_returns_a_failure_with_a_reproduce_command(config, monkeypatch):
    Gate(monkeypatch, "fail", {
        "reason": "differential failed", "step": "differential", "log_tail": "row 3 differs",
        "reproduce": "ACCEL_ENGINE_INSTANCE=i host.sh run diff_bin 6",
    })
    result = run_quick_check(config, WORKLOAD, 6)
    assert result.status == "fail"
    failure = result.failure
    assert failure.kind == "code" and failure.source == "quick_check"
    assert failure.reproduce == "ACCEL_ENGINE_INSTANCE=i host.sh run diff_bin 6"
    assert "row 3 differs" in failure.evidence and "differential" in failure.summary
    assert any("host_build" in line for line in failure.diagnosis)
    rendered = render_quick_result(result)
    assert "Reproduce: `ACCEL_ENGINE_INSTANCE=i host.sh run diff_bin 6`" in rendered


def test_a_failure_is_not_cached_as_a_pass(config, monkeypatch):
    gate = Gate(monkeypatch, "fail", {"reason": "x"})
    run_quick_check(config, WORKLOAD, 6)
    run_quick_check(config, WORKLOAD, 6)
    assert len(gate.calls) == 2


def test_a_timeout_becomes_a_timeout_failure(config, monkeypatch):
    Gate(monkeypatch, "fail", {"reason": "gate did not finish within 120s"})
    result = run_quick_check(config, WORKLOAD, 6)
    assert result.status == "fail" and result.failure.kind == "timeout"


def test_the_configured_timeout_and_probes_reach_the_gate(config, monkeypatch):
    gate = Gate(monkeypatch)
    config.quick_tier = QuickTierConfig(timeout_sec=45, differential_probes=2, build_stamp_cache=False)
    run_quick_check(config, WORKLOAD, 6)
    assert gate.calls[0]["timeout_s"] == 45
    assert gate.calls[0]["env"]["ACCEL_QUICK_PROBES"] == "2" and gate.calls[0]["env"]["ACCEL_BUILD_STAMP_CACHE"] == "0"


# the test databases and the report -------------------------------------------------------------------------

class FakeBench:
    """The aux bench on the quick instance: fixed read latency and counters."""
    instances: list = []

    def __init__(self, cfg, workload, query, instance):
        self.instance = instance
        FakeBench.instances.append(instance)

    def row(self, pct=0):
        return {"engine_ref_query_median_us": 2000.0, "accel_hit_median_us": 250.0, "accel_hit_rate": 1.0}

    def counters(self, row):
        return {"read": {"instructions": 650e3, "cycles": 900e3, "cache-misses": 400}, "build": {"instructions": 2e9, "cycles": 1.5e9, "cache-misses": 4e6}}


def stage_reports(config, build_n, build_1, plan="Partitioned build over the thread cap"):
    """The suite reports of the N-thread and the one-thread run, and the analysis' scale plan."""
    art = query_artifacts_dir(config.repo_root, 6, workload=WORKLOAD)
    art.mkdir(parents=True, exist_ok=True)
    (art / "analysis.json").write_text(json.dumps({"correctness_facts": {"scale_plan": {"build_parallelism": plan}}}))
    gates = gates_dir_for_query(art)
    gates.mkdir(parents=True, exist_ok=True)
    return [(path, {"build_seconds": seconds, "write_costs": []})
            for path, seconds in zip(write_cost.report_paths(gates), (build_n, build_1))]


class ReportGate(Gate):
    def __init__(self, monkeypatch, reports, data=None):
        super().__init__(monkeypatch, data={"engine_instance": "i-quick", "engine_state_dir": "/s-quick", "dataset": "/d", **(data or {})})
        self.reports = reports
        monkeypatch.setattr(quick, "Bench", FakeBench)
        monkeypatch.setattr(check_report, "gate_threads", lambda: 40)

    def run_gate(self, config, name, query, workload, **kwargs):
        for path, report in self.reports:
            path.write_text(json.dumps(report))
        return super().run_gate(config, name, query, workload, **kwargs)


def cuts(scale=50):
    return CutDatasets(Path("/subset"), Path("/tiny"), {"source_rows": {"orders": 1000 * scale}, "subset_rows": {"orders": 1000}})


def test_the_gate_runs_on_the_subset_and_a_sanitizer_run_on_the_tiny_database(config, monkeypatch):
    gate = Gate(monkeypatch)
    monkeypatch.setattr(quick, "ensure_cut_datasets", lambda *a: cuts())
    run_quick_check(config, WORKLOAD, 6)
    run_quick_check(config, WORKLOAD, 6, sanitizer="thread", threads=8)
    assert [c["env"]["ACCEL_SUBSET_DATASET_DIR"] for c in gate.calls] == ["/subset", "/tiny"]


def test_a_passing_check_reports_build_read_and_counters(config, monkeypatch):
    ReportGate(monkeypatch, stage_reports(config, build_n=0.2, build_1=1.0))
    monkeypatch.setattr(quick, "ensure_cut_datasets", lambda *a: cuts())
    result = run_quick_check(config, WORKLOAD, 6)
    assert result.status == "pass"
    assert result.report["build"] == "0.20 s at 40 threads, 1.00 s at 1 thread (5.0x) on the subset; projected 10 s on the full dataset (budget 15 s)"
    assert result.report["read"] == "engine SQL 2000 us, accelerator 250 us (8.0x faster)"
    assert result.report["counters"].startswith("per read 650.0k instructions, 900.0k cycles, 400 cache misses; build run 2.0G")
    assert FakeBench.instances[-1] == {"engine_instance": "i-quick", "engine_state_dir": "/s-quick", "dataset": "/d"}
    assert json.loads(render_quick_result(result))["build"] == result.report["build"]


def test_a_serial_build_fails_the_check_as_a_defect_with_the_next_step(config, monkeypatch):
    ReportGate(monkeypatch, stage_reports(config, build_n=1.60, build_1=1.64))
    monkeypatch.setattr(quick, "ensure_cut_datasets", lambda *a: cuts())
    result = run_quick_check(config, WORKLOAD, 6)
    assert result.status == "fail" and result.failure.kind == "code"
    assert "does not scale with threads: 1.60 s at 40 threads vs 1.64 s at 1 (1.0x)" in result.summary
    assert "PartitionedBuild" in result.summary
    assert quick.admission_refusal(config, WORKLOAD, 6, "aux_correctness")  # a failed check admits nothing


def test_a_serial_build_the_analysis_declared_serial_only_has_to_fit_the_budget(config, monkeypatch):
    ReportGate(monkeypatch, stage_reports(config, build_n=0.10, build_1=0.10, plan="n/a: a single pass"))
    monkeypatch.setattr(quick, "ensure_cut_datasets", lambda *a: cuts())
    assert run_quick_check(config, WORKLOAD, 6).status == "pass"


def test_a_build_too_slow_for_the_full_dataset_fails_even_when_it_scales(config, monkeypatch):
    ReportGate(monkeypatch, stage_reports(config, build_n=0.50, build_1=3.0))
    monkeypatch.setattr(quick, "ensure_cut_datasets", lambda *a: cuts())
    result = run_quick_check(config, WORKLOAD, 6)
    assert result.status == "fail" and "projected ~25 s at full scale" in result.summary and "budget 15 s" in result.summary


def test_an_unchanged_check_hands_back_its_report(config, monkeypatch):
    ReportGate(monkeypatch, stage_reports(config, build_n=0.2, build_1=1.0))
    monkeypatch.setattr(quick, "ensure_cut_datasets", lambda *a: cuts())
    first = run_quick_check(config, WORKLOAD, 6)
    again = run_quick_check(config, WORKLOAD, 6)
    assert "unchanged" in again.summary and again.report == first.report


def test_a_bench_that_cannot_run_is_reported_not_fatal(config, monkeypatch):
    ReportGate(monkeypatch, stage_reports(config, build_n=0.2, build_1=1.0))
    monkeypatch.setattr(quick, "ensure_cut_datasets", lambda *a: cuts())

    class Broken(FakeBench):
        def row(self, pct=0):
            raise RuntimeError("the bench exited 1: no such table")
    monkeypatch.setattr(quick, "Bench", Broken)
    result = run_quick_check(config, WORKLOAD, 6)
    assert result.status == "pass" and result.report["read"].startswith("unavailable (the bench exited 1")


def test_the_run_prepares_the_databases_and_prebuilds_the_sanitizer_variant_in_the_background(config, monkeypatch):
    calls = []
    monkeypatch.setattr(quick, "ensure_cut_datasets", lambda *a: cuts())
    monkeypatch.setattr(quick, "run_gate", lambda cfg, name, query, workload, **kw: calls.append((name, kw)))
    quick.prepare_inner_loop(config, WORKLOAD, 6).join()
    (name, kwargs), = calls
    assert name == "quick_check" and kwargs["out_dir"].name == "prebuild"
    assert kwargs["env"] == {"ACCEL_QUICK_SANITIZER": "thread", "ACCEL_QUICK_PREBUILD": "1", "ACCEL_SUBSET_DATASET_DIR": "/tiny"}
    config.quick_tier = QuickTierConfig(enabled=False)
    assert quick.prepare_inner_loop(config, WORKLOAD, 6) is None


# run_profile ----------------------------------------------------------------------------------------------

class FakeProfiles(FakeBench):
    result: dict = {}

    def profile(self, seconds):
        return self.result


OK = {"status": "ok", "sampling": {"self_pct_by_bucket": {"own_code": 90.0}, "top_functions": [
    {"self_pct": 60.0, "bucket": "own_code", "symbol": "q::Build()"}]}}


def profile(config, monkeypatch, profiles, gate_status="pass", **kwargs):
    Gate(monkeypatch, gate_status, {"engine_instance": "i", "engine_state_dir": "/s", "dataset": "/d", "reason": "boom"})
    monkeypatch.setattr(quick, "Bench", FakeProfiles)
    FakeProfiles.result = profiles
    FakeBench.instances = []
    return json.loads(quick.profile_hot_spots(config, WORKLOAD, 6, **kwargs))


def test_run_profile_returns_the_hot_spots_of_each_path(config, monkeypatch):
    out = profile(config, monkeypatch, {"read": OK, "write": OK, "build": OK})
    assert out["status"] == "pass" and set(out) == {"status", "scale", "read_path", "write_path", "build_path"}
    assert any("q::Build()" in line for line in out["build_path"])


def test_run_profile_defaults_to_the_subset_instance(config, monkeypatch):
    loaded = []
    monkeypatch.setattr(quick, "ensure_dataset_loaded", lambda *a, **k: loaded.append(k) or None)
    out = profile(config, monkeypatch, {"read": OK, "write": OK, "build": OK})
    assert out["scale"] == "subset" and not loaded
    assert FakeBench.instances == [{"engine_instance": "i", "engine_state_dir": "/s", "dataset": "/d"}]


def test_a_full_profile_runs_on_the_full_dataset_in_an_instance_of_its_own(config, monkeypatch):
    loaded = []
    monkeypatch.setattr(quick, "ensure_dataset_loaded", lambda workload, *a, **k: loaded.append(k["instance_env"]) or None)
    monkeypatch.setenv("ACCEL_DATASET_DIR", "/full")
    out = profile(config, monkeypatch, {"read": OK, "write": OK, "build": OK}, scale="full")
    (own,) = FakeBench.instances
    assert out["scale"] == "full" and own["dataset"] == "/full"
    assert own["engine_instance"] != "i" and own["engine_state_dir"] != "/s"
    assert loaded == [{"ACCEL_ENGINE_INSTANCE": own["engine_instance"], "ACCEL_ENGINE_STATE_DIR": own["engine_state_dir"]}]


def test_a_full_profile_whose_dataset_cannot_load_fails(config, monkeypatch):
    monkeypatch.setattr(quick, "ensure_dataset_loaded", lambda *a, **k: "host.sh load failed (exit 1)")
    out = profile(config, monkeypatch, {}, scale="full")
    assert out["status"] == "fail" and "host.sh load failed" in out["summary"]


def test_a_profiler_that_cannot_run_fails_run_profile_loudly(config, monkeypatch):
    out = profile(config, monkeypatch, {"read": {"status": "error", "reason": "perf is not installed on this host"}, "write": OK, "build": OK})
    assert out["status"] == "fail"
    assert "perf is not installed" in out["failure"] and "not because there is nothing to profile" in out["failure"]


def test_a_server_engine_is_not_applicable_rather_than_a_failure(config, monkeypatch):
    na = {"status": "not_applicable", "reason": "the engine runs the hit path in a server process"}
    out = profile(config, monkeypatch, {"read": na, "write": na, "build": na})
    assert out["status"] == "pass" and "not applicable" in out["read_path"][0]


def test_run_profile_needs_a_tree_that_passes_the_check(config, monkeypatch):
    out = profile(config, monkeypatch, {}, gate_status="fail")
    assert out["status"] == "fail" and "boom" in out["failure"]

from __future__ import annotations

import subprocess
from pathlib import Path
from types import SimpleNamespace

import pytest

from accel_pipeline import quick
from accel_pipeline.config import QuickTierConfig
from accel_pipeline.gates import GateResult
from accel_pipeline.quick import ensure_subset_dataset, render_quick_result, run_quick_check

WORKLOAD = {"workload_kind": "aux_state", "benchmark_name": "tpch", "engine_name": "eng"}
GIT = ["git", "-c", "user.name=t", "-c", "user.email=t@t"]


@pytest.fixture()
def config(tmp_path):
    root = tmp_path / "repo"
    (root / "accelerators" / "tpch" / "q6").mkdir(parents=True)
    (root / "accelerators" / "tpch" / "q6" / "module.cpp").write_text("// v1\n")
    (root / ".gitignore").write_text("pipeline/artifacts/\n")
    subprocess.run(["git", "init", "-q"], cwd=root, check=True)
    subprocess.run([*GIT, "add", "-A"], cwd=root, check=True)
    subprocess.run([*GIT, "commit", "-qm", "base"], cwd=root, check=True)
    return SimpleNamespace(repo_root=root, build_dir=tmp_path / "build", quick_tier=QuickTierConfig())


class Gate:
    def __init__(self, monkeypatch, status="pass", data=None):
        self.calls: list[dict] = []
        self.status = status
        self.data = data or {}
        monkeypatch.setattr(quick, "run_gate", self.run_gate)
        monkeypatch.setattr(quick, "ensure_subset_dataset", lambda *a: None)

    def run_gate(self, config, name, query, workload, *, env=None, timeout_s=None, **kwargs):
        self.calls.append({"name": name, "query": query, "env": env, "timeout_s": timeout_s})
        return GateResult(name, self.status, {"status": self.status, "phase_timings": {"host_build": 1.5}, **self.data})


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


def test_other_arguments_are_other_checks(config, monkeypatch):
    gate = Gate(monkeypatch)
    run_quick_check(config, WORKLOAD, 6)
    run_quick_check(config, WORKLOAD, 6, differential=False)
    run_quick_check(config, WORKLOAD, 6, targets=["t"])
    assert len(gate.calls) == 3


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


def test_a_pass_after_a_failure_is_cached_again(config, monkeypatch):
    gate = Gate(monkeypatch, "fail", {"reason": "x"})
    run_quick_check(config, WORKLOAD, 6)
    gate.status = "pass"
    run_quick_check(config, WORKLOAD, 6)
    run_quick_check(config, WORKLOAD, 6)
    assert len(gate.calls) == 2


def test_a_timeout_becomes_a_timeout_failure(config, monkeypatch):
    Gate(monkeypatch, "fail", {"reason": "gate did not finish within 120s"})
    result = run_quick_check(config, WORKLOAD, 6)
    assert result.status == "fail" and result.failure.kind == "timeout"


def test_the_quick_tier_can_be_turned_off(config, monkeypatch):
    gate = Gate(monkeypatch)
    config.quick_tier = QuickTierConfig(enabled=False)
    assert run_quick_check(config, WORKLOAD, 6).status == "skip" and not gate.calls


def test_only_aux_workloads_are_checked(config, monkeypatch):
    gate = Gate(monkeypatch)
    assert run_quick_check(config, {**WORKLOAD, "workload_kind": "base"}, 6).status == "skip" and not gate.calls


def test_the_configured_timeout_and_probes_reach_the_gate(config, monkeypatch):
    gate = Gate(monkeypatch)
    config.quick_tier = QuickTierConfig(timeout_sec=45, differential_probes=2, build_stamp_cache=False)
    run_quick_check(config, WORKLOAD, 6)
    assert gate.calls[0]["timeout_s"] == 45
    assert gate.calls[0]["env"]["ACCEL_QUICK_PROBES"] == "2" and gate.calls[0]["env"]["ACCEL_BUILD_STAMP_CACHE"] == "0"


SCHEMA = "CREATE TABLE orders (o_orderkey BIGINT, o_flag CHAR(1));\nCREATE TABLE lineitem (l_orderkey BIGINT, l_n INTEGER);\n"
SUBSET = """-- the subset: orders with an even key and their lineitems
CREATE TABLE orders AS SELECT * FROM src.orders WHERE o_orderkey % 2 = 0;
CREATE TABLE lineitem AS SELECT * FROM src.lineitem WHERE l_orderkey IN (SELECT o_orderkey FROM orders);
"""


@pytest.fixture()
def dataset(tmp_path):
    root = tmp_path / "repo"
    (root / "workloads" / "tpch").mkdir(parents=True)
    (root / "workloads" / "tpch" / "schema.sql").write_text(SCHEMA)
    (root / "workloads" / "tpch" / "test_db_subset.sql").write_text(SUBSET)
    data = tmp_path / "tbl"
    data.mkdir()
    (data / "orders.tbl").write_text("".join(f"{k}|A\n" for k in range(1, 11)))
    (data / "lineitem.tbl").write_text("".join(f"{k}|{n}\n" for k in range(1, 11) for n in range(2)))
    return SimpleNamespace(repo_root=root, build_dir=tmp_path / "build"), {**WORKLOAD, "dataset_dir": str(data)}


def test_the_subset_dataset_holds_the_cut_tables_as_tbl_files(dataset):
    config, workload = dataset
    subset = ensure_subset_dataset(config, workload)
    assert sorted(p.name for p in subset.glob("*.tbl")) == ["lineitem.tbl", "orders.tbl"]
    assert (subset / "orders.tbl").read_text().splitlines() == [f"{k}|A" for k in (2, 4, 6, 8, 10)]
    assert len((subset / "lineitem.tbl").read_text().splitlines()) == 10


def test_the_subset_is_built_once_per_dataset(dataset):
    config, workload = dataset
    first = ensure_subset_dataset(config, workload)
    stamp = (first / ".done").stat().st_mtime_ns
    assert ensure_subset_dataset(config, workload) == first
    assert (first / ".done").stat().st_mtime_ns == stamp


def test_a_changed_dataset_gets_a_new_subset(dataset):
    config, workload = dataset
    first = ensure_subset_dataset(config, workload)
    (Path(workload["dataset_dir"]) / "orders.tbl").write_text("2|A\n")
    assert ensure_subset_dataset(config, workload) != first


def test_no_subset_without_a_subset_sql_or_a_dataset(dataset, tmp_path):
    config, workload = dataset
    assert ensure_subset_dataset(config, {**workload, "benchmark_name": "other"}) is None
    assert ensure_subset_dataset(config, {**workload, "dataset_dir": str(tmp_path / "missing")}) is None


def test_a_dataset_beyond_the_size_cap_runs_the_quick_tier_on_the_full_data(dataset, monkeypatch):
    config, workload = dataset
    monkeypatch.setattr(quick, "SUBSET_MAX_SOURCE_GB", 1e-9)
    assert ensure_subset_dataset(config, workload) is None


def test_a_subset_dir_from_the_environment_wins(dataset, monkeypatch):
    config, workload = dataset
    monkeypatch.setenv("ACCEL_SUBSET_DATASET_DIR", "/given")
    assert ensure_subset_dataset(config, workload) == Path("/given")

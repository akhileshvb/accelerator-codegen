"""Code stamps, the gate results recorded against them and background gates reused at stage exit."""

from __future__ import annotations

import json
import subprocess
import threading
import time
from types import SimpleNamespace

import pytest

from accel_pipeline import background_gates
from accel_pipeline.background_gates import background_status, reusable_background_result, start_background_gate
from accel_pipeline.build_stamp import build_lock, code_stamp
from accel_pipeline.config import QuickTierConfig
from accel_pipeline.gate_stamps import current_stamp, passed_at_current_stamp, record_pass, reusable_gate_result

WORKLOAD = {"workload_kind": "aux_state", "benchmark_name": "demo", "engine_name": "eng"}
GIT = ["git", "-c", "user.name=t", "-c", "user.email=t@t"]


@pytest.fixture()
def config(tmp_path):
    root = tmp_path / "repo"
    for q in (5, 6):
        (root / "accelerators" / "demo" / f"q{q}").mkdir(parents=True)
        (root / "accelerators" / "demo" / f"q{q}" / "module.cpp").write_text("// v1\n")
    (root / "pipeline" / "artifacts").mkdir(parents=True)
    (root / ".gitignore").write_text("pipeline/artifacts/\n")
    subprocess.run(["git", "init", "-q"], cwd=root, check=True)
    subprocess.run([*GIT, "add", "-A"], cwd=root, check=True)
    subprocess.run([*GIT, "commit", "-qm", "base"], cwd=root, check=True)
    return SimpleNamespace(repo_root=root, build_dir=tmp_path / "build", quick_tier=QuickTierConfig(), worker_id="")


def write(config, rel, text):
    path = config.repo_root / rel
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text)


def test_the_stamp_follows_the_query_and_shared_files_only(config):
    root = config.repo_root
    base = code_stamp(root, query_dir="accelerators/demo/q5")
    write(config, "accelerators/demo/q6/module.cpp", "// other query\n")
    write(config, "pipeline/gates/x.sh", "# pipeline code\n")
    write(config, "pipeline/artifacts/tpch/q5/note.json", "{}")
    assert code_stamp(root, query_dir="accelerators/demo/q5") == base
    write(config, "accelerators/demo/q5/module.cpp", "// v2\n")
    own = code_stamp(root, query_dir="accelerators/demo/q5")
    assert own != base
    write(config, "substrate/shared.hpp", "// shared\n")
    assert code_stamp(root, query_dir="accelerators/demo/q5") != own


@pytest.mark.parametrize("shared", [
    "accelerators/prims/ring.hpp", "accelerators/demo/common/scan.hpp", "backends/eng/host.sh",
    "substrate/store.hpp", "cmake/Find.cmake", "CMakeLists.txt", "workloads/demo/q5.sql", "eval/run.py",
    "accelerators/demo/qregistry.cpp",
])
def test_a_shared_file_changes_every_querys_stamp(config, shared):
    base = {q: code_stamp(config.repo_root, query_dir=f"accelerators/demo/q{q}") for q in (5, 6)}
    write(config, shared, "// changed\n")
    assert all(code_stamp(config.repo_root, query_dir=f"accelerators/demo/q{q}") != base[q] for q in (5, 6))


@pytest.mark.parametrize("excluded", [
    "pipeline/prompts/a.md", "scripts/x.py", "docs/y.md", "build-release/z.o", "build/z.o",
    "accelerators/demo/q50/module.cpp", "accelerators/other/q5/module.cpp",
])
def test_only_pipeline_scripts_docs_build_and_other_queries_are_excluded(config, excluded):
    base = code_stamp(config.repo_root, query_dir="accelerators/demo/q5")
    write(config, excluded, "// changed\n")
    assert code_stamp(config.repo_root, query_dir="accelerators/demo/q5") == base


def test_the_stamp_follows_content_not_timestamps(config):
    root = config.repo_root
    write(config, "substrate/a.hpp", "// one\n")
    first = code_stamp(root)
    write(config, "substrate/a.hpp", "// two\n")
    write(config, "substrate/a.hpp", "// one\n")
    assert code_stamp(root) == first


def test_extras_and_deleted_files_change_the_stamp(config):
    root = config.repo_root
    base = code_stamp(root, query_dir="accelerators/demo/q5")
    assert code_stamp(root, query_dir="accelerators/demo/q5", extra=["target"]) != base
    (root / "accelerators" / "demo" / "q5" / "module.cpp").unlink()
    assert code_stamp(root, query_dir="accelerators/demo/q5") != base


def test_outside_a_git_checkout_there_is_no_stamp(tmp_path):
    assert code_stamp(tmp_path) == ""


def test_a_gate_passes_at_the_stamp_it_was_recorded_for(config):
    record_pass(config, WORKLOAD, 5, "aux_unit", current_stamp(config, WORKLOAD, 5))
    assert passed_at_current_stamp(config, WORKLOAD, 5, "aux_unit")
    assert not passed_at_current_stamp(config, WORKLOAD, 5, "aux_correctness")
    write(config, "accelerators/demo/q5/module.cpp", "// v2\n")
    assert not passed_at_current_stamp(config, WORKLOAD, 5, "aux_unit")


def test_no_stamp_never_counts_as_passed(tmp_path):
    cfg = SimpleNamespace(repo_root=tmp_path, build_dir=tmp_path / "b")
    record_pass(cfg, WORKLOAD, 5, "aux_unit", "")
    assert not passed_at_current_stamp(cfg, WORKLOAD, 5, "aux_unit")


def test_concurrent_records_all_survive(config):
    stamp = current_stamp(config, WORKLOAD, 5)
    gates = [f"gate{i}" for i in range(8)]
    threads = [threading.Thread(target=record_pass, args=(config, WORKLOAD, 5, g, stamp)) for g in gates]
    [t.start() for t in threads]
    [t.join() for t in threads]
    assert all(passed_at_current_stamp(config, WORKLOAD, 5, g) for g in gates)


def test_build_lock_is_exclusive(tmp_path):
    order = []

    def second():
        with build_lock(tmp_path / "build"):
            order.append("second")

    with build_lock(tmp_path / "build"):
        thread = threading.Thread(target=second)
        thread.start()
        time.sleep(0.2)
        order.append("first")
    thread.join()
    assert order == ["first", "second"]


def finished(config, gate, *, status="pass", stamp_end=None, stamp_start=None):
    state_path = background_gates._state_path(config, WORKLOAD, 5, gate)
    state_path.parent.mkdir(parents=True, exist_ok=True)
    current = current_stamp(config, WORKLOAD, 5)
    state_path.write_text(json.dumps({
        "gate": gate, "status": status, "data": {"status": status, "reason": "r"},
        "stamp_start": stamp_start or current, "stamp_end": stamp_end or current,
    }))


def test_a_background_pass_on_the_current_code_is_reused(config):
    finished(config, "aux_correctness")
    result = reusable_background_result(config, WORKLOAD, 5, "aux_correctness")
    assert result.status == "pass" and result.data["reused"] == "background"


def test_a_stale_background_stamp_triggers_a_rerun(config):
    finished(config, "aux_correctness")
    write(config, "accelerators/demo/q5/module.cpp", "// edited after the gate ran\n")
    assert reusable_background_result(config, WORKLOAD, 5, "aux_correctness") is None
    assert background_status(config, WORKLOAD, 5, "aux_correctness")["stale"] is True


def test_code_that_moved_during_the_run_is_not_reused_even_if_it_moved_back(config):
    finished(config, "aux_correctness", stamp_start="another-stamp")
    assert reusable_background_result(config, WORKLOAD, 5, "aux_correctness") is None


def test_a_failed_or_missing_background_run_is_never_reused(config):
    finished(config, "aux_unit", status="fail")
    assert reusable_background_result(config, WORKLOAD, 5, "aux_unit") is None
    assert reusable_background_result(config, WORKLOAD, 5, "prims_tests") is None


def test_status_of_a_gate_never_started(config):
    assert background_status(config, WORKLOAD, 5, "aux_unit")["status"] == "none"


def test_a_background_run_whose_process_vanished_is_reported_died(config):
    path = background_gates._state_path(config, WORKLOAD, 5, "aux_unit")
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps({"gate": "aux_unit", "status": "running", "pid": 2**22 + 12345, "started_at": time.time()}))
    assert background_status(config, WORKLOAD, 5, "aux_unit")["status"] == "died"


def test_only_the_listed_gates_run_in_the_background(config):
    refused = start_background_gate(config, WORKLOAD, 5, "aux_bench_sweep", workload_name="w")
    assert refused["status"] == "refused" and "aux_correctness" in refused["reason"]


def test_starting_a_background_gate_records_it_running_and_does_not_start_it_twice(config, monkeypatch):
    spawned = []

    class Proc:
        pid = 4242

    fake_subprocess = SimpleNamespace(Popen=lambda *a, **k: spawned.append(a) or Proc(), STDOUT=subprocess.STDOUT)
    monkeypatch.setattr(background_gates, "subprocess", fake_subprocess)
    monkeypatch.setattr(background_gates, "_alive", lambda pid: True)
    state = start_background_gate(config, WORKLOAD, 5, "aux_unit", workload_name="tpch_q5_aux")
    assert state["status"] == "running" and state["pid"] == 4242 and state["stamp_start"]
    assert "--gate" in spawned[0][0] and "aux_unit" in spawned[0][0]
    again = start_background_gate(config, WORKLOAD, 5, "aux_unit", workload_name="tpch_q5_aux")
    assert again["pid"] == 4242 and len(spawned) == 1
    status = background_status(config, WORKLOAD, 5, "aux_unit")
    assert status["status"] == "running" and "elapsed_sec" in status


def test_a_timing_gate_is_reused_only_from_the_same_stages_acceptance_run(config):
    stamp = current_stamp(config, WORKLOAD, 5)
    path = config.repo_root / "pipeline" / "artifacts" / "sweep.json"
    path.write_text('{"status": "pass"}')
    record_pass(config, WORKLOAD, 5, "aux_bench_sweep", stamp, change_kind="audit", result=path)
    assert reusable_gate_result(config, WORKLOAD, 5, "aux_bench_sweep", change_kind="audit").data["reused"] is True
    assert reusable_gate_result(config, WORKLOAD, 5, "aux_bench_sweep", change_kind="implementation") is None
    assert reusable_gate_result(config, WORKLOAD, 5, "aux_bench_sweep") is None
    record_pass(config, WORKLOAD, 5, "aux_bench_sweep", stamp, change_kind="optimization", result=path)
    assert reusable_gate_result(config, WORKLOAD, 5, "aux_bench_sweep", change_kind="optimization") is None


def test_a_correctness_gate_is_reused_across_stages_on_identical_code_only(config):
    stamp = current_stamp(config, WORKLOAD, 5)
    path = config.repo_root / "pipeline" / "artifacts" / "unit.json"
    path.write_text('{"status": "pass", "log": "x"}')
    record_pass(config, WORKLOAD, 5, "aux_unit", stamp, change_kind="implementation", result=path)
    assert reusable_gate_result(config, WORKLOAD, 5, "aux_unit", change_kind="audit").data["log"] == "x"
    write(config, "accelerators/prims/ring.hpp", "// edited\n")
    assert reusable_gate_result(config, WORKLOAD, 5, "aux_unit", change_kind="audit") is None


def test_a_gate_without_a_readable_passing_result_is_not_reused(config):
    record_pass(config, WORKLOAD, 5, "aux_unit", current_stamp(config, WORKLOAD, 5), result=config.repo_root / "gone.json")
    assert reusable_gate_result(config, WORKLOAD, 5, "aux_unit") is None


def test_a_background_pass_is_reused_for_a_non_timing_gate(config):
    finished(config, "aux_correctness")
    assert reusable_gate_result(config, WORKLOAD, 5, "aux_correctness").data["reused"] == "background"

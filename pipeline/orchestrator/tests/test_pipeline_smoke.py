"""The pipeline smoke test (tests/smoke): its verdict logic always, the end-to-end run only on request."""

from __future__ import annotations

import json
import os
import subprocess
import sys
from pathlib import Path

import pytest

SMOKE = Path(__file__).resolve().parent / "smoke"
sys.path.insert(0, str(SMOKE))
import smoke_report  # noqa: E402

CLEAN_LOG = """\
[pipeline] gate complete: host_bootstrap (host_bootstrap) status=pass duration=3ms
=== stage: analysis ===
=== stage: test_suite ===
[pipeline] ═══ gates (test_suite, Q14): 1 to run ═══
[pipeline]   1. test_suite_validate
[pipeline] gate complete: [1/1] test_suite_validate (test_suite) status=pass duration=5ms
=== stage: implementation ===
[pipeline] ═══ gates (implementation, Q14): 2 to run ═══
[pipeline]   1. aux_unit (usually seconds)
[pipeline]   2. aux_correctness (usually seconds)
[pipeline] gate complete: [2/2] aux_correctness (implementation) status=pass duration=9ms
[pipeline] gate aux_unit: reused (passed on identical code)
=== stage: optimization ===
=== stage: audit ===
pipeline complete
"""


def problems(log: str, tmp_path: Path, calls=(), rc: int | None = 0) -> list[str]:
    return smoke_report.problems_of(log, tmp_path, list(calls), rc)


def test_a_clean_run_has_no_problems(tmp_path):
    calls = [{"stage": s, "resumed": False} for s in smoke_report.STAGES]
    assert problems(CLEAN_LOG, tmp_path, calls) == []


def test_the_first_bootstrap_gate_of_a_clean_tree_fails_by_design(tmp_path):
    calls = [{"stage": s, "resumed": False} for s in smoke_report.STAGES]
    fail = "[pipeline] gate FAILED: host_bootstrap\n"
    assert problems(fail + "[pipeline] stage start: host_bootstrap model=x\n" + CLEAN_LOG, tmp_path, calls) == []
    assert any("gate FAILED" in p for p in problems(fail + "[pipeline] stage start: analysis\n" + CLEAN_LOG, tmp_path, calls))


def test_announced_gates_are_read_from_the_log():
    assert smoke_report.planned_gates(CLEAN_LOG) == [
        ("test_suite", "test_suite_validate"), ("implementation", "aux_unit"), ("implementation", "aux_correctness")]


@pytest.mark.parametrize("edit, expected", [
    (lambda l: l.replace("=== stage: audit ===\n", ""), "stage audit never started"),
    (lambda l: l.replace("pipeline complete\n", ""), "never printed"),
    (lambda l: l.replace("status=pass duration=5ms", "status=fail duration=5ms"), "test_suite_validate"),
    (lambda l: l.replace("[pipeline] gate aux_unit: reused (passed on identical code)\n", ""), "aux_unit"),
    (lambda l: l + "[repair] code failure in gate:aux_unit; round 1/3\n", "[repair]"),
])
def test_each_kind_of_breakage_is_named(tmp_path, edit, expected):
    assert any(expected in p for p in problems(edit(CLEAN_LOG), tmp_path))


def test_a_stop_report_a_repair_call_or_a_timeout_fails_the_run(tmp_path):
    (tmp_path / "a").mkdir()
    (tmp_path / "a" / "stop_report.md").write_text("stopped")
    assert any("stop report" in p for p in problems(CLEAN_LOG, tmp_path))
    assert any("ran 2 times" in p for p in problems(CLEAN_LOG, tmp_path / "b", [{"stage": "audit"}, {"stage": "audit"}]))
    assert any("only asks on a failure" in p for p in problems(CLEAN_LOG, tmp_path / "d", [{"stage": "crash_debug"}]))
    assert problems(CLEAN_LOG, tmp_path / "e", [{"stage": "optimization_worker", "resumed": True}] * 2) == []
    assert any("timed out" in p for p in problems(CLEAN_LOG, tmp_path / "c", rc=None))


@pytest.mark.pipeline_smoke
@pytest.mark.skipif(not os.environ.get("ACCEL_RUN_PIPELINE_SMOKE"), reason="builds a clean tree and runs the pipeline: set ACCEL_RUN_PIPELINE_SMOKE=1")
@pytest.mark.parametrize("threads, speedup, flagged", [(8, 1.04, True), (8, 4.4, False), (2, 1.04, False)])
def test_a_serial_build_fails_the_run_only_where_threads_could_speed_it_up(tmp_path, threads, speedup, flagged):
    gate = tmp_path / "pipeline/artifacts/tpch/q14/duckdb/gates/aux_bench_sweep.json"
    gate.parent.mkdir(parents=True)
    gate.write_text(json.dumps({"thread_scaling": {"threads": threads, "build_speedup_at_N_vs_1": speedup}}))
    assert bool(smoke_report.serial_builds(tmp_path)) is flagged


def test_the_pipeline_passes_end_to_end_with_a_scripted_agent():
    env = {k: v for k, v in os.environ.items() if k != "OPENROUTER_API_KEY"}
    env["PYTHONPATH"] = os.pathsep.join(filter(None, (str(SMOKE.parents[1]), env.get("PYTHONPATH"))))  # the package
    proc = subprocess.run([sys.executable, str(SMOKE / "pipeline_smoke.py"), "--workload",
                           os.environ.get("ACCEL_SMOKE_WORKLOAD", "tpch_q14_aux"), "--engine",
                           os.environ.get("ACCEL_SMOKE_ENGINE", "duckdb")],
                          env=env, capture_output=True, text=True)
    assert proc.returncode == 0, proc.stdout[-6000:] + proc.stderr[-2000:]


CASE = {"about": "x", "expect": ["refused: run quick_check first", "\\[repair\\] code failure"], "escaped": "=== stage: optimization ==="}


def test_case_is_decided_when_its_messages_appear_or_the_misbehaviour_gets_through():
    assert not smoke_report.case_decided(CASE, "refused: run quick_check first")
    assert smoke_report.case_decided(CASE, "refused: run quick_check first\n[repair] code failure in quick_check")
    assert smoke_report.case_decided(CASE, "=== stage: optimization ===")


def test_case_verdicts(tmp_path, capsys):
    (tmp_path / "state").mkdir()
    (tmp_path / "pipeline.log").write_text("refused: run quick_check first\n[repair] code failure in quick_check\n")
    assert smoke_report.verify_case("c", CASE, tmp_path, tmp_path, []) == 0
    (tmp_path / "pipeline.log").write_text("=== stage: optimization ===\n")
    assert smoke_report.verify_case("c", CASE, tmp_path, tmp_path, []) == 1
    assert smoke_report.verify_case("c", {**CASE, "xfail": "TODO"}, tmp_path, tmp_path, []) == 0  # expected to get through
    (tmp_path / "pipeline.log").write_text("refused: run quick_check first\n[repair] code failure in quick_check\n")
    assert smoke_report.verify_case("c", {**CASE, "xfail": "TODO"}, tmp_path, tmp_path, []) == 1  # caught: drop the xfail


def test_a_new_run_deletes_all_but_the_newest_earlier_runs(tmp_path, monkeypatch):
    import pipeline_smoke

    monkeypatch.setattr(pipeline_smoke, "SMOKE_HOME", tmp_path)
    for name in ("20261024-100000", "20261024-110000", "20261025-090000", "20261025-100000"):
        (tmp_path / "runs" / name).mkdir(parents=True)
    new = pipeline_smoke.new_run_dir()
    assert sorted(p.name for p in (tmp_path / "runs").iterdir()) == ["20261025-090000", "20261025-100000"]
    assert new.parent == tmp_path / "runs" and not new.exists()

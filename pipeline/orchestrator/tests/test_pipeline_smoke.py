"""The pipeline smoke test (tests/smoke): its verdict logic always, the end-to-end run only on request."""

from __future__ import annotations

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
def test_the_pipeline_passes_end_to_end_with_a_scripted_agent():
    env = {k: v for k, v in os.environ.items() if k != "OPENROUTER_API_KEY"}
    proc = subprocess.run([sys.executable, str(SMOKE / "pipeline_smoke.py"), "--workload",
                           os.environ.get("ACCEL_SMOKE_WORKLOAD", "tpch_q14_aux"), "--engine",
                           os.environ.get("ACCEL_SMOKE_ENGINE", "duckdb")],
                          env=env, capture_output=True, text=True)
    assert proc.returncode == 0, proc.stdout[-6000:] + proc.stderr[-2000:]

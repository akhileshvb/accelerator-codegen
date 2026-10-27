"""run_aux_exit.sh against the fake host of test_gate_plumbing: the phases run side by side as the gates they are, each
leaves that gate's result file, and aux_exit.json says what each did."""

from __future__ import annotations

from test_gate_plumbing import GATES, repo, result, run_gate  # noqa: F401  (repo is a fixture)
from test_gate_threads import bench_calls


def exit_run(repo, tmp_path, **env):
    (repo / "pipeline" / "orchestrator").symlink_to(GATES.parents[0] / "orchestrator")
    return run_gate(repo, "run_aux_exit.sh", "5", env_extra={
        "FAKE_SWEEP": "1", "FAKE_MULTI_POINT": "1", "ACCEL_AUX_WRITE_FREQUENCIES": "0,10", "ACCEL_AUX_THREADS": "4",
        "ACCEL_HOST_LOCK_DIR": str(tmp_path / "locks"), "ACCEL_MEASURE_SET": "0-3", "ACCEL_GATE_NAME": "aux_exit", **env,
    })


def test_every_phase_runs_as_its_gate_and_the_exit_result_lists_them(repo, tmp_path):
    proc, calls, log, gj, _ = exit_run(repo, tmp_path)
    assert proc.returncode == 0, proc.stdout + proc.stderr
    exit_result = result(gj, "aux_exit")
    assert exit_result["status"] == "pass"
    assert set(exit_result["phases"]) == {"correctness", "multi_connection", "cross_accel", "tsan", "bench", "calibrate"}
    for phase, gate in [("correctness", "aux_correctness"), ("multi_connection", "aux_multi_connection"),
                        ("cross_accel", "aux_cross_accel"), ("bench", "aux_bench_sweep"), ("tsan", "aux_tsan"),
                        ("calibrate", "aux_cost_model_calibrate")]:
        assert exit_result["phases"][phase]["result"].endswith(f"{gate}.json")
        assert result(gj, gate)["status"] == exit_result["phases"][phase]["status"]
    assert exit_result["phases"]["tsan"]["status"] == "skip"  # the fake host is not embedded
    assert any(c.startswith("run calibrator") for c in calls)  # calibrated in the exit run, not in a gate after it
    assert len(bench_calls(log)) == 3  # the sweep's one call and its one- and N-thread points
    assert any(c.startswith("build ") and "diff_bin" in c and "bench_aux" in c for c in calls)  # the targets built together


def test_a_failing_phase_fails_the_exit_with_its_reason_and_the_others_finish(repo, tmp_path):
    proc, _, _, gj, _ = exit_run(repo, tmp_path, FAKE_RC_run="1")
    assert proc.returncode == 1
    exit_result = result(gj, "aux_exit")
    assert exit_result["status"] == "fail" and exit_result["failed_phases"]
    assert exit_result["reason"].split(":")[0] in exit_result["failed_phases"]
    assert all(phase["seconds"] >= 0 for phase in exit_result["phases"].values())


def test_phases_can_be_picked(repo, tmp_path):
    proc, _, _, gj, _ = exit_run(repo, tmp_path, ACCEL_EXIT_PHASES="cross_accel")
    assert proc.returncode == 0, proc.stdout + proc.stderr
    assert set(result(gj, "aux_exit")["phases"]) == {"cross_accel"}


def test_an_optimization_exit_runs_the_lighter_correctness_tier(repo, tmp_path):
    proc, _, _, gj, _ = exit_run(repo, tmp_path, ACCEL_EXIT_PHASES="correctness", ACCEL_GATE_CHANGE_KIND="optimization")
    assert proc.returncode == 0, proc.stdout + proc.stderr
    assert result(gj, "aux_exit")["phases"]["correctness"]["result"].endswith("aux_correctness_iterate.json")


def test_the_run_limit_follows_the_measured_build_time(tmp_path):
    import subprocess

    def limit(built, **env):
        gates = tmp_path / "gates"
        gates.mkdir(exist_ok=True)
        (gates / "build_seconds").write_text(built)
        out = subprocess.run(["bash", "-c", f"source {GATES}/aux_gate_common.sh; run_limit"], text=True, capture_output=True,
                             env={"PATH": "/usr/bin:/bin", "GATE_OUT": str(gates), **env})
        return int(out.stdout)

    assert limit("") == 3600  # never measured (phases side by side start before a build was timed)
    assert limit("300") == 8 * 300 + 600  # 8 builds and a margin
    assert limit("1.2") == 1800 and limit("0.01") == 1800
    assert limit("105.5", ACCEL_GATE_RUN_TIMEOUT="77") == 77

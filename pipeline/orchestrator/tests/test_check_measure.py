"""The measurements on the subset: the aux bench through the host's `run` verb, and perf around it."""

from __future__ import annotations

import stat
from types import SimpleNamespace

import pytest

from accel_pipeline import check_measure, perf_profile
from accel_pipeline.check_measure import Bench

HOST = """#!/usr/bin/env bash
echo "$@" >>"$FAKE_HOST_LOG"
if [[ "$1" == run && "$2" == perf ]]; then exec "$@"; fi
[[ "$1" == run ]] || exit 2
[[ -z "$FAKE_BENCH_FAIL" ]] || { echo "no such table" >&2; exit 1; }
echo "suite,write_frequency_pct,engine_ref_query_median_us,accel_hit_median_us,accel_write_median_us,accel_hit_rate,setup_full_build_ms,workload_speedup"
echo "lineitem_only,$ACCEL_AUX_WRITE_FREQUENCIES,2000.0,250.0,900.0,1.00,40.0,8.0"
"""


@pytest.fixture()
def bench(tmp_path, monkeypatch):
    repo = tmp_path / "repo"
    host = repo / "backends" / "fake" / "host.sh"
    host.parent.mkdir(parents=True)
    host.write_text(HOST)
    host.chmod(host.stat().st_mode | stat.S_IXUSR)
    monkeypatch.setenv("FAKE_HOST_LOG", str(tmp_path / "host.log"))
    monkeypatch.setattr(check_measure, "resolve_workload_paths", lambda *a: {"aux_bench_target": "bench_aux", "aux_bench_suites": "lineitem_only"})
    monkeypatch.setattr(check_measure, "host_info", lambda *a: {"embedded": True})
    cfg = SimpleNamespace(repo_root=repo, build_dir=tmp_path / "build")
    instance = {"engine_instance": "i-quick", "engine_state_dir": "/state-quick", "dataset": "/subset"}
    return Bench(cfg, {"engine_name": "fake", "benchmark_name": "acme"}, 3, instance), tmp_path / "host.log"


def test_the_bench_env_names_the_instance_the_write_frequency_and_the_repetitions(bench):
    env = bench[0].env("/work", 20, pct=50)
    assert env["ACCEL_ENGINE_INSTANCE"] == "i-quick" and env["ACCEL_ENGINE_STATE_DIR"] == "/state-quick"
    assert env["ACCEL_DATASET_DIR"] == "/subset" and env["ACCEL_AUX_WRITE_FREQUENCIES"] == "50"
    assert env["ACCEL_Q3_BENCH_SUITE"] == "lineitem_only" and env["ACCEL_BENCH_QUERIES"] == "20"


def test_a_row_is_the_benchs_csv_run_through_the_host(bench):
    b, log = bench
    row = b.row(pct=50)
    assert float(row["engine_ref_query_median_us"]) == 2000.0 and row["write_frequency_pct"] == 50
    assert log.read_text().startswith(f"run {b.cfg.build_dir}/bench_aux --query 3 50")


def test_a_failing_bench_raises_with_its_output(bench, monkeypatch):
    monkeypatch.setenv("FAKE_BENCH_FAIL", "1")
    with pytest.raises(RuntimeError, match="no such table"):
        bench[0].row()


def test_a_server_engine_has_no_counters_and_no_profile(bench, monkeypatch):
    monkeypatch.setattr(check_measure, "host_info", lambda *a: {"embedded": False})
    assert "server process" in bench[0].counters({"accel_hit_median_us": 250.0})["unavailable"]
    assert {r["status"] for r in bench[0].profile(1.0).values()} == {"not_applicable"}


def test_the_outermost_perf_command_goes_through_the_host(bench, monkeypatch):
    b, log = bench
    seen = []
    monkeypatch.setattr(check_measure.subprocess, "run", lambda command, **kw: seen.append(command))
    monkeypatch.setattr(perf_profile, "perf_binary", lambda: "/usr/bin/perf")
    run = b.perf_runner()
    run(["/usr/bin/perf", "stat", "--", "bench"], env={})
    run(["/usr/bin/perf", "report", "-i", "x"], env={})
    assert seen[0][1:3] == ["run", "/usr/bin/perf"] and seen[0][0].endswith("host.sh")
    assert seen[1] == ["/usr/bin/perf", "report", "-i", "x"]


def test_the_profile_samples_read_write_and_build(bench, monkeypatch):
    b, _ = bench
    calls = []
    monkeypatch.setattr(perf_profile, "profile_hit_path", lambda cmd, env, **kw: calls.append(("hit", cmd[-1], env["ACCEL_AUX_WRITE_FREQUENCIES"], kw["hit_us"])) or {"status": "ok"})
    monkeypatch.setattr(perf_profile, "profile_build", lambda cmd, env, **kw: calls.append(("build",)) or {"status": "ok"})
    assert set(b.profile(5.0)) == {"read", "write", "build"}
    assert calls[0][:3] == ("hit", "0", "0") and calls[0][3] == 250.0
    assert calls[1][:3] == ("hit", "50", "50") and calls[1][3] == pytest.approx(575.0)  # half reads (250 us), half writes (900 us)
    assert calls[2] == ("build",)

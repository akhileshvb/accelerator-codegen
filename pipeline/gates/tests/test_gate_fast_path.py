"""The fast path of the gates: one-process context loading, phase timings, the build-stamp cache,
the quick tier and the quick_check gate, against the fake host of test_gate_plumbing."""

from __future__ import annotations

import json
import os
import shutil
import subprocess
import sys

import pytest
from test_gate_plumbing import GATES, repo, result, run_gate  # noqa: F401  (repo is a fixture)

sys.path.insert(0, str(GATES))
import read_gate_context  # noqa: E402

ORCHESTRATOR = GATES.parents[0] / "orchestrator" / "accel_pipeline"

CTX = {
    "engine_name": "fake", "benchmark_name": "tpch", "artifacts_dir": "/art/q3", "bench_target": "bench_base",
    "schema_sql": "workloads/tpch/schema.sql", "dataset_dir": "/data/tbl", "aux_test_target": "aux_bin",
    "aux_bench_target": "bench_aux", "aux_calibrate_target": "calibrator", "cost_model_config": "cm.json",
    "aux_ctest_smoke_regex": "^q3_.*smoke$", "aux_bench_suites": "lineitem_only",
    "aux_bench_work_dir_suffix": "q3 bench", "aux_write_frequencies": "0,1,10",
    "engine_host": {
        "differential_binary": "diff_bin", "differential_source": "d.cpp", "smoke_ready": False,
        "query_host_ready_hint": "it's \"generated\" by $host_bootstrap", "multi_conn_target": "multi_bin",
        "cross_accel_target": "cross_bin", "bench_target": "harness_bench", "calibrate_target": "harness_cal",
    },
}


def old_loader_values(ctx: dict) -> dict[str, str]:
    """What the per-field `python3 -c` spawns of the previous aux_gate_common.sh computed."""
    host = ctx.get("engine_host") or {}
    return {
        "ENGINE_NAME": ctx["engine_name"], "SCHEMA_SQL": ctx.get("schema_sql", ""),
        "DATASET_DIR": ctx.get("dataset_dir", ""), "GATE_ART": ctx["artifacts_dir"],
        "BENCHMARK_NAME": ctx["benchmark_name"], "DIFF_BINARY": host.get("differential_binary") or "",
        "DIFF_SOURCE": host.get("differential_source") or "",
        "SMOKE_READY": "1" if host.get("smoke_ready", True) else "0",
        "HOST_HINT": host.get("query_host_ready_hint") or "",
        "MULTI_CONN_TARGET": host.get("multi_conn_target") or "",
        "CROSS_ACCEL_TARGET": host.get("cross_accel_target") or "",
        "AUX_TEST_TARGET": ctx.get("aux_test_target", ""), "AUX_BENCH_TARGET": ctx.get("aux_bench_target", ""),
        "AUX_CALIBRATE_TARGET": ctx.get("aux_calibrate_target", ""),
        "AUX_CTEST_SMOKE_REGEX": ctx.get("aux_ctest_smoke_regex", ""), "BENCH_TARGET": ctx["bench_target"],
        "COST_MODEL_CONFIG": ctx.get("cost_model_config", ""),
        "AUX_BENCH_SUITES": ctx.get("aux_bench_suites", ""),
        "AUX_BENCH_WORK_DIR_SUFFIX": ctx.get("aux_bench_work_dir_suffix", ""),
        "AUX_WRITE_FREQUENCIES_CTX": ctx.get("aux_write_frequencies", ""),
        "HARNESS_BENCH_TARGET": host.get("bench_target") or "",
        "HARNESS_CALIBRATE_TARGET": host.get("calibrate_target") or "",
    }


def test_context_loader_gives_the_variables_of_the_per_field_loader():
    new = read_gate_context.shell_variables(CTX, aux=True)
    expected = old_loader_values(CTX)
    assert {k: new[k] for k in expected} == expected
    assert json.loads(new["CTX"]) == CTX


def test_context_loader_defaults_match_for_a_sparse_context():
    sparse = {"engine_name": "fake", "benchmark_name": "tpch", "artifacts_dir": "/a", "bench_target": "b"}
    new = read_gate_context.shell_variables(sparse, aux=True)
    assert {k: new[k] for k in old_loader_values(sparse)} == old_loader_values(sparse)


def test_shell_exports_survive_an_eval_with_quotes_and_dollars(tmp_path):
    ctx_path = tmp_path / "ctx.json"
    ctx_path.write_text(json.dumps(CTX))
    done = subprocess.run(
        [sys.executable, str(GATES / "read_gate_context.py"), "--shell", str(tmp_path), "3"],
        env={"ACCEL_GATE_CONTEXT": str(ctx_path), "PATH": "/usr/bin:/bin"}, capture_output=True, text=True, check=True,
    )
    shell = subprocess.run(["bash", "-c", f'{done.stdout}printf %s "$HOST_HINT"'], capture_output=True, text=True)
    assert shell.stdout == CTX["engine_host"]["query_host_ready_hint"]


def test_engine_only_loading_exports_just_the_engine_fields():
    assert set(read_gate_context.shell_variables(CTX, aux=False)) == {"CTX", "ENGINE_NAME", "SCHEMA_SQL", "DATASET_DIR"}


def test_gates_leave_phase_timings_beside_their_result(repo):
    proc, _, _, _, out_json = run_gate(repo, "run_aux_correctness.sh", "5")
    assert proc.returncode == 0, proc.stdout + proc.stderr
    phases = dict(line.split("=") for line in out_json.with_suffix(".phases").read_text().splitlines())
    assert {"load_context", "host_up", "host_load", "host_build", "run_aux_unit_bin", "run_diff_bin",
            "checker"} <= set(phases)
    assert all(float(seconds) >= 0 for seconds in phases.values())


def test_phase_timings_are_written_for_a_failing_gate_too(repo):
    proc, _, _, _, out_json = run_gate(repo, "run_aux_correctness.sh", "5", env_extra={"FAKE_RC_run": "1"})
    assert proc.returncode == 1
    assert "run_aux_unit_bin" in out_json.with_suffix(".phases").read_text()


@pytest.fixture()
def stamped_repo(repo):
    """The fake repo as a git checkout with the stamp tool, so engine_ready can stamp builds."""
    target = repo / "pipeline" / "orchestrator" / "accel_pipeline"
    target.mkdir(parents=True)
    shutil.copy(ORCHESTRATOR / "__init__.py", target)
    shutil.copy(ORCHESTRATOR / "layout.py", target)
    (repo / "accelerators" / "tpch" / "q5").mkdir(parents=True)
    (repo / "accelerators" / "tpch" / "q5" / "module.cpp").write_text("// v1\n")
    (repo / "accelerators" / "tpch" / "q6").mkdir()
    (repo / "accelerators" / "tpch" / "q6" / "module.cpp").write_text("// v1\n")
    (repo / ".gitignore").write_text("ctx.json\nhost.log\nhost.env.log\ngate_out.*\nart/\nstate/\nbuild-release/\ncost_model.json\n")
    git = ["git", "-c", "user.name=t", "-c", "user.email=t@t"]
    subprocess.run([*git[:1], "init", "-q"], cwd=repo, check=True)
    subprocess.run([*git, "add", "-A"], cwd=repo, check=True)
    subprocess.run([*git, "commit", "-qm", "base"], cwd=repo, check=True)
    return repo


def builds(calls: list[str]) -> int:
    return sum(1 for call in calls if call.startswith("build"))


def aux_correctness(repo, state, **extra):
    env = {"ACCEL_ENGINE_STATE_DIR": str(state), **extra}
    (repo / "host.log").unlink(missing_ok=True)
    return run_gate(repo, "run_aux_correctness.sh", "5", env_extra=env)


def test_an_unchanged_tree_skips_the_build_the_second_time(stamped_repo, tmp_path):
    first = aux_correctness(stamped_repo, tmp_path / "s")
    second = aux_correctness(stamped_repo, tmp_path / "s")
    assert first[0].returncode == 0 and second[0].returncode == 0, second[0].stdout + second[0].stderr
    assert builds(first[1]) == 1 and builds(second[1]) == 0
    assert "run diff_bin 5" in second[1]


def test_an_edit_to_the_query_rebuilds(stamped_repo, tmp_path):
    aux_correctness(stamped_repo, tmp_path / "s")
    (stamped_repo / "accelerators" / "tpch" / "q5" / "module.cpp").write_text("// v2\n")
    assert builds(aux_correctness(stamped_repo, tmp_path / "s")[1]) == 1


def test_an_edit_to_another_query_does_not_rebuild(stamped_repo, tmp_path):
    aux_correctness(stamped_repo, tmp_path / "s")
    (stamped_repo / "accelerators" / "tpch" / "q6" / "module.cpp").write_text("// v2\n")
    assert builds(aux_correctness(stamped_repo, tmp_path / "s")[1]) == 0


def test_a_shared_edit_rebuilds(stamped_repo, tmp_path):
    aux_correctness(stamped_repo, tmp_path / "s")
    (stamped_repo / "substrate").mkdir()
    (stamped_repo / "substrate" / "shared.hpp").write_text("// new\n")
    assert builds(aux_correctness(stamped_repo, tmp_path / "s")[1]) == 1


def test_a_fresh_instance_builds_again(stamped_repo, tmp_path):
    aux_correctness(stamped_repo, tmp_path / "one")
    assert builds(aux_correctness(stamped_repo, tmp_path / "two")[1]) == 1


def test_changed_cmake_args_rebuild(stamped_repo, tmp_path):
    aux_correctness(stamped_repo, tmp_path / "s")
    assert builds(aux_correctness(stamped_repo, tmp_path / "s", ACCEL_CMAKE_ARGS="-G Ninja -DX=1")[1]) == 1


def test_the_stamp_cache_can_be_turned_off(stamped_repo, tmp_path):
    aux_correctness(stamped_repo, tmp_path / "s")
    assert builds(aux_correctness(stamped_repo, tmp_path / "s", ACCEL_BUILD_STAMP_CACHE="0")[1]) == 1


def test_a_failed_build_is_not_remembered(stamped_repo, tmp_path):
    aux_correctness(stamped_repo, tmp_path / "s", FAKE_RC_build="1")
    assert builds(aux_correctness(stamped_repo, tmp_path / "s")[1]) == 1


def test_the_quick_tier_loads_the_subset_into_its_own_instance(repo):
    proc, _, log, _, _ = run_gate(repo, "run_aux_correctness.sh", "5",
                                  env_extra={"ACCEL_GATE_TIER": "quick", "ACCEL_SUBSET_DATASET_DIR": "/data/subset",
                                             "ACCEL_ENGINE_STATE_DIR": str(repo / "state" / "full")})
    assert proc.returncode == 0, proc.stdout + proc.stderr
    first = log.read_text().splitlines()[0]
    assert "instance=w7-fake-quick" in first and "data=/data/subset" in first
    assert "state/full/quick" in first  # inside the state dir, which the agents' sandbox mounts


def test_the_full_tier_ignores_the_subset(repo):
    _, _, log, _, _ = run_gate(repo, "run_aux_correctness.sh", "5", env_extra={"ACCEL_SUBSET_DATASET_DIR": "/data/subset"})
    assert "data=/data/tbl" in log.read_text().splitlines()[0]


def suite_ctx(repo, suite_yaml: str) -> dict:
    """A frozen one-tuple suite in the fake repo and the gate-context entry that points at it."""
    from dataclasses import asdict

    orchestrator = repo / "pipeline" / "orchestrator"
    orchestrator.symlink_to(GATES.parents[0] / "orchestrator")
    sys.path.insert(0, str(orchestrator))
    from accel_pipeline import data as ts

    suite = repo / "suite"
    suite.mkdir()
    (suite / "suite.yaml").write_text(suite_yaml)
    (suite / "params.sql").write_text("1\n")
    (repo / "q.sql").write_text("SELECT $1\n")
    (repo / "art").mkdir(exist_ok=True)
    ts.freeze(repo / "art", suite, query=5)
    return {"test_suite": {"dir": str(suite), "query_sql": str(repo / "q.sql"), "function": "f", "arity": 1,
                           "schema_sql": "", "config": asdict(ts.SuiteConfig()), "param_names": [], "aux": True}}


def test_the_quick_tier_caps_the_differential_probes_and_requires_none(repo):
    ctx = suite_ctx(repo, "point_writes: {max_probes: 9, min_count: 6}\n")
    host = repo / "backends" / "fake" / "host.sh"
    host.write_text('#!/usr/bin/env bash\n[[ "$1" != run ]] || echo "$2 probes=${ACCEL_ADV_MAX_PROBES:-unset}/${ACCEL_ADV_MIN_PROBES:-unset}" >> "${FAKE_LOG}.probes"\n')
    seen = {}
    for tier in ("quick", "full"):
        proc, *_ = run_gate(repo, "run_aux_correctness.sh", "5", ctx_extra=ctx, env_extra={"ACCEL_GATE_TIER": tier})
        assert proc.returncode == 0, proc.stdout + proc.stderr
        probes = (repo / "host.log.probes").read_text().splitlines()
        seen[tier] = next(line for line in probes if line.startswith("diff_bin")).split("=")[1]
        (repo / "host.log.probes").unlink()
    assert seen == {"quick": "3/0", "full": "9/6"}  # the subset may hold fewer answer-changing rows than min_count


def test_the_iterate_tier_uses_the_minimum_probes_the_full_dataset_and_one_thread_count(repo):
    ctx = suite_ctx(repo, "point_writes: {max_probes: 9, min_count: 6}\n")
    host = repo / "backends" / "fake" / "host.sh"
    host.write_text('#!/usr/bin/env bash\n[[ "$1" != run ]] || echo "$2 probes=${ACCEL_ADV_MAX_PROBES:-unset} threads=${ACCEL_AUX_THREADS:-n}" >> "${FAKE_LOG}.probes"\n')
    proc, *_ = run_gate(repo, "run_aux_correctness.sh", "5", ctx_extra=ctx,
                        env_extra={"ACCEL_GATE_TIER": "iterate", "ACCEL_SUBSET_DATASET_DIR": "/data/subset"})
    assert proc.returncode == 0, proc.stdout + proc.stderr
    runs = [line for line in (repo / "host.log.probes").read_text().splitlines() if line.startswith("diff_bin")]
    assert runs == ["diff_bin probes=6 threads=n"]   # one run (no threads=1 rerun), the min_count probes


def test_the_quick_tier_result_never_overwrites_the_full_one(repo):
    run_gate(repo, "run_aux_correctness.sh", "5")
    run_gate(repo, "run_aux_correctness.sh", "5", env_extra={"ACCEL_GATE_NAME": "aux_correctness_quick", "FAKE_RC_run": "1"})
    gates = repo / "art" / "gates"
    assert result(gates, "aux_correctness")["status"] == "pass"
    assert result(gates, "aux_correctness_quick")["status"] == "fail"


def test_aux_unit_runs_ctest_in_parallel_and_stops_on_the_first_failure(repo):
    _, calls, *_ = run_gate(repo, "run_aux_unit_tests.sh", "5")
    ctest = next(call for call in calls if call.startswith("run ctest"))
    assert " -j " in ctest and "--stop-on-failure" in ctest


def test_aux_unit_builds_the_binaries_of_the_tests_it_runs(repo, tmp_path):
    """A clean tree has no test binaries yet: the gate configures, lists the matching ctest tests, builds their
    binaries, and only then runs ctest."""
    bin_dir = tmp_path / "fakebin"
    bin_dir.mkdir()
    ctest = bin_dir / "ctest"
    ctest.write_text(
        "#!/usr/bin/env bash\n"
        "if [[ \"$*\" == *--show-only=json-v1* ]]; then\n"
        # the listing has no command for a binary that is not built; the add_test lines do
        "  printf 'add_test(\"a\" \"%s/test_unit_one\" \"x\")\\nadd_test(\"t\" \"/usr/bin/cmake\" \"-P\" \"y\")\\n"
        "add_test(\"b\" \"%s/test_unit_two\")\\n' \"$ACCEL_BUILD_DIR\" \"$ACCEL_BUILD_DIR\" > \"$ACCEL_BUILD_DIR/CTestTestfile.cmake\"\n"
        "  echo '{\"tests\": [{\"name\": \"a\"}, {\"name\": \"t\"}, {\"name\": \"b\"}]}'\n"
        "fi\n")
    ctest.chmod(0o755)
    _, calls, *_ = run_gate(repo, "run_aux_unit_tests.sh", "5",
                            env_extra={"PATH": f"{bin_dir}:{os.environ['PATH']}"})
    built = calls.index("build test_unit_one test_unit_two")
    assert built < next(i for i, call in enumerate(calls) if call.startswith("run ctest"))


def test_quick_check_builds_runs_smoke_then_differential(repo):
    proc, calls, _, gj, _ = run_gate(repo, "run_aux_quick_check.sh", "5", env_extra={"ACCEL_GATE_NAME": "quick_check", "ACCEL_AUX_THREADS": "1"})
    assert proc.returncode == 0, proc.stdout + proc.stderr
    assert calls[:2] == ["up", "load workloads/tpch/schema.sql"]
    assert calls[2] == "build aux_unit_bin diff_bin bench_aux"
    assert calls[3:] == ["run aux_unit_bin", "run diff_bin 5"]
    passed = result(gj, "quick_check")
    assert passed["status"] == "pass" and passed["engine_instance"] and passed["engine_state_dir"]  # what the measurements run in


def test_quick_check_builds_and_runs_the_agents_own_targets(repo):
    proc, calls, *_ = run_gate(repo, "run_aux_quick_check.sh", "5",
                               env_extra={"ACCEL_GATE_NAME": "quick_check", "ACCEL_QUICK_TARGETS": "my_test other_test", "ACCEL_AUX_THREADS": "1"})
    assert proc.returncode == 0, proc.stdout + proc.stderr
    assert calls[2] == "build aux_unit_bin diff_bin bench_aux my_test other_test"
    assert calls[3:] == ["run aux_unit_bin", "run my_test", "run other_test", "run diff_bin 5"]


def test_quick_check_without_the_differential(repo):
    _, calls, *_ = run_gate(repo, "run_aux_quick_check.sh", "5",
                            env_extra={"ACCEL_GATE_NAME": "quick_check", "ACCEL_QUICK_DIFFERENTIAL": "0"})
    assert calls[2:] == ["build aux_unit_bin bench_aux", "run aux_unit_bin"]


def test_a_sanitizer_check_builds_and_runs_only_the_differential_and_the_agents_targets(repo):
    proc, calls, *_ = run_gate(repo, "run_aux_quick_check.sh", "5", env_extra={
        "ACCEL_GATE_NAME": "quick_check", "ACCEL_QUICK_SANITIZER": "thread", "ACCEL_QUICK_TARGETS": "my_test", "ACCEL_AUX_THREADS": "1",
        "FAKE_EMBEDDED": "true"})
    assert proc.returncode == 0, proc.stdout + proc.stderr
    assert calls[3:] == ["build diff_bin my_test", "run my_test", "run diff_bin 5"]  # after the embedded check (info)  # the unit smoke and the bench stay out of it


def test_a_sanitizer_prebuild_builds_the_harness_and_nothing_else(repo):
    proc, calls, _, gj, _ = run_gate(repo, "run_aux_quick_check.sh", "5", env_extra={
        "ACCEL_GATE_NAME": "quick_check", "ACCEL_QUICK_SANITIZER": "thread", "ACCEL_QUICK_PREBUILD": "1", "FAKE_EMBEDDED": "true"})
    assert proc.returncode == 0, proc.stdout + proc.stderr
    assert calls[3:] == ["build diff_bin"] and result(gj, "quick_check")["status"] == "pass"


def test_a_failing_quick_check_names_the_step_and_how_to_reproduce_it(repo):
    proc, _, _, gj, _ = run_gate(repo, "run_aux_quick_check.sh", "5",
                                 env_extra={"ACCEL_GATE_NAME": "quick_check", "FAKE_RC_run": "1"})
    assert proc.returncode == 1
    data = result(gj, "quick_check")
    assert data["status"] == "fail" and data["step"] == "smoke"
    assert data["reproduce"].endswith("run aux_unit_bin") and "ACCEL_ENGINE_INSTANCE=" in data["reproduce"]


def test_a_failing_build_stops_the_quick_check_before_any_run(repo):
    proc, calls, _, gj, _ = run_gate(repo, "run_aux_quick_check.sh", "5",
                                     env_extra={"ACCEL_GATE_NAME": "quick_check", "FAKE_RC_build": "1"})
    assert proc.returncode == 1 and not any(call.startswith("run") for call in calls)
    assert result(gj, "quick_check")["step"] == "build"


def test_quick_check_makes_the_benchmarks_test_database_when_the_build_dir_has_none(repo):
    (repo / "build-release" / "test_dbs" / "tpch_test.db").unlink()
    proc, calls, _, _, _ = run_gate(repo, "run_aux_quick_check.sh", "5")
    assert proc.returncode == 0, proc.stdout + proc.stderr
    runs = [c for c in calls if c.startswith("run ")]
    assert "-R ^tpch_test_db_fixture$" in runs[0] and runs[1] == "run aux_unit_bin"

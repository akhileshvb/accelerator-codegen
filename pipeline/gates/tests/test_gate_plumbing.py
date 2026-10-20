"""Gate plumbing against a fake backends/fake/host.sh: every gate reaches the engine
only through host.sh verbs, with the host-contract environment, and names no engine."""

from __future__ import annotations

import json
import os
import re
import shutil
import subprocess
from pathlib import Path

import pytest

GATES = Path(__file__).resolve().parents[1]

FAKE_HOST = r"""#!/usr/bin/env bash
echo "$* | instance=${ACCEL_ENGINE_INSTANCE} state=${ACCEL_ENGINE_STATE_DIR} build=${ACCEL_BUILD_DIR} data=${ACCEL_DATASET_DIR}" >> "${FAKE_LOG}"
verb="$1"; shift
rc_var="FAKE_RC_${verb}"
case "${verb}" in
  info) echo "{\"engine\": \"fake\", \"embedded\": ${FAKE_EMBEDDED:-false}, \"verbs\": []}" ;;
  run)
    echo "${FAKE_RUN_OUT:-ran $*}"
    if [[ "$1" == "calibrator" ]]; then
      echo '{"interference_delta_per_query_us": 3, "interference_rebuild_per_query_us": 4}' > "${FAKE_COST_MODEL}"
    fi ;;
  build)  # configures the build dir from ACCEL_CMAKE_ARGS, as the contract asks (unless told not to)
    args="${FAKE_IGNORE_CMAKE_ARGS:+-G Ninja}${FAKE_IGNORE_CMAKE_ARGS:-${ACCEL_CMAKE_ARGS}}"
    switch=OFF; [[ "${args}" == *-DACCELERATOR_CODEGEN_BUILD_FAKE=ON* ]] && switch=ON
    type="$(sed -nE 's/.*-DCMAKE_BUILD_TYPE=([^ ]+).*/\1/p' <<<"${args}")"
    printf 'ACCELERATOR_CODEGEN_BUILD_FAKE:BOOL=%s\nCMAKE_BUILD_TYPE:STRING=%s\n' "${switch}" "${type}" \
      > "${ACCEL_BUILD_DIR}/CMakeCache.txt"
    echo "build ok" ;;
  *) echo "${verb} ok" ;;
esac
exit "${!rc_var:-0}"
"""


@pytest.fixture()
def repo(tmp_path):
    root = tmp_path / "repo"
    shutil.copytree(GATES, root / "pipeline" / "gates", ignore=shutil.ignore_patterns("tests", "__pycache__"))
    (root / "backends" / "fake").mkdir(parents=True)
    host = root / "backends" / "fake" / "host.sh"
    host.write_text(FAKE_HOST)
    host.chmod(0o755)
    (root / "scripts").mkdir()
    (root / "scripts" / "check_backend_query_free.py").write_text("import sys; sys.exit(0)\n")
    (root / "build-release").mkdir()
    return root


def run_gate(repo, script, *args, ctx_extra=None, env_extra=None, kind="aux_state"):
    art = repo / "art"
    ctx = {
        "engine_name": "fake", "benchmark_name": "tpch", "artifacts_dir": str(art),
        "workload_kind": kind, "bench_target": "bench_base", "schema_sql": "workloads/tpch/schema.sql",
        "dataset_dir": "/data/tbl", "aux_test_target": "aux_unit_bin", "aux_bench_target": "bench_aux",
        "aux_calibrate_target": "calibrator", "cost_model_config": "cost_model.json",
        "engine_host": {"differential_binary": "diff_bin", "multi_conn_target": "multi_bin",
                        "cross_accel_target": "cross_bin"},
        "target_paths": {"contract_test_target": "contract_bin", "adversarial_test_target": "adv_bin"},
    }
    ctx.update(ctx_extra or {})
    ctx_path = repo / "ctx.json"
    ctx_path.write_text(json.dumps(ctx))
    log = repo / "host.log"
    out_json = repo / "gate_out.json"
    env = {
        **os.environ, "ACCEL_GATE_CONTEXT": str(ctx_path), "FAKE_LOG": str(log),
        "ACCEL_GATE_OUT": str(out_json), "ACCEL_PIPELINE_WORKER_ID": "w7",
        "ACCEL_ENGINE_STATE_ROOT": str(repo / "state"), "FAKE_COST_MODEL": str(repo / "cost_model.json"),
        "ACCEL_PIPELINE_BUILD_DIR": str(repo / "build-release"), **(env_extra or {}),
    }
    env.pop("ACCEL_ENGINE_INSTANCE", None)
    env.pop("ACCEL_ENGINE_STATE_DIR", None)
    env.pop("ACCEL_CMAKE_ARGS", None)
    proc = subprocess.run(["bash", str(repo / "pipeline" / "gates" / script), *args], cwd=repo,
                          env=env, capture_output=True, text=True, timeout=120)
    calls = [l.split(" | ")[0] for l in log.read_text().splitlines()] if log.exists() else []
    gate_json = art / "gates"
    return proc, calls, log, gate_json, out_json


def result(path: Path, name: str) -> dict:
    return json.loads((path / f"{name}.json").read_text())


def test_engine_build_passes_when_host_configures_with_the_cmake_args(repo):
    proc, calls, _, _, out = run_gate(repo, "run_engine_build.sh", kind="engine_backend")
    assert proc.returncode == 0, proc.stdout + proc.stderr
    assert calls == ["info", "up", "build"]
    assert json.loads(out.read_text())["status"] == "pass"


def test_engine_build_fails_when_the_backend_is_not_in_the_build(repo):
    proc, _, _, _, out = run_gate(repo, "run_engine_build.sh", kind="engine_backend",
                                  env_extra={"FAKE_IGNORE_CMAKE_ARGS": "1"})
    assert proc.returncode != 0
    assert "ACCEL_CMAKE_ARGS" in json.loads(out.read_text())["reason"]


def test_correctness_runs_up_load_build_run_with_contract_env(repo):
    proc, calls, log, gj, _ = run_gate(repo, "run_correctness.sh", "3")
    assert proc.returncode == 0, proc.stdout + proc.stderr
    assert calls == ["up", "load workloads/tpch/schema.sql", "build diff_bin", "run diff_bin 3"]
    first = log.read_text().splitlines()[0]
    assert "instance=w7-fake" in first
    assert "/state/w7-fake" in first or "/flash1/" in first or "state=" in first
    assert "data=/data/tbl" in first and "build=" in first
    assert result(gj, "correctness")["status"] == "pass"


def test_correctness_fails_when_run_fails(repo):
    proc, calls, _, gj, _ = run_gate(repo, "run_correctness.sh", "3", env_extra={"FAKE_RC_run": "1"})
    assert proc.returncode == 1
    assert result(gj, "correctness")["status"] == "fail"


def test_correctness_fails_when_build_fails_without_running(repo):
    proc, calls, _, gj, _ = run_gate(repo, "run_correctness.sh", "3", env_extra={"FAKE_RC_build": "1"})
    assert proc.returncode == 1
    assert not any(c.startswith("run") for c in calls)
    assert "host.sh build failed" in result(gj, "correctness")["reason"]


def test_correctness_without_differential_target_fails_closed(repo):
    proc, calls, _, gj, _ = run_gate(repo, "run_correctness.sh", "3", ctx_extra={"engine_host": {}})
    assert proc.returncode == 1 and calls == []


@pytest.mark.parametrize("rc,status,exit_code", [(0, "pass", 0), (77, "skip", 0), (1, "fail", 1)])
def test_slot_gate_calls_host_slot(repo, rc, status, exit_code):
    proc, calls, _, _, out_json = run_gate(
        repo, "run_slot_gates.sh", env_extra={"ACCEL_GATE_NAME": "slot_txn_edges", "FAKE_RC_slot": str(rc),
                                              "ACCEL_SLOT_GATE_SECONDS": "2"})
    assert proc.returncode == exit_code
    assert calls == ["slot txn_edges 2"]
    assert json.loads(out_json.read_text())["status"] == status


def test_aux_correctness_runs_smoke_then_differential(repo):
    proc, calls, _, gj, _ = run_gate(repo, "run_aux_correctness.sh", "5")
    assert proc.returncode == 0, proc.stdout + proc.stderr
    assert calls == ["up", "load workloads/tpch/schema.sql", "build aux_unit_bin diff_bin",
                     "run aux_unit_bin", "run diff_bin 5"]
    assert result(gj, "aux_correctness")["status"] == "pass"


def test_aux_correctness_differential_failure_fails(repo):
    proc, _, _, gj, _ = run_gate(repo, "run_aux_correctness.sh", "5", env_extra={"FAKE_RC_run": "1"})
    assert proc.returncode == 1
    assert result(gj, "aux_correctness")["status"] == "fail"


@pytest.mark.parametrize("script,name,target", [
    ("run_aux_multi_connection.sh", "aux_multi_connection", "multi_bin"),
    ("run_aux_cross_accel.sh", "aux_cross_accel", "cross_bin"),
])
def test_harness_gates_run_their_target_through_host(repo, script, name, target):
    proc, calls, _, gj, _ = run_gate(repo, script, "2")
    assert proc.returncode == 0, proc.stdout + proc.stderr
    assert calls[-1] == f"run {target} 2"
    assert result(gj, name)["status"] == "pass"


def test_calibrate_actually_runs_the_calibrator(repo):
    proc, calls, _, gj, _ = run_gate(repo, "run_aux_cost_model_calibrate.sh", "4",
                                     ctx_extra={"cost_model_config": "cost_model.json"})
    assert proc.returncode == 0, proc.stdout + proc.stderr
    assert any(c.startswith("run calibrator") for c in calls)
    assert result(gj, "aux_cost_model_calibrate")["status"] == "pass"


def test_calibrate_does_not_reuse_an_existing_cost_model(repo):
    (repo / "cost_model.json").write_text('{"interference_delta_per_query_us": 9, "interference_rebuild_per_query_us": 9}')
    _, calls, *_ = run_gate(repo, "run_aux_cost_model_calibrate.sh", "4")
    assert any(c.startswith("run calibrator") for c in calls)


def test_baseline_scores_both_axes_for_aux_state(repo):
    out = "read_only_query_speedup,5.0\nwrite_mix_workload_speedup,2.0"
    proc, calls, _, gj, _ = run_gate(repo, "run_baseline.sh", "6", env_extra={"FAKE_RUN_OUT": out})
    assert proc.returncode == 0, proc.stdout + proc.stderr
    assert calls[-2:] == ["run bench_base 6 0", "run bench_base 6 10"]
    assert result(gj, "baseline")["read_only_query_speedup"] == 5.0


def test_baseline_fails_below_speedup_floor(repo):
    out = "read_only_query_speedup,1.1\nwrite_mix_workload_speedup,2.0"
    proc, *_ = run_gate(repo, "run_baseline.sh", "6", env_extra={"FAKE_RUN_OUT": out})
    assert proc.returncode == 1


def test_engine_contract_and_adversarial_use_target_paths(repo):
    for script, target in (("run_engine_contract.sh", "contract_bin"), ("run_engine_adversarial.sh", "adv_bin")):
        proc, calls, *_ = run_gate(repo, script, kind="engine_backend")
        assert proc.returncode == 0, proc.stdout + proc.stderr
        assert calls == ["up", f"build {target}", f"run {target}"]
        (repo / "host.log").unlink()


@pytest.fixture()
def api_repo(repo):
    (repo / "scripts" / "check_layer_includes.py").write_text("import sys; sys.exit(0)\n")
    api = repo / "backends" / "fake" / "api"
    api.mkdir()
    (api / "hooks.hpp").write_text("// accel_get_execute_counts accel_invalidate_all\n")
    rows = "\n".join(f"| {r} | | |" for r in ("registration", "runtime", "scan", "sql_fallback", "cost_model",
                                              "maintenance", "thread_knobs", "value_semantics"))
    (api / "README.md").write_text(
        "## Contract roles\n\n| role | headers | symbols |\n|---|---|---|\n"
        "| introspection | `api/hooks.hpp` | `accel_get_execute_counts`, `accel_invalidate_all` |\n" + rows + "\n")
    return repo


def test_api_conformance_calls_every_required_hook_and_the_listed_optional_one(api_repo):
    proc, calls, *_ = run_gate(api_repo, "run_engine_api_conformance.sh", kind="engine_backend")
    assert proc.returncode == 0, proc.stdout + proc.stderr
    assert calls == ["up", "build api_conformance_fake", "build", "info"] + ["sql -"] * 5


def test_api_conformance_skips_the_sql_hook_calls_on_an_embedded_engine(api_repo):
    proc, calls, *_ = run_gate(api_repo, "run_engine_api_conformance.sh", kind="engine_backend",
                               env_extra={"FAKE_EMBEDDED": "true"})
    assert proc.returncode == 0, proc.stdout + proc.stderr
    assert "sql -" not in calls


def test_api_conformance_fails_when_a_hook_errors(api_repo):
    proc, _, _, _, out = run_gate(api_repo, "run_engine_api_conformance.sh", kind="engine_backend",
                                  env_extra={"FAKE_RC_sql": "1"})
    assert proc.returncode != 0
    assert "accel_get_execute_counts()" in json.loads(out.read_text())["reason"]


def test_adversarial_gate_rejects_template_stubs(repo):
    src = repo / "adv.cpp"
    src.write_text('void TestAdversarialCapture() { Fail("TestAdversarialCapture not implemented"); }\n')
    ctx = {"target_paths": {"adversarial_test": "adv.cpp", "adversarial_test_target": "adv_bin"}}
    proc, calls, _, _, out = run_gate(repo, "run_engine_adversarial.sh", kind="engine_backend", ctx_extra=ctx)
    assert proc.returncode != 0 and "template stubs" in json.loads(out.read_text())["reason"]
    assert "run adv_bin" not in calls


def test_tsan_gate_skips_non_embedded_engine_by_host_info(repo):
    proc, calls, *_ , = run_gate(repo, "run_aux_tsan.sh", "5", env_extra={"FAKE_EMBEDDED": "false"})
    assert proc.returncode == 0
    assert calls == ["info"]
    assert "embedded=false" in proc.stdout


def test_gates_name_no_engine():
    pattern = re.compile(r"duckdb|postgres|mysql|docker|psql", re.IGNORECASE)
    offenders = {}
    for path in sorted(GATES.glob("*.sh")) + sorted(GATES.glob("*.py")):
        hits = [l for l in path.read_text().splitlines() if pattern.search(l) and "SKIP_DUCKDB_REF" not in l
                and "skip_duckdb_ref" not in l]
        if hits:
            offenders[path.name] = hits
    assert not offenders


HARNESS_HOST = {
    "differential_binary": "test_fake_tpch_differential", "multi_conn_target": "test_fake_aux_multi_connection_stress",
    "cross_accel_target": "test_fake_cross_accel", "bench_target": "benchmark_fake_tpch_aux",
    "calibrate_target": "calibrate_fake_tpch_aux_cost_model",
    "differential_source": "backends/fake/tests/harness/test_fake_tpch_differential.cpp",
}


def host_bootstrap_repo(repo):
    harness = repo / "backends" / "fake" / "tests" / "harness"
    harness.mkdir(parents=True)
    (harness / "test_fake_tpch_differential.cpp").write_text("// harness\n")
    (repo / "backends" / "fake" / "tests" / "harness.cmake").write_text("# targets\n")


def test_host_bootstrap_builds_every_harness_and_smokes_each(repo):
    host_bootstrap_repo(repo)
    proc, calls, _, gj, _ = run_gate(repo, "run_host_bootstrap.sh", "0", ctx_extra={"engine_host": HARNESS_HOST})
    assert proc.returncode == 0, proc.stdout + proc.stderr
    assert calls[:3] == ["up", "load workloads/tpch/schema.sql", "build " + " ".join(
        HARNESS_HOST[k] for k in ("differential_binary", "multi_conn_target", "cross_accel_target",
                                  "bench_target", "calibrate_target"))]
    assert calls[3:] == [f"run {t} --help" for t in (
        "test_fake_tpch_differential", "test_fake_aux_multi_connection_stress", "test_fake_cross_accel",
        "benchmark_fake_tpch_aux", "calibrate_fake_tpch_aux_cost_model")]
    assert result(gj, "host_bootstrap")["status"] == "pass"


def test_host_bootstrap_smokes_the_differential_on_a_registered_query(repo):
    host_bootstrap_repo(repo)
    register = repo / "accelerators" / "tpch" / "q6" / "fake"
    register.mkdir(parents=True)
    (register / "register.cpp").write_text("// q6\n")
    proc, calls, _, _, _ = run_gate(repo, "run_host_bootstrap.sh", "0", ctx_extra={"engine_host": HARNESS_HOST})
    assert proc.returncode == 0, proc.stdout + proc.stderr
    assert "run test_fake_tpch_differential 6" in calls


def test_host_bootstrap_fails_without_the_harness_sources(repo):
    proc, _, _, gj, _ = run_gate(repo, "run_host_bootstrap.sh", "0", ctx_extra={"engine_host": HARNESS_HOST})
    assert proc.returncode == 1
    assert "missing backends/fake/tests/harness" in result(gj, "host_bootstrap")["reason"]


def test_host_bootstrap_fails_when_a_smoke_fails(repo):
    host_bootstrap_repo(repo)
    proc, _, _, gj, _ = run_gate(repo, "run_host_bootstrap.sh", "0", ctx_extra={"engine_host": HARNESS_HOST},
                                 env_extra={"FAKE_RC_run": "1"})
    assert proc.returncode == 1
    assert "smoke failed" in result(gj, "host_bootstrap")["reason"]

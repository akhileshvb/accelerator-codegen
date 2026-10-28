"""run_engine_canary.sh against a fake backends/fake/host.sh: the canary binary is
reached only through host.sh, its JSON lines become the gate result (owning layers of the failures), and
sanitizer reports and hangs fail the gate."""

from __future__ import annotations

import json
import os
import shutil
import subprocess
from pathlib import Path

import pytest

GATES = Path(__file__).resolve().parents[1]

FAKE_HOST = r"""#!/usr/bin/env bash
verb="$1"; shift
case "${verb}" in
  info) echo "{\"engine\": \"fake\", \"embedded\": ${FAKE_EMBEDDED:-true}, \"verbs\": []}" ;;
  up) echo "up" >> "${FAKE_LOG}" ;;
  build) echo "build $* cmake_args=${ACCEL_CMAKE_ARGS:-} dir=${ACCEL_BUILD_DIR}" >> "${FAKE_LOG}"; exit "${FAKE_RC_build:-0}" ;;
  run)
    echo "run $* asan=${ASAN_OPTIONS:+set}" >> "${FAKE_LOG}"
    printf '%s\n' "${FAKE_OUTPUT}"
    [[ -z "${FAKE_STDERR:-}" ]] || printf '%s\n' "${FAKE_STDERR}" >&2
    exit "${FAKE_RC_run:-0}" ;;
esac
"""

PASS_LINE = '{"check": "hit_after_first_read", "owner_layer": "api", "status": "pass", "seconds": 1.0, "defect_class": "d", "detail": ""}'
FAIL_LINE = ('{"check": "dml_incremental_hits", "owner_layer": "capture", "status": "fail", "seconds": 2.0, '
             '"defect_class": "a write forcing a rebuild", "detail": "builds 1 -> 2"}')
ASAN = """==1==ERROR: AddressSanitizer: stack-use-after-return on address 0x1
READ of size 3 at 0x1 thread T0
    #0 0x1 in accelerator_codegen::TextAccessor::TextView /repo/backends/fake/api/scan.hpp:99
"""


@pytest.fixture()
def repo(tmp_path):
    root = tmp_path / "repo"
    shutil.copytree(GATES, root / "pipeline" / "gates", ignore=shutil.ignore_patterns("tests", "__pycache__"))
    (root / "backends" / "fake" / "tests").mkdir(parents=True)
    (root / "backends" / "fake" / "tests" / "test_fake_canary.cpp").write_text("// binding\n")
    host = root / "backends" / "fake" / "host.sh"
    host.write_text(FAKE_HOST)
    host.chmod(0o755)
    return root


def run(repo, script="run_engine_canary.sh", gate="engine_canary", known_gaps=(), layers=None, **env_extra):
    ctx = {"engine_name": "fake", "schema_sql": "schema.sql", "dataset_dir": "/data", "target_paths": {},
           "known_gaps": list(known_gaps), "implementation_layers": layers or []}
    (repo / "ctx.json").write_text(json.dumps(ctx))
    (repo / "host.log").unlink(missing_ok=True)
    out = repo / "gate.json"
    env = {**os.environ, "ACCEL_GATE_CONTEXT": str(repo / "ctx.json"), "ACCEL_GATE_OUT": str(out), "ACCEL_GATE_NAME": gate,
           "FAKE_LOG": str(repo / "host.log"), "ACCEL_ENGINE_STATE_DIR": str(repo / "state"),
           "ACCEL_BUILD_DIR": str(repo / "build"), "ACCEL_CMAKE_ARGS": "-G Ninja", "TMPDIR": str(repo),
           **{k: str(v) for k, v in env_extra.items()}}
    proc = subprocess.run(["bash", str(repo / "pipeline/gates" / script), "0"], cwd=repo, env=env,
                          capture_output=True, text=True, timeout=60)
    log = (repo / "host.log").read_text().splitlines() if (repo / "host.log").exists() else []
    return proc, log, json.loads(out.read_text())


def test_all_checks_passing_is_a_pass_through_host_sh(repo):
    proc, log, result = run(repo, FAKE_OUTPUT=PASS_LINE)
    assert proc.returncode == 0, proc.stdout + proc.stderr
    assert result["status"] == "pass" and result["checks"][0]["check"] == "hit_after_first_read"
    assert log == ["up", f"build test_fake_canary cmake_args=-G Ninja dir={repo}/build", "run test_fake_canary asan=set"]


def test_a_failing_check_fails_the_gate_and_names_the_owning_layer(repo):
    proc, _, result = run(repo, FAKE_OUTPUT=PASS_LINE + "\n" + FAIL_LINE, FAKE_RC_run=1)
    assert proc.returncode == 1 and result["status"] == "fail"
    assert "dml_incremental_hits" in result["reason"] and "builds 1 -> 2" in result["reason"]
    assert result["owner_layer"] == "capture" and result["owner_layers"] == ["capture"]


def test_a_sanitizer_report_fails_with_the_report_in_the_log(repo):
    proc, _, result = run(repo, FAKE_OUTPUT=FAIL_LINE, FAKE_STDERR=ASAN, FAKE_RC_run=1)
    assert result["status"] == "fail" and "sanitizer report" in result["reason"]
    assert "backends/fake/api/scan.hpp:99" in result["log_tail"]


def test_a_hung_binary_fails_the_gate(repo):
    _, _, result = run(repo, FAKE_OUTPUT="", FAKE_RC_run=124)
    assert result["status"] == "fail" and "did not finish" in result["reason"]


def test_the_sanitizer_tiers_build_their_own_tree_and_skip_server_engines(repo):
    _, log, result = run(repo, gate="engine_canary_asan", FAKE_OUTPUT=PASS_LINE)
    assert result["status"] == "pass"
    assert f"dir={repo}/build-asan" in log[1] and "-DACCEL_SANITIZE=address" in log[1]
    _, log, result = run(repo, gate="engine_canary_tsan", FAKE_OUTPUT=PASS_LINE)
    assert "-DACCEL_SANITIZE=thread" in log[1] and "--check bulk_insert_threads" in log[2]
    proc, log, result = run(repo, gate="engine_canary_asan", FAKE_OUTPUT=PASS_LINE, FAKE_EMBEDDED="false")
    assert proc.returncode == 0 and result["status"] == "skip" and log == []


def test_a_backend_without_the_binding_fails_unless_it_is_a_known_gap(repo):
    (repo / "backends/fake/tests/test_fake_canary.cpp").unlink()
    proc, log, result = run(repo)
    assert proc.returncode == 1 and result["status"] == "fail" and result["owner_layer"] == "api"
    assert "canary_binding.cpp.tpl" in result["reason"] and log == []
    proc, _, result = run(repo, known_gaps=["engine_canary"])
    assert proc.returncode == 0 and result["status"] == "skip" and "known gap" in result["reason"]


def test_a_subset_selection_reaches_the_binary(repo):
    _, log, _ = run(repo, FAKE_OUTPUT=PASS_LINE, ACCEL_CANARY_CHECKS="a b", ACCEL_CANARY_OWNERS="scan")
    assert log[-1].startswith("run test_fake_canary --check a --check b --owner scan")


def _report(*frames, kind="data race"):
    lines = [f"WARNING: ThreadSanitizer: {kind} (pid=1)", "  Write of size 8 at 0x1 by thread T1:"]
    lines += [f"    #{i} {frame}" for i, frame in enumerate(frames)]
    lines += ["", "  Location is global 'x' of size 24 at 0x1", ""]
    return "\n".join(lines)


def test_tsan_races_are_ours_by_the_innermost_non_library_frame(tmp_path):
    import importlib.util

    spec = importlib.util.spec_from_file_location("canary_result", GATES / "canary_result.py")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    backend_race = _report("std::vector::push_back /usr/include/c++/16/bits/stl_vector.h:1 (a.so+0x1)",
                           "fake::Host::OnMutation /repo/backends/fake/api/host.cpp:416 (a.so+0x2)",
                           "main /repo/pipeline/canary/canary_engine.hpp:33 (t+0x3)")
    engine_noise = _report("pthread_mutex_trylock <null> (libtsan.so.2+0x1)", "je_tcache_arena_associate <null> (libengine.so+0x2)",
                           "Session::Exec /repo/pipeline/canary/canary_engine.hpp:33 (t+0x3)", kind="double lock of a mutex")
    (tmp_path / "tsan.1").write_text(backend_race + "\n" + engine_noise)
    ours, engine_only, text = module.tsan_reports(tmp_path, "fake", "/repo")
    assert (ours, engine_only) == (1, 1) and "OnMutation" in text

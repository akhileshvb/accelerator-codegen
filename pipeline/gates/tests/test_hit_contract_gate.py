"""run_engine_hit_contract.sh against a fake backends/fake/host.sh: it reaches the engine only through
host.sh, passes ACCEL_FALLBACK_TRAP=1 to the harness, and maps its exit status to pass / fail / skip."""

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
  info) echo "{\"engine\": \"fake\", \"embedded\": false, \"verbs\": []${FAKE_HIT_CONTRACT:+, \"hit_contract\": true}}" ;;
  run)  echo "run $* trap=${ACCEL_FALLBACK_TRAP:-unset}" >> "${FAKE_LOG}"; echo "harness output"; exit "${FAKE_RC_run:-0}" ;;
  build) echo "build $*" >> "${FAKE_LOG}"; exit "${FAKE_RC_build:-0}" ;;
  *) echo "${verb}" >> "${FAKE_LOG}" ;;
esac
"""


@pytest.fixture()
def repo(tmp_path):
    root = tmp_path / "repo"
    shutil.copytree(GATES, root / "pipeline" / "gates", ignore=shutil.ignore_patterns("tests", "__pycache__"))
    (root / "backends" / "fake").mkdir(parents=True)
    host = root / "backends" / "fake" / "host.sh"
    host.write_text(FAKE_HOST)
    host.chmod(0o755)
    return root


def run_gate(repo, *, contract=True, target_paths=None, known_gaps=(), **env_extra):
    ctx = {"engine_name": "fake", "schema_sql": "schema.sql", "dataset_dir": "/data", "target_paths": target_paths or {},
           "known_gaps": list(known_gaps)}
    (repo / "ctx.json").write_text(json.dumps(ctx))
    out = repo / "gate.json"
    env = {
        **os.environ, "ACCEL_GATE_CONTEXT": str(repo / "ctx.json"), "ACCEL_GATE_OUT": str(out),
        "FAKE_LOG": str(repo / "host.log"), "ACCEL_ENGINE_STATE_ROOT": str(repo / "state"),
        "ACCEL_ENGINE_STATE_DIR": str(repo / "state"), "ACCEL_BUILD_DIR": str(repo / "build"),
        **{k: str(v) for k, v in env_extra.items()},
    }
    if contract:
        env["FAKE_HIT_CONTRACT"] = "1"
    proc = subprocess.run(["bash", str(repo / "pipeline/gates/run_engine_hit_contract.sh"), "0"], cwd=repo, env=env,
                          capture_output=True, text=True, timeout=60)
    log = (repo / "host.log").read_text().splitlines() if (repo / "host.log").exists() else []
    return proc, log, json.loads(out.read_text())


def test_pass_runs_the_conventional_target_with_the_trap(repo):
    proc, log, result = run_gate(repo)
    assert proc.returncode == 0, proc.stdout + proc.stderr
    assert result["status"] == "pass"
    assert log == ["up", "load", "build test_fake_hit_contract", "run test_fake_hit_contract trap=1"]


def test_target_comes_from_the_gate_context(repo):
    _, log, _ = run_gate(repo, target_paths={"hit_contract_test_target": "custom_hit"})
    assert log[-1] == "run custom_hit trap=1"


def test_violation_fails_with_the_harness_output(repo):
    proc, _, result = run_gate(repo, FAKE_RC_run=1)
    assert proc.returncode == 1
    assert result["status"] == "fail" and "exited 1" in result["reason"]
    assert "harness output" in result["log_tail"]


def test_exit_77_skips(repo):
    proc, _, result = run_gate(repo, FAKE_RC_run=77)
    assert proc.returncode == 0 and result["status"] == "skip"


def test_backend_without_the_contract_fails_without_building(repo):
    proc, log, result = run_gate(repo, contract=False)
    assert proc.returncode == 1 and result["status"] == "fail"
    assert "required" in result["reason"] and result["owner_layer"] == "api"
    assert log == []


def test_a_known_gap_of_a_hand_kept_backend_skips(repo):
    proc, log, result = run_gate(repo, contract=False, known_gaps=["engine_hit_contract"])
    assert proc.returncode == 0 and result["status"] == "skip"
    assert "known gap" in result["reason"]
    assert log == []


def test_build_failure_fails(repo):
    proc, log, result = run_gate(repo, FAKE_RC_build=1)
    assert proc.returncode == 1 and result["status"] == "fail"
    assert not any(line.startswith("run") for line in log)

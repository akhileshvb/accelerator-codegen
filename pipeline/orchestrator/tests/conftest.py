"""Fixtures shared by the quick_check and write-cost tests: a git repo with one query dir, and a scripted gate."""

from __future__ import annotations

import subprocess
from types import SimpleNamespace

import pytest

from accel_pipeline import gates
from accel_pipeline.config import QuickTierConfig
from accel_pipeline.gates import GateResult

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
        monkeypatch.setattr(gates, "run_gate", self.run_gate)
        monkeypatch.setattr(gates, "ensure_cut_datasets", lambda *a: None)

    def run_gate(self, config, name, query, workload, *, env=None, timeout_s=None, **kwargs):
        self.calls.append({"name": name, "query": query, "env": env, "timeout_s": timeout_s})
        return GateResult(name, self.status, {"status": self.status, "phase_timings": {"host_build": 1.5}, **self.data})

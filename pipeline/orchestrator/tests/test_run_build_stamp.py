"""run_build skips the build when the code is what the last successful build saw."""

from __future__ import annotations

import subprocess
from types import SimpleNamespace

import pytest

from accel_pipeline import optimization_smoke
from accel_pipeline.config import QuickTierConfig

GIT = ["git", "-c", "user.name=t", "-c", "user.email=t@t"]
WORKLOAD = {"workload_kind": "aux_state", "benchmark_name": "demo"}


@pytest.fixture()
def setup(tmp_path, monkeypatch):
    root = tmp_path / "repo"
    (root / "accelerators" / "demo" / "q4").mkdir(parents=True)
    (root / "accelerators" / "demo" / "q4" / "module.cpp").write_text("// v1\n")
    subprocess.run(["git", "init", "-q"], cwd=root, check=True)
    subprocess.run([*GIT, "add", "-A"], cwd=root, check=True)
    subprocess.run([*GIT, "commit", "-qm", "base"], cwd=root, check=True)
    build = tmp_path / "build"
    build.mkdir()
    (build / "CMakeCache.txt").write_text("")
    config = SimpleNamespace(repo_root=root, build_dir=build, quick_tier=QuickTierConfig())
    builds: list[list[str]] = []

    def fake_run(cmd, **kwargs):
        builds.append(cmd)
        return SimpleNamespace(returncode=0, stdout="ok", stderr="")

    monkeypatch.setattr(optimization_smoke, "subprocess", SimpleNamespace(run=fake_run))
    monkeypatch.setattr("accel_pipeline.worker_isolation.ensure_worker_build_dir", lambda *a, **k: None)
    monkeypatch.setattr(optimization_smoke, "resolve_workload_paths",
                        lambda *a: {"aux_bench_target": "bench_t", "aux_test_target": "test_t"})
    return config, builds


def test_the_second_build_of_unchanged_code_is_a_no_op(setup):
    config, builds = setup
    assert optimization_smoke.run_build(config, WORKLOAD, 4)[0]
    assert len(builds) == 2
    ok, out = optimization_smoke.run_build(config, WORKLOAD, 4)
    assert ok and "skipped" in out and len(builds) == 2


def test_an_edit_builds_again(setup):
    config, builds = setup
    optimization_smoke.run_build(config, WORKLOAD, 4)
    (config.repo_root / "accelerators" / "demo" / "q4" / "module.cpp").write_text("// v2\n")
    optimization_smoke.run_build(config, WORKLOAD, 4)
    assert len(builds) == 4


def test_a_failed_build_is_not_remembered(setup, monkeypatch):
    config, builds = setup
    monkeypatch.setattr(
        optimization_smoke, "subprocess",
        SimpleNamespace(run=lambda cmd, **k: builds.append(cmd) or SimpleNamespace(returncode=1, stdout="", stderr="boom")),
    )
    assert not optimization_smoke.run_build(config, WORKLOAD, 4)[0]
    assert not optimization_smoke.run_build(config, WORKLOAD, 4)[0]
    assert len(builds) == 2


def test_an_unconfigured_build_dir_always_builds(setup):
    config, builds = setup
    optimization_smoke.run_build(config, WORKLOAD, 4)
    (config.build_dir / "CMakeCache.txt").unlink()
    optimization_smoke.run_build(config, WORKLOAD, 4)
    assert len(builds) == 4


def test_the_cache_can_be_turned_off(setup):
    config, builds = setup
    config.quick_tier = QuickTierConfig(build_stamp_cache=False)
    optimization_smoke.run_build(config, WORKLOAD, 4)
    optimization_smoke.run_build(config, WORKLOAD, 4)
    assert len(builds) == 4

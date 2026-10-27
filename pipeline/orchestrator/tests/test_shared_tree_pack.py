"""Tests for shared-file lease + pack isolation helpers."""

from __future__ import annotations

from pathlib import Path

import pytest

from accel_pipeline.pack.run import parse_pack_queries
from accel_pipeline.pack.shared_file_lease import (
    SharedFileLease,
)
from accel_pipeline.pack.worker_isolation import apply_worker_isolation
from accel_pipeline.tools.shell_tools import reject_shared_build_dir_cmd, rewrite_command_build_dir
from accel_pipeline.config import PipelineConfig


def test_lease_blocks_second_holder(tmp_path: Path):
    lease_a = SharedFileLease(tmp_path)
    info = lease_a.acquire("q2", timeout_sec=1.0, reason="test")
    assert info.holder == "q2"

    lease_b = SharedFileLease(tmp_path)
    with pytest.raises(TimeoutError):
        lease_b.acquire("q5", timeout_sec=0.4, poll_sec=0.1)

    assert lease_a.release("q2")
    info2 = lease_b.acquire("q5", timeout_sec=1.0)
    assert info2.holder == "q5"
    lease_b.release("q5")


def test_worker_isolation_rewrites_build_dir(tmp_path: Path, monkeypatch):
    monkeypatch.delenv("ACCEL_PIPELINE_BUILD_DIR", raising=False)
    monkeypatch.delenv("ACCEL_PIPELINE_CODEX_HOME", raising=False)
    cfg = PipelineConfig(
        repo_root=tmp_path,
        pipeline_root=tmp_path / "pipeline",
        build_dir=tmp_path / "build-release",
        allowed_commands=[r"^cmake --build build-release( .*)?$"],
    )
    isolated = apply_worker_isolation(
        cfg, query=2, workload_name="tpch_q2_aux", set_environ=False
    )
    assert isolated.worker_id == "tpch_q2_aux"
    assert isolated.build_dir.name == "build-release-tpch_q2_aux"
    assert any("build-release-tpch_q2_aux" in p for p in isolated.allowed_commands)
    rewritten = rewrite_command_build_dir(
        "cmake --build build-release --target benchmark_tpch_q2_aux", isolated
    )
    assert "build-release-tpch_q2_aux" in rewritten


def test_pack_expands_queries_by_engine(tmp_path: Path):
    pack = {
        "engines": ["duckdb", "postgres"],
        "queries": [1, {"query": 6, "engines": ["mysql"]}],
    }
    runs = [(q.query, q.engine, q.workload) for q in parse_pack_queries(pack)]
    assert runs == [
        (1, "duckdb", "tpch_q1_aux"),
        (1, "postgres", "tpch_q1_aux"),
        (6, "mysql", "tpch_q6_aux"),
    ]


def test_ensure_build_dir_retargets_source(tmp_path: Path, monkeypatch):
    from accel_pipeline.gates import ensure_build_dir

    old_src = tmp_path / "primary"
    new_src = tmp_path / "worktree"
    old_src.mkdir()
    new_src.mkdir()
    bdir = tmp_path / "build-release-tpch_q4_aux"
    bdir.mkdir()
    (bdir / "CMakeCache.txt").write_text(
        f"CMAKE_HOME_DIRECTORY:INTERNAL={old_src}\n",
        encoding="utf-8",
    )
    (bdir / "CMakeFiles").mkdir()
    (bdir / "worker_q4_tpch_sf1.duckdb").write_bytes(b"keep-me")
    cfg = PipelineConfig(
        repo_root=new_src,
        pipeline_root=new_src / "pipeline",
        build_dir=bdir,
        worker_id="tpch_q4_aux",
    )
    seen: list[list[str]] = []

    def fake_run(cmd, **kwargs):
        seen.append(list(cmd))
        assert not (bdir / "CMakeCache.txt").exists()
        (bdir / "CMakeCache.txt").write_text(
            f"CMAKE_HOME_DIRECTORY:INTERNAL={new_src}\n",
            encoding="utf-8",
        )

        class R:
            returncode = 0
            stdout = ""
            stderr = ""

        return R()

    monkeypatch.setattr("subprocess.run", fake_run)
    ensure_build_dir(cfg, configure=True)
    assert seen and seen[0][:4] == ["cmake", "-S", str(new_src), "-B"]
    assert (bdir / "worker_q4_tpch_sf1.duckdb").read_bytes() == b"keep-me"


def test_write_ownership_blocks_cross_query_and_shared_during_opt(tmp_path: Path):
    from accel_pipeline.layout import write_allowed

    owned = tmp_path / "accelerators/tpch/q7/duckdb/aux/module.cpp"
    foreign = tmp_path / "accelerators/tpch/q8/duckdb/aux/module.cpp"
    shared = tmp_path / "eval/tests/test_support/cross_accel_helpers.cpp"
    for p in (owned, foreign, shared):
        p.parent.mkdir(parents=True, exist_ok=True)
        p.write_text("x\n", encoding="utf-8")

    ok, _ = write_allowed(
        tmp_path, owned, query=7, stage="optimization", write_paths=[]
    )
    assert ok
    ok, reason = write_allowed(
        tmp_path, foreign, query=7, stage="optimization", write_paths=["accelerators/tpch/q*/**"]
    )
    assert not ok and "cross-query" in reason
    ok, reason = write_allowed(
        tmp_path, shared, query=7, stage="optimization", write_paths=[]
    )
    assert not ok and "shared registration" in reason
    ok, _ = write_allowed(
        tmp_path,
        shared,
        query=7,
        stage="implementation",
        write_paths=["eval/tests/test_support/**"],
    )
    assert ok
    ok, reason = write_allowed(
        tmp_path, shared, query=7, stage="implementation", write_paths=[]
    )
    assert not ok


def test_shell_blocks_python_foreign_rewrite():
    from accel_pipeline.layout import shell_command_cross_query_write

    assert shell_command_cross_query_write(
        "sed -n '1,20p' accelerators/tpch/q22/duckdb/aux/module.cpp", query=4
    ) is None
    deny = shell_command_cross_query_write(
        "python3 - <<'PY'\nfrom pathlib import Path\n"
        "Path('accelerators/tpch/q22/duckdb/aux/module.cpp').write_text('x')\nPY",
        query=4,
    )
    assert deny and "Q22" in deny
    deny2 = shell_command_cross_query_write(
        "cp pipeline/artifacts/tpch/q22/best_valid_baseline_snapshot/accelerators/tpch/q22/duckdb/aux/module.hpp "
        "accelerators/tpch/q22/duckdb/aux/module.hpp",
        query=19,
    )
    assert deny2 and "Q22" in deny2


def test_rewrite_and_reject_shared_build_dir(tmp_path: Path):
    cfg = PipelineConfig(
        repo_root=tmp_path,
        pipeline_root=tmp_path / "pipeline",
        build_dir=tmp_path / "build-release-tpch_q4_aux",
        datasets={},
        gates_dir=tmp_path / "gates",
        worker_id="tpch_q4_aux",
    )
    rewritten = rewrite_command_build_dir(
        "cmake --build build-release --target benchmark_tpch_q4_aux -j4", cfg
    )
    assert "build-release-tpch_q4_aux" in rewritten
    assert reject_shared_build_dir_cmd(
        "cmake --build build-release --target x", cfg
    )
    assert (
        reject_shared_build_dir_cmd(
            "cmake --build build-release-tpch_q4_aux --target x", cfg
        )
        is None
    )

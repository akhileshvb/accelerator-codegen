"""Tests for shared-file lease + pack isolation helpers."""

from __future__ import annotations

from pathlib import Path

import pytest

from accel_pipeline.run_pack import load_pack, parse_pack_queries
from accel_pipeline.shared_file_lease import (
    SharedFileLease,
    ensure_sticky_lease_for_write,
    is_shared_path,
    release_sticky_lease,
)
from accel_pipeline.worker_isolation import (
    apply_worker_isolation,
    rewrite_command_build_dir,
    worker_id_for_query,
)
from accel_pipeline.config import PipelineConfig


def test_is_shared_path_defaults(tmp_path: Path):
    (tmp_path / "backends/duckdb/api/catalog").mkdir(parents=True)
    f = tmp_path / "backends/duckdb/api/catalog/query_catalog.cpp"
    f.write_text("x\n", encoding="utf-8")
    assert is_shared_path(tmp_path, f)
    q_aux = tmp_path / "accelerators/tpch/q2/duckdb/aux/module.cpp"
    q_aux.parent.mkdir(parents=True)
    q_aux.write_text("y\n", encoding="utf-8")
    assert not is_shared_path(tmp_path, q_aux)


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


def test_sticky_lease_auto_on_shared_write(tmp_path: Path):
    shared = tmp_path / "CMakeLists.txt"
    shared.write_text("project(x)\n", encoding="utf-8")
    info = ensure_sticky_lease_for_write(tmp_path, holder="q2", path=shared)
    assert info is not None
    assert info.holder == "q2"
    # Non-shared path does not require/refresh lease semantics beyond None.
    local = tmp_path / "accelerators/tpch/q2/duckdb/aux/module.cpp"
    local.parent.mkdir(parents=True)
    local.write_text("z\n", encoding="utf-8")
    assert ensure_sticky_lease_for_write(tmp_path, holder="q2", path=local) is None
    assert release_sticky_lease(tmp_path, holder="q2")


def test_worker_isolation_rewrites_build_dir(tmp_path: Path, monkeypatch):
    monkeypatch.delenv("ACCEL_PIPELINE_BUILD_DIR", raising=False)
    monkeypatch.delenv("ACCEL_PIPELINE_CODEX_HOME", raising=False)
    cfg = PipelineConfig(
        repo_root=tmp_path,
        pipeline_root=tmp_path / "pipeline",
        build_dir=tmp_path / "build-release",
        tpch_db_sf1=tmp_path / "a.duckdb",
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


def test_parse_pack_queries(tmp_path: Path):
    pack = {
        "workload_template": "tpch_q{q}_aux",
        "stage": "optimization",
        "queries": [
            2,
            {"query": 5, "workload": "tpch_q5_aux", "stage": "implementation"},
        ],
    }
    qs = parse_pack_queries(pack)
    assert qs[0].query == 2 and qs[0].workload == "tpch_q2_aux"
    assert qs[0].stage == "optimization"
    assert qs[1].query == 5 and qs[1].stage == "implementation"


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


def test_pack_engine_runs_get_their_own_worker(tmp_path: Path):
    from accel_pipeline.worker_isolation import worker_id_for_query

    ids = {worker_id_for_query(q.query, q.workload, q.engine) for q in parse_pack_queries(
        {"engines": ["duckdb", "postgres"], "queries": [1]}
    )}
    assert ids == {"tpch_q1_aux_duckdb", "tpch_q1_aux_postgres"}


def test_pack_without_engines_defers_to_the_workload():
    (entry,) = parse_pack_queries({"queries": [{"query": 1, "workload": "sec_edgar_q1_aux"}]})
    assert entry.engine == ""


def test_load_pack_file():
    from accel_pipeline.paths import find_repo_root

    root = find_repo_root()
    pack = load_pack("tpch_aux_parallel", root)
    assert pack.get("pack_id") == "tpch_aux_parallel"
    assert parse_pack_queries(pack)


def test_worker_id_for_query():
    assert worker_id_for_query(2, "tpch_q2_aux") == "tpch_q2_aux"
    assert worker_id_for_query(2) == "q2"


def test_rewrite_command_and_prompt_build_dir(tmp_path: Path):
    cfg = PipelineConfig(
        repo_root=tmp_path,
        pipeline_root=tmp_path / "pipeline",
        build_dir=tmp_path / "build-release-tpch_q2_aux",
        tpch_db_sf1=tmp_path / "a.duckdb",
        worker_id="tpch_q2_aux",
    )
    cmd = rewrite_command_build_dir(
        "cmake --build build-release --target benchmark_tpch_q2_aux", cfg
    )
    assert cmd == "cmake --build build-release-tpch_q2_aux --target benchmark_tpch_q2_aux"

    prompts = tmp_path / "pipeline" / "prompts"
    prompts.mkdir(parents=True)
    (prompts / "stub.md").write_text(
        "Use cmake --build build-release --target x\n", encoding="utf-8"
    )
    # Direct rewrite path used by render_prompt when worker build != build-release.
    text = (prompts / "stub.md").read_text(encoding="utf-8")
    build_name = cfg.build_dir.name
    assert text.replace("build-release", build_name).startswith(
        "Use cmake --build build-release-tpch_q2_aux"
    )


def test_gate_build_env_helper():
    from accel_pipeline.gates import run_gate
    import inspect

    src = inspect.getsource(run_gate)
    assert "ACCEL_PIPELINE_BUILD_DIR" in src
    assert "ACCEL_BUILD_DIR" in src
    assert "ensure_worker_build_dir" in src


def test_ensure_worker_build_dir_creates(tmp_path: Path, monkeypatch):
    from accel_pipeline.worker_isolation import ensure_worker_build_dir

    cfg = PipelineConfig(
        repo_root=tmp_path,
        pipeline_root=tmp_path / "pipeline",
        build_dir=tmp_path / "build-release-tpch_q7_aux",
        tpch_db_sf1=tmp_path / "a.duckdb",
        worker_id="tpch_q7_aux",
    )

    def fake_run(cmd, **kwargs):
        # pretend cmake wrote a cache
        (cfg.build_dir / "CMakeCache.txt").write_text("x\n", encoding="utf-8")

        class R:
            returncode = 0
            stdout = ""
            stderr = ""

        return R()

    monkeypatch.setattr("subprocess.run", fake_run)
    out = ensure_worker_build_dir(cfg, configure=True)
    assert out == cfg.build_dir
    assert cfg.build_dir.is_dir()
    assert (cfg.build_dir / "CMakeCache.txt").is_file()


def test_ensure_worker_build_dir_retargets_source(tmp_path: Path, monkeypatch):
    from accel_pipeline.worker_isolation import ensure_worker_build_dir

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
        tpch_db_sf1=tmp_path / "a.duckdb",
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
    ensure_worker_build_dir(cfg, configure=True)
    assert seen and seen[0][:4] == ["cmake", "-S", str(new_src), "-B"]
    assert (bdir / "worker_q4_tpch_sf1.duckdb").read_bytes() == b"keep-me"


def test_write_ownership_blocks_cross_query_and_shared_during_opt(tmp_path: Path):
    from accel_pipeline.write_ownership import write_allowed

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
    from accel_pipeline.write_ownership import shell_command_cross_query_write

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
    from accel_pipeline.worker_isolation import (
        reject_shared_build_dir_cmd,
        rewrite_command_build_dir,
    )

    cfg = PipelineConfig(
        repo_root=tmp_path,
        pipeline_root=tmp_path / "pipeline",
        build_dir=tmp_path / "build-release-tpch_q4_aux",
        tpch_db_sf1=tmp_path / "a.duckdb",
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


def test_repo_context_pack_roundtrip(tmp_path: Path):
    from accel_pipeline.repo_context_pack import (
        ensure_repo_context_pack,
        format_seed_pointer,
        pack_path,
    )

    (tmp_path / "accelerators/tpch/q7/duckdb/aux").mkdir(parents=True)
    (tmp_path / "accelerators/tpch/q7/duckdb/aux/module.cpp").write_text("x\n", encoding="utf-8")
    out = ensure_repo_context_pack(tmp_path, force=True)
    assert out == pack_path(tmp_path)
    assert "Registration surfaces" in out.read_text(encoding="utf-8")
    ptr = format_seed_pointer(tmp_path)
    assert "repo_context_pack.md" in ptr


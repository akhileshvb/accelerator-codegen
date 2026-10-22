"""One run per query x engine: sibling-engine hiding and write ownership."""

from __future__ import annotations

from pathlib import Path

from accel_pipeline.config import apply_workload_overrides, load_pipeline_config, load_workload
from accel_pipeline.engine_quarantine import (
    apply_foreign_engine_quarantine,
    foreign_trees_for_workload,
    restore_quarantine,
)
from accel_pipeline.write_ownership import owned_path_globs, write_allowed

REPO_ROOT = Path(__file__).resolve().parents[3]


def test_a_run_hides_the_querys_other_engines():
    w = load_workload("tpch_q3_aux", REPO_ROOT, engine="postgres")
    assert foreign_trees_for_workload(w) == [
        "accelerators/tpch/q3/duckdb",
        "pipeline/artifacts/tpch/q3/duckdb",
        "accelerators/tpch/q3/mysql",
        "pipeline/artifacts/tpch/q3/mysql",
    ]


def test_quarantine_moves_and_restores_sibling_trees(tmp_path: Path):
    workload = {
        "benchmark_name": "tpch",
        "query_number": 3,
        "engine_name": "postgres",
        "engines": ["duckdb", "postgres"],
    }
    mine = tmp_path / "accelerators/tpch/q3/postgres/aux/module.cpp"
    theirs = tmp_path / "accelerators/tpch/q3/duckdb/aux/module.cpp"
    for path in (mine, theirs):
        path.parent.mkdir(parents=True)
        path.write_text("//", encoding="utf-8")
    session = apply_foreign_engine_quarantine(tmp_path, workload)
    assert mine.is_file() and not theirs.exists()
    restore_quarantine(session)
    assert mine.is_file() and theirs.is_file()


def test_run_owns_only_its_engines_artifacts():
    assert owned_path_globs(3, benchmark="tpch", engine="postgres") == [
        "accelerators/tpch/q3/**",
        "pipeline/artifacts/tpch/q3/postgres/**",
    ]


def test_sibling_engine_artifacts_are_not_writable():
    run = apply_workload_overrides(
        load_pipeline_config(repo_root=REPO_ROOT),
        load_workload("tpch_q3_aux", REPO_ROOT, engine="postgres"),
    )

    def allowed(rel: str) -> bool:
        return write_allowed(REPO_ROOT, rel, query=3, stage="analysis", pipeline_config=run)[0]

    assert allowed("pipeline/artifacts/tpch/q3/postgres/analysis.json")
    assert not allowed("pipeline/artifacts/tpch/q3/duckdb/analysis.json")


def test_gate_scripts_read_the_orchestrators_context(tmp_path: Path, monkeypatch):
    import json
    import subprocess

    ctx = tmp_path / "ctx.json"
    ctx.write_text(json.dumps({"engine_name": "mysql", "artifacts_dir": str(tmp_path)}), encoding="utf-8")
    script = REPO_ROOT / "pipeline" / "gates" / "read_gate_context.py"
    out = subprocess.run(
        ["python3", str(script), str(REPO_ROOT), "3"],
        env={"ACCEL_GATE_CONTEXT": str(ctx), "PATH": "/usr/bin:/bin"},
        capture_output=True, text=True, check=True,
    )
    assert json.loads(out.stdout)["engine_name"] == "mysql"
    bare = subprocess.run(["python3", str(script), str(REPO_ROOT), "3"], env={"PATH": "/usr/bin:/bin"},
                          capture_output=True, text=True)
    assert bare.returncode != 0 and "ACCEL_GATE_CONTEXT" in bare.stderr

"""One run per query x engine: write ownership."""

from __future__ import annotations

from pathlib import Path

from accel_pipeline.config import apply_workload_overrides, load_workload, load_pipeline_config
from accel_pipeline.layout import owned_path_globs, write_allowed

REPO_ROOT = Path(__file__).resolve().parents[3]


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

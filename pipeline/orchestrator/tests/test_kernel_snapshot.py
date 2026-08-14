from pathlib import Path

from accel_pipeline.config import PipelineConfig
from accel_pipeline.kernel_snapshot import (
    has_best_kernel_snapshot,
    resolve_snapshot_paths,
    restore_best_kernel,
    snapshot_best_kernel,
)


def test_kernel_snapshot_roundtrip(tmp_path: Path):
    repo = tmp_path / "repo"
    repo.mkdir()
    (repo / "pipeline" / "config").mkdir(parents=True)
    (repo / "pipeline" / "config" / "default.yaml").write_text(
        "repo_root: .\nbuild_dir: build-release\ngates_dir: pipeline/gates\n",
        encoding="utf-8",
    )

    header = repo / "src/generated/sec_edgar/q1/base/query.hpp"
    kernel = repo / "src/generated/sec_edgar/q1/base/single-threaded/kernel.cpp"
    package = repo / "src/generated/sec_edgar/q1/base/single-threaded/package.cpp"
    fused = repo / "src/generated/sec_edgar/q1/base/single-threaded/fused_scan.cpp"
    runtime_hpp = repo / "src/generated/sec_edgar/runtime.hpp"
    runtime_cpp = repo / "src/generated/sec_edgar/runtime.cpp"
    for path in (header, kernel, package, fused, runtime_hpp, runtime_cpp):
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(f"content:{path.name}:v1\n", encoding="utf-8")

    workload = {
        "benchmark_name": "sec_edgar",
        "query_number": 1,
        "header_basename": "sec_edgar_q1",
        "function_name": "accel_sec_edgar_q1",
        "test_target": "test_sec_edgar_q1",
        "bench_target": "benchmark_sec_edgar_q1",
        "artifact_namespace": "sec_edgar",
        "snapshot_paths": [
            "src/generated/sec_edgar/q{N}/base/query.hpp",
            "src/generated/sec_edgar/q{N}/base/fused.hpp",
            "src/generated/sec_edgar/q{N}/base/single-threaded/kernel.cpp",
            "src/generated/sec_edgar/q{N}/base/single-threaded/package.cpp",
            "src/generated/sec_edgar/q{N}/base/single-threaded/fused_scan.cpp",
            "src/generated/sec_edgar/runtime.hpp",
            "src/generated/sec_edgar/runtime.cpp",
        ],
    }
    art = repo / "pipeline" / "artifacts" / "sec_edgar" / "q1"
    art.mkdir(parents=True)

    pipeline_config = PipelineConfig(
        repo_root=repo,
        pipeline_root=repo / "pipeline",
        build_dir=repo / "build-release",
        tpch_db_sf1=repo / "tpch.duckdb",
        tpch_db_sf10=repo / "tpch10.duckdb",
        gates_dir=repo / "pipeline" / "gates",
    )

    snap_paths = resolve_snapshot_paths(pipeline_config, workload, 1)
    assert fused in snap_paths
    assert runtime_hpp in snap_paths

    snapshot_best_kernel(
        pipeline_config,
        workload,
        1,
        speedup_median=1.09,
        iteration=0,
        change_kind="implementation",
    )
    assert has_best_kernel_snapshot(art)
    manifest = (art / "best_kernel_snapshot" / "manifest.json").read_text(encoding="utf-8")
    assert "fused_scan.cpp" in manifest
    assert "runtime.hpp" in manifest

    kernel.write_text("content:kernel.cpp:v2-broken\n", encoding="utf-8")
    manifest = restore_best_kernel(pipeline_config, workload, 1)
    assert manifest is not None
    assert "v1" in kernel.read_text(encoding="utf-8")

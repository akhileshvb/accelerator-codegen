from __future__ import annotations

import json
import shutil
import subprocess
from pathlib import Path
from typing import Any

from accel_pipeline.gates.run import build_jobs
from accel_pipeline.config import PipelineConfig, resolve_optimization_edit_paths, resolve_workload_paths
from accel_pipeline.paths import query_artifacts_dir
from accel_pipeline.playbook import append_experiment_log

SNAPSHOT_DIRNAME = "best_kernel_snapshot"
MANIFEST_NAME = "manifest.json"


def _expand_snapshot_template(repo_root: Path, template: str, query: int) -> Path:
    rel = template.replace("{N}", str(query))
    return repo_root / rel


def resolve_snapshot_paths(
    pipeline_config: PipelineConfig,
    workload: dict[str, Any],
    query: int,
) -> list[Path]:
    """Return repo paths to snapshot; optional files are included only when present."""
    repo_root = pipeline_config.repo_root
    configured = list(workload.get("snapshot_paths", []))
    if configured:
        paths: list[Path] = []
        for template in configured:
            path = _expand_snapshot_template(repo_root, str(template), query)
            if path.is_file():
                paths.append(path)
        return paths

    paths = resolve_workload_paths(pipeline_config, workload, query)
    if workload.get("workload_kind") == "aux_state":
        edit = resolve_optimization_edit_paths(pipeline_config, workload, query)
        rels = [p for p in edit["all_paths"] if (repo_root / p).is_file()]
        return [repo_root / rel for rel in rels]

    rels = [paths["header_path"], paths["kernel_path"], paths["package_path"]]
    return [repo_root / rel for rel in rels]


def snapshot_dir(art: Path) -> Path:
    return art / SNAPSHOT_DIRNAME


def has_best_kernel_snapshot(art: Path) -> bool:
    manifest = snapshot_dir(art) / MANIFEST_NAME
    return manifest.is_file()


def snapshot_best_kernel(
    pipeline_config: PipelineConfig,
    workload: dict[str, Any],
    query: int,
    *,
    speedup_median: float,
    iteration: int,
    change_kind: str,
) -> Path:
    repo_root = pipeline_config.repo_root
    art = query_artifacts_dir(repo_root, query, workload=workload)
    dest = snapshot_dir(art)
    if dest.exists():
        shutil.rmtree(dest)
    dest.mkdir(parents=True)

    files: list[dict[str, str]] = []
    for src in resolve_snapshot_paths(pipeline_config, workload, query):
        if not src.is_file():
            continue
        rel = src.relative_to(repo_root)
        target = dest / rel
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(src, target)
        files.append({"repo_rel": str(rel), "snapshot_rel": str(rel)})

    manifest = {
        "speedup_median": speedup_median,
        "iteration": iteration,
        "change_kind": change_kind,
        "files": files,
    }
    (dest / MANIFEST_NAME).write_text(json.dumps(manifest, indent=2), encoding="utf-8")
    return dest


def restore_best_kernel(
    pipeline_config: PipelineConfig,
    workload: dict[str, Any],
    query: int,
) -> dict[str, Any] | None:
    repo_root = pipeline_config.repo_root
    art = query_artifacts_dir(repo_root, query, workload=workload)
    dest = snapshot_dir(art)
    manifest_path = dest / MANIFEST_NAME
    if not manifest_path.is_file():
        return None

    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    restored: list[str] = []
    for entry in manifest.get("files", []):
        repo_rel = entry.get("repo_rel")
        if not repo_rel:
            continue
        src = dest / repo_rel
        if not src.is_file():
            continue
        target = repo_root / repo_rel
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(src, target)
        restored.append(repo_rel)

    if not restored:
        return None

    manifest["restored_files"] = restored
    return manifest


def rebuild_workload_targets(pipeline_config: PipelineConfig, workload: dict[str, Any], query: int) -> bool:
    paths = resolve_workload_paths(pipeline_config, workload, query)
    if workload.get("workload_kind") == "aux_state":
        targets = [
            paths.get("test_target", ""),
            paths.get("aux_bench_target", ""),
            "accelerator_codegen",
        ]
    else:
        targets = [paths["test_target"], paths["bench_target"], "accelerator_codegen"]
    targets = [t for t in targets if t]
    cmd = ["cmake", "--build", str(pipeline_config.build_dir), "--target", *targets, "-j", str(build_jobs())]
    result = subprocess.run(
        cmd,
        cwd=pipeline_config.repo_root,
        capture_output=True,
        text=True,
    )
    return result.returncode == 0


def maybe_restore_best_kernel(
    pipeline_config: PipelineConfig,
    workload: dict[str, Any],
    query: int,
    *,
    best_speedup_median: float,
    last_speedup_median: float,
    reason: str,
) -> bool:
    """Restore the on-disk kernel to the best-known snapshot when regressed."""
    if best_speedup_median <= 0 or last_speedup_median + 0.005 >= best_speedup_median:
        return False

    art = query_artifacts_dir(pipeline_config.repo_root, query, workload=workload)
    if not has_best_kernel_snapshot(art):
        append_experiment_log(
            pipeline_config.repo_root,
            query,
            f"- {reason}: regressed ({last_speedup_median:.3f}x vs best {best_speedup_median:.3f}x) "
            "but no kernel snapshot exists — re-run implementation to seed best baseline",
            workload=workload,
        )
        return False

    manifest = restore_best_kernel(pipeline_config, workload, query)
    if manifest is None:
        return False

    rebuild_workload_targets(pipeline_config, workload, query)
    append_experiment_log(
        pipeline_config.repo_root,
        query,
        f"- {reason}: auto-restored best kernel snapshot "
        f"({manifest.get('speedup_median', best_speedup_median):.3f}x from "
        f"{manifest.get('change_kind', 'unknown')} iter {manifest.get('iteration', '?')})",
        workload=workload,
    )
    return True

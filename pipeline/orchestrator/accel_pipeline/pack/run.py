"""Fan-out runner: shared tree + separate builds + shared-file lease (no stage barriers)."""

from __future__ import annotations

import argparse
import json
import os
import subprocess
import sys
import time
from concurrent.futures import ThreadPoolExecutor, as_completed
from dataclasses import dataclass
from pathlib import Path
from typing import Any

import yaml

from accel_pipeline.engine_host import instance_tag
from accel_pipeline.env_loader import load_repo_dotenv
from accel_pipeline.paths import find_repo_root, pipeline_root
from accel_pipeline.pack.worker_isolation import (
    apply_worker_isolation,
    default_build_dir_for_worker,
    prepare_codex_home,
    worker_id_for_query,
)


@dataclass(frozen=True)
class PackQuery:
    workload: str
    query: int
    engine: str = ""  # "" when the workload supports a single engine
    stage: str = "all"
    resume: bool = True
    mode: str = ""  # "" (workload default / cpp) or "verus"


def load_pack(name_or_path: str, repo_root: Path | None = None) -> dict[str, Any]:
    root = repo_root or find_repo_root()
    path = Path(name_or_path)
    if not path.is_absolute():
        candidates = [
            path,
            root / path,
            pipeline_root(root) / "config" / "packs" / path,
            pipeline_root(root) / "config" / "packs" / f"{path.stem}.yaml",
            pipeline_root(root) / "config" / "packs" / f"{name_or_path}.yaml",
        ]
        for cand in candidates:
            if cand.is_file():
                path = cand
                break
        else:
            raise FileNotFoundError(f"pack not found: {name_or_path}")
    with path.open(encoding="utf-8") as f:
        data = yaml.safe_load(f) or {}
    data["_pack_path"] = str(path)
    return data


def parse_pack_queries(pack: dict[str, Any]) -> list[PackQuery]:
    """One entry per query x engine; an entry's `engines:` overrides the pack's."""
    items = pack.get("queries") or []
    out: list[PackQuery] = []
    default_stage = str(pack.get("stage") or "all")
    default_resume = bool(pack.get("resume", True))
    default_mode = str(pack.get("mode") or "")
    default_engines = list(pack.get("engines") or [])
    templates = pack.get("workload_template", "tpch_q{q}_aux")
    # A list runs each query's accelerators side by side (e.g. its base and its aux workload).
    default_templates = [str(t) for t in templates] if isinstance(templates, list) else [str(templates)]
    for item in items:
        if isinstance(item, (int, str)) and str(item).isdigit():
            item = {"query": int(item)}
        if not isinstance(item, dict):
            raise ValueError(f"invalid pack query entry: {item!r}")
        q = int(item.get("query") or item.get("query_number"))
        workloads = [str(item["workload"])] if item.get("workload") else default_templates
        for engine in item.get("engines") or default_engines or [""]:
            for template in workloads:
                out.append(
                    PackQuery(
                        workload=template.format(q=q),
                        query=q,
                        engine=str(engine),
                        stage=str(item.get("stage") or default_stage),
                        resume=bool(item.get("resume", default_resume)),
                        mode=str(item.get("mode") or default_mode),
                    )
                )
    return out


def _worker_log_path(repo_root: Path, worker_id: str) -> Path:
    path = pipeline_root(repo_root) / "artifacts" / "packs" / "logs"
    path.mkdir(parents=True, exist_ok=True)
    return path / f"{worker_id}.log"


def run_pack_query(
    *,
    repo_root: Path,
    entry: PackQuery,
    models_config: str | None,
    pipeline_config_path: str | None,
    dry_run: bool,
    skip_gates: bool,
    extra_args: list[str] | None = None,
    isolation_mode: str = "worktree",
    pack_queries: list[int] | None = None,
    concurrent_workers: int = 1,
) -> dict[str, Any]:
    from accel_pipeline.pack.worker_worktree import ensure_worker_worktree

    worker_id = worker_id_for_query(entry.query, entry.workload, entry.engine)
    primary = Path(repo_root).resolve()
    # Build + Codex home always on primary (shared caches / sidecars).
    build_dir = default_build_dir_for_worker(primary, worker_id)
    codex_home = prepare_codex_home(repo_root=primary, worker_id=worker_id)

    worker_repo = primary
    if isolation_mode in {"worktree", "worktrees", "git_worktree"}:
        worker_repo = ensure_worker_worktree(
            primary,
            worker_id,
            reset=True,
            pack_queries=pack_queries,
        )

    log_path = _worker_log_path(primary, worker_id)

    env = os.environ.copy()
    env["ACCEL_PIPELINE_WORKER_ID"] = worker_id
    env["ACCEL_PIPELINE_BUILD_DIR"] = str(build_dir)
    env["ACCEL_PIPELINE_CODEX_HOME"] = str(codex_home)
    env["ACCEL_PIPELINE_QUERY"] = str(entry.query)
    env["ACCEL_PIPELINE_WORKLOAD"] = entry.workload
    env["ACCEL_PIPELINE_REPO_ROOT"] = str(worker_repo)
    env["ACCEL_PIPELINE_PRIMARY_REPO"] = str(primary)
    env["ACCEL_PIPELINE_ISOLATION_MODE"] = isolation_mode
    env["CODEX_HOME"] = str(codex_home)
    # One engine instance per worker (container names and ports derive from it).
    tag = instance_tag(worker_id, build_dir)
    env["ACCEL_INSTANCE_TAG"] = tag
    env["ACCEL_ENGINE_INSTANCE"] = f"{tag}-{entry.engine or 'engine'}"
    env["ACCEL_CONCURRENT_WORKERS"] = str(concurrent_workers)  # the CPU partition (core_budget)
    if pack_queries:
        env["ACCEL_PIPELINE_PACK_QUERIES"] = ",".join(str(q) for q in pack_queries)

    cmd = [
        sys.executable,
        "-m",
        "accel_pipeline.run_pipeline",
        "--query",
        str(entry.query),
        "--workload",
        entry.workload,
        "--stage",
        entry.stage,
    ]
    if entry.engine:
        cmd.extend(["--engine", entry.engine])
    if entry.resume:
        cmd.append("--resume")
    if entry.mode:
        cmd.extend(["--mode", entry.mode])
    if dry_run:
        cmd.append("--dry-run")
    if skip_gates:
        cmd.append("--skip-gates")
    if pipeline_config_path:
        cmd.extend(["--config", pipeline_config_path])
    if models_config:
        cmd.extend(["--models-config", models_config])
    if extra_args:
        cmd.extend(extra_args)

    started = time.time()
    print(
        f"[pack] start worker={worker_id} query={entry.query} workload={entry.workload} "
        f"engine={entry.engine or '-'} "
        f"build_dir={build_dir.name} repo={worker_repo.name if worker_repo != primary else 'primary'} "
        f"isolation={isolation_mode} stage={entry.stage}",
        file=sys.stderr,
        flush=True,
    )
    with log_path.open("w", encoding="utf-8") as logf:
        logf.write(f"# cmd: {' '.join(cmd)}\n")
        logf.write(
            f"# build_dir={build_dir}\n# codex_home={codex_home}\n"
            f"# repo_root={worker_repo}\n# primary={primary}\n"
            f"# isolation={isolation_mode}\n\n"
        )
        logf.flush()
        proc = subprocess.run(
            cmd,
            cwd=primary / "pipeline" / "orchestrator",
            env=env,
            stdout=logf,
            stderr=subprocess.STDOUT,
            check=False,
        )
    duration = time.time() - started
    result = {
        "worker_id": worker_id,
        "query": entry.query,
        "workload": entry.workload,
        "engine": entry.engine,
        "stage": entry.stage,
        "exit_code": proc.returncode,
        "duration_sec": round(duration, 2),
        "build_dir": str(build_dir),
        "codex_home": str(codex_home),
        "repo_root": str(worker_repo),
        "isolation_mode": isolation_mode,
        "log": str(log_path),
        "ok": proc.returncode == 0,
    }
    status = "ok" if result["ok"] else "FAIL"
    print(
        f"[pack] {status} worker={worker_id} exit={proc.returncode} "
        f"duration={duration:.1f}s log={log_path}",
        file=sys.stderr,
        flush=True,
    )
    return result


def _chains_by_artifact_dir(entries: list[PackQuery]) -> list[list[PackQuery]]:
    """A query's workloads on one engine (its base kernel and its aux accelerator) share an
    artifact directory, so they run one after another; different query x engine pairs run in
    parallel."""
    chains: dict[tuple[int, str], list[PackQuery]] = {}
    for entry in entries:
        chains.setdefault((entry.query, entry.engine), []).append(entry)
    return list(chains.values())


def run_pack(
    pack: dict[str, Any],
    *,
    repo_root: Path,
    concurrency: int | None = None,
    models_config: str | None = None,
    pipeline_config_path: str | None = None,
    dry_run: bool = False,
    skip_gates: bool = False,
    extra_args: list[str] | None = None,
) -> dict[str, Any]:
    """`first_wave: N` in the pack runs its first N runs to completion before fanning out the
    rest, so the primitives they publish exist before the mass of runs starts."""
    queries = parse_pack_queries(pack)
    if not queries:
        raise ValueError("pack has no queries")
    max_workers = int(concurrency or pack.get("concurrency") or 2)
    max_workers = max(1, min(max_workers, len(queries)))

    # Ensure isolation fields are sensible even for dry summary.
    _ = apply_worker_isolation  # imported for API stability / tests

    from accel_pipeline.pack.repo_context_pack import ensure_repo_context_pack

    pack_md = ensure_repo_context_pack(repo_root, force=True)
    print(
        f"[pack] shared repo context pack ready: {pack_md}",
        flush=True,
    )

    # One-shot host bootstrap per engine BEFORE parallel query workers.
    from accel_pipeline.config import load_model_config, load_pipeline_config
    from accel_pipeline.engine_backend.host_bootstrap import ensure_host_bootstrap_for_pack

    pipeline_cfg_path = pipeline_config_path or pack.get("pipeline_config")
    models_cfg_path = models_config or pack.get("models_config")
    pipeline_config = load_pipeline_config(
        Path(pipeline_cfg_path) if pipeline_cfg_path else None,
        repo_root=repo_root,
    )
    model_config = load_model_config(
        Path(models_cfg_path) if models_cfg_path else None,
        repo_root=repo_root,
    )
    # Backend defects earlier query runs recorded go back to the engine's backend run first.
    from accel_pipeline.engine_backend.backend_requests import reopen_engine_backend

    for engine in sorted({str(q.engine) for q in queries if q.engine}):
        reopen_engine_backend(pipeline_config, model_config, engine, dry_run=dry_run)
    hb_results = ensure_host_bootstrap_for_pack(
        pack,
        repo_root=repo_root,
        pipeline_config=pipeline_config,
        model_config=model_config,
        queries=queries,
        dry_run=dry_run,
        skip_gates=skip_gates,
    )
    if hb_results:
        print(f"[pack] host_bootstrap: {hb_results}", flush=True)
        failed = [r for r in hb_results if r.get("status") != "pass"]
        if failed and not dry_run:
            raise RuntimeError(f"host_bootstrap failed before pack fan-out: {failed}")

    results: list[dict[str, Any]] = []
    isolation = dict(pack.get("isolation") or {})
    isolation_mode = str(
        isolation.get("mode")
        or pack.get("isolation_mode")
        or "worktree"
    )
    # Legacy alias: shared_tree_separate_builds → upgrade to worktree isolation.
    if isolation_mode in {"shared_tree_separate_builds", "shared_tree"}:
        isolation_mode = "worktree"
    pack_query_ids = sorted({e.query for e in queries})
    print(
        f"[pack] isolation_mode={isolation_mode} workers={len(queries)} "
        f"queries={pack_query_ids}",
        flush=True,
    )

    first_wave = int(pack.get("first_wave") or 0)
    waves = [queries[:first_wave], queries[first_wave:]] if 0 < first_wave < len(queries) else [queries]
    def run_chain(chain: list[PackQuery]) -> list[dict[str, Any]]:
        return [
            run_pack_query(
                repo_root=repo_root,
                entry=entry,
                models_config=models_config or pack.get("models_config"),
                pipeline_config_path=pipeline_config_path or pack.get("pipeline_config"),
                dry_run=dry_run,
                skip_gates=skip_gates,
                extra_args=extra_args,
                isolation_mode=isolation_mode,
                pack_queries=pack_query_ids,
                concurrent_workers=max_workers,
            )
            for entry in chain
        ]

    for wave in waves:
        chains = _chains_by_artifact_dir(wave)
        with ThreadPoolExecutor(max_workers=max(1, min(max_workers, len(chains)))) as pool:
            futures = [pool.submit(run_chain, chain) for chain in chains]
            for fut in as_completed(futures):
                results.extend(fut.result())

    results.sort(key=lambda r: (r["query"], r["workload"], r["engine"]))
    summary = {
        "pack_id": pack.get("pack_id") or pack.get("name") or "pack",
        "pack_path": pack.get("_pack_path"),
        "concurrency": max_workers,
        "isolation": isolation_mode,
        "shared_file_lease": True,
        "stage_barriers": False,
        "results": results,
        "ok_count": sum(1 for r in results if r["ok"]),
        "fail_count": sum(1 for r in results if not r["ok"]),
    }
    out_dir = pipeline_root(repo_root) / "artifacts" / "packs"
    out_dir.mkdir(parents=True, exist_ok=True)
    out_path = out_dir / f"{summary['pack_id']}_last_run.json"
    out_path.write_text(json.dumps(summary, indent=2) + "\n", encoding="utf-8")
    summary["summary_path"] = str(out_path)
    return summary


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        description=(
            "Run many query pipelines concurrently on a shared tree with separate "
            "build dirs and a shared-file lease (no stage barriers)."
        )
    )
    parser.add_argument("--pack", required=True, help="Pack name or YAML path under pipeline/config/packs/")
    parser.add_argument("--concurrency", type=int, default=None)
    parser.add_argument("--models-config", default=None)
    parser.add_argument("--config", default=None, help="Pipeline config YAML")
    parser.add_argument("--dry-run", action="store_true")
    parser.add_argument("--skip-gates", action="store_true")
    parser.add_argument("--mode", choices=["cpp", "verus"], default=None,
                        help="Pipeline mode for every query of the pack (overrides the pack's `mode`)")
    args = parser.parse_args(argv)

    repo_root = find_repo_root()
    load_repo_dotenv(repo_root)
    pack = load_pack(args.pack, repo_root)
    if args.mode:
        pack["mode"] = args.mode
        for item in pack.get("queries") or []:
            if isinstance(item, dict):
                item["mode"] = args.mode
    summary = run_pack(
        pack,
        repo_root=repo_root,
        concurrency=args.concurrency,
        models_config=args.models_config,
        pipeline_config_path=args.config,
        dry_run=args.dry_run,
        skip_gates=args.skip_gates,
    )
    print(json.dumps(summary, indent=2))
    return 0 if summary["fail_count"] == 0 else 1


if __name__ == "__main__":
    raise SystemExit(main())

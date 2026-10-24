"""Run context of the Verus accelerator pipeline (``--mode verus``).

One query's run has three places:

* the **build tree** (``pipeline_config.repo_root``, a pack worker's worktree
  or the primary checkout): sql2verus writes the frozen generated files there,
  everything the run produces lives in the query's own directory
  (``accelerators/<bench>/q<N>/verus_kit/<crate>/``: host glue, C header, and
  the kernel crate in ``crate/``), and every gate runs there;
* the **workspace** (``pipeline/verus_runs/<workload>/workspace``): the only
  directory agents work in.  It holds the query, the frozen spec files (copies),
  the agent-owned files (inputs, analysis, tests, kernel), and read-only
  references (the verified libraries, the proof library, the playbook, the
  contracts).  Nothing from the C++ accelerators or other Verus accelerators
  is copied in;
* the **artifacts** (``pipeline/artifacts/<bench>/q<N>/verus``): state,
  rendered prompts, gate results, reports.

``VerusContext`` is written to ``context.json`` so the MCP tools (run by Codex
in another process) see the same settings.
"""

from __future__ import annotations

import json
import os
from dataclasses import asdict, dataclass, field
from pathlib import Path
from typing import Any

import yaml

from accel_pipeline.layout import query_dir, workload_query_sql, workload_schema_sql
from accel_pipeline.paths import query_artifacts_dir


@dataclass
class VerusContext:
    query: int
    workload_name: str
    benchmark: str
    repo_root: str  # build tree
    pipeline_root: str
    build_dir: str
    lib_dir: str
    crate: str
    accel_id: str
    ffi_prefix: str
    query_sql: str
    schema_sql: str
    test_db: str
    bench_db: str
    run_dir: str
    workspace: str
    artifacts: str
    settings: dict[str, Any] = field(default_factory=dict)

    # ---- paths ----
    @property
    def root(self) -> Path:
        return Path(self.repo_root)

    @property
    def ws(self) -> Path:
        return Path(self.workspace)

    @property
    def art(self) -> Path:
        return Path(self.artifacts)

    @property
    def crate_dir(self) -> Path:
        return self.kit_dir / "crate"

    @property
    def kit_dir(self) -> Path:
        return self.root / query_dir(self.benchmark, self.query) / "verus_kit" / self.crate

    @property
    def header_name(self) -> str:
        return f"{self.crate}_verus.h"

    @property
    def runner(self) -> Path:
        return Path(self.build_dir) / "verus_kit_runner"

    def setting(self, key: str, default: Any = None) -> Any:
        cur: Any = self.settings
        for part in key.split("."):
            if not isinstance(cur, dict) or part not in cur:
                return default
            cur = cur[part]
        return cur

    def save(self) -> Path:
        self.art.mkdir(parents=True, exist_ok=True)
        path = self.art / "context.json"
        path.write_text(json.dumps(asdict(self), indent=2), encoding="utf-8")
        return path

    @classmethod
    def load(cls, path: Path) -> "VerusContext":
        return cls(**json.loads(Path(path).read_text(encoding="utf-8")))


def _deep_merge(base: dict[str, Any], over: dict[str, Any]) -> dict[str, Any]:
    out = dict(base)
    for k, v in (over or {}).items():
        out[k] = _deep_merge(out[k], v) if isinstance(v, dict) and isinstance(out.get(k), dict) else v
    return out


def load_verus_settings(pipeline_root: Path, workload: dict[str, Any]) -> dict[str, Any]:
    """config/verus.yaml defaults, overridden by the workload's ``verus:`` section."""
    path = pipeline_root / "config" / "verus.yaml"
    base = yaml.safe_load(path.read_text(encoding="utf-8")) if path.is_file() else {}
    return _deep_merge(base or {}, workload.get("verus") or {})


def build_context(pipeline_config: Any, workload: dict[str, Any], workload_name: str, query: int) -> VerusContext:
    root = Path(pipeline_config.repo_root)
    primary = Path(os.environ.get("ACCEL_PIPELINE_PRIMARY_REPO") or root)
    settings = load_verus_settings(Path(pipeline_config.pipeline_root), workload)
    bench = str(workload.get("benchmark_name") or "tpch")
    crate = str(settings.get("crate") or f"{bench}_q{query}_vgen")
    accel_id = str(settings.get("accelerator_id") or crate)
    prefix = str(settings.get("ffi_prefix") or crate.replace("_", ""))
    # The query SQL and schema are workload inputs: workloads/<bench>/q<N>.sql, workloads/<bench>/schema.sql.
    sql_rel = settings.get("query_sql") or workload_query_sql(bench, query)
    schema_rel = settings.get("schema_sql") or workload_schema_sql(bench)
    datasets = dict(getattr(pipeline_config, "datasets", {}) or {})
    test_db = settings.get("test_db") or str(datasets.get(f"{bench}_test") or "")
    bench_db = settings.get("bench_db") or str(datasets.get(f"{bench}_sf1") or "")
    build_dir = settings.get("build_dir") or str(pipeline_config.build_dir)
    lib_dir = settings.get("lib_dir") or str(root / "build-verus")
    # the Verus binary (kept in settings: the MCP server loads the context)
    settings["verus_bin"] = str(settings.get("verus_bin") or os.environ.get("VERUS_BIN")
                                or root / "tools" / "verus" / "verus-x86-linux" / "verus")
    run_dir = Path(pipeline_config.pipeline_root) / "verus_runs" / workload_name
    art = query_artifacts_dir(root, query, workload=workload) / f"verus_{workload_name}"

    def resolve(p: str) -> str:
        path = Path(p)
        if path.is_absolute():
            return str(path)
        return str((primary / path) if (primary / path).exists() else (root / path))

    return VerusContext(
        query=query,
        workload_name=workload_name,
        benchmark=bench,
        repo_root=str(root),
        pipeline_root=str(pipeline_config.pipeline_root),
        build_dir=str(Path(resolve(build_dir))),
        lib_dir=str(Path(resolve(lib_dir))),
        crate=crate,
        accel_id=accel_id,
        ffi_prefix=prefix,
        query_sql=resolve(str(sql_rel)),
        schema_sql=resolve(str(schema_rel)),
        test_db=test_db,
        bench_db=bench_db,
        run_dir=str(run_dir),
        workspace=str(run_dir / "workspace"),
        artifacts=str(art),
        settings=settings,
    )


# C++-pipeline workload fields the Verus mode never uses (they would give
# agents context from C++ accelerators).
IGNORED_WORKLOAD_FIELDS = (
    "optimization_guidance",
    "requires_base_accelerator",
    "prerequisite_workload",
    "cost_model_config",
    "agent_boundary",
)

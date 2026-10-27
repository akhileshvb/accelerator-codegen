"""Materialize starter source files from templates before implementation."""

from __future__ import annotations

import re
from pathlib import Path
from typing import Any

from accel_pipeline.config import PipelineConfig, load_workload, resolve_workload_paths
from accel_pipeline.workload_facts import with_facts


def _substitute(template: str, values: dict[str, str]) -> str:
    out = template
    for key, value in values.items():
        out = out.replace("{{" + key + "}}", value)
    return out


def _type_prefix(create_fn: str) -> str:
    name = create_fn
    if name.startswith("Create"):
        name = name[6:]
    if name.endswith("FunctionSet"):
        name = name[: -len("FunctionSet")]
    return name


def read_api_manifest(repo_root: Path, engine: str) -> dict[str, dict[str, list[str]]]:
    """Role -> {headers, symbols} from the first table under `## Contract roles` of the engine's api/README.md."""
    readme = repo_root / "backends" / engine / "api" / "README.md"
    if not readme.is_file():
        return {}
    roles: dict[str, dict[str, list[str]]] = {}
    in_section = False
    for line in readme.read_text(encoding="utf-8").splitlines():
        if line.startswith("## "):
            in_section = line.strip() == "## Contract roles"
            continue
        cells = [c.strip() for c in line.strip().strip("|").split("|")]
        if not in_section or not line.startswith("|") or len(cells) != 3 or set(cells[0]) <= set("-: "):
            continue
        if cells[0] != "role":
            roles[cells[0]] = {"headers": _backticked(cells[1]), "symbols": _backticked(cells[2])}
    return roles


def _backticked(cell: str) -> list[str]:
    return re.findall(r"`([^`]+)`", cell)


def _api_stub_values(manifest: dict[str, dict[str, list[str]]]) -> dict[str, str]:
    headers = dict.fromkeys(h for role in manifest.values() for h in role["headers"])
    return {
        "api_includes": "".join(f'#include "{h}"\n' for h in headers),
        "api_manifest": "\n".join(
            f"//   {role}: {', '.join(spec['symbols'])} ({', '.join(spec['headers'])})"
            for role, spec in manifest.items()
        )
        or "//   (backends/<engine>/api/README.md lists none)",
    }


def _build_stub_values(
    workload: dict[str, Any],
    paths: dict[str, str],
    query: int,
    manifest: dict[str, dict[str, list[str]]] | None = None,
) -> dict[str, str]:
    benchmark = str(workload.get("benchmark_name", "tpch"))
    header_basename = str(
        workload.get("header_basename", paths["header_path"].split("/")[-1].replace(".hpp", ""))
    )
    type_prefix = _type_prefix(paths["create_fn"])
    output_schema = list(workload.get("output_schema", []))
    schema_names = ", ".join(f'"{col["name"]}"' for col in output_schema) or '"result"'

    # cost_model.hpp: AuxCostModelLocation{<config dir>, <file stem>} for every engine.
    bench_camel = "".join(part.title() for part in benchmark.split("_"))
    cost_model_json = str(paths.get("cost_model_default") or f"accelerators/{benchmark}/q{query}/config/q{query}_aux_cost_model.json")
    cost_model_dir, _, cost_model_file = cost_model_json.rpartition("/")

    return {
        "query_number": str(query),
        "engine_name": str(workload.get("engine_name") or ""),
        "cost_model_const": f"k{bench_camel}Q{query}CostModel",
        "cost_model_dir": cost_model_dir,
        "cost_model_stem": cost_model_file.removesuffix(".json"),
        "function_name": paths["function_name"],
        "create_fn": paths["create_fn"],
        "header_basename": header_basename,
        "type_prefix": type_prefix,
        "scan_spec_fn": f"Create{type_prefix}ScanSpec",
        "kernel_fn": f"Create{type_prefix}Kernel",
        "profiling_kernel_fn": f"Create{type_prefix}ProfilingKernel",
        "display_name": str(workload.get("display_name", benchmark)),
        "benchmark_name": benchmark,
        "test_target": paths["test_target"],
        "bench_target": paths["bench_target"],
        "output_schema_names": schema_names,
        **_api_stub_values(manifest or {}),
    }


def _write_if_missing(path: Path, content: str) -> bool:
    if path.is_file():
        return False
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(content, encoding="utf-8")
    return True


def materialize_workload_stubs(
    pipeline_config: PipelineConfig,
    workload: dict[str, Any],
    query: int,
) -> dict[str, Any]:
    """Create missing header/kernel/package/test/benchmark files from templates."""
    repo = pipeline_config.repo_root
    pipe = pipeline_config.pipeline_root
    paths = resolve_workload_paths(pipeline_config, workload, query)
    benchmark = str(workload.get("benchmark_name", "tpch"))
    engine = str(workload.get("engine_name") or "")
    values = _build_stub_values(with_facts(repo, workload, query), paths, query, read_api_manifest(repo, engine))

    created: list[str] = []
    tpl_root = pipe / "templates" / "scaffold"
    bench_tpl_root = pipe / "templates" / benchmark

    stub_specs: list[tuple[Path, Path]] = [
        # Every query declares where its cost models live; each engine's
        # registration passes it on (catalog entry / Runtime).
        (tpl_root / "cost_model.hpp.tpl", repo / paths["cost_model_header_path"]),
        (tpl_root / "header.hpp.tpl", repo / paths["header_path"]),
        (tpl_root / "kernel.cpp.tpl", repo / paths["kernel_path"]),
        (tpl_root / "package.cpp.tpl", repo / paths["package_path"]),
    ]
    # The query registers itself from its own directory.
    register_path = str((workload.get("engine_host") or {}).get("register_path") or "")
    if register_path:
        stub_specs.append((tpl_root / "register.cpp.tpl", repo / register_path))
    if (bench_tpl_root / "test_template.cpp").is_file():
        stub_specs.append((bench_tpl_root / "test_template.cpp", repo / paths["test_path"]))
    if (bench_tpl_root / "benchmark_template.cpp").is_file():
        stub_specs.append((bench_tpl_root / "benchmark_template.cpp", repo / paths["benchmark_path"]))

    for tpl_path, dest in stub_specs:
        if not tpl_path.is_file():
            continue
        content = _substitute(tpl_path.read_text(encoding="utf-8"), values)
        if _write_if_missing(dest, content):
            created.append(str(dest.relative_to(repo)))

    return {
        "created": created,
        "skipped_existing": max(0, len(stub_specs) - len(created)),
    }


def materialize_from_gate(
    repo_root: Path, query: int, engine: str, workload_name: str | None = None
) -> dict[str, Any]:
    from accel_pipeline.config import load_pipeline_config

    pipeline_config = load_pipeline_config(repo_root=repo_root)
    if workload_name is None:
        workload_name = _workload_name_from_gate(repo_root, query, engine)
    workload = load_workload(workload_name, repo_root, engine=engine)
    if workload.get("query_number") is None:
        workload = dict(workload)
        workload["query_number"] = query
    return materialize_workload_stubs(pipeline_config, workload, query)


def _workload_name_from_gate(repo_root: Path, query: int, engine: str) -> str:
    import json

    artifacts = repo_root / "pipeline" / "artifacts"
    candidates = sorted(artifacts.glob(f"*/q{query}/{engine}/gates/_gate_context.json"))
    if not candidates:
        raise FileNotFoundError(f"gate context not found for query {query} on {engine}")
    ctx = json.loads(candidates[-1].read_text(encoding="utf-8"))
    workload_name = ctx.get("workload_name")
    if workload_name:
        return str(workload_name)
    benchmark = str(ctx.get("benchmark_name", "tpch"))
    return f"{benchmark}_q{query}"


def _cpp_string_list(items: list[str]) -> str:
    return "{" + ", ".join('"' + item.replace("\\", "\\\\").replace('"', '\\"') + '"' for item in items) + "}"


def materialize_registration(pipeline_config: PipelineConfig, workload: dict[str, Any], query: int) -> dict[str, Any]:
    """The query's registration before any test is written: the engine's scaffold (`backends/<engine>/api/scaffold/
    register.cpp.tpl`) filled with the query's SQL and facts, so the query is registered with a module that declines
    every call and the fallback SQL answers. Written only when missing: the implementation replaces it in place."""
    import json

    from accel_pipeline.paths import analysis_artifact_path

    repo = pipeline_config.repo_root
    if workload.get("workload_kind") != "aux_state":  # the declining module is an aux module; a stateless query has none
        return {"created": [], "skipped": "not an aux-state workload"}
    engine = str(workload.get("engine_name") or "")
    host = workload.get("engine_host") or {}
    destination = str(host.get("aux_register_path") or "")
    template = repo / "backends" / engine / "api" / "scaffold" / "register.cpp.tpl"
    if not destination or not template.is_file():
        return {"created": [], "skipped": f"engine {engine!r} has no registration scaffold"}
    if (repo / destination).is_file():
        return {"created": [], "skipped_existing": 1}
    benchmark = str(workload.get("benchmark_name", "tpch"))
    analysis = json.loads(analysis_artifact_path(repo, query, workload=workload).read_text(encoding="utf-8"))
    facts = with_facts(repo, workload, query)
    accelerator_id = str(analysis["accelerator_id"])
    values = {
        "benchmark_name": benchmark,
        "engine_name": engine,
        "query_number": str(query),
        "accelerator_id": accelerator_id,
        "table_function": "accel_" + accelerator_id,
        "argument_types": _cpp_string_list([str(p["type"]).upper() for p in workload.get("parameters", [])]),
        "base_sql": Path(workload["reference_sql_path"]).read_text(encoding="utf-8").strip().rstrip(";"),
        "fallback_sql": (repo / "workloads" / benchmark / f"q{query}.sql").read_text(encoding="utf-8").strip().rstrip(";"),
        "tables": _cpp_string_list([t["name"] for t in facts["dependent_tables"]]),
    }
    content = _substitute(template.read_text(encoding="utf-8"), values)
    _write_if_missing(repo / destination, content)
    return {"created": [destination]}

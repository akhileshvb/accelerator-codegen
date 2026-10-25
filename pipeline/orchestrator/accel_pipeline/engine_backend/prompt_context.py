"""The prompt keys of an engine_backend run: the engine profile, its host contract and the layer being built."""

from __future__ import annotations

from typing import Any

from accel_pipeline.config import PipelineConfig
from accel_pipeline.context import read_text
from accel_pipeline.engine_host import format_host_block
from accel_pipeline.engine_profile import render_engine_profile


def engine_backend_keys(cfg: PipelineConfig, workload: dict[str, Any], stage: str) -> dict[str, str]:
    repo, prompts = cfg.repo_root, cfg.pipeline_root / "prompts" / "engine_backend"
    target_paths = dict(workload.get("target_paths") or {})
    cmake = workload.get("cmake") or {}
    placement = workload.get("placement_candidates") or []
    optional_examples = read_text(prompts / "optional_examples.md")
    if placement:
        optional_examples += "\n\nWorkload `placement_candidates` (S6):\n" + "\n".join(f"- `{m}`" for m in placement)
    update_existing = (repo / str(target_paths.get("host_header", ""))).is_file() and workload.get("allow_update_existing", True)
    return {
        "engine_profile": _profile_for_stage(workload, stage),
        "backend_api": "",
        "accelerator_engine_profile": "",
        "engine_host_discovery": format_host_block(workload, repo, cfg.build_dir),
        "optional_examples_section": optional_examples,
        "engine_generation_mode": "update_existing" if update_existing else "generate_from_scratch",
        "engine_target_paths": "\n".join(f"- `{k}`: `{v}`" for k, v in target_paths.items()),
        "engine_write_paths": "\n".join(f"- `{p}`" for p in (workload.get("write_paths") or [])),
        "engine_cmake_entry": str(cmake.get("entry") or ""),
        "engine_cmake_build_flag": str(cmake.get("build_flag") or "(none)"),
        "engine_layers_spec": read_text(prompts / "layers.md"),
        "layer_contracts": read_text(prompts / "layer_contracts.md"),
        "host_contract": read_text(prompts / "host_contract.md"),
        "host_runtime_kind": str((workload.get("host_runtime") or {}).get("kind") or "host_cmake"),
        # a layer stage supplies these (stage._layer_extra_context)
        **dict.fromkeys(
            ("layer_id", "layer_title", "layer_gate", "layer_harness", "layer_cmake_target", "layer_rules", "prior_layers"), ""
        ),
    }


def _profile_for_stage(workload: dict[str, Any], stage: str) -> str:
    """The engine profile sections this stage sees (impl_<layer> / critique_<layer> use the layer's)."""
    layer = stage.split("_", 1)[1] if stage.startswith(("impl_", "critique_")) else None
    return render_engine_profile(
        workload["engine_profile"],
        workload["engine_profile_sections"][layer or stage],
        layer=layer,
        host_layer_runs=bool(workload.get("requires_in_process_host", False)),
    )

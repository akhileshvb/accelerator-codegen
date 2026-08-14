"""Compressed optimization runtime state (file-backed, not chat history)."""

from __future__ import annotations

import json
from datetime import datetime, timezone
from pathlib import Path
from typing import Any

from accel_pipeline.base_bench import (
    build_optimization_diagnosis,
    load_base_optimization_state,
)
from accel_pipeline.config import PipelineConfig, load_workload, resolve_optimization_edit_paths, resolve_workload_paths
from accel_pipeline.kernel_hot_path_review import (
    analyze_kernel_source,
    format_kernel_review_for_prompt,
)
from accel_pipeline.optimization_evidence import extract_bottleneck_evidence
from accel_pipeline.paths import load_query_analysis, query_artifacts_dir

RUNTIME_MD = "optimization_runtime.md"
RUNTIME_JSON = "optimization_runtime.json"
AB_TRIALS_JSON = "ab_trials.json"
EPISODE_LOG = "episode_log.jsonl"
PLAYBOOK_TECHNIQUES_JSON = "playbook_techniques.json"
TOOL_ARTIFACTS_DIR = "runtime_tool_artifacts"
BASE_KERNEL_OPTIMIZATION_REL = "pipeline/prompts/shared/base_kernel_optimization.md"
OPTIMIZATION_WORKFLOW_COMPACT_REL = "pipeline/prompts/shared/optimization_workflow_compact.md"


def runtime_artifacts_dir(art: Path) -> Path:
    path = art / TOOL_ARTIFACTS_DIR
    path.mkdir(parents=True, exist_ok=True)
    return path


def load_runtime_json(art: Path) -> dict[str, Any]:
    path = art / RUNTIME_JSON
    if not path.is_file():
        return {}
    return json.loads(path.read_text(encoding="utf-8"))


def save_runtime_json(art: Path, state: dict[str, Any]) -> None:
    (art / RUNTIME_JSON).write_text(json.dumps(state, indent=2), encoding="utf-8")


def load_ab_trials(art: Path) -> list[dict[str, Any]]:
    path = art / "attempt_log.jsonl"
    if path.is_file():
        rows: list[dict[str, Any]] = []
        for line in path.read_text(encoding="utf-8").splitlines():
            line = line.strip()
            if not line:
                continue
            try:
                rows.append(json.loads(line))
            except json.JSONDecodeError:
                continue
        if rows:
            return rows
    legacy = art / AB_TRIALS_JSON
    if not legacy.is_file():
        return []
    data = json.loads(legacy.read_text(encoding="utf-8"))
    return list(data.get("trials") or [])


def save_ab_trials(art: Path, trials: list[dict[str, Any]]) -> None:
    (art / AB_TRIALS_JSON).write_text(
        json.dumps({"trials": trials}, indent=2),
        encoding="utf-8",
    )


def append_episode_summary(art: Path, episode: int, summary: str) -> None:
    path = art / EPISODE_LOG
    payload = {
        "ts": datetime.now(timezone.utc).isoformat(),
        "episode": episode,
        "summary": _compress_text(summary, max_chars=400),
    }
    with path.open("a", encoding="utf-8") as handle:
        handle.write(json.dumps(payload, ensure_ascii=False) + "\n")


def load_episode_log(art: Path, *, limit: int = 8) -> list[dict[str, Any]]:
    path = art / EPISODE_LOG
    if not path.is_file():
        return []
    rows: list[dict[str, Any]] = []
    for line in path.read_text(encoding="utf-8").splitlines():
        line = line.strip()
        if not line:
            continue
        try:
            rows.append(json.loads(line))
        except json.JSONDecodeError:
            continue
    return rows[-limit:]


def load_playbook_techniques(art: Path, *, limit: int = 12) -> list[dict[str, Any]]:
    path = art / PLAYBOOK_TECHNIQUES_JSON
    if not path.is_file():
        return []
    try:
        data = json.loads(path.read_text(encoding="utf-8"))
    except json.JSONDecodeError:
        return []
    techniques = list(data.get("techniques") or [])
    return techniques[-limit:]


def load_optimization_workflow_compact(repo_root: Path) -> str:
    path = repo_root / OPTIMIZATION_WORKFLOW_COMPACT_REL
    if not path.is_file():
        return ""
    return path.read_text(encoding="utf-8").strip()


def load_base_kernel_optimization_text(repo_root: Path) -> str:
    path = repo_root / BASE_KERNEL_OPTIMIZATION_REL
    if not path.is_file():
        return ""
    return path.read_text(encoding="utf-8").strip()


def _compress_text(text: str, *, max_chars: int) -> str:
    cleaned = " ".join((text or "").split())
    if len(cleaned) <= max_chars:
        return cleaned
    return cleaned[: max_chars - 3] + "..."


def _read_json(path: Path) -> dict[str, Any]:
    if not path.is_file():
        return {}
    try:
        return json.loads(path.read_text(encoding="utf-8"))
    except json.JSONDecodeError:
        return {}


def _profile_guidance(profile: dict[str, Any], diagnosis: dict[str, Any]) -> list[str]:
    lines: list[str] = []
    for signal in (diagnosis.get("profile_signals") or [])[:8]:
        lines.append(f"- {signal}")
    counters = profile.get("counters") or {}
    for key in ("flat_chunks", "dict_chunks", "generic_chunks"):
        if counters.get(key) is not None:
            lines.append(f"- `{key}={counters[key]}`")
    phases = diagnosis.get("profile_phases_us") or {}
    for key in ("sub_scan_us", "num_scan_us", "num_aggregate_us", "pre_consume_us", "finalize_us"):
        if phases.get(key) is not None:
            lines.append(f"- `{key}={float(phases[key]):.0f}`")
    return lines


def build_runtime_state(
    pipeline_config: PipelineConfig,
    workload: dict[str, Any],
    query: int,
    *,
    optimization_iter: int,
    episode: int = 1,
) -> dict[str, Any]:
    art = query_artifacts_dir(pipeline_config.repo_root, query, workload=workload)
    opt_state = load_base_optimization_state(art)
    baseline = _read_json(art / "baseline_summary.json")
    profile = _read_json(art / "profile_summary.json")
    _, analysis = load_query_analysis(pipeline_config.repo_root, query, workload=workload)

    paths = resolve_workload_paths(pipeline_config, workload, query)
    edit_paths = resolve_optimization_edit_paths(pipeline_config, workload, query)
    kernel_path = pipeline_config.repo_root / edit_paths["primary_cpp"]
    kernel_review = analyze_kernel_source(kernel_path)

    diagnosis = build_optimization_diagnosis(
        baseline,
        profile,
        best_speedup_median=float(opt_state.get("best_speedup_median") or 0.0),
        analysis=analysis or None,
        kernel_review=kernel_review,
    )
    evidence = extract_bottleneck_evidence(
        diagnosis, profile, kernel_review, analysis=analysis or None
    )
    artifact_ns = str(workload.get("artifact_namespace", workload.get("benchmark_name", "tpch")))

    return {
        "optimization_iter": optimization_iter,
        "episode": episode,
        "artifact_namespace": artifact_ns,
        "query": query,
        "best_speedup_median": opt_state.get("best_speedup_median"),
        "last_speedup_median": opt_state.get("last_speedup_median"),
        "speedup_median": diagnosis.get("speedup_median"),
        "beats_duckdb": diagnosis.get("beats_duckdb"),
        "regressed_vs_best": diagnosis.get("regressed_vs_best"),
        "dominant_phase": diagnosis.get("dominant_phase"),
        "dominant_phase_us": diagnosis.get("dominant_phase_us"),
        "profile_counters": diagnosis.get("profile_counters") or {},
        "profile_phases_us": diagnosis.get("profile_phases_us") or {},
        "profile_guidance": _profile_guidance(profile, diagnosis),
        "profile_signals": diagnosis.get("profile_signals") or [],
        "investigation_questions": diagnosis.get("investigation_questions") or [],
        "bottleneck_evidence": evidence,
        "suggested_focus": (diagnosis.get("suggested_focus") or [])[:2],
        "kernel_hot_path_review": kernel_review,
        "kernel_hot_path_guidance": format_kernel_review_for_prompt(kernel_review),
        "lacking": (diagnosis.get("lacking") or [])[:4],
        "findings": (diagnosis.get("findings") or [])[:5],
        "recent_iterations": (opt_state.get("iterations") or [])[-2:],
        "ab_trials": load_ab_trials(art)[-5:],
        "episode_summaries": load_episode_log(art, limit=5),
        "playbook_techniques": load_playbook_techniques(art, limit=8),
        "optimization_workflow_compact": load_optimization_workflow_compact(pipeline_config.repo_root),
        "kernel_paths": {
            "header": edit_paths["header"],
            "kernel": edit_paths["primary_cpp"],
            "package": edit_paths["secondary_cpp"],
            "fused_scan": paths.get("fused_scan_path") if not edit_paths.get("is_aux") else None,
            "cost_model": paths.get("cost_model_config") if edit_paths.get("is_aux") else None,
            "base_kernel_readonly": edit_paths.get("base_kernel_path"),
        },
        "bench_target": paths["bench_target"],
        "dataset_db": paths["dataset_db"],
        "db_env_var": paths["db_env_var"],
    }


def format_runtime_markdown(state: dict[str, Any]) -> str:
    lines = [
        f"# Optimization runtime (iter {state.get('optimization_iter')}, episode {state.get('episode')})",
        "",
        "## Goals",
        f"- best_speedup_median: **{float(state.get('best_speedup_median') or 0):.3f}x**",
        f"- last_speedup_median: **{float(state.get('last_speedup_median') or 0):.3f}x**",
        f"- last_gate speedup: **{float(state.get('speedup_median') or 0):.3f}x**",
    ]
    if state.get("regressed_vs_best"):
        lines.append("- **REGRESSION vs best** — workspace should be on restored best snapshot.")
    lines.append("")
    lines.append("## Profile signals")
    guidance = state.get("profile_guidance") or []
    if guidance:
        lines.extend(guidance)
    else:
        lines.append("- (no profile data yet — run `run_quick_benchmark` with profile after edits)")
    questions = state.get("investigation_questions") or []
    if questions:
        lines.append("")
        lines.append("## Investigate (grep_context before editing)")
        for item in questions:
            lines.append(f"- {item}")
    kernel_guidance = state.get("kernel_hot_path_guidance") or []
    if kernel_guidance:
        lines.append("")
        lines.extend(kernel_guidance)
    palette = state.get("bottleneck_evidence", {}).get("tiers") or []
    if palette:
        lines.append("")
        lines.append("## Bottleneck evidence (tier-organized — pick ONE hypothesis)")
        for tier_block in palette[:7]:
            tier = tier_block.get("tier", "")
            lines.append(f"### {tier}")
            for signal in (tier_block.get("signals") or [])[:4]:
                lines.append(
                    f"- {signal.get('metric')}: {signal.get('value')} ({signal.get('context', '')})"
                )
            if tier_block.get("upper_bound_pct") is not None:
                lines.append(f"- upper_bound_pct (Amdahl): {tier_block['upper_bound_pct']}")
    focus = state.get("suggested_focus") or []
    if focus:
        lines.append("")
        lines.append("## Focus this iteration")
        for item in focus[:3]:
            lines.append(f"- {item}")
    lines.append("")
    lines.append("## Attempt history (this optimization iter)")
    trials = state.get("ab_trials") or []
    if trials:
        lines.append("| attempt | hypothesis | improvement | kept |")
        lines.append("|---------|------------|-------------|------|")
        for row in trials:
            lines.append(
                f"| {row.get('attempt', row.get('trial_id', ''))} "
                f"| {_compress_text(str(row.get('hypothesis', '')), max_chars=48)} "
                f"| {row.get('improvement_pct', 'n/a')} "
                f"| {row.get('kept', row.get('accepted', False))} |"
            )
    else:
        lines.append("- No attempts yet.")
    if state.get("lacking"):
        lines.append("")
        lines.append("## Gaps vs goal")
        for item in state["lacking"]:
            lines.append(f"- {item}")
    if state.get("recent_iterations"):
        lines.append("")
        lines.append("## Recent gate iterations")
        for row in state["recent_iterations"]:
            lines.append(
                f"- iter {row.get('iteration')}: {float(row.get('speedup_median') or 0):.3f}x "
                f"({'improved' if row.get('improved') else 'flat/regressed'})"
            )
    episode_summaries = state.get("episode_summaries") or []
    if episode_summaries:
        lines.append("")
        lines.append("## Episode history (this optimization iter)")
        lines.append("Do not re-read snapshot/reference kernels — prior episodes are summarized here.")
        for row in episode_summaries:
            lines.append(
                f"- ep {row.get('episode')}: {_compress_text(str(row.get('summary', '')), max_chars=160)}"
            )
    playbook_techniques = state.get("playbook_techniques") or []
    if playbook_techniques:
        lines.append("")
        lines.append("## Technique memory (structured)")
        for row in playbook_techniques:
            technique_id = row.get("technique_id", "unknown")
            kept = row.get("kept")
            verdict = "kept" if kept else "discarded" if kept is False else "n/a"
            delta = row.get("speedup_delta")
            delta_text = f", Δspeedup={float(delta):+.3f}x" if delta is not None else ""
            profile = row.get("profile_deltas") or {}
            profile_bits = ", ".join(f"{k}={v}" for k, v in list(profile.items())[:3])
            profile_text = f", {profile_bits}" if profile_bits else ""
            lines.append(
                f"- iter {row.get('iteration', '?')} `{technique_id}` ({verdict}{delta_text}{profile_text})"
            )
    base_kernel = str(state.get("optimization_workflow_compact") or "").strip()
    if base_kernel:
        lines.append("")
        lines.append("## Optimization workflow")
        lines.append(base_kernel)
    lines.append("")
    lines.append("## Scale invariance")
    lines.append(
        "- Benchmark/profile are one dataset size only; optimizations must scale with row volume."
    )
    lines.append(
        "- Size structures from bind-time `row_estimate` / table row counts — not hardcoded profile counters."
    )
    pre_rows = int((state.get("profile_counters") or {}).get("pre_rows") or 0)
    if pre_rows:
        lines.append(f"- Current `pre_rows` (this run only): {pre_rows} — do not bake this constant into code.")
    lines.append("")
    for key, value in (state.get("kernel_paths") or {}).items():
        if value:
            lines.append(f"- {key}: `{value}`")
    return "\n".join(lines)


def refresh_optimization_runtime(
    pipeline_config: PipelineConfig,
    workload: dict[str, Any],
    query: int,
    *,
    optimization_iter: int,
    episode: int = 1,
) -> Path:
    art = query_artifacts_dir(pipeline_config.repo_root, query, workload=workload)
    state = build_runtime_state(
        pipeline_config,
        workload,
        query,
        optimization_iter=optimization_iter,
        episode=episode,
    )
    save_runtime_json(art, state)
    md_path = art / RUNTIME_MD
    md_path.write_text(format_runtime_markdown(state), encoding="utf-8")
    return md_path


def runtime_path_for_prompt(
    pipeline_config: PipelineConfig,
    workload: dict[str, Any],
    query: int,
) -> str:
    art = query_artifacts_dir(pipeline_config.repo_root, query, workload=workload)
    rel = art / RUNTIME_MD
    try:
        return str(rel.relative_to(pipeline_config.repo_root))
    except ValueError:
        return str(rel)


def store_tool_artifact(art: Path, tool_name: str, content: str) -> str:
    artifacts = runtime_artifacts_dir(art)
    existing = sorted(artifacts.glob("*.txt"))
    seq = len(existing) + 1
    path = artifacts / f"{seq:04d}_{tool_name}.txt"
    path.write_text(content, encoding="utf-8")
    try:
        return str(path.relative_to(art.parent.parent.parent))
    except ValueError:
        return str(path)


def compress_tool_output(art: Path, tool_name: str, raw: str) -> str:
    if tool_name == "run_command" and len(raw) > 1800:
        stored = store_tool_artifact(art, "run_command", raw)
        tail = raw[-1200:]
        parsed = _parse_benchmark_from_output(raw)
        if parsed:
            return (
                f"compressed_command_output (full log: {stored})\n"
                f"speedup_median={parsed.get('speedup_median')} "
                f"ref_us={parsed.get('reference_median_us')} accel_us={parsed.get('accelerator_median_us')}\n"
                f"profile: pre_consume_us={parsed.get('pre_consume_us')} generic_rows={parsed.get('generic_rows')}\n"
                f"tail:\n{tail}"
            )
        return f"compressed_command_output (full log: {stored})\n{tail}"
    if tool_name == "read_file" and len(raw) > 6_000:
        stored = store_tool_artifact(art, "read_file", raw)
        return raw[:6_000] + f"\n... [truncated {len(raw)} chars; full: {stored}; use offset/limit or grep_context]"
    if tool_name == "grep_context" and len(raw) > 8_000:
        stored = store_tool_artifact(art, "grep_context", raw)
        return raw[:8_000] + f"\n... [truncated; full: {stored}; narrow pattern or reduce context]"
    if tool_name in {"rip_grep", "ast_grep", "search_repo"} and len(raw) > 10_000:
        stored = store_tool_artifact(art, tool_name, raw)
        return raw[:10_000] + f"\n... [truncated; full: {stored}]"
    return raw


def _parse_benchmark_from_output(output: str) -> dict[str, Any]:
    from accel_pipeline.base_bench import parse_benchmark_log

    parsed = parse_benchmark_log(output)
    if not parsed:
        return {}
    return parsed


def record_ab_trial(
    art: Path,
    *,
    trial_id: str,
    hypothesis: str,
    metrics: dict[str, Any],
    kept: bool,
) -> None:
    trials = load_ab_trials(art)
    trials.append(
        {
            "trial_id": trial_id,
            "hypothesis": hypothesis,
            "metrics": metrics,
            "kept": kept,
            "ts": datetime.now(timezone.utc).isoformat(),
        }
    )
    save_ab_trials(art, trials)

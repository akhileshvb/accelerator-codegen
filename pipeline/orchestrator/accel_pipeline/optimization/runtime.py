"""Compressed optimization runtime state (file-backed, not chat history)."""

from __future__ import annotations

import json
from datetime import datetime, timezone
from pathlib import Path
from typing import Any

from accel_pipeline.optimization.base_bench import (
    build_optimization_diagnosis,
    load_base_optimization_state,
)
from accel_pipeline.config import (
    PipelineConfig,
    resolve_optimization_edit_paths,
    resolve_workload_paths,
    load_query_analysis,
    query_artifacts_dir,
)
from accel_pipeline.jsonio import read_json
from accel_pipeline.optimization.kernel_hot_path_review import (
    analyze_kernel_source,
    format_kernel_review_for_prompt,
)
from accel_pipeline.optimization.saturation import early_stop_config
from accel_pipeline.optimization.evidence import extract_bottleneck_evidence
from accel_pipeline.optimization.negative_memory import (
    format_negative_memory_markdown,
    load_negative_memory,
    load_negative_memory_guidance,
)

RUNTIME_MD = "optimization_runtime.md"
RUNTIME_JSON = "optimization_runtime.json"
AB_TRIALS_JSON = "ab_trials.json"
EPISODE_LOG = "episode_log.jsonl"
PLAYBOOK_TECHNIQUES_JSON = "playbook_techniques.json"
OPTIMIZATION_WORKFLOW_COMPACT_REL = "pipeline/prompts/shared/optimization_workflow_compact.md"


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


def _compress_text(text: str, *, max_chars: int) -> str:
    cleaned = " ".join((text or "").split())
    if len(cleaned) <= max_chars:
        return cleaned
    return cleaned[: max_chars - 3] + "..."


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
    baseline = read_json(art / "baseline_summary.json")
    profile = read_json(art / "profile_summary.json")
    _, analysis = load_query_analysis(pipeline_config.repo_root, query, workload=workload)

    paths = resolve_workload_paths(pipeline_config, workload, query)
    edit_paths = resolve_optimization_edit_paths(pipeline_config, workload, query)
    kernel_path = pipeline_config.repo_root / edit_paths["primary_cpp"]
    kernel_review = analyze_kernel_source(kernel_path)

    is_aux = workload.get("workload_kind") == "aux_state"
    if is_aux:
        from accel_pipeline.optimization.aux_bench import load_optimization_state
        from accel_pipeline.optimization.aux_profile import build_aux_profile_digest
        from accel_pipeline.optimization.checkpoint import load_best_valid_baseline

        aux_digest = build_aux_profile_digest(
            art,
            repo_root=pipeline_config.repo_root,
            editable_rel_paths=[p for p in edit_paths.get("all_paths") or [] if p],
        )
        aux_profile = dict(aux_digest.get("profile_summary") or {})
        scoreboard = dict(aux_digest.get("scoreboard") or {})
        freshness = dict(aux_digest.get("freshness") or {})
        best_valid = load_best_valid_baseline(art)
        aux_opt = load_optimization_state(pipeline_config.repo_root, query, workload) or {}
        fair = read_json(art / "aux_bench_summary.json")
        read_floor = early_stop_config(pipeline_config)["saturation_min_read_speedup"]
        best_read = float(
            (best_valid.get("metrics") or {}).get("read_only_query_speedup")
            or (aux_opt.get("best_summary") or {}).get("read_only_query_speedup")
            or fair.get("read_only_query_speedup")
            or aux_profile.get("read_only_query_speedup")
            or 0.0
        )
        last_read = float(aux_profile.get("read_only_query_speedup") or best_read or 0.0)
        diag = dict(aux_profile.get("store_diagnostics") or {})
        phases = dict(aux_profile.get("profile_phases_us") or {})
        signals = list(aux_profile.get("profile_signals") or [])
        if not signals:
            signals = ["call MCP run_profile / run_quick_benchmark to refresh aux metrics"]
        best_metrics = dict(best_valid.get("metrics") or {})
        best_by_freq = dict(best_metrics.get("by_write_frequency_pct") or {})
        fair_by_freq = dict(fair.get("by_write_frequency_pct") or {})
        write_row = (
            best_by_freq.get("10")
            or fair_by_freq.get("10")
            or best_by_freq.get("1")
            or fair_by_freq.get("1")
            or {}
        )
        write_mix = float(
            best_metrics.get("write_mix_workload_speedup")
            or write_row.get("workload_speedup")
            or 0.0
        )
        write_mix_pct = int(
            best_metrics.get("write_mix_pct")
            or write_row.get("write_frequency_pct")
            or (10 if write_mix > 0 else 0)
        )
        evidence = {
            "tiers": [
                {
                    "tier": "aux_read_path",
                    "signals": [
                        {"metric": k, "value": v, "context": "store_diagnostics"}
                        for k, v in diag.items()
                    ][:8],
                    "relevant_phases_us": phases,
                }
            ]
        }
        if write_mix_pct > 0:
            evidence["tiers"].insert(
                0,
                {
                    "tier": "aux_write_maintenance",
                    "signals": [
                        {
                            "metric": "write_mix_workload_speedup",
                            "value": write_mix,
                            "context": f"{write_mix_pct}% write quick-bench / fair sweep",
                        },
                        {
                            "metric": "guidance",
                            "value": (
                                "Prefer ApplyDelta/partition-local maintenance; "
                                "RebuildAll-per-point-write is a cliff"
                            ),
                            "context": "dual_axis",
                        },
                    ],
                },
            )
        if aux_profile.get("custom_profile_events"):
            evidence["tiers"].append(
                {
                    "tier": "custom_instrumentation",
                    "signals": [
                        {
                            "metric": "custom_profile_events",
                            "value": len(aux_profile["custom_profile_events"]),
                            "context": "ACCEL_CUSTOM_PROFILE lines",
                        }
                    ],
                }
            )
        dual_score = float(scoreboard.get("best_valid_score") or 0.0)
        focus = [
            f"Accept score = read^0.75 * write_mix^0.25"
            + (f" (best_valid={dual_score:.3f})" if dual_score > 0 else "")
            + f"; read_only best={best_read:.3f}x; "
            f"{write_mix_pct or 10}% write-mix"
            + (f"={write_mix:.2f}x" if write_mix > 0 else " (measure via quick bench)."),
            "Cheap incremental maintenance is required — Prefer ApplyDelta / touched partitions "
            "over RebuildAll on every point write. Raising write-mix raises the accept score.",
            "Designs must scale with SF (bind-time row estimates; no SF1 hardcoding).",
            "Measure-first: run_profile on CURRENT tree; cite phase_breakdown + 0% and 10% metrics.",
        ]
        if write_mix > 0 and write_mix < 1.0 and best_read >= read_floor:
            focus.insert(
                0,
                f"PRIORITY: write-mix cliff ({write_mix_pct}% workload={write_mix:.2f}x ≪ 1×) while "
                f"read is already strong ({best_read:.1f}x) — hypothesize maintenance "
                f"(ApplyDelta / local rebuild), not further hit-path micro-opts.",
            )
        if float(diag.get("accel_hit_median_us") or 0) > 0:
            focus.append(
                f"Hit path currently {float(diag['accel_hit_median_us']):.0f}µs @ "
                f"hit_rate={float(diag.get('accel_hit_rate') or 0):.2f}."
            )
        if aux_profile.get("dominant_phase"):
            focus.append(
                f"Dominant timed phase: {aux_profile.get('dominant_phase')} "
                f"@ {float(aux_profile.get('dominant_phase_us') or 0):.1f}µs."
            )
        lacking = []
        if freshness.get("stale"):
            for reason in list(freshness.get("reasons") or [])[:3]:
                lacking.append(f"STALE PROFILE: {reason}")
        fair_ro = float(fair.get("read_only_query_speedup") or 0.0)
        if fair_ro > 0 and last_read + 0.05 < fair_ro:
            lacking.append(
                f"Quick/current read_only {last_read:.3f}x is below fair-sweep best "
                f"{fair_ro:.3f}x — restore best_kernel before chasing micro-opts."
            )
        if write_mix > 0 and write_mix < 1.0:
            lacking.append(
                f"WRITE-MIX CLIFF: {write_mix_pct}% workload_speedup={write_mix:.2f}x — "
                "incremental maintenance too expensive (likely full rebuild per write). "
                "Attack ApplyDelta / partition-local paths before more read micro-opts."
            )
        if not (aux_profile.get("phase_breakdown") or []):
            lacking.append(
                "No split phase timers yet — add ACCEL_CUSTOM_PROFILE phase+us+counts, "
                "then run_profile before hypothesizing."
            )
        lacking.append(
            "Expect large gains on BOTH axes when aux is valid; micro-shuffles that move "
            "<5% on read while leaving a write cliff are the wrong tier."
        )
        return {
            "optimization_iter": optimization_iter,
            "episode": episode,
            "artifact_namespace": str(
                workload.get("artifact_namespace", workload.get("benchmark_name", "tpch"))
            ),
            "query": query,
            "workload_kind": "aux_state",
            "best_speedup_median": best_read,
            "last_speedup_median": last_read,
            "speedup_median": last_read,
            "read_only_query_speedup": last_read,
            "beats_duckdb": last_read > 1.0,
            "regressed_vs_best": best_read > 0 and last_read + 0.01 < best_read,
            "scoreboard": scoreboard,
            "read_floor": read_floor,
            "freshness": freshness,
            "phase_breakdown": aux_profile.get("phase_breakdown") or [],
            "work_counters_mean": aux_profile.get("work_counters_mean") or {},
            "residual_vs_hit_us": aux_profile.get("residual_vs_hit_us"),
            "dominant_phase": aux_profile.get("dominant_phase"),
            "dominant_phase_us": aux_profile.get("dominant_phase_us"),
            "profile_counters": aux_profile.get("counters") or {},
            "profile_phases_us": phases,
            "profile_guidance": [f"- {s}" for s in signals],
            "profile_signals": signals,
            "investigation_questions": [
                "Where does accel_hit_median_us go (decode vs scan vs materialize vs join)?",
                "Is StageMaintenance RebuildAll-only, or is ApplyDelta / partition-local used for point writes?",
                "What dominates 10% write-mix time: rebuild, delta apply, or cold build amortized?",
                "Can partition layout / encoding cut partitions_read or state_bytes_read at SF20+?",
            ],
            "bottleneck_evidence": evidence,
            "suggested_focus": focus[:4],
            "kernel_hot_path_review": kernel_review,
            "kernel_hot_path_guidance": format_kernel_review_for_prompt(kernel_review),
            "lacking": lacking[:5],
            "findings": [],
            "recent_iterations": (aux_opt.get("history") or [])[-2:],
            "ab_trials": load_ab_trials(art)[-5:],
            "episode_summaries": load_episode_log(art, limit=5),
            "playbook_techniques": load_playbook_techniques(art, limit=8),
            "negative_memory": load_negative_memory(pipeline_config.repo_root, pipeline_config.engine_name, limit=8),
            "negative_memory_guidance": load_negative_memory_guidance(pipeline_config.repo_root, pipeline_config.engine_name),
            "optimization_workflow_compact": load_optimization_workflow_compact(
                pipeline_config.repo_root
            ),
            "kernel_paths": {
                "header": edit_paths["header"],
                "kernel": edit_paths["primary_cpp"],
                "package": edit_paths["secondary_cpp"],
                "fused_scan": None,
                "cost_model": paths.get("cost_model_config"),
            },
            "bench_target": paths.get("aux_bench_target") or paths["bench_target"],
            "dataset_db": paths["dataset_db"],
            "db_env_var": paths["db_env_var"],
            "store_diagnostics": diag,
            "fair_sweep_read_only": fair_ro,
            "write_mix_pct": write_mix_pct,
            "write_mix_workload_speedup": write_mix,
            "aux_profile_digest": aux_digest,
        }

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
        "negative_memory": load_negative_memory(pipeline_config.repo_root, pipeline_config.engine_name, limit=8),
        "negative_memory_guidance": load_negative_memory_guidance(pipeline_config.repo_root, pipeline_config.engine_name),
        "optimization_workflow_compact": load_optimization_workflow_compact(pipeline_config.repo_root),
        "kernel_paths": {
            "header": edit_paths["header"],
            "kernel": edit_paths["primary_cpp"],
            "package": edit_paths["secondary_cpp"],
            "fused_scan": paths.get("fused_scan_path") if not edit_paths.get("is_aux") else None,
            "cost_model": paths.get("cost_model_config") if edit_paths.get("is_aux") else None,
        },
        "bench_target": paths["bench_target"],
        "dataset_db": paths["dataset_db"],
        "db_env_var": paths["db_env_var"],
    }


def _read_axis_lines(board: dict[str, Any], floor: float) -> list[str]:
    axes = dict((board.get("read_speedup") or {}).get("best_valid") or {})
    last = dict((board.get("read_speedup") or {}).get("last_measured") or {})

    def fmt(value: Any) -> str:
        return f"{float(value):.2f}x" if value else "n/a"

    lines = [
        f"- **READ AXIS (best valid)**: {fmt(axes.get('vs_engine'))} vs the engine's own SQL",
        f"- read axis (last measured): {fmt(last.get('vs_engine'))} vs the engine's own SQL",
    ]
    base = axes.get("vs_engine")
    if floor > 0 and base and float(base) < floor:
        lines.append(
            f"- **READ FLOOR NOT MET** ({float(base):.2f}x < {floor:g}x over the engine's SQL): the write axis "
            "does not excuse this. Profile first, then attack the top hot spot; restructure when the "
            "profile shows the read visits too much state."
        )
    return lines


def format_runtime_markdown(state: dict[str, Any]) -> str:
    is_aux = state.get("workload_kind") == "aux_state"
    score_label = "read_only_query_speedup" if is_aux else "speedup_median"
    board = state.get("scoreboard") or {}
    metric_name = str(board.get("score_metric") or (
        "dual_axis_weighted(read^0.75 * write_mix^0.25)" if is_aux else "speedup_median"
    ))
    lines = [
        f"# Optimization runtime (iter {state.get('optimization_iter')}, episode {state.get('episode')})",
        "",
        "## Scoreboard (stable — A/B compare against best_valid only)",
        f"- score_metric: `{metric_name}`",
        f"- best_{score_label}: **{float(state.get('best_speedup_median') or 0):.3f}x**",
        f"- last_{score_label}: **{float(state.get('last_speedup_median') or 0):.3f}x**",
        f"- last_gate {score_label}: **{float(state.get('speedup_median') or 0):.3f}x**",
    ]
    if board.get("best_valid_score") is not None:
        lines.append(
            f"- scoreboard.best_valid_score: **{float(board['best_valid_score']):.3f}x** "
            f"(attempt={board.get('best_valid_attempt_id') or 'n/a'}; "
            f"hit_us={board.get('best_valid_hit_us')})"
        )
    if board.get("improvement_pct_vs_best_valid") is not None:
        lines.append(
            f"- last vs best_valid: **{float(board['improvement_pct_vs_best_valid']):+.2f}%**"
        )
    if is_aux and state.get("fair_sweep_read_only"):
        lines.append(
            f"- fair_sweep_read_only: **{float(state['fair_sweep_read_only']):.3f}x** "
            "(SF1 fair reference — do not confuse with A/B keep/regress target)"
        )
    if is_aux and state.get("write_mix_pct"):
        wm = float(state.get("write_mix_workload_speedup") or 0.0)
        lines.append(
            f"- write_mix ({int(state['write_mix_pct'])}%): **{wm:.3f}x** workload "
            "(feeds dual-axis accept score as write_mix^0.25)"
        )
        if wm > 0 and wm < 1.0:
            lines.append(
                "- **WRITE-MIX CLIFF** — prefer ApplyDelta / local rebuilds; do not spend the "
                "attempt on tiny read micro-opts while this stays ≪1×."
            )
    if is_aux:
        lines.extend(_read_axis_lines(board, float(state.get("read_floor") or 0.0)))
    lines.append(
        "- Early stop: `OPTIMIZATION_SATURATED` is honored once the read speedup over the engine's own SQL "
        "reaches the floor; below it, only after the last trials were each profile-guided "
        "(`begin_ab_trial` with hot_spot, expected_gain, strategy), distinct, and included one "
        "`structural` and one `low_level` change."
    )
    if state.get("regressed_vs_best"):
        lines.append("- **REGRESSION vs best** — workspace should be on restored best snapshot.")
    freshness = state.get("freshness") or {}
    if freshness:
        lines.append("")
        lines.append("## Profile freshness")
        lines.append(f"- stale: **{bool(freshness.get('stale'))}**")
        for reason in list(freshness.get("reasons") or [])[:4]:
            lines.append(f"- {reason}")
    lines.append("")
    lines.append("## Profile signals")
    guidance = state.get("profile_guidance") or []
    if guidance:
        lines.extend(guidance)
    else:
        lines.append("- (no profile data yet — run `run_quick_benchmark` / `run_profile`)")
    phase_rows = state.get("phase_breakdown") or []
    if phase_rows:
        lines.append("")
        lines.append("## Phase breakdown (custom timers)")
        lines.append("| phase | mean_us | p50_us | n | counts_mean |")
        lines.append("|-------|---------|--------|---|-------------|")
        for row in phase_rows[:12]:
            counts = row.get("counts_mean") or {}
            count_s = ",".join(f"{k}={v}" for k, v in list(counts.items())[:4]) or "-"
            lines.append(
                f"| {row.get('phase')} | {row.get('mean_us')} | {row.get('p50_us')} "
                f"| {row.get('n')} | {count_s} |"
            )
        if state.get("residual_vs_hit_us") is not None:
            lines.append(f"- residual_vs_hit_us: {state.get('residual_vs_hit_us')}")
    work = state.get("work_counters_mean") or {}
    if work:
        lines.append("")
        lines.append("## Work counters (mean)")
        for key, value in list(work.items())[:10]:
            lines.append(f"- `{key}={value}`")
    if is_aux and state.get("store_diagnostics"):
        lines.append("")
        lines.append("## Store diagnostics")
        for key, value in dict(state["store_diagnostics"]).items():
            lines.append(f"- `{key}={value}`")
    questions = state.get("investigation_questions") or []
    if questions:
        lines.append("")
        lines.append("## Investigate (read the code before editing)")
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
    if state.get("recent_iterations") and not is_aux:
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
    neg_lines = format_negative_memory_markdown(
        list(state.get("negative_memory") or []),
        guidance=str(state.get("negative_memory_guidance") or ""),
        limit=8,
    )
    if neg_lines:
        lines.append("")
        lines.extend(neg_lines)
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
    if is_aux:
        lines.append(
            "- Custom profiling: getenv(`ACCEL_AUX_CUSTOM_PROFILE`) + `ACCEL_CUSTOM_PROFILE {json}` lines are allowed; no-op when unset."
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

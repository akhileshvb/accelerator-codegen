"""MCP tools of an optimization stage: the profile and bottleneck digests and the A/B trial loop."""

from __future__ import annotations

import json
from typing import Callable

from accel_pipeline.config import resolve_optimization_edit_paths, query_artifacts_dir
from accel_pipeline.jsonio import read_json
from accel_pipeline.optimization.ab_tools import AbToolContext, make_ab_tools
from accel_pipeline.optimization.aux_profile import build_aux_profile_digest, load_aux_store_diagnostics
from accel_pipeline.optimization.scoreboard import (
    build_scoreboard,
    custom_profiling_contract,
    profile_freshness,
    write_scoreboard,
)
from accel_pipeline.tools.gate_tools import ToolContext

_FEEDBACK_TAIL_CHARS = 4000
_DIGEST_MAX_CHARS = 50_000
_EVIDENCE_PART_CHARS = 40_000
_EVIDENCE_MAX_CHARS = 80_000


def callables(ctx: ToolContext) -> dict[str, Callable[..., str]]:
    pipeline_config, workload, query = ctx.pipeline_config, ctx.workload, ctx.query
    art = query_artifacts_dir(pipeline_config.repo_root, query, workload=workload)
    is_aux = workload.get("workload_kind") == "aux_state"
    begin_ab_trial, run_quick_benchmark, end_ab_trial = make_ab_tools(
        AbToolContext(pipeline_config=pipeline_config, query=query, workload=workload, optimization_iter=ctx.optimization_iter)
    )

    def feedback_tail() -> str:
        path = art / "optimization_feedback.md"
        return path.read_text(encoding="utf-8")[-_FEEDBACK_TAIL_CHARS:] if path.is_file() else ""

    def get_profile_digest() -> str:
        """Compact phase counters / dominant bottleneck from last profile artifacts."""
        edit = resolve_optimization_edit_paths(pipeline_config, workload, query)
        editable = [p for p in edit.get("all_paths") or [] if p]
        if is_aux:
            payload = build_aux_profile_digest(art, repo_root=pipeline_config.repo_root, editable_rel_paths=editable)
            payload["optimization_feedback_tail"] = feedback_tail()
            return json.dumps(payload, indent=2)[:_DIGEST_MAX_CHARS]

        profile = read_json(art / "profile_summary.json")
        baseline = read_json(art / "baseline_summary.json")
        board = build_scoreboard(art, is_aux=False, last_metrics=profile or baseline)
        write_scoreboard(art, board)
        payload = {
            "scoreboard": board,
            "freshness": profile_freshness(art, repo_root=pipeline_config.repo_root, editable_rel_paths=editable),
            "profile_summary": profile,
            "baseline_summary": baseline,
            "custom_profiling": custom_profiling_contract(is_aux=False),
            "measurement_rules": [
                "Call run_profile on the CURRENT tree before forming a hypothesis.",
                "If freshness.stale is true, do not hypothesize from this digest — remeasure.",
                "A/B keep/regress vs scoreboard.best_valid_score only.",
            ],
            "optimization_feedback_tail": feedback_tail(),
        }
        return json.dumps(payload, indent=2)[:_DIGEST_MAX_CHARS]

    def get_optimization_evidence() -> str:
        """Bottleneck evidence packet from artifacts when available."""
        parts = [
            path.read_text(encoding="utf-8")[:_EVIDENCE_PART_CHARS]
            for path in (art / "optimization_evidence.json", art / "optimization_runtime.md")
            if path.is_file()
        ]
        if is_aux:
            parts.append(get_profile_digest())  # a fresh aux digest, so an empty or mis-shaped runtime still helps
        if not parts:
            return get_profile_digest()
        return "\n\n".join(parts)[:_EVIDENCE_MAX_CHARS]

    def get_store_diagnostics() -> str:
        """Aux store diagnostics from bench/opt state when present."""
        if is_aux:
            diag, source = load_aux_store_diagnostics(art)
            if diag:
                return json.dumps({"source": source, "store_diagnostics": diag}, indent=2)
            return json.dumps(
                {"store_diagnostics": {}, "note": "no aux diagnostics yet — call run_profile or run_quick_benchmark"}
            )
        for name in ("bench_summary.json", "optimization_state.json", "baseline_summary.json"):
            data = read_json(art / name)
            diag = data.get("store_diagnostics") or data.get("median_metrics", {}).get("store_diagnostics")
            if diag:
                return json.dumps({"source": name, "store_diagnostics": diag}, indent=2)
        return json.dumps({"store_diagnostics": {}, "note": "no diagnostics on disk yet"})

    return {
        "get_profile_digest": get_profile_digest,
        "get_optimization_evidence": get_optimization_evidence,
        "get_store_diagnostics": get_store_diagnostics,
        "begin_ab_trial": begin_ab_trial,
        "run_quick_benchmark": run_quick_benchmark,
        "end_ab_trial": end_ab_trial,
    }


def register(tool, call) -> None:
    @tool(read_only=True)
    def get_profile_digest() -> str:
        """Compact profile/baseline digest from artifacts."""
        return call("get_profile_digest")

    @tool(read_only=True)
    def get_optimization_evidence() -> str:
        """Optimization evidence / runtime packet."""
        return call("get_optimization_evidence")

    @tool(read_only=True)
    def get_store_diagnostics() -> str:
        """Aux store diagnostics if present."""
        return call("get_store_diagnostics")

    @tool(read_only=False)
    def begin_ab_trial(trial_id: str, hypothesis: str, hot_spot: str = "", expected_gain: str = "",
                       strategy: str = "") -> str:
        """Begin exploratory A/B trial (non-binding). Start from run_profile output: hot_spot names the
        hot function or loop it targets, expected_gain the gain you expect there, strategy is
        structural | low_level | other."""
        return call("begin_ab_trial", trial_id, hypothesis, hot_spot, expected_gain, strategy)

    @tool(read_only=False)
    def run_quick_benchmark(include_profile: bool = True, acceptance: bool = False) -> str:
        """Run the benchmark for the active A/B trial. Default: the quick tier, seconds, read axis only,
        direction not measurement. acceptance=True: the full optimization tier; end_ab_trial(keep=True)
        needs one."""
        return call("run_quick_benchmark", include_profile, acceptance)

    @tool(read_only=False)
    def end_ab_trial(trial_id: str, keep: bool = False) -> str:
        """End exploratory A/B trial."""
        return call("end_ab_trial", trial_id, keep)

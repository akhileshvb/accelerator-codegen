from __future__ import annotations

from accel_pipeline.config import ModelConfig, PipelineConfig, load_run_workload
from accel_pipeline.gates import GateFailure, GateResult, gate_failure_message
from accel_pipeline.layout import query_dir
from accel_pipeline.paths import query_artifacts_dir
from accel_pipeline.playbook import append_experiment_log
from accel_pipeline.stages.base import run_agent_stage
from accel_pipeline.stages.gate_plan import run_post_stage_gates_collect


def _gate_log_tail(gate_data: dict, *, max_chars: int = 6000) -> str:
    for key in ("log_tail", "stderr", "stdout", "notes", "reason"):
        value = gate_data.get(key)
        if value:
            text = str(value).strip()
            if text:
                return text[-max_chars:]
    return ""


def extract_crash_highlights(log_tail: str, *, max_lines: int = 40) -> list[str]:
    import re

    crash_signals = re.compile(
        r"(?i)(segmentation fault|sigsegv|sigabrt|internalexception|"
        r"assertion failed|stack trace|sanitizer|core dumped|"
        r"undefined reference|error:|fatal error:|terminate called|"
        r"tests passed.*failed|test.*failed)"
    )
    if not log_tail:
        return []
    lines = log_tail.splitlines()
    highlighted = [line.strip() for line in lines if crash_signals.search(line)]
    if highlighted:
        return highlighted[-max_lines:]
    return [line.strip() for line in lines[-max_lines:] if line.strip()]


def format_gate_failure_diagnosis(
    failed: GateResult,
    *,
    change_kind: str,
    iteration: int,
    reproduce_hint: str = "",
) -> str:
    data = failed.data
    log_tail = _gate_log_tail(data)
    highlights = extract_crash_highlights(log_tail)
    lines = [
        "## Gate failure diagnostics (fix before this iteration can complete)",
        "",
        f"- failed gate: `{failed.name}` (status={failed.status})",
        f"- context: {change_kind} iter {iteration}",
        "- **This stage/iteration is not finished** until mandatory gates pass (working prototype).",
    ]
    if data.get("exit_code") is not None:
        lines.append(f"- exit_code: {data.get('exit_code')}")
    if reproduce_hint:
        lines.append(f"- reproduce: `{reproduce_hint}`")
    if highlights:
        lines.append("")
        lines.append("### Likely failure lines")
        for item in highlights:
            lines.append(f"- `{item}`")
    if log_tail:
        lines.append("")
        lines.append("### Log tail")
        lines.append("```text")
        lines.append(log_tail[-4000:])
        lines.append("```")
    lines.append("")
    lines.append(
        "Diagnose like an interactive debug session: find root cause, patch, rebuild, "
        "and re-run the failing command locally. The pipeline will re-run gates automatically."
    )
    return "\n".join(lines)


def _crash_debug_limit(pipeline_config: PipelineConfig, round_idx: int, max_rounds: int | None = None) -> None:
    """Raise when crash-debug rounds exceed configured limits (`max_rounds` overrides the configured one)."""
    if max_rounds is None:
        max_rounds = int(getattr(pipeline_config, "crash_debug_max_rounds", 0))
    max_rounds = max(0, max_rounds)
    safety_cap = max(1, int(getattr(pipeline_config, "crash_debug_safety_cap", 25)))
    if max_rounds > 0 and round_idx >= max_rounds:
        raise GateFailure(
            f"crash debug exhausted max_rounds={max_rounds} for this stage/iteration "
            "(mandatory gates still failing)"
        )
    if max_rounds == 0 and round_idx >= safety_cap:
        raise GateFailure(
            f"crash debug safety_cap={safety_cap} reached for this stage/iteration "
            "(mandatory gates still failing)"
        )


def run_post_stage_gates_with_crash_debug(
    pipeline_config: PipelineConfig,
    model_config: ModelConfig,
    workload_name: str,
    query: int,
    gate_names: list[str],
    *,
    change_kind: str = "",
    iteration: int = 0,
    model_overrides: dict[str, str] | None = None,
    dry_run: bool = False,
    max_rounds: int | None = None,
) -> list[GateResult]:
    """Run gates; on failure invoke crash-debug agent turns until gates pass (at most `max_rounds`)."""
    if dry_run:
        return []

    workload = load_run_workload(pipeline_config, workload_name)
    reproduce = (
        f"cmake --build build-release --target {workload.get('test_target', 'test')} -j$(nproc) && "
        f"ctest -R {workload.get('ctest_smoke_regex', '')}"
    )

    last_results: list[GateResult] = []
    round_idx = 0
    while True:
        last_results, failed = run_post_stage_gates_collect(
            pipeline_config,
            workload_name,
            query,
            gate_names,
            change_kind=change_kind,
            iteration=iteration,
        )
        if failed is None:
            if round_idx > 0:
                append_experiment_log(
                    pipeline_config.repo_root,
                    query,
                    f"- crash debug recovered after {round_idx} round(s) "
                    f"({change_kind} iter {iteration}): mandatory gates now pass",
                    workload=workload,
                )
            return last_results

        round_idx += 1
        diagnosis = format_gate_failure_diagnosis(
            failed,
            change_kind=change_kind,
            iteration=iteration,
            reproduce_hint=reproduce,
        )
        art = query_artifacts_dir(pipeline_config.repo_root, query, workload=workload)
        artifact = art / f"crash_debug_{change_kind}_iter{iteration}_round{round_idx}.md"
        artifact.write_text(diagnosis, encoding="utf-8")
        append_experiment_log(
            pipeline_config.repo_root,
            query,
            f"- crash debug round {round_idx} after {change_kind} iter {iteration}: "
            f"gate `{failed.name}` failed — see {artifact.name}",
            workload=workload,
        )

        _crash_debug_limit(pipeline_config, round_idx, max_rounds)

        run_agent_stage(
            stage="crash_debug",
            prompt_file="04_crash_debug.md",
            pipeline_config=pipeline_config,
            model_config=model_config,
            workload_name=workload_name,
            query=query,
            user_message=diagnosis,
            model_overrides=model_overrides,
            optimization_iter=iteration,
            must_change=pipeline_config.repo_root / query_dir(str(workload.get("benchmark_name", "tpch")), query),
        )

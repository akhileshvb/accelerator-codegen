"""Build/correctness fix loop for optimization attempts (mirrors the repair loop)."""

from __future__ import annotations

import sys
from typing import Any

from accel_pipeline.config import PipelineConfig
from accel_pipeline.workload import query_artifacts_dir
from accel_pipeline.failure import extract_crash_highlights
from accel_pipeline.optimization.smoke import SmokeResult
from accel_pipeline.playbook import append_experiment_log


def _tail(text: str, max_chars: int) -> str:
    text = (text or "").strip()
    if not text:
        return ""
    return text[-max_chars:]


def format_build_failure_diagnosis(
    build_out: str,
    *,
    optimization_iter: int,
    attempt: int,
    fix_round: int,
    reproduce_cmd: str,
    max_log_chars: int = 6000,
) -> str:
    log_tail = _tail(build_out, max_log_chars)
    highlights = extract_crash_highlights(log_tail)
    lines = [
        "## Build failure (fix before benchmark)",
        "",
        f"- context: optimization iter {optimization_iter}, attempt {attempt}, fix round {fix_round}",
        "- **Your hypothesis patch is still active** — fix the compile/build error; do not start a new redesign.",
        f"- reproduce: `{reproduce_cmd}`",
    ]
    if highlights:
        lines.append("")
        lines.append("### Likely failure lines")
        for item in highlights:
            lines.append(f"- `{item}`")
    if log_tail:
        lines.append("")
        lines.append("### Build log tail")
        lines.append("```text")
        lines.append(log_tail[-4000:])
        lines.append("```")
    lines.append("")
    lines.append(
        "Diagnose root cause, patch the minimum files, then rebuild in this worker's build dir. "
        "The orchestrator will re-run build automatically after you finish."
    )
    return "\n".join(lines)


def format_smoke_failure_diagnosis(
    smoke: SmokeResult,
    *,
    optimization_iter: int,
    attempt: int,
    fix_round: int,
    reproduce_cmd: str,
    max_log_chars: int = 6000,
) -> str:
    log_tail = _tail(smoke.output, max_log_chars)
    highlights = extract_crash_highlights(log_tail)
    lines = [
        "## Quick correctness failure (fix before benchmark)",
        "",
        f"- context: optimization iter {optimization_iter}, attempt {attempt}, fix round {fix_round}",
        f"- failed smoke: `ctest -R {smoke.regex}`",
        "- **Your hypothesis patch is still active** — fix correctness; do not start a new redesign.",
        f"- reproduce: `{reproduce_cmd}`",
    ]
    if highlights:
        lines.append("")
        lines.append("### Likely failure lines")
        for item in highlights:
            lines.append(f"- `{item}`")
    if log_tail:
        lines.append("")
        lines.append("### Test output tail")
        lines.append("```text")
        lines.append(log_tail[-4000:])
        lines.append("```")
    lines.append("")
    lines.append(
        "Find why the smoke test failed (assertion, wrong result, segfault), patch, "
        "then rebuild and re-run the smoke test in this worker's build dir."
    )
    return "\n".join(lines)


def optimization_fix_limit(pipeline_config: PipelineConfig, round_idx: int, *, kind: str) -> None:
    if kind == "build":
        max_rounds = max(0, int(getattr(pipeline_config, "optimization_build_fix_max_rounds", 0) or 0))
        safety_cap = max(1, int(getattr(pipeline_config, "optimization_build_fix_safety_cap", 12) or 12))
    else:
        max_rounds = max(0, int(getattr(pipeline_config, "optimization_correctness_fix_max_rounds", 0) or 0))
        safety_cap = max(1, int(getattr(pipeline_config, "optimization_correctness_fix_safety_cap", 12) or 12))
    if max_rounds > 0 and round_idx >= max_rounds:
        raise RuntimeError(
            f"optimization {kind} fix exhausted max_rounds={max_rounds} "
            f"(attempt still failing after {round_idx} fix rounds)"
        )
    if max_rounds == 0 and round_idx >= safety_cap:
        raise RuntimeError(
            f"optimization {kind} fix safety_cap={safety_cap} reached "
            f"(attempt still failing after {round_idx} fix rounds)"
        )


def log_fix_round(
    pipeline_config: PipelineConfig,
    workload: dict[str, Any],
    query: int,
    *,
    optimization_iter: int,
    attempt: int,
    kind: str,
    fix_round: int,
    diagnosis: str,
) -> None:
    art = query_artifacts_dir(pipeline_config.repo_root, query, workload=workload)
    artifact = art / f"optimization_fix_{kind}_iter{optimization_iter}_a{attempt}_r{fix_round}.md"
    artifact.write_text(diagnosis, encoding="utf-8")
    append_experiment_log(
        pipeline_config.repo_root,
        query,
        f"- optimization {kind} fix round {fix_round} (iter {optimization_iter} attempt {attempt}): "
        f"see {artifact.name}",
        workload=workload,
    )
    print(
        f"[pipeline] optimization {kind} fix round {fix_round} "
        f"(iter {optimization_iter} attempt {attempt})",
        file=sys.stderr,
        flush=True,
    )

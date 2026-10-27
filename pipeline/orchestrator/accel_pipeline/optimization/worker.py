"""Persistent in-memory optimization worker for Codex-like candidate cycles."""

from __future__ import annotations

import re
import sys
import time
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any

from accel_pipeline.codex import StageRunRequest, create_agent_backend
from accel_pipeline.config import (
    ModelConfig,
    PipelineConfig,
    make_model_id,
    apply_workload_overrides,
    load_run_workload,
    query_artifacts_dir,
)
from accel_pipeline.optimization.pending import PendingFeedback
from accel_pipeline.optimization.saturation import (
    OPTIMIZATION_SATURATED,
    parse_optimization_saturated,
)
from accel_pipeline.optimization.worker_checkpoint import (
    WORKER_SEED_FILE,
    build_semantic_worker_checkpoint,
    load_worker_checkpoint,
)
from accel_pipeline.optimization.worker_metrics import WorkerMetrics, save_worker_metrics
from accel_pipeline.runlog import log_event
from accel_pipeline.session import MAIN, run_in_session
from accel_pipeline.prompts import render_prompt, save_rendered_prompt, with_codex_tooling

CANDIDATE_READY = "CANDIDATE_READY"
CANNOT_TEST = "CANNOT_TEST"
_WORKER_STAGE = "optimization_worker"
_UNLIMITED_TURNS = 10_000


@dataclass
class CandidateCycleResult:
    status: str  # ready | cannot_test | failed | saturated
    hypothesis: dict[str, Any] | None = None
    reason: str = ""
    final_text: str = ""
    tool_calls: int = 0
    wall_clock_ms: int = 0
    saturation: dict[str, Any] | None = None


@dataclass
class EpisodeMetadata:
    optimization_iter: int = 0
    attempt: int = 0


def resolve_worker_max_turns(pipeline_config: PipelineConfig) -> int:
    raw = int(getattr(pipeline_config, "optimization_worker_max_turns_per_candidate", 0) or 0)
    if raw <= 0:
        cap = int(getattr(pipeline_config, "optimization_worker_safety_cap_turns", 80) or 80)
        return cap if cap > 0 else _UNLIMITED_TURNS
    return raw


def _parse_hypothesis_json(text: str) -> dict[str, Any] | None:
    if not text:
        return None
    fence = re.search(r"```(?:json)?\s*(\{.*?\})\s*```", text, re.DOTALL)
    if fence:
        try:
            import json

            return json.loads(fence.group(1))
        except json.JSONDecodeError:
            pass
    for match in re.finditer(r"\{[^{}]*(?:\{[^{}]*\}[^{}]*)*\}", text, re.DOTALL):
        try:
            import json

            payload = json.loads(match.group(0))
            if isinstance(payload, dict) and payload.get("bottleneck"):
                return payload
        except json.JSONDecodeError:
            continue
    return None


def _parse_cannot_test(text: str) -> str | None:
    if CANNOT_TEST not in text:
        return None
    fence = re.search(r"```(?:json)?\s*(\{.*?\})\s*```", text, re.DOTALL)
    if fence:
        try:
            import json

            payload = json.loads(fence.group(1))
            if payload.get("status") == "cannot_test":
                return str(payload.get("reason", "cannot test hypothesis"))
        except json.JSONDecodeError:
            pass
    return "worker returned CANNOT_TEST"


@dataclass
class OptimizationWorker:
    pipeline_config: PipelineConfig
    model_config: ModelConfig
    workload_name: str
    query: int
    workload: dict[str, Any]
    art_dir: Path
    backend: Any
    instructions: str
    provider: str
    pending: PendingFeedback = field(default_factory=PendingFeedback)
    metrics: WorkerMetrics = field(default_factory=WorkerMetrics)
    episode_meta: EpisodeMetadata = field(default_factory=EpisodeMetadata)
    seeded: bool = False
    seed_message: str = ""
    model_overrides: dict[str, str] | None = None

    @classmethod
    def create(
        cls,
        *,
        pipeline_config: PipelineConfig,
        model_config: ModelConfig,
        workload_name: str,
        query: int,
        model_overrides: dict[str, str] | None = None,
        resume: bool = False,
    ) -> OptimizationWorker:
        from accel_pipeline.config import resolve_stage_provider

        workload = load_run_workload(pipeline_config, workload_name)
        pipeline_config = apply_workload_overrides(pipeline_config, workload)
        art_dir = query_artifacts_dir(pipeline_config.repo_root, query, workload=workload)
        provider = resolve_stage_provider(_WORKER_STAGE, model_config)

        prompt = render_prompt(
            pipeline_config,
            "03_optimization_worker.md",
            workload,
            query,
            _WORKER_STAGE,
            optimization_iter=1,
        )
        save_rendered_prompt(pipeline_config, workload, query, _WORKER_STAGE, prompt, optimization_iter=1)
        instructions = with_codex_tooling(pipeline_config, prompt)

        backend = create_agent_backend(
            provider=provider,
            pipeline_config=pipeline_config,
            query=query,
            stage=_WORKER_STAGE,
            model_config=model_config,
            workload_name=workload_name,
            optimization_iter=1,
        )

        pending = PendingFeedback()
        metrics = WorkerMetrics()
        seeded = False

        if resume:
            ckpt = load_worker_checkpoint(
                art_dir,
                getattr(pipeline_config, "optimization_checkpoint_file", "optimization_worker_checkpoint.json"),
            )
            if ckpt:
                pending = PendingFeedback.from_dict(ckpt)
                metrics = WorkerMetrics.from_dict(ckpt)
                seeded = bool(ckpt.get("seeded", False))
                log_event(
                    pipeline_config.repo_root,
                    query,
                    workload,
                    "optimization_worker_episode_resume",
                    reconstructed_from_artifacts=True,
                )

        worker = cls(
            pipeline_config=pipeline_config,
            model_config=model_config,
            workload_name=workload_name,
            query=query,
            workload=workload,
            art_dir=art_dir,
            backend=backend,
            instructions=instructions,
            provider=provider,
            pending=pending,
            metrics=metrics,
            seeded=seeded,
            model_overrides=model_overrides,
        )
        log_event(
            pipeline_config.repo_root,
            query,
            workload,
            "optimization_worker_episode_start",
            provider=provider,
            seed_chars=len(worker.seed_message),
        )
        return worker

    def seed_episode(self, seed_message: str) -> None:
        self.seed_message = seed_message.strip()
        self.seeded = True
        seed_path = self.art_dir / WORKER_SEED_FILE
        seed_path.write_text(self.seed_message, encoding="utf-8")
        log_event(
            self.pipeline_config.repo_root,
            self.query,
            self.workload,
            "optimization_worker_episode_start",
            seeded=True,
            seed_chars=len(self.seed_message),
        )

    def _run(self, request: StageRunRequest) -> Any:
        """Candidate cycles continue the stage's main codex session (instructions sent once)."""
        return run_in_session(self.backend, request, pipeline_config=self.pipeline_config, art=self.art_dir,
                              query=self.query, role=MAIN)

    def run_candidate_cycle(self, user_message: str) -> CandidateCycleResult:
        model_id = make_model_id(_WORKER_STAGE, self.pipeline_config, self.model_config, self.model_overrides)
        agent_model = model_id
        max_turns = resolve_worker_max_turns(self.pipeline_config)

        log_event(
            self.pipeline_config.repo_root,
            self.query,
            self.workload,
            "optimization_worker_candidate_start",
            iter=self.episode_meta.optimization_iter,
            attempt=self.episode_meta.attempt,
            pending_feedback_chars=len(self.pending.peek()),
        )

        request = StageRunRequest(
            stage=_WORKER_STAGE,
            instructions=self.instructions,
            user_message=user_message,
            agent_name=f"accel_{_WORKER_STAGE}",
            model_id=model_id,
            agent_model=agent_model,
            max_turns=max_turns,
        )

        started = time.monotonic()
        result = self._run(request)

        # Codex often exits after edits/tests without printing CANDIDATE_READY.
        # Give one short follow-up attempt so the cycle can still complete.
        if (
            result.status == "finished"
            and CANDIDATE_READY not in (result.final_text or "")
            and OPTIMIZATION_SATURATED not in (result.final_text or "")
            and not _parse_cannot_test(result.final_text or "")
        ):
            log_event(
                self.pipeline_config.repo_root,
                self.query,
                self.workload,
                "optimization_worker_yield_nudge",
                attempt=self.episode_meta.attempt,
            )
            nudge = StageRunRequest(
                stage=_WORKER_STAGE,
                instructions=self.instructions,
                user_message=(
                    "Your previous optimization attempt ended without a required yield marker.\n"
                    "Do not start a new redesign. Based on the current tree state:\n"
                    "1. Briefly confirm build/smoke already done (or run MCP `run_correctness` / shell ctest if unsure).\n"
                    "2. Call MCP `get_profile_digest` then, if you edited, `begin_ab_trial`, `run_quick_benchmark(acceptance=True)` and\n"
                    "   `end_ab_trial` (`keep=true` needs that acceptance run).\n"
                    "   Do not yield after `ab_signal=regress` / `forced_discard`.\n"
                    "3. Print hypothesis JSON then end with exactly `CANDIDATE_READY`,\n"
                    "   or `CANNOT_TEST` with JSON reason,\n"
                    "   or `OPTIMIZATION_SATURATED` with JSON if you already pushed hard and "
                    "further attempts are stuck (no target speedup number).\n"
                    "Exiting without that marker fails this attempt."
                ),
                agent_name=f"accel_{_WORKER_STAGE}",
                model_id=model_id,
                agent_model=agent_model,
                max_turns=max_turns,
            )
            nudge_result = self._run(nudge)
            if nudge_result.status == "finished" and (
                CANDIDATE_READY in (nudge_result.final_text or "")
                or OPTIMIZATION_SATURATED in (nudge_result.final_text or "")
                or _parse_cannot_test(nudge_result.final_text or "")
            ):
                result = nudge_result
            elif nudge_result.final_text:
                # Prefer combined text so a marker in either segment counts.
                combined = f"{result.final_text or ''}\n{nudge_result.final_text}"
                result.final_text = combined
                if nudge_result.status == "finished":
                    result.status = "finished"

        wall_ms = int((time.monotonic() - started) * 1000)
        tool_calls = int((result.metadata or {}).get("tool_calls", 0) or 0)
        usage = (result.metadata or {}).get("usage")
        self.metrics.record_run(tool_calls=tool_calls, usage=usage)

        final_text = result.final_text or ""

        marker = "failed"
        saturated = parse_optimization_saturated(final_text)
        if result.status != "finished":
            cycle = CandidateCycleResult(
                status="failed",
                reason=result.error or result.status,
                final_text=final_text,
                tool_calls=tool_calls,
                wall_clock_ms=wall_ms,
            )
        elif saturated is not None:
            # Prefer saturation over CANDIDATE_READY when both appear.
            marker = "saturated"
            cycle = CandidateCycleResult(
                status="saturated",
                hypothesis=_parse_hypothesis_json(final_text),
                reason=str(saturated.get("reason") or "optimization_saturated"),
                final_text=final_text,
                tool_calls=tool_calls,
                wall_clock_ms=wall_ms,
                saturation=saturated,
            )
        elif _parse_cannot_test(final_text):
            marker = "cannot_test"
            cycle = CandidateCycleResult(
                status="cannot_test",
                reason=_parse_cannot_test(final_text) or "cannot_test",
                final_text=final_text,
                tool_calls=tool_calls,
                wall_clock_ms=wall_ms,
            )
        elif CANDIDATE_READY not in final_text:
            cycle = CandidateCycleResult(
                status="failed",
                reason="missing CANDIDATE_READY",
                hypothesis=_parse_hypothesis_json(final_text),
                final_text=final_text,
                tool_calls=tool_calls,
                wall_clock_ms=wall_ms,
            )
        else:
            marker = "ready"
            hypothesis = _parse_hypothesis_json(final_text)
            if not hypothesis:
                hypothesis = {"bottleneck": "optimization_worker_patch", "why": "no hypothesis JSON"}
            cycle = CandidateCycleResult(
                status="ready",
                hypothesis=hypothesis,
                final_text=final_text,
                tool_calls=tool_calls,
                wall_clock_ms=wall_ms,
            )

        fail_snip = ""
        if marker == "failed":
            raw_reason = (cycle.reason or result.error or "")[:500]
            fail_snip = " ".join(raw_reason.split())[:240]
            if fail_snip:
                print(
                    f"[pipeline] optimization_worker_candidate_failed: {fail_snip}",
                    file=sys.stderr,
                    flush=True,
                )
        log_event(
            self.pipeline_config.repo_root,
            self.query,
            self.workload,
            "optimization_worker_candidate_end",
            runner_invocation_idx=self.metrics.runner_invocations,
            tool_calls=tool_calls,
            wall_clock_ms=wall_ms,
            input_tokens=(usage or {}).get("input_tokens") if usage else None,
            output_tokens=(usage or {}).get("output_tokens") if usage else None,
            cache_read_tokens=(usage or {}).get("cache_read_tokens") if usage else None,
            marker=marker,
            failure_snip=fail_snip or None,
        )
        return cycle

    def save_checkpoint(self) -> None:
        ckpt_file = getattr(
            self.pipeline_config, "optimization_checkpoint_file", "optimization_worker_checkpoint.json"
        )
        payload = build_semantic_worker_checkpoint(
            self.art_dir,
            provider=self.provider,
            optimization_iter=self.episode_meta.optimization_iter,
            attempt=self.episode_meta.attempt,
            seeded=self.seeded,
            pending=self.pending,
            runner_invocations=self.metrics.runner_invocations,
            episode_tool_calls=self.metrics.episode_tool_calls,
            checkpoint_file=ckpt_file,
        )
        log_event(
            self.pipeline_config.repo_root,
            self.query,
            self.workload,
            "optimization_worker_checkpoint_saved",
            checkpoint_path=str(self.art_dir / ckpt_file),
        )
        return payload

    def close(self) -> None:
        save_worker_metrics(self.art_dir, self.metrics)
        log_event(
            self.pipeline_config.repo_root,
            self.query,
            self.workload,
            "optimization_worker_episode_end",
            runner_invocations=self.metrics.runner_invocations,
            tool_calls=self.metrics.episode_tool_calls,
            input_tokens=self.metrics.input_tokens,
            cache_read_tokens=self.metrics.cache_read_tokens,
        )


def worker_enabled(pipeline_config: PipelineConfig) -> bool:
    return bool(getattr(pipeline_config, "optimization_persistent_worker", True))

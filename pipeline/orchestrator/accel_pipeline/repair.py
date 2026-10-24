"""The repair loop: no stage failure, gate failure, bench failure, timeout or exception ends a run
uncaught. A failure is triaged; infra failures are retried (and, when persistent, quarantined),
code failures resume the agent's session with the failure report, and once that budget is spent
the tree goes back to the best snapshot, the plan is redone once, and the run stops cleanly."""

from __future__ import annotations

import json
import sys
import time
import traceback
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any, Callable, TypeVar

from accel_pipeline.config import ModelConfig, PipelineConfig, load_run_workload
from accel_pipeline.failure import PROVIDER_FLAKE, Failure, StageFailure, failure_from_gate, render_failure
from accel_pipeline.gate_parallel import first_failure, run_gates_parallel
from accel_pipeline.gates import GateFailure, GateResult
from accel_pipeline.layout import query_dir
from accel_pipeline.paths import query_artifacts_dir
from accel_pipeline.playbook import append_experiment_log
from accel_pipeline.session import MAIN, SessionStore
from accel_pipeline.stages.gate_plan import run_post_stage_gates_collect
from accel_pipeline.triage import classify, failure_text

T = TypeVar("T")

REPAIR_STATE_FILE = "repair_state.json"
QUARANTINE_FILE = "quarantine.json"
STOP_REPORT_FILE = "stop_report.md"
LAST_FAILURE_FILE = "last_failure.md"
EXIT_STOPPED = 3

_REPLANNABLE = frozenset({"implementation", "optimization"})


class StageStopped(Exception):
    """The repair budget is spent: the run stops here, recorded, and `--resume` continues."""

    def __init__(self, reason: str, report: Path):
        super().__init__(f"{reason} (see {report})")
        self.reason = reason
        self.report = report


@dataclass
class RepairCtx:
    pipeline_config: PipelineConfig
    model_config: ModelConfig
    workload_name: str
    query: int
    stage: str
    change_kind: str = ""
    iteration: int = 0
    model_overrides: dict[str, str] | None = None
    max_code_rounds: int | None = None
    repair_turn: Callable[[Failure], None] | None = None  # replaces the crash-debug turn (engine flows)
    gate_runner: Callable[[list[str]], tuple[list[GateResult], GateResult | None]] | None = None
    results: list[GateResult] = field(default_factory=list)
    workload: dict[str, Any] = field(init=False)
    art: Path = field(init=False)

    def __post_init__(self) -> None:
        self.workload = load_run_workload(self.pipeline_config, self.workload_name)
        self.art = query_artifacts_dir(self.pipeline_config.repo_root, self.query, workload=self.workload)

    def log(self, line: str) -> None:
        print(f"[repair] {line}", file=sys.stderr, flush=True)
        append_experiment_log(self.pipeline_config.repo_root, self.query,
                              f"- repair[{self.stage} {self.change_kind} iter {self.iteration}]: {line}",
                              workload=self.workload)


def run_with_repair(ctx: RepairCtx, attempt: Callable[[], T], *, gates: list[str] | None = None) -> T:
    """`attempt()` once, then `gates`; on any failure, repair rounds until it all passes.

    After a repair round the work is re-checked by the gates, or by calling `attempt` again when
    there are none. Returns attempt's result, or raises StageStopped (never anything else)."""
    cfg = ctx.pipeline_config.repair
    code_left = cfg.max_code_rounds if ctx.max_code_rounds is None else ctx.max_code_rounds
    code_rounds = code_left
    infra_used: dict[str, int] = {}
    orchestrator_left = cfg.max_orchestrator_retries
    flakes = 0
    quarantined: set[str] = set()
    rolled_back = False
    result: Any = None
    run_attempt = True

    while True:
        failure, from_attempt = None, False
        if run_attempt or not gates:
            result, failure = _attempt(ctx, attempt)
            run_attempt, from_attempt = False, failure is not None
        if failure is None and gates:
            failure = _check_gates(ctx, gates, quarantined)
        if failure is None:
            return result

        _record(ctx, failure)
        if failure.infra_reason == PROVIDER_FLAKE:
            flakes += 1
            if flakes > cfg.provider_retries:
                raise stop_stage(ctx, failure, f"the provider failed {flakes} times in a row")
            _wait_out_flake(ctx, flakes)
            run_attempt = from_attempt
            continue
        flakes = 0
        if failure.kind == "infra":
            used = infra_used[failure.source] = infra_used.get(failure.source, 0) + 1
            if used <= cfg.infra_retries:
                _clear_infra(ctx, failure, used)
                run_attempt = from_attempt
                continue
            gate = failure.source.removeprefix("gate:")
            if failure.source.startswith("gate:") and gate not in cfg.never_quarantine:
                _quarantine(ctx, gate, failure)
                quarantined.add(gate)
                continue
            raise stop_stage(ctx, failure, f"infra failure persists: {failure.infra_reason or failure.source}")
        if failure.kind == "orchestrator":
            if orchestrator_left <= 0:
                raise stop_stage(ctx, failure, f"orchestrator failure: {failure.summary}")
            orchestrator_left -= 1
            ctx.log(f"orchestrator failure ({failure.summary}); retrying once")
            run_attempt = from_attempt
            continue

        if code_left > 0:
            code_left -= 1
            ctx.log(f"{failure.kind} failure in {failure.source}; round {code_rounds - code_left}/{code_rounds}, "
                    "resuming the agent's session")
            repair_turn(ctx, failure)
            continue
        if not rolled_back:
            rolled_back = True
            ctx.log(f"{failure.kind} failure in {failure.source} after {code_rounds} rounds; rolling back")
            _rollback(ctx)
            if gates:
                continue
        if _replan(ctx, failure):
            code_left, rolled_back = code_rounds, False
            continue
        raise stop_stage(ctx, failure, f"{failure.kind} failure in {failure.source} survived {code_rounds} repair rounds")


def _attempt(ctx: RepairCtx, attempt: Callable[[], T]) -> tuple[T | None, Failure | None]:
    try:
        return attempt(), None
    except StageStopped:
        raise
    except StageFailure as exc:
        return None, exc.failure
    except GateFailure as exc:
        return None, _failure_from_text(ctx, "gate", str(exc))
    except Exception as exc:  # noqa: BLE001 - the loop's contract: nothing else escapes
        return None, Failure(kind="orchestrator", stage=ctx.stage, source="exception",
                             summary=f"{type(exc).__name__}: {exc}", evidence=traceback.format_exc())


def _failure_from_text(ctx: RepairCtx, source: str, text: str) -> Failure:
    kind, reason = classify(source, {"log_tail": text}, ctx.workload, ctx.pipeline_config.repo_root,
                            query=ctx.query, load_factor=ctx.pipeline_config.repair.load_factor)
    return Failure(kind=kind, stage=ctx.stage, source=source, summary=text.splitlines()[0][:300] if text else source,  # type: ignore[arg-type]
                   evidence=text, infra_reason=reason)


def _check_gates(ctx: RepairCtx, gates: list[str], quarantined: set[str]) -> Failure | None:
    names = [g for g in gates if g not in quarantined]
    try:
        ctx.results, failed = _run_gates(ctx, names)
    except Exception as exc:  # noqa: BLE001
        return Failure(kind="orchestrator", stage=ctx.stage, source="gate_runner",
                       summary=f"{type(exc).__name__}: {exc}", evidence=traceback.format_exc())
    if failed is None:
        return None
    return failure_from_gate(failed, stage=ctx.stage, workload=ctx.workload,
                             repo_root=ctx.pipeline_config.repo_root, load_factor=ctx.pipeline_config.repair.load_factor)


def _run_gates(ctx: RepairCtx, names: list[str]) -> tuple[list[GateResult], GateResult | None]:
    """The gates, skipping any that already passed on identical code; the results and the failing one."""
    if ctx.gate_runner is not None:
        return ctx.gate_runner(names)
    cfg = ctx.pipeline_config
    kwargs = {"change_kind": ctx.change_kind, "iteration": ctx.iteration, "reuse": True}
    if cfg.quick_tier.parallel_gates:
        results = run_gates_parallel(cfg, names, ctx.query, ctx.workload, **kwargs)
        return results, first_failure(results)
    return run_post_stage_gates_collect(cfg, ctx.workload_name, ctx.query, names, **kwargs)


def _record(ctx: RepairCtx, failure: Failure) -> None:
    (ctx.art / LAST_FAILURE_FILE).write_text(render_failure(failure), encoding="utf-8")


def _clear_infra(ctx: RepairCtx, failure: Failure, used: int) -> None:
    """Infra failures cost no code budget: repair the shared tree if that is the cause, wait out a
    provider flake, and let the caller re-run."""
    ctx.log(f"infra failure ({failure.infra_reason}) in {failure.source}; retry {used} without the agent")
    from accel_pipeline.infra_fail_fast import handle_infra_failure

    handle_infra_failure(ctx.pipeline_config, ctx.workload, ctx.query, text=failure_text({"log_tail": failure.evidence}))


def _wait_out_flake(ctx: RepairCtx, retry: int) -> None:
    wait = ctx.pipeline_config.repair.provider_backoff(retry)
    ctx.log(f"provider flake; retry {retry}/{ctx.pipeline_config.repair.provider_retries} in {wait:.0f}s")
    time.sleep(wait)


def _quarantine(ctx: RepairCtx, gate: str, failure: Failure) -> None:
    path = ctx.art / QUARANTINE_FILE
    records = json.loads(path.read_text(encoding="utf-8")) if path.is_file() else []
    records.append({"gate": gate, "stage": ctx.stage, "change_kind": ctx.change_kind, "iteration": ctx.iteration,
                    "infra_reason": failure.infra_reason, "summary": failure.summary, "at": time.strftime("%FT%T")})
    path.write_text(json.dumps(records, indent=2), encoding="utf-8")
    ctx.log(f"gate `{gate}` quarantined after repeated infra failure ({failure.infra_reason}); "
            "not rerun in this batch")


def repair_turn(ctx: RepairCtx, failure: Failure, *, must_change: bool | None = None) -> None:
    """One agent turn, in the stage's own session, on the failure report; a provider flake re-runs it.
    must_change: the turn has to edit the query's files (default: when the failure is a code failure)."""
    if ctx.stage != "test_suite" and _revise_test_suite(ctx):
        return
    turn = ctx.repair_turn or _crash_debug_turn(ctx, failure.kind == "code" if must_change is None else must_change)
    retries = ctx.pipeline_config.repair.provider_retries
    for flake in range(1, retries + 2):
        try:
            turn(failure)
            return
        except StageStopped:
            raise
        except StageFailure as exc:
            if exc.failure.infra_reason == PROVIDER_FLAKE and flake <= retries:
                _wait_out_flake(ctx, flake)
                continue
            ctx.log(f"the repair turn itself failed ({exc.failure.kind}: {exc.failure.summary}); "
                    "the next check judges the tree")
        except Exception as exc:  # noqa: BLE001
            ctx.log(f"the repair turn raised {type(exc).__name__}: {exc}; the next check judges the tree")
        return


def _revise_test_suite(ctx: RepairCtx) -> bool:
    """A change request against the frozen test suite goes to the tester before the implementation is touched again."""
    from accel_pipeline import test_suite as suite
    from accel_pipeline.stages import test_suite

    if not suite.pending_change_request(ctx.art):
        return False
    test_suite.revise(ctx.pipeline_config, ctx.model_config, ctx.workload_name, ctx.query, ctx.model_overrides)
    ctx.log("the tester handled a test-suite change request; the next check judges the suite it froze")
    return True


def _crash_debug_turn(ctx: RepairCtx, must_change: bool) -> Callable[[Failure], None]:
    from accel_pipeline.stages.base import run_agent_stage

    bench = str(ctx.workload.get("benchmark_name", "tpch"))

    def turn(failure: Failure) -> None:
        run_agent_stage(
            stage="crash_debug",
            prompt_file="04_crash_debug.md",
            pipeline_config=ctx.pipeline_config,
            model_config=ctx.model_config,
            workload_name=ctx.workload_name,
            query=ctx.query,
            user_message=render_failure(failure),
            model_overrides=ctx.model_overrides,
            optimization_iter=ctx.iteration,
            must_change=ctx.pipeline_config.repo_root / query_dir(bench, ctx.query) if must_change else None,
            session_role=MAIN,
        )

    return turn


def _rollback(ctx: RepairCtx) -> None:
    from accel_pipeline.kernel_snapshot import has_best_kernel_snapshot, rebuild_workload_targets, restore_best_kernel

    if not has_best_kernel_snapshot(ctx.art):
        ctx.log("no best snapshot to roll back to")
        return
    manifest = restore_best_kernel(ctx.pipeline_config, ctx.workload, ctx.query)
    if manifest is not None:
        rebuild_workload_targets(ctx.pipeline_config, ctx.workload, ctx.query)
        ctx.log(f"restored best snapshot ({manifest.get('speedup_median', '?')}x, "
                f"{manifest.get('change_kind', '?')} iter {manifest.get('iteration', '?')})")


def _replan_budget(ctx: RepairCtx) -> int:
    path = ctx.art / REPAIR_STATE_FILE
    used = int(json.loads(path.read_text(encoding="utf-8")).get("replans", 0)) if path.is_file() else 0
    return ctx.pipeline_config.repair.max_replans - used


def _replan(ctx: RepairCtx, failure: Failure) -> bool:
    """One structural re-plan (analysis, tests, implementation again, in a fresh session) for the whole run."""
    if ctx.stage not in _REPLANNABLE or _replan_budget(ctx) <= 0:
        return False
    from accel_pipeline import run_pipeline, structural_reanalysis
    from accel_pipeline.state import PipelineState, state_path

    path = ctx.art / REPAIR_STATE_FILE
    used = ctx.pipeline_config.repair.max_replans - _replan_budget(ctx)
    path.write_text(json.dumps({"replans": used + 1}), encoding="utf-8")
    ctx.log("repair budget spent; re-planning once (analysis, tests, implementation) in a fresh session")
    SessionStore(ctx.art).reset(ctx.query, MAIN)
    try:
        structural_reanalysis.write_request(
            ctx.pipeline_config.repo_root, ctx.query, ctx.workload,
            source_stage=ctx.stage, source_iteration=ctx.iteration, bottleneck_type="other",
            evidence=f"Repair rounds could not fix the failure below.\n{render_failure(failure)}"[:4000],
            proposed_direction="Choose a design that the failure above shows is implementable: simpler state, "
                               "fewer moving parts, or a different candidate from the search.",
            current_candidate_id=_selected_candidate(ctx),
        )
        state = PipelineState.load(state_path(ctx.pipeline_config.repo_root, ctx.query, ctx.workload_name,
                                              ctx.pipeline_config.engine_name))
        next_idx = run_pipeline._maybe_process_structural_reanalysis(
            triggered_after="implementation", pipeline_config=ctx.pipeline_config, model_config=ctx.model_config,
            workload_name=ctx.workload_name, workload=ctx.workload, query=ctx.query,
            model_overrides=ctx.model_overrides or {}, dry_run=False, state=state)
    except StageStopped:
        return False
    except Exception as exc:  # noqa: BLE001
        ctx.log(f"re-plan failed: {type(exc).__name__}: {exc}")
        return False
    if next_idx is None:
        ctx.log("re-plan not possible (structural re-analysis budget or workload kind)")
    return next_idx is not None


def _selected_candidate(ctx: RepairCtx) -> str:
    from accel_pipeline.paths import analysis_artifact_path

    try:
        analysis = json.loads(analysis_artifact_path(ctx.pipeline_config.repo_root, ctx.query,
                                                     workload=ctx.workload).read_text(encoding="utf-8"))
        return str(analysis["correctness_facts"]["state_design"]["selected_candidate_id"]) or "current"
    except (OSError, KeyError, TypeError, json.JSONDecodeError):
        return "current"


def stop_stage(ctx: RepairCtx, failure: Failure, reason: str) -> StageStopped:
    report = ctx.art / STOP_REPORT_FILE
    report.write_text(
        f"# Stopped: {reason}\n\nStage `{ctx.stage}` ({ctx.change_kind or '-'} iteration {ctx.iteration}) "
        f"ran out of repair budget. The tree is as the last repair left it (or the best snapshot, after a "
        f"rollback). Fix the cause, then continue with `--resume`.\n\n{render_failure(failure)}\n",
        encoding="utf-8",
    )
    ctx.log(f"stopping: {reason}; report in {report.name}")
    return StageStopped(reason, report)

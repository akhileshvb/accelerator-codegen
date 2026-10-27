"""A failure, how it is triaged, the repair loop that answers it, and the advisor the loop consults.

No stage failure, gate failure, bench failure, timeout or exception ends a run uncaught. A failure is triaged
(from the failure's own text, engine-generic) as infra or code; infra failures are retried (and, when persistent,
quarantined), code failures resume the agent's session with the failure report, and once that budget is spent the
tree goes back to the best snapshot, the plan is redone once, and the run stops cleanly.

The advisor is a second opinion for a stuck agent: a stronger model forks the agent's own codex session (full
context) and answers once. The fork is a new session; the agent's is never touched, and files the advisor changes
are undone. Consults are capped per stage and logged. Two ways in: the agent's `ask_advisor` tool (a request file
the codex supervisor serves while the agent waits in the tool call), and the repair loop (a stalled failure, a
no-progress redirect or a reported blocker).
"""
from __future__ import annotations

import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import threading
import time
import traceback
import uuid
from dataclasses import dataclass, field, replace
from pathlib import Path
from typing import Any, Callable, Literal, Mapping, TYPE_CHECKING, TypeVar

from accel_pipeline.config import (
    load_run_workload,
    make_model_id,
    ModelConfig,
    PipelineConfig,
    query_artifacts_dir,
    benchmark_of,
    read_json,
)
from accel_pipeline.data import pending_change_request
from accel_pipeline.layout import query_dir, tree_snapshot
from accel_pipeline.prompts import render_prompt
from accel_pipeline.runlog import append_experiment_log, log_event
from accel_pipeline.agent import MAIN, READ_ONLY_TOOLS, SessionStore, write_json


T = TypeVar("T")

REPAIR_STATE_FILE = "repair_state.json"
QUARANTINE_FILE = "quarantine.json"
STOP_REPORT_FILE = "stop_report.md"
LAST_FAILURE_FILE = "last_failure.md"
EXIT_STOPPED = 3

_REPLANNABLE = frozenset({"implementation", "optimization"})

ADVISOR_STAGE = "advisor"
REQUEST_FILE = "advisor_request.json"
RESPONSE_FILE = "advisor_response.json"
COUNT_FILE = "advisor_consults.json"
MAX_ADVICE_CHARS = 6000
ADVICE_INTRO = "A reviewer with your full context diagnosed:"
ADVISED = frozenset({"implementation", "optimization", "crash_debug"})  # crash_debug: the repair turns of either


class StageStopped(Exception):
    """The repair budget is spent: the run stops here, recorded, and `--resume` continues."""

    def __init__(self, reason: str, report: Path):
        super().__init__(f"{reason} (see {report})")
        self.reason = reason
        self.report = report


class Unavailable(Exception):
    """No consult happens: the cap is spent, the stage has no session to fork, or the advisor is off."""


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
    quick_first: bool = False  # the stage's gate batch starts with quick_check and its write-cost projection
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
    stall = _Stall()
    blocker: Failure | None = None  # the agent said the obstacle is not in its files: the next code round adapts to it
    last_blocker: Failure | None = None  # cited in the stop report
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
        if failure.infra_reason == AGENT_BLOCKER:
            blocker = last_blocker = failure
            if revise_pending_suite(ctx):  # the blocker was the frozen suite: the tester decides, then the check reruns
                continue
            if gates and from_attempt:  # the obstacle may be a flake: the stage's own checks run once first
                ctx.log(f"the agent reported a blocker ({failure.summary}); running the stage's gates once")
                continue
            failure = replace(failure, kind="code", infra_reason=None)  # nothing left to check: the agent adapts
        if failure.infra_reason == PROVIDER_CREDITS:
            raise stop_stage(ctx, failure, failure.summary)
        if failure.infra_reason == HARNESS_MISSING:
            raise stop_stage(ctx, failure, "a workload-harness target is missing: run `--stage host_bootstrap`, then `--resume`")
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
            if blocker is not None:
                ctx.log(f"the agent reported a blocker ({blocker.summary}); it adapts in this round")
                failure, blocker = _adapting(ctx, blocker, failure), None
            else:
                failure = _with_advice(ctx, failure, stall)
            if (blocker := repair_turn(ctx, failure)) is not None:
                last_blocker = blocker
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
        reason = f"{failure.kind} failure in {failure.source} survived {code_rounds} repair rounds"
        if last_blocker is not None:
            raise stop_stage(ctx, _persisting(last_blocker, failure), f"{reason}; the agent's blocker: {last_blocker.summary}")
        raise stop_stage(ctx, failure, reason)


def run_gates_with_repair(
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
    """Run gates; on failure repair (at most `max_rounds` code rounds) until they pass, or stop the run cleanly."""
    if dry_run:
        return []
    ctx = RepairCtx(
        pipeline_config, model_config, workload_name, query, stage=change_kind or "gates",
        change_kind=change_kind, iteration=iteration, model_overrides=model_overrides, max_code_rounds=max_rounds,
    )
    run_with_repair(ctx, lambda: None, gates=gate_names)
    return ctx.results


_VOLATILE = re.compile(r"0x[0-9a-fA-F]+|\d+")


@dataclass
class _Stall:
    """The same failure source, summary and last log line, round after round; the advisor is consulted once per stall."""

    signature: str = ""
    rounds: int = 0
    consulted: bool = False

    def seen(self, failure: Failure) -> None:
        ending = next((line for line in reversed(failure.evidence.splitlines()) if line.strip()), "")
        signature = _VOLATILE.sub("#", f"{failure.source}:{failure.summary}:{ending}")
        if signature == self.signature:
            self.rounds += 1
        else:
            self.signature, self.rounds, self.consulted = signature, 1, False


def _with_advice(ctx: RepairCtx, failure: Failure, stall: _Stall) -> Failure:
    """The failure, with the advisor's diagnosis added once the same failure has stalled `advisor.stall_rounds`
    rounds or the watchdog ended the agent's turn without progress."""
    stall.seen(failure)
    cfg = ctx.pipeline_config.advisor
    if stall.consulted or not (failure.source == "no_progress" or stall.rounds >= cfg.stall_rounds):
        return failure
    stall.consulted = True
    why = ("the watchdog ended the agent's turn without progress" if failure.source == "no_progress"
           else f"the same failure {stall.rounds} repair rounds in a row")
    return _advised(ctx, failure, why, trigger="stall")


ADAPT = ("You reported a blocker outside your files. The pipeline does not stop for it, and it cannot change those "
         "files for you: adapt. Work around it within your own files; if part of the analysed design cannot be built "
         "with what exists, build the rest, record the deviation in experiment_log.md and fix the failing check; if "
         "the design itself no longer holds, call request_structural_reanalysis with the evidence.")


def _adapting(ctx: RepairCtx, blocker: Failure, failure: Failure) -> Failure:
    """The failing check with the agent's blocker and the instruction to adapt to it, advised when the cap allows."""
    notes = [] if failure.evidence == blocker.evidence else [f"Your blocker: {blocker.summary}\n{blocker.evidence}"[:4000]]
    adapting = replace(failure, diagnosis=[*failure.diagnosis, *notes, ADAPT])
    return _advised(ctx, adapting, "the agent reported a blocker outside its files", trigger="blocker")


def _advised(ctx: RepairCtx, failure: Failure, why: str, *, trigger: str) -> Failure:
    """The failure with the advisor's diagnosis added, or unchanged when no consult happens."""
    question = (f"The orchestrator is consulting you because {why}. Latest failure report:\n\n{render_failure(failure)}"[:8000])
    try:
        advice = consult(ctx.pipeline_config, ctx.model_config, ctx.workload_name, ctx.query, ctx.stage,
                                 question=question, trigger=trigger, optimization_iter=ctx.iteration)
    except Unavailable as exc:
        ctx.log(f"advisor not consulted: {exc}")
        return failure
    except Exception as exc:  # noqa: BLE001 - repair goes on without advice
        ctx.log(f"advisor consult failed: {type(exc).__name__}: {exc}")
        return failure
    ctx.log(f"the advisor was consulted ({trigger}); its advice goes to the agent")
    return replace(failure, diagnosis=[*failure.diagnosis, advise_message(advice)])


def _attempt(ctx: RepairCtx, attempt: Callable[[], T]) -> tuple[T | None, Failure | None]:
    from accel_pipeline import gates as gate_mod  # gates imports Failure from this module
    try:
        return attempt(), None
    except StageStopped:
        raise
    except StageFailure as exc:
        return None, exc.failure
    except gate_mod.GateFailure as exc:
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

    from accel_pipeline import gates as gate_mod  # gates imports Failure from this module
    cfg = ctx.pipeline_config
    if ctx.quick_first and (quick := _quick_gate(ctx)) is not None:
        return [quick], quick
    kwargs = {"change_kind": ctx.change_kind, "iteration": ctx.iteration, "reuse": True}
    if cfg.quick_tier.parallel_gates:
        results = gate_mod.run_gates_parallel(cfg, names, ctx.query, ctx.workload, **kwargs)
        return results, gate_mod.first_failure(results)
    from accel_pipeline.stages import run_post_stage_gates_collect  # stages.base imports the agent backend, which imports this module

    return run_post_stage_gates_collect(cfg, ctx.workload_name, ctx.query, names, **kwargs)


def _quick_gate(ctx: RepairCtx) -> GateResult | None:
    """quick_check as the batch's first gate: a write the projection says would rebuild at full scale fails here,
    before the heavy gates run for minutes on code that cannot pass them."""
    from accel_pipeline import gates as gate_mod  # gates imports Failure from this module
    quick = gate_mod.run_quick_check(ctx.pipeline_config, ctx.workload, ctx.query)
    if quick.status != "fail":
        return None
    text = render_failure(quick.failure) if quick.failure else quick.summary
    return gate_mod.GateResult("quick_check", "fail", {"status": "fail", "reason": quick.summary, "log_tail": text})


def _record(ctx: RepairCtx, failure: Failure) -> None:
    (ctx.art / LAST_FAILURE_FILE).write_text(render_failure(failure), encoding="utf-8")


def _clear_infra(ctx: RepairCtx, failure: Failure, used: int) -> None:
    """Infra failures cost no code budget: repair the shared tree if that is the cause, wait out a
    provider flake, and let the caller re-run."""
    ctx.log(f"infra failure ({failure.infra_reason}) in {failure.source}; retry {used} without the agent")

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


def _persisting(blocker: Failure, failure: Failure) -> Failure:
    """The agent's blocker, with the check that still failed when the repair budget ran out (the report cites blocker.md)."""
    again = f"the check still failed after the repair rounds ({failure.source}): {failure.summary}"
    return replace(blocker, summary=f"{blocker.summary}; {again}", diagnosis=[*blocker.diagnosis, again],
                   reproduce=failure.reproduce)


def repair_turn(ctx: RepairCtx, failure: Failure, *, must_change: bool | None = None) -> Failure | None:
    """One agent turn, in the stage's own session, on the failure report; a provider flake re-runs it.
    must_change: the turn has to edit the query's files (default: when the failure is a code failure).
    Returns the blocker when the agent ended the turn with report_blocker, else None."""
    if ctx.stage != "test_suite" and revise_pending_suite(ctx):
        return None
    turn = ctx.repair_turn or _crash_debug_turn(ctx, failure.kind == "code" if must_change is None else must_change)
    retries = ctx.pipeline_config.repair.provider_retries
    for flake in range(1, retries + 2):
        try:
            turn(failure)
            return None
        except StageStopped:
            raise
        except StageFailure as exc:
            if exc.failure.infra_reason == AGENT_BLOCKER:
                return exc.failure
            if exc.failure.infra_reason == PROVIDER_CREDITS:
                raise stop_stage(ctx, exc.failure, exc.failure.summary) from exc
            if exc.failure.infra_reason == PROVIDER_FLAKE and flake <= retries:
                _wait_out_flake(ctx, flake)
                continue
            ctx.log(f"the repair turn itself failed ({exc.failure.kind}: {exc.failure.summary}); "
                    "the next check judges the tree")
        except Exception as exc:  # noqa: BLE001
            ctx.log(f"the repair turn raised {type(exc).__name__}: {exc}; the next check judges the tree")
        return None
    return None


def revise_pending_suite(ctx: RepairCtx) -> bool:
    """A change request against the frozen test suite goes to the tester before the implementation is touched again."""
    from accel_pipeline.stages import revise_test_suite  # stages imports this module

    if not pending_change_request(ctx.art):
        return False
    revise_test_suite(ctx.pipeline_config, ctx.model_config, ctx.workload_name, ctx.query, ctx.model_overrides)
    ctx.log("the tester handled a test-suite change request; the next check judges the suite it froze")
    return True


def _crash_debug_turn(ctx: RepairCtx, must_change: bool) -> Callable[[Failure], None]:
    from accel_pipeline.stages import run_agent_stage

    bench = benchmark_of(ctx.workload)

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
    from accel_pipeline.optimization.kernel_snapshot import has_best_kernel_snapshot, rebuild_workload_targets, restore_best_kernel

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
    from accel_pipeline import run_pipeline, stages
    from accel_pipeline.runlog import PipelineState, state_path

    path = ctx.art / REPAIR_STATE_FILE
    used = ctx.pipeline_config.repair.max_replans - _replan_budget(ctx)
    path.write_text(json.dumps({"replans": used + 1}), encoding="utf-8")
    ctx.log("repair budget spent; re-planning once (analysis, tests, implementation) in a fresh session")
    SessionStore(ctx.art).reset(ctx.query, MAIN)
    try:
        stages.write_request(
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
    from accel_pipeline.config import analysis_artifact_path

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


def scope(stage: str) -> str:
    """The stage family that shares one consult cap."""
    return "optimization" if stage.startswith("optimization") else "implementation" if stage.startswith("impl") else stage


def consults_used(art: Path, stage: str) -> int:
    return int(read_json(art / COUNT_FILE).get(scope(stage), 0))


def _claim(art: Path, stage: str, cap: int) -> int:
    used = consults_used(art, stage)
    if used >= cap:
        raise Unavailable(f"advisor limit reached ({cap} consults in {scope(stage)})")
    write_json(art / COUNT_FILE, {**read_json(art / COUNT_FILE), scope(stage): used + 1})
    return used + 1


def snapshot_tree(repo_root: Path, dest: Path) -> dict[str, str]:
    """The changed files (git status) and their signatures, copied into `dest`."""
    before = tree_snapshot(repo_root) or {}
    for rel in before:
        src = repo_root / rel
        if src.is_file() and not src.is_symlink():
            (dest / rel).parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(src, dest / rel)
    return before


def restore_tree(repo_root: Path, before: dict[str, str], saved: Path) -> list[str]:
    """Undo every change since `snapshot_tree`; returns the paths put back."""
    now = tree_snapshot(repo_root) or {}
    undone = []
    for rel in sorted(before.keys() | now.keys()):
        if before.get(rel) == now.get(rel):
            continue
        target = repo_root / rel
        if (saved / rel).is_file():
            shutil.copy2(saved / rel, target)
        elif rel not in before and subprocess.run(["git", "-C", str(repo_root), "checkout", "HEAD", "--", rel],
                                                  capture_output=True).returncode == 0:
            pass  # a tracked file the advisor edited: back at HEAD
        else:
            target.unlink(missing_ok=True)
        undone.append(rel)
    return undone


def consult(pipeline_config: Any, model_config: Any, workload_name: str, query: int, stage: str, *,
            question: str, trigger: str, optimization_iter: int = 0, session_id: str = "") -> str:
    """Fork the stage's main session on the advisor model and return its advice (raises Unavailable)."""
    from accel_pipeline.agent import StageRunRequest, create_agent_backend  # codex imports this module

    cfg = pipeline_config.advisor
    if cfg.max_consults_per_stage <= 0:
        raise Unavailable("the advisor is disabled")
    repo_root = pipeline_config.repo_root
    workload = load_run_workload(pipeline_config, workload_name)
    art = query_artifacts_dir(repo_root, query, workload=workload)
    sid = SessionStore(art).session_id(query, MAIN) or session_id
    if not sid:
        raise Unavailable("this stage has no session to fork yet")
    number = _claim(art, stage, cfg.max_consults_per_stage)

    model_id = make_model_id(ADVISOR_STAGE, pipeline_config, model_config)
    backend = create_agent_backend(pipeline_config=pipeline_config, query=query, stage=stage, model_config=model_config,
                                   workload_name=workload_name, optimization_iter=optimization_iter)
    backend.timeout_sec = cfg.timeout_sec
    request = StageRunRequest(
        stage=ADVISOR_STAGE, instructions=render_prompt(pipeline_config, "05_advisor.md", workload, query, stage, optimization_iter),
        user_message=question, agent_name=f"accel_{ADVISOR_STAGE}", model_id=model_id, agent_model=model_id,
        metadata={"fork_session_id": sid})
    started = time.monotonic()
    with tempfile.TemporaryDirectory(prefix="accel_advisor_") as tmp:
        before = snapshot_tree(repo_root, Path(tmp))
        try:
            result = backend.fork_stage(request)
        finally:
            undone = restore_tree(repo_root, before, Path(tmp))
    advice = (result.final_text or "").strip()[-MAX_ADVICE_CHARS:] if result.status == "finished" else ""
    log_event(repo_root, query, workload, "advisor_consult", stage=stage, trigger=trigger, model=model_id,
              session_id=sid, consult=number, status=result.status, advice_chars=len(advice), edits_undone=len(undone),
              duration_ms=int((time.monotonic() - started) * 1000))
    append_experiment_log(
        repo_root, query, f"- advisor[{stage}] consult {number}/{cfg.max_consults_per_stage} ({trigger}, {model_id}): "
        f"{result.status}, {len(advice)} chars" + (f", undid edits to {', '.join(undone[:5])}" if undone else ""),
        workload=workload)
    if not advice:
        raise Unavailable(f"the advisor gave no answer ({result.status}: {(result.error or '')[:200]})")
    return advice


def advise_message(advice: str) -> str:
    return f"{ADVICE_INTRO}\n{advice}"


def ask(art: Path, question: str, timeout_sec: float, poll_sec: float = 2.0) -> str:
    """The `ask_advisor` tool: file a request and wait for the supervisor's answer."""
    request_id = uuid.uuid4().hex
    (art / RESPONSE_FILE).unlink(missing_ok=True)
    write_json(art / REQUEST_FILE, {"id": request_id, "question": question})
    deadline = time.monotonic() + timeout_sec
    while time.monotonic() < deadline:
        response = read_json(art / RESPONSE_FILE)
        if response.get("id") == request_id:
            return str(response.get("advice") or f"No advice: {response.get('reason', 'unavailable')}.")
        time.sleep(poll_sec)
    (art / REQUEST_FILE).unlink(missing_ok=True)
    return "No advice: the advisor did not answer in time. Carry on with your own diagnosis."


class Servicer:
    """Run by the codex supervisor during an agent turn: serves ask_advisor requests, one at a time."""

    def __init__(self, pipeline_config: Any, model_config: Any, workload_name: str, query: int, stage: str,
                 optimization_iter: int, art: Path) -> None:
        self._args = (pipeline_config, model_config, workload_name, query, stage)
        self._iter, self._art = optimization_iter, art
        self._thread: threading.Thread | None = None
        self._served = ""
        (art / REQUEST_FILE).unlink(missing_ok=True)  # a request left by an earlier turn is nobody's

    def poll(self, live_session_id: str = "") -> None:
        request = read_json(self._art / REQUEST_FILE)
        if not request or request.get("id") == self._served or (self._thread and self._thread.is_alive()):
            return
        self._served = str(request.get("id"))
        self._thread = threading.Thread(target=self._answer, args=(request, live_session_id), daemon=True)
        self._thread.start()

    def join(self) -> None:
        if self._thread:
            self._thread.join()

    def _answer(self, request: dict[str, Any], live_session_id: str) -> None:
        response: dict[str, Any] = {"id": request.get("id")}
        try:
            response["advice"] = consult(*self._args, question=str(request.get("question") or ""), trigger="agent",
                                         optimization_iter=self._iter, session_id=live_session_id)
        except Unavailable as exc:
            response["reason"] = str(exc)
        except Exception as exc:  # noqa: BLE001 - the agent's turn goes on without advice
            response["reason"] = f"{type(exc).__name__}: {exc}"
        write_json(self._art / RESPONSE_FILE, response)
        (self._art / REQUEST_FILE).unlink(missing_ok=True)


if TYPE_CHECKING:
    from accel_pipeline.gates import GateResult


PROVIDER_FLAKE = "provider_flake"  # the infra_reason of a network or provider error: the agent never ran
AGENT_BLOCKER = "agent_reported_blocker"  # the infra_reason of a blocker the agent handed to the operator
PROVIDER_CREDITS = "provider_out_of_credits"  # the infra_reason of a 402 from the provider: retrying cannot help
HARNESS_MISSING = "harness_missing"  # a harness target host_bootstrap should have made is not there: its stage, not this one
FailureKind = Literal["code", "infra", "timeout", "measurement", "orchestrator"]


@dataclass
class Failure:
    kind: FailureKind
    stage: str
    source: str
    summary: str
    evidence: str = ""
    reproduce: str = ""
    infra_reason: str | None = None
    diagnosis: list[str] = field(default_factory=list)
    stacks: str = ""  # every thread's stack when a run hit its deadline (gates/run_outcome.py)


class StageFailure(Exception):
    def __init__(self, failure: Failure):
        super().__init__(failure.summary)
        self.failure = failure


def render_failure(f: Failure) -> str:
    """The markdown report fed to the agent."""
    lines = [f"## {f.kind} failure in {f.stage} ({f.source})", "", f.summary]
    if f.diagnosis:
        lines += ["", *(f"- {d}" for d in f.diagnosis)]
    if f.reproduce:
        lines += ["", f"Reproduce: `{f.reproduce}`"]
    return "\n".join(lines + render_evidence(f.stacks, f.evidence))


def render_evidence(stacks: str, log_tail: str, *, max_chars: int = 6000) -> list[str]:
    """The traces of a failure: the thread stacks at a deadline, then the end of the log."""
    lines = []
    if stacks:
        lines += ["", "Thread stacks at the deadline:", "```", stacks, "```"]
    if log_tail:
        lines += ["", "Log tail:", "```", log_tail[-max_chars:], "```"]
    return lines


_CRASH_SIGNALS = re.compile(
    r"(?i)(segmentation fault|sigsegv|sigabrt|internalexception|"
    r"assertion failed|stack trace|sanitizer|core dumped|"
    r"undefined reference|error:|fatal error:|terminate called|"
    r"tests passed.*failed|test.*failed)"
)


def crash_lines(log_tail: str, *, max_lines: int = 40) -> list[str]:
    """The log lines that name a crash, an error or a failed test."""
    return [line.strip() for line in log_tail.splitlines() if _CRASH_SIGNALS.search(line)][-max_lines:]


def extract_crash_highlights(log_tail: str, *, max_lines: int = 40) -> list[str]:
    """crash_lines, else the last lines of the log."""
    return crash_lines(log_tail, max_lines=max_lines) or [
        line.strip() for line in log_tail.splitlines()[-max_lines:] if line.strip()]


def gate_log_tail(gate_data: dict[str, Any], *, max_chars: int = 6000) -> str:
    for key in _TEXT_KEYS:
        text = str(gate_data.get(key) or "").strip()
        if text:
            return text[-max_chars:]
    return ""


def failure_from_gate(
    result: GateResult,
    *,
    stage: str,
    workload: dict[str, Any] | None = None,
    repo_root: Path | None = None,
    load_factor: float = 1.5,
) -> Failure:
    """A failed gate as a Failure, triaged as infra or code."""
    from accel_pipeline import gates as gate_mod  # gates imports Failure from this module
    workload = workload or {}
    kind, reason = classify(result.name, result.data, workload, repo_root, load_factor=load_factor)
    tail = gate_log_tail(result.data)
    return Failure(
        kind=kind,  # type: ignore[arg-type]
        stage=stage,
        source=f"gate:{result.name}",
        summary=gate_mod.gate_failure_message(result),
        evidence=tail,
        reproduce=str(result.data.get("reproduce") or f'request_gate("{result.name}")'),
        infra_reason=reason,
        diagnosis=_racy_diagnosis(result.data) + [f"`{line}`" for line in crash_lines(tail, max_lines=15)],
        stacks=str(result.data.get("stacks") or ""),
    )


def _racy_diagnosis(data: dict[str, Any]) -> list[str]:
    repeats = racy_repeats(data)
    if repeats is None:
        return []
    passed, failed_on = repeats
    return [f"racy: the harness passed {passed} of {failed_on} repeats. A timing-dependent failure is a real "
            "defect (a missing lock, a wait with no bound, an unsynchronised read), not infrastructure: find the "
            "shared state and fix it. Rerunning until it passes proves nothing."]


# --- telling infrastructure failures from code failures, from the failure's own text (engine-generic)

_TEXT_KEYS = ("log_tail", "stderr", "stdout", "notes", "reason")

_SANITIZER_REPORT = re.compile(r"(?:WARNING: ThreadSanitizer|ERROR: AddressSanitizer|ERROR: LeakSanitizer)")
_STACK_FRAME = re.compile(r"^\s*#\d+ .*? (?:in )?\S*?(/\S+?\.(?:c|cc|cpp|cxx|h|hh|hpp|rs)):\d+", re.MULTILINE)
_RACY = re.compile(r"failed on run (\d+) after passing (\d+) times")
_TIMEOUT = re.compile(r"did not finish within|timed out after")
_TIMEOUT_RC = frozenset({124, 137})
_QUEUED_SEC = 5.0  # a wait for the measurement lease this long means other benchmarks were running
_MISSING_PRODUCER = re.compile(
    r"(?:analysis\.json|suite\.yaml)[^\n]*(?:not found|missing|no such file)"
    r"|(?:not found|missing|no such file)[^\n]*(?:analysis\.json|suite\.yaml)"
    r"|target missing from the gate context",
    re.IGNORECASE,
)
_HARNESS_ERROR = re.compile(
    r"unbound variable"
    r"|Traceback \(most recent call last\):\n(?:[ \t]+.*\n)*?[ \t]+File \"[^\"]*(?:gates/[^\"]*\.py|read_gate_context\.py)\""
    r"|host\.sh[^\n]*(?:exit(?:ed)?(?: with)?(?: code| status)?[ =:]+2\b|returned 2\b)",
    re.IGNORECASE,
)
# The test_suite validation runs on the registration the pipeline scaffolded: a failure to find or serve it is the
# pipeline's, never the tester's (who owns tests/workload only).
_REGISTRATION_ERROR = re.compile(r"unsupported shared query|Table Function with name \S+ does not exist", re.IGNORECASE)
_TRANSIENT_PROVIDER = (
    "stream disconnected", "error sending request", "connection refused", "cannot connect to host",
    "timed out", "timeout", "temporarily unavailable", "503", "502", "429", "rate limit", "reconnecting...",
)


_OUT_OF_CREDITS = ("402 payment required", "requires more credits")


def is_out_of_credits(reason: str | None) -> bool:
    text = (reason or "").lower()
    return any(needle in text for needle in _OUT_OF_CREDITS)


def is_transient_provider_failure(reason: str | None) -> bool:
    """True when the agent never ran (network/provider flake), not a real hyp fail."""
    text = (reason or "").lower()
    return bool(text) and any(needle in text for needle in _TRANSIENT_PROVIDER)


def failure_text(data: Mapping[str, Any]) -> str:
    return "\n".join(str(data[k]) for k in _TEXT_KEYS if data.get(k))


def _returncode(data: Mapping[str, Any]) -> int | None:
    rc = data.get("returncode", data.get("exit_code"))
    return int(rc) if rc is not None and str(rc).lstrip("-").isdigit() else None


def _changed_files(repo_root: Path) -> set[str]:
    cmds = (["git", "diff", "--name-only", "HEAD"], ["git", "ls-files", "--others", "--exclude-standard"])
    changed: set[str] = set()
    for cmd in cmds:
        try:
            out = subprocess.run(cmd, cwd=repo_root, capture_output=True, text=True, timeout=60).stdout
        except (OSError, subprocess.SubprocessError):
            continue
        changed.update(line.strip() for line in out.splitlines() if line.strip())
    return changed


def racy_repeats(data: Mapping[str, Any]) -> tuple[int, int] | None:
    """(passed, total) of a gate that repeats its harness and failed on a later repeat, else None."""
    match = _RACY.search(failure_text(data))
    if match is None:
        return None
    failed_on, passed = int(match.group(1)), int(match.group(2))
    return passed, failed_on


def _own_paths(workload: Mapping[str, Any], query: int | None, repo_root: Path) -> set[str]:
    """What counts as the failing code's own: files changed this stage plus the stage's home, a
    query's directory or an engine backend's tree."""
    ours = set(_changed_files(repo_root))
    if workload.get("workload_kind") == "engine_backend":
        ours.add(f"backends/{workload.get('engine_name') or ''}/")
    elif query is not None:
        ours.add(query_dir(str(workload.get("benchmark_name") or "tpch"), query) + "/")
    return ours


def _engine_internal_sanitizer(text: str, workload: Mapping[str, Any], query: int | None, repo_root: Path | None) -> bool:
    """A sanitizer report none of whose frames is in the failing code's own files."""
    if repo_root is None or not _SANITIZER_REPORT.search(text):
        return False
    frames = _STACK_FRAME.findall(text)
    if not frames:
        return False
    ours = _own_paths(workload, query, repo_root)
    return not any(rel in frame for frame in frames for rel in ours)


def _queued_for_lease(text: str) -> bool:
    from accel_pipeline.pack import measurement_lease

    waited = measurement_lease.waited_seconds(text)
    return waited is not None and waited >= _QUEUED_SEC


def _loaded(repo_root: Path | None, load_factor: float, text: str = "") -> bool:
    """The machine is oversubscribed, the failing run queued for the measurement lease, or another
    worker holds the measurement lease or the shared-tree lease."""
    try:
        if os.getloadavg()[0] > load_factor * (os.cpu_count() or 1):
            return True
    except OSError:
        pass
    from accel_pipeline.pack import measurement_lease

    if _queued_for_lease(text) or measurement_lease.held_by_other(os.environ.get("ACCEL_INSTANCE_TAG", "")):
        return True
    if repo_root is None:
        return False
    from accel_pipeline.pack.shared_file_lease import SharedFileLease, worker_id_from_env

    holder = SharedFileLease(repo_root).holder()
    return holder is not None and holder != worker_id_from_env("")


def classify(
    gate_name: str,
    data: Mapping[str, Any],
    workload: Mapping[str, Any],
    repo_root: Path | None,
    *,
    query: int | None = None,
    load_factor: float = 1.5,
) -> tuple[str, str | None]:
    """(kind, infra_reason) of a failed gate: kind is "infra" or "code"."""
    from accel_pipeline.pack import measurement_lease

    text = failure_text(data)
    if query is None and workload.get("query_number") is not None:
        query = int(workload["query_number"])
    if _HARNESS_ERROR.search(text):
        return "infra", "harness_error"
    if gate_name != "host_bootstrap" and missing_harness_target(text, workload):  # making them is that gate's stage
        return "infra", HARNESS_MISSING
    if _MISSING_PRODUCER.search(text):
        return "infra", "missing_producer"
    if gate_name == "test_suite_validate" and _REGISTRATION_ERROR.search(text):
        return "infra", "harness_error"
    shared = classify_infra_failure(text, query=query if query is not None else -1)
    if shared:
        return "infra", shared
    if _engine_internal_sanitizer(text, workload, query, repo_root):
        return "infra", "engine_internal_sanitizer"
    if measurement_lease.BUSY.search(text):
        return "infra", "measurement_lease_busy"
    if (_TIMEOUT.search(text) or _returncode(data) in _TIMEOUT_RC) and _loaded(repo_root, load_factor, text):
        return "infra", "timeout_under_load"
    return "code", None


_UNKNOWN_TARGET = re.compile(r"unknown target '([A-Za-z0-9_]+)'|No rule to make target '([A-Za-z0-9_]+)'")


def missing_harness_target(text: str, workload: Mapping[str, Any]) -> str:
    """The workload-harness target a build could not find (host_bootstrap makes them), if that is why it failed."""
    from accel_pipeline.config import harness_targets, sanitize_engine_name, unit_test_target

    engine, bench = str(workload.get("engine_name") or ""), str(workload.get("benchmark_name") or "")
    if not (engine and bench and workload.get("query_number") is not None):
        return ""
    harness = {*harness_targets(sanitize_engine_name(engine), bench).values(),
               f"{unit_test_target(bench, int(workload['query_number']))}_aux"}
    return next((t for m in _UNKNOWN_TARGET.finditer(text) for t in m.groups() if t in harness), "")


# --- shared-tree infrastructure failures: repaired without burning agent turns

_UNSUPPORTED = re.compile(r"unsupported shared query(?:\s+binding)?", re.IGNORECASE)
_REBUILD_REVERT = re.compile(r"rebuild after (?:revert|restore) failed", re.IGNORECASE)
_FOREIGN_COMPILE = re.compile(
    r"(?:error|fatal error):.*?accelerators/[A-Za-z0-9_]+/q(\d+)/",
    re.IGNORECASE | re.DOTALL,
)
_FOREIGN_COMPILE_PATH = re.compile(
    r"accelerators/[A-Za-z0-9_]+/q(\d+)/[^\s:]+",
    re.IGNORECASE,
)


def classify_infra_failure(text: str, *, query: int) -> str | None:
    """Return a short infra reason if ``text`` is a shared-tree / catalog failure."""
    if not text:
        return None
    if _UNSUPPORTED.search(text):
        return "unsupported_shared_query"
    if _REBUILD_REVERT.search(text):
        return "rebuild_after_revert_failed"
    # Prefer the path on an actual error/fatal/undefined line — not every TU
    # mentioned while cmake compiles peers in the same shared library.
    foreign_from_errors: set[int] = set()
    for line in text.splitlines():
        low = line.lower()
        if not (
            "error:" in low
            or "fatal error:" in low
            or "undefined reference" in low
        ):
            continue
        for m in _FOREIGN_COMPILE_PATH.finditer(line):
            foreign_from_errors.add(int(m.group(1)))
    foreign_from_errors.discard(int(query))
    if foreign_from_errors:
        return f"foreign_file_compile:q{sorted(foreign_from_errors)[0]}"
    m = _FOREIGN_COMPILE.search(text)
    if m and int(m.group(1)) != int(query):
        return f"foreign_file_compile:q{m.group(1)}"
    return None


def release_shared_lease(repo_root: Path, *, query: int, worker_id: str | None = None) -> bool:
    from accel_pipeline.pack.shared_file_lease import release_sticky_lease, worker_id_from_env

    holder = worker_id or worker_id_from_env(f"q{query}")
    return bool(release_sticky_lease(repo_root, holder=holder))


def restore_aux_from_snapshot(repo_root: Path, query: int, workload: dict[str, Any] | None = None) -> list[str]:
    art = query_artifacts_dir(repo_root, query, workload=workload)
    # The snapshot mirrors repo-relative paths; everything a query owns (aux module,
    # tests, cost model under config/) lives in its directory accelerators/<bench>/q<N>/.
    snap_root = art / "best_valid_baseline_snapshot"
    bench = str((workload or {}).get("benchmark_name") or "tpch")
    rel_q = Path("accelerators") / bench / f"q{query}"
    snap = snap_root / rel_q
    restored: list[str] = []
    if not snap.is_dir():
        return restored
    for src in sorted(snap.rglob("*")):
        if src.is_file():
            dest = repo_root / src.relative_to(snap_root)
            dest.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(src, dest)
            restored.append(str(dest.relative_to(repo_root)))
    return restored


def handle_infra_failure(pipeline_config: Any, workload: dict[str, Any], query: int, *, text: str) -> dict[str, Any] | None:
    """If ``text`` is an infra failure, repair the tree, release the lease and return what was done."""
    reason = classify_infra_failure(text, query=query)
    if not reason:
        return None
    repo_root = Path(pipeline_config.repo_root)
    released = release_shared_lease(repo_root, query=query, worker_id=getattr(pipeline_config, "worker_id", None))

    restored: dict[str, list[str]] = {}
    if foreign := re.match(r"foreign_file_compile:q(\d+)$", reason):  # the compile error names the peer's file
        if files := restore_aux_from_snapshot(repo_root, int(foreign.group(1))):
            restored[f"q{foreign.group(1)}"] = files
    # Restore this query from its own baseline so a bad mid-edit does not linger.
    if own := restore_aux_from_snapshot(repo_root, query, workload=workload):
        restored[f"q{query}_own"] = own
    return {
        "infra_reason": reason,
        "lease_released": released,
        "repair": {"owner_query": query, "restored": restored},
        "fail_fast": True,
    }

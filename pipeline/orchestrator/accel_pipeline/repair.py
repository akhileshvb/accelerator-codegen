"""The repair loop, and the advisor it consults.

No stage failure, gate failure, bench failure, timeout or exception ends a run uncaught. A failure is triaged;
infra failures are retried (and, when persistent, quarantined), code failures resume the agent's session with the
failure report, and once that budget is spent the tree goes back to the best snapshot, the plan is redone once, and
the run stops cleanly.

The advisor is a second opinion for a stuck agent: a stronger model forks the agent's own codex session (full
context) and answers once. The fork is a new session; the agent's is never touched, and files the advisor changes
are undone. Consults are capped per stage and logged. Two ways in: the agent's `ask_advisor` tool (a request file
the codex supervisor serves while the agent waits in the tool call), and the repair loop (a stalled failure, a
no-progress redirect or a reported blocker).
"""

from __future__ import annotations

import json
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
from typing import Any, Callable, TypeVar

from accel_pipeline import session as session_mod
from accel_pipeline.config import ModelConfig, PipelineConfig, make_model_id
from accel_pipeline.workload import load_run_workload, query_artifacts_dir
from accel_pipeline.failure import (
    AGENT_BLOCKER,
    HARNESS_MISSING,
    PROVIDER_CREDITS,
    PROVIDER_FLAKE,
    Failure,
    StageFailure,
    classify,
    failure_from_gate,
    failure_text,
    render_failure,
)
from accel_pipeline.gates.parallel import first_failure, run_gates_parallel
from accel_pipeline.gates.run import GateFailure, GateResult
from accel_pipeline.layout import query_dir, tree_snapshot
from accel_pipeline.pipeline_log import log_event
from accel_pipeline.playbook import append_experiment_log
from accel_pipeline.prompts import render_prompt
from accel_pipeline.session import MAIN, SessionStore
from accel_pipeline.stages.gate_plan import run_post_stage_gates_collect

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
READ_ONLY_TOOLS = ["quick_check", "gate_status", "get_profile_digest", "get_store_diagnostics",
                   "get_optimization_evidence", "discover_host_sdk"]
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
            if _revise_test_suite(ctx):  # the blocker was the frozen suite: the tester decides, then the check reruns
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
    if ctx.quick_first and (quick := _quick_gate(ctx)) is not None:
        return [quick], quick
    kwargs = {"change_kind": ctx.change_kind, "iteration": ctx.iteration, "reuse": True}
    if cfg.quick_tier.parallel_gates:
        results = run_gates_parallel(cfg, names, ctx.query, ctx.workload, **kwargs)
        return results, first_failure(results)
    return run_post_stage_gates_collect(cfg, ctx.workload_name, ctx.query, names, **kwargs)


def _quick_gate(ctx: RepairCtx) -> GateResult | None:
    """quick_check as the batch's first gate: a write the projection says would rebuild at full scale fails here,
    before the heavy gates run for minutes on code that cannot pass them."""
    from accel_pipeline.quick import run_quick_check

    quick = run_quick_check(ctx.pipeline_config, ctx.workload, ctx.query)
    if quick.status != "fail":
        return None
    text = render_failure(quick.failure) if quick.failure else quick.summary
    return GateResult("quick_check", "fail", {"status": "fail", "reason": quick.summary, "log_tail": text})


def _record(ctx: RepairCtx, failure: Failure) -> None:
    (ctx.art / LAST_FAILURE_FILE).write_text(render_failure(failure), encoding="utf-8")


def _clear_infra(ctx: RepairCtx, failure: Failure, used: int) -> None:
    """Infra failures cost no code budget: repair the shared tree if that is the cause, wait out a
    provider flake, and let the caller re-run."""
    ctx.log(f"infra failure ({failure.infra_reason}) in {failure.source}; retry {used} without the agent")
    from accel_pipeline.failure import handle_infra_failure

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
    if ctx.stage != "test_suite" and _revise_test_suite(ctx):
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
    from accel_pipeline.workload import analysis_artifact_path

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
    return int(session_mod._read(art / COUNT_FILE).get(scope(stage), 0))


def _claim(art: Path, stage: str, cap: int) -> int:
    used = consults_used(art, stage)
    if used >= cap:
        raise Unavailable(f"advisor limit reached ({cap} consults in {scope(stage)})")
    session_mod._write(art / COUNT_FILE, {**session_mod._read(art / COUNT_FILE), scope(stage): used + 1})
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
    from accel_pipeline.codex import StageRunRequest, create_agent_backend  # codex imports this module

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
    session_mod._write(art / REQUEST_FILE, {"id": request_id, "question": question})
    deadline = time.monotonic() + timeout_sec
    while time.monotonic() < deadline:
        response = session_mod._read(art / RESPONSE_FILE)
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
        request = session_mod._read(self._art / REQUEST_FILE)
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
        session_mod._write(self._art / RESPONSE_FILE, response)
        (self._art / REQUEST_FILE).unlink(missing_ok=True)

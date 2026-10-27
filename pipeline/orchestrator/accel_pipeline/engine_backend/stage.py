"""Engine backend onboarding stages (substrate-first, sequential layers), run under the repair loop.

One main agent session carries every layer (resumed; a fresh one starts from the backend digest when
the context cap is reached). A layer's gates and its repairs go through repair.run_with_repair; its
critic reviews in parallel with the gates and its findings come back as one batched repair round.
Finished steps are markers (engine_progress.Progress), so a relaunch continues where the run stopped.
"""

from __future__ import annotations

import dataclasses
import json
import threading
from pathlib import Path
from typing import Any, Callable

from accel_pipeline.engine_backend.backend_digest import (
    backend_diff,
    backend_digest,
    layer_contracts_digest,
    snapshot_backend,
)
from accel_pipeline.config import ModelConfig, PipelineConfig
from accel_pipeline.workload import load_run_workload, query_artifacts_dir
from accel_pipeline.engine_host import prepare_host_sdk_payload
from accel_pipeline.pack.engine_quarantine import apply_foreign_engine_quarantine, restore_quarantine
from accel_pipeline.failure import Failure, render_failure
from accel_pipeline.gates.run import format_reloop_failure_sections, GateResult
from accel_pipeline.playbook import append_experiment_log
from accel_pipeline.repair import RepairCtx, run_with_repair
from accel_pipeline.session import SessionStore
from accel_pipeline.stages.base import run_agent_stage
from accel_pipeline.engine_backend.architecture_audit import run_architecture_audit
from accel_pipeline.engine_backend.layer_critique import (
    canary_requests,
    format_layer_critique_feedback,
    load_layer_critique,
    run_layer_critique_agent,
)
from accel_pipeline.engine_backend.progress import Progress
from accel_pipeline.stages.gate_plan import run_post_stage_gates_collect

MAIN_ROLE = "main"


def implementation_layers(workload: dict) -> list[dict[str, Any]]:
    return [dict(layer) for layer in (workload.get("implementation_layers") or [])]


def layer_stage_name(layer_id: str) -> str:
    return f"impl_{layer_id}"


def _prepare_engine_host_sdk(
    pipeline_config: PipelineConfig,
    workload: dict,
    query: int,
) -> None:
    payload = prepare_host_sdk_payload(
        workload, pipeline_config.repo_root, pipeline_config.build_dir
    )
    if payload is None:
        return
    art = query_artifacts_dir(pipeline_config.repo_root, query, workload=workload)
    art.mkdir(parents=True, exist_ok=True)
    (art / "host_sdk_discovery.json").write_text(
        json.dumps(payload, indent=2) + "\n", encoding="utf-8"
    )


def _engine_exists(pipeline_config: PipelineConfig, workload: dict) -> bool:
    targets = workload.get("target_paths") or {}
    header = targets.get("host_header")
    if not header:
        return False
    return (pipeline_config.repo_root / header).is_file()


def _layer_should_run(workload: dict, layer: dict) -> bool:
    skip_unless = str(layer.get("skip_unless") or "")
    if skip_unless == "requires_in_process_host":
        return bool(workload.get("requires_in_process_host", False))
    return True


def _user_message(
    pipeline_config: PipelineConfig,
    workload: dict,
    stage: str,
    *,
    extra: str = "",
    layer: dict | None = None,
) -> str:
    engine = workload.get("engine_name", "engine")
    exists = _engine_exists(pipeline_config, workload)
    mode = (
        "update_existing"
        if exists and workload.get("allow_update_existing", True)
        else "generate_from_scratch"
    )
    base = (
        f"Engine backend stage={stage} engine={engine} mode={mode}. "
        "Implement this engine's native slot implementations (S1-S8 of the frozen substrate "
        "flow) from templates and the engine profile; read no other engine's tree. "
        "Follow pipeline/prompts/engine_backend/layers.md."
    )
    if layer:
        gate = layer.get("gate")
        base += (
            f" This layer is `{layer.get('id')}` ({layer.get('title')}): ship its objects plus "
            f"`{layer.get('harness')}`. MCP tools on server `accel_pipeline` (tools, not resources): "
            f"`run_engine_build()` and `run_layer_gate()` (or `request_gate` `engine_build`, "
            f"`{gate}`) after each increment until both pass; not host cmake or `docker exec` in their "
            f"place. host_runtime.kind=`{(workload.get('host_runtime') or {}).get('kind') or 'host_cmake'}`. "
            "Do not implement later layers' contracts, but the wiring this layer's contract needs "
            "(registration, SQL that host.sh installs, a binding into another layer's object) is this "
            "layer's work wherever the file lives. Reuse prior layers; patch one only for a real bug "
            "that blocks this layer, then re-gate it. You run unattended: make the change."
        )
    if extra.strip():
        return base + "\n\n" + extra.strip()
    return base


def _layer_extra_context(workload: dict, layer: dict, repo_root: Path) -> dict[str, str]:
    prior = []
    for item in implementation_layers(workload):
        if item.get("id") == layer.get("id"):
            break
        prior.append(f"- `{item.get('id')}` ({item.get('title')}): `{item.get('harness')}`")
    host = dict(workload.get("host_runtime") or {})
    contracts = "pipeline/prompts/engine_backend/layer_contracts.md"
    return {
        "layer_contracts": layer_contracts_digest(
            (Path(repo_root) / contracts).read_text(encoding="utf-8"), str(layer.get("id") or ""), contracts
        ),
        "layer_id": str(layer.get("id") or ""),
        "layer_title": str(layer.get("title") or ""),
        "layer_gate": str(layer.get("gate") or ""),
        "layer_harness": str(layer.get("harness") or ""),
        "layer_cmake_target": str(layer.get("cmake_target") or ""),
        "prior_layers": "\n".join(prior) or "- (this is the first layer)",
        "host_runtime_kind": str(host.get("kind") or "host_cmake"),
        "layer_rules": ", ".join(str(r) for r in (layer.get("rules") or [])) or "(none)",
    }


_GATE_LOG_TAIL_CHARS = 8000


def _feedback_path(art: Path, lid: str) -> Path:
    return art / f"layer_{lid}.feedback.md"


def _carried_layer_feedback(art: Path, lid: str) -> str:
    """The feedback the previous run of this layer stopped on (a relaunch starts from it), or ""
    when that run passed or there is none."""
    path = _feedback_path(art, lid)
    if Progress(art).layer_passed(lid) or not path.is_file():
        return ""
    return "## Carried over: the previous run of this layer stopped on this feedback\n\n" + path.read_text(encoding="utf-8")


def format_layer_gate_feedback(
    failed: GateResult,
    *,
    layer_id: str,
    round_idx: int,
    max_rounds: int,
) -> str:
    data = failed.data or {}
    path = str(failed.path) if failed.path else ""
    lines = [
        "## Layer gate FAILED — fix from this trace; this stage will re-run implementation and gates",
        "",
        f"- layer: `{layer_id}`",
        f"- gate: `{failed.name}`",
        f"- status: `{failed.status}`",
        f"- attempt: {round_idx}/{max_rounds}",
    ]
    if path:
        lines.append(f"- gate json: `{path}`")
    lines.extend(format_reloop_failure_sections(data, max_chars=_GATE_LOG_TAIL_CHARS))
    lines.extend(
        [
            "",
            "Fix the adapter, harness, and/or CMake so this gate passes. "
            "Mid-loop MCP `request_gate` `engine_build` then this layer's gate "
            "until both are green. Do not start a later layer. Do not ignore this log.",
        ]
    )
    return "\n".join(lines)


def _capped(pipeline_config: PipelineConfig, workload: dict) -> PipelineConfig:
    """The main session's context cap (`main_session_max_context_tokens`): past it a fresh session
    starts from the backend digest."""
    cap = int(workload.get("main_session_max_context_tokens") or 0)
    if not cap:
        return pipeline_config
    return dataclasses.replace(pipeline_config, session=dataclasses.replace(pipeline_config.session, max_context_tokens=cap))


def _gate_runner(pipeline_config, workload_name, query, change_kind, iteration: Callable[[], int],
                 after_gate: Callable[[str], None] | None = None):
    """The repair loop's gate runner: the gates one at a time, in order, stopping at the first failure."""

    def run(names: list[str]) -> tuple[list[GateResult], GateResult | None]:
        results: list[GateResult] = []
        for name in names:
            batch, failed = run_post_stage_gates_collect(
                pipeline_config, workload_name, query, [name], change_kind=change_kind, iteration=iteration())
            results += batch
            if failed is not None:
                return results, failed
            if after_gate is not None:
                after_gate(name)
        return results, None

    return run


def layer_gates(layer: dict) -> list[str]:
    return [g for g in ("engine_build", str(layer.get("gate") or ""), *(layer.get("extra_gates") or [])) if g]


def _agent_turns(pipeline_config, model_config, workload_name, query, model_overrides, stage, turn):
    """A stage that is one agent turn, under the repair loop: a turn that fails (a deadline, a provider
    error) is resumed in its session with the failure as the message; past the budget the run stops cleanly."""
    pending = {"feedback": ""}

    def attempt():
        text, pending["feedback"] = pending["feedback"], ""
        return turn(text)

    def note(failure: Failure) -> None:
        pending["feedback"] = render_failure(failure)

    ctx = RepairCtx(pipeline_config, model_config, workload_name, query, stage=stage, model_overrides=model_overrides,
                    repair_turn=note)
    return run_with_repair(ctx, attempt)


def run_analysis(pipeline_config, model_config, workload_name, query, model_overrides, dry_run):
    workload = load_run_workload(pipeline_config, workload_name)
    art = query_artifacts_dir(pipeline_config.repo_root, query, workload=workload)
    progress = Progress(art)
    if not dry_run and progress.done("analysis"):
        return None

    def turn(feedback: str):
        return run_agent_stage(
            stage="analysis",
            prompt_file="01_analysis.md",
            pipeline_config=pipeline_config,
            model_config=model_config,
            workload_name=workload_name,
            query=query,
            user_message=_user_message(pipeline_config, workload, "analysis", extra=feedback),
            model_overrides=model_overrides,
            dry_run=dry_run,
            session_role="analysis",
        )

    if dry_run:
        return turn("")
    result = _agent_turns(pipeline_config, model_config, workload_name, query, model_overrides, "analysis", turn)
    progress.mark("analysis")
    return result


class _Critic:
    """A layer's critic, reviewing in a thread while the layer's gates run."""

    def __init__(self, run: "_LayerRun") -> None:
        self.run = run
        self.report: dict[str, Any] | None = None
        self.thread: threading.Thread | None = None

    def start(self) -> None:
        if self.thread is None:
            self.thread = threading.Thread(target=self._review, name=f"critic-{self.run.lid}", daemon=True)
            self.thread.start()

    def _review(self) -> None:
        run = self.run
        diff = backend_diff(run.pc.repo_root, run.engine, run.art / f"layer_{run.lid}.before")
        diff_path = run.art / "layer_diffs" / f"{run.lid}.diff"
        diff_path.parent.mkdir(parents=True, exist_ok=True)
        diff_path.write_text(diff, encoding="utf-8")
        brief = (
            backend_digest(run.pc.repo_root, run.engine, run.art)
            + f"### This layer's diff (also at `{diff_path}`)\n\n```diff\n{diff[:30000]}\n```\n"
        )
        self.report = run.critique(round_idx=1, brief=brief)

    def join(self) -> dict[str, Any] | None:
        if self.thread is not None:
            self.thread.join()
        return self.report


class _LayerRun:
    """One layer of the backend, in the main session: the agent's turns, the gates and their
    repairs, the critic and its batched findings."""

    def __init__(self, pipeline_config, model_config, workload_name, query, model_overrides, layer, workload):
        self.pc = _capped(pipeline_config, workload)
        self.mc = model_config
        self.workload_name = workload_name
        self.workload = workload
        self.query = query
        self.overrides = model_overrides
        self.layer = layer
        self.lid = str(layer.get("id") or "")
        self.engine = str(workload.get("engine_name") or "")
        self.stage = layer_stage_name(self.lid)
        self.art = query_artifacts_dir(pipeline_config.repo_root, query, workload=workload)
        self.art.mkdir(parents=True, exist_ok=True)
        self.extra_context = _layer_extra_context(workload, layer, pipeline_config.repo_root)
        self.base_extra = ""
        self.critic: _Critic | None = None

    def log(self, line: str) -> None:
        append_experiment_log(self.pc.repo_root, self.query, f"- layer `{self.lid}` {line}", workload=self.workload)

    def turn(self, feedback: str = "", *, dry_run: bool = False) -> Any:
        """One main-session turn. In a session that already worked on this layer only the feedback
        is sent; in one that has not (the next layer) the layer's first message; with no session
        the first message after the backend digest."""
        store = SessionStore(self.art)
        sid = store.session_id(self.query, MAIN_ROLE)
        if sid and self.stage in store.phases(self.query, MAIN_ROLE):
            text = feedback.strip() or "Continue this layer: run the gates and fix what is red."
            message = text + f"\n\nThis layer's gates: {', '.join(layer_gates(self.layer))}."
        else:
            extra = "\n\n".join(x for x in (self.base_extra, feedback) if x.strip())
            message = ("" if sid else self.digest()) + _user_message(
                self.pc, self.workload, self.stage, extra=extra, layer=self.layer)
        return run_agent_stage(
            stage=self.stage,
            prompt_file=str(self.layer.get("prompt") or "02_implementation.md"),
            pipeline_config=self.pc,
            model_config=self.mc,
            workload_name=self.workload_name,
            query=self.query,
            user_message=message,
            model_overrides=self.overrides,
            extra_context=self.extra_context,
            dry_run=dry_run,
            session_role=MAIN_ROLE,
            handoff=self.digest,
        )

    def digest(self) -> str:
        return backend_digest(self.pc.repo_root, self.engine, self.art, next_layer=self.lid)

    def critique(self, *, round_idx: int, brief: str) -> dict[str, Any]:
        try:
            run_layer_critique_agent(
                self.pc, self.mc, self.workload_name, self.query, self.overrides, False, self.layer,
                round_idx=round_idx, extra_context=self.extra_context, brief=brief,
            )
        except Exception as exc:  # noqa: BLE001 - a critic that cannot answer never stops the run
            self.log(f"critic round {round_idx} raised {type(exc).__name__}: {exc}")
        return load_layer_critique(self.art, self.lid)

    def converge(self, gates: list[str], feedback: str = "", *, critic: bool = False, rounds: int | None = None) -> Any:
        """The turn on `feedback`, then `gates`, repaired in this session until they pass (or the
        run stops). `critic`: start the layer's critic once the build is green."""
        pending = {"feedback": feedback}
        failures = {"n": 0}
        ctx = RepairCtx(self.pc, self.mc, self.workload_name, self.query, stage=self.stage, change_kind=self.stage,
                        model_overrides=self.overrides, max_code_rounds=rounds)

        def attempt() -> Any:
            text, pending["feedback"] = pending["feedback"], ""
            return self.turn(text)

        def repair(failure: Failure) -> None:
            failures["n"] += 1
            text = self.failure_feedback(ctx, failure, failures["n"])
            _feedback_path(self.art, self.lid).write_text(text + "\n", encoding="utf-8")
            self.log(f"{failure.source} failed (repair {failures['n']}); resuming the main session")
            if gates:
                self.turn(text)
            else:
                pending["feedback"] = text

        def after_gate(name: str) -> None:
            if critic and name == "engine_build" and self.critic is not None:
                self.critic.start()

        ctx.repair_turn = repair
        ctx.gate_runner = _gate_runner(self.pc, self.workload_name, self.query, self.stage,
                                       lambda: failures["n"], after_gate)
        result = run_with_repair(ctx, attempt, gates=gates)
        _feedback_path(self.art, self.lid).unlink(missing_ok=True)
        return result

    def failure_feedback(self, ctx: RepairCtx, failure: Failure, round_idx: int) -> str:
        failed = next((r for r in ctx.results if f"gate:{r.name}" == failure.source and r.status != "pass"), None)
        if failed is None:
            return render_failure(failure)
        text = format_layer_gate_feedback(failed, layer_id=self.lid, round_idx=round_idx,
                                          max_rounds=ctx.max_code_rounds or ctx.pipeline_config.repair.max_code_rounds)
        return text + "".join(f"\n- {d}" for d in failure.diagnosis if d.startswith("racy"))

    def run(self, gates: list[str], *, carried: str, with_critic: bool) -> Any:
        progress = Progress(self.art)
        progress.reopen_layer(self.lid)
        snapshot_backend(self.pc.repo_root, self.engine, self.art / f"layer_{self.lid}.before")
        if with_critic:
            self.critic = _Critic(self)
        result = self.converge(gates, carried, critic=with_critic, rounds=self.rounds())
        if with_critic:
            self.review(gates)
        (self.art / "layer_diffs").mkdir(exist_ok=True)
        (self.art / "layer_diffs" / f"{self.lid}.diff").write_text(
            backend_diff(self.pc.repo_root, self.engine, self.art / f"layer_{self.lid}.before"), encoding="utf-8")
        progress.mark_layer(self.lid)
        return result

    def rounds(self) -> int | None:
        configured = int(self.workload.get("layer_gate_max_rounds") or 0)
        return configured or None

    def review(self, gates: list[str]) -> None:
        """The critic's findings, all at once, to the main session; then the critic re-checks only those."""
        assert self.critic is not None
        report = self.critic.join() or load_layer_critique(self.art, self.lid)
        max_rounds = int(self.workload.get("layer_critique_max_rounds") or 1)
        for round_idx in range(1, max_rounds + 1):
            self.record_canary_requests(report)
            if report["status"] != "fail":
                self.log(f"critique round {round_idx}: {report['status'].upper()}")
                return
            feedback = format_layer_critique_feedback(report, round_idx=round_idx)
            (self.art / f"layer_critique_{self.lid}_feedback_round{round_idx}.md").write_text(feedback + "\n", encoding="utf-8")
            self.log(f"critique round {round_idx}: FAIL; one batched repair round in the main session")
            self.converge(gates, feedback, rounds=self.rounds())
            report = self.critique(round_idx=round_idx + 1, brief=feedback)
        self.record_canary_requests(report)
        if report["status"] == "fail":
            (self.art / f"layer_{self.lid}.open_findings.md").write_text(
                format_layer_critique_feedback(report, round_idx=max_rounds + 1) + "\n", encoding="utf-8")
            self.log(f"accepted with open findings after {max_rounds} repair round(s) (for the architecture audit)")

    def record_canary_requests(self, report: dict[str, Any]) -> None:
        requests = canary_requests(report)
        if not requests:
            return
        with (self.art / "canary_requests.md").open("a", encoding="utf-8") as out:
            for item in requests:
                out.write(f"- layer `{self.lid}`: {item.get('contract_item') or item.get('rule')}: "
                          f"{item.get('expected') or item.get('evidence')}\n")


def run_implementation_layer(
    pipeline_config,
    model_config,
    workload_name,
    query,
    model_overrides,
    dry_run,
    layer: dict,
    *,
    skip_gates=False,
    extra_user_message: str = "",
):
    workload = load_run_workload(pipeline_config, workload_name)
    run = _LayerRun(pipeline_config, model_config, workload_name, query, model_overrides, layer, workload)
    run.base_extra = extra_user_message.strip()
    if dry_run:
        return run.turn(dry_run=True)
    gates = [] if skip_gates else layer_gates(layer)
    with_critic = bool(gates) and int(workload.get("layer_critique_max_rounds") or 0) > 0
    return run.run(gates, carried=_carried_layer_feedback(run.art, run.lid), with_critic=with_critic)


def repair_layer(pipeline_config, model_config, workload_name, query, model_overrides, layer_id: str, feedback: str,
                 *, skip_gates: bool = False) -> None:
    """Fixes found after a layer passed (audit, validation): the layer's own session turn on
    `feedback`, then its gates. Whatever judged the old code is stale afterwards."""
    workload = load_run_workload(pipeline_config, workload_name)
    layer = next((l for l in implementation_layers(workload) if str(l.get("id")) == layer_id), None)
    if layer is None:
        raise ValueError(f"unknown layer {layer_id!r}")
    run = _LayerRun(pipeline_config, model_config, workload_name, query, model_overrides, layer, workload)
    run.converge([] if skip_gates else layer_gates(layer), feedback, rounds=run.rounds())
    Progress(run.art).invalidate_gates()


def run_implementation(
    pipeline_config,
    model_config,
    workload_name,
    query,
    model_overrides,
    dry_run,
    *,
    skip_gates=False,
    extra_user_message: str = "",
    only_layer_id: str | None = None,
    from_layer_id: str | None = None,
):
    workload = load_run_workload(pipeline_config, workload_name)
    art = query_artifacts_dir(pipeline_config.repo_root, query, workload=workload)
    known = {str(layer.get("id") or "") for layer in implementation_layers(workload)}
    if from_layer_id and from_layer_id not in known:
        from_layer_id = None
    started = from_layer_id is None
    result = None
    for layer in implementation_layers(workload):
        if not _layer_should_run(workload, layer):
            continue
        lid = str(layer.get("id") or "")
        if only_layer_id and lid != only_layer_id:
            continue
        if from_layer_id and not started:
            if lid == from_layer_id:
                started = True
            else:
                continue
        if not only_layer_id and not dry_run and Progress(art).layer_passed(lid):
            continue
        result = run_implementation_layer(
            pipeline_config,
            model_config,
            workload_name,
            query,
            model_overrides,
            dry_run,
            layer,
            skip_gates=skip_gates,
            extra_user_message=extra_user_message,
        )
    return result


def validation_owner(gate: str, data: dict[str, Any], workload: dict) -> str:
    """The layer whose main session fixes a failing validation gate (or `validation`, the session
    that wrote the adversarial tests): the gate's own report, the configured owner, the layer whose
    gate it is, else the last layer."""
    layers = implementation_layers(workload)
    ids = [str(l.get("id")) for l in layers]
    configured = (workload.get("validation_gate_owners") or {}).get(gate)
    own = next((str(l.get("id")) for l in layers if l.get("gate") == gate), "")
    for candidate in (str((data or {}).get("owner_layer") or ""), str(configured or ""), own):
        if candidate in ids or candidate == VALIDATION_ROLE:
            return candidate
    return ids[-1]


VALIDATION_ROLE = "validation"


def run_validation(
    pipeline_config, model_config, workload_name, query, model_overrides, dry_run, *, skip_gates=False
):
    workload = load_run_workload(pipeline_config, workload_name)
    pipeline_config = _capped(pipeline_config, workload)
    art = query_artifacts_dir(pipeline_config.repo_root, query, workload=workload)
    progress = Progress(art)

    def validation_turn(message: str) -> Any:
        return run_agent_stage(
            stage="validation",
            prompt_file="03_validation.md",
            pipeline_config=pipeline_config,
            model_config=model_config,
            workload_name=workload_name,
            query=query,
            user_message=message,
            model_overrides=model_overrides,
            dry_run=dry_run,
            session_role=VALIDATION_ROLE,
        )

    if dry_run:
        return validation_turn(_user_message(pipeline_config, workload, "validation"))
    result = None
    if not progress.done("validation_authored"):
        result = _agent_turns(
            pipeline_config, model_config, workload_name, query, model_overrides, "validation",
            lambda feedback: validation_turn(_user_message(pipeline_config, workload, "validation", extra=feedback)))
        progress.mark("validation_authored")
    if skip_gates:
        return result
    gates = list(workload.get("post_validation_gates") or [])
    _run_validation_gates(pipeline_config, model_config, workload_name, workload, query, model_overrides,
                          gates, validation_turn)
    return result


def _run_validation_gates(pipeline_config, model_config, workload_name, workload, query, model_overrides, gates,
                          validation_turn) -> None:
    """Each gate under the repair loop. A failure goes to the owning layer's session (or the
    validation session) as a Failure; after the fix every gate runs again, cheap ones first."""
    art = query_artifacts_dir(pipeline_config.repo_root, query, workload=workload)
    progress = Progress(art)
    for gate in gates:
        if progress.gate_passed(gate):
            continue
        ctx = RepairCtx(pipeline_config, model_config, workload_name, query, stage="validation", change_kind=gate,
                        model_overrides=model_overrides)

        def route(failure: Failure, ctx=ctx, gate=gate) -> None:
            failed = next((r for r in ctx.results if r.name == gate and r.status != "pass"), None)
            owner = validation_owner(gate, failed.data if failed else {}, workload)
            ctx.log(f"validation gate `{gate}` failed; fix goes to `{owner}`")
            feedback = "## Validation gate failed\n\n" + render_failure(failure)
            if owner == VALIDATION_ROLE:
                validation_turn(feedback)
            else:
                repair_layer(pipeline_config, model_config, workload_name, query, model_overrides, owner, feedback)
            progress.invalidate_gates()

        ctx.repair_turn = route
        ctx.gate_runner = _gate_runner(pipeline_config, workload_name, query, gate, lambda: 0)
        run_with_repair(ctx, lambda: None, gates=[gate])
        progress.mark_gate(gate)


def run_engine_pipeline(
    pipeline_config: PipelineConfig,
    model_config: ModelConfig,
    workload_name: str,
    query: int,
    model_overrides: dict[str, str],
    dry_run: bool,
    *,
    stage: str = "all",
    skip_gates: bool = False,
) -> None:
    workload = load_run_workload(pipeline_config, workload_name)
    _prepare_engine_host_sdk(pipeline_config, workload, query)
    qsession = None if dry_run else apply_foreign_engine_quarantine(
        pipeline_config.repo_root, workload
    )
    args = (pipeline_config, model_config, workload_name, query, model_overrides, dry_run)
    try:
        if stage == "analysis":
            run_analysis(*args)
        elif stage.startswith("impl_"):
            run_implementation(*args, skip_gates=skip_gates, only_layer_id=stage[len("impl_") :])
        elif stage == "implementation":
            run_implementation(*args, skip_gates=skip_gates)
            if not skip_gates:
                _audit(*args)
        elif stage == "architecture_audit":
            _audit(*args, skip_gates=skip_gates)
        elif stage in ("validation", "audit"):
            run_validation(*args, skip_gates=skip_gates)
        else:
            run_analysis(*args)
            run_implementation(*args, skip_gates=skip_gates)
            _audit(*args, skip_gates=skip_gates)
            run_validation(*args, skip_gates=skip_gates)
    finally:
        restore_quarantine(qsession)


def _audit(pipeline_config, model_config, workload_name, query, model_overrides, dry_run, *, skip_gates=False):
    workload = load_run_workload(pipeline_config, workload_name)
    art = query_artifacts_dir(pipeline_config.repo_root, query, workload=workload)
    progress = Progress(art)
    if not dry_run and progress.done("architecture_audit"):
        return
    audit = run_architecture_audit(
        pipeline_config, model_config, workload_name, query, model_overrides, dry_run,
        repair_layer=lambda lid, feedback: repair_layer(
            pipeline_config, model_config, workload_name, query, model_overrides, lid, feedback,
            skip_gates=skip_gates),
    )
    if not dry_run:
        progress.mark("architecture_audit")
    return audit

"""Stages of the Verus accelerator pipeline (``--mode verus``).

    preflight     seeds and inputs present; the workspace (clean room)
    spec          freeze A: the spec from the SQL alone; faithfulness audit
    analysis      aux-state design + declared kernel inputs (q.inputs); critic
    freeze        freeze B: obligations, store adapter, C ABI (generated);
                  obligations faithfulness audit
    tests         adversarial tests (frozen before the kernel exists)
    glue          the host glue (an agent writes it; trusted): build, contract
                  check against the declared inputs, glue audit; frozen
    kernel        the kernel and its proofs
    integration   frozen tests on the Verus global store; the tests' fault self-check
    optimization  A/B loop on the Verus global store (stopping.py)
    audit         trusted base, leaks, C++-store compatibility, report

Hard gates are correctness and trust only.  Scaling is guidance in the
prompts (contracts/scale_principles.md).  Every agent step is codex exec.
"""

from __future__ import annotations

import json
import random
import re
import shutil
import statistics
import time
import threading
from concurrent.futures import ThreadPoolExecutor, as_completed
from dataclasses import asdict, dataclass, field
from pathlib import Path
from typing import Any

from accel_pipeline.verus import actions, workspace
from accel_pipeline.verus import params as qparams
from accel_pipeline.verus.agent import json_block, render, run_agent
from accel_pipeline.verus.context import IGNORED_WORKLOAD_FIELDS, VerusContext
from accel_pipeline.verus.stopping import Attempt, StopConfig, StopState, should_stop, uncovered

_STATE_LOCK = threading.RLock()

STAGES = ("preflight", "spec", "analysis", "freeze", "tests", "glue", "kernel", "integration", "optimization",
          "audit")


_TRANSIENT = re.compile(r"\b(402|429|500|502|503|504|529)\b|Payment Required|rate.?limit|overloaded|temporarily|unavailable|"
                        r"stream (error|disconnected)|connection (reset|refused|closed)",
                        re.I)


class PipelineStop(RuntimeError):
    """The run cannot continue (reason in the message; state is saved)."""


@dataclass
class VerusState:
    stage: str = "preflight"
    frozen: dict[str, str] = field(default_factory=dict)
    tests_hash: str = ""
    kernel_session: str = ""  # the kernel agent's codex session (gate failures go back to it)
    kernel_restarts: int = 0  # fresh, simpler kernels after a design proved too hard to prove
    done: list[str] = field(default_factory=list)  # stages already run inside an earlier parallel group
    opt: dict[str, Any] = field(default_factory=dict)
    notes: list[str] = field(default_factory=list)

    @staticmethod
    def path(ctx: VerusContext) -> Path:
        return ctx.art / "verus_state.json"

    @classmethod
    def load(cls, ctx: VerusContext) -> "VerusState":
        p = cls.path(ctx)
        if p.is_file():
            data = json.loads(p.read_text(encoding="utf-8"))
            return cls(**{k: v for k, v in data.items() if k in cls.__dataclass_fields__})
        return cls()

    def save(self, ctx: VerusContext) -> None:
        ctx.art.mkdir(parents=True, exist_ok=True)
        self.path(ctx).write_text(json.dumps(asdict(self), indent=2), encoding="utf-8")


@dataclass
class Run:
    """What every stage needs."""

    ctx: VerusContext
    state: VerusState
    workload: dict[str, Any]
    pipeline_config: Any
    model_config: Any
    model_overrides: dict[str, str]
    dry_run: bool

    def agent(self, stage: str, prompt: str, task: str, extra: dict[str, str] | None = None,
              timeout_sec: int | None = None, resume: str = "", tolerate_timeout: bool = False):
        """`tolerate_timeout`: a step that runs out of time returns (its work is then judged by
        the gates, and its session can be continued) instead of stopping the pipeline."""
        retries = int(self.ctx.setting("agent_retries", 2))
        for attempt in range(retries + 1):
            res = run_agent(self.ctx, stage=stage, prompt=prompt, task=task, pipeline_config=self.pipeline_config,
                            model_config=self.model_config, model_overrides=self.model_overrides, extra=extra,
                            dry_run=self.dry_run, timeout_sec=timeout_sec, resume=resume)
            if res.ok or res.timed_out or attempt == retries or not _TRANSIENT.search(res.error or ""):
                break
            # a provider hiccup (rate limit, overload, dropped stream): wait, then the same
            # step again (a session that got going is continued, not restarted)
            resume = res.session_id or resume
            self.note(f"{stage}: transient agent failure, retry {attempt + 1}/{retries}: {res.error[:200]}")
            # out of provider credits (402) waits longer: room for a top-up or in-flight requests to settle
            time.sleep((900 if "402" in (res.error or "") else 120) * (attempt + 1))
        if not res.ok and res.timed_out and tolerate_timeout:
            self.note(f"{stage}: the step ran out of time; judging what it left")
            return res
        if not res.ok:
            raise PipelineStop(f"{stage} agent failed: {res.error[:2000]}")
        return res

    def note(self, msg: str) -> None:
        with _STATE_LOCK:
            print(f"[verus] {msg}", flush=True)
            self.state.notes.append(f"{time.strftime('%H:%M:%S')} {msg}")
            self.state.save(self.ctx)

    def save(self) -> None:
        with _STATE_LOCK:
            self.state.save(self.ctx)

    def parallel(self, jobs: dict[str, Any]) -> None:
        """Run independent jobs (name -> fn(run)) concurrently; then raise the first failure."""
        if self.dry_run or len(jobs) == 1 or not self.ctx.setting("parallel_stages", True):
            for fn in jobs.values():
                fn(self)
            return
        self.note("in parallel: " + ", ".join(jobs))
        errors: dict[str, BaseException] = {}
        with ThreadPoolExecutor(max_workers=len(jobs)) as pool:
            futures = {pool.submit(fn, self): name for name, fn in jobs.items()}
            for fut in as_completed(futures):
                name = futures[fut]
                exc = fut.exception()
                if exc is not None:
                    errors[name] = exc
                    self.note(f"{name} failed: {str(exc)[:300]}")
                else:
                    self.note(f"{name} done")
        for name in jobs:  # report in stage order
            if name in errors:
                exc = errors[name]
                raise exc if isinstance(exc, PipelineStop) else PipelineStop(f"{name}: {exc!r}")

    def mark_done(self, *stages: str) -> None:
        with _STATE_LOCK:
            self.state.done = sorted(set(self.state.done) | set(stages))
            self.state.save(self.ctx)

    def already_done(self, stage: str) -> bool:
        if stage in self.state.done:
            self.note(f"{stage}: already done (ran in parallel with an earlier stage)")
            return True
        return False

    def gate(self, name: str, data: dict[str, Any]) -> dict[str, Any]:
        actions.write_result(self.ctx, name, data)
        return data


def _brief(d: dict[str, Any], n: int = 6000) -> str:
    return json.dumps(d, indent=2)[:n]


# ───────────────────────────── stages ─────────────────────────────


def preflight(r: Run) -> None:
    ctx = r.ctx
    problems = []
    for what, p in (("query SQL ($n parameters)", ctx.query_sql), ("schema", ctx.schema_sql),
                    ("test database", ctx.test_db), ("bench database", ctx.bench_db)):
        if not p or not Path(p).exists():
            problems.append(f"{what}: {p or '(unset)'} not found (set verus.{what.split()[0]}... in the workload)")
    if not (Path(ctx.build_dir) / "CMakeCache.txt").is_file() and not r.dry_run:
        Path(ctx.build_dir).mkdir(parents=True, exist_ok=True)
        with actions.file_lock(Path(ctx.build_dir) / ".verus_build.lock"):  # parallel pipelines, one tree
            done = (Path(ctx.build_dir) / "CMakeCache.txt").is_file()
            cfg = {"ok": True} if done else actions.configure_build(ctx)
        if not cfg["ok"]:
            problems.append(f"build dir {ctx.build_dir}: cmake configure (DuckDB engine) failed: {cfg['error']}")
    if Path(ctx.query_sql or "/nonexistent").is_file() and (why := qparams.check_query(ctx)):
        problems.append(why)
    verus_bin = Path(str(ctx.setting("verus_bin") or ""))
    if not verus_bin.is_file():
        problems.append(f"Verus binary {verus_bin} missing (set VERUS_BIN or verus.verus_bin)")
    if str(ctx.setting("sandbox", "bwrap")) == "bwrap" and not r.dry_run and not problems:
        from accel_pipeline.verus import sandbox
        check = sandbox.selfcheck(ctx)
        r.note(f"sandbox (bwrap): {check['probed']} paths probed, visible {check['visible']}, missing {check['missing']}")
        if not check["ok"]:
            problems.append(f"agent sandbox leaks or lacks something: visible={check['visible']} "
                            f"missing={check['missing']} {check['stderr'][-500:]}")
    if not problems and not r.dry_run:
        built = actions.ensure_runner(ctx)  # the agents' SQL tool runs it from the first stage
        if not built["ok"]:
            problems.append(f"verus_kit_runner does not build: {built['error'][-500:]}")
    if problems and not r.dry_run:
        raise PipelineStop("preflight: " + "; ".join(problems))
    ignored = [k for k in IGNORED_WORKLOAD_FIELDS if r.workload.get(k)]
    if ignored:
        r.note(f"ignored under --mode verus (C++ pipeline context): {', '.join(ignored)}")
    workspace.create(ctx, r.workload)
    if not r.dry_run:
        actions.sql2verus_bin(ctx)
    r.note(f"workspace {ctx.workspace}; crate {ctx.crate}; accelerator {ctx.accel_id}")


def spec(r: Run) -> None:
    res = actions.freeze_spec(r.ctx)
    if not res["ok"]:
        raise PipelineStop(f"sql2verus refused the query: {res['error']}")
    r.state.save(r.ctx)
    key = _spec_audit_key(r.ctx)
    reused = _reused_spec_audit(r.ctx, key) if r.ctx.setting("stages.reuse_spec_audit", True) else None
    if reused is not None:
        (r.ctx.ws / "analysis").mkdir(parents=True, exist_ok=True)
        (r.ctx.ws / "analysis" / "spec_audit.json").write_text(json.dumps(reused, indent=2), encoding="utf-8")
        r.note("spec audit reused: an accepted audit of this exact SQL, schema and generated spec")
        return
    # the audit reads only the SQL and the spec: the analysis (a later stage) runs meanwhile
    r.parallel({"spec_audit": lambda run: _spec_audit(run, key), "analysis": _analysis_and_mark})


def _analysis_and_mark(r: Run) -> None:
    _analysis(r)
    r.mark_done("analysis")


def _spec_audit(r: Run, key: str) -> None:
    r.agent("verus_spec_audit", "01_spec_audit.md",
            "Audit spec/gen_spec.rs against query/query.sql; write analysis/spec_audit.json.")
    if r.dry_run:
        return
    audit = _read_json(r.ctx.ws / "analysis" / "spec_audit.json")
    if audit.get("verdict") != "ok":
        raise PipelineStop("the spec does not mean what the SQL means (sql2verus must be fixed, not the spec): "
                           + json.dumps(audit.get("issues") or audit)[:3000])
    _record_spec_audit(r.ctx, key, audit)


def _spec_audit_store(ctx: VerusContext) -> Path:
    return Path(ctx.pipeline_root) / "verus" / "spec_audits.jsonl"


def _spec_audit_key(ctx: VerusContext) -> str:
    """The spec audit judges exactly these: the SQL, the schema, the generated spec."""
    import hashlib

    h = hashlib.sha256()
    for rel in ("query/query.sql", "query/schema.sql", "spec/gen_spec.rs"):
        h.update(rel.encode() + b"\0" + (ctx.ws / rel).read_bytes() + b"\0")
    return h.hexdigest()


def _reused_spec_audit(ctx: VerusContext, key: str) -> dict[str, Any] | None:
    store = _spec_audit_store(ctx)
    if not store.is_file():
        return None
    for line in reversed(store.read_text(encoding="utf-8").splitlines()):
        try:
            rec = json.loads(line)
        except json.JSONDecodeError:
            continue
        if rec.get("key") == key and (rec.get("audit") or {}).get("verdict") == "ok":
            return {**rec["audit"], "reused_from": rec.get("run", "")}
    return None


def _record_spec_audit(ctx: VerusContext, key: str, audit: dict[str, Any]) -> None:
    store = _spec_audit_store(ctx)
    store.parent.mkdir(parents=True, exist_ok=True)
    with store.open("a", encoding="utf-8") as fh:
        fh.write(json.dumps({"key": key, "run": f"{ctx.workload_name} q{ctx.query} {time.strftime('%Y-%m-%d %H:%M')}",
                             "audit": audit}) + "\n")


REVISE_MESSAGE = """Your work did not pass yet:

{feedback}

Revise it (same files, same rules as before), check it with your tools, and end with the same kind
of summary as before."""


def _round(r: Run, stage: str, prompt: str, task: str, feedback: str, sid: str,
           extra: dict[str, str] | None = None) -> str:
    """One round of a write-check loop: the full prompt the first time; after a failed
    check, the feedback goes to the same agent session (its context kept).  Returns the
    session id."""
    if sid and feedback:
        res = r.agent(stage, "", REVISE_MESSAGE.format(feedback=feedback), resume=sid)
    else:
        res = r.agent(stage, prompt, task, {**(extra or {}), "feedback": feedback or "(none: first round)"})
    return res.session_id or sid


def analysis(r: Run) -> None:
    if r.already_done("analysis"):
        return
    _analysis(r)


def _analysis(r: Run) -> None:
    ctx = r.ctx
    revisions = int(ctx.setting("stages.analysis_revisions", 1))
    task = "Design the aux state and write analysis/analysis.json and inputs/q.inputs."
    feedback, sid = "", ""
    for round_ in range(revisions + 1):
        sid = _round(r, "verus_analysis", "02_analysis.md", task, feedback, sid)
        if r.dry_run:
            return
        check = r.gate("check_inputs", actions.check_inputs(ctx))
        if not check["ok"]:
            feedback = f"sql2verus refused inputs/q.inputs:\n{check['error']}"
            continue
        r.agent("verus_critic", "02b_critic.md", "Review the analysis; write analysis/critique.json.")
        crit = _read_json(ctx.ws / "analysis" / "critique.json")
        failed = _failed_checks(crit)
        if (crit.get("verdict") == "accept" and not failed) or round_ == revisions:
            r.note(f"analysis accepted after {round_ + 1} round(s) (critic: {crit.get('verdict', '?')}"
                   + (f"; failed checks left: {[c.get('id') for c in failed]}" if failed else "") + ")")
            return
        feedback = "The critic asks for a revision:\n" + json.dumps(failed or crit.get("objections") or crit, indent=2)
        if crit.get("estimates"):
            feedback += "\nIts SF3000 estimates of your design:\n" + json.dumps(crit["estimates"], indent=2)
    raise PipelineStop(f"analysis: inputs still refused after {revisions + 1} rounds: {feedback[:2000]}")


def _failed_checks(review: dict[str, Any]) -> list[dict[str, Any]]:
    """A checklist review's failed checks (any one rejects, whatever its overall verdict says)."""
    return [c for c in (review.get("checks") or []) if isinstance(c, dict) and c.get("ok") is False]


def freeze(r: Run) -> None:
    if not r.dry_run:
        res = actions.freeze_inputs(r.ctx)
        if not res["ok"]:
            raise PipelineStop(f"freeze B failed: {res['error']}")
        r.state.frozen = res["hashes"]
        r.save()
        r.note(f"frozen: {', '.join(sorted(r.state.frozen))}")

    def audit(run: Run) -> None:
        issues = _obligations_audit(run)
        if issues:
            raise PipelineStop("the generated obligations do not mean what the declared inputs say "
                               "(sql2verus must be fixed): " + issues)
        run.mark_done("obligations_audit")

    def tests_job(run: Run) -> None:
        _tests(run)
        run.mark_done("tests")

    def glue_job(run: Run) -> None:
        _glue(run)
        run.mark_done("glue")

    # independent after freeze B: they read the frozen inputs / header, write different dirs
    jobs = {"obligations_audit": audit, "tests": tests_job, "glue": glue_job}
    # on resume, skip what an earlier attempt of this group already finished
    todo = {name: fn for name, fn in jobs.items() if name not in r.state.done}
    for name in jobs.keys() - todo.keys():
        r.note(f"{name}: already done")
    if todo:
        r.parallel(todo)


def _obligations_audit(r: Run) -> str:
    """The obligations' faithfulness audit; '' if they are faithful."""
    r.agent("verus_obligations_audit", "01b_obligations_audit.md",
            "Audit frozen/gen_obligations.rs against inputs/q.inputs; write analysis/obligations_audit.json.")
    if r.dry_run:
        return ""
    audit = _read_json(r.ctx.ws / "analysis" / "obligations_audit.json")
    return "" if audit.get("verdict") == "ok" else json.dumps(audit.get("issues") or audit)[:3000]


def tests(r: Run) -> None:
    if r.already_done("tests"):
        return
    _tests(r)


def _tests(r: Run) -> None:
    ctx = r.ctx
    feedback, sid = "", ""
    for _ in range(3):
        sid = _round(r, "verus_tests", "03_tests.md", "Write the adversarial tests in tests/.", feedback, sid)
        if r.dry_run:
            return
        r.note(f"random parameter sets added to the tests: {actions.add_random_params(ctx)}")
        v = r.gate("validate_tests", actions.validate_tests(ctx))
        if v["ok"]:
            r.state.tests_hash = actions.tree_hash(ctx.ws / "tests")
            r.save()
            r.note(f"tests frozen: {v.get('params')} parameter tuples, {v.get('txns')} transactions")
            return
        feedback = "verus_validate_tests failed:\n" + _brief(v, 4000)
    raise PipelineStop(f"tests do not run on the database: {feedback[:2000]}")


def glue(r: Run) -> None:
    if r.already_done("glue"):
        return
    _glue(r)


def _glue(r: Run) -> None:
    ok, why = _write_glue(r, "Write the host glue in glue/glue.cpp.")
    if not ok:
        raise PipelineStop(why)
    if not r.dry_run:
        r.state.frozen.update(actions.glue_hashes(r.ctx))
        r.save()
        r.note("glue frozen: it builds, delivers the declared inputs, and passed its audit")


def _write_glue(r: Run, task: str) -> tuple[bool, str]:
    """The glue agent writes glue/; then build, contract check, glue audit (with fix rounds)."""
    ctx = r.ctx
    feedback, sid = "", ""
    rounds = int(ctx.setting("stages.glue_fix_rounds", 3))
    for k in range(rounds + 1):
        sid = _round(r, "verus_glue", "03c_glue.md", task, feedback, sid)
        if r.dry_run:
            r.agent("verus_glue_audit", "03d_glue_audit.md", "(dry run)")
            return True, ""
        b = r.gate("glue_build", actions.build_runner(ctx))
        if not b["ok"]:
            feedback = "The glue does not build:\n" + _brief(b, 6000)
            continue
        c = r.gate("glue_contract", actions.glue_contract(ctx))
        if not c["ok"]:
            feedback = "The glue contract check failed (what the glue delivered vs the declared SQL):\n" + _brief(c, 6000)
            continue
        r.agent("verus_glue_audit", "03d_glue_audit.md", "Audit glue/glue.cpp; write analysis/glue_audit.json.")
        audit = _read_json(ctx.ws / "analysis" / "glue_audit.json")
        failed = _failed_checks(audit)
        if audit.get("verdict") == "ok" and not failed:
            return True, ""
        feedback = "The glue audit found problems:\n" + json.dumps(failed or audit.get("issues") or audit, indent=2)[:6000]
        r.note(f"glue round {k + 1}: {feedback.splitlines()[0]}")
    return False, f"the host glue fails its checks after {rounds + 1} rounds: {feedback[:2000]}"


def _kernel_gates(r: Run, *, snapshots_too: bool = False, proofs: bool = True) -> dict[str, Any]:
    ctx = r.ctx
    changed = actions.check_frozen(ctx, r.state.frozen, r.state.tests_hash)
    if changed:
        return {"ok": False, "stage": "frozen", "changed": changed,
                "error": "frozen files changed (restore them; the spec, generated files and tests are fixed)"}
    tb = actions.trusted_base(ctx)
    if not tb["ok"]:
        return {"ok": False, "stage": "trusted_base", **tb}
    b = actions.build_runner(ctx, proofs=proofs)
    if not b["ok"]:
        return b
    c = actions.correctness(ctx, snapshots_too=snapshots_too)
    c["stage"] = "tests"
    c["verified"] = b.get("verified")
    c["verify_seconds"] = b.get("verify_seconds")
    if c["ok"]:
        # correct on the small test data: now also fast enough on SF1 (code first, and again once proved,
        # so a proof-driven rewrite cannot quietly make it slow)
        cost = r.gate("cost_check", actions.cost_check(ctx))
        if not cost["ok"]:
            return {**c, "ok": False, "stage": "cost", "cost": cost, "error": cost.get("error")}
        c["cost"] = cost
    return c


FIX_MESSAGE = """The gates failed after your last change (stage: {stage}). The failure:

{failure}

Fix it in crate/src (the frozen files, the glue and the tests stay as they are), check it with the
tools ({check}), and end with the same kind of summary as before. For proof failures,
contracts/proof_method.md maps each obligation to its standard proof and the verus_common lemmas;
work top-down from the failing top-level fact (build / apply / execute). Never stub out build / apply /
execute (e.g. an execute that always returns None) to make verification pass: the tests reject a kernel
that declines most reads."""


def _progress(gates: dict[str, Any]) -> tuple[int, int]:
    """How far a failing kernel is (larger is better): fewer failing gates' errors, then more
    verified.  A crate that does not compile (errors -1) ranks below every verifying one."""
    errors = int(gates.get("errors", -1) if gates.get("stage") == "verify" else 0)
    stage_rank = {"verify": 0, "build": 0, "tests": 1, "snapshots": 2, "cost": 3}.get(str(gates.get("stage")), 0)
    return (stage_rank, -(errors if errors >= 0 else 10**6), int(gates.get("verified") or 0))


ADVICE_MESSAGE = """Your fixes have not made progress for a few rounds, so a reviewer looked at the
problem afresh (it did not change any files). Its diagnosis and suggested approach:

{advice}

Weigh it against what you know (you have more context; it may be wrong in details), then fix the
kernel in crate/src. The current failure:

{failure}

Check it with the tools ({check}), and end with the same kind of summary as before."""


def _advice(r: Run, gates: dict[str, Any], why: str, proofs: bool) -> str:
    """A second opinion when fixes stall: a fresh, cheaper reviewer (verus_kernel_advisor)
    diagnoses the failure and suggests an approach; it may not change the workspace (any
    change is undone)."""
    ctx = r.ctx
    snap = workspace.snapshot_kernel(ctx, "before_advice")
    try:
        res = r.agent("verus_kernel_advisor", "04d_kernel_advice.md", "Diagnose the failure; write analysis/kernel_advice.md.",
                      {"failure": _brief(gates, 12000), "phase": "proofs" if proofs else "code (tests)"},
                      tolerate_timeout=True)
    finally:
        if workspace.differs(ctx, "before_advice"):
            r.note(f"{why}: the advisor changed files; undone")
        workspace.restore_kernel(ctx, "before_advice")
        shutil.rmtree(snap, ignore_errors=True)
    path = ctx.ws / "analysis" / "kernel_advice.md"
    text = path.read_text(encoding="utf-8") if path.is_file() else (res.final_text or "")
    return text.strip()[:8000]


def _fix_loop(r: Run, gates: dict[str, Any], rounds: int, *, snapshots_too: bool, why: str,
              session: tuple[str, str] = ("", ""), proofs: bool = True) -> dict[str, Any]:
    """Up to `rounds` fixes.  `session` = (stage, codex session id): the agent that wrote the
    code gets the failure as its next message (its context kept); without one, a fresh
    kernel-fix agent.

    A second opinion: after `stages.escalate_after_stalled_rounds` rounds in a row without
    progress (fewer errors, or more verified at the same count), a reviewer
    (verus_kernel_advisor, a different, cheaper model) diagnoses the failure in a fresh
    session; its advice goes to the same agent, with `stages.escalation_extra_rounds` more
    rounds."""
    stage, sid = session
    stall_limit = int(r.ctx.setting("stages.escalate_after_stalled_rounds", 2))
    extra = int(r.ctx.setting("stages.escalation_extra_rounds", 2))
    check = "verus_verify, verus_test" if proofs else "verus_test with proofs=false"
    best, stalled, advised, extended = _progress(gates), 0, False, False
    advice = ""
    k = 0
    while True:
        if gates["ok"]:
            break
        if k >= rounds:
            # out of rounds: a stall that has not had its second opinion yet gets one (below);
            # a kernel still improving in its last round gets one more block of rounds, once
            if not (not advised and stalled >= stall_limit):
                if extended or stalled > 0:
                    break
                extended, rounds = True, rounds + extra
                r.note(f"{why}: still improving after {k} rounds; {extra} more")
        if not advised and stalled >= stall_limit:
            advised, rounds = True, rounds + extra
            r.note(f"{why}: no progress in {stalled} rounds; asking the advisor for a second opinion (+{extra} rounds)")
            advice = _advice(r, gates, why, proofs)
        k += 1
        r.note(f"{why}: fix round {k}/{rounds} ({gates.get('stage')})" + (" [same session]" if sid else ""))
        failure = _brief(gates, 12000)
        if sid:
            msg = (ADVICE_MESSAGE.format(advice=advice, failure=failure, check=check) if advice
                   else FIX_MESSAGE.format(stage=gates.get("stage"), failure=failure, check=check))
            res = r.agent(stage, "", msg, resume=sid, tolerate_timeout=True)
            sid = res.session_id or sid
        else:
            r.agent("verus_kernel_fix", "04b_kernel_fix.md", "Make the kernel pass the gates.",
                    {"failure": failure + (f"\n\nA reviewer's diagnosis:\n{advice}" if advice else "")})
        advice = ""  # once: later rounds get the plain failure
        gates = r.gate("kernel_gates", _kernel_gates(r, snapshots_too=snapshots_too, proofs=proofs))
        now = _progress(gates)
        stalled, best = (0, now) if now > best else (stalled + 1, best)
    return gates


SIMPLER_NOTE = """This is a fresh start on the kernel. The previous attempt passed the tests but could not be proved
after its fix rounds and a reviewer's second opinion; it is archived ({archive}). Its last failure:

{failure}

Implement the state in analysis/analysis.json as plainly as possible, so that it can be proved:
* persistent maps keyed by tuples: the shared verified one in the proof library, or one proven in
  another query's kernel lifted into the library (see "Proven code in this tree"); when neither
  exists, a new generic one, published to the library. Not a vstd HashMap /
  Vec cloned per write (that is O(state) per transaction) and not a flat Vec searched linearly. No new
  custom hash tables, pages, dictionaries, compression, prefix-array encodings or multi-level bucket
  schemes unless the analysis requires one for a cost bound -- and then only that one, kept small;
* keep the analysis's read and write cost bounds: reads touch only what the query needs, writes only
  the keys they change;
* every structure's invariant is a sum (or count) over db, proved with contracts/proof_method.md's
  patterns: build = sum over the scans, apply = old + signed delta (bag_step at the same weight),
  execute = the sum over what it reads;
* small functions, each with its own lemma; prove one layer at a time (collections, build, apply,
  execute). Never stub out build / apply / execute."""


def _restart_kernel_simpler(r: Run, gates: dict[str, Any]) -> None:
    """Archive the kernel, reset crate/src to the frozen skeleton, and start over with a note asking for
    the plainest provable implementation of the analysis's state (code first, then proofs)."""
    ctx = r.ctx
    r.state.kernel_restarts += 1
    src = ctx.ws / "crate" / "src"
    archive = Path(ctx.run_dir) / "archive" / f"kernel_attempt{r.state.kernel_restarts}"
    archive.mkdir(parents=True, exist_ok=True)
    for p in src.glob("*"):
        if p.is_file() and not p.name.startswith("gen_"):
            shutil.move(str(p), str(archive / p.name))
    shutil.copy2(ctx.ws / "frozen" / "kernel_stub.rs", src / "kernel.rs")
    (src / "lib.rs").write_text(
        "// Verus kernel crate (pipeline).  gen_* are generated and frozen; the\n"
        "// kernel (kernel.rs and any modules it adds) is written by the agents.\n"
        "#![allow(unused_imports)]\n\n" + "".join(f"pub mod {m};\n" for m in actions.LIB_RS_REQUIRED),
        encoding="utf-8")
    (ctx.ws / "analysis" / "operator_note.md").write_text(
        SIMPLER_NOTE.format(archive=archive, failure=_brief(gates, 4000)), encoding="utf-8")
    r.state.done = [d for d in r.state.done if d != "kernel_code"]
    r.state.kernel_session = ""
    r.save()
    r.note(f"kernel: not provable after its rounds; restarting with a simpler implementation "
           f"(attempt {r.state.kernel_restarts + 1}; previous archived in {archive})")


def kernel(r: Run) -> None:
    """Code first, then proofs, in one agent session: the executable kernel passes
    the frozen tests (compiled without its proofs), then the same agent proves it;
    gate failures go back to that session.  On a rerun, a kernel whose code already
    passed continues straight to the proofs in its saved session."""
    rounds = int(r.ctx.setting("stages.kernel_fix_rounds", 4))
    sid = r.state.kernel_session
    note_path = r.ctx.ws / "analysis" / "operator_note.md"  # a reviewer's note for the next agent step, used once
    note = ""
    if note_path.is_file() and not r.dry_run:
        note = f"A note from the pipeline operator (read it first):\n\n{note_path.read_text(encoding='utf-8')}\n\n"
        note_path.rename(note_path.with_name("operator_note.used.md"))
    if "kernel_code" in r.state.done and sid and not r.dry_run:
        r.note("kernel code already passes the tests; continuing the proofs in the saved session")
    else:
        res = r.agent("verus_kernel", "04_kernel.md",
                      note + "Implement the kernel in crate/src (code first; no proofs yet).", tolerate_timeout=True)
        note = ""
        if r.dry_run:
            r.agent("verus_kernel_proofs", "04c_kernel_proofs.md", "(dry run)")
            return
        sid = res.session_id
        r.state.kernel_session = sid
        r.save()
        gates = r.gate("kernel_code_gates", _kernel_gates(r, proofs=False))
        gates = _fix_loop(r, gates, rounds, snapshots_too=False, why="kernel code", session=("verus_kernel", sid),
                          proofs=False)
        if not gates["ok"]:
            raise PipelineStop("the kernel's code does not pass the tests: " + _brief(gates, 3000))
        r.mark_done("kernel_code")
        r.note("kernel code passes the tests (unverified); proving it")
    proof_task = note + render(r.ctx, "04c_kernel_proofs.md")
    if sid:
        # the proofs continue the code's session, on the proofs' model (verus_kernel_proofs)
        res = r.agent("verus_kernel_proofs", "", proof_task, resume=sid, tolerate_timeout=True)
    else:
        res = r.agent("verus_kernel_proofs", "04c_kernel_proofs.md", "Prove the kernel in crate/src.", tolerate_timeout=True)
    session = ("verus_kernel_proofs", res.session_id or sid)
    r.state.kernel_session = session[1]
    r.save()
    gates = r.gate("kernel_gates", _kernel_gates(r))
    gates = _fix_loop(r, gates, rounds, snapshots_too=False, why="kernel proofs", session=session)
    # a kernel that is nearly proved is not thrown away: it stops for review (resume continues its session)
    # -- and neither is one that verifies and fails a later gate (tests, snapshots, cost)
    near = (gates.get("stage") == "verify" and 0 <= int(gates.get("errors", -1)) <= int(
        r.ctx.setting("stages.keep_kernel_within_errors", 3))) or gates.get("stage") in ("tests", "snapshots", "cost")
    if not gates["ok"] and not near and \
            r.state.kernel_restarts < int(r.ctx.setting("stages.kernel_simplify_restarts", 1)):
        _restart_kernel_simpler(r, gates)
        return kernel(r)
    if not gates["ok"]:
        raise PipelineStop("the kernel does not pass its gates: " + _brief(gates, 3000))
    r.note(f"kernel verified ({gates.get('verified')} verified in {gates.get('verify_seconds')}s), tests pass")


def integration(r: Run) -> None:
    ctx = r.ctx
    if r.dry_run:
        return
    rounds = int(ctx.setting("stages.adversarial_revisions", 2))
    for k in range(rounds + 1):
        gates = r.gate("integration", _kernel_gates(r, snapshots_too=True))
        gates = _fix_loop(r, gates, int(ctx.setting("stages.kernel_fix_rounds", 4)), snapshots_too=True,
                          why="integration", session=("verus_kernel_proofs", r.state.kernel_session))
        if not gates["ok"]:
            raise PipelineStop("integration: the kernel fails the frozen tests: " + _brief(gates, 3000))
        sc = r.gate("fault_selfcheck", actions.fault_selfcheck(ctx))
        if sc["ok"]:
            r.note("integration passed; the tests catch every injected bug and the SQL fallback is right")
            return
        if "fallback_correct" in sc["missed"]:  # not the tests' fault: stronger tests cannot fix it
            raise PipelineStop("integration: the SQL fallback gives wrong answers (a host/runtime bug, not the "
                               "kernel's): " + _brief(sc["results"].get("fallback_correct"), 3000))
        if k == rounds:
            break
        missed = "\n".join(f"- {f}: {actions.explain_fault(f)}" for f in sc["missed"])
        r.note(f"tests too weak, strengthening ({len(sc['missed'])} bug(s) uncaught)")
        old = {p.relative_to(ctx.ws / 'tests'): p.read_bytes() for p in (ctx.ws / "tests").rglob("*") if p.is_file()}
        r.agent("verus_tests", "03b_tests_strengthen.md", "Strengthen tests/ so every listed bug is caught.",
                {"missed": missed})
        for rel, data in old.items():  # tests only grow
            p = ctx.ws / "tests" / rel
            if not p.is_file() or p.read_bytes() != data:
                p.parent.mkdir(parents=True, exist_ok=True)
                p.write_bytes(data)
        v = r.gate("validate_tests", actions.validate_tests(ctx))
        if not v["ok"]:
            raise PipelineStop("strengthened tests do not run: " + _brief(v, 3000))
        r.state.tests_hash = actions.tree_hash(ctx.ws / "tests")
        r.state.save(ctx)
    raise PipelineStop("the tests still miss injected bugs: " + ", ".join(sc["missed"]))


# ───────────────────────────── optimization ─────────────────────────────


def _hotspots(ctx: VerusContext, prof: dict[str, Any]) -> list[str]:
    share = float(ctx.setting("optimization.hotspot_share", 0.10))
    agg: dict[str, float] = {}
    for axis in ("reads", "maintenance"):  # per axis of the score; names say which
        a = prof.get(axis) or {}
        if axis == "maintenance" and "overhead_us_per_write" in a:
            # writes are mostly the engine executing the write itself; only what
            # the accelerator adds is a target
            if a["overhead_us_per_write"] < share * max(a.get("plain_write_us") or 0, 1):
                continue
        for h in a.get("hotspots") or []:
            name = f"{axis}:{actions.hotspot_name(h)}"
            agg[name] = agg.get(name, 0.0) + float(h["share"])
    return sorted((n for n, s in agg.items() if s >= share), key=lambda n: -agg[n])


def _full_bench(r: Run, tag: str, runner: Path | None = None, draw: qparams.Draw | None = None) -> tuple[float, dict[str, Any]]:
    """`draw`: random parameter tuples shared with other benches of one decision; None: a fresh draw."""
    mixes = [float(x) for x in r.ctx.setting("optimization.bench.write_mix_choices", [0.01, 0.05, 0.1])]
    b = actions.bench(r.ctx, mixes, tag=tag, runner=runner, draw=draw)
    if not b["ok"]:
        return 0.0, b
    return actions.score(b, r.ctx), b


def _paired_bench(r: Run, tag: str, best: Path, cand: Path) -> dict[str, Any]:
    """Best and candidate benched alternately (best, cand, cand, best, ...), so
    both see the same machine conditions; compared by their median scores.
    A score stored from an earlier run drifts with the machine's load by more
    than the gains being judged."""
    rounds = int(r.ctx.setting("optimization.bench.paired_rounds", 3))
    # one fresh random draw of parameters for this decision, the same for both sides and every round
    draw = qparams.new_draw(r.ctx, tag)
    scores: dict[str, list[float]] = {"best": [], "cand": []}
    benches: dict[str, list[dict[str, Any]]] = {"best": [], "cand": []}
    for i in range(rounds):
        order = [("best", best), ("cand", cand)] if i % 2 == 0 else [("cand", cand), ("best", best)]
        for who, runner in order:
            s, b = _full_bench(r, f"{tag}_{who}_{i}", runner=runner, draw=draw)
            if not s:
                return {"ok": False, "who": who, "bench": b}
            scores[who].append(s)
            benches[who].append(b)
    med = {k: statistics.median(v) for k, v in scores.items()}
    mid = {k: benches[k][v.index(sorted(v)[len(v) // 2])] for k, v in scores.items()}
    spread = (max(scores["best"]) - min(scores["best"])) / med["best"] * 100.0
    wins = sum(c > b for c, b in zip(scores["cand"], scores["best"]))
    return {"ok": True, **qparams.summary(draw), "best": med["best"], "cand": med["cand"], "scores": scores, "wins": wins,
            "rounds": rounds, "best_spread_pct": spread, "cand_bench": mid["cand"], "best_bench": mid["best"]}


def _best_runner_frozen(ctx: VerusContext) -> Path:
    d = Path(ctx.run_dir) / "runners" / "best"
    if not (d / ctx.runner.name).is_file():
        raise PipelineStop("optimization: no frozen runner for the best kernel")
    return d


def _best_runner(r: Run) -> Path:
    """The frozen runner of the best kernel (built from its snapshot if missing)."""
    d = Path(r.ctx.run_dir) / "runners" / "best"
    if not (d / r.ctx.runner.name).is_file():
        workspace.restore_kernel(r.ctx, "best")
        b = actions.build_runner(r.ctx)
        if not b["ok"]:
            raise PipelineStop("optimization: the best kernel no longer builds: " + _brief(b, 3000))
        actions.freeze_runner(r.ctx, "best")
    return d


def optimization(r: Run) -> None:
    ctx = r.ctx
    if r.dry_run:
        r.agent("verus_opt_worker", "05_opt_worker.md", "(dry run)", _opt_extra(r, StopState(), {}, ""))
        return
    # parallel pipelines: only a few optimize at once (their benchmarks share one machine);
    # the budget clock starts once this one has a slot
    slots = int(ctx.setting("optimization.max_concurrent", 4))
    with actions.machine_slot(ctx, "optimization", slots, note=r.note) as slot:
        r.note(f"optimization slot {slot + 1}/{slots}")
        _optimization(r)


def _optimization(r: Run) -> None:
    ctx = r.ctx
    opt = ctx.setting("optimization", {}) or {}
    cfg = StopConfig.from_settings(opt)
    o = r.state.opt
    st = StopState(**o["stop"]) if o.get("stop") else StopState()
    if not o.get("best_snapshot"):
        workspace.snapshot_kernel(ctx, "best")
        b = actions.build_runner(ctx)
        if not b["ok"]:
            raise PipelineStop("optimization: the accepted kernel no longer builds: " + _brief(b, 3000))
        base_draw = qparams.new_draw(r.ctx, "baseline")  # both baselines on one fresh draw
        s1, b1 = _full_bench(r, "baseline_1", draw=base_draw)
        s2, _ = _full_bench(r, "baseline_2", draw=base_draw)
        if not s1 or not s2:
            raise PipelineStop("optimization: the baseline bench failed: " + _brief(b1, 3000))
        actions.freeze_runner(ctx, "best")
        o.update(best_snapshot="best", best_score=(s1 + s2) / 2, noise_pct=abs(s1 - s2) / ((s1 + s2) / 2) * 100,
                 best_verify_seconds=b.get("verify_seconds") or 1.0, structural_moves=0, attempt=0,
                 best_bench=b1, history=[])
        prof = actions.profile(ctx)
        st.hotspots = _hotspots(ctx, prof) if prof.get("ok") else ["read path", "maintenance"]
        o["profile"] = prof
        r.state.opt = o
        o["stop"] = asdict(st)
        r.state.save(ctx)
        r.note(f"baseline score {o['best_score']:.3f} (noise {o['noise_pct']:.1f}%), hotspots {st.hotspots}")
    else:  # resuming
        if workspace.differs(ctx, "best"):
            # an interrupted attempt left a kernel: judge it (no fix rounds) rather than drop it
            snap = workspace.snapshot_kernel(ctx, "interrupted")
            _best_runner(r)
            workspace.restore_kernel(ctx, "interrupted")
            o["attempt"] = n = int(o.get("attempt", 0)) + 1
            outcome, gain = _try_candidate(r, o, float(opt.get("min_improvement_pct", 2.0)), False, "",
                                           fix_rounds=0)
            st.record(Attempt(n, "interrupted attempt", "both", False, outcome == "accepted", gain, outcome))
            o.setdefault("history", []).append({"attempt": n, "target": "interrupted attempt", "axis": "both",
                                                "outcome": outcome, "gain_pct": round(gain, 2),
                                                "summary": "the kernel an interrupted attempt left, judged on resume"})
            o["stop"] = asdict(st)
            r.state.save(ctx)
            r.note(f"attempt {n} (interrupted, judged on resume): {outcome}")
            shutil.rmtree(snap, ignore_errors=True)
        workspace.restore_kernel(ctx, "best")
        _best_runner(r)
    if "reads" not in (o.get("profile") or {}):  # a profile from before the per-axis steady-state one
        b = actions.build_runner(ctx)
        prof = actions.profile(ctx) if b["ok"] else {"ok": False}
        if prof.get("ok"):
            st.hotspots = _hotspots(ctx, prof)
            o["profile"] = prof
            o["stop"] = asdict(st)
            r.state.save(ctx)
            r.note(f"re-profiled the steady state: hotspots {st.hotspots}")
    # paired benches cancel the machine's drift: the bar is the configured minimum
    threshold = float(opt.get("min_improvement_pct", 2.0))
    message = ""
    while True:
        stop, why = should_stop(st, cfg)
        if stop:
            r.note(f"optimization stops: {why}")
            break
        o["attempt"] = n = int(o.get("attempt", 0)) + 1
        mixes = random.Random(n).sample(opt.get("bench", {}).get("write_mix_choices", [0.01, 0.05, 0.1]),
                                        int(opt.get("bench", {}).get("write_mix_count", 2)))
        res = r.agent("verus_opt_worker", "05_opt_worker.md", "Make one measured improvement (one attempt).",
                      _opt_extra(r, st, o, message, mixes), timeout_sec=int(opt.get("attempt_timeout_sec", 2700)),
                      tolerate_timeout=True)
        report = json_block(res.final_text) or {}
        message = ""
        for h, why_inf in (report.get("infeasible") or {}).items():
            st.infeasible[str(h)] = str(why_inf)
        if report.get("structural_considered"):
            st.structural_considered = True
        if "OPTIMIZATION_SATURATED" in res.final_text:
            stop, why = should_stop(st, cfg, saturated_claim=True)
            if stop:
                r.note(f"optimization stops: {why}")
                workspace.restore_kernel(ctx, "best")
                break
            message = f"Your OPTIMIZATION_SATURATED claim was not accepted: {why}"
            workspace.restore_kernel(ctx, "best")
            st.record(Attempt(n, "saturation_claim", "both", False, False, 0.0, "rejected: " + why))
            o["stop"] = asdict(st)
            r.state.save(ctx)
            continue
        target = str(report.get("target") or "unspecified")
        axis = str(report.get("axis") or "both")
        structural = bool(report.get("structural"))
        outcome, gain = _try_candidate(r, o, threshold, structural, res.session_id)
        accepted = outcome == "accepted"
        st.record(Attempt(n, target, axis, structural, accepted, gain, outcome))
        o.setdefault("history", []).append({"attempt": n, "target": target, "axis": axis, "outcome": outcome,
                                            "gain_pct": round(gain, 2), "summary": str(report.get("summary", ""))[:400]})
        if accepted:
            prof = actions.profile(ctx)
            if prof.get("ok"):
                st.hotspots = _hotspots(ctx, prof)
                o["profile"] = prof
        o["stop"] = asdict(st)
        r.state.save(ctx)
        r.note(f"attempt {n} ({target}, {axis}): {outcome}")
    # leave the best kernel in place and built
    workspace.restore_kernel(ctx, "best")
    if (ctx.ws / "inputs" / "q.inputs").read_text() != (ctx.ws / "frozen" / "q.inputs").read_text():
        actions.freeze_inputs(ctx)
        r.state.frozen = {**actions.frozen_hashes(ctx), **actions.glue_hashes(ctx)}
    b = actions.build_runner(ctx)
    if not b["ok"]:
        raise PipelineStop("the best kernel no longer builds: " + _brief(b, 3000))
    r.state.save(ctx)


def _opt_extra(r: Run, st: StopState, o: dict[str, Any], message: str, mixes: list[float] | None = None) -> dict[str, str]:
    cfg = StopConfig.from_settings(r.ctx.setting("optimization", {}) or {})
    hist = o.get("history") or []
    lines = [f"| {h['attempt']} | {h['target']} | {h['axis']} | {h['outcome']} | {h['gain_pct']} |" for h in hist[-15:]]
    return {
        "best_score": f"{o.get('best_score', 0):.3f}",
        "noise_pct": f"{o.get('noise_pct', 0):.1f}",
        "history": "| # | target | axis | outcome | gain % |\n|---|---|---|---|---|\n" + "\n".join(lines)
        if lines else "(no attempts yet)",
        "profile": _brief(o.get("profile") or {"note": "no profile yet"}, 8000),
        "owed": "\n".join(f"- {x}" for x in uncovered(st, cfg)) or "- nothing: you may stop when gains run out",
        "message": message or "(none)",
        "mixes": ",".join(str(m) for m in (mixes or [])),
        "structural_left": str(int(r.ctx.setting("optimization.max_structural_moves", 2)) - int(o.get("structural_moves", 0))),
    }


def _try_candidate(r: Run, o: dict[str, Any], threshold: float, structural: bool,
                   session: str = "", fix_rounds: int | None = None) -> tuple[str, float]:
    """Gate and score the workspace's candidate; keep it (accepted) or restore the best."""
    ctx = r.ctx
    inputs_changed = (ctx.ws / "inputs" / "q.inputs").read_text() != (ctx.ws / "frozen" / "q.inputs").read_text()
    frozen_before = dict(r.state.frozen)

    def reject(why: str) -> tuple[str, float]:
        workspace.restore_kernel(ctx, "best")
        if inputs_changed:
            actions.freeze_inputs(ctx)
            r.state.frozen = frozen_before
        return f"rejected: {why}", 0.0

    if inputs_changed:
        if int(o.get("structural_moves", 0)) >= int(ctx.setting("optimization.max_structural_moves", 2)):
            return reject("no structural moves left (inputs/q.inputs may not change)")
        chk = actions.check_inputs(ctx)
        if not chk["ok"]:
            return reject("sql2verus refused the new inputs: " + chk["error"][:300])
        fz = actions.freeze_inputs(ctx)
        if not fz["ok"]:
            return reject("re-freeze failed")
        r.state.frozen = fz["hashes"]
        issues = _obligations_audit(r)
        if issues:
            return reject("the new obligations failed their audit: " + issues[:300])
        ok, why = _write_glue(r, "The kernel inputs changed (inputs/q.inputs): update the host glue in glue/ to deliver them.")
        if not ok:
            return reject(why[:300])
        r.state.frozen.update(actions.glue_hashes(ctx))
    gates = _kernel_gates(r, snapshots_too=True)
    rounds = int(ctx.setting("optimization.attempt_gate_fix_rounds", 2)) if fix_rounds is None else fix_rounds
    gates = _fix_loop(r, gates, rounds, snapshots_too=True,
                      why="attempt", session=("verus_opt_worker", session))
    actions.write_result(ctx, f"attempt_{o['attempt']}_gates", gates)
    if not gates["ok"]:
        return reject(f"gates failed ({gates.get('stage')})")
    cand_runner = actions.freeze_runner(ctx, "candidate")
    pb = _paired_bench(r, f"attempt_{o['attempt']}", _best_runner_frozen(ctx), cand_runner)
    actions.write_result(ctx, f"attempt_{o['attempt']}_bench", pb)
    if not pb["ok"]:
        return reject(f"bench failed ({pb.get('who')})")
    score, bench = pb["cand"], pb["cand_bench"]
    # proof cost: a slower proof needs a larger gain
    w = float(ctx.setting("optimization.proof_cost_weight", 0.0))
    t_best, t_new = float(o.get("best_verify_seconds") or 1.0), float(gates.get("verify_seconds") or 1.0)
    if w > 0 and t_new > t_best:
        import math

        score /= 1.0 + w * math.log2(t_new / t_best)
    gain = (score / pb["best"] - 1.0) * 100.0
    majority = pb["wins"] * 2 > pb["rounds"]
    if gain <= threshold or not majority:
        return reject(f"paired median {score:.3f} vs best {pb['best']:.3f} ({gain:+.1f}%, needs > {threshold:.1f}%; "
                      f"won {pb['wins']}/{pb['rounds']} pairs)")
    workspace.snapshot_kernel(ctx, "best")
    shutil.rmtree(Path(ctx.run_dir) / "runners" / "best", ignore_errors=True)
    cand_runner.rename(Path(ctx.run_dir) / "runners" / "best")
    o.update(best_score=score, best_bench=bench, best_verify_seconds=t_new,
             noise_pct=pb["best_spread_pct"])
    if inputs_changed:
        o["structural_moves"] = int(o.get("structural_moves", 0)) + 1
    return "accepted", gain


# ───────────────────────────── audit ─────────────────────────────


def audit(r: Run) -> None:
    ctx = r.ctx
    report: dict[str, Any] = {"accelerator": ctx.accel_id, "crate": ctx.crate}
    if not r.dry_run:
        report["frozen_changed"] = actions.check_frozen(ctx, r.state.frozen, r.state.tests_hash)
        report["trusted_base"] = actions.trusted_base(ctx)
        report["leak_check"] = actions.leak_check(ctx)
        b = actions.build_runner(ctx)
        report["build"] = b
        report["verus_store"] = actions.correctness(ctx, store="verus_global")
        report["cpp_store"] = actions.correctness(ctx, store="cpp")
        report["fault_selfcheck"] = actions.fault_selfcheck(ctx)
        report["glue_contract"] = actions.glue_contract(ctx)
        audit_draw = qparams.new_draw(ctx, "audit")  # a fresh draw, shared by the two audit benches
        _, report["bench_verus"] = _full_bench(r, "audit_verus", draw=audit_draw)
        mixes = [float(x) for x in ctx.setting("optimization.bench.write_mix_choices", [0.01, 0.05, 0.1])]
        report["bench_cpp_store"] = actions.bench(ctx, mixes, store="cpp", tag="audit_cpp", draw=audit_draw)
        report["optimization"] = {k: v for k, v in r.state.opt.items() if k not in ("stop", "best_bench")}
        ok = (not report["frozen_changed"] and report["trusted_base"]["ok"] and b.get("ok")
              and report["verus_store"]["ok"] and report["cpp_store"]["ok"] and report["fault_selfcheck"]["ok"]
              and report["glue_contract"]["ok"])
        report["verdict"] = "pass" if ok and report["leak_check"]["ok"] else ("contaminated" if ok else "fail")
    path = ctx.art / "audit_report.json"
    path.write_text(json.dumps(report, indent=2), encoding="utf-8")
    (ctx.ws / "analysis" / "audit_report.json").write_text(json.dumps(report, indent=2), encoding="utf-8")
    r.agent("verus_audit", "06_audit.md", "Write analysis/audit_summary.md from analysis/audit_report.json.")
    summary = ctx.ws / "analysis" / "audit_summary.md"
    if summary.is_file():
        (ctx.art / "audit_summary.md").write_text(summary.read_text(encoding="utf-8"), encoding="utf-8")
    r.note(f"audit: {report.get('verdict', '(dry run)')}; report {path}")


def _read_json(p: Path) -> dict[str, Any]:
    try:
        return json.loads(p.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError):
        return {}


STAGE_FUNCS = {
    "preflight": preflight,
    "spec": spec,
    "analysis": analysis,
    "freeze": freeze,
    "tests": tests,
    "glue": glue,
    "kernel": kernel,
    "integration": integration,
    "optimization": optimization,
    "audit": audit,
}

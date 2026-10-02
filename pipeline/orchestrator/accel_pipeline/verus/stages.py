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
    integration   frozen tests on the Verus store; the tests' fault self-check
    optimization  A/B loop on the Verus store (stopping.py)
    audit         trusted base, leaks, C++-store compatibility, report

Hard gates are correctness and trust only.  Scaling is guidance in the
prompts (contracts/scale_principles.md).  Every agent step is codex exec.
"""

from __future__ import annotations

import json
import random
import time
from dataclasses import asdict, dataclass, field
from pathlib import Path
from typing import Any

from accel_pipeline.verus import actions, workspace
from accel_pipeline.verus.agent import json_block, run_agent
from accel_pipeline.verus.context import IGNORED_WORKLOAD_FIELDS, VerusContext
from accel_pipeline.verus.stopping import Attempt, StopConfig, StopState, should_stop, uncovered

STAGES = ("preflight", "spec", "analysis", "freeze", "tests", "glue", "kernel", "integration", "optimization",
          "audit")


class PipelineStop(RuntimeError):
    """The run cannot continue (reason in the message; state is saved)."""


@dataclass
class VerusState:
    stage: str = "preflight"
    spec_sha: str = ""
    frozen: dict[str, str] = field(default_factory=dict)
    tests_hash: str = ""
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
              timeout_sec: int | None = None):
        res = run_agent(self.ctx, stage=stage, prompt=prompt, task=task, pipeline_config=self.pipeline_config,
                        model_config=self.model_config, model_overrides=self.model_overrides, extra=extra,
                        dry_run=self.dry_run, timeout_sec=timeout_sec)
        if not res.ok:
            raise PipelineStop(f"{stage} agent failed: {res.error[:2000]}")
        return res

    def note(self, msg: str) -> None:
        print(f"[verus] {msg}", flush=True)
        self.state.notes.append(f"{time.strftime('%H:%M:%S')} {msg}")
        self.state.save(self.ctx)

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
        cfg = actions.configure_build(ctx)
        if not cfg["ok"]:
            problems.append(f"build dir {ctx.build_dir}: cmake configure (DuckDB engine) failed: {cfg['error']}")
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
    r.state.spec_sha = res["sha256"]
    r.state.save(r.ctx)
    r.agent("verus_spec_audit", "01_spec_audit.md",
            "Audit spec/gen_spec.rs against query/query.sql; write analysis/spec_audit.json.")
    if r.dry_run:
        return
    audit = _read_json(r.ctx.ws / "analysis" / "spec_audit.json")
    if audit.get("verdict") != "ok":
        raise PipelineStop("the spec does not mean what the SQL means (sql2verus must be fixed, not the spec): "
                           + json.dumps(audit.get("issues") or audit)[:3000])


def analysis(r: Run) -> None:
    ctx = r.ctx
    revisions = int(ctx.setting("stages.analysis_revisions", 2))
    task = "Design the aux state and write analysis/analysis.json and inputs/q.inputs."
    feedback = ""
    for round_ in range(revisions + 1):
        r.agent("verus_analysis", "02_analysis.md", task, {"feedback": feedback or "(none: first round)"})
        if r.dry_run:
            return
        check = r.gate("check_inputs", actions.check_inputs(ctx))
        if not check["ok"]:
            feedback = f"sql2verus refused inputs/q.inputs:\n{check['error']}"
            continue
        r.agent("verus_critic", "02b_critic.md", "Review the analysis; write analysis/critique.json.")
        crit = _read_json(ctx.ws / "analysis" / "critique.json")
        if crit.get("verdict") == "accept" or round_ == revisions:
            r.note(f"analysis accepted after {round_ + 1} round(s) (critic: {crit.get('verdict', '?')})")
            return
        feedback = "The critic asks for a revision:\n" + json.dumps(crit.get("objections") or crit, indent=2)
    raise PipelineStop(f"analysis: inputs still refused after {revisions + 1} rounds: {feedback[:2000]}")


def freeze(r: Run) -> None:
    if not r.dry_run:
        res = actions.freeze_inputs(r.ctx)
        if not res["ok"]:
            raise PipelineStop(f"freeze B failed: {res['error']}")
        r.state.frozen = res["hashes"]
        r.state.save(r.ctx)
        r.note(f"frozen: {', '.join(sorted(r.state.frozen))}")
    issues = _obligations_audit(r)
    if issues:
        raise PipelineStop("the generated obligations do not mean what the declared inputs say "
                           "(sql2verus must be fixed): " + issues)


def _obligations_audit(r: Run) -> str:
    """The obligations' faithfulness audit; '' if they are faithful."""
    r.agent("verus_obligations_audit", "01b_obligations_audit.md",
            "Audit frozen/gen_obligations.rs against inputs/q.inputs; write analysis/obligations_audit.json.")
    if r.dry_run:
        return ""
    audit = _read_json(r.ctx.ws / "analysis" / "obligations_audit.json")
    return "" if audit.get("verdict") == "ok" else json.dumps(audit.get("issues") or audit)[:3000]


def tests(r: Run) -> None:
    ctx = r.ctx
    feedback = ""
    for _ in range(3):
        r.agent("verus_tests", "03_tests.md", "Write the adversarial tests in tests/.",
                {"feedback": feedback or "(none: first round)"})
        if r.dry_run:
            return
        v = r.gate("validate_tests", actions.validate_tests(ctx))
        if v["ok"]:
            r.state.tests_hash = actions.tree_hash(ctx.ws / "tests")
            r.state.save(ctx)
            r.note(f"tests frozen: {v.get('params')} parameter tuples, {v.get('txns')} transactions")
            return
        feedback = "verus_validate_tests failed:\n" + _brief(v, 4000)
    raise PipelineStop(f"tests do not run on the database: {feedback[:2000]}")


def glue(r: Run) -> None:
    ok, why = _write_glue(r, "Write the host glue in glue/glue.cpp.")
    if not ok:
        raise PipelineStop(why)
    if not r.dry_run:
        r.state.frozen.update(actions.glue_hashes(r.ctx))
        r.state.save(r.ctx)
        r.note("glue frozen: it builds, delivers the declared inputs, and passed its audit")


def _write_glue(r: Run, task: str) -> tuple[bool, str]:
    """The glue agent writes glue/; then build, contract check, glue audit (with fix rounds)."""
    ctx = r.ctx
    feedback = ""
    rounds = int(ctx.setting("stages.glue_fix_rounds", 3))
    for k in range(rounds + 1):
        r.agent("verus_glue", "03c_glue.md", task, {"feedback": feedback or "(none: first round)"})
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
        if audit.get("verdict") == "ok":
            return True, ""
        feedback = "The glue audit found problems:\n" + json.dumps(audit.get("issues") or audit, indent=2)[:6000]
        r.note(f"glue round {k + 1}: {feedback.splitlines()[0]}")
    return False, f"the host glue fails its checks after {rounds + 1} rounds: {feedback[:2000]}"


def _kernel_gates(r: Run, *, snapshots_too: bool = False) -> dict[str, Any]:
    ctx = r.ctx
    changed = actions.check_frozen(ctx, r.state.frozen, r.state.tests_hash)
    if changed:
        return {"ok": False, "stage": "frozen", "changed": changed,
                "error": "frozen files changed (restore them; the spec, generated files and tests are fixed)"}
    tb = actions.trusted_base(ctx)
    if not tb["ok"]:
        return {"ok": False, "stage": "trusted_base", **tb}
    b = actions.build_runner(ctx)
    if not b["ok"]:
        return b
    c = actions.correctness(ctx, snapshots_too=snapshots_too)
    c["stage"] = "tests"
    c["verified"] = b.get("verified")
    c["verify_seconds"] = b.get("verify_seconds")
    return c


def _fix_loop(r: Run, gates: dict[str, Any], rounds: int, *, snapshots_too: bool, why: str) -> dict[str, Any]:
    for k in range(rounds):
        if gates["ok"]:
            break
        r.note(f"{why}: fix round {k + 1}/{rounds} ({gates.get('stage')})")
        r.agent("verus_kernel_fix", "04b_kernel_fix.md", "Make the kernel pass the gates.",
                {"failure": _brief(gates, 12000)})
        gates = r.gate("kernel_gates", _kernel_gates(r, snapshots_too=snapshots_too))
    return gates


def kernel(r: Run) -> None:
    r.agent("verus_kernel", "04_kernel.md", "Implement and prove the kernel in crate/src.")
    if r.dry_run:
        return
    gates = r.gate("kernel_gates", _kernel_gates(r))
    gates = _fix_loop(r, gates, int(r.ctx.setting("stages.kernel_fix_rounds", 4)), snapshots_too=False,
                      why="kernel")
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
                          why="integration")
        if not gates["ok"]:
            raise PipelineStop("integration: the kernel fails the frozen tests: " + _brief(gates, 3000))
        sc = r.gate("fault_selfcheck", actions.fault_selfcheck(ctx))
        if sc["ok"]:
            r.note("integration passed; the tests catch every injected bug")
            return
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
    for h in prof.get("hotspots") or []:
        name = actions.hotspot_name(h)
        agg[name] = agg.get(name, 0.0) + float(h["share"])
    return sorted((n for n, s in agg.items() if s >= share), key=lambda n: -agg[n])


def _full_bench(r: Run, tag: str) -> tuple[float, dict[str, Any]]:
    mixes = [float(x) for x in r.ctx.setting("optimization.bench.write_mix_choices", [0.01, 0.05, 0.1])]
    b = actions.bench(r.ctx, mixes, tag=tag)
    if not b["ok"]:
        return 0.0, b
    return actions.score(b, r.ctx), b


def optimization(r: Run) -> None:
    ctx = r.ctx
    if r.dry_run:
        r.agent("verus_opt_worker", "05_opt_worker.md", "(dry run)", _opt_extra(r, StopState(), {}, ""))
        return
    opt = ctx.setting("optimization", {}) or {}
    cfg = StopConfig.from_settings(opt)
    o = r.state.opt
    st = StopState(**o["stop"]) if o.get("stop") else StopState()
    if not o.get("best_snapshot"):
        workspace.snapshot_kernel(ctx, "best")
        b = actions.build_runner(ctx)
        if not b["ok"]:
            raise PipelineStop("optimization: the accepted kernel no longer builds: " + _brief(b, 3000))
        s1, b1 = _full_bench(r, "baseline_1")
        s2, _ = _full_bench(r, "baseline_2")
        if not s1 or not s2:
            raise PipelineStop("optimization: the baseline bench failed: " + _brief(b1, 3000))
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
    threshold = max(float(opt.get("min_improvement_pct", 2.0)), 2 * float(o.get("noise_pct", 0.0)))
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
                      _opt_extra(r, st, o, message, mixes), timeout_sec=int(opt.get("attempt_timeout_sec", 2700)))
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
        outcome, gain = _try_candidate(r, o, threshold, structural)
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
        "profile": _brief(o.get("profile") or {"note": "no profile yet"}, 5000),
        "owed": "\n".join(f"- {x}" for x in uncovered(st, cfg)) or "- nothing: you may stop when gains run out",
        "message": message or "(none)",
        "mixes": ",".join(str(m) for m in (mixes or [])),
        "structural_left": str(int(r.ctx.setting("optimization.max_structural_moves", 2)) - int(o.get("structural_moves", 0))),
    }


def _try_candidate(r: Run, o: dict[str, Any], threshold: float, structural: bool) -> tuple[str, float]:
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
    gates = _fix_loop(r, gates, int(ctx.setting("optimization.attempt_gate_fix_rounds", 2)), snapshots_too=True,
                      why="attempt")
    actions.write_result(ctx, f"attempt_{o['attempt']}_gates", gates)
    if not gates["ok"]:
        return reject(f"gates failed ({gates.get('stage')})")
    score, bench = _full_bench(r, f"attempt_{o['attempt']}")
    actions.write_result(ctx, f"attempt_{o['attempt']}_bench", bench)
    if not score:
        return reject("bench failed")
    # proof cost: a slower proof needs a larger gain
    w = float(ctx.setting("optimization.proof_cost_weight", 0.0))
    t_best, t_new = float(o.get("best_verify_seconds") or 1.0), float(gates.get("verify_seconds") or 1.0)
    if w > 0 and t_new > t_best:
        import math

        score /= 1.0 + w * math.log2(t_new / t_best)
    gain = (score / float(o["best_score"]) - 1.0) * 100.0
    if gain <= threshold:
        return reject(f"score {score:.3f} vs best {o['best_score']:.3f} ({gain:+.1f}%, needs > {threshold:.1f}%)")
    workspace.snapshot_kernel(ctx, "best")
    o.update(best_score=score, best_bench=bench, best_verify_seconds=t_new)
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
        report["verus_store"] = actions.correctness(ctx, store="verus")
        report["cpp_store"] = actions.correctness(ctx, store="cpp")
        report["fault_selfcheck"] = actions.fault_selfcheck(ctx)
        report["glue_contract"] = actions.glue_contract(ctx)
        _, report["bench_verus"] = _full_bench(r, "audit_verus")
        mixes = [float(x) for x in ctx.setting("optimization.bench.write_mix_choices", [0.01, 0.05, 0.1])]
        report["bench_cpp_store"] = actions.bench(ctx, mixes, store="cpp", tag="audit_cpp")
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

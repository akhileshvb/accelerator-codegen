"""Verus pipeline (--mode verus): stopping logic, trust gates, prompts, routing."""

from __future__ import annotations

import json
import time
from pathlib import Path

import pytest

from accel_pipeline.verus import actions
from accel_pipeline.verus.agent import json_block, render
from accel_pipeline.verus.context import VerusContext
from accel_pipeline.verus.stopping import Attempt, StopConfig, StopState, should_stop, uncovered
from accel_pipeline.verus.tools import STAGE_TOOLS, TOOLS

REPO = Path(__file__).resolve().parents[3]


def _ctx(tmp: Path) -> VerusContext:
    ws = tmp / "ws"
    (ws / "crate" / "src").mkdir(parents=True)
    return VerusContext(
        query=21, workload_name="w", benchmark="tpch", repo_root=str(tmp / "root"), pipeline_root=str(REPO / "pipeline"),
        build_dir=str(tmp / "build"), lib_dir=str(tmp / "lib"), crate="tpch_q21_vgen", accel_id="tpch_q21_vgen",
        ffi_prefix="tpchq21vgen", query_sql="", schema_sql="", test_db="", bench_db="", run_dir=str(tmp / "run"),
        workspace=str(ws), artifacts=str(tmp / "art"), settings={})


def _attempt(n: int, *, target: str = "kernel::f", axis: str = "read", accepted: bool = False, gain: float = 0.0,
             structural: bool = False) -> Attempt:
    return Attempt(n, target, axis, structural, accepted, gain, "accepted" if accepted else "rejected")


# ---- stopping ----

def test_budget_always_stops() -> None:
    st = StopState(started_at=time.time() - 100)
    stop, why = should_stop(st, StopConfig(stage_budget_sec=50))
    assert stop and "budget" in why


def test_progress_stop_waits_for_exploration() -> None:
    cfg = StopConfig(patience=2, min_attempts=3)
    st = StopState(hotspots=["kernel::execute"])
    st.record(_attempt(1, axis="read"))
    st.record(_attempt(2, axis="read"))
    stop, why = should_stop(st, cfg)
    # two attempts without an accept, but the hotspot, maintenance and a third attempt are owed
    assert not stop and "exploration is owed" in why
    owed = uncovered(st, cfg)
    assert any("kernel::execute" in o for o in owed) and any("maintenance" in o for o in owed)
    st.record(_attempt(3, target="kernel::execute", axis="maintenance", structural=True))
    stop, why = should_stop(st, cfg)
    assert stop and "without an accepted improvement" in why


def test_infeasible_hotspot_counts_as_covered() -> None:
    cfg = StopConfig(patience=1, min_attempts=1)
    st = StopState(hotspots=["engine"], infeasible={"engine": "DuckDB internals"}, structural_considered=True)
    st.record(_attempt(1, axis="both"))
    assert should_stop(st, cfg)[0]


def test_diminishing_returns_stop() -> None:
    cfg = StopConfig(patience=10, diminishing_window=3, diminishing_min_gain_pct=5.0, min_attempts=1)
    st = StopState(structural_considered=True)
    for i in range(3):
        st.record(_attempt(i + 1, axis="both", accepted=True, gain=1.0))
    st.record(_attempt(4, axis="both"))
    stop, why = should_stop(st, cfg)
    assert stop and "together" in why


def test_saturation_claim_needs_exploration() -> None:
    cfg = StopConfig(min_attempts=2)
    st = StopState()
    stop, why = should_stop(st, cfg, saturated_claim=True)
    assert not stop and "owed" in why


def test_accepts_keep_the_loop_going() -> None:
    cfg = StopConfig(patience=2, diminishing_window=3, diminishing_min_gain_pct=5.0, min_attempts=1)
    st = StopState(structural_considered=True)
    st.record(_attempt(1, axis="read", accepted=True, gain=20.0))
    st.record(_attempt(2, axis="maintenance"))
    assert not should_stop(st, cfg)[0]


# ---- trusted base / leaks ----

def test_trusted_base_flags_escape_hatches(tmp_path: Path) -> None:
    ctx = _ctx(tmp_path)
    src = ctx.ws / "crate" / "src"
    (src / "lib.rs").write_text("pub mod gen_spec;\npub mod gen_obligations;\npub mod kernel;\npub mod gen_world;\n")
    (src / "kernel.rs").write_text(
        "fn f() { proof { assume(false); } }\n#[verifier::external_body]\nfn g() {}\n// assume(x) in a comment is fine\n")
    (src / "gen_ffi.rs").write_text("#[no_mangle] pub unsafe extern \"C\" fn x() {}\n")  # generated: exempt
    res = actions.trusted_base(ctx)
    assert not res["ok"]
    text = "\n".join(res["violations"])
    assert "assume" in text and "external_body" in text and "gen_ffi" in text  # lib.rs lacks gen_ffi
    assert "comment" not in text
    assert not any(v.startswith("gen_ffi.rs") for v in res["violations"])


def test_trusted_base_accepts_plain_kernel(tmp_path: Path) -> None:
    ctx = _ctx(tmp_path)
    src = ctx.ws / "crate" / "src"
    (src / "lib.rs").write_text("".join(f"pub mod {m};\n" for m in actions.LIB_RS_REQUIRED))
    (src / "kernel.rs").write_text("use vstd::prelude::*;\nverus! { pub fn f(x: u64) -> u64 { x } }\n")
    assert actions.trusted_base(ctx)["ok"]


def test_leak_check_flags_copied_kernel(tmp_path: Path) -> None:
    ctx = _ctx(tmp_path)
    root = Path(ctx.repo_root)
    ctx.settings["leak"] = {"excluded_glob": "excluded_kernels/q*/src"}
    excluded = root / "excluded_kernels" / "q99" / "src"
    excluded.mkdir(parents=True)
    body = "\n".join(f"    let distinctive_name_{i} = compute_something_special_{i}(argument_value);" for i in range(40))
    (excluded / "maint.rs").write_text(body)
    for d in ("substrate/verus/common", "substrate/verus/store", "accelerators/prooflib"):
        (root / d / "src").mkdir(parents=True)
    (ctx.ws / "crate" / "src" / "kernel.rs").write_text(body)
    res = actions.leak_check(ctx)
    assert not res["ok"] and res["shared_identifiers"] > 25
    (ctx.ws / "crate" / "src" / "kernel.rs").write_text("fn independent_work() {}\n")
    assert actions.leak_check(ctx)["ok"]


# ---- prompts, tools, agent output ----

def test_prompts_render_without_holes(tmp_path: Path) -> None:
    ctx = _ctx(tmp_path)
    extra = {k: "x" for k in ("feedback", "missed", "failure", "phase", "request", "verify_cmd", "best_score", "noise_pct", "history", "profile",
                              "owed", "message", "mixes", "structural_left")}
    for p in sorted((REPO / "pipeline" / "prompts" / "verus").glob("*.md")):
        text = render(ctx, p.name, extra)
        assert "{{" not in text, p.name
        assert "Ground rules" in text, p.name


def test_every_stage_tool_is_served() -> None:
    for stage, names in STAGE_TOOLS.items():
        assert stage.startswith("verus_")
        assert set(names) <= set(TOOLS), (stage, set(names) - set(TOOLS))
    mcp = (REPO / "pipeline" / "orchestrator" / "accel_pipeline" / "mcp_server.py").read_text()
    for name in TOOLS:
        assert f"def {name}(" in mcp, name


def test_verus_steps_are_codex_exec() -> None:
    import yaml

    models = yaml.safe_load((REPO / "pipeline" / "config" / "models.codex.yaml").read_text())
    for stage in STAGE_TOOLS:
        assert models["stage_providers"].get(stage) == "codex_exec", stage
        assert stage in models["stages"], stage


def test_codex_backend_limits_verus_tools() -> None:
    from accel_pipeline.backends.codex_exec import CodexExecBackend

    assert CodexExecBackend._engine_impl_mcp_tools("verus_kernel") == STAGE_TOOLS["verus_kernel"]
    assert CodexExecBackend._engine_impl_mcp_tools("optimization_worker") is None


def test_json_block_takes_the_last_fence() -> None:
    text = 'x\n```json\n{"a": 1}\n```\nmore\n```json\n{"target": "t", "axis": "read"}\n```\n'
    assert json_block(text) == {"target": "t", "axis": "read"}
    assert json_block('done {"axis": "both"}\n') is None or json_block('{"axis": "both"}') == {"axis": "both"}


# ---- front end ----

def test_front_end_routes_verus_mode(monkeypatch: pytest.MonkeyPatch) -> None:
    import accel_pipeline.run_pipeline as rp
    import accel_pipeline.verus.run as vr

    seen: dict[str, object] = {}

    def fake_run(pipeline_config, model_config, workload, workload_name, query, **kw):  # noqa: ANN001
        seen.update(workload_name=workload_name, query=query, **kw)
        return 0

    monkeypatch.setattr(vr, "run_verus_pipeline", fake_run)
    monkeypatch.setattr(rp, "load_repo_dotenv", lambda root: None)
    monkeypatch.setenv("OPENROUTER_API_KEY", "x")
    rc = rp.main(["--query", "21", "--workload", "tpch_q21_aux", "--mode", "verus", "--stage", "spec",
                  "--models-config", str(REPO / "pipeline" / "config" / "models.codex.yaml")])
    assert rc == 0 and seen["workload_name"] == "tpch_q21_aux" and seen["stage"] == "spec"


def test_pack_passes_mode() -> None:
    from accel_pipeline.run_pack import parse_pack_queries

    qs = parse_pack_queries({"mode": "verus", "queries": [21, {"query": 3, "mode": "cpp"}]})
    assert [q.mode for q in qs] == ["verus", "cpp"]


def test_outputs_live_in_the_query_directory(tmp_path: Path) -> None:
    """Glue, header and kernel crate go under accelerators/<bench>/q<N>/verus_kit/<crate>/."""
    ctx = _ctx(tmp_path)
    assert ctx.kit_dir == ctx.root / "accelerators" / ctx.benchmark / f"q{ctx.query}" / "verus_kit" / ctx.crate
    assert ctx.crate_dir == ctx.kit_dir / "crate"


def test_sandbox_mounts_only_the_tree_and_tools(tmp_path: Path) -> None:
    import shutil

    from accel_pipeline.verus import sandbox

    ctx = _ctx(tmp_path)
    ctx.root.mkdir(parents=True, exist_ok=True)
    binds = sandbox.mounts(ctx)
    assert ("rw", ctx.root.resolve(), ctx.root.resolve()) in binds
    for mode, src, _ in binds:  # nothing else writable but the crate cache
        assert mode != "rw" or src in (ctx.root.resolve(), Path.home() / ".cargo", actions.cargo_target_dir(ctx))
    if shutil.which("bwrap"):
        args = sandbox.prefix(ctx, ctx.ws)
        assert args[args.index("--tmpfs", args.index("/run")) + 1] == "/home"  # $HOME hidden before the binds
        assert args[-2:] == ["--chdir", str(ctx.ws)]


def test_codex_session_id_from_banner_or_log(tmp_path: Path) -> None:
    import time as _time

    from accel_pipeline.backends.codex_exec import CodexExecBackend

    sid = "01a0fdce-14e4-71c1-acd6-2e3ad015fbd8"
    assert CodexExecBackend._session_id(f"workdir: /x\nsession id: {sid}\n", None, 0) == sid
    d = tmp_path / "sessions" / "2026" / "10" / "02"
    d.mkdir(parents=True)
    (d / f"rollout-2026-10-02T14-08-56-{sid}.jsonl").write_text("{}")
    assert CodexExecBackend._session_id("no banner", str(tmp_path), _time.time() - 60) == sid
    assert CodexExecBackend._session_id("no banner", str(tmp_path), _time.time() + 60) == ""


def test_gate_failures_go_back_to_the_same_session(monkeypatch: pytest.MonkeyPatch, tmp_path: Path) -> None:
    from types import SimpleNamespace

    from accel_pipeline.verus import stages

    calls = []

    class FakeRun:
        ctx = SimpleNamespace(setting=lambda key, default=None: default)
        model_config = SimpleNamespace(stages={})

        def note(self, msg): pass

        def gate(self, name, data): return data

        def agent(self, stage, prompt, task, extra=None, timeout_sec=None, resume="", tolerate_timeout=False):
            calls.append((stage, prompt, resume))
            return SimpleNamespace(session_id=resume or "fresh", final_text="")

    outcomes = iter([{"ok": False, "stage": "tests"}, {"ok": True}])
    monkeypatch.setattr(stages, "_kernel_gates", lambda r, **kw: next(outcomes))
    first = {"ok": False, "stage": "verify", "output": "error: postcondition not satisfied"}
    g = stages._fix_loop(FakeRun(), first, 4, snapshots_too=False, why="kernel", session=("verus_kernel", "S1"))
    assert g["ok"]
    assert calls == [("verus_kernel", "", "S1"), ("verus_kernel", "", "S1")]
    calls.clear()
    outcomes = iter([{"ok": True}])
    stages._fix_loop(FakeRun(), first, 4, snapshots_too=False, why="kernel")  # no session: a fresh fixer
    assert calls == [("verus_kernel_fix", "04b_kernel_fix.md", "")]


def test_review_prompts_inline_their_material(tmp_path: Path) -> None:
    ctx = _ctx(tmp_path)
    (ctx.ws / "query").mkdir(parents=True)
    (ctx.ws / "query" / "query.sql").write_text("SELECT 42 AS answer")
    (ctx.ws / "glue").mkdir()
    (ctx.ws / "glue" / "glue.cpp").write_text("// the glue")
    text = render(ctx, "01_spec_audit.md")
    assert "SELECT 42 AS answer" in text and "```sql" in text
    assert "`spec/gen_spec.rs`: (not present)" in text
    assert "// the glue" in render(ctx, "03d_glue_audit.md")


def test_spec_audit_is_reused_only_for_the_same_inputs(tmp_path: Path) -> None:
    from types import SimpleNamespace

    from accel_pipeline.verus import stages

    ws = tmp_path / "ws"
    for rel, body in (("query/query.sql", "SELECT 1"), ("query/schema.sql", "CREATE TABLE t (a INT)"),
                      ("spec/gen_spec.rs", "spec")):
        (ws / rel).parent.mkdir(parents=True, exist_ok=True)
        (ws / rel).write_text(body)
    ctx = SimpleNamespace(ws=ws, pipeline_root=str(tmp_path / "pipeline"), workload_name="w", query=1)
    key = stages._spec_audit_key(ctx)
    assert stages._reused_spec_audit(ctx, key) is None
    stages._record_spec_audit(ctx, key, {"verdict": "ok", "issues": []})
    assert stages._reused_spec_audit(ctx, key)["verdict"] == "ok"
    (ws / "spec/gen_spec.rs").write_text("a different spec")
    assert stages._reused_spec_audit(ctx, stages._spec_audit_key(ctx)) is None


def test_parallel_jobs_overlap_and_failures_stop_the_run(tmp_path: Path) -> None:
    import time as _time

    from accel_pipeline.verus import stages

    ctx = _ctx(tmp_path)
    ctx.art.mkdir(parents=True, exist_ok=True)
    run = stages.Run(ctx, stages.VerusState(), {}, None, None, {}, False)
    seen = []

    def slow(name):
        def job(r):
            _time.sleep(0.5)
            seen.append(name)
        return job

    t0 = _time.monotonic()
    run.parallel({"a": slow("a"), "b": slow("b"), "c": slow("c")})
    assert sorted(seen) == ["a", "b", "c"] and _time.monotonic() - t0 < 1.2  # overlapped, not 1.5 s

    def bad(r):
        raise stages.PipelineStop("the obligations are wrong")

    seen.clear()
    with pytest.raises(stages.PipelineStop, match="obligations are wrong"):
        run.parallel({"audit": bad, "tests": slow("tests")})
    assert seen == ["tests"]  # the other job still finished before the stop


def test_mcp_wrappers_pass_every_tool_parameter() -> None:
    """Each verus_* MCP wrapper takes exactly its tool's parameters (a dropped one is silently ignored)."""
    import ast
    import inspect

    from accel_pipeline.verus.tools import TOOLS

    src = (REPO / "pipeline" / "orchestrator" / "accel_pipeline" / "mcp_server.py").read_text()
    wrappers = {n.name: [a.arg for a in n.args.args] for n in ast.walk(ast.parse(src))
                if isinstance(n, ast.FunctionDef) and n.name in TOOLS}
    for name, fn in TOOLS.items():
        assert name in wrappers, f"{name} has no MCP wrapper"
        assert wrappers[name] == list(inspect.signature(fn).parameters), name


def test_stalled_fix_rounds_get_a_second_opinion(monkeypatch: pytest.MonkeyPatch) -> None:
    """No progress for `escalate_after_stalled_rounds` rounds: the advisor (a different model)
    diagnoses once, its advice goes to the same session (same stage, same model), with extra
    rounds; progress (more verified) resets the count."""
    from types import SimpleNamespace

    from accel_pipeline.verus import stages

    calls, advised = [], []

    class FakeRun:
        ctx = SimpleNamespace(setting=lambda key, default=None: default)
        model_config = SimpleNamespace(stages={})

        def note(self, msg): pass

        def gate(self, name, data): return data

        def agent(self, stage, prompt, task, extra=None, timeout_sec=None, resume="", tolerate_timeout=False):
            calls.append((stage, resume, "ADVICE-TEXT" in task))
            return SimpleNamespace(session_id=resume, final_text="")

    stuck = {"ok": False, "stage": "verify", "errors": 3, "verified": 100}
    outcomes = iter([{**stuck, "verified": 104}, stuck, stuck, {**stuck, "errors": 1}, {"ok": True}])
    monkeypatch.setattr(stages, "_kernel_gates", lambda r, **kw: next(outcomes))
    monkeypatch.setattr(stages, "_advice", lambda r, g, why, proofs: advised.append(g) or "ADVICE-TEXT")
    g = stages._fix_loop(FakeRun(), stuck, 4, snapshots_too=False, why="proofs", session=("verus_kernel", "S1"))
    assert g["ok"]
    assert len(advised) == 1
    # round 1 progressed; rounds 2-3 stalled; round 4 carries the advice (same stage and session); round 5 plain
    assert calls == [("verus_kernel", "S1", False)] * 3 + [("verus_kernel", "S1", True), ("verus_kernel", "S1", False)]
    from accel_pipeline.verus.tools import stage_tools
    assert "verus_verify" in stage_tools("verus_kernel_advisor")


# ───────────────────────────── random parameters ─────────────────────────────


def _pctx(tmp: Path, query: int = 21, sql: str = "SELECT 1 WHERE x = $1") -> VerusContext:
    ctx = _ctx(tmp)
    ctx.query = query
    ctx.repo_root = str(REPO)
    q = tmp / "q.sql"
    q.write_text(sql)
    ctx.query_sql = str(q)
    return ctx


def test_draw_seeds_differ_and_are_recorded(tmp_path):
    from accel_pipeline.verus import params as qp

    ctx = _pctx(tmp_path)
    draws = [qp.new_draw(ctx, "bench") for _ in range(4)]
    assert len({d.seed for d in draws}) == 4
    for d in draws:
        rec = json.loads(d.record.read_text())
        assert rec["seed"] == d.seed and rec["sets"] == d.sets
        assert d.path.read_text().strip().splitlines() == d.sets


def test_draw_replays_with_its_seed_and_stays_in_domain(tmp_path):
    from accel_pipeline.verus import params as qp

    ctx = _pctx(tmp_path)
    a = qp.new_draw(ctx, "a", seed=123)
    b = qp.new_draw(ctx, "b", seed=123)
    assert a.sets == b.sets
    nations = {"ALGERIA", "ARGENTINA", "BRAZIL", "CANADA", "EGYPT", "ETHIOPIA", "FRANCE", "GERMANY", "INDIA",
               "INDONESIA", "IRAN", "IRAQ", "JAPAN", "JORDAN", "KENYA", "MOROCCO", "MOZAMBIQUE", "PERU", "CHINA",
               "ROMANIA", "SAUDI ARABIA", "VIETNAM", "RUSSIA", "UNITED KINGDOM", "UNITED STATES"}
    for _ in range(5):
        for t in qp.new_draw(ctx, "c").sets:
            assert qp.split_tuple(t)[0].strip("'") in nations  # q21: one nation


def test_draw_arity_matches_the_query(tmp_path):
    from accel_pipeline.verus import params as qp

    ctx = _pctx(tmp_path, query=12, sql="... $1 $2 $3 $4")
    for t in qp.new_draw(ctx, "x").sets:
        assert len(qp.split_tuple(t)) == 4
    assert qp.split_tuple("'A, B', DATE '1995-01-01', 0.05") == ["'A, B'", "DATE '1995-01-01'", "0.05"]
    assert qp.split_tuple("'it''s, ok', 3") == ["'it''s, ok'", "3"]


def test_no_parameters_no_draw(tmp_path):
    from accel_pipeline.verus import params as qp

    assert qp.new_draw(_pctx(tmp_path, sql="SELECT 1"), "x") is None


def test_check_query_rejects_literal_like(tmp_path):
    from accel_pipeline.verus import params as qp

    # q2's type pattern left as a literal: fewer $n than its domain
    ctx = _pctx(tmp_path / "a", query=2, sql="SELECT 1 WHERE p_type LIKE '%BRASS' AND s = $1 AND r = $2")
    assert "literal" in qp.check_query(ctx)
    # a literal the benchmark fixes (q16's complaints pattern) is not a substitution parameter
    q16 = ("SELECT 1 WHERE p_brand <> $1 AND p_type NOT LIKE $2 AND p_size IN ($3, $4, $5, $6, $7, $8, $9, $10) "
           "AND s_comment LIKE '%Customer%Complaints%'")
    assert qp.check_query(_pctx(tmp_path / "d", query=16, sql=q16)) == ""
    ctx = _pctx(tmp_path / "b", query=21, sql="SELECT 1 WHERE n = $1 AND p LIKE $2")
    assert "parameters" in qp.check_query(ctx)  # q21's domain has one parameter
    assert qp.check_query(_pctx(tmp_path / "c", query=21)) == ""


def test_paired_bench_shares_one_draw_and_new_decisions_differ(tmp_path, monkeypatch):
    from accel_pipeline.verus import stages
    from accel_pipeline.verus import params as qp

    ctx = _pctx(tmp_path)
    seen = []

    def fake_bench(c, mixes, tag="", runner=None, draw=None, **kw):
        seen.append((tag, runner, draw.seed))
        return {"ok": True, "read_speedup": 2.0 if runner == "cand" else 1.0, "mixes": []}

    monkeypatch.setattr(stages.actions, "bench", fake_bench)

    class R:  # the parts of Run that _paired_bench uses
        pass

    r = R()
    r.ctx = ctx
    first = stages._paired_bench(r, "attempt_1", "best", "cand")
    second = stages._paired_bench(r, "attempt_2", "best", "cand")
    seeds1 = {s for t, _, s in seen if t.startswith("attempt_1")}
    seeds2 = {s for t, _, s in seen if t.startswith("attempt_2")}
    assert len(seen) == 12 and len(seeds1) == 1 and len(seeds2) == 1 and seeds1 != seeds2
    assert first["param_seed"] in seeds1 and second["param_seed"] in seeds2
    assert (Path(ctx.art) / "param_draws").is_dir()


def test_random_params_added_once_to_frozen_tests(tmp_path):
    ctx = _pctx(tmp_path)
    (Path(ctx.ws) / "tests").mkdir(parents=True)
    (Path(ctx.ws) / "tests" / "params.sql").write_text("'FRANCE'\n")
    r1 = actions.add_random_params(ctx)
    text = (Path(ctx.ws) / "tests" / "params.sql").read_text()
    assert r1["added"] > 0 and text.startswith("'FRANCE'\n") and actions.RANDOM_PARAMS_MARK in text
    assert actions.add_random_params(ctx) == {"added": 0}
    assert (Path(ctx.ws) / "tests" / "params.sql").read_text() == text


def test_runner_env_modes(tmp_path):
    from accel_pipeline.verus import params as qp

    d = qp.new_draw(_pctx(tmp_path), "x")
    assert "VERUS_KIT_PARAMS_FILE" in qp.runner_env(d, "replace")
    assert "VERUS_KIT_EXTRA_PARAMS_FILE" in qp.runner_env(d, "extra")
    assert qp.runner_env(None, "replace") == {}

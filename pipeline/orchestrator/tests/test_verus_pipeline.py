"""Verus pipeline (--mode verus): stopping logic, trust gates, prompts, routing."""

from __future__ import annotations

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
    excluded = root / "src" / "verus" / "q99" / "src"
    excluded.mkdir(parents=True)
    body = "\n".join(f"    let distinctive_name_{i} = compute_something_special_{i}(argument_value);" for i in range(40))
    (excluded / "maint.rs").write_text(body)
    for d in ("common", "substrate", "prooflib"):
        (root / "src" / "verus" / d / "src").mkdir(parents=True)
    (ctx.ws / "crate" / "src" / "kernel.rs").write_text(body)
    res = actions.leak_check(ctx)
    assert not res["ok"] and res["shared_identifiers"] > 25
    (ctx.ws / "crate" / "src" / "kernel.rs").write_text("fn independent_work() {}\n")
    assert actions.leak_check(ctx)["ok"]


# ---- prompts, tools, agent output ----

def test_prompts_render_without_holes(tmp_path: Path) -> None:
    ctx = _ctx(tmp_path)
    extra = {k: "x" for k in ("feedback", "missed", "failure", "best_score", "noise_pct", "history", "profile",
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
    """Glue, header and kernel crate go under src/generated/<bench>/q<N>/verus_kit/<crate>/."""
    ctx = _ctx(tmp_path)
    assert ctx.kit_dir == ctx.root / "src" / "generated" / ctx.benchmark / f"q{ctx.query}" / "verus_kit" / ctx.crate
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
        def note(self, msg): pass

        def gate(self, name, data): return data

        def agent(self, stage, prompt, task, extra=None, timeout_sec=None, resume=""):
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

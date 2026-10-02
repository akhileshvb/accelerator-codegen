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
        assert mode != "rw" or src in (ctx.root.resolve(), Path.home() / ".cargo")
    if shutil.which("bwrap"):
        args = sandbox.prefix(ctx, ctx.ws)
        assert args[args.index("--tmpfs", args.index("/run")) + 1] == "/home"  # $HOME hidden before the binds
        assert args[-2:] == ["--chdir", str(ctx.ws)]

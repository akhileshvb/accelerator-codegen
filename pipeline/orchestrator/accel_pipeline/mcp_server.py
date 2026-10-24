"""stdio MCP server exposing pipeline measurement tools for Codex exec."""

from __future__ import annotations

import argparse
import json
import os
import sys
from pathlib import Path
from typing import Any, Callable

from accel_pipeline.jsonio import read_json


def _env_int(name: str, default: int = 0) -> int:
    raw = os.environ.get(name, "")
    try:
        return int(raw) if raw else default
    except ValueError:
        return default


def _resolve_query(args: argparse.Namespace) -> int:
    raw = os.environ.get("ACCEL_PIPELINE_QUERY")
    if raw is not None and str(raw).strip() != "":
        return int(raw)
    return int(args.query or 0)


def _build_context(args: argparse.Namespace):
    from accel_pipeline.config import load_pipeline_config, load_workload, apply_workload_overrides
    from accel_pipeline.paths import find_repo_root, query_artifacts_dir

    repo_root = Path(args.repo_root or os.environ.get("ACCEL_PIPELINE_REPO_ROOT") or find_repo_root())
    pipeline_config = load_pipeline_config(repo_root=repo_root)
    query = _resolve_query(args)
    workload_name = args.workload or os.environ.get("ACCEL_PIPELINE_WORKLOAD") or "tpch_q3"
    stage = args.stage or os.environ.get("ACCEL_PIPELINE_STAGE") or "optimization_worker"
    optimization_iter = int(args.optimization_iter or _env_int("ACCEL_PIPELINE_OPT_ITER", 0))
    workload = load_workload(workload_name, repo_root, engine=os.environ.get("ACCEL_PIPELINE_ENGINE", ""))
    pipeline_config = apply_workload_overrides(pipeline_config, workload)
    art = query_artifacts_dir(repo_root, query, workload=workload)
    return pipeline_config, workload, workload_name, query, stage, optimization_iter, art


def build_mcp_tool_callables(
    pipeline_config,
    workload: dict[str, Any],
    workload_name: str,
    query: int,
    stage: str,
    optimization_iter: int,
    art: Path,
) -> dict[str, Callable[..., str]]:
    from accel_pipeline.gate_stamps import current_stamp
    from accel_pipeline.gates import run_gate
    from accel_pipeline.tools.ab_tools import AbToolContext, make_ab_tools
    from accel_pipeline.tools.gate_tools import ToolContext as GateCtx
    from accel_pipeline.tools.gate_tools import make_gate_tools
    from accel_pipeline.tools.reanalysis_tools import ToolContext as ReanalysisCtx
    from accel_pipeline.tools.reanalysis_tools import make_reanalysis_tools
    from accel_pipeline.tools.shell_tools import ToolContext as ShellCtx
    from accel_pipeline.tools.shell_tools import make_shell_tools
    from accel_pipeline.tools.gate_tools import _compact_gate_payload
    from accel_pipeline.tools.quick_tools import make_quick_tools

    gate_ctx = GateCtx(pipeline_config, query, workload=workload, stage=stage, workload_name=workload_name)
    request_gate, run_engine_build, run_layer_gate, gate_status = make_gate_tools(gate_ctx)
    quick_check = make_quick_tools(gate_ctx)
    run_command = make_shell_tools(ShellCtx(pipeline_config, query))
    request_structural_reanalysis = make_reanalysis_tools(
        ReanalysisCtx(pipeline_config, query, stage, workload_name, optimization_iter)
    )
    ab_ctx = AbToolContext(
        pipeline_config=pipeline_config,
        query=query,
        workload=workload,
        optimization_iter=optimization_iter,
    )
    begin_ab_trial, run_quick_benchmark, end_ab_trial = make_ab_tools(ab_ctx)

    def _run_named_gate(gate_name: str, env: dict[str, str] | None = None) -> str:
        result = run_gate(
            pipeline_config,
            gate_name,
            query,
            workload=workload,
            raise_on_failure=False,
            allow_skip=True,
            env=env,
        )
        return json.dumps(_compact_gate_payload(result.name, result.data), indent=2)

    def _cached_by_code_stamp(name: str, measure: Callable[[], str]) -> str:
        """`measure()`, or its last result when the code is exactly what it measured (build stamp)."""
        path = art / f"{name}_cache.json"
        stamp = current_stamp(pipeline_config, workload, query)
        cached = read_json(path)
        if stamp and cached.get("stamp") == stamp:
            return f"{cached['payload']}\n(cached: the code is unchanged since this was measured)"
        payload = measure()
        if stamp and current_stamp(pipeline_config, workload, query) == stamp:
            path.write_text(json.dumps({"stamp": stamp, "payload": payload}), encoding="utf-8")
        return payload

    def run_profile() -> str:
        """Refresh profile artifacts. Aux: quick instrumented bench (not full fair sweep)."""

        def measure() -> str:
            if workload.get("workload_kind") == "aux_state":
                # Full aux_bench_sweep is too heavy mid-loop; use the same quick path as A/B.
                out = run_quick_benchmark(True)
                digest = get_profile_digest()
                return json.dumps(
                    {
                        "mode": "aux_quick_profile",
                        "quick_benchmark": out[:12_000],
                        "digest": json.loads(digest) if digest.startswith("{") else {"raw": digest},
                    },
                    indent=2,
                )[:50_000]
            return _run_named_gate("profile", env={"ACCEL_BENCH_WARMUP": "1", "ACCEL_BENCH_ITERS": "3"})

        return _cached_by_code_stamp("profile", measure)

    def run_correctness() -> str:
        """Run correctness / aux_correctness gate (smoke + SF1 differential for aux)."""
        name = "aux_correctness" if workload.get("workload_kind") == "aux_state" else "correctness"
        return _run_named_gate(name)

    def run_multi_connection() -> str:
        """Run aux_multi_connection gate (registered entry points + multi-conn stress). Aux-state only."""
        if workload.get("workload_kind") != "aux_state":
            return json.dumps(
                {
                    "gate": "aux_multi_connection",
                    "status": "skip",
                    "reason": "not an aux_state workload",
                },
                indent=2,
            )
        return _run_named_gate("aux_multi_connection")

    def run_baseline() -> str:
        """Run baseline / aux unit gate used for timing baselines."""
        name = "aux_unit" if workload.get("workload_kind") == "aux_state" else "baseline"
        return _run_named_gate(name)

    def run_fairness() -> str:
        """Run the fairness gate."""
        return _run_named_gate("fairness")

    def get_profile_digest() -> str:
        """Compact phase counters / dominant bottleneck from last profile artifacts."""
        from accel_pipeline.config import resolve_optimization_edit_paths
        from accel_pipeline.optimization_scoreboard import (
            build_scoreboard,
            custom_profiling_contract,
            profile_freshness,
            write_scoreboard,
        )

        edit = resolve_optimization_edit_paths(pipeline_config, workload, query)
        editable = [p for p in edit.get("all_paths") or [] if p]
        is_aux = workload.get("workload_kind") == "aux_state"

        if is_aux:
            from accel_pipeline.aux_profile import build_aux_profile_digest

            payload = build_aux_profile_digest(
                art,
                repo_root=pipeline_config.repo_root,
                editable_rel_paths=editable,
            )
            feedback = ""
            fb_path = art / "optimization_feedback.md"
            if fb_path.is_file():
                feedback = fb_path.read_text(encoding="utf-8")[-4000:]
            payload["optimization_feedback_tail"] = feedback
            return json.dumps(payload, indent=2)[:50_000]

        profile = read_json(art / "profile_summary.json")
        baseline = read_json(art / "baseline_summary.json")
        board = build_scoreboard(art, is_aux=False, last_metrics=profile or baseline)
        write_scoreboard(art, board)
        freshness = profile_freshness(
            art,
            repo_root=pipeline_config.repo_root,
            editable_rel_paths=editable,
        )
        feedback = ""
        fb_path = art / "optimization_feedback.md"
        if fb_path.is_file():
            feedback = fb_path.read_text(encoding="utf-8")[-4000:]
        payload = {
            "scoreboard": board,
            "freshness": freshness,
            "profile_summary": profile,
            "baseline_summary": baseline,
            "custom_profiling": custom_profiling_contract(is_aux=False),
            "measurement_rules": [
                "Call run_profile on the CURRENT tree before forming a hypothesis.",
                "If freshness.stale is true, do not hypothesize from this digest — remeasure.",
                "A/B keep/regress vs scoreboard.best_valid_score only.",
            ],
            "optimization_feedback_tail": feedback,
        }
        return json.dumps(payload, indent=2)[:50_000]

    def get_optimization_evidence() -> str:
        """Bottleneck evidence packet from artifacts when available."""
        runtime = art / "optimization_runtime.md"
        evidence = art / "optimization_evidence.json"
        parts: list[str] = []
        if evidence.is_file():
            parts.append(evidence.read_text(encoding="utf-8")[:40_000])
        if runtime.is_file():
            parts.append(runtime.read_text(encoding="utf-8")[:40_000])
        if workload.get("workload_kind") == "aux_state":
            # Always append a fresh aux digest so empty/mis-shaped runtime still helps.
            parts.append(get_profile_digest())
        if not parts:
            return get_profile_digest()
        return "\n\n".join(parts)[:80_000]

    def get_store_diagnostics() -> str:
        """Aux store diagnostics from bench/opt state when present."""
        if workload.get("workload_kind") == "aux_state":
            from accel_pipeline.aux_profile import load_aux_store_diagnostics

            diag, source = load_aux_store_diagnostics(art)
            if diag:
                return json.dumps({"source": source, "store_diagnostics": diag}, indent=2)
            return json.dumps(
                {
                    "store_diagnostics": {},
                    "note": "no aux diagnostics yet — call run_profile or run_quick_benchmark",
                }
            )
        for name in ("bench_summary.json", "optimization_state.json", "baseline_summary.json"):
            data = read_json(art / name)
            diag = data.get("store_diagnostics") or data.get("median_metrics", {}).get("store_diagnostics")
            if diag:
                return json.dumps({"source": name, "store_diagnostics": diag}, indent=2)
        return json.dumps({"store_diagnostics": {}, "note": "no diagnostics on disk yet"})

    def run_pipeline_command(command: str) -> str:
        """Allowlisted cmake/ctest only (same policy as pipeline shell tools)."""
        return run_command(command)

    def acquire_shared_lease(reason: str = "", timeout_sec: float = 600.0) -> str:
        """Hold shared-file lease before editing catalog/CMake/shared registration files."""
        from accel_pipeline.shared_file_lease import (
            ensure_sticky_lease_for_write,
            worker_id_from_env,
        )

        holder = pipeline_config.worker_id or worker_id_from_env(f"q{query}")
        info = ensure_sticky_lease_for_write(
            pipeline_config.repo_root,
            holder=holder,
            path=None,
            timeout_sec=float(timeout_sec),
        )
        return json.dumps(
            {
                "acquired": True,
                "holder": holder,
                "reason": reason,
                "lease": info.to_dict() if info else {},
            },
            indent=2,
        )

    def release_shared_lease() -> str:
        """Release sticky shared-file lease after shared edits + rebuild."""
        from accel_pipeline.shared_file_lease import release_sticky_lease, worker_id_from_env

        holder = pipeline_config.worker_id or worker_id_from_env(f"q{query}")
        ok = release_sticky_lease(pipeline_config.repo_root, holder=holder)
        return json.dumps({"released": ok, "holder": holder})

    def primitive_publish(files: list[str], rationale: str) -> str:
        """Publish staged primitives to the shared accelerators/prims/ library (additions only)."""
        from accel_pipeline.prims_library import publish_tool

        return publish_tool(pipeline_config, query, files, rationale)

    def discover_host_sdk() -> str:
        """Describe this engine's host.sh verbs and how to connect."""
        from accel_pipeline.engine_host import discover_host_sdk_json

        return discover_host_sdk_json(workload, pipeline_config.repo_root, pipeline_config.build_dir)

    return {
        "request_gate": request_gate,
        "gate_status": gate_status,
        "quick_check": quick_check,
        "run_engine_build": run_engine_build,
        "run_layer_gate": run_layer_gate,
        "run_profile": run_profile,
        "run_correctness": run_correctness,
        "run_multi_connection": run_multi_connection,
        "run_baseline": run_baseline,
        "run_fairness": run_fairness,
        "get_profile_digest": get_profile_digest,
        "get_optimization_evidence": get_optimization_evidence,
        "get_store_diagnostics": get_store_diagnostics,
        "begin_ab_trial": begin_ab_trial,
        "run_quick_benchmark": run_quick_benchmark,
        "end_ab_trial": end_ab_trial,
        "request_structural_reanalysis": request_structural_reanalysis,
        "run_pipeline_command": run_pipeline_command,
        "acquire_shared_lease": acquire_shared_lease,
        "release_shared_lease": release_shared_lease,
        "primitive_publish": primitive_publish,
        "discover_host_sdk": discover_host_sdk,
    }


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="Accelerator pipeline MCP server (stdio)")
    parser.add_argument("--query", type=int, default=0)
    parser.add_argument("--workload", type=str, default="")
    parser.add_argument("--stage", type=str, default="")
    parser.add_argument("--optimization-iter", type=int, default=0)
    parser.add_argument("--repo-root", type=str, default="")
    args = parser.parse_args(argv)

    # Allow initialize / tools/list even if query env is not ready yet.
    # Tool handlers re-resolve context from env on each call so a late-written
    # sidecar env file (or inherited ACCEL_* vars) still works.
    query = _resolve_query(args)
    if os.environ.get("ACCEL_PIPELINE_QUERY") is None and args.query == 0:
        print(
            "accel_pipeline MCP: ACCEL_PIPELINE_QUERY unset at startup; "
            "tools will resolve context on first call",
            file=sys.stderr,
        )

    def _context():
        context = _build_context(args)
        # Query 0 is valid (engine_backend). Only fail when neither CLI nor env set a query.
        if os.environ.get("ACCEL_PIPELINE_QUERY") is None and args.query == 0:
            raise RuntimeError(
                "ACCEL_PIPELINE_QUERY / --query required for measurement tools "
                "(CodexExecBackend should write pipeline/.codex/accel_pipeline_mcp.env)"
            )
        return context

    def _budget_footer(pipeline_config, stage: str, q: int) -> str:
        """The wall-budget notice (empty until a deadline is near); never fails a tool call."""
        try:
            from accel_pipeline.session import budget_footer

            footer = budget_footer(pipeline_config, stage, q)
        except Exception:  # noqa: BLE001
            return ""
        return f"\n\n{footer}" if footer else ""

    def _call(name: str, *a, **kw) -> str:
        try:
            pipeline_config, workload, workload_name, q, stage, optimization_iter, art = _context()
            tools = build_mcp_tool_callables(
                pipeline_config, workload, workload_name, q, stage, optimization_iter, art
            )
            return tools[name](*a, **kw) + _budget_footer(pipeline_config, stage, q)
        except Exception as exc:  # noqa: BLE001 — surface to MCP client
            return json.dumps({"error": str(exc), "tool": name})

    # mcp>=2 removed FastMCP; prefer MCPServer. Keep FastMCP fallback for mcp 1.x.
    mcp = None
    try:
        from mcp.server import MCPServer

        mcp = MCPServer("accel_pipeline")
    except ImportError:
        try:
            from mcp.server.fastmcp import FastMCP

            mcp = FastMCP("accel_pipeline")
        except ImportError:
            print(
                "mcp package required for Codex MCP server; "
                "pip install -e '.[codex]' (need mcp with MCPServer or FastMCP)",
                file=sys.stderr,
            )
            return 2

    # Annotations help Codex `auto`/`approve` skip the interactive app-tool
    # approval path that cancels under `codex exec` (openai/codex#16632).
    try:
        try:
            from mcp.types import ToolAnnotations
        except ImportError:
            from mcp_types import ToolAnnotations  # type: ignore

        _ro = ToolAnnotations(
            readOnlyHint=True,
            destructiveHint=False,
            idempotentHint=True,
            openWorldHint=False,
        )
        _rw = ToolAnnotations(
            readOnlyHint=False,
            destructiveHint=False,
            idempotentHint=False,
            openWorldHint=False,
        )
    except Exception:  # noqa: BLE001
        _ro = None
        _rw = None

    def _tool(**kwargs):
        if _ro is None:
            kwargs.pop("annotations", None)
        return mcp.tool(**kwargs)

    @_tool(annotations=_rw)
    def request_gate(gate_name: str, background: bool = False) -> str:
        """MCP TOOL (not a resource — do not list_mcp_resources).

        Run an official orchestrator gate by name. Returns JSON with status,
        notes, and on fail failure_summary. Engine backend: pass engine_build
        then this layer's gate (engine_capture, engine_epochs, engine_scan,
        engine_runtime, engine_host). Prefer run_engine_build / run_layer_gate when listed.
        Keep calling until status=pass. Do not substitute host cmake or docker
        exec of the harness for this tool.
        Aux gates are for acceptance and take minutes; use quick_check in the edit loop.
        background=True starts aux_correctness, aux_unit, prims_tests, aux_multi_connection or
        aux_cross_accel in its own process and returns at once; read it once with gate_status.
        """
        return _call("request_gate", gate_name, background)

    @_tool(annotations=_ro)
    def gate_status(gate_name: str) -> str:
        """State of a background gate (request_gate background=True): running, or its result.
        stale=true: the code changed after it ran, so it is rerun at stage exit. Never poll in a loop."""
        return _call("gate_status", gate_name)

    @_tool(annotations=_ro)
    def quick_check(targets: list[str] | None = None, differential: bool = True) -> str:
        """Seconds-level check of this query's accelerator: incremental build, its own tests, the
        differential on the small test subset. Run after every edit. targets: extra build targets of
        your own test binaries (built and run). Correctness only; the subset is never a tuning target."""
        return _call("quick_check", targets, differential)

    @_tool(annotations=_rw)
    def run_engine_build() -> str:
        """MCP TOOL. Official engine_build gate (docker ensure+build or host cmake).

        Call this after CMake/adapter changes. On fail, read failure_summary, fix, retry.
        """
        return _call("run_engine_build")

    @_tool(annotations=_rw)
    def run_layer_gate() -> str:
        """MCP TOOL. Official harness gate for this impl_* layer.

        Maps impl_capture→engine_capture, impl_epochs→engine_epochs, etc. Call after
        run_engine_build. On fail, read failure_summary, fix, retry until pass.
        """
        return _call("run_layer_gate")

    @_tool(annotations=_ro)
    def run_profile() -> str:
        """Run profile (or aux bench) gate."""
        return _call("run_profile")

    @_tool(annotations=_ro)
    def run_correctness() -> str:
        """Run correctness / aux_correctness gate (smoke + SF1 differential)."""
        return _call("run_correctness")

    @_tool(annotations=_ro)
    def run_multi_connection() -> str:
        """Run aux_multi_connection gate (entry-point registration + multi-conn stress)."""
        return _call("run_multi_connection")

    @_tool(annotations=_ro)
    def run_baseline() -> str:
        """Run baseline gate."""
        return _call("run_baseline")

    @_tool(annotations=_ro)
    def run_fairness() -> str:
        """Run fairness gate."""
        return _call("run_fairness")

    @_tool(annotations=_ro)
    def get_profile_digest() -> str:
        """Compact profile/baseline digest from artifacts."""
        return _call("get_profile_digest")

    @_tool(annotations=_ro)
    def get_optimization_evidence() -> str:
        """Optimization evidence / runtime packet."""
        return _call("get_optimization_evidence")

    @_tool(annotations=_ro)
    def get_store_diagnostics() -> str:
        """Aux store diagnostics if present."""
        return _call("get_store_diagnostics")

    @_tool(annotations=_rw)
    def begin_ab_trial(trial_id: str, hypothesis: str) -> str:
        """Begin exploratory A/B trial (non-binding)."""
        return _call("begin_ab_trial", trial_id, hypothesis)

    @_tool(annotations=_rw)
    def run_quick_benchmark(include_profile: bool = True, acceptance: bool = False) -> str:
        """Run the benchmark for the active A/B trial. Default: the quick tier, seconds, read axis only,
        direction not measurement. acceptance=True: the full optimization tier; end_ab_trial(keep=True)
        needs one."""
        return _call("run_quick_benchmark", include_profile, acceptance)

    @_tool(annotations=_rw)
    def end_ab_trial(trial_id: str, keep: bool = False) -> str:
        """End exploratory A/B trial."""
        return _call("end_ab_trial", trial_id, keep)

    @_tool(annotations=_rw)
    def request_structural_reanalysis(
        bottleneck_type: str,
        evidence: str,
        proposed_direction: str,
        current_candidate_id: str,
        alternate_candidate_ids: list[str] | None = None,
        cost_model_insufficient_reason: str = "",
    ) -> str:
        """Request aux structural re-analysis (escape hatch)."""
        return _call(
            "request_structural_reanalysis",
            bottleneck_type,
            evidence,
            proposed_direction,
            current_candidate_id,
            alternate_candidate_ids,
            cost_model_insufficient_reason,
        )

    @_tool(annotations=_rw)
    def run_pipeline_command(command: str) -> str:
        """Allowlisted cmake/ctest command."""
        return _call("run_pipeline_command", command)

    @_tool(annotations=_rw)
    def acquire_shared_lease(reason: str = "", timeout_sec: float = 600.0) -> str:
        """Hold shared-file lease before editing catalog/CMake/shared registration files."""
        return _call("acquire_shared_lease", reason, timeout_sec)

    @_tool(annotations=_rw)
    def release_shared_lease() -> str:
        """Release sticky shared-file lease after shared edits + rebuild."""
        return _call("release_shared_lease")

    @_tool(annotations=_rw)
    def primitive_publish(files: list[str], rationale: str) -> str:
        """Publish staged primitives to the shared accelerators/prims/ library.

        files: repo-relative paths (a header accelerators/prims/foo.hpp and
        accelerators/prims/tests/test_prims.cpp), each staged as a full file under
        pipeline/prims_staging/<same path>. The header comment declares PURPOSE, KEY SPACE,
        COMPLEXITY, MEMORY, COW/THREADS and USE WHEN; the README catalogue row is generated
        from it. Rejected when its name or purpose duplicates a published primitive (reuse or
        extend that one). Additions only, generic only; builds and runs test_prims before
        publishing. rationale: one line.
        """
        return _call("primitive_publish", files, rationale)

    @_tool(annotations=_ro)
    def discover_host_sdk() -> str:
        """Describe this engine's host.sh verbs and how to connect."""
        return _call("discover_host_sdk")

    # ---- Verus pipeline (--mode verus); context from ACCEL_VERUS_CONTEXT ----
    def _verus(name: str, *a, **kw) -> str:
        try:
            from accel_pipeline.verus.tools import TOOLS as VERUS_TOOLS

            return VERUS_TOOLS[name](*a, **kw)
        except Exception as exc:  # noqa: BLE001 — surface to MCP client
            return json.dumps({"error": str(exc), "tool": name})

    @_tool(annotations=_rw)
    def verus_check_inputs() -> str:
        """Check inputs/q.inputs with sql2verus: accepted (and its obligations) or the refusal."""
        return _verus("verus_check_inputs")

    @_tool(annotations=_ro)
    def verus_sql(query: str, max_rows: int = 50) -> str:
        """Read-only SQL on the test dataset database, to explore the data."""
        return _verus("verus_sql", query, max_rows)

    @_tool(annotations=_rw)
    def verus_validate_tests() -> str:
        """Check tests/: setup runs, every params tuple runs the query, every txn applies (rolled back)."""
        return _verus("verus_validate_tests")

    @_tool(annotations=_rw)
    def verus_glue_check() -> str:
        """Build with glue/ and check what the glue delivers against the declared inputs (record mode)."""
        return _verus("verus_glue_check")

    @_tool(annotations=_rw)
    def verus_verify() -> str:
        """Verify the kernel crate with Verus (errors on failure) plus the trusted-base check."""
        return _verus("verus_verify")

    @_tool(annotations=_rw)
    def verus_test(snapshots: bool = False, proofs: bool = True) -> str:
        """Build the accelerator; run the frozen tests on the Verus store (differential; snapshots if asked).
        proofs=false: compile the kernel without checking its proofs (code first)."""
        return _verus("verus_test", snapshots, proofs)

    @_tool(annotations=_rw)
    def verus_bench(write_mixes: str = "0.01,0.05") -> str:
        """Build and benchmark on SF1 (Verus store): read speedups, write mixes, score."""
        return _verus("verus_bench", write_mixes)

    @_tool(annotations=_rw)
    def verus_profile() -> str:
        """perf profile of a short SF1 bench: hotspots by symbol and area."""
        return _verus("verus_profile")

    @_tool(annotations=_ro)
    def prooflib_verify() -> str:
        """Verify this workspace's copy of the proof library (prooflib/src/lib.rs) on its own, without publishing."""
        return _verus("prooflib_verify")

    @_tool(annotations=_rw)
    def prooflib_publish(note: str) -> str:
        """Publish prooflib/src/lib.rs (additions only; dependents re-verified first)."""
        return _verus("prooflib_publish", note)

    @_tool(annotations=_rw)
    def playbook_append(section: str, title: str, body: str, evidence: str) -> str:
        """Add an evidence-backed lesson to the playbook (section: performance | proofs)."""
        return _verus("playbook_append", section, title, body, evidence)

    @_tool(annotations=_rw)
    def backend_addition_record(engine: str, helper: str, reason: str) -> str:
        """Request a generic engine-level helper from the engine_backend run (logs the request only;
        query runs never write backends/, so it is not available in this run)."""
        return _verus("backend_addition_record", engine, helper, reason)

    mcp.run(transport="stdio")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

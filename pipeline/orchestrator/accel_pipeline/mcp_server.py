"""stdio MCP server exposing the pipeline's tools to Codex exec.

The core tools (gates, quick_check, the data tools, the analysis edit tools) are always registered; the
tools of a mode (optimization, engine_backend, pack, verus) only when that mode runs, from its own package.
"""

from __future__ import annotations

import argparse
import json
import os
import sys
from pathlib import Path
from typing import Any, Callable, Literal

from accel_pipeline.gates.run import tool_call_limit
from accel_pipeline.workload import read_json
from accel_pipeline.session import record_check
from accel_pipeline.quick import profile_hot_spots
from accel_pipeline.session import keep_alive
from accel_pipeline.tools import mode_tools

_ENGINE_BACKEND_STAGES = ("impl_", "critique_", "layer_critique", "validation", "architecture_audit", "host_bootstrap")
_DEFAULT_STAGE = "optimization_worker"


def active_modes(stage: str, workload_name: str) -> list[str]:
    """The modes whose tools this MCP server serves, from the environment the agent was started with."""
    if os.environ.get("ACCEL_VERUS_CONTEXT"):
        return ["verus"]
    modes = []
    if workload_name.startswith("engine_") or stage.startswith(_ENGINE_BACKEND_STAGES):
        modes.append("engine_backend")
    if stage.startswith("optimization"):
        modes.append("optimization")
    if os.environ.get("ACCEL_PIPELINE_PACK_QUERIES") or os.environ.get("ACCEL_PIPELINE_PRIMARY_REPO"):
        modes.append("pack")
    return modes


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


def _budget_footer(pipeline_config, stage: str, query: int) -> str:
    """The wall-budget notice appended to every tool result (empty until a deadline is near); never fails a call."""
    try:
        from accel_pipeline.session import budget_footer

        footer = budget_footer(pipeline_config, stage, query)
    except Exception:  # noqa: BLE001
        return ""
    return f"\n\n{footer}" if footer else ""


def _build_context(args: argparse.Namespace):
    from accel_pipeline.config import load_pipeline_config, find_repo_root
    from accel_pipeline.workload import load_workload, apply_workload_overrides, query_artifacts_dir

    repo_root = Path(args.repo_root or os.environ.get("ACCEL_PIPELINE_REPO_ROOT") or find_repo_root())
    pipeline_config = load_pipeline_config(repo_root=repo_root)
    query = _resolve_query(args)
    workload_name = args.workload or os.environ.get("ACCEL_PIPELINE_WORKLOAD") or "tpch_q3"
    stage = args.stage or os.environ.get("ACCEL_PIPELINE_STAGE") or _DEFAULT_STAGE
    optimization_iter = int(args.optimization_iter or _env_int("ACCEL_PIPELINE_OPT_ITER", 0))
    workload = load_workload(workload_name, repo_root, engine=os.environ.get("ACCEL_PIPELINE_ENGINE", ""))
    pipeline_config = apply_workload_overrides(pipeline_config, workload)
    art = query_artifacts_dir(repo_root, query, workload=workload)
    return pipeline_config, workload, workload_name, query, stage, optimization_iter, art


def _session_write_check(pipeline_config, query: int, stage: str):
    """The ownership check of this agent session (it starts with the session, as the orchestrator's does), or None
    where the write policy does not apply."""
    from accel_pipeline.layout import StageWriteCheck

    if pipeline_config.write_policy != "enforce" or not stage or stage.startswith("verus_"):
        return None
    return StageWriteCheck(pipeline_config.repo_root, query=query, stage=stage, pipeline_config=pipeline_config)


def build_mcp_tool_callables(
    pipeline_config,
    workload: dict[str, Any],
    workload_name: str,
    query: int,
    stage: str,
    optimization_iter: int,
    art: Path,
) -> dict[str, Callable[..., str]]:
    from accel_pipeline.gates.run import run_gate
    from accel_pipeline.gates.stamps import current_stamp
    from accel_pipeline.tools.data_tools import DataToolContext, make_data_tools
    from accel_pipeline.tools.gate_tools import (
        ToolContext as GateCtx,
        _compact_gate_payload,
        admission_refusal_payload,
        make_gate_tools,
        make_quick_tools,
    )
    from accel_pipeline.structural_reanalysis import ReanalysisToolContext, make_reanalysis_tools
    from accel_pipeline.tools.shell_tools import ToolContext as ShellCtx, make_shell_tools

    is_aux = workload.get("workload_kind") == "aux_state"
    gate_ctx = GateCtx(pipeline_config, query, workload=workload, stage=stage, workload_name=workload_name,
                       optimization_iter=optimization_iter, writes=_session_write_check(pipeline_config, query, stage))
    request_gate, gate_status = make_gate_tools(gate_ctx)
    run_pipeline_command = make_shell_tools(ShellCtx(pipeline_config, query))

    def _run_named_gate(gate_name: str, env: dict[str, str] | None = None) -> str:
        if refusal := admission_refusal_payload(gate_ctx, gate_name):
            return refusal
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

    def run_profile(scale: Literal["subset", "full"] = "subset") -> str:
        """Hot spots by symbol. Aux: perf samples of the read, write and build paths. scale="subset" (default, about 20 s)
        profiles the test subset, for direction on code paths; "full" profiles the full gate dataset in an instance of its
        own and shows where time goes at scale (state build, big-write maintenance, caches); it takes minutes."""
        from accel_pipeline.optimization.saturation import mark_profile_run

        if is_aux:
            payload = profile_hot_spots(pipeline_config, workload, query, scale)
            if json.loads(payload).get("status") == "pass":
                mark_profile_run(art)
            return payload
        payload = _cached_by_code_stamp(
            "profile", lambda: _run_named_gate("profile", env={"ACCEL_BENCH_WARMUP": "1", "ACCEL_BENCH_ITERS": "3"}))
        mark_profile_run(art)
        return payload

    def run_correctness() -> str:
        """Run correctness / aux_correctness gate (smoke + SF1 differential for aux)."""
        return _run_named_gate("aux_correctness" if is_aux else "correctness")

    def run_multi_connection() -> str:
        """Run aux_multi_connection gate (registered entry points + multi-conn stress). Aux-state only."""
        if not is_aux:
            return json.dumps(
                {"gate": "aux_multi_connection", "status": "skip", "reason": "not an aux_state workload"}, indent=2
            )
        return _run_named_gate("aux_multi_connection")

    def primitive_publish(files: list[str], rationale: str) -> str:
        """Publish staged primitives to the shared accelerators/prims/ library (additions only)."""
        from accel_pipeline.prims_library import publish_tool

        return publish_tool(pipeline_config, query, files, rationale)

    def discover_host_sdk() -> str:
        """Describe this engine's host.sh verbs and how to connect."""
        from accel_pipeline.engine_host import discover_host_sdk_json

        return discover_host_sdk_json(workload, pipeline_config.repo_root, pipeline_config.build_dir)

    def report_blocker(summary: str, evidence: str) -> str:
        """The obstacle is outside your files: write blocker.md and end the turn."""
        from accel_pipeline.session import write_blocker

        write_blocker(art, summary, evidence)
        return "Blocker recorded. Stop now; the orchestrator takes over. Do not continue working."

    def ask_advisor(question: str) -> str:
        """A stronger model reads this conversation and answers once (see the MCP tool's description)."""
        from accel_pipeline import repair as repair_mod

        cfg = pipeline_config.advisor
        if repair_mod.scope(stage) not in repair_mod.ADVISED or cfg.max_consults_per_stage <= 0:
            return "No advice: the advisor is not available in this stage."
        left = cfg.max_consults_per_stage - repair_mod.consults_used(art, stage)
        if left <= 0:
            return f"No advice: advisor limit reached ({cfg.max_consults_per_stage} consults in this stage)."
        advice = repair_mod.ask(art, question, min(cfg.timeout_sec + 30, 560))
        return f"{advice}\n\n[advisor consults left in this stage: {max(left - 1, 0)}]"

    def validate_analysis() -> str:
        """Check analysis.json as the analysis stage will when the turn ends."""
        from accel_pipeline.workload import analysis_artifact_path
        from accel_pipeline.validation import AuxAnalysisValidationError, validate_aux_analysis_file

        try:
            validate_aux_analysis_file(analysis_artifact_path(pipeline_config.repo_root, query, workload=workload),
                                       allow_poor_fitness=bool(workload.get("allow_poor_fitness", False)))
        except AuxAnalysisValidationError as exc:
            return f"invalid: {exc}"
        return "valid"

    def update_analysis(operations: list[dict[str, Any]]) -> str:
        """Apply JSON Patch operations to analysis.json, write it, and validate."""
        from accel_pipeline.json_patch import PatchError, apply
        from accel_pipeline.workload import analysis_artifact_path

        path = analysis_artifact_path(pipeline_config.repo_root, query, workload=workload)
        try:
            patched = apply(json.loads(path.read_text(encoding="utf-8")), operations)
        except (OSError, ValueError, PatchError) as exc:
            return f"not applied: {exc}"
        path.write_text(json.dumps(patched, indent=2) + "\n", encoding="utf-8")
        return f"applied {len(operations)} operation(s); validation: {validate_analysis()}"

    tools = {
        "report_blocker": report_blocker,
        "ask_advisor": ask_advisor,
        "validate_analysis": validate_analysis,
        "update_analysis": update_analysis,
        "request_gate": request_gate,
        "gate_status": gate_status,
        "quick_check": make_quick_tools(gate_ctx),
        "run_profile": run_profile,
        "run_correctness": run_correctness,
        "run_multi_connection": run_multi_connection,
        "request_structural_reanalysis": make_reanalysis_tools(
            ReanalysisToolContext(pipeline_config, query, stage, workload_name, optimization_iter)
        ),
        "run_pipeline_command": run_pipeline_command,
        "primitive_publish": primitive_publish,
        "discover_host_sdk": discover_host_sdk,
        **make_data_tools(DataToolContext(pipeline_config, workload, query, art)),
    }
    for mode in active_modes(stage, workload_name):
        if mode != "verus":  # the verus tools run on their own, outside this context
            tools.update(mode_tools(mode).callables(gate_ctx))
    return tools


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

    def _call(name: str, *a, **kw) -> str:
        try:
            pipeline_config, workload, workload_name, q, stage, optimization_iter, art = _context()
            tools = build_mcp_tool_callables(
                pipeline_config, workload, workload_name, q, stage, optimization_iter, art
            )
            with keep_alive(art), tool_call_limit():  # a long tool call (gate, bench, build) is agent activity, not idleness
                result = tools[name](*a, **kw)
            record_check(art, name, a, result)
            return result + _budget_footer(pipeline_config, stage, q)
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

        annotations = {
            read_only: ToolAnnotations(
                readOnlyHint=read_only, destructiveHint=False, idempotentHint=read_only, openWorldHint=False
            )
            for read_only in (True, False)
        }
    except Exception:  # noqa: BLE001
        annotations = {}

    def tool(*, read_only: bool):
        return mcp.tool(annotations=annotations[read_only]) if annotations else mcp.tool()

    stage = args.stage or os.environ.get("ACCEL_PIPELINE_STAGE") or _DEFAULT_STAGE
    workload_name = args.workload or os.environ.get("ACCEL_PIPELINE_WORKLOAD") or ""
    modes = active_modes(stage, workload_name)
    if modes != ["verus"]:
        register_core(tool, _call)
    for mode in modes:
        module = mode_tools(mode)
        module.register(tool, module.call if mode == "verus" else _call)

    mcp.run(transport="stdio")
    return 0


def register_core(tool, call) -> None:
    @tool(read_only=False)
    def request_gate(gate_name: str, background: bool = False) -> str:
        """MCP TOOL (not a resource — do not list_mcp_resources).

        Run an official orchestrator gate by name. Returns JSON with status,
        notes, and on fail failure_summary. Engine backend: pass engine_build
        then this layer's gate (engine_capture, engine_epochs, engine_scan,
        engine_runtime, engine_host). Prefer run_engine_build / run_layer_gate when listed.
        Keep calling until status=pass. Do not substitute host cmake or docker
        exec of the harness for this tool.
        Aux gates are for acceptance and take minutes; use quick_check in the edit loop.
        background=True starts the gate in its own process and returns at once; read it once with gate_status.
        """
        return call("request_gate", gate_name, background)

    @tool(read_only=True)
    def gate_status(gate_name: str) -> str:
        """State of a background gate (request_gate background=True): running, or its result.
        stale=true: the code changed after it ran, so it is rerun at stage exit. Never poll in a loop."""
        return call("gate_status", gate_name)

    @tool(read_only=True)
    def quick_check(targets: list[str] | None = None, differential: bool = True, sanitizer: str = "",
                    threads: int = 4) -> str:
        """Seconds-level check of this query's accelerator: incremental build, its own tests, the
        differential on the small test subset. Run after every edit. targets: extra build targets of
        your own test binaries (built and run). Correctness only; the subset is never a tuning target.
        sanitizer="thread" or "address": the same check in an instrumented build (its own build dir, incremental)
        with every phase at `threads` threads; use quick_check(sanitizer="thread", threads=N) after touching
        concurrent code. Only reports with a frame in the query's own directory fail."""
        return call("quick_check", targets, differential, sanitizer, threads)

    @tool(read_only=True)
    def run_profile(scale: Literal["subset", "full"] = "subset") -> str:
        """Hot spots by symbol of the read, write and build paths. "subset" (default, about 20 s): direction on code
        paths; "full": the full gate dataset, where time goes at scale (state build, big-write maintenance, caches); minutes."""
        return call("run_profile", scale)

    @tool(read_only=True)
    def run_correctness() -> str:
        """Run correctness / aux_correctness gate (smoke + SF1 differential)."""
        return call("run_correctness")

    @tool(read_only=True)
    def run_multi_connection() -> str:
        """Run aux_multi_connection gate (entry-point registration + multi-conn stress)."""
        return call("run_multi_connection")

    @tool(read_only=False)
    def request_structural_reanalysis(
        bottleneck_type: str,
        evidence: str,
        proposed_direction: str,
        current_candidate_id: str,
        alternate_candidate_ids: list[str] | None = None,
        cost_model_insufficient_reason: str = "",
    ) -> str:
        """Request aux structural re-analysis (escape hatch)."""
        return call(
            "request_structural_reanalysis",
            bottleneck_type,
            evidence,
            proposed_direction,
            current_candidate_id,
            alternate_candidate_ids,
            cost_model_insufficient_reason,
        )

    @tool(read_only=False)
    def report_blocker(summary: str, evidence: str) -> str:
        """Call when the obstacle is outside what you own (a pipeline, gate, harness or backend bug, a
        missing input) instead of investigating it. Writes blocker.md and ends your turn; the
        orchestrator re-runs the failing check once and otherwise stops for an operator."""
        return call("report_blocker", summary, evidence)

    @tool(read_only=False)
    def run_pipeline_command(command: str) -> str:
        """Allowlisted cmake/ctest command."""
        return call("run_pipeline_command", command)

    @tool(read_only=False)
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
        return call("primitive_publish", files, rationale)

    @tool(read_only=True)
    def discover_host_sdk() -> str:
        """Describe this engine's host.sh verbs and how to connect."""
        return call("discover_host_sdk")

    @tool(read_only=False)
    def ask_advisor(question: str) -> str:
        """Ask a stronger model for a second opinion. It reads your whole conversation and the files on disk
        (it cannot edit them), diagnoses the root cause and returns concrete next steps, in a few minutes.
        Use it when stuck on one failure for more than ~10 minutes, or before declaring that part of the
        analysed plan cannot be built. question: what you are stuck on and what you have tried. A few consults
        per stage."""
        return call("ask_advisor", question)

    @tool(read_only=True)
    def validate_analysis() -> str:
        """Check analysis.json exactly as the analysis stage will when your turn ends: "valid" or the first problem."""
        return call("validate_analysis")

    @tool(read_only=False)
    def update_analysis(operations: list[dict[str, Any]]) -> str:
        """Edit analysis.json in place with JSON Patch operations, then validate it as the stage will.

        operations: [{"op": "replace"|"add"|"remove", "path": "/correctness_facts/state_design/state_storage/2/primitive",
        "value": ...}]; paths are JSON Pointers ("/" separated keys and list indexes, "-" appends to a list). Use it
        for revisions: change only what the critique or validation names instead of rewriting the file. Returns
        "applied N operation(s); validation: valid" or the first problem; nothing is written when an operation
        cannot apply."""
        return call("update_analysis", operations)

    @tool(read_only=True)
    def query_data(sql: str, max_rows: int | None = None) -> str:
        """Run ONE read-only statement (SELECT/WITH/EXPLAIN/DESCRIBE/SUMMARIZE) on the iteration dataset: CSV, capped rows,
        time-boxed, logged to data_queries.jsonl. Measure what a design depends on instead of guessing."""
        return call("query_data", sql, max_rows)

    @tool(read_only=True)
    def column_stats(columns: list[str]) -> str:
        """Per column ("table.column"): type, NULL fraction, ~NDV, min, p1, p50, p99, max (sampled above a size threshold)."""
        return call("column_stats", columns)

    @tool(read_only=True)
    def join_fanout(left: str, right: str) -> str:
        """Fan-out of a join between two key columns ("table.column"): keys, rows per key (avg/p99/max), rows with a partner."""
        return call("join_fanout", left, right)

    @tool(read_only=True)
    def rows_qualifying(predicate_sql: str, values: list[str]) -> str:
        """Rows qualifying for a predicate template over one table (e.g. "t.d < $1"), $1 replaced by each value."""
        return call("rows_qualifying", predicate_sql, values)

    @tool(read_only=True)
    def column_spread(a: str, b: str, join_sql: str = "") -> str:
        """Distribution of a - b ("table.column" each; dates in days) over joined rows; join_sql is the linking equality."""
        return call("column_spread", a, b, join_sql)

    @tool(read_only=True)
    def query_shape() -> str:
        """The statistics the query's own SQL implies (columns, fan-outs, parameter extremes, spreads), as tables."""
        return call("query_shape")


if __name__ == "__main__":
    raise SystemExit(main())

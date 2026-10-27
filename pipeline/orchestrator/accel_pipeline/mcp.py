"""The MCP server and every tool it serves.

The agent's tools run in a stdio MCP server codex starts (pipeline/codex/run_mcp_server.sh,
`python -m accel_pipeline.mcp`). The core tools (gates, quick_check, the data tools, the analysis edit tools) are
always registered; the tools of a mode (optimization, engine_backend, pack, verus) only when that mode runs, from
its own package (MODE_TOOLS, imported when the mode runs). Each tool call rebuilds its context from the
environment (build_mcp_tool_callables), runs under keep_alive and the tool-call limit, and answers with the
session's budget footer.

Data tools: every statement runs on the target engine through its host.sh `sql` verb. `query_data` runs the agent's
own read-only statement (in the engine's SQL); the convenience tools (column_stats, join_fanout, rows_qualifying,
column_spread, query_shape) emit portable SQL through data.py, transpiled to the dialect the engine profile names
(`data_sql.dialect`). Tables and columns come from the engine's catalog and the query template, never from this
code. Every statement is time-boxed, every result capped, and every call is logged to the run's artifacts
(data_queries.jsonl) so analysis can cite it and the audit can check it.

Gate tools run and report gates and the quick check; the shell tool is the allowlisted cmake/ctest command.

The analysis agent revises analysis.json with a few targeted JSON Patch operations (`apply`) instead of
regenerating the whole document.
"""
from __future__ import annotations

import argparse
import importlib
import copy
import csv
import fcntl
import io
import json
import os
import re
import signal
import subprocess
import sys
import threading
import time
from dataclasses import dataclass
from pathlib import Path
from types import ModuleType
from typing import Any, Callable, Literal

import sqlglot
from sqlglot import exp
from sqlglot.tokens import Tokenizer, TokenType

from accel_pipeline import data, repair as repair_mod
from accel_pipeline.agent import keep_alive, record_check, budget_footer, write_blocker
from accel_pipeline.config import (
    pipeline_root,
    PipelineConfig,
    read_json,
    load_pipeline_config,
    find_repo_root,
    load_workload,
    apply_workload_overrides,
    query_artifacts_dir,
    analysis_artifact_path,
    sanitize_engine_name,
)
from accel_pipeline.data import (
    build_data_shape_markdown,
    catalog_sql,
    Dataset,
    DataToolContext,
    host_script,
    HostSql,
    instance_env,
    md_table,
    discover_host_sdk_json,
    AuxAnalysisValidationError,
    validate_aux_analysis_file,
)
from accel_pipeline.gates import (
    background_status,
    profile_hot_spots,
    render_quick_result,
    run_gate,
    run_quick_check,
    start_background_gate,
    tool_call_limit,
    current_stamp,
    log_tail_for_agent,
    admission_refusal,
)
from accel_pipeline.layout import (
    StageWriteCheck,
    violation_message,
    shell_command_cross_query_write,
    shell_command_layer_write,
)


MODE_TOOLS = {
    "optimization": "accel_pipeline.optimization.loop",
    "engine_backend": "accel_pipeline.engine_backend",
    "pack": "accel_pipeline.pack",
    "verus": "accel_pipeline.verus.actions",
}


def mode_tools(mode: str) -> ModuleType:
    return importlib.import_module(MODE_TOOLS[mode])


def none_default(value: Any, default: Any) -> Any:
    """`default` when an LLM's tool call passes JSON null for an optional argument."""
    return default if value is None else value



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
        footer = budget_footer(pipeline_config, stage, query)
    except Exception:  # noqa: BLE001
        return ""
    return f"\n\n{footer}" if footer else ""


def _build_context(args: argparse.Namespace):
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
    from accel_pipeline.stages import ReanalysisToolContext, make_reanalysis_tools

    is_aux = workload.get("workload_kind") == "aux_state"
    gate_ctx = ToolContext(pipeline_config, query, workload=workload, stage=stage, workload_name=workload_name,
                       optimization_iter=optimization_iter, writes=_session_write_check(pipeline_config, query, stage))
    request_gate, gate_status = make_gate_tools(gate_ctx)
    run_pipeline_command = make_shell_tools(ShellToolContext(pipeline_config, query))

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
        from accel_pipeline.optimization.memory import mark_profile_run

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
        from accel_pipeline.prims import publish_tool

        return publish_tool(pipeline_config, query, files, rationale)

    def discover_host_sdk() -> str:
        """Describe this engine's host.sh verbs and how to connect."""
        return discover_host_sdk_json(workload, pipeline_config.repo_root, pipeline_config.build_dir)

    def report_blocker(summary: str, evidence: str, owner: str = "") -> str:
        """The obstacle is outside your files: write blocker.md and end the turn. A defect in the engine's workload
        harnesses or its backend is also recorded as a request to that owner."""
        from accel_pipeline.engine_backend import record

        write_blocker(art, summary, evidence)
        if owner in ("harness", "backend") and workload.get("engine_name"):
            record(pipeline_config.repo_root, sanitize_engine_name(str(workload["engine_name"])), {
                "benchmark": str(workload.get("benchmark_name") or ""), "query": query, "scope": owner,
                "helper": summary.strip()[:200], "reason": evidence.strip()})
        return "Blocker recorded. Stop now; the orchestrator takes over. Do not continue working."

    def ask_advisor(question: str) -> str:
        """A stronger model reads this conversation and answers once (see the MCP tool's description)."""
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
        try:
            validate_aux_analysis_file(analysis_artifact_path(pipeline_config.repo_root, query, workload=workload),
                                       allow_poor_fitness=bool(workload.get("allow_poor_fitness", False)))
        except AuxAnalysisValidationError as exc:
            return f"invalid: {exc}"
        return "valid"

    def update_analysis(operations: list[dict[str, Any]]) -> str:
        """Apply JSON Patch operations to analysis.json, write it, and validate."""
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
        module.register(tool, _call)

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
        """Request a bounded structural re-analysis (max 2 per pipeline run).

        Use only when a structural bottleneck is confirmed — e.g. residual O(n) state scan,
        scan-for-scan swap, wrong partition grain, or maintenance semantics that cannot be fixed
        by cost-model tuning or switching among viable alternates already in analysis.json.

        Do NOT use for coefficient tweaks, EWMA tuning, or minor threshold changes.
        """
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
    def report_blocker(summary: str, evidence: str, owner: str = "") -> str:
        """Call when the obstacle is outside what you own (a pipeline, gate, harness or backend bug, a
        missing input) instead of investigating it. Writes blocker.md and ends your turn.

        owner: "harness" when the engine's workload harnesses are wrong (host_bootstrap fixes them before
        your next check), "backend" for the engine backend (recorded for its owner; you adapt meanwhile),
        else empty. The orchestrator re-runs the failing check, then comes back to you to adapt."""
        return call("report_blocker", summary, evidence, owner)

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




LOG_FILE = "data_queries.jsonl"

_LEADING = {"SELECT", "WITH", "EXPLAIN", "DESCRIBE", "DESC", "SUMMARIZE", "SHOW", "VALUES", "FROM"}
_FORBIDDEN = {
    "INSERT", "UPDATE", "DELETE", "MERGE", "UPSERT", "CREATE", "ALTER", "DROP", "TRUNCATE", "ATTACH", "DETACH",
    "COPY", "EXPORT", "IMPORT", "PRAGMA", "CALL", "INSTALL", "LOAD", "SET", "RESET", "GRANT", "REVOKE", "VACUUM",
    "CHECKPOINT", "INTO", "OUTFILE", "DUMPFILE", "LOCK", "COMMENT", "USE", "BEGIN", "COMMIT", "ROLLBACK",
    "EXECUTE", "PREPARE", "DO", "ANALYZE",
}
# Table and scalar functions that reach files, other databases or arbitrary SQL text.
_REACHING_FUNCTIONS = re.compile(
    r"^(read_\w*|glob|\w*file\w*|lo_\w+|dblink\w*|\w+_scan|query|query_table|copy_\w+|sniff_csv)$", re.I
)


def check_read_only(sql: str) -> str | None:
    """Why `sql` is not a single read-only statement, or None when it is."""
    try:
        tokens = Tokenizer().tokenize(sql)
    except Exception as exc:  # noqa: BLE001 — an unlexable statement is refused
        return f"cannot tokenize the statement: {exc}"
    while tokens and tokens[-1].token_type == TokenType.SEMICOLON:
        tokens.pop()
    if not tokens:
        return "empty statement"
    if any(t.token_type == TokenType.SEMICOLON for t in tokens):
        return "exactly one statement is allowed"
    if tokens[0].text.upper() not in _LEADING:
        return f"only SELECT/WITH/EXPLAIN/DESCRIBE/SUMMARIZE/SHOW/VALUES reads are allowed, not {tokens[0].text.upper()}"
    for i, tok in enumerate(tokens):
        if tok.token_type in (TokenType.STRING, TokenType.IDENTIFIER, TokenType.NUMBER):
            continue
        word = tok.text.upper()
        if word in _FORBIDDEN:
            return f"{word} is not a read (quote the identifier if it is a column name)"
        follows_paren = i + 1 < len(tokens) and tokens[i + 1].token_type == TokenType.L_PAREN
        if follows_paren and _REACHING_FUNCTIONS.match(tok.text):
            return f"{tok.text}() reaches outside the dataset tables"
    return None


def _log(art: Path, record: dict[str, Any]) -> None:
    with (art / LOG_FILE).open("a", encoding="utf-8") as f:
        f.write(json.dumps({"ts": time.strftime("%Y-%m-%dT%H:%M:%S"), **record}, default=str) + "\n")


def _csv(header: list[str], rows: list[tuple] | list[list]) -> str:
    out = io.StringIO()
    writer = csv.writer(out, lineterminator="\n")
    writer.writerow(header)
    writer.writerows([["" if v is None else v for v in row] for row in rows])
    return out.getvalue().rstrip("\n")


def _source(ds: Dataset, table: str) -> tuple[str, float]:
    return data.sample_of(table, ds.rows(table), ds.schema[table])


def _quantile_cells(res: tuple) -> list[Any]:
    n, lo, qs, hi = res
    return [f"{n:,}", lo, qs[0], qs[1], qs[2], hi]


def make_data_tools(ctx: DataToolContext) -> dict[str, Callable[..., str]]:
    def logged(tool: str, args: dict[str, Any], run: Callable[[], tuple[str, dict[str, Any]]]) -> str:
        """Run a tool body, log the call (sql, rows, seconds), and return its text; errors come back as text."""
        started = time.monotonic()
        try:
            text, info = run()
            error = None
        except (ValueError, RuntimeError, TimeoutError, OSError, sqlglot.errors.SqlglotError) as exc:
            text, info, error = f"error: {exc}", {}, str(exc)
        seconds = round(time.monotonic() - started, 3)
        _log(ctx.art, {"tool": tool, "args": args, "seconds": seconds, "error": error, **info})
        return text if error else f"{text}\n({seconds:.2f} s)"

    def query_data(sql: str, max_rows: int | None = None) -> str:
        """MCP TOOL. Run ONE read-only statement (SELECT/WITH/EXPLAIN/DESCRIBE/SUMMARIZE) on the iteration dataset.

        Use it to measure what the design depends on (rows qualifying for a predicate, distinct counts,
        value ranges, fan-out, how two columns relate) and to simulate a candidate design's read: count the
        entries it would visit for a binding. The statement runs on the target engine, in its SQL.
        Results are CSV with a header, capped at max_rows (default and maximum are configured); a
        statement is stopped at the time box (default 60 s). Counts and spreads measured here are at
        iteration scale. Every call is logged to data_queries.jsonl.
        """
        limits = ctx.limits
        rows_cap = max(1, min(int(max_rows or limits.default_rows), limits.max_rows))

        def run() -> tuple[str, dict[str, Any]]:
            why = check_read_only(sql)
            if why:
                raise ValueError(f"refused: {why}")
            runner = HostSql(ctx)
            runner.require_up()
            table, more = runner.query(sql, limits.query_seconds, rows_cap)
            n = len(table) - 1
            note = f"; capped at {rows_cap} rows, more exist" if more else ""
            return (f"-- {n} rows{note}\n{_csv(table[0], table[1:])}",
                    {"sql": sql, "rows": n, "capped": more})

        return logged("query_data", {"sql": sql, "max_rows": rows_cap}, run)

    def column_stats(columns: list[str]) -> str:
        """MCP TOOL. Per column ("table.column"): type, NULL fraction, distinct count, min, p1, p50, p99, max.

        One pass per table; tables above the sampling threshold are sampled (the output says so).
        """

        def run() -> tuple[str, dict[str, Any]]:
            ds = Dataset(ctx)
            refs = [ds.column(c) for c in columns]
            rows = []
            for table in dict.fromkeys(t for t, _ in refs):
                cols = [c for t, c in refs if t == table]
                found = data.column_stats(ds.probe, table, cols, ds.schema[table], ds.rows(table))
                for c in cols:
                    st = found.get(c)
                    if st:
                        q = st["q"] or [None] * 3
                        rows.append([f"{table}.{c}", ds.schema[table][c], f"{st['null_frac']:.1%}", st["ndv"],
                                     st["min"], q[0], q[1], q[2], st["max"]])
            lines = md_table(["column", "type", "NULL", "NDV", "min", "p1", "p50", "p99", "max"], rows)
            text = f"{ds.sampling_note([t for t, _ in refs])}\n\n" + "\n".join(lines)
            return text, {"sql": ds.probe.executed, "rows": len(rows)}

        return logged("column_stats", {"columns": columns}, run)

    def join_fanout(left: str, right: str) -> str:
        """MCP TOOL. Fan-out of a join between two key columns ("table.column" each), in both directions.

        Per side: distinct keys present, rows per key (avg, p99, max), and the share of rows that have a partner.
        """

        def run() -> tuple[str, dict[str, Any]]:
            ds = Dataset(ctx)
            (lt, lc), (rt, rc) = ds.column(left), ds.column(right)
            rows = []
            for (t, c), (ot, oc) in (((lt, lc), (rt, rc)), ((rt, rc), (lt, lc))):
                res, match = data.fanout(ds.probe, t, c, ot, oc, ds.rows(t), ds.schema[t])
                if res:
                    rows.append([f"{t}.{c} -> {ot}.{oc}", f"{res[0]:,}", res[1], res[2], res[3],
                                 "-" if match is None else f"{match:.1%}"])
            text = ds.sampling_note([lt, rt]) + "\n\n" + "\n".join(
                md_table(["side (key -> joined to)", "keys", "avg", "p99", "max", "rows with a partner"], rows))
            return text, {"sql": ds.probe.executed, "rows": len(rows)}

        return logged("join_fanout", {"left": left, "right": right}, run)

    def rows_qualifying(predicate_sql: str, values: list[str]) -> str:
        """MCP TOOL. Rows qualifying for a predicate template, with $1 replaced by each value.

        predicate_sql: a condition over one table's columns, e.g. "t.d < $1". values: the
        bindings to try (dates and strings are quoted for you); pick the ends of the parameter's domain
        and a few between. Counts are rescaled when the table is sampled (the output says so).
        """

        def run() -> tuple[str, dict[str, Any]]:
            why = check_read_only(f"SELECT 1 WHERE {predicate_sql}")
            if why:
                raise ValueError(f"refused: {why}")
            if set(re.findall(r"\$(\d+)", predicate_sql)) - {"1"}:
                raise ValueError("only $1 is substituted; write the other parameters as literals")
            if not values:
                raise ValueError("values must not be empty")
            ds = Dataset(ctx)
            table = ds.table_of(predicate_sql)
            total = ds.rows(table)
            src, scale = _source(ds, table)
            rows = []
            for v in values:
                where = re.sub(r"\$1\b", lambda _: data.literal(v), predicate_sql)
                n = ds.probe.count(f"SELECT COUNT(*) FROM {src} AS {data._q(table)} WHERE {where}")
                n = None if n is None else int(n * scale)
                rows.append([v, "-" if n is None else f"{n:,}", "-" if n is None or not total else f"{n / total:.2%}"])
            text = (f"table {table} ({total:,} rows); {ds.sampling_note([table])}\n\n"
                    + "\n".join(md_table(["$1", "rows qualifying", "of table"], rows)))
            return text, {"sql": ds.probe.executed, "rows": len(rows)}

        return logged("rows_qualifying", {"predicate_sql": predicate_sql, "values": values}, run)

    def column_spread(a: str, b: str, join_sql: str = "") -> str:
        """MCP TOOL. Distribution of a - b ("table.column" each; dates in days) over the joined rows.

        join_sql: the equality linking the two tables, e.g. "a.id = b.a_id"
        (empty when both columns are in one table). Reports rows, min, p1, p50, p99, max.
        """

        def run() -> tuple[str, dict[str, Any]]:
            ds = Dataset(ctx)
            ka, kb = ds.column(a), ds.column(b)
            shape = data.QueryShape()
            if ka[0] != kb[0]:
                shape.joins.append(_equality(ds, join_sql, {ka[0], kb[0]}))
            res, _ = data.spread(ds.probe, ka, kb, shape, {t: ds.rows(t) for t in {ka[0], kb[0]}}, ds.schema)
            if not res:
                raise RuntimeError("no result (timed out, or no joined rows)")
            unit = "days" if data.type_family(ds.schema[ka[0]][ka[1]]) == "date" else "units"
            text = (f"{a} - {b} over {'joined ' if ka[0] != kb[0] else ''}rows, in {unit}; "
                    f"{ds.sampling_note([ka[0], kb[0]])}\n\n"
                    + "\n".join(md_table(["rows", "min", "p1", "p50", "p99", "max"], [_quantile_cells(res)])))
            return text, {"sql": ds.probe.executed, "rows": res[0]}

        return logged("column_spread", {"a": a, "b": b, "join_sql": join_sql}, run)

    def query_shape() -> str:
        """MCP TOOL. The statistics the query's own SQL implies, as tables: per-column stats of the columns it
        uses, join-key fan-outs, rows qualifying at the extremes of each parameterised predicate, and spreads
        between the columns its predicates combine. A starting point; follow up with the other tools.
        """

        def run() -> tuple[str, dict[str, Any]]:
            sql = catalog_sql(ctx.workload, ctx.pipeline_config.repo_root, ctx.query)
            if not sql:
                raise RuntimeError("the query template is not available to this run")
            ds = Dataset(ctx)
            seconds = ctx.limits.query_seconds
            text = build_data_shape_markdown(sql, ds.runner, ds.schema, f"engine `{ds.runner.engine}`",
                                             seconds, 3 * seconds)
            return text, {"sql": "(derived from the query template)", "rows": text.count("\n")}

        return logged("query_shape", {}, run)

    return {
        "query_data": query_data,
        "column_stats": column_stats,
        "join_fanout": join_fanout,
        "rows_qualifying": rows_qualifying,
        "column_spread": column_spread,
        "query_shape": query_shape,
    }


def _equality(ds: Dataset, join_sql: str, tables: set[str]) -> tuple[tuple[str, str], tuple[str, str]]:
    """The two (table, column) sides of `join_sql`, an equality between columns of `tables`."""
    if not join_sql.strip():
        raise ValueError("join_sql is required when the columns are in different tables")
    tree = data.parse_template(f"SELECT 1 WHERE {join_sql}", ds.dialect).args["where"].this
    if not (isinstance(tree, exp.EQ) and isinstance(tree.this, exp.Column) and isinstance(tree.expression, exp.Column)):
        raise ValueError("join_sql must be one equality between two columns")
    sides = []
    for col in (tree.this, tree.expression):
        owners = (
            [t for t in tables if t.lower() == col.table.lower()] if col.table
            else [t for t in tables if col.name.lower() in {c.lower() for c in ds.schema[t]}]
        )
        if len(owners) != 1:
            raise ValueError(f"cannot place {col.sql()} in {sorted(tables)}; qualify it as table.column")
        sides.append(ds.column(f"{owners[0]}.{col.name}"))
    if {s[0] for s in sides} != tables:
        raise ValueError("join_sql must link the two columns' tables")
    return sides[0], sides[1]


@dataclass
class ToolContext:
    pipeline_config: PipelineConfig
    query: int
    workload: dict[str, Any] | None = None
    stage: str = ""
    workload_name: str = ""
    optimization_iter: int = 0
    writes: Any = None  # the session's StageWriteCheck: no check runs on a tree with out-of-bounds edits


def _compact_gate_payload(gate_name: str, data: dict) -> dict:
    compact: dict = {"gate": gate_name, "status": data.get("status")}
    for key in (
        "composite_score",
        "composite_workload_speedup",
        "read_only_query_speedup",
        "read_only_cold_start_workload_speedup",
        "exit_code",
        "returncode",
        "reason",
        "notes",
        "reproduce",
        "smoke_exit_code",
        "differential_exit_code",
        "failed_phases",
        "phases",
        "stacks",
    ):
        if key in data and data[key] is not None:
            compact[key] = data[key]
    if waited := (data.get("phase_timings") or {}).get("measurement_lease_wait"):
        compact["measurement_lease"] = f"waited {waited} s for the measurement lease"
    if data.get("status") == "fail":
        compact["failure_summary"] = log_tail_for_agent(data, max_chars=4000)
    for key in ("layer", "cmake_target", "harness", "mode", "engine"):
        if data.get(key):
            compact[key] = data[key]
    return compact


def _compact_background_state(state: dict) -> dict:
    compact = {k: state[k] for k in ("gate", "status", "reason", "note", "elapsed_sec", "seconds", "phase", "progress", "stale") if k in state}
    if state.get("data"):
        compact.update({k: v for k, v in _compact_gate_payload(state["gate"], state["data"]).items() if k != "gate"})
    return compact


def admission_refusal_payload(ctx: ToolContext, gate_name: str) -> str:
    """The refusal (JSON) when `gate_name` may not run yet, "" when it may: the session edited paths its stage may
    not write, or a heavy gate has no passing quick_check on this code."""
    if ctx.writes is not None and (bad := ctx.writes.violations()):
        reason = f"{gate_name} refused: {violation_message(ctx.stage, bad)}"
    else:
        reason = admission_refusal(ctx.pipeline_config, ctx.workload or {}, ctx.query, gate_name)
    return json.dumps({"gate": gate_name, "status": "refused", "reason": reason}, indent=2) if reason else ""


def make_gate_tools(ctx: ToolContext):
    def request_gate(gate_name: str, background: bool = False) -> str:
        """MCP TOOL (not a resource). Run an official orchestrator gate by name.

        Engine-backend layers: ``engine_build`` then this layer's gate
        (``engine_capture``, ``engine_epochs``, …) after each
        increment until ``status`` is ``pass``. Prefer ``run_engine_build`` /
        ``run_layer_gate`` when listed. Docker hosts: this is the in-container
        build+run — do not cmake on the developer machine.
        Aux: ``aux_exit`` (every heavy check in one run, a few minutes) or one of
        ``aux_correctness`` / ``aux_multi_connection``. Gates are for acceptance; for the
        edit-test loop use ``quick_check``.

        ``background=True`` starts the gate in its own process and returns at once;
        keep working and read the result once with ``gate_status``. Never poll it in a loop.
        """
        if refusal := admission_refusal_payload(ctx, gate_name):
            return refusal
        if background:
            state = start_background_gate(
                ctx.pipeline_config, ctx.workload or {}, ctx.query, gate_name, workload_name=ctx.workload_name
            )
            return json.dumps(_compact_background_state(state), indent=2)
        result = run_gate(
            ctx.pipeline_config,
            gate_name,
            ctx.query,
            workload=ctx.workload,
            raise_on_failure=False,
            allow_skip=True,
        )
        return json.dumps(_compact_gate_payload(result.name, result.data), indent=2)

    def gate_status(gate_name: str) -> str:
        """MCP TOOL. State of the background run of a gate: running, or its result.

        ``stale: true`` means the code changed after it ran: its result does not cover your latest
        edit, and the orchestrator reruns the gate at stage exit. Call it once when you need the
        answer, not in a loop.
        """
        state = background_status(ctx.pipeline_config, ctx.workload or {}, ctx.query, gate_name)
        return json.dumps(_compact_background_state(state), indent=2)

    return request_gate, gate_status


def make_quick_tools(ctx: ToolContext):
    def quick_check(targets: list[str] | None = None, differential: bool = True, sanitizer: str = "",
                    threads: int = 4) -> str:
        """MCP TOOL. 10-30 s check of this query's accelerator; run it after every edit.

        Incremental build (a no-op when nothing changed), the query's own tests, your `targets` (extra build
        targets of your own test binaries, built and run: put the brute-force oracle test here) and, unless
        differential=False, the frozen suite at N and 1 threads on the join-closed test subset.
        A pass reports: the state build's seconds at N and 1 thread with the speedup, projected to the full
        dataset; read latency against the engine's own SQL; each write kind's maintenance action and projected
        full-scale cost; perf counters of the read and the build. A serial or over-budget build and a write that
        rebuilds the state fail the check, each with the next step. The subset is never a tuning target.
        sanitizer="thread" (or "address") runs the differential on the tiny database in an instrumented build
        (incremental, about 30 s) with every phase at `threads` threads; run it after touching concurrent code.
        Reports with a frame in the query's own directory fail; engine and substrate reports are counted as
        infrastructure.
        """
        if refusal := admission_refusal_payload(ctx, "quick_check"):
            return refusal
        result = run_quick_check(ctx.pipeline_config, ctx.workload or {}, ctx.query, targets=targets,
                                 differential=differential, sanitizer=sanitizer, threads=threads)
        return render_quick_result(result)

    return quick_check


_CTEST_DEFAULT_TIMEOUT_SECONDS = 240

# Read-only repo navigation; always permitted (does not need YAML allowlist entries).
_BUILTIN_DIAGNOSTIC_COMMANDS = [
    re.compile(r"^pwd$"),
    re.compile(r"^pwd && ls(?: [^|;&\n]*)?$"),
    re.compile(r"^ls$"),
    re.compile(r"^ls [^|;&\n]*$"),
    re.compile(r"^test -[ef] .+$"),
]


def _command_allowed(cmd: str, patterns: list[re.Pattern[str]]) -> bool:
    stripped = cmd.strip()
    if any(p.match(stripped) for p in _BUILTIN_DIAGNOSTIC_COMMANDS):
        return True
    return any(p.match(stripped) for p in patterns)


@dataclass
class ShellToolContext:
    pipeline_config: PipelineConfig
    query: int


def _build_dir_arg(pipeline_config: PipelineConfig) -> str:
    try:
        return str(pipeline_config.build_dir.relative_to(pipeline_config.repo_root))
    except ValueError:
        return str(pipeline_config.build_dir)


def _normalize_ctest_cmd(cmd: str, pipeline_config: PipelineConfig) -> str:
    stripped = cmd.strip()
    if not re.search(r"\bctest\b", stripped):
        return stripped
    if "--test-dir" in stripped or re.match(r"^cd\s+", stripped):
        return stripped
    build_dir = _build_dir_arg(pipeline_config)
    return re.sub(r"\bctest\b", f"ctest --test-dir {build_dir}", stripped, count=1)


def rewrite_command_build_dir(cmd: str, pipeline_config: PipelineConfig) -> str:
    """Rewrite bare ``build-release`` tokens to this worker's build dir name."""
    build_name = pipeline_config.build_dir.name
    if build_name == "build-release" or "build-release" not in cmd:
        return cmd
    # Avoid double-rewriting already-scoped paths.
    if build_name in cmd:
        return cmd
    # Rewrite absolute .../build-release (exact dir) and bare tokens.
    root = str(pipeline_config.build_dir.parent).rstrip("/")
    abs_shared = f"{root}/build-release"
    abs_worker = str(pipeline_config.build_dir)
    out = cmd
    out = re.sub(rf"(?<![\w.-]){re.escape(abs_shared)}(?![\w.-])", abs_worker, out)
    out = re.sub(r"(?<![\w.-])build-release(?![\w.-])", build_name, out)
    return out


def reject_shared_build_dir_cmd(cmd: str, pipeline_config: PipelineConfig) -> str | None:
    """If worker isolation is active, forbid targeting the shared ``build-release`` tree."""
    build_name = pipeline_config.build_dir.name
    if build_name == "build-release":
        return None
    if re.search(r"(?<![\w.-])build-release(?![\w.-])", cmd) and build_name not in cmd:
        return (
            f"shared build-release is forbidden under pack isolation; "
            f"use {build_name} (or omit — run_pipeline_command rewrites it)"
        )
    return None


def make_shell_tools(ctx: ShellToolContext):
    allowed = [re.compile(p) for p in ctx.pipeline_config.allowed_commands]

    def run_command(cmd: str, cwd: str | None = "", timeout: int | None = 600) -> str:
        """Run an allowlisted build/test command."""
        from accel_pipeline.pack import (
            SharedFileLease,
            command_touches_shared_build,
            sticky_holder,
            worker_id_from_env,
        )

        cwd = str(none_default(cwd, ""))
        timeout = int(none_default(timeout, 600))
        raw_cmd = rewrite_command_build_dir(cmd.strip(), ctx.pipeline_config)
        effective_cmd = _normalize_ctest_cmd(raw_cmd, ctx.pipeline_config)
        shared_block = reject_shared_build_dir_cmd(effective_cmd, ctx.pipeline_config)
        if shared_block:
            raise PermissionError(shared_block)
        if not _command_allowed(raw_cmd, allowed) and not _command_allowed(effective_cmd, allowed):
            raise PermissionError(f"command not allowlisted: {cmd}")

        cross = shell_command_cross_query_write(effective_cmd, query=int(ctx.query))
        if cross:
            raise PermissionError(cross)
        layer = shell_command_layer_write(effective_cmd)
        if layer:
            raise PermissionError(layer)

        holder = ctx.pipeline_config.worker_id or worker_id_from_env(f"q{ctx.query}")
        lease_obj = None
        acquired_here = False
        # If we already hold the sticky lease (shared edits pending), keep it
        # through cmake so shared TUs rebuild under the same critical section.
        # Otherwise acquire briefly for cmake configure/build.
        if command_touches_shared_build(effective_cmd):
            from accel_pipeline.pack import sticky_lease_obj

            if sticky_holder() == holder and sticky_lease_obj() is not None:
                sticky_lease_obj().ensure_held(holder, reason="cmake under sticky lease")
            else:
                lease_obj = SharedFileLease(ctx.pipeline_config.repo_root)
                lease_obj.acquire(
                    holder, reason="cmake shared build", timeout_sec=ctx.pipeline_config.concurrency.shared_lease_wait_sec
                )
                acquired_here = True

        workdir = ctx.pipeline_config.repo_root
        if cwd:
            workdir = ctx.pipeline_config.repo_root / cwd
        effective_timeout = timeout
        if re.search(r"\bctest\b", effective_cmd) and timeout >= 600:
            effective_timeout = _CTEST_DEFAULT_TIMEOUT_SECONDS
        proc = subprocess.Popen(
            effective_cmd,
            shell=True,
            cwd=workdir,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            start_new_session=True,
        )
        try:
            try:
                stdout, stderr = proc.communicate(timeout=effective_timeout)
                exit_code = proc.returncode
            except subprocess.TimeoutExpired:
                try:
                    os.killpg(proc.pid, signal.SIGKILL)
                except ProcessLookupError:
                    proc.kill()
                stdout, stderr = proc.communicate()
                out = (stdout or "") + (stderr or "")
                return (
                    f"exit=124\n"
                    f"command timed out after {effective_timeout}s (process group killed)\n"
                    f"{out[-2000:]}"
                )
            out = (stdout or "") + (stderr or "")
            return f"exit={exit_code}\n{out[-2000:]}"
        finally:
            if acquired_here and lease_obj is not None:
                lease_obj.release(holder)

    return run_command


class PatchError(ValueError):
    pass


def _tokens(path: str) -> list[str]:
    if path == "":
        return []
    if not path.startswith("/"):
        raise PatchError(f"path {path!r} must start with '/'")
    return [t.replace("~1", "/").replace("~0", "~") for t in path[1:].split("/")]


def _index(container: list, token: str, *, append: bool) -> int:
    if append and token == "-":
        return len(container)
    if not token.isdigit():
        raise PatchError(f"list index {token!r} is not a number")
    i = int(token)
    if i > len(container) or (i == len(container) and not append):
        raise PatchError(f"list index {i} out of range (length {len(container)})")
    return i


def _parent(doc: Any, tokens: list[str], path: str) -> Any:
    node = doc
    for token in tokens[:-1]:
        if isinstance(node, dict) and token in node:
            node = node[token]
        elif isinstance(node, list):
            node = node[_index(node, token, append=False)]
        else:
            raise PatchError(f"{path}: no {token!r}")
    return node


def apply(doc: Any, ops: list[dict[str, Any]]) -> Any:
    """A patched copy of `doc`; raises PatchError naming the first operation that cannot apply."""
    out = copy.deepcopy(doc)
    for n, op in enumerate(ops):
        kind, path = op.get("op"), op.get("path")
        if kind not in ("add", "replace", "remove") or not isinstance(path, str):
            raise PatchError(f"operation {n}: need op add|replace|remove and a string path, got {op}")
        tokens = _tokens(path)
        if not tokens:
            if kind == "remove":
                raise PatchError(f"operation {n}: cannot remove the whole document")
            out = copy.deepcopy(op["value"])
            continue
        parent, last = _parent(out, tokens, path), tokens[-1]
        if isinstance(parent, dict):
            if kind != "add" and last not in parent:
                raise PatchError(f"operation {n}: {path} does not exist")
            if kind == "remove":
                del parent[last]
            else:
                parent[last] = copy.deepcopy(op["value"])
        elif isinstance(parent, list):
            i = _index(parent, last, append=kind == "add")
            if kind == "add":
                parent.insert(i, copy.deepcopy(op["value"]))
            elif kind == "replace":
                parent[i] = copy.deepcopy(op["value"])
            else:
                del parent[i]
        else:
            raise PatchError(f"operation {n}: {path} has no container to edit")
    return out


if __name__ == "__main__":
    raise SystemExit(main())

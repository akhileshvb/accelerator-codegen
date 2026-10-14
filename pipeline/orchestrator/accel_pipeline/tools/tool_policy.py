from __future__ import annotations

import json
import os
import subprocess
from typing import Any, Callable

from accel_pipeline.config import PipelineConfig, load_workload
from accel_pipeline.paths import query_artifacts_dir
from accel_pipeline.tools.ab_tools import AbToolContext, make_ab_tools
from accel_pipeline.tools.gate_tools import ToolContext as GateCtx
from accel_pipeline.tools.gate_tools import make_gate_tools
from accel_pipeline.tools.repo_tools import ToolContext as RepoCtx
from accel_pipeline.tools.repo_tools import make_repo_tools
from accel_pipeline.tools.reanalysis_tools import ToolContext as ReanalysisCtx
from accel_pipeline.tools.reanalysis_tools import make_reanalysis_tools
from accel_pipeline.tools.shell_tools import ToolContext as ShellCtx
from accel_pipeline.tools.shell_tools import make_shell_tools
from accel_pipeline.tools.search_tools import make_search_tools
from accel_pipeline.tools.tool_args import none_default as _none_default

STAGE_TOOL_NAMES: dict[str, list[str]] = {
    "host_bootstrap": [
        "read_file",
        "grep_context",
        "write_file",
        "apply_patch",
        "insert_lines",
        "search_repo",
        "list_dir",
        "glob_files",
        "file_info",
        "run_command",
        "request_gate",
        "discover_host_sdk",
        "acquire_shared_lease",
        "release_shared_lease",
    ],
    "analysis": [
        "read_file",
        "grep_context",
        "write_file",
        "search_repo",
        "list_dir",
        "glob_files",
        "file_info",
        "discover_host_sdk",
    ],
    "adversarial_tests": [
        "read_file",
        "grep_context",
        "write_file",
        "search_repo",
        "list_dir",
        "glob_files",
        "file_info",
        "discover_host_sdk",
    ],
    "implementation": [
        "read_file",
        "read_reference_files",
        "grep_context",
        "write_file",
        "apply_patch",
        "insert_lines",
        "delete_file",
        "search_repo",
        "list_dir",
        "glob_files",
        "file_info",
        "run_command",
        "request_gate",
        "request_structural_reanalysis",
        "acquire_shared_lease",
        "release_shared_lease",
        "discover_host_sdk",
        "primitive_publish",
    ],
    "optimization_unified": [
        "rip_grep",
        "ast_grep",
        "grep_context",
        "glob_files",
        "write_file",
        "apply_patch",
        "insert_lines",
        "delete_file",
        "file_info",
        "request_structural_reanalysis",
        "primitive_publish",
    ],
    "optimization_unified_hypothesis": [
        "file_info",
        "request_structural_reanalysis",
    ],
    "optimization_unified_patch": [
        "write_file",
        "apply_patch",
        "insert_lines",
        "delete_file",
        "file_info",
    ],
    "optimization_fix": [
        "read_file",
        "grep_context",
        "rip_grep",
        "ast_grep",
        "glob_files",
        "write_file",
        "apply_patch",
        "insert_lines",
        "file_info",
        "run_command",
        "primitive_publish",
    ],
    "optimization_worker": [
        "rip_grep",
        "ast_grep",
        "grep_context",
        "glob_files",
        "read_file",
        "write_file",
        "apply_patch",
        "insert_lines",
        "delete_file",
        "file_info",
        "run_command",
        "begin_ab_trial",
        "run_quick_benchmark",
        "end_ab_trial",
        "request_structural_reanalysis",
        "record_optimization_trap",
        "acquire_shared_lease",
        "release_shared_lease",
        "primitive_publish",
    ],
    "crash_debug": [
        "read_file",
        "grep_context",
        "apply_patch",
        "insert_lines",
        "write_file",
        "search_repo",
        "list_dir",
        "glob_files",
        "file_info",
        "run_command",
    ],
    "audit": ["read_file", "grep_context", "search_repo", "list_dir", "glob_files", "file_info", "request_gate"],
    "architecture_audit": [
        "read_file",
        "grep_context",
        "search_repo",
        "list_dir",
        "glob_files",
        "file_info",
        "write_file",
        "discover_host_sdk",
    ],
    "layer_critique": [
        "read_file",
        "grep_context",
        "search_repo",
        "list_dir",
        "glob_files",
        "file_info",
        "write_file",
        "discover_host_sdk",
    ],
    "validation": [
        "read_file",
        "grep_context",
        "write_file",
        "apply_patch",
        "insert_lines",
        "search_repo",
        "list_dir",
        "glob_files",
        "file_info",
        "run_command",
        "request_gate",
        "discover_host_sdk",
    ],
}


def stage_tool_names(stage: str) -> list[str]:
    if stage.startswith("impl_"):
        names = list(STAGE_TOOL_NAMES.get("implementation") or [])
        for extra in ("run_engine_build", "run_layer_gate"):
            if extra not in names:
                names.append(extra)
        return names
    if stage == "layer_critique" or stage.startswith("critique_"):
        return list(STAGE_TOOL_NAMES.get("layer_critique") or [])
    return list(STAGE_TOOL_NAMES.get(stage) or [])

TOOL_USAGE_INSTRUCTIONS = """\
## Tooling

Use ONLY the pipeline tools for this stage. Do not use built-in shell/edit tools (unless running under Codex — see codex_tooling.md).

- read_file: default first 100 lines; use offset/limit for more. numbered=true for patches. optimization reads: default limit=600 for full implementation reads.
- grep_context: search one file with ±context — **prefer over read_file** on kernels/packages (after full read in optimization patch).
- write_file: rewrite a whole editable file in one call when you have read it fully and the change is coordinated across many lines.
- apply_patch: old_string must match exactly once — prefer **large hunks** (whole struct/function) over many one-line edits when you have just read the file.
- insert_lines: for adding new blocks; avoid chaining many tiny insert_lines when write_file is safer.
- run_command: allowlisted cmake/ctest. After adding new CMake targets (aux test/bench), run `cmake -S . -B <worker_build_dir>` before `cmake --build ...` (compound forms are allowlisted). Under pack isolation, bare `build-release` is rewritten to this worker's build dir — never target the shared tree.
- Benchmarks and accept/reject are orchestrator-owned — exploratory A/B tools are non-binding.
- optimization_unified: hypothesize/implement then `HYPOTHESIS_PATCH_COMPLETE` for build → smoke → benchmark.
- request_gate / run_correctness / run_multi_connection: under Codex MCP, implementation **must** call `run_correctness` (and for aux-state also `run_multi_connection`, or `request_gate` with those gate names) and **keep iterating until they pass** before finishing. Orchestrator still re-runs post-stage gates. For **host_bootstrap**, call `request_gate` with `host_bootstrap` until pass (postgres: Docker ninja, never host `cmake --build build-release`). If that post-stage gate still fails, the orchestrator re-invokes this stage with `notes`/`log_tail`. For **engine_backend** implementation layers, call `request_gate` with `engine_build` then that layer's gate (`engine_store`, `engine_aux_backend`, `engine_txn`, `engine_mutations`, `engine_scan`, `engine_runtime`, `engine_visibility`, `engine_host`) until pass. For **engine_backend** validation, also loop **`engine_adversarial`** after authoring unseen tests. After all layers, a separate **architecture_audit** agent judges hard constraints and may re-invoke implementation layers with feedback.
"""

IMPLEMENTATION_TOOL_USAGE = """\
## Implementation workflow

- **First tool call:** `read_reference_files` on the paths in **Implementation bootstrap** (one call, up to 8 files).
- **Then write:** create `files_to_create` with `write_file` — do not spend more than 2 turns reading before the first write.
- `read_reference_files`: batch-read reference aux modules; prefer this over many `read_file` calls.
- `read_file`: default 250 lines in implementation; use `limit=500` for a full reference module if needed.
- `grep_context`: **file path only** (not directories). Use `search_repo` / `glob_files` to find paths.
- Do not `list_dir` repo root or re-glob for q14/q1 after bootstrap paths are listed.
- After creating new `test_tpch_q*_aux.cpp` / `benchmark_q*_aux.cpp` / `calibrate_q*_aux_cost_model.cpp`, run `cmake -S . -B <build_dir>` before building (use this worker's build dir; `build-release` is rewritten automatically). Targets are usually auto-discovered; edit `CMakeLists.txt` only if configure still omits them.
- **Register everywhere:** this query must be wired into **every** shared registration / allowlist / SQL-helper surface that neighboring aux queries use (catalog entry, shared-query participation lists, DuckDB aux table-function registration, cross-accel / multi-conn helpers, bench arg variants, CMake targets). Discover those sites by searching how a peer query is registered — do not invent a partial wiring. Missing any surface → `unsupported shared query` or `Table Function … does not exist`.
- **Shared-tree concurrency:** edits to shared registration / catalog / CMake / helper wiring auto-take a **shared-file lease**. Call `release_shared_lease` after you finish those edits + rebuild. Query-local `q{{N}}/aux/**` does not need the lease.
- **Before finishing (hard):** build smoke + differential targets, then loop MCP `run_correctness` **and** `run_multi_connection` (or `request_gate` with `aux_correctness` / `aux_multi_connection`) until both report `status=pass`. Do not end on compile-only or smoke-only success.
"""

OPTIMIZATION_PATCH_TOOL_USAGE = """\
## Optimization patch workflow

- **Read once, write big:** after `read_file` (limit=600) on each editable file, prefer **one `write_file` per file** or **one large `apply_patch` per function/struct** — not dozens of one-line patches.
- Use `write_file` when the hypothesis changes multiple regions in the same file (new helpers + struct fields + updated methods).
- Use `apply_patch` for a single localized change; include enough surrounding context that `old_string` is unique (whole function body, whole struct, etc.).
- Do **not** re-read the same file repeatedly between tiny patches; batch edits, then `HYPOTHESIS_PATCH_COMPLETE`.
- Do not call `run_command` for builds — the orchestrator builds after handoff.
"""


OPTIMIZATION_HYPOTHESIS_TOOL_USAGE = """\
## Hypothesis phase (no edits)

- Review store diagnostics, retrieved precedents, and bootstrap sources in the user message.
- Output structured hypothesis JSON citing metric names and measured values.
- End with `HYPOTHESIS_COMPLETE` — no write_file/apply_patch in this phase.
"""

OPTIMIZATION_UNIFIED_TOOL_USAGE = """\
## Unified optimization workflow (fast experiment loop — like a Cursor agent)

- **Editable sources are bootstrapped** in the user message (full contents, line numbers). Do not try to `read_file` — it is not available.
- **Explore references with search:** use `rip_grep` (fast text+context) or `ast_grep` (structural C++ patterns) on other `accelerators/tpch/q*/*/aux/**` modules listed in the reference index.
- **Ambitious redesigns welcome:** new aux-state layouts, partition strategies, encode/decode versions, **incremental maintenance modes** (ApplyDelta / local rebuilds — not RebuildAll-per-write) — coordinate header + module + finalize + tests + cost model. Optimize **both** read hit latency and 10% write-mix health every attempt.
- **Hypothesize → implement → hand off** in few turns (target ≤6 tool calls before `HYPOTHESIS_PATCH_COMPLETE`).
- **Write big:** `write_file` per file for multi-region changes; large `apply_patch` hunks otherwise.
- Orchestrator runs **build → quick correctness (ctest) → 0%+sampled-write quick bench → accept/reject** after handoff."""

OPTIMIZATION_WORKER_TOOL_USAGE = """\
## Optimization worker (Codex-like dev loop)

- **Inspect** with `read_file`, `rip_grep`, `ast_grep` on editable files and reference aux modules.
- **Patch** with `write_file` / `apply_patch` — write big, coordinated hunks.
- **Build and test locally** with `run_command` (cmake build, targeted ctest) — fix compiler/smoke failures before signaling ready.
- **Shared-tree concurrency:** shared registration / catalog / CMake / helper edits auto-take a shared-file lease; call `release_shared_lease` after those edits + rebuild. Query-local sources do not need it.
- **Exploratory benchmark:** `begin_ab_trial` → `run_quick_benchmark` → `end_ab_trial` (results are non-binding).
- When you have a plausible candidate, output hypothesis JSON and **`CANDIDATE_READY`**.
- Orchestrator runs authoritative gates after `CANDIDATE_READY`; do not skip local build/test.
"""


ENGINE_BACKEND_IMPL_TOOL_USAGE = """\
## Engine backend layer workflow

- **First:** read `pipeline/prompts/engine_backend/layer_contracts.md` for this
  layer, then the substrate headers listed in the prompt
  (`aux_types.hpp`, `partitioned_state_store.hpp`, `aux_registry.hpp` as needed).
- Fill `pipeline/templates/engine_backend/` stubs. Adapter types stay in
  `aux_state::engine::{engine}` — never `core::`.
- If `backend_analysis.json` contradicts `layer_contracts.md` or
  `acceptance_checklist.md`, follow the contracts (including CAS publish,
  token-filtered Read, key-from-publish, `AttachServices` in the aux ctor).
- Mid-loop MCP tools on server `accel_pipeline` (tools, not resources — do
  **not** `list_mcp_resources`): `run_engine_build()`, `run_layer_gate()`,
  or `request_gate` with `engine_build` then this layer's gate. A green
  exit that skipped contract asserts is not done — add the asserts.
  Do **not** treat host cmake / `docker exec` of the harness as the pass bar.
- Prior layers: reuse. If a real bug/design flaw in a prior layer blocks this
  contract, make the smallest patch, keep that prior harness honest, and
  `request_gate` the prior layer's gate before continuing. No rewrites for
  style; do not start later layers.
"""


LAYER_CRITIQUE_TOOL_USAGE = """\
## Layer critique workflow

- Read-only on adapter sources. Write only `pipeline/artifacts/**`
  (`layer_critique_{layer}.json` + `.md`).
- **Last action must be writing those files** (Codex: `cat > …json <<'EOF'`).
  Stopping without the JSON fails the stage. Do not explore later layers.
- Under Codex: do **not** use native edit/shell on adapter / harness / CMake.
- Read this layer's `layer_contracts.md` section, checklist rules, adapter,
  harness, and gate JSON. Fail on contract holes a green gate missed.
- If this layer is blocked by a prior-layer bug/design flaw, fail and tell
  impl to make the smallest prior-layer patch (keep that prior harness green).
- Do not start later layers or weaken the harness.
"""


HOST_BOOTSTRAP_CRITIQUE_TOOL_USAGE = """\
## Host bootstrap critique workflow

- Read-only on adapter / harness / CMake. Write only `pipeline/artifacts/**`
  (`host_bootstrap_critique.json` + `.md` under the engine host_bootstrap dir).
- **Last action must be writing those files** (Codex: `cat > …json <<'EOF'`).
  Stopping without the JSON fails the stage.
- Under Codex: do **not** use native edit/shell on adapter / harness / CMake.
- Fail on a stub differential, wrong CMake target name, or a second adapter
  tree. Do not fail Phase 2 multi-conn/cross-accel still missing.
"""


def tool_usage_instructions(stage: str) -> str:
    text = TOOL_USAGE_INSTRUCTIONS
    if stage == "critique_host_bootstrap":
        text = f"{text.rstrip()}\n\n{HOST_BOOTSTRAP_CRITIQUE_TOOL_USAGE}"
    elif stage == "layer_critique" or stage.startswith("critique_"):
        text = f"{text.rstrip()}\n\n{LAYER_CRITIQUE_TOOL_USAGE}"
    elif stage.startswith("impl_"):
        text = f"{text.rstrip()}\n\n{ENGINE_BACKEND_IMPL_TOOL_USAGE}"
    elif stage == "implementation":
        text = f"{text.rstrip()}\n\n{IMPLEMENTATION_TOOL_USAGE}"
    elif stage in ("optimization_unified", "optimization", "optimization_fix"):
        text = f"{text.rstrip()}\n\n{OPTIMIZATION_PATCH_TOOL_USAGE}"
        if stage == "optimization_unified":
            text = f"{text.rstrip()}\n\n{OPTIMIZATION_UNIFIED_TOOL_USAGE}"
        if stage == "optimization_worker":
            text = f"{text.rstrip()}\n\n{OPTIMIZATION_WORKER_TOOL_USAGE}"
        if stage == "optimization_unified_hypothesis":
            text = f"{TOOL_USAGE_INSTRUCTIONS.rstrip()}\n\n{OPTIMIZATION_HYPOTHESIS_TOOL_USAGE}"
        if stage == "optimization_unified_patch":
            text = f"{TOOL_USAGE_INSTRUCTIONS.rstrip()}\n\n{OPTIMIZATION_PATCH_TOOL_USAGE}"
        if stage == "optimization_fix":
            text = (
                f"{text.rstrip()}\n\n"
                "## Fix-round workflow\n\n"
                "- Same hypothesis as the failed patch — fix build/correctness only.\n"
                "- `read_file` / `grep_context` allowed — use logs to locate the bug.\n"
                "- `run_command`: rebuild and re-run the reproduce command until it passes.\n"
            )
    elif stage == "optimization_worker":
        text = f"{text.rstrip()}\n\n{OPTIMIZATION_PATCH_TOOL_USAGE}\n\n{OPTIMIZATION_WORKER_TOOL_USAGE}"
    elif stage == "optimization_unified_hypothesis":
        text = f"{TOOL_USAGE_INSTRUCTIONS.rstrip()}\n\n{OPTIMIZATION_HYPOTHESIS_TOOL_USAGE}"
    elif stage == "optimization_unified_patch":
        text = f"{TOOL_USAGE_INSTRUCTIONS.rstrip()}\n\n{OPTIMIZATION_PATCH_TOOL_USAGE}"
    return text


def make_search_repo(pipeline_config: PipelineConfig):
    def search_repo(
        pattern: str,
        glob: str | None = "*",
        path: str | None = ".",
        max_matches: int | None = 12,
    ) -> str:
        """Search repo with ripgrep."""
        glob = str(_none_default(glob, "*"))
        path = str(_none_default(path, "."))
        max_matches = int(_none_default(max_matches, 12))
        root = pipeline_config.repo_root / path
        proc = subprocess.run(
            [
                "rg",
                "-n",
                "--no-follow",
                pattern,
                str(root),
                "-g",
                glob,
                "--max-count",
                str(max_matches),
            ],
            capture_output=True,
            text=True,
            check=False,
        )
        output = proc.stdout or proc.stderr or "(no matches)"
        if proc.returncode not in (0, 1):
            return f"rg failed (exit={proc.returncode})\n{output[-8000:]}"
        return output

    return search_repo


def build_tool_registry(
    pipeline_config: PipelineConfig,
    query: int,
    *,
    stage: str = "",
    workload_name: str = "",
    optimization_iter: int = 0,
) -> dict[str, Callable[..., Any]]:
    repo_ctx = RepoCtx(pipeline_config, query, stage=stage)
    shell_ctx = ShellCtx(pipeline_config, query)
    gate_ctx = GateCtx(pipeline_config, query, stage=stage)
    if workload_name:
        try:
            gate_ctx = GateCtx(
                pipeline_config,
                query,
                workload=load_workload(workload_name, pipeline_config.repo_root),
                stage=stage,
            )
        except FileNotFoundError:
            pass
    reanalysis_ctx = ReanalysisCtx(
        pipeline_config, query, stage, workload_name, optimization_iter
    )

    (
        read_file,
        write_file,
        apply_patch,
        insert_lines,
        delete_file,
        list_dir,
        glob_files,
        file_info,
        grep_context,
        read_reference_files,
    ) = make_repo_tools(repo_ctx)
    run_command = make_shell_tools(shell_ctx)
    request_gate, run_engine_build, run_layer_gate = make_gate_tools(gate_ctx)
    request_structural_reanalysis = make_reanalysis_tools(reanalysis_ctx)
    search_repo = make_search_repo(pipeline_config)
    rip_grep, ast_grep = make_search_tools(pipeline_config)

    begin_ab_trial = run_quick_benchmark = end_ab_trial = lambda *args, **kwargs: "ERROR: A/B tools unavailable"
    if stage in ("optimization", "optimization_worker") and workload_name:
        from accel_pipeline.aux_write_freqs import attempt_from_env

        workload = load_workload(workload_name, pipeline_config.repo_root)
        ab_ctx = AbToolContext(
            pipeline_config=pipeline_config,
            query=query,
            workload=workload,
            optimization_iter=optimization_iter,
            attempt=attempt_from_env(),
        )
        begin_ab_trial, run_quick_benchmark, end_ab_trial = make_ab_tools(ab_ctx)

    def record_optimization_trap(trap: str, when: str = "", evidence: str = "") -> str:
        from accel_pipeline.negative_memory import append_negative_memory
        import json

        wl = {}
        if workload_name:
            try:
                wl = load_workload(workload_name, pipeline_config.repo_root)
            except FileNotFoundError:
                wl = {}
        entry = append_negative_memory(
            pipeline_config.repo_root,
            trap=trap,
            when=when,
            evidence=evidence,
            query=query,
            workload_kind=str(wl.get("workload_kind") or "") or None,
            source="agent",
        )
        if entry is None:
            return json.dumps({"recorded": False, "reason": "duplicate_or_too_short"})
        return json.dumps({"recorded": True, "entry": entry})

    def acquire_shared_lease(reason: str = "", timeout_sec: float | None = 600.0) -> str:
        """Hold the shared-file lease before editing catalog/CMake/shared registration files."""
        import json

        from accel_pipeline.shared_file_lease import (
            ensure_sticky_lease_for_write,
            worker_id_from_env,
        )

        timeout_sec = float(_none_default(timeout_sec, 600.0))
        holder = pipeline_config.worker_id or worker_id_from_env(f"q{query}")
        info = ensure_sticky_lease_for_write(
            pipeline_config.repo_root,
            holder=holder,
            path=None,
            timeout_sec=timeout_sec,
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

    def primitive_publish(files: list[str], rationale: str) -> str:
        """Publish staged primitives to the shared accelerators/prims/ library (additions only)."""
        from accel_pipeline.prims_library import publish_tool

        return publish_tool(pipeline_config, query, files, rationale)

    def release_shared_lease() -> str:
        """Release the sticky shared-file lease after shared edits + rebuild."""
        import json

        from accel_pipeline.shared_file_lease import release_sticky_lease, worker_id_from_env

        holder = pipeline_config.worker_id or worker_id_from_env(f"q{query}")
        ok = release_sticky_lease(pipeline_config.repo_root, holder=holder)
        return json.dumps({"released": ok, "holder": holder})

    def discover_host_sdk() -> str:
        """Locate this engine's host from workload host_runtime (not engine_name)."""
        from accel_pipeline.host_runtime import discover_host_sdk_json

        return discover_host_sdk_json(workload)

    return {
        "read_file": read_file,
        "grep_context": grep_context,
        "read_reference_files": read_reference_files,
        "write_file": write_file,
        "apply_patch": apply_patch,
        "insert_lines": insert_lines,
        "delete_file": delete_file,
        "list_dir": list_dir,
        "glob_files": glob_files,
        "file_info": file_info,
        "run_command": run_command,
        "begin_ab_trial": begin_ab_trial,
        "run_quick_benchmark": run_quick_benchmark,
        "end_ab_trial": end_ab_trial,
        "request_gate": request_gate,
        "run_engine_build": run_engine_build,
        "run_layer_gate": run_layer_gate,
        "request_structural_reanalysis": request_structural_reanalysis,
        "record_optimization_trap": record_optimization_trap,
        "acquire_shared_lease": acquire_shared_lease,
        "release_shared_lease": release_shared_lease,
        "primitive_publish": primitive_publish,
        "search_repo": search_repo,
        "rip_grep": rip_grep,
        "ast_grep": ast_grep,
        "discover_host_sdk": discover_host_sdk,
    }


def build_tools(
    pipeline_config: PipelineConfig,
    query: int,
    stage: str,
    *,
    workload_name: str = "",
    optimization_iter: int = 0,
) -> list[Callable[..., Any]]:
    registry = build_tool_registry(
        pipeline_config,
        query,
        stage=stage,
        workload_name=workload_name,
        optimization_iter=optimization_iter,
    )
    names = stage_tool_names(stage)
    return [registry[n] for n in names if n in registry]


def build_stage_tool_registry(
    pipeline_config: PipelineConfig,
    query: int,
    stage: str,
    *,
    workload_name: str = "",
    optimization_iter: int = 0,
) -> dict[str, Callable[..., Any]]:
    registry = build_tool_registry(
        pipeline_config,
        query,
        stage=stage,
        workload_name=workload_name,
        optimization_iter=optimization_iter,
    )
    names = stage_tool_names(stage)
    return {name: registry[name] for name in names if name in registry}

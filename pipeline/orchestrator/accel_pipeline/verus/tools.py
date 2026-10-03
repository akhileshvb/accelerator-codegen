"""MCP tools of the Verus pipeline (served by accel_pipeline.mcp_server).

They run the same actions the stages' gates run, on the build tree, using
the run context the orchestrator wrote (``ACCEL_VERUS_CONTEXT``).  Each agent
step sees only its stage's tools (``STAGE_TOOLS``; Codex ``enabled_tools``).
"""

from __future__ import annotations

import json
import os
from pathlib import Path
from typing import Any, Callable

from accel_pipeline.verus import actions
from accel_pipeline.verus.context import VerusContext

COMMON = ["verus_sql", "playbook_append"]
STAGE_TOOLS: dict[str, list[str]] = {
    "verus_spec_audit": ["verus_sql"],
    "verus_analysis": ["verus_check_inputs", *COMMON],
    "verus_critic": ["verus_check_inputs", "verus_sql"],
    "verus_obligations_audit": ["verus_sql", "verus_check_inputs"],
    "verus_tests": ["verus_validate_tests", "verus_sql"],
    "verus_glue": ["verus_glue_check", "verus_sql"],
    "verus_glue_audit": ["verus_sql"],
    "verus_kernel": ["verus_verify", "verus_test", "prooflib_publish", "backend_addition_record", *COMMON],
    "verus_kernel_fix": ["verus_verify", "verus_test", "prooflib_publish", *COMMON],
    "verus_opt_worker": ["verus_verify", "verus_test", "verus_bench", "verus_profile", "verus_check_inputs",
                         "prooflib_publish", "backend_addition_record", *COMMON],
    "verus_audit": ["verus_sql"],
}


def stage_tools(stage: str) -> list[str] | None:
    return STAGE_TOOLS.get(stage)


def _ctx() -> VerusContext:
    path = os.environ.get("ACCEL_VERUS_CONTEXT")
    if not path:
        raise RuntimeError("ACCEL_VERUS_CONTEXT is not set (the Verus pipeline writes it before each agent step)")
    return VerusContext.load(Path(path))


def _json(d: Any) -> str:
    return json.dumps(d, indent=2)[:60000]


def verus_check_inputs() -> str:
    """Check inputs/q.inputs with sql2verus: accepted (with the obligations it yields) or the refusal."""
    return _json(actions.check_inputs(_ctx()))


def verus_sql(query: str, max_rows: int = 50) -> str:
    """Run a read-only SQL query on the test database (explore the data; DuckDB SQL)."""
    return _json(actions.sql(_ctx(), query, max_rows))


def verus_validate_tests() -> str:
    """Check tests/ under the schema's NOT NULL constraints: setup.sql runs, every params.sql tuple runs the query,
    and the txns apply in sequence (committed, as the tests run them)."""
    return _json(actions.validate_tests(_ctx()))


def verus_glue_check() -> str:
    """Build the accelerator with glue/ and check what it delivers against the declared inputs (record mode)."""
    ctx = _ctx()
    b = actions.build_runner(ctx)
    if not b["ok"]:
        return _json(b)
    return _json(actions.glue_contract(ctx))


def verus_verify() -> str:
    """Verify the kernel crate (crate/src) with Verus; errors on failure, plus the trusted-base check."""
    ctx = _ctx()
    res = actions.verify(ctx)
    res["trusted_base"] = actions.trusted_base(ctx)
    return _json(res)


def verus_test(snapshots: bool = False, proofs: bool = True) -> str:
    """Build the accelerator and run the frozen tests on the Verus store (differential; snapshots if asked).

    proofs=false: compile the kernel without checking its proofs first (code first: test the
    executable kernel before proving it). The kernel's gates always check the proofs.
    """
    ctx = _ctx()
    b = actions.build_runner(ctx, proofs=proofs)
    if not b["ok"]:
        return _json(b)
    return _json(actions.correctness(ctx, snapshots_too=snapshots))


def verus_bench(write_mixes: str = "0.01,0.05") -> str:
    """Build and benchmark on the SF1 database (Verus store): read speedup per parameter tuple, write mixes."""
    ctx = _ctx()
    b = actions.build_runner(ctx)
    if not b["ok"]:
        return _json(b)
    mixes = [float(x) for x in write_mixes.split(",") if x.strip()]
    r = actions.bench(ctx, mixes, tag="agent_bench")
    r["score"] = actions.score(r, ctx)
    return _json(r)


def verus_profile() -> str:
    """Steady-state perf profiles on SF1 (build excluded), one per axis: `reads` and `maintenance` (writes); hotspots by symbol and area (kernel, glue, store, engine)."""
    ctx = _ctx()
    b = actions.build_runner(ctx)
    if not b["ok"]:
        return _json(b)
    return _json(actions.profile(ctx))


def prooflib_publish(note: str) -> str:
    """Publish prooflib/src/lib.rs (additions only); every kernel using the library is re-verified first."""
    return _json(actions.prooflib_publish(_ctx(), note))


def playbook_append(section: str, title: str, body: str, evidence: str) -> str:
    """Add an evidence-backed lesson to the playbook (section: performance | proofs)."""
    return _json(actions.playbook_append(_ctx(), section, title, body, evidence))


def backend_addition_record(engine: str, helper: str, reason: str) -> str:
    """Record an engine-level helper the accelerator needs from the runtime backend (generic, not per query)."""
    return _json(actions.backend_addition_record(_ctx(), engine, helper, reason))


TOOLS: dict[str, Callable[..., str]] = {
    f.__name__: f
    for f in (verus_check_inputs, verus_sql, verus_validate_tests, verus_glue_check, verus_verify, verus_test,
              verus_bench,
              verus_profile, prooflib_publish, playbook_append, backend_addition_record)
}

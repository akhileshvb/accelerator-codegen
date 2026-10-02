"""The agents' workspace: an allowlisted clean room (see context.py).

Only these are copied in (read-only unless noted):

    query/        query.sql (with $n parameters), schema.sql, workload.json
    spec/         gen_spec.rs (frozen A)
    frozen/       q.inputs, <crate>_verus.h (the C ABI), gen_obligations.rs, kernel_stub.rs (frozen B)
    inputs/       q.inputs                       (analysis writes; frozen at B)
    analysis/     analysis.json, notes           (analysis writes)
    tests/        params.sql, txns/, setup.sql, bench_txns/   (tests stage writes; frozen)
    glue/         glue.cpp: the host glue (glue stage writes; frozen)
    crate/src/    lib.rs, kernel.rs, ... (writable), gen_*.rs (frozen copies)
    prooflib/     src/lib.rs                     (writable; published with prooflib_publish)
    ref/          verus_common, verus_substrate sources (the verified libraries);
                  kit/ (the host kit's API: verus_kit.hpp, aux_types.hpp)
    playbook/     performance.md, proofs.md      (append with playbook_append)
    contracts/    the rules (inputs language, glue, trusted base, scale principles, proof method)

Nothing from the C++ accelerators or other Verus kernels is copied.
"""

from __future__ import annotations

import json
import shutil
from pathlib import Path
from typing import Any

from accel_pipeline.verus.context import VerusContext

README = """# Verus accelerator workspace: {accel_id}

You are building a Verus-verified auxiliary-state accelerator for one SQL query,
from the query alone.  Work only in this directory.  The pipeline builds and
tests in its own tree through the MCP tools (accel_pipeline server, `verus_*`).

| path | what | yours? |
|---|---|---|
| query/query.sql | the query ($1, $2, ... are parameters) | read |
| query/schema.sql | the schema | read |
| query/workload.json | parameters, engine, scale notes | read |
| spec/gen_spec.rs | the spec generated from the SQL (frozen, trusted) | read |
| inputs/q.inputs | the kernel's declared inputs (scans, rescans, deltas) | analysis |
| analysis/ | analysis.json and notes | analysis |
| frozen/ | after inputs are frozen: q.inputs, the C ABI header, gen_obligations.rs, kernel stub | read |
| tests/ | params.sql, txns/*.sql, setup.sql, bench_txns/*.sql | tests stage |
| glue/ | glue.cpp: the host glue (C++ against ref/kit and the C ABI header) | glue stage |
| crate/src/ | the kernel crate: gen_*.rs frozen; kernel.rs and your modules | kernel |
| prooflib/src/lib.rs | the shared proof library (add lemmas; publish them) | append |
| ref/ | verus_common and verus_substrate (the verified libraries you build on); kit/ (host kit API) | read |
| playbook/ | what earlier runs learned (performance, proofs) | read; append via tool |
| contracts/ | the rules: inputs language, glue, trusted base, scale principles, how to prove a kernel | read |

Accelerator id: `{accel_id}` (table function `accel_{accel_id}`); crate `{crate}`.
"""


def _copy(src: Path, dst: Path) -> None:
    if not src.exists():
        return
    if src.is_dir():
        if dst.exists():
            shutil.rmtree(dst)
        shutil.copytree(src, dst, ignore=shutil.ignore_patterns("target", "*.lock"))
    else:
        dst.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(src, dst)


def contracts_dir(ctx: VerusContext) -> Path:
    return Path(ctx.pipeline_root) / "prompts" / "verus" / "contracts"


def create(ctx: VerusContext, workload: dict[str, Any]) -> Path:
    """Create (or refresh the read-only parts of) the workspace."""
    ws = ctx.ws
    ws.mkdir(parents=True, exist_ok=True)
    for d in ("query", "spec", "frozen", "inputs", "analysis", "tests/txns", "crate/src", "glue", "playbook", "ref"):
        (ws / d).mkdir(parents=True, exist_ok=True)
    _copy(Path(ctx.query_sql), ws / "query" / "query.sql")
    _copy(Path(ctx.schema_sql), ws / "query" / "schema.sql")
    info = {
        "benchmark": ctx.benchmark,
        "query": ctx.query,
        "engine": "duckdb",
        "parameters": workload.get("parameters") or [],
        "output_schema": workload.get("output_schema") or [],
        "dependent_tables": [t.get("name") for t in workload.get("dependent_tables") or [] if isinstance(t, dict)],
        "scale": "iteration at SF1; production up to SF3000 (see contracts/scale_principles.md)",
    }
    (ws / "query" / "workload.json").write_text(json.dumps(info, indent=2), encoding="utf-8")
    root = ctx.root / "src" / "verus"
    _copy(root / "common" / "src", ws / "ref" / "verus_common")
    _copy(root / "substrate" / "src", ws / "ref" / "verus_substrate")
    inc = ctx.root / "include" / "accelerator_codegen"
    _copy(inc / "verus" / "verus_kit.hpp", ws / "ref" / "kit" / "verus_kit.hpp")
    _copy(inc / "stateful" / "core" / "aux_types.hpp", ws / "ref" / "kit" / "aux_types.hpp")
    # the proof library: the agents' editable copy starts as the published one
    if not (ws / "prooflib" / "src" / "lib.rs").is_file():
        _copy(root / "prooflib" / "src" / "lib.rs", ws / "prooflib" / "src" / "lib.rs")
    pb = Path(ctx.pipeline_root) / "verus" / "playbook"
    for name in ("performance.md", "proofs.md"):
        src = pb / name
        dst = ws / "playbook" / name
        if src.is_file():
            _copy(src, dst)
        elif not dst.exists():
            dst.write_text(f"# Playbook: {name[:-3]}\n\n(empty: nothing learned yet)\n", encoding="utf-8")
    _copy(contracts_dir(ctx), ws / "contracts")
    names = {"accel_id": ctx.accel_id, "crate": ctx.crate, "ffi_prefix": ctx.ffi_prefix,
             "benchmark": ctx.benchmark, "query": str(ctx.query)}
    for doc in (ws / "contracts").glob("*.md"):
        text = doc.read_text(encoding="utf-8")
        for k, v in names.items():
            text = text.replace("{{" + k + "}}", v)
        doc.write_text(text, encoding="utf-8")
    (ws / "README.md").write_text(README.format(accel_id=ctx.accel_id, crate=ctx.crate), encoding="utf-8")
    return ws


def refresh_prooflib(ctx: VerusContext) -> None:
    """Take in lemmas other runs published (keeps local unpublished additions)."""
    tree = (ctx.root / "src" / "verus" / "prooflib" / "src" / "lib.rs").read_text(encoding="utf-8")
    local_path = ctx.ws / "prooflib" / "src" / "lib.rs"
    local = local_path.read_text(encoding="utf-8") if local_path.is_file() else ""
    if not local or all(l in local.splitlines() for l in tree.splitlines()):
        if not local:
            local_path.parent.mkdir(parents=True, exist_ok=True)
            local_path.write_text(tree, encoding="utf-8")
        return
    # another run published first: keep theirs, then our unpublished lines
    extra = [l for l in local.splitlines() if l not in tree.splitlines()]
    body = tree.rstrip()
    closing = "} // verus!"
    if body.endswith(closing) and extra:
        body = body[: -len(closing)].rstrip() + "\n" + "\n".join(extra) + "\n\n" + closing
    local_path.write_text(body + "\n", encoding="utf-8")


def snapshot_kernel(ctx: VerusContext, name: str) -> Path:
    """The kernel, its inputs and its glue (a structural move changes all three)."""
    d = Path(ctx.run_dir) / "snapshots" / name
    _copy(ctx.ws / "crate" / "src", d / "crate_src")
    _copy(ctx.ws / "inputs", d / "inputs")
    _copy(ctx.ws / "glue", d / "glue")
    return d


def restore_kernel(ctx: VerusContext, name: str) -> None:
    d = Path(ctx.run_dir) / "snapshots" / name
    _copy(d / "crate_src", ctx.ws / "crate" / "src")
    _copy(d / "inputs", ctx.ws / "inputs")
    if (d / "glue").is_dir():
        _copy(d / "glue", ctx.ws / "glue")

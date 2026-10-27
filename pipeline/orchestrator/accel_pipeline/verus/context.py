"""Run context of the Verus accelerator pipeline (``--mode verus``).

One query's run has three places:

* the **build tree** (``pipeline_config.repo_root``, a pack worker's worktree
  or the primary checkout): sql2verus writes the frozen generated files there,
  everything the run produces lives in the query's own directory
  (``accelerators/<bench>/q<N>/verus_kit/<crate>/``: host glue, C header, and
  the kernel crate in ``crate/``), and every gate runs there;
* the **workspace** (``pipeline/verus_runs/<workload>/workspace``): the only
  directory agents work in, an allowlisted clean room.  Only these are copied
  in (read-only unless noted):

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

  Nothing from the C++ accelerators or other Verus kernels is copied in;
* the **artifacts** (``pipeline/artifacts/<bench>/q<N>/verus``): state,
  rendered prompts, gate results, reports.

``VerusContext`` is written to ``context.json`` so the MCP tools (run by Codex
in another process) see the same settings.

Every substitution parameter of a query is a runtime parameter of its kit, so
every benchmark, profile and correctness run draws fresh values from the
parameter domain (``eval/compare/gen_tpch_params.py``, TPC-H spec 2.4 / qgen
ranges; its ``verus`` field is the kit's argument list in $n order): a kernel
specialized to fixed parameter values gains nothing.  A *draw* is one seed and
the parameter tuples it gives, written to the run's artifacts only when a
measurement starts, so the agent cannot read the values ahead of it.  Paired
comparisons take one draw and hand it to both sides.
"""

from __future__ import annotations

import json
import os
import shutil
import subprocess
import sys
import time
from dataclasses import asdict, dataclass, field
from pathlib import Path
from typing import Any

import yaml

from accel_pipeline.config import query_artifacts_dir, with_facts
from accel_pipeline.data import fresh_seed, n_params, split_tuple
from accel_pipeline.layout import PROOFLIB, query_dir, workload_query_sql, workload_schema_sql


@dataclass
class VerusContext:
    query: int
    workload_name: str
    benchmark: str
    repo_root: str  # build tree
    pipeline_root: str
    build_dir: str
    lib_dir: str
    crate: str
    accel_id: str
    ffi_prefix: str
    query_sql: str
    schema_sql: str
    test_db: str
    bench_db: str
    run_dir: str
    workspace: str
    artifacts: str
    settings: dict[str, Any] = field(default_factory=dict)

    # ---- paths ----
    @property
    def root(self) -> Path:
        return Path(self.repo_root)

    @property
    def ws(self) -> Path:
        return Path(self.workspace)

    @property
    def art(self) -> Path:
        return Path(self.artifacts)

    @property
    def crate_dir(self) -> Path:
        return self.kit_dir / "crate"

    @property
    def kit_dir(self) -> Path:
        return self.root / query_dir(self.benchmark, self.query) / "verus_kit" / self.crate

    @property
    def header_name(self) -> str:
        return f"{self.crate}_verus.h"

    @property
    def runner(self) -> Path:
        return Path(self.build_dir) / "verus_kit_runner"

    def setting(self, key: str, default: Any = None) -> Any:
        cur: Any = self.settings
        for part in key.split("."):
            if not isinstance(cur, dict) or part not in cur:
                return default
            cur = cur[part]
        return cur

    def save(self) -> Path:
        self.art.mkdir(parents=True, exist_ok=True)
        path = self.art / "context.json"
        path.write_text(json.dumps(asdict(self), indent=2), encoding="utf-8")
        return path

    @classmethod
    def load(cls, path: Path) -> "VerusContext":
        return cls(**json.loads(Path(path).read_text(encoding="utf-8")))


def _deep_merge(base: dict[str, Any], over: dict[str, Any]) -> dict[str, Any]:
    out = dict(base)
    for k, v in (over or {}).items():
        out[k] = _deep_merge(out[k], v) if isinstance(v, dict) and isinstance(out.get(k), dict) else v
    return out


def load_verus_settings(pipeline_root: Path, workload: dict[str, Any]) -> dict[str, Any]:
    """config/verus.yaml defaults, overridden by the workload's ``verus:`` section."""
    path = pipeline_root / "config" / "verus.yaml"
    base = yaml.safe_load(path.read_text(encoding="utf-8")) if path.is_file() else {}
    return _deep_merge(base or {}, workload.get("verus") or {})


def build_context(pipeline_config: Any, workload: dict[str, Any], workload_name: str, query: int) -> VerusContext:
    root = Path(pipeline_config.repo_root)
    primary = Path(os.environ.get("ACCEL_PIPELINE_PRIMARY_REPO") or root)
    settings = load_verus_settings(Path(pipeline_config.pipeline_root), workload)
    bench = str(workload.get("benchmark_name") or "tpch")
    crate = str(settings.get("crate") or f"{bench}_q{query}_vgen")
    accel_id = str(settings.get("accelerator_id") or crate)
    prefix = str(settings.get("ffi_prefix") or crate.replace("_", ""))
    # The query SQL and schema are workload inputs: workloads/<bench>/q<N>.sql, workloads/<bench>/schema.sql.
    sql_rel = settings.get("query_sql") or workload_query_sql(bench, query)
    schema_rel = settings.get("schema_sql") or workload_schema_sql(bench)
    datasets = dict(getattr(pipeline_config, "datasets", {}) or {})
    test_db = settings.get("test_db") or str(datasets.get(f"{bench}_test") or "")
    bench_db = settings.get("bench_db") or str(datasets.get(f"{bench}_sf1") or "")
    build_dir = settings.get("build_dir") or str(pipeline_config.build_dir)
    lib_dir = settings.get("lib_dir") or str(root / "build-verus")
    # the Verus binary (kept in settings: the MCP server loads the context)
    settings["verus_bin"] = str(settings.get("verus_bin") or os.environ.get("VERUS_BIN")
                                or root / "tools" / "verus" / "verus-x86-linux" / "verus")
    run_dir = Path(pipeline_config.pipeline_root) / "verus_runs" / workload_name
    art = query_artifacts_dir(root, query, workload=workload) / f"verus_{workload_name}"

    def resolve(p: str) -> str:
        path = Path(p)
        if path.is_absolute():
            return str(path)
        return str((primary / path) if (primary / path).exists() else (root / path))

    return VerusContext(
        query=query,
        workload_name=workload_name,
        benchmark=bench,
        repo_root=str(root),
        pipeline_root=str(pipeline_config.pipeline_root),
        build_dir=str(Path(resolve(build_dir))),
        lib_dir=str(Path(resolve(lib_dir))),
        crate=crate,
        accel_id=accel_id,
        ffi_prefix=prefix,
        query_sql=resolve(str(sql_rel)),
        schema_sql=resolve(str(schema_rel)),
        test_db=test_db,
        bench_db=bench_db,
        run_dir=str(run_dir),
        workspace=str(run_dir / "workspace"),
        artifacts=str(art),
        settings=settings,
    )


# C++-pipeline workload fields the Verus mode never uses (they would give
# agents context from C++ accelerators).
IGNORED_WORKLOAD_FIELDS = (
    "optimization_guidance",
    "cost_model_config",
    "agent_boundary",
)


# ───────────────────────────── workspace ─────────────────────────────


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


def prooflib_lib(ctx: VerusContext) -> Path:
    """The published proof library (accelerators/prooflib/, changed only by prooflib_publish)."""
    return ctx.root / PROOFLIB / "src" / "lib.rs"


def contracts_dir(ctx: VerusContext) -> Path:
    return Path(ctx.pipeline_root) / "prompts" / "verus" / "contracts"


def create_workspace(ctx: VerusContext, workload: dict[str, Any]) -> Path:
    """Create (or refresh the read-only parts of) the workspace."""
    ws = ctx.ws
    ws.mkdir(parents=True, exist_ok=True)
    for d in ("query", "spec", "frozen", "inputs", "analysis", "tests/txns", "crate/src", "glue", "playbook", "ref"):
        (ws / d).mkdir(parents=True, exist_ok=True)
    _copy(Path(ctx.query_sql), ws / "query" / "query.sql")
    _copy(Path(ctx.schema_sql), ws / "query" / "schema.sql")
    workload = with_facts(ctx.root, workload, ctx.query)
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
    sub = ctx.root / "substrate"
    _copy(sub / "verus" / "common" / "src", ws / "ref" / "verus_common")
    _copy(sub / "verus" / "store" / "src", ws / "ref" / "verus_substrate")  # crate verus_substrate
    _copy(sub / "verus" / "kit" / "verus_kit.hpp", ws / "ref" / "kit" / "verus_kit.hpp")
    _copy(sub / "aux" / "core" / "aux_types.hpp", ws / "ref" / "kit" / "aux_types.hpp")
    # the proof library: the agents' editable copy starts as the published one
    if not (ws / "prooflib" / "src" / "lib.rs").is_file():
        _copy(prooflib_lib(ctx), ws / "prooflib" / "src" / "lib.rs")
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
    tree = prooflib_lib(ctx).read_text(encoding="utf-8")
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


def _files(d: Path) -> dict[str, bytes]:
    if not d.is_dir():
        return {}
    return {str(p.relative_to(d)): p.read_bytes() for p in sorted(d.rglob("*"))
            if p.is_file() and "target" not in p.relative_to(d).parts and p.suffix != ".lock"}


def kernel_differs(ctx: VerusContext, name: str) -> bool:
    """Whether the workspace's kernel, inputs or glue differ from snapshot `name`."""
    d = Path(ctx.run_dir) / "snapshots" / name
    pairs = [(ctx.ws / "crate" / "src", d / "crate_src"), (ctx.ws / "inputs", d / "inputs")]
    if (d / "glue").is_dir():
        pairs.append((ctx.ws / "glue", d / "glue"))
    return any(_files(a) != _files(b) for a, b in pairs)


def restore_kernel(ctx: VerusContext, name: str) -> None:
    d = Path(ctx.run_dir) / "snapshots" / name
    _copy(d / "crate_src", ctx.ws / "crate" / "src")
    _copy(d / "inputs", ctx.ws / "inputs")
    if (d / "glue").is_dir():
        _copy(d / "glue", ctx.ws / "glue")


# ───────────────────────────── random parameters ─────────────────────────────


GENERATOR = "eval/compare/gen_tpch_params.py"


@dataclass
class Draw:
    tag: str
    seed: int
    sets: list[str]  # SQL literal tuples, one per parameter set, in $n order
    path: Path  # the tuples, one per line (what the runner reads)
    record: Path  # JSON copy in the run's artifacts
    problems: list[str] = field(default_factory=list)


def _generate(ctx: VerusContext, sets: int, seed: int) -> list[str]:
    """The domain's tuples for this query from the shared generator (run as a script, seeded)."""
    script = Path(str(ctx.setting("params.generator") or ctx.root / GENERATOR))
    if ctx.benchmark != "tpch" or not script.is_file():
        return []
    proc = subprocess.run([sys.executable, str(script), str(sets), str(seed)], capture_output=True, text=True,
                          timeout=120, check=False)
    if proc.returncode != 0:
        raise RuntimeError(f"parameter generator failed: {proc.stderr[-500:]}")
    q = json.loads(proc.stdout)["queries"].get(str(ctx.query)) or {}
    return [s["verus"] for s in q.get("sets") or [] if s.get("verus")]


def new_draw(ctx: VerusContext, tag: str, *, sets: int | None = None, seed: int | None = None) -> Draw | None:
    """A fresh random draw of parameter tuples from the query's domain (None: the query has no
    parameters, or the benchmark has no generator -- the frozen tuples are used).

    `seed` is for replaying a recorded draw; a run never sets it (setting `params.seed` pins
    it, for tests only)."""
    want = n_params(Path(ctx.query_sql).read_text(encoding="utf-8")) if ctx.query_sql and Path(ctx.query_sql).is_file() else 0
    if want == 0:
        return None
    if seed is None:
        seed = ctx.setting("params.seed")
    seed = int(seed) if seed is not None else fresh_seed()
    count = int(sets if sets is not None else ctx.setting("params.sets", 5))
    tuples = _generate(ctx, count, seed)
    problems = []
    good = []
    for t in tuples:
        if len(split_tuple(t)) == want:
            good.append(t)
        else:
            problems.append(f"{t!r}: {len(split_tuple(t))} values, the query takes {want}")
    if not good:
        raise RuntimeError(f"no parameter domain for {ctx.benchmark} q{ctx.query} ({want} parameters): "
                           + ("; ".join(problems[:2]) or "the generator gives none")
                           + ". Every parameter must be drawn from its domain (accel_pipeline/verus/context.py)")
    stamp = f"{tag}_{seed}"
    path = Path(ctx.run_dir) / "param_draws" / f"{stamp}.params"
    record = ctx.art / "param_draws" / f"{stamp}.json"
    for p in (path, record):
        p.parent.mkdir(parents=True, exist_ok=True)
    path.write_text("\n".join(good) + "\n", encoding="utf-8")
    record.write_text(json.dumps({"tag": tag, "seed": seed, "time": time.strftime("%Y-%m-%dT%H:%M:%S"),
                                  "query": ctx.query, "sets": good, "problems": problems}, indent=2),
                      encoding="utf-8")
    return Draw(tag=tag, seed=seed, sets=good, path=path, record=record, problems=problems)


def draw_env(draw: Draw | None, mode: str) -> dict[str, str]:
    """Environment for verus_kit_runner: `replace` (bench, profile, snapshots: these tuples are the
    parameter sets) or `extra` (differential: these tuples are checked besides the frozen ones)."""
    if draw is None:
        return {}
    return {"VERUS_KIT_PARAMS_FILE" if mode == "replace" else "VERUS_KIT_EXTRA_PARAMS_FILE": str(draw.path)}


def draw_summary(draw: Draw | None) -> dict[str, Any]:
    """What a result records about its draw."""
    return {} if draw is None else {"param_seed": draw.seed, "param_sets": draw.sets, "param_record": str(draw.record)}


def check_query(ctx: VerusContext) -> str:
    """'' if every substitution parameter of the query is a $n parameter; else what is wrong (preflight).
    A substitution parameter left as a literal (a LIKE pattern, a factor) would let a kernel specialize to
    one value: the query then takes fewer $n than its domain has.  Literals the benchmark itself fixes
    (TPC-H Q16's '%Customer%Complaints%') are not substitution parameters and are fine."""
    if not ctx.query_sql or not Path(ctx.query_sql).is_file():
        return ""
    sql = Path(ctx.query_sql).read_text(encoding="utf-8")
    if ctx.benchmark != "tpch":
        return ""
    want = n_params(sql)
    gen = _generate(ctx, 1, 1)
    if want and not gen:
        return (f"no parameter domain for tpch q{ctx.query} in {GENERATOR} (its `verus` argument list is empty): "
                "every substitution parameter must be drawn from its domain")
    if len(split_tuple(gen[0])) != want:
        return (f"{ctx.query_sql} takes {want} $n parameters but the TPC-H domain of q{ctx.query} has "
                f"{len(split_tuple(gen[0]))} ({gen[0]}): a substitution parameter is a literal (or an extra one is declared)")
    return ""

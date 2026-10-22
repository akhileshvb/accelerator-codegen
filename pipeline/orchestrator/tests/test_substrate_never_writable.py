"""substrate/** is never writable: no stage, publish tool or generator may write under it.

Fails if any stage's write rules (for every workload, every stage), any agent
tool (repo writes, shell), any publish tool (primitive_publish, prooflib_publish,
playbook / backend-addition records) or any generator (scaffold stubs, the
Verus kit and sql2verus outputs, generator scripts) can write under substrate/.
"""

from __future__ import annotations

import fnmatch
import re
from pathlib import Path

import pytest

from accel_pipeline import layout
from accel_pipeline import prims_library as pl
from accel_pipeline.config import (
    PipelineConfig,
    apply_workload_overrides,
    load_aux_state_defaults,
    load_engine_backend_defaults,
    load_pipeline_config,
    load_workload,
    resolve_workload_paths,
    workload_supported_engines,
)
from accel_pipeline.write_ownership import shell_command_layer_write, write_allowed

REPO = Path(__file__).resolve().parents[3]

# Existing substrate files plus new files a writer could try to create.
SUBSTRATE_SAMPLES = sorted(
    {str(p.relative_to(REPO)) for p in (REPO / "substrate").rglob("*") if p.is_file()}
)[:15] + [
    "substrate/new_file.hpp",
    "substrate/aux/core/new_helper.hpp",
    "substrate/aux/state/new_state.cpp",
    "substrate/verus/store/src/new.rs",
    "substrate/verus/sql2verus/src/new.rs",
    "substrate/verus/kit/verus_kit.hpp",
]

EXTRA_STAGES = (
    "", "implementation", "analysis", "optimization", "optimization_worker", "optimization_unified",
    "optimization_unified_patch", "optimization_fix", "crash_debug", "host_bootstrap", "audit",
    "adversarial_tests", "engine_backend", "impl_capture", "impl_runtime", "impl_scan", "verus_kernel",
    "optimization_unified_hypothesis", "architecture_audit", "layer_critique", "validation",
)
STAGES = sorted(EXTRA_STAGES)


def _workload_names() -> list[str]:
    return sorted(p.stem for p in (REPO / "pipeline/config/workloads").glob("*.yaml") if "template" not in p.stem)


def _loaded_workloads():
    pc = load_pipeline_config(repo_root=REPO)
    out = []
    for name in _workload_names():
        try:
            engine = "" if name.startswith("engine_") else workload_supported_engines(name, REPO)[0]
            w = load_workload(name, REPO, engine=engine)
            merged = apply_workload_overrides(pc, w)
        except Exception:  # noqa: BLE001 - stale/partial workloads are other tests' concern
            continue
        out.append((name, w, merged))
    return out


WORKLOADS = _loaded_workloads()


def test_samples_and_workloads_present():
    assert any(not s.endswith(("new_file.hpp", "new_helper.hpp")) for s in SUBSTRATE_SAMPLES)
    assert len(WORKLOADS) >= 10, "expected the workload configs to load"
    kinds = {w.get("workload_kind") for _, w, _ in WORKLOADS}
    assert "engine_backend" in kinds and "aux_state" in kinds


@pytest.mark.parametrize("name,workload,merged", WORKLOADS, ids=[n for n, _, _ in WORKLOADS])
def test_no_stage_of_any_workload_can_write_substrate(name, workload, merged):
    q = int(workload.get("query_number") or 0)
    for stage in STAGES:
        # even a catch-all write glob (or one naming substrate) cannot open the layer
        for extra in ([], ["**"], ["substrate/**"]):
            for rel in SUBSTRATE_SAMPLES:
                ok, reason = write_allowed(
                    REPO, rel, query=q, stage=stage, write_paths=list(merged.write_paths) + extra,
                    pipeline_config=merged, benchmark=str(workload.get("benchmark_name") or "tpch"),
                )
                assert not ok, (name, stage, rel, reason)


def test_no_configured_write_glob_names_substrate():
    """Config hygiene: no write_paths entry (any workload, defaults, engine/aux defaults) matches substrate/."""
    pc = load_pipeline_config(repo_root=REPO)
    sources: dict[str, list[str]] = {
        "default.yaml": list(pc.write_paths),
        "aux_state.yaml": [str(x) for x in load_aux_state_defaults(REPO).get("write_paths") or []],
        "engine_backend.yaml": [str(x) for x in load_engine_backend_defaults(REPO).get("write_paths") or []],
    }
    for name, w, merged in WORKLOADS:
        sources[name] = list(merged.write_paths) + [str(x) for x in w.get("write_paths") or []]
    bad = [(src, pat, rel) for src, pats in sources.items() for pat in pats for rel in SUBSTRATE_SAMPLES
           if fnmatch.fnmatch(rel, pat.replace("{engine}", "duckdb"))
           and not pat.startswith("**") and pat != "**"]
    assert not bad, bad[:10]


def _tmp_pipeline_config(tmp_path: Path, **kw) -> PipelineConfig:
    return PipelineConfig(
        repo_root=tmp_path, pipeline_root=tmp_path / "pipeline", build_dir=tmp_path / "build",
        tpch_db_sf1=tmp_path / "a.duckdb", tpch_db_sf10=tmp_path / "b.duckdb", **kw,
    )


@pytest.mark.parametrize("cmd", [
    "sed -i 's/a/b/' substrate/aux/core/aux_types.hpp",
    "cp /tmp/x.hpp substrate/aux/core/x.hpp",
    "echo x > substrate/aux/core/x.hpp",
    "cat foo | tee substrate/verus/store/src/lib.rs",
    "rm -rf substrate/verus",
    "git checkout -- substrate/aux",
    "perl -pi -e 's/a/b/' ./substrate/aux/state/x.cpp",
])
def test_shell_commands_cannot_mutate_substrate(cmd: str):
    assert shell_command_layer_write(cmd), cmd


@pytest.mark.parametrize("cmd", [
    "cat substrate/aux/core/aux_types.hpp",
    "grep -rn AuxQueryModule substrate/ 2>&1 | head",
    "cmake --build build-release --target test_prims",
])
def test_shell_reads_of_substrate_stay_allowed(cmd: str):
    assert shell_command_layer_write(cmd) is None


def test_primitive_publish_cannot_write_substrate(tmp_path: Path):
    root = tmp_path / "primary"
    (root / pl.PRIMS_REL).mkdir(parents=True)
    victim = root / "substrate/aux/core/aux_types.hpp"
    victim.parent.mkdir(parents=True)
    victim.write_text("// substrate\n", encoding="utf-8")
    for rel in ("substrate/aux/core/aux_types.hpp", "substrate/aux/core/new_prim.hpp"):
        assert not pl.is_prims_path(rel)
        staged = root / pl.STAGING_REL / rel
        staged.parent.mkdir(parents=True, exist_ok=True)
        staged.write_text("#pragma once\n", encoding="utf-8")
        res = pl.primitive_publish(root, [rel], "try to write the substrate", 6)
        assert not res["ok"] and "not publishable" in res["error"], res
    assert victim.read_text(encoding="utf-8") == "// substrate\n"
    assert not (root / "substrate/aux/core/new_prim.hpp").exists()
    # what publish may write lies in accelerators/prims/ only
    for rel in (pl.PRIMS_REL, pl.README_REL, pl.TESTS_REL):
        assert layout.classify(rel).kind == "prims", rel


def _verus_ctx(tmp_path: Path):
    from accel_pipeline.verus.context import VerusContext

    ws = tmp_path / "ws"
    (ws / "crate" / "src").mkdir(parents=True)
    return VerusContext(
        query=6, workload_name="w", benchmark="tpch", repo_root=str(tmp_path / "root"),
        pipeline_root=str(tmp_path / "pipeline"), build_dir=str(tmp_path / "build"), lib_dir=str(tmp_path / "lib"),
        crate="tpch_q6_vgen", accel_id="tpch_q6_vgen", ffi_prefix="tpchq6vgen", query_sql="", schema_sql="",
        test_db="", bench_db="", run_dir=str(tmp_path / "run"), workspace=str(ws), artifacts=str(tmp_path / "art"),
        settings={})


def _rel_to(root: Path, p: Path) -> str:
    return str(Path(p).resolve().relative_to(Path(root).resolve()))


def test_verus_publish_and_generated_outputs_stay_out_of_substrate(tmp_path: Path):
    from accel_pipeline.verus import sandbox
    from accel_pipeline.verus import workspace as ws

    ctx = _verus_ctx(tmp_path)
    ctx.root.mkdir(parents=True)
    (ctx.root / "substrate").mkdir()
    # prooflib_publish writes exactly the published proof library
    assert layout.classify(_rel_to(ctx.root, ws.prooflib_lib(ctx))).kind == "prooflib"
    # sql2verus / the glue stage write into the query's kit directory
    for p in (ctx.kit_dir, ctx.crate_dir, ctx.crate_dir / "src" / "gen_spec.rs"):
        lay = layout.classify(_rel_to(ctx.root, p))
        assert lay.kind == "query" and lay.query == 6, p
    # the agents' sandbox mounts substrate/ read-only over the read-write tree
    binds = sandbox.mounts(ctx)
    assert ("ro", ctx.root.resolve() / "substrate", ctx.root.resolve() / "substrate") in binds


def test_verus_records_write_under_pipeline(tmp_path: Path):
    from accel_pipeline.verus import actions

    ctx = _verus_ctx(tmp_path)
    ctx.root.mkdir(parents=True)
    res = actions.playbook_append(ctx, "proofs", "a lesson", "body of the lesson", "evidence: " + "x" * 40)
    assert res.get("ok", True), res
    actions.backend_addition_record(ctx, "duckdb", "helper", "reason")
    written = [p for p in tmp_path.rglob("*") if p.is_file()]
    assert written and all("substrate" not in p.relative_to(tmp_path).parts for p in written), written


def test_scaffold_and_workload_paths_stay_out_of_substrate():
    pc = load_pipeline_config(repo_root=REPO)
    checked = 0
    for name, w, _ in WORKLOADS:
        if w.get("workload_kind") == "engine_backend":
            continue
        try:
            paths = resolve_workload_paths(pc, w, int(w.get("query_number") or 0))
        except Exception:  # noqa: BLE001
            continue
        for key, val in paths.items():
            if isinstance(val, str) and "/" in val and not val.startswith("/"):
                assert layout.classify(val).kind != "substrate", (name, key, val)
                checked += 1
    assert checked > 50


_WRITE_OP = re.compile(r"write_text|write_bytes|open\([^)]*['\"][wa]b?['\"]|mkdir|shutil\.(?:copy|move|rmtree)"
                       r"|\.unlink\(|>\s*\"?\$|cp |mv |rm |tee ")
_SUBSTRATE_LITERAL = re.compile(r"""["'/]substrate(?:/|["'])""")


def _generator_sources() -> list[Path]:
    scripts = REPO / "scripts"
    gens = [p for p in scripts.glob("*") if p.is_file() and re.match(r"(gen|generate|consolidate|make|update)_", p.name)]
    gens += sorted((REPO / "pipeline/gates").glob("*.py")) + sorted((REPO / "pipeline/gates").glob("*.sh"))
    gens += sorted((REPO / "pipeline/orchestrator/accel_pipeline").rglob("*.py"))
    return gens


def test_generator_scripts_do_not_write_substrate():
    """Static: no generator/gate/orchestrator line both names a substrate path and writes."""
    srcs = _generator_sources()
    assert len(srcs) > 50
    bad = []
    for p in srcs:
        for no, line in enumerate(p.read_text(encoding="utf-8", errors="replace").splitlines(), 1):
            if line.lstrip().startswith("#"):
                continue
            if _SUBSTRATE_LITERAL.search(line) and _WRITE_OP.search(line):
                bad.append(f"{p.relative_to(REPO)}:{no}: {line.strip()[:120]}")
    assert not bad, bad

"""substrate/** is never writable: no stage, publish tool or generator may write under it.

Fails if any stage's write rules (for every workload, every stage), any agent
tool (repo writes, shell), any publish tool (primitive_publish, prooflib_publish,
playbook / backend-addition records) or any generator (scaffold stubs, the
Verus kit and sql2verus outputs, generator scripts) can write under substrate/.
"""

from __future__ import annotations

import fnmatch
from pathlib import Path

import pytest

from accel_pipeline import layout, prims_library as pl
from accel_pipeline.config import (
    apply_workload_overrides,
    load_aux_state_defaults,
    load_engine_backend_defaults,
    load_workload,
    workload_supported_engines,
    load_pipeline_config,
)
from accel_pipeline.layout import shell_command_layer_write, write_allowed

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
    (root / layout.PRIMS).mkdir(parents=True)
    victim = root / "substrate/aux/core/aux_types.hpp"
    victim.parent.mkdir(parents=True)
    victim.write_text("// substrate\n", encoding="utf-8")
    for rel in ("substrate/aux/core/aux_types.hpp", "substrate/aux/core/new_prim.hpp"):
        assert not pl.is_prims_path(rel)
        staged = root / layout.STAGING_REL / rel
        staged.parent.mkdir(parents=True, exist_ok=True)
        staged.write_text("#pragma once\n", encoding="utf-8")
        res = pl.primitive_publish(root, [rel], "try to write the substrate", 6)
        assert not res["ok"] and "not publishable" in res["error"], res
    assert victim.read_text(encoding="utf-8") == "// substrate\n"
    assert not (root / "substrate/aux/core/new_prim.hpp").exists()
    # what publish may write lies in accelerators/prims/ only
    for rel in (layout.PRIMS, pl.README_REL, pl.TESTS_REL):
        assert layout.classify(rel).kind == "prims", rel








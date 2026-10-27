"""The exit run is a registered gate, planned at every stage exit and admitted like the heavy gates it replaces."""

from __future__ import annotations

from pathlib import Path

from accel_pipeline.config import load_pipeline_config, load_workload, workload_supported_engines
from accel_pipeline.gates import DEFAULT_GROUPS, GATE_SCRIPTS, SUBSET_DATASET_GATES
from accel_pipeline.stages.base import (
    workload_post_audit_gates,
    workload_post_implementation_gates,
    workload_post_optimization_gates,
)

REPO_ROOT = Path(__file__).resolve().parents[3]


def test_the_exit_run_is_a_gate_with_its_script_and_the_subset_dataset():
    assert (REPO_ROOT / "pipeline/gates" / GATE_SCRIPTS["aux_exit"]).is_file()
    assert "aux_exit" in SUBSET_DATASET_GATES


def test_every_stage_exit_runs_it_after_the_unit_checks():
    workload = load_workload("tpch_q3_aux", REPO_ROOT, engine=workload_supported_engines("tpch_q3_aux", REPO_ROOT)[0])
    for gates in (workload_post_implementation_gates(workload), workload_post_optimization_gates(workload),
                  workload_post_audit_gates(workload)):
        assert gates.index("aux_unit") < gates.index("aux_exit")
        assert not {"aux_correctness", "aux_multi_connection", "aux_bench_sweep", "aux_tsan"} & set(gates)


def test_it_is_admitted_only_after_quick_check():
    quick = load_pipeline_config(repo_root=REPO_ROOT).quick_tier
    assert "aux_exit" in quick.admission_gates
    assert any(group.names == ("aux_exit",) for group in DEFAULT_GROUPS)

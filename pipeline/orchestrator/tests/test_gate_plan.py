from accel_pipeline.stages.gate_plan import (
    workload_post_implementation_gates,
    workload_post_optimization_gates,
)


def test_base_implementation_gates_include_correctness_and_profile():
    workload = {"benchmark_name": "sec_edgar"}
    gates = workload_post_implementation_gates(workload)
    assert gates[0] == "correctness"
    assert "baseline" in gates
    assert "profile" in gates


def test_base_optimization_gates_order():
    workload = {
        "post_optimization_gates": ["correctness", "baseline", "profile", "fairness"],
    }
    gates = workload_post_optimization_gates(workload)
    assert gates == ["correctness", "baseline", "profile", "fairness"]

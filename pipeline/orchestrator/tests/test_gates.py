from __future__ import annotations

import pytest

from accel_pipeline.gates import GateFailure, GateResult, require_gate_pass


def test_require_gate_pass_accepts_pass():
    require_gate_pass(GateResult(name="aux_bench_sweep", status="pass", data={}))


def test_require_gate_pass_rejects_fail():
    result = GateResult(
        name="aux_bench_sweep",
        status="fail",
        data={"notes": "sweep status=empty"},
    )
    with pytest.raises(GateFailure, match="aux_bench_sweep fail"):
        require_gate_pass(result)
    with pytest.raises(GateFailure, match="sweep status=empty"):
        require_gate_pass(result)


def test_require_gate_pass_rejects_skip_by_default():
    result = GateResult(name="aux_bench_sweep", status="skip", data={"reason": "binary missing"})
    with pytest.raises(GateFailure, match="skip"):
        require_gate_pass(result)


def test_require_gate_pass_allows_skip_when_configured():
    result = GateResult(name="baseline", status="skip", data={})
    require_gate_pass(result, allow_skip=True)

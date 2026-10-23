"""Is a benchmark measuring the accelerator? (workstream B fills the checks.)"""

from __future__ import annotations

from typing import Any

from accel_pipeline.failure import Failure


def check_measurement(rows_or_metrics: Any, *, workload: dict, phase: str,
                      baseline: dict | None = None) -> list[Failure]:
    """Failures (kind="measurement", non-empty diagnosis) when the signal is untrustworthy; [] when it is.

    phase: "baseline" | "attempt" | "gate".
    """
    return []

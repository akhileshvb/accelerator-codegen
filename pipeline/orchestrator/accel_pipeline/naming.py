"""Names derived from a query's benchmark and number: the one place the naming convention lives.

Engine-specific symbols (a registration function) are patterns in the engine profile
(`entry_points.registration_symbol`, schemas/engine_profile.schema.json); this module
only expands them.
"""

from __future__ import annotations

from typing import Any


def bench_pascal(benchmark: str) -> str:
    return benchmark.title().replace("_", "")


def base_function_name(benchmark: str, query: Any) -> str:
    return f"accel_{benchmark}_q{query}"


def aux_function_name(benchmark: str, query: Any) -> str:
    return f"{base_function_name(benchmark, query)}_aux"


def accelerator_id(benchmark: str, query: Any) -> str:
    return f"{benchmark}_q{query}_aux"


def base_workload_name(benchmark: str, query: Any) -> str:
    """The base accelerator's workload: the prerequisite of the aux one."""
    return f"{benchmark}_q{query}"


def header_basename(benchmark: str, query: Any) -> str:
    return f"{benchmark}_q{query}"


def unit_test_target(benchmark: str, query: Any) -> str:
    return f"test_{benchmark}_q{query}"


def benchmark_target(benchmark: str, query: Any) -> str:
    return f"benchmark_{benchmark}_q{query}"


def smoke_ctest_name(benchmark: str, query: Any) -> str:
    return f"{benchmark}_q{query}_smoke_test"


def aux_smoke_ctest_name(benchmark: str, query: Any) -> str:
    return f"{benchmark}_q{query}_aux_smoke_test"


def hybrid_benchmark_target(benchmark: str, query: Any) -> str:
    return f"benchmark_{benchmark}_q{query}_hybrid_eval"


def registration_symbol(pattern: str, benchmark: str, query: Any) -> str:
    """Expand a profile's registration-symbol pattern: {Bench} is the CamelCase benchmark, {N} the query."""
    return pattern.replace("{Bench}", bench_pascal(benchmark)).replace("{N}", str(query))

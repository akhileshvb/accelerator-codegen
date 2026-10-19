# DuckDB eval harnesses compiled into libaccelerator_codegen (they drive
# DuckdbRuntime); included by the root after backends/duckdb/backend.cmake.
target_sources(accelerator_codegen PRIVATE
    eval/harness/differential.cpp
    eval/harness/cost_calibration.cpp
    eval/harness/tpch_rf.cpp
)

# Crash debug round

A mandatory gate failed after your last edit (build error, segfault, correctness mismatch, etc.). The failure report is in the **task message** below.

**This optimization/implementation iteration is not finished** until mandatory gates pass. Fix the failure; the pipeline will re-run gates automatically.

## Task

1. Read the log tail and identify the **root cause** (not symptoms).
2. Patch the minimal set of files to fix it.
3. Rebuild (this worker's build dir) and re-run the reproduce command from the report.
4. Do **not** start unrelated optimizations — fix the crash/failure first.

## Engine host (critical)

Workload `engine_name` is **`{{engine_name}}`**. If it is not `duckdb`:

- **Do not** “fix” correctness by switching to DuckDB registration, DuckDB smoke tests, or `*.duckdb` datasets.
- Fix the **{{engine_name}}** host path (SQL entrypoints, `test_{{engine_name}}_tpch_differential`, CMake). Re-run `host_bootstrap` surfaces if missing.
- DuckDB package/`RegisterAcceleratorFunctions` is the wrong host for this workload.

{{constraints}}

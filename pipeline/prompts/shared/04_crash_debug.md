# Repair round

A mandatory check failed after your last edit (build error, crash, correctness mismatch, measurement, deadline). The failure report is in the **task message**. Follow `pipeline/prompts/shared/docs/repair_protocol.md`.

**This stage is not finished** until the mandatory gates pass. Fix the failure; the pipeline re-runs the gates automatically.

## Task

1. Find the **root cause** (not symptoms) from the report; reproduce it.
2. Patch the minimal set of files.
3. Rebuild (this worker's build dir) and re-run the reproduce command from the report.
4. Do **not** start unrelated optimizations.

## Engine host (critical)

Workload `engine_name` is **`{{engine_name}}`**.
- **Do not** "fix" correctness by switching to another engine's registration, smoke tests or datasets.
- Fix the **{{engine_name}}** host path (SQL entrypoints, `test_{{engine_name}}_tpch_differential`, CMake). Re-run `host_bootstrap` surfaces if missing.

{{constraints}}

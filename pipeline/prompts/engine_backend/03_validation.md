# Engine backend validation

Validate the **{{display_name}}** (`{{engine_name}}`) adapter.

{{hard_guidelines}}

## Checks

1. Target paths exist:
{{engine_target_paths}}
2. Native base-table scans only (`ScanNativeBaseTable` / equivalent); aux may use custom storage.
3. Mutation/txn bridges forward to `AuxTransactionCoordinator`.
4. Rebuild/resync semantics respected for aux maintenance.
5. CMake collector `{{engine_cmake_collect_fn}}` is referenced when a build flag is used.
6. Run gates: `engine_build`, `engine_scan_conformance` (and `engine_aux_smoke` if configured).

Write `{{artifacts_path}}/validation_report.md` summarizing fitness and remaining gaps.

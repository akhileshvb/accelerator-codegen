# Engine backend validation

Validate the **{{display_name}}** (`{{engine_name}}`) adapter.

{{hard_guidelines}}

## Checks

1. Scan adapter + stateful backend files exist at target paths.
2. Native base-table scans only; aux may use custom storage.
3. Mutation/txn bridges forward to `AuxTransactionCoordinator`.
4. Rebuild/resync semantics respected for aux maintenance.
5. Run gates: `engine_build`, `engine_scan_conformance` (and `engine_aux_smoke` if configured).

Write `{{artifacts_path}}/validation_report.md` summarizing fitness and remaining gaps.

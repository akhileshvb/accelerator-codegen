# Runtime policy (placeholders)

Use accelerator-specific env vars following this pattern (replace `{PREFIX}` / `{ACCEL_ID}`):

| Variable | Purpose |
|----------|---------|
| `ACCEL_{ACCEL_ID}_MAINTENANCE_MODE` | `hybrid` (cost-based selector), `write_policy`, `force_action` |
| `{PREFIX}_WRITE_POLICY` | Baseline write-path policy override |
| `{PREFIX}_FORCE_ACTION` | Force a specific `MaintenanceAction` for testing |
| `ACCEL_{ACCEL_ID}_COST_MODEL_PATH` | Path to `config/q{N}_aux_cost_model.json` |

Document chosen names in generated diagnostics. Defaults should use hybrid cost-based selection with lazy-on-query repair.

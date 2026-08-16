# Optimization fix round

Your hypothesis patch **built or passed smoke initially but failed validation** (compile error or quick correctness test). The failure report is in the **task message** below.

{{hard_guidelines}}

{{codex_tooling}}

**Stay on the same hypothesis** — fix the reported error; do not abandon the patch for a new redesign unless the failure is unfixable.

## Task

1. Read the log tail and identify the **root cause**.
2. Patch the minimal set of editable files (header, module, tests, cost model as needed).
3. Rebuild and re-run the reproduce command from the report (MCP `run_pipeline_command` or allowlisted shell).
4. When the reproduce command passes, reply briefly — the orchestrator will re-validate automatically.

{{kernel_inspiration_section}}

{{constraints}}

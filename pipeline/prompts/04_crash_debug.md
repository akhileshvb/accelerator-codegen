# Crash debug round

A mandatory gate failed after your last edit (build error, segfault, correctness mismatch, etc.). The failure report is in the **task message** below.

**This optimization/implementation iteration is not finished** until mandatory gates pass. Fix the failure; the pipeline will re-run gates automatically.

## Task

1. Read the log tail and identify the **root cause** (not symptoms).
2. Patch the minimal set of files to fix it.
3. `run_command`: rebuild and re-run the reproduce command from the report.
4. Do **not** start unrelated optimizations — fix the crash/failure first.

{{kernel_inspiration_section}}

{{constraints}}

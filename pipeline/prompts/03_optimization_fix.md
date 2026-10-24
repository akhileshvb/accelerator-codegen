# Optimization fix round (stateless)

Your hypothesis patch built or passed smoke at first but **failed validation** (compile error or quick correctness test). The failure report is in the task message.

{{hard_guidelines}}

{{codex_tooling}}

**Stay on the same hypothesis.** Fix the reported error; redesign only if the failure is unfixable.

1. Read the log tail and find the root cause.
2. Patch the smallest set of editable files.
3. Rebuild and re-run the reproduce command from the report (`quick_check`, or `run_pipeline_command`).
4. When it passes, reply briefly; the orchestrator re-validates.

{{constraints}}

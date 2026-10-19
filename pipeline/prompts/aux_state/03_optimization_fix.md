# Aux-state optimization fix round

Your hypothesis patch failed **build** or **quick smoke correctness** (`ctest -R …_aux`). The failure report is in the **task message** below.

{{hard_guidelines}}

{{codex_tooling}}

**Stay on the same hypothesis** — debug and fix; do not start a unrelated redesign.

## Task

1. Read assertion / compile errors in the log tail.
2. Patch editable aux files (`module.hpp`, `module.cpp`, `finalize.cpp`, tests, cost model).
3. Rebuild with `cmake --build build-release --target test_tpch_q{{query_number}}_aux` (and the bench target if needed), then run the `ctest` command from the report (shell or MCP `run_pipeline_command`).
4. Confirm reproduce passes before ending your turn.

{{constraints}}

# Aux-state optimization fix round

Your hypothesis patch failed **build** or **quick smoke correctness** (`ctest -R …_aux`). The failure report is in the **task message** below.

{{hard_guidelines}}

{{codex_tooling}}

**Stay on the same hypothesis** — debug and fix; do not start a unrelated redesign.

## Task

1. Read assertion / compile errors in the log tail.
2. Patch editable aux files (`module.hpp`, `module.cpp`, `finalize.cpp`, tests, cost model).
3. `run_command`: `cmake --build build-release --target test_tpch_q{{query_number}}_aux` (and bench target if needed), then the `ctest` command from the report.
4. Confirm reproduce passes before ending your turn.

{{kernel_inspiration_section}}

{{constraints}}

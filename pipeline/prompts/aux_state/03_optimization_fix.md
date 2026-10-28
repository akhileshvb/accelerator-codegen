# Optimization fix round (stateful)

Your hypothesis patch failed **build** or **quick smoke correctness** (`ctest -R …_aux`). The failure report is in the task message.

{{hard_guidelines}}


**Stay on the same hypothesis.** Fix the reported error; do not start an unrelated redesign.

1. Read the assertion or compile error in the log tail.
2. Patch the smallest set of editable files (`module.hpp`, `module.cpp`, `finalize.cpp`, tests, cost model).
3. Rebuild (`cmake --build {{build_dir}} --target {{test_target}}_aux`, plus the bench target if needed) and run the `ctest` command from the report, or use `quick_check`.
4. When the reproduce passes, end your turn.

{{constraints}}

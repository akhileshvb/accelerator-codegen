# Stage 3: Aux-state optimization patch (iter {{optimization_iter}})

Implement the **minimum coordinated patch** to test the supplied hypothesis on the **aux-state accelerator** (not the base kernel). **Be aggressive on read-path performance**; maintenance must remain correct.

## Mindset

Analysis already chose a read-path hypothesis. Your job:

1. **Read** the full existing implementation.
2. **Apply** the patch in **few large steps** (not dozens of tiny edits).
3. **Hand off** — the orchestrator builds, smoke-tests, benchmarks (`read_only_query_speedup`), and accept/rejects.

Do **not** run benchmarks yourself. Do **not** switch hypotheses.

**Scale contract**: size structures from bind-time `row_estimate` with amortized growth. Never hardcode SF1 row counts, bucket sizes, or cardinality constants from the iteration dataset.

## Workflow (follow in order)

### 1. Read full implementation

Use `read_file` with `limit=600` on each editable file (read every line before editing):

{{optimization_edit_paths}}

Do **not** edit `src/generated/tpch/q{{query_number}}/base/**` (read-only reference).

### 2. Apply patch (prefer few large writes)

- Follow the hypothesis `edit_plan` when present.
- **Prefer `write_file`** on a file you have fully read when the hypothesis touches multiple regions in that file.
- Otherwise use **one large `apply_patch` per function/struct** (include enough context that `old_string` is unique).
- Avoid chains of many small `apply_patch` / `insert_lines` calls — batch the change, then hand off.
- Accept/reject score is `read_only_query_speedup`. The orchestrator builds after handoff; do not rely on `run_command`.

### 3. Hand off to orchestrator

When edits are done, **stop calling tools**. Reply with a short summary of what changed and why it tests the hypothesis, then on the **same message**:

`HYPOTHESIS_PATCH_COMPLETE`

The orchestrator will then: **build → smoke test → read-only benchmark → accept/reject**.

If the hypothesis truly cannot be tested after reading the full implementation:

```json
{"status": "cannot_test", "reason": "..."}
```

and `CANNOT_TEST`.

## Primary sources

- module: `{{kernel_path}}`
- header: `{{header_path}}`
- finalize: `{{package_path}}`
- cost model: `{{cost_model_config}}`

{{constraints}}

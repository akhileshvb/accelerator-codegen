# Stage 3: Optimization patch (iter {{optimization_iter}})

Implement the **minimum coordinated patch** to test the supplied hypothesis.

## Mindset

Analysis chose a kernel-suited hypothesis. Your job:

1. **Read** the full existing implementation.
2. **Apply** the patch in **few large steps**.
3. **Hand off** — orchestrator runs build → smoke → benchmark → accept/reject.

Do **not** run benchmarks yourself. Do **not** switch hypotheses.

## Workflow (follow in order)

### 1. Read full implementation

Use `read_file` with `limit=600` on each editable file before any edits:

{{optimization_edit_paths}}

### 2. Apply patch (prefer few large writes)

- Follow the hypothesis `edit_plan` when present.
- **Prefer `write_file`** when you have fully read a file and the hypothesis changes multiple regions in it.
- Otherwise use **one large `apply_patch` per function/struct** — not many one-line patches.
- Use `grep_context` only for targeted symbol lookup after the full read.

### 3. Hand off to orchestrator

When edits are done, **stop calling tools**. Short summary + `HYPOTHESIS_PATCH_COMPLETE` on the **same message**.

The orchestrator will: **build → smoke test → benchmark → accept/reject**.

If the hypothesis cannot be tested after reading the full implementation:

```json
{"status": "cannot_test", "reason": "..."}
```

and `CANNOT_TEST`.

Kernel paths:
- header: `{{header_path}}`
- kernel: `{{kernel_path}}`
- package: `{{package_path}}`

{{constraints}}

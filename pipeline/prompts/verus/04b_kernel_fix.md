# Verus pipeline: fix the kernel ({{benchmark}} q{{query}})

The kernel in `crate/src` fails a gate:

```
{{failure}}
```

Fix the kernel (never the frozen files, never the tests). `stage` says which
gate: `frozen` (restore the listed files), `trusted_base` (remove the listed
constructs and prove instead), `verify` (Verus errors), `build`, or `tests`
(the accelerator's answer differs from the SQL: `examples` shows where; or it
answers with SQL too often). Use `verus_verify` and `verus_test` until both pass.

{{include:shared/rules.md}}

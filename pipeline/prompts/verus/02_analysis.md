# Verus pipeline: analysis ({{benchmark}} q{{query}})

Design the auxiliary state of a Verus-verified accelerator for
`query/query.sql`, from the query alone. The state is maintained across
write transactions and answers the query (for any parameter values) much
faster than running the SQL. Read `query/`, `spec/gen_spec.rs`,
`contracts/inputs_language.md`, `contracts/scale_principles.md`,
`contracts/verus_kernel.md` and the playbook.

Decide:

1. **The aux-state family**: what is materialized, keyed by what, so a query
   reads only its slice. Explain why it answers the query (relate it to the
   spec's definitions) and why it is cheap to read.
2. **Maintenance**: for every table the query reads, how an insert / delete
   / update changes the state, locally. Which changes need rows the state
   does not keep (a rescan), and of which keys.
3. **The kernel inputs** (`inputs/q.inputs`): the scans the build needs, the
   rescans, and one delta per table (lossy where you can drop columns or text).
4. **Complexity notes** (guidance, see scale_principles): state size, build,
   per-write maintenance and per-query read cost as rough formulas of table
   sizes and distinct keys; integer widths at SF3000.
5. If you cache the final answer, justify it (small, cheap to maintain
   locally, large speedup).

Write `inputs/q.inputs` and `analysis/analysis.json`:
`{"aux_state": "...", "keys": "...", "read_path": "...", "maintenance": {"<table>": "..."}, "rescans": "...", "complexity": {"state_bytes": "...", "build": "...", "write": "...", "read": "..."}, "widths": "...", "final_answer_cache": null | "justification", "risks": ["..."]}`.
Run `verus_check_inputs` until sql2verus accepts the inputs, and read the
obligations it returns: they are what the kernel will be proved against.

Feedback from the previous round: {{feedback}}

{{include:shared/rules.md}}

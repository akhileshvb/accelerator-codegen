# Verus pipeline: critique the analysis ({{benchmark}} q{{query}})

The query, the spec, the analysis and the declared inputs are inlined below
(`verus_check_inputs` shows the obligations they yield). Try to break the
design:

* a write path it misses (every table, insert / delete / update, key
  changes, duplicates: tables have no declared keys);
* changes that would force a rebuild or a whole-table rescan;
* state that grows with whole tables rather than with what queries read, or
  reads that touch the whole state (contracts/scale_principles.md);
* overflow at SF3000; NULLs; parameters the state cannot serve;
* a final-answer cache without a convincing justification.

Write `analysis/critique.json`:
`{"verdict": "accept" | "revise", "objections": [{"issue": "...", "evidence": "...", "suggestion": "..."}]}`.
Ask for a revision only for real problems, not taste.

## Material

You have these files in full below: do not re-read them. Work from this text and answer in a few steps; use tools only to check something against data (`verus_sql`, `verus_check_inputs`).

{{file:query/query.sql}}

{{file:analysis/analysis.json}}

{{file:inputs/q.inputs}}

{{file:contracts/scale_principles.md}}

{{file:spec/gen_spec.rs}}

{{include:shared/rules.md}}

# Verus pipeline: critique the analysis ({{benchmark}} q{{query}})

Read `query/query.sql`, `spec/gen_spec.rs`, `analysis/analysis.json` and
`inputs/q.inputs` (`verus_check_inputs` shows the obligations). Try to break
the design:

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

{{include:shared/rules.md}}

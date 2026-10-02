# Verus pipeline: critique the analysis ({{benchmark}} q{{query}})

Answer one question, quickly: **does this design have a blocking problem?**
The query, the spec, the analysis and the declared inputs are inlined below.

Look for these only:

* a write path it misses (every table, insert / delete / update, key
  changes, duplicates: tables have no declared keys);
* changes that would force a rebuild or a whole-table rescan;
* state that grows with whole tables rather than with what queries read, or
  reads that touch the whole state;
* overflow at SF3000; NULLs; parameters the state cannot serve;
* a final-answer cache without a convincing justification.

Be brief. Judge from the material below; do not re-derive or re-verify the
design, and do not explore the workspace. Use a tool only if one specific
claim decides your verdict (at most a couple of calls: `verus_check_inputs`,
`verus_sql`). Then write `analysis/critique.json` and stop:

`{"verdict": "accept" | "revise", "objections": [{"issue": "...", "evidence": "...", "suggestion": "..."}]}`

`revise` only for a real, blocking problem (each objection one or two
sentences); style, naming and nice-to-haves are not objections. If in doubt,
`accept`.

## Material

You have these files in full below: do not re-read them.

{{file:query/query.sql}}

{{file:analysis/analysis.json}}

{{file:inputs/q.inputs}}

{{file:contracts/scale_principles.md}}

{{file:spec/gen_spec.rs}}

{{include:shared/rules.md}}

# Verus pipeline: analysis ({{benchmark}} q{{query}})

Design the auxiliary state of a Verus-verified accelerator for
`query/query.sql`, from the query alone. The state is maintained across
write transactions and answers the query (for any parameter values) much
faster than running the SQL. Read `query/`, `spec/gen_spec.rs`,
`contracts/inputs_language.md`, `contracts/scale_principles.md`,
`contracts/verus_kernel.md` and the playbook.

**Every parameter is a runtime parameter.** Every TPC-H substitution
parameter (spec 2.4) of the query is a `$n` parameter in `query/query.sql`,
including LIKE patterns (a color, a word pair, a type syllable or prefix) and
numeric factors (Q11's fraction). The kit must answer correctly and
efficiently for ANY value in the parameter's TPC-H domain. A design
specialized to one value (e.g. only the parts of one color, one nation, one
pattern, one fraction) is not allowed. Its benchmarks and tests draw fresh
random values from the domain on every run, which you cannot see ahead of
time. The kernel may decline (host falls back to SQL) only for values outside
the TPC-H domain; declines count against it.

Decide:

1. **The aux-state family**: what is materialized, keyed by what, so a query
   reads only its slice. Explain why it answers the query (relate it to the
   spec's definitions) and why it is cheap to read: the read path's cost in
   proportion to the answer, at production scale (`scale_principles`:
   ordered indexes for ranked or thresholded output, pre-ordered groups for
   ordered output, coarse buckets for range parameters). The kernel is
   measured: a read must beat the base query clearly, and a write mix must
   not be slower than running without the accelerator.
2. **Maintenance**: for every table the query reads, how an insert / delete
   / update changes the state, locally: cost per changed row, independent of
   table size, including keeping the read-side indexes above in order. Which changes need rows the state
   does not keep (a rescan), and of which keys. Prefer keeping per-key
   sums, counts or multisets so a delta suffices; a rescan of every row of each
   touched key (e.g. all lineitems of each touched part) is paid per refresh and
   dominates it. If one is unavoidable, declare it in the inputs so it runs as
   one set-oriented query. Obligations are frozen before implementation, so this
   is decided here. A refresh (0.1% of orders, keys spread uniformly) touches
   nearly every page: state the unit a write copies and why it is O(changed rows).
3. **The kernel inputs** (`inputs/q.inputs`): the scans the build needs, the
   rescans, and one delta per table (lossy where you can drop columns or text).
4. **Complexity notes** (guidance, see scale_principles): state size, build,
   per-write maintenance and per-query read cost as rough formulas of table
   sizes and distinct keys; integer widths at SF3000. For the state, give
   each structure's entry layout, its real bytes per entry (with the
   overheads `scale_principles` lists) and its entry count, and total them
   at SF20 for the steady state and for the build peak, against the
   per-accelerator budget there.
5. If you cache the final answer, justify it (small, cheap to maintain
   locally, large speedup).
6. **How it will be proved** (`contracts/proof_method.md`): the invariant as
   sums over the database (which sum each structure equals), and the outline
   of the three top-level proofs (build, apply, execute). Choose data
   structures with proof cost AND per-write cost in mind: every committed
   version stays readable, so a structure that is cloned or rebuilt per write
   costs the whole state on every transaction. Use a **persistent** map (a
   write copies only the path to the keys it changes): the shared verified
   one in the proof library, or one proven in another query's kernel (to be
   lifted into the library), rather than writing a new tree from scratch. A
   `vstd` `HashMap` / `Vec` behind an `Arc` is NOT persistent: updating it
   copies it. Name the persistent structure for each part of the state.

Write `inputs/q.inputs` and `analysis/analysis.json`:
`{"aux_state": "...", "keys": "...", "read_path": "...", "maintenance": {"<table>": "..."}, "rescans": "...", "complexity": {"state_bytes": "...", "build": "...", "write": "...", "read": "..."}, "widths": "...", "proof": "...", "final_answer_cache": null | "justification", "risks": ["..."]}`.
Run `verus_check_inputs` until sql2verus accepts the inputs, and read the
obligations it returns: they are what the kernel will be proved against.

Feedback from the previous round: {{feedback}}


**Prefer the plainest provable state.** The kernel must be proved in Verus, and each custom structure is
its own proof project. Use maps (or sets) keyed by tuples, whose entries are sums or counts over the
database (`contracts/proof_method.md`), and meet the read / write cost bounds with the choice of keys.
Use a custom encoding (prefix arrays, multi-level or dyadic buckets, compression, packed offsets, custom
hash tables or pages) only where the plain version misses a cost bound, and then state its invariant and
how it will be proved. The critic rejects one without that.

{{include:shared/rules.md}}

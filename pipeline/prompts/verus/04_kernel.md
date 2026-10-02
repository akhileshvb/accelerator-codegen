# Verus pipeline: the kernel ({{benchmark}} q{{query}}, crate {{crate}})

Implement the kernel in `crate/src` against the frozen obligations
(`crate/src/gen_obligations.rs`), following the design in
`analysis/analysis.json`. Read `contracts/verus_kernel.md`,
`contracts/trusted_base.md`, `contracts/scale_principles.md`, the playbook,
and the libraries in `ref/` and `prooflib/`.

**Code first, proofs second.** This step is the executable kernel only; once
its code passes the frozen tests, you will be asked (in this same session) to
prove it. Testing first catches logic bugs in minutes instead of as
unprovable goals later.

`kernel.rs` starts as the generated stub: every entry point is
`#[verifier::external_body]` / `unimplemented!()`. Replace them all with real
implementations: `State` (with `db()` and a type invariant tying the data to
the database it answers for), `build`, `execute`, `apply`, and any
`rescan_<n>_keys`. Split the work into modules as you see fit. Write the spec
side now too (`db()`, the type invariant, the `requires`/`ensures` the
obligations impose; loop invariants you already know), since it shapes the
code; leave the proof bodies (lemmas, `assert`s, `proof` blocks) for the next
step. Do not add `assume`, `admit` or new `external_body` to get there (the
trusted-base check rejects them).

The host glue (`glue/`) is already written and frozen; you only write the
kernel. Loop: `verus_test` with `proofs=false` (compiles the kernel without
checking its proofs, builds the accelerator, runs the frozen tests on the Verus
store; `snapshots=true` adds concurrent readers) until they pass. Performance
comes later (an optimization loop), but design for it now: the aux state from
the analysis, not a recomputation of the query.

End with a short summary: the modules and what each holds, the state's
invariant in words, and anything you expect to be hard to prove.

{{include:shared/rules.md}}

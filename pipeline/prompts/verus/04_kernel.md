# Verus pipeline: the kernel ({{benchmark}} q{{query}}, crate {{crate}})

Implement and prove the kernel in `crate/src` against the frozen obligations
(`crate/src/gen_obligations.rs`), following the design in
`analysis/analysis.json`. Read `contracts/verus_kernel.md`,
`contracts/trusted_base.md`, `contracts/scale_principles.md`, the playbook,
and the libraries in `ref/` and `prooflib/`.

`kernel.rs` starts as the generated stub: every entry point is
`#[verifier::external_body]` / `unimplemented!()`. Replace them all with
verified implementations: `State` (with `db()` and a type invariant tying the
data to the database it answers for), `build`, `execute`, `apply`, and any
`rescan_<n>_keys`. Split the work into modules and lemmas as you see fit.

The host glue (`glue/`) is already written and frozen; you only write the
kernel. Loop: `verus_verify` (Verus + trusted-base check) until it verifies, then
`verus_test` (builds the accelerator and runs the frozen tests on the Verus
store; `snapshots=true` adds concurrent readers) until they pass. Performance
comes later (an optimization loop), but design for it now: the aux state from
the analysis, not a recomputation of the query.

Add general lemmas to `prooflib/src/lib.rs` and publish them with
`prooflib_publish` when they would serve other queries. Record what you
learned about proving (what invariant shapes, triggers or lemma structure
worked, with verification times) with `playbook_append` (section `proofs`).

{{include:shared/rules.md}}

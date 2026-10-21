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
`rescan_<n>_keys` and its spec fn `rescan_<n>_wanted`. Split the work into modules as you see fit. Write the spec
side now too (`db()`, the type invariant, the `requires`/`ensures` the
obligations impose; loop invariants you already know), since it shapes the
code; leave the proof bodies (lemmas, `assert`s, `proof` blocks) for the next
step. Do not add `assume`, `admit` or new `external_body` to get there (the
trusted-base check rejects them).

"No proofs yet" still means **Verus-compatible code**: Verus checks these even
without verifying, so write them from the start:

* only the Rust standard library Verus supports: `Vec` (`new`, `push`, `pop`,
  `len`, indexing, `set`, `insert`, `remove`, `clear`), slices by index, and
  the `vstd` collections and helpers; no `swap`, `to_vec`, `sort`, iterators
  or closures over collections; write the loop (or a small helper) instead;
* every `loop` / `while` has a `decreases` clause (a termination measure);
* every quantifier you write (`forall` / `exists`, in specs and invariants)
  has a `#[trigger]` on a term that mentions its bound variables;
* exec code calls only exec functions; spec functions appear only in specs,
  `proof` blocks and ghost code.

Write the code so it is ready to prove (read `contracts/proof_method.md`): say
in the type invariant which sum over the database each structure equals; give
every loop over input rows a ghost index (the invariant will be "accumulated
so far == the sum over the rows processed"); prefer `vstd` collections to
hand-written trees or indexes, whose own proofs are large.

The host glue (`glue/`) is already written and frozen; you only write the
kernel. Loop: `verus_test` with `proofs=false` (compiles the kernel without
checking its proofs, builds the accelerator, runs the frozen tests on the Verus
store, the database's one global store; `snapshots=true` adds concurrent readers) until they pass. Performance
comes later (an optimization loop), but design for it now: the aux state from
the analysis, not a recomputation of the query.

End with a short summary: the modules and what each holds, the state's
invariant in words, and anything you expect to be hard to prove.

## Proven code in this tree

These kernels of other queries verified fully (read them freely; they are proved against their own specs):

{{completed_kernels}}

Before you write a data structure or a long lemma, look for one there (and in the shared proof library,
`prooflib/src/lib.rs`). To reuse one, **lift it into the shared proof library** -- made generic (no
query-specific types), with its specification and proofs -- and publish it with `prooflib_publish`; then
use it through `verus_prooflib`. Do not copy another query's module into your crate. Lifting helps every
later kernel, and the library is re-verified with every completed kernel when you publish.

Every state version must stay valid after a write (readers may still use it): a structure that is cloned
or rebuilt per write costs the whole state on every transaction. Prefer a persistent structure (a write
copies only the path to the keys it changes, e.g. a path-copying trie) -- look for one in the kernels
above and the library before writing your own.

{{include:shared/rules.md}}

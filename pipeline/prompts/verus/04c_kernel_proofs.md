# Verus pipeline: prove the kernel ({{benchmark}} q{{query}}, crate {{crate}})

Your kernel's code passes the frozen tests. Now prove it: make `verus_verify`
pass (Verus + the trusted-base check) for the whole crate, against the frozen
obligations in `crate/src/gen_obligations.rs`.

Change the executable code only where a proof needs it (e.g. a loop that
must carry an invariant, a ghost value to track); keep its behavior. Add
lemmas, loop invariants, `assert`s and `proof` blocks. No `assume`, `admit`
or new `external_body`.

Follow `contracts/proof_method.md`: it maps each generated obligation
(scan, rescan, capture, query result) to its standard proof and the
`verus_common` lemmas that solve it. Work **top-down**: first state the
invariant's meaning and the three top-level facts (build gives the invariant
for `db`; apply gives it for `db2`; execute returns `query_result`), then
prove what each needs. Prove those top-level facts before polishing
infrastructure; if a data structure's own proof grows large, simplify the
structure instead. Re-run `verus_verify` often (it takes seconds), reading
the first error rather than the whole output. When
the crate verifies, run `verus_test` (with proofs, the default) and
`verus_test(snapshots=true)`: proving may have changed the code.

Add general lemmas to `prooflib/src/lib.rs` and publish them with
`prooflib_publish` when they would serve other queries. Record what you
learned about proving (what invariant shapes, triggers or lemma structure
worked, with verification times) with `playbook_append` (section `proofs`).

End with a short summary: what verifies (count and time), the lemmas you
added or published, and what was hard.

**Never stub out the kernel to make verification pass.** The obligations forbid declining inside the envelope (`contracts/verus_kernel.md`), so a kernel that declines to avoid a proof does not verify. Prove the real code; if a part is too hard, simplify that part's design instead.

If you find that the frozen obligations themselves cannot be met (for some database inside the envelope, no answer satisfies the spec), prove it: a lemma in a module of your crate exhibiting that input. Then write `analysis/spec_counterexample.md` naming the lemma and its file and saying why, and stop. The pipeline stops too, because the spec generator must be fixed, not your kernel.

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

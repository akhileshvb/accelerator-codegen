# Verus pipeline: prove the kernel ({{benchmark}} q{{query}}, crate {{crate}})

Your kernel's code passes the frozen tests. Now prove it: make `verus_verify`
pass (Verus + the trusted-base check) for the whole crate, against the frozen
obligations in `crate/src/gen_obligations.rs`.

Change the executable code only where a proof needs it (e.g. a loop that
must carry an invariant, a ghost value to track); keep its behavior. Add
lemmas, loop invariants, `assert`s and `proof` blocks. No `assume`, `admit`
or new `external_body`.

Work in small steps: prove one function or module at a time and re-run
`verus_verify`, reading the first error rather than the whole output. When
the crate verifies, run `verus_test` (with proofs, the default) and
`verus_test(snapshots=true)`: proving may have changed the code.

Add general lemmas to `prooflib/src/lib.rs` and publish them with
`prooflib_publish` when they would serve other queries. Record what you
learned about proving (what invariant shapes, triggers or lemma structure
worked, with verification times) with `playbook_append` (section `proofs`).

End with a short summary: what verifies (count and time), the lemmas you
added or published, and what was hard.

{{include:shared/rules.md}}

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

{{include:shared/rules.md}}

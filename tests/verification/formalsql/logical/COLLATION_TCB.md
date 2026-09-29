# Collation correspondence TCB — RelNode text ORDER BY

## Claim

RelNode `value_le` / Dafny `ValueLe` on `VText` use **byte-lexicographic ≤**
(`text_c_leb` / `TextCLeb`).

## FormalSQL / Logos authority

Vendored FormalSQL (`Values.v`):

> Under PostgreSQL's UTF8/C environment, text comparison is the
> lexicographic comparison of the encoded bytes. Rocq strings are byte
> strings and `[string_compare]` is exactly that comparison.

Logos lower for JetStream pins `--sql-default-collation C` (see
`scripts/logos_lower_catalog.sh`).

## Residual correspondence

Cross-prover agreement that JetStream `text_c_leb` matches FormalSQL
`string_compare` under C collation for TPC-H `CHAR`/`VARCHAR` values is a
**residual TCB assumption** until a mechanical dual proof is added.

It is **not** an arbitrary deterministic order: the intended SQL semantics
are FormalSQL/Logos C-collation ordering.

## Fail-closed

Other `SqlValue` kinds (and mismatched text/int pairs) still return
`None` / `NoBool` from `value_le`.

## Discharged in Rocq for Q21

`Q21_text_c_leb_string_compare` (q21/Q21NormalizeSoundFinal.v) proves
`text_c_leb a b = true <-> string_compare a b <> Gt` for all Rocq strings, and
Q21's ORDER BY bridge uses only that lemma (Print Assumptions: closed). The
residual above remains for the Rocq ↔ Dafny (`TextCLeb`) correspondence.

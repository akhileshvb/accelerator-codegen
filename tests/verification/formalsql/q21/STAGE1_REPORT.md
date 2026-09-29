# Q21 Stage 1 — LogicalSpec IR extension

**Status: PASS** (2026-09-28)

## Extensions

1. **`RelSemiJoin` / `RelAntiJoin`** (Rocq + Dafny)
   - Output schema = left; `cond` typed over left++right
   - Semi: each left row at most once (SQL EXISTS multiplicity)
   - Anti: keep left rows with no matching right (NOT EXISTS)
   - Helpers: `join_exists_right` / `join_semi_rows` / `join_anti_rows` (+ Dafny duals)

2. **VText ORDER BY** via `text_c_leb` / `TextCLeb`
   - Byte-lexicographic ≤ matching FormalSQL C-collation `string_compare`
   - Residual correspondence: [`COLLATION_TCB.md`](../logical/COLLATION_TCB.md)

3. Serialize (`semi_join` / `anti_join`), emit script, vocab, manifest, pin updates
4. Morphisms / PROOF_LIBRARY_INDEX pattern notes for Semi/Anti

## Gates

| Gate | Result |
|------|--------|
| `compile_logical_ir.sh` | OK |
| `check_spec_export.sh` | SPEC_EXPORT_CHECKED OK |
| `dafny verify EvalLogicalSpec.dfy` | 74 verified, 0 errors |
| `run_verification_q1.sh` | PASS (Q1 regression green) |

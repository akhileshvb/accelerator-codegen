# Q21 parameterization bridge

## Intent

```text
∀ nation_name.
  Hit(S, nation_name) →
  Exec_Q21(DB(S), nation_name) ≈ Eval_FormalSQL(q̂_Q21(nation_name), DB(S))
```

## Catalog SQL vs LogicalSpec

| Layer | Nation binding |
|-------|----------------|
| Catalog `base_sql` / Logos `source_query_expr` | `CstString "SAUDI ARABIA"` |
| `LogicalSpec_Q21_rel` | `EParam "nation_name"` |
| Accelerator interface | `nation_name` argument |

## Explicit bridge (not silent generalization)

1. `normalize_q21` **recognizes** the Logos-lowered catalog AST (with constant
   `'SAUDI ARABIA'`) and emits the **parameterized** RelNode template.
2. Catalog instantiation:
   `params_catalog_nation = [("nation_name", VText "SAUDI ARABIA")]`
3. Semantic claim to prove (NormalizeSound / public endpoint):

```text
FormalSQL outcomes of source_query_expr on EmbedDB(DB)
  ≡
Adapt(Eval_Spec(LogicalSpec_Q21_rel, params_catalog_nation, DB))
```

4. Accelerator proofs remain ∀ `nation_name`, using
   `params_nation(nation_name)`.

This mirrors Q1’s cutoff-param pattern: FormalSQL carries a concrete catalog
shape; LogicalSpec is the reusable template; instantiation is explicit.

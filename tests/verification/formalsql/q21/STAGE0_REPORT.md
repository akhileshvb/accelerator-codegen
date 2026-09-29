# Q21 Stage 0 — Logos / FormalSQL gate

**Status: PASS** (2026-09-28)

## Command

```bash
JAVA_HOME=/usr/lib/jvm/java-21-openjdk scripts/logos_lower_catalog.sh q21
```

## Artifacts

- `catalog_base_sql.sql` sha256 `58f4982b93bd765675f5f9b4f97c9aaa1ee5e4a5f5c5bcd6e147fe915648318a`
- `generated/{Schema,Queries,Witness,WitnessData}.v`
- `Q21Catalog.v`
- `logos_lower_report.json` — TRANSFORM COMPLETE

## Exact lowered shape (from `generated/Queries.v`)

| Feature | Present |
|---------|---------|
| Outer spine | 3× `QExpr_CrossJoin` over supplier / lineitem / orders / nation + filters |
| Correlated EXISTS (l2) | `SExpr_Exists` over `QExpr_Filter(lineitem)` with `PredicateEq` orderkey + `PredicateNeq` suppkey vs outer `__logos_cor_cor0_{7,9}` |
| Correlated NOT EXISTS (l3) | `SExpr_Not (SExpr_Exists …)` with orderkey eq, suppkey neq, and late `PredicateGt` receiptdate/commitdate |
| Nation literal | `CstString … "SAUDI ARABIA"` (catalog instance) |
| GROUP BY / agg | `QExpr_Group` on `s_name`, `CountStar` |
| ORDER BY | `SortDescNullsFirst numwait`, `SortAscNullsLast s_name` |
| LIMIT / Fetch | **Absent** (matches catalog) |
| `QueryJoinSemi` / `QueryJoinAnti` | **Not used** — EXISTS kept as scalar predicates |

## Mapping note for Stage 1+

LogicalSpec will encode EXISTS/NOT EXISTS as `RelSemiJoin` / `RelAntiJoin` via fail-closed `normalize_q21`, justified by FormalSQL/Logos EXISTS↔semi/anti acceptance lemmas. Nation stays `EParam "nation_name"` with catalog `'SAUDI ARABIA'` as an instantiation.

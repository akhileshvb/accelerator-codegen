# FormalSQLLogical — generic RelNode LogicalSpec IR

Query-independent typed relational IR + evaluator.

| Module | Role |
|--------|------|
| `SqlTypes.v` | SqlType, SqlValue (NULL, VAvgExact), Truth3 |
| `Schema.v` | Attr, Schema, Row, Params, DB |
| `Expr.v` | Expr / Pred (InputRef, arithmetic, compare, CASE, …) |
| `Agg.v` | AggSpec (Count*/Count/Sum/Avg/Min/Max) |
| `RelNode.v` | RelNode tree = LogicalSpec |
| `Typing.v` | Catalog, schema inference, WF |
| `Eval.v` | Eval_Spec (no query branches) |
| `proof/Morphisms.v` | Filter/Project/Join/Fold/Agg/Order patterns |
| `QUERY_PROOF_GUIDE.md` | how to instantiate for a new query |
| `PROOF_LIBRARY_INDEX.md` | USE WHEN / REQUIRES / PROVES index |

Q1 encoding: `../q1/Q1LogicalSpecTree.v`, `../q1/Q1Adapt.v`, `../q1/Q1Schema.v`.

Legacy flat IR (proved Q1 path until RelNode bridge Qed): `../resultspec/LogicalSpec.v`.

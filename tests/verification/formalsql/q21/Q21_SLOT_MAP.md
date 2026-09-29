# Q21 RelNode slot / alias map

## Outer spine (after `supplier ⋈ l1 ⋈ orders ⋈ nation`)

| Slot | Alias.column | Binding |
|------|--------------|---------|
| 0 | s_suppkey | supplier |
| 1 | s_name | supplier |
| 2 | s_nationkey | supplier |
| 3 | l1.l_orderkey | outer lineitem |
| 4 | l1.l_suppkey | outer lineitem |
| 5 | l1.l_commitdate | outer lineitem |
| 6 | l1.l_receiptdate | outer lineitem |
| 7 | o_orderkey | orders |
| 8 | o_orderstatus | orders |
| 9 | n_nationkey | nation |
| 10 | n_name | nation |

`outer_width = 11`.

## SemiJoin right (`l2`)

`RelSemiJoin(outer, RelScan(lineitem), cond)` — concatenated layout:

| Slot | Binding |
|------|---------|
| 0..10 | outer (l1 correlations use 3, 4) |
| 11 | **l2**.l_orderkey |
| 12 | **l2**.l_suppkey |
| 13 | l2.l_commitdate |
| 14 | l2.l_receiptdate |

`pred_exists_l2`: `slot3 = slot11 ∧ slot4 ≠ slot12`.

## AntiJoin right (`l3`)

Right input is `RelFilter(RelScan(lineitem), late)` where late uses **local** lineitem slots 2/3 before concat.
After concat into AntiJoin cond:

| Slot | Binding |
|------|---------|
| 0..10 | outer (still l1) |
| 11 | **l3**.l_orderkey |
| 12 | **l3**.l_suppkey |

`pred_not_exists_l3`: `slot3 = slot11 ∧ slot4 ≠ slot12` (late already applied on right).

## Nation parameterization

- LogicalSpec: `EParam "nation_name"` at outer filter vs `n_name` (slot 10)
- Catalog SQL literal `'SAUDI ARABIA'` is an **instantiation** via `params_catalog_nation`
- Accelerator proofs quantify over `nation_name`

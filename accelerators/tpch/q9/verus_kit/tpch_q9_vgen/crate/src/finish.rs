// The last steps of apply: the rollup equation, keys, and small helpers.
use vstd::prelude::*;
use std::sync::Arc;
use verus_common::sums::*;
use verus_common::sqlfns::*;
use crate::gen_spec::*;
use crate::gen_obligations::*;
use crate::meaning::*;
use crate::storage::*;
use crate::values::*;
use crate::rows::*;
use crate::events::*;
use crate::rollup::*;
use crate::applyspec::*;
use crate::applydiff::*;

verus! {

broadcast use vstd::seq_lib::group_seq_properties;

pub proof fn lemma_rollup_step(db: Db, db2: Db, p: Seq<u8>, nk: int, y: int, f: int,
    lines: Seq<LineDelta>, orders: Seq<OrderDelta>, part: Seq<PartDelta>, ps: Seq<PartsuppDelta>, sup: Seq<SupplierDelta>,
    okeys: Seq<i64>, pkeys: Seq<i64>, skeys: Seq<i64>,
    keys_a: Seq<i64>, rows_a: Seq<LineIn>, keys_b: Seq<i64>, rows_b: Seq<LineIn>, keys_c: Seq<i64>, rows_c: Seq<LineIn>)
    requires
        bag_step(db.lineitem, db2.lineitem, line_delta_seq_view(lines)),
        bag_step(db.orders, db2.orders, order_delta_seq_view(orders)),
        bag_step(db.part, db2.part, part_delta_seq_view(part)),
        bag_step(db.partsupp, db2.partsupp, partsupp_delta_seq_view(ps)),
        bag_step(db.supplier, db2.supplier, supplier_delta_seq_view(sup)),
        0 <= f <= 1,
        okeys == okeys_of(orders),
        forall|k: i64| #[trigger] pkeys.contains(k) <==> (part_keys_of(part).contains(k) || ps_keys_of(ps).contains(k)),
        forall|k: i64| #[trigger] skeys.contains(k) <==> (!is_cancel(sup) && sup_keys_of(sup).contains(k)),
        rescan_lines_for_orders_ok(db2, keys_a, rows_a), rescan_lines_for_parts_ok(db2, keys_b, rows_b), rescan_lines_for_suppliers_ok(db2, keys_c, rows_c),
        forall|k: i64| okeys.contains(k) ==> #[trigger] keys_a.contains(k),
        forall|k: i64| pkeys.contains(k) ==> #[trigger] keys_b.contains(k),
        forall|k: i64| skeys.contains(k) ==> #[trigger] keys_c.contains(k),
    ensures nation_total(filter_db(db2, p), nk, y, f) == nation_total(filter_db(db, p), nk, y, f)
        + sum(rows_a, mode_weight(db, db2, p, nk, y, f, 0, okeys, pkeys, skeys))
        + sum(rows_b, mode_weight(db, db2, p, nk, y, f, 1, okeys, pkeys, skeys))
        + sum(rows_c, mode_weight(db, db2, p, nk, y, f, 2, okeys, pkeys, skeys))
        + sum(lines, ldelta_weight(db, p, nk, y, f)),
{
    lemma_tl_bridge(db, p, nk, y, f);
    lemma_tl_bridge(db2, p, nk, y, f);
    let h1 = |l: LineitemRow| hline(db, p, l, nk, y, f);
    let h2 = |l: LineitemRow| hline(db2, p, l, nk, y, f);
    let g = gdiff(db, db2, p, nk, y, f);
    let view = line_delta_seq_view(lines);
    lemma_master(db.lineitem, db2.lineitem, view, h1, h2);
    lemma_sum_ext_all(db2.lineitem, |l: LineitemRow| h2(l) - h1(l), g);
    assert forall|l: LineitemRow| !selr(0, okeys, pkeys, skeys, l) && !selr(1, okeys, pkeys, skeys, l) && !selr(2, okeys, pkeys, skeys, l) implies g(l) == 0 by {
        lemma_line_frame(db, db2, p, l, nk, y, f, orders, part, ps, sup, pkeys, skeys);
    }
    lemma_decompose(db2, okeys, pkeys, skeys, keys_a, rows_a, keys_b, rows_b, keys_c, rows_c, g);
    let signed = |d: (LineitemRow, int)| d.1 * h1(d.0);
    lemma_sum_map(lines, |d: LineDelta| (line_in_view(d.row), d.sign as int), signed, ldelta_weight(db, p, nk, y, f));
}

pub proof fn lemma_keys_fit_step(db: Db, db2: Db, sup: Seq<SupplierDelta>, ndel: Seq<NationDelta>)
    requires database_keys_fit(db),
        bag_step(db.supplier, db2.supplier, supplier_delta_seq_view(sup)),
        bag_step(db.nation, db2.nation, nation_delta_seq_view(ndel)),
    ensures database_keys_fit(db2),
{
    let supplier_fit = |row: SupplierRow| i64::MIN <= row.s_suppkey <= i64::MAX && i64::MIN <= row.s_nationkey <= i64::MAX;
    assert forall|index: int| 0 <= index < sup.len() implies supplier_fit((#[trigger] supplier_delta_seq_view(sup)[index]).0) by {}
    verus_prooflib::lemma_bag_step_preserves_predicate(db.supplier, db2.supplier, supplier_delta_seq_view(sup), supplier_fit);
    let nation_fit = |row: NationRow| i64::MIN <= row.n_nationkey <= i64::MAX;
    assert forall|index: int| 0 <= index < ndel.len() implies nation_fit((#[trigger] nation_delta_seq_view(ndel)[index]).0) by {}
    verus_prooflib::lemma_bag_step_preserves_predicate(db.nation, db2.nation, nation_delta_seq_view(ndel), nation_fit);
}

/// Every needed key is among the rescanned keys.
pub fn covers(keys: &[i64], needed: &Vec<i64>) -> (r: bool)
    ensures r ==> forall|k: i64| needed@.contains(k) ==> #[trigger] keys@.contains(k),
{
    let mut i = 0;
    while i < needed.len()
        invariant i <= needed.len(), forall|j: int| 0 <= j < i ==> keys@.contains(#[trigger] needed@[j]),
        decreases needed.len() - i,
    {
        if !crate::orders::has_key(keys, needed[i]) { return false; }
        i += 1;
    }
    proof {
        assert forall|k: i64| needed@.contains(k) implies #[trigger] keys@.contains(k) by {
            let j = choose|j: int| 0 <= j < needed@.len() && needed@[j] == k;
            assert(keys@.contains(needed@[j]));
        }
    }
    true
}

pub fn empty_delta(n: usize) -> (d: Vec<Vec<RCell>>)
    ensures d.len() == n, forall|c: int, nk: int, y: int, f: int| 0 <= c < n && 0 <= f <= 1 ==> #[trigger] dval(d@, c, nk, y, f) == 0,
{
    let mut d: Vec<Vec<RCell>> = Vec::new();
    let mut c = 0;
    while c < n
        invariant c <= n, d.len() == c, forall|c2: int| 0 <= c2 < c ==> (#[trigger] d@[c2])@.len() == 0,
        decreases n - c,
    { d.push(Vec::new()); c += 1; }
    proof {
        assert forall|c2: int, nk: int, y: int, f: int| 0 <= c2 < n && 0 <= f <= 1 implies #[trigger] dval(d@, c2, nk, y, f) == 0 by {
            lemma_sum_empty(d@[c2]@, |row: RCell| if row.nk as int == nk && row.year as int == y { if f == 0 { row.count as int } else { row.profit as int } } else { 0int });
        }
    }
    d
}

/// base plus the cells of add.
#[verifier::spinoff_prover]
pub fn merge_cells(base: &Vec<RCell>, add: &Vec<RCell>) -> (r: Option<Vec<RCell>>)
    ensures r matches Some(v) ==> forall|nk: int, y: int, f: int| 0 <= f <= 1 ==> #[trigger] cell_at(v@, nk, y, f) == cell_at(base@, nk, y, f) + cell_at(add@, nk, y, f),
{
    let mut v = copy_cells(base);
    let mut i = 0;
    proof {
        assert forall|nk: int, y: int, f: int| 0 <= f <= 1 implies cell_at(v@, nk, y, f) == cell_at(base@, nk, y, f) + cell_at(add@.take(0), nk, y, f) by {
            lemma_sum_empty(add@.take(0), |row: RCell| if row.nk as int == nk && row.year as int == y { if f == 0 { row.count as int } else { row.profit as int } } else { 0int });
        }
    }
    while i < add.len()
        invariant i <= add.len(),
            forall|nk: int, y: int, f: int| 0 <= f <= 1 ==> #[trigger] cell_at(v@, nk, y, f) == cell_at(base@, nk, y, f) + cell_at(add@.take(i as int), nk, y, f),
        decreases add.len() - i,
    {
        let cell = add[i];
        bump_cell(&mut v, cell.nk, cell.year, cell.count, cell.profit)?;
        proof {
            assert forall|nk: int, y: int, f: int| 0 <= f <= 1 implies #[trigger] cell_at(v@, nk, y, f) == cell_at(base@, nk, y, f) + cell_at(add@.take(i as int + 1), nk, y, f) by {
                lemma_sum_take(add@, i as int, |row: RCell| if row.nk as int == nk && row.year as int == y { if f == 0 { row.count as int } else { row.profit as int } } else { 0int });
            }
        }
        i += 1;
    }
    proof { assert(add@.take(i as int) =~= add@); }
    Some(v)
}

}

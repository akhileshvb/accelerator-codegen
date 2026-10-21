// What the host's scans and the envelope say about sizes and ranges of the delivered rows.
use vstd::prelude::*;
use verus_common::sums::*;
use crate::gen_spec::*;
use crate::gen_obligations::*;
use verus_prooflib::lemma_sum_ones;

verus! {

pub proof fn lemma_parts_len(db: Db, rows: Seq<PartInput>)
    requires scan_parts_ok(db, rows),
    ensures rows.len() == db.part.len(),
{
    let ones = |row: PartRow| 1int;
    lemma_sum_ones(part_input_seq_view(rows));
    lemma_sum_ones(db.part);
    assert(sum(part_input_seq_view(rows), ones) == sum(db.part, ones));
}

pub proof fn lemma_nations_len(db: Db, rows: Seq<NationInput>)
    requires scan_nations_ok(db, rows),
    ensures rows.len() == db.nation.len(),
{
    let ones = |row: NationRow| 1int;
    lemma_sum_ones(nation_input_seq_view(rows));
    lemma_sum_ones(db.nation);
    assert(sum(nation_input_seq_view(rows), ones) == sum(db.nation, ones));
}

pub proof fn lemma_regions_len(db: Db, rows: Seq<RegionInput>)
    requires scan_regions_ok(db, rows),
    ensures rows.len() == db.region.len(),
{
    let ones = |row: RegionRow| 1int;
    lemma_sum_ones(region_input_seq_view(rows));
    lemma_sum_ones(db.region);
    assert(sum(region_input_seq_view(rows), ones) == sum(db.region, ones));
}

/// The scanned orders are the ones in the window.
pub proof fn lemma_window_rows(db: Db, rows: Seq<OrderInput>)
    requires scan_window_orders_ok(db, rows),
    ensures forall|i: int| 0 <= i < rows.len() ==> 9131 <= (#[trigger] rows[i]).o_orderdate <= 9861,
{
    let outside = |order: OrdersRow| if order.o_orderdate < 9131 || order.o_orderdate > 9861 { 1int } else { 0int };
    let view = order_input_seq_view(rows);
    let kept = |order: OrdersRow| if order.o_orderdate >= 9131 && order.o_orderdate <= 9861 { outside(order) } else { 0int };
    assert forall|i: int| 0 <= i < db.orders.len() implies kept(#[trigger] db.orders[i]) == 0 by {}
    lemma_sum_zero(db.orders, kept);
    assert(sum(view, outside) == 0);
    assert forall|i: int| 0 <= i < rows.len() implies 9131 <= (#[trigger] rows[i]).o_orderdate <= 9861 by {
        assert forall|j: int| 0 <= j < view.len() implies outside(#[trigger] view[j]) >= 0 by {}
        if !(9131 <= rows[i].o_orderdate <= 9861) {
            assert(view[i] == order_input_view(rows[i]));
            lemma_sum_pos(view, outside, i);
        }
    }
}

}

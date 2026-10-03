use vstd::prelude::*;
use verus_common::sums::*;
use crate::aggregates::*;
use crate::aggregates::some_or_return;
use crate::gen_obligations::*;
use crate::index::*;
use crate::maintenance::*;
use crate::gen_spec::*;
use crate::neighborhood_proofs::*;

verus! {

pub type Summaries = Index<OrderSummary>;

pub open spec fn summary_for_order(index: Summaries, db: Db, order: int) -> bool {
    &&& order_finished(index, (order, Seq::<u8>::empty())) == crate::aggregates::finished(db, order)
    &&& forall|supplier: KeyView| #![trigger order_total(index, (order, Seq::<u8>::empty()), supplier)] #![trigger order_late(index, (order, Seq::<u8>::empty()), supplier)] {
        &&& order_total(index, (order, Seq::<u8>::empty()), supplier) == if supplier.1 == Seq::<u8>::empty() { total_lines(db, order, supplier.0) } else { 0 }
        &&& order_late(index, (order, Seq::<u8>::empty()), supplier) == if supplier.1 == Seq::<u8>::empty() { late_lines(db, order, supplier.0) } else { 0 }
    }
}

pub open spec fn summaries_keys_db(index: Summaries, keys: Counts, db: Db) -> bool {
    forall|order: int| #![trigger keys.at((order, Seq::<u8>::empty()))] keys.at((order, Seq::<u8>::empty())).is_some() ==> summary_for_order(index, db, order)
}

pub proof fn lemma_key_list_view(keys: Seq<i64>, order: int)
    requires key_list_contains(keys, (order, Seq::<u8>::empty())),
    ensures seq_view_i64(keys).contains(order),
{
    let position = choose|position: int| #![trigger keys[position]] 0 <= position < keys.len() && order == keys[position] as int;
    assert(seq_view_i64(keys)[position] == order);
}

#[verifier::rlimit(20)]
pub proof fn lemma_scanned_summary(index: &Summaries, keys: &Counts, db: Db, lines: Seq<OrderLine>, finished_rows: Seq<OrderFinished>, line_keys: Seq<i64>, finished_keys: Seq<i64>, order: int)
    requires
        summaries_scanned(*index, *keys, lines, finished_rows), keys.at((order, Seq::<u8>::empty())).is_some(),
        key_list_contains(line_keys, (order, Seq::<u8>::empty())), key_list_contains(finished_keys, (order, Seq::<u8>::empty())),
        rescan_order_lines_ok(db, line_keys, lines), rescan_order_finished_ok(db, finished_keys, finished_rows),
    ensures summary_for_order(*index, db, order),
{
    lemma_key_list_view(line_keys, order);
    lemma_key_list_view(finished_keys, order);
    let order_key = (order, Seq::<u8>::empty());
    assert forall|supplier: KeyView| #![trigger order_total(*index, order_key, supplier)] #![trigger order_late(*index, order_key, supplier)] {
        &&& order_total(*index, order_key, supplier) == if supplier.1 == Seq::<u8>::empty() { total_lines(db, order, supplier.0) } else { 0 }
        &&& order_late(*index, order_key, supplier) == if supplier.1 == Seq::<u8>::empty() { late_lines(db, order, supplier.0) } else { 0 }
    } by {
        if supplier.1 == Seq::<u8>::empty() {
            let total = total_weight(order, supplier.0);
            let late = late_weight(order, supplier.0);
            lemma_sum_map(lines, |row: OrderLine| order_line_view(row), total, scanned_total(*keys, order_key, supplier));
            lemma_sum_map(lines, |row: OrderLine| order_line_view(row), late, scanned_late(*keys, order_key, supplier));
            let restricted_total = |row: LineitemRow| if seq_view_i64(line_keys).contains(row.l_orderkey) { total(row) } else { 0 };
            let restricted_late = |row: LineitemRow| if seq_view_i64(line_keys).contains(row.l_orderkey) { late(row) } else { 0 };
            assert(sum(order_line_seq_view(lines), total) == sum(db.lineitem, restricted_total));
            assert(sum(order_line_seq_view(lines), late) == sum(db.lineitem, restricted_late));
            lemma_sum_ext(db.lineitem, restricted_total, total);
            lemma_sum_ext(db.lineitem, restricted_late, late);
        } else {
            lemma_sum_zero(lines, scanned_total(*keys, order_key, supplier));
            lemma_sum_zero(lines, scanned_late(*keys, order_key, supplier));
        }
    }
    let indicator = |key: int| if key == order { 1int } else { 0int };
    lemma_sum_map(finished_rows, |row: OrderFinished| order_finished_view(row), indicator, scanned_finished(*keys, order_key));
    let restricted = |row: OrdersRow| if row.o_orderstatus == seq![b'F'] && seq_view_i64(finished_keys).contains(row.o_orderkey) { indicator(row.o_orderkey) } else { 0 };
    assert(sum(order_finished_seq_view(finished_rows), indicator) == sum(db.orders, restricted));
    lemma_sum_ext(db.orders, restricted, |row: OrdersRow| if row.o_orderkey == order && row.o_orderstatus == seq![b'F'] { 1int } else { 0int });
    assert(order_finished(*index, order_key) == sum(finished_rows, scanned_finished(*keys, order_key)));
}

#[verifier::rlimit(20)]
pub proof fn lemma_reconstructed_summary(before: &Summaries, after: &Summaries, db: Db, db2: Db, lines: Seq<LineitemDelta>, orders: Seq<OrdersDelta>, full: Seq<(OrdersRow, int)>, order: int)
    requires summaries_reconstructed(*before, *after, lines, orders), summary_for_order(*after, db2, order),
        bag_step(db.lineitem, db2.lineitem, lineitem_delta_seq_view(lines)), bag_step(db.orders, db2.orders, full), orders_delta_ok(orders, full),
    ensures summary_for_order(*before, db, order),
{
    let order_key = (order, Seq::<u8>::empty());
    assert forall|supplier: KeyView| #![trigger order_total(*before, order_key, supplier)] #![trigger order_late(*before, order_key, supplier)] {
        &&& order_total(*before, order_key, supplier) == if supplier.1 == Seq::<u8>::empty() { total_lines(db, order, supplier.0) } else { 0 }
        &&& order_late(*before, order_key, supplier) == if supplier.1 == Seq::<u8>::empty() { late_lines(db, order, supplier.0) } else { 0 }
    } by {
        if supplier.1 == Seq::<u8>::empty() {
            let total = total_weight(order, supplier.0);
            let late = late_weight(order, supplier.0);
            let total_signed = |delta: (LineitemRow, int)| delta.1 * total(delta.0);
            let late_signed = |delta: (LineitemRow, int)| delta.1 * late(delta.0);
            lemma_sum_map(lines, |delta: LineitemDelta| (lineitem_input_view(delta.row), delta.sign as int), total_signed, delta_total(order_key, supplier));
            lemma_sum_map(lines, |delta: LineitemDelta| (lineitem_input_view(delta.row), delta.sign as int), late_signed, delta_late(order_key, supplier));
            assert(sum(db2.lineitem, total) == sum(db.lineitem, total) + sum(lineitem_delta_seq_view(lines), total_signed));
            assert(sum(db2.lineitem, late) == sum(db.lineitem, late) + sum(lineitem_delta_seq_view(lines), late_signed));
        } else {
            lemma_sum_zero(lines, delta_total(order_key, supplier));
            lemma_sum_zero(lines, delta_late(order_key, supplier));
        }
    }
    let finished = |row: OrdersRow| if row.o_orderkey == order && row.o_orderstatus == seq![b'F'] { 1int } else { 0int };
    let signed = |delta: (OrdersRow, int)| delta.1 * finished(delta.0);
    let indexed = |position: int| signed(full[position]);
    assert forall|position: int| 0 <= position < orders.len() implies indexed(position) == delta_finished(order_key)(#[trigger] orders[position]) by {
        assert(orders[position].o_orderkey as int == full[position].0.o_orderkey);
        assert((orders[position].is_finished != 0) == (full[position].0.o_orderstatus == seq![b'F']));
        assert(orders[position].sign as int == full[position].1);
    }
    lemma_sum_index(orders, delta_finished(order_key), indexed);
    lemma_sum_index(full, signed, indexed);
    assert(sum(db2.orders, finished) == sum(db.orders, finished) + sum(full, signed));
    assert(order_finished(*before, order_key) == order_finished(*after, order_key) - sum(orders, delta_finished(order_key)));
}

pub open spec fn scanned_total(keys: Counts, order: KeyView, supplier: KeyView) -> spec_fn(OrderLine) -> int {
    |row: OrderLine| if keys.at((row.l_orderkey as int, Seq::<u8>::empty())).is_some() && order == (row.l_orderkey as int, Seq::<u8>::empty()) && supplier == (row.l_suppkey as int, Seq::<u8>::empty()) { 1int } else { 0int }
}

pub open spec fn scanned_late(keys: Counts, order: KeyView, supplier: KeyView) -> spec_fn(OrderLine) -> int {
    |row: OrderLine| if row.l_receiptdate > row.l_commitdate { scanned_total(keys, order, supplier)(row) } else { 0 }
}

pub open spec fn scanned_finished(keys: Counts, order: KeyView) -> spec_fn(OrderFinished) -> int {
    |row: OrderFinished| if keys.at((row.o_orderkey as int, Seq::<u8>::empty())).is_some() && order == (row.o_orderkey as int, Seq::<u8>::empty()) { 1int } else { 0int }
}

pub open spec fn summaries_scanned(index: Summaries, keys: Counts, lines: Seq<OrderLine>, finished: Seq<OrderFinished>) -> bool {
    &&& forall|order: KeyView, supplier: KeyView| #![trigger order_total(index, order, supplier)] #![trigger order_late(index, order, supplier)] {
        &&& order_total(index, order, supplier) == sum(lines, scanned_total(keys, order, supplier))
        &&& order_late(index, order, supplier) == sum(lines, scanned_late(keys, order, supplier))
    }
    &&& forall|order: KeyView| #![trigger order_finished(index, order)] order_finished(index, order) == sum(finished, scanned_finished(keys, order))
}

pub fn summarize(keys: &Counts, lines: &[OrderLine], finished: &[OrderFinished]) -> (result: Option<Summaries>)
    ensures result matches Some(index) ==> index.wf() && summaries_scanned(index, *keys, lines@, finished@),
{
    hide(Tree::at);
    hide(Tree::entries);
    hide(Tree::wf);
    let mut index: Summaries = Summaries::new();
    let mut position = 0usize;
    while position < lines.len()
        invariant position <= lines.len(), index.wf(),
            forall|order: KeyView, supplier: KeyView| #![trigger order_total(index, order, supplier)] #![trigger order_late(index, order, supplier)] {
                &&& order_total(index, order, supplier) == sum(lines@.take(position as int), scanned_total(*keys, order, supplier))
                &&& order_late(index, order, supplier) == sum(lines@.take(position as int), scanned_late(*keys, order, supplier))
            },
            forall|order: KeyView| #![trigger order_finished(index, order)] order_finished(index, order) == 0,
        decreases lines.len() - position,
    {
        let row = &lines[position];
        if keys.get(&Key::number(row.l_orderkey)).is_some() {
            let late = if row.l_receiptdate > row.l_commitdate { 1i128 } else { 0i128 };
            some_or_return!(add_line(&mut index, row.l_orderkey, row.l_suppkey, 1, late));
        }
        proof {
            assert forall|order: KeyView, supplier: KeyView| #![trigger order_total(index, order, supplier)] #![trigger order_late(index, order, supplier)] {
                &&& order_total(index, order, supplier) == sum(lines@.take(position as int + 1), scanned_total(*keys, order, supplier))
                &&& order_late(index, order, supplier) == sum(lines@.take(position as int + 1), scanned_late(*keys, order, supplier))
            } by {
                lemma_sum_take(lines@, position as int, scanned_total(*keys, order, supplier));
                lemma_sum_take(lines@, position as int, scanned_late(*keys, order, supplier));
            }
        }
        position += 1;
    }
    proof { assert(lines@.take(position as int) =~= lines@); }
    position = 0;
    while position < finished.len()
        invariant position <= finished.len(), index.wf(),
            forall|order: KeyView, supplier: KeyView| #![trigger order_total(index, order, supplier)] #![trigger order_late(index, order, supplier)] {
                &&& order_total(index, order, supplier) == sum(lines@, scanned_total(*keys, order, supplier))
                &&& order_late(index, order, supplier) == sum(lines@, scanned_late(*keys, order, supplier))
            },
            forall|order: KeyView| #![trigger order_finished(index, order)] order_finished(index, order) == sum(finished@.take(position as int), scanned_finished(*keys, order)),
        decreases finished.len() - position,
    {
        let row = &finished[position];
        if keys.get(&Key::number(row.o_orderkey)).is_some() {
            some_or_return!(add_finished(&mut index, row.o_orderkey, 1));
        }
        proof {
            assert forall|order: KeyView| #![trigger order_finished(index, order)] order_finished(index, order) == sum(finished@.take(position as int + 1), scanned_finished(*keys, order)) by {
                lemma_sum_take(finished@, position as int, scanned_finished(*keys, order));
            }
        }
        position += 1;
    }
    proof { assert(finished@.take(position as int) =~= finished@); }
    Some(index)
}

pub open spec fn delta_total(order: KeyView, supplier: KeyView) -> spec_fn(LineitemDelta) -> int {
    |delta: LineitemDelta| if order == (delta.row.l_orderkey as int, Seq::<u8>::empty()) && supplier == (delta.row.l_suppkey as int, Seq::<u8>::empty()) { delta.sign as int } else { 0 }
}

pub open spec fn delta_late(order: KeyView, supplier: KeyView) -> spec_fn(LineitemDelta) -> int {
    |delta: LineitemDelta| if delta.row.l_receiptdate > delta.row.l_commitdate { delta_total(order, supplier)(delta) } else { 0 }
}

pub open spec fn delta_finished(order: KeyView) -> spec_fn(OrdersDelta) -> int {
    |delta: OrdersDelta| if delta.is_finished != 0 && order == (delta.o_orderkey as int, Seq::<u8>::empty()) { delta.sign as int } else { 0 }
}

pub open spec fn summaries_reconstructed(before: Summaries, after: Summaries, lines: Seq<LineitemDelta>, orders: Seq<OrdersDelta>) -> bool {
    &&& forall|order: KeyView, supplier: KeyView| #![trigger order_total(before, order, supplier)] #![trigger order_late(before, order, supplier)] {
        &&& order_total(before, order, supplier) == order_total(after, order, supplier) - sum(lines, delta_total(order, supplier))
        &&& order_late(before, order, supplier) == order_late(after, order, supplier) - sum(lines, delta_late(order, supplier))
    }
    &&& forall|order: KeyView| #![trigger order_finished(before, order)] order_finished(before, order) == order_finished(after, order) - sum(orders, delta_finished(order))
}

pub fn reconstruct(after: &Summaries, lines: &[LineitemDelta], orders: &[OrdersDelta]) -> (result: Option<Summaries>)
    ensures result matches Some(before) ==> before.wf() && summaries_reconstructed(before, *after, lines@, orders@),
{
    hide(Tree::at);
    hide(Tree::entries);
    hide(Tree::wf);
    let mut before = after.share();
    let mut position = 0usize;
    while position < lines.len()
        invariant position <= lines.len(), before.wf(),
            forall|order: KeyView, supplier: KeyView| #![trigger order_total(before, order, supplier)] #![trigger order_late(before, order, supplier)] {
                &&& order_total(before, order, supplier) == order_total(*after, order, supplier) - sum(lines@.take(position as int), delta_total(order, supplier))
                &&& order_late(before, order, supplier) == order_late(*after, order, supplier) - sum(lines@.take(position as int), delta_late(order, supplier))
            },
            forall|order: KeyView| #![trigger order_finished(before, order)] order_finished(before, order) == order_finished(*after, order),
        decreases lines.len() - position,
    {
        let delta = &lines[position];
        let change = -(delta.sign as i128);
        let late = if delta.row.l_receiptdate > delta.row.l_commitdate { change } else { 0 };
        some_or_return!(add_line(&mut before, delta.row.l_orderkey, delta.row.l_suppkey, change, late));
        proof {
            assert forall|order: KeyView, supplier: KeyView| #![trigger order_total(before, order, supplier)] #![trigger order_late(before, order, supplier)] {
                &&& order_total(before, order, supplier) == order_total(*after, order, supplier) - sum(lines@.take(position as int + 1), delta_total(order, supplier))
                &&& order_late(before, order, supplier) == order_late(*after, order, supplier) - sum(lines@.take(position as int + 1), delta_late(order, supplier))
            } by {
                lemma_sum_take(lines@, position as int, delta_total(order, supplier));
                lemma_sum_take(lines@, position as int, delta_late(order, supplier));
            }
        }
        position += 1;
    }
    proof { assert(lines@.take(position as int) =~= lines@); }
    position = 0;
    while position < orders.len()
        invariant position <= orders.len(), before.wf(),
            forall|order: KeyView, supplier: KeyView| #![trigger order_total(before, order, supplier)] #![trigger order_late(before, order, supplier)] {
                &&& order_total(before, order, supplier) == order_total(*after, order, supplier) - sum(lines@, delta_total(order, supplier))
                &&& order_late(before, order, supplier) == order_late(*after, order, supplier) - sum(lines@, delta_late(order, supplier))
            },
            forall|order: KeyView| #![trigger order_finished(before, order)] order_finished(before, order) == order_finished(*after, order) - sum(orders@.take(position as int), delta_finished(order)),
        decreases orders.len() - position,
    {
        let delta = &orders[position];
        if delta.is_finished != 0 { some_or_return!(add_finished(&mut before, delta.o_orderkey, -(delta.sign as i128))); }
        proof {
            assert forall|order: KeyView| #![trigger order_finished(before, order)] order_finished(before, order) == order_finished(*after, order) - sum(orders@.take(position as int + 1), delta_finished(order)) by {
                lemma_sum_take(orders@, position as int, delta_finished(order));
            }
        }
        position += 1;
    }
    proof { assert(orders@.take(position as int) =~= orders@); }
    Some(before)
}

}

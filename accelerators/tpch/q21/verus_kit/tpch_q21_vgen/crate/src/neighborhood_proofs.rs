use vstd::prelude::*;
use verus_common::sums::*;
use verus_prooflib::wide::Wide;
use crate::gen_spec::*;
use crate::aggregates::*;
use crate::index::*;
use crate::maintenance::*;
use crate::gen_obligations::*;

verus! {

pub proof fn lemma_indicator_exists<Row>(rows: Seq<Row>, predicate: spec_fn(Row) -> bool)
    ensures
        sum(rows, |row: Row| if predicate(row) { 1int } else { 0int }) >= 0,
        (sum(rows, |row: Row| if predicate(row) { 1int } else { 0int }) > 0) == (exists|position: int| 0 <= position < rows.len() && predicate(#[trigger] rows[position])),
{
    let weight = |row: Row| if predicate(row) { 1int } else { 0int };
    lemma_sum_nonneg(rows, weight);
    if exists|position: int| 0 <= position < rows.len() && predicate(#[trigger] rows[position]) {
        let position = choose|position: int| 0 <= position < rows.len() && predicate(#[trigger] rows[position]);
        lemma_sum_ge_term(rows, weight, position);
    }
    if sum(rows, weight) > 0 {
        let position = lemma_sum_nonzero_witness(rows, weight);
        assert(predicate(rows[position]));
    }
}

pub open spec fn total_weight(order: int, supplier: int) -> spec_fn(LineitemRow) -> int {
    |row: LineitemRow| if row.l_orderkey == order && row.l_suppkey == supplier { 1int } else { 0int }
}

pub open spec fn late_weight(order: int, supplier: int) -> spec_fn(LineitemRow) -> int {
    |row: LineitemRow| if row.l_orderkey == order && row.l_suppkey == supplier && row.l_receiptdate > row.l_commitdate { 1int } else { 0int }
}

pub open spec fn total_lines(db: Db, order: int, supplier: int) -> int { sum(db.lineitem, total_weight(order, supplier)) }
pub open spec fn late_lines(db: Db, order: int, supplier: int) -> int { sum(db.lineitem, late_weight(order, supplier)) }

pub open spec fn other_total(db: Db, order: int, supplier: int) -> int {
    sum(db.lineitem, |row: LineitemRow| if row.l_orderkey == order && row.l_suppkey != supplier { 1int } else { 0int })
}

pub open spec fn other_late(db: Db, order: int, supplier: int) -> int {
    sum(db.lineitem, |row: LineitemRow| if row.l_orderkey == order && row.l_suppkey != supplier && row.l_receiptdate > row.l_commitdate { 1int } else { 0int })
}

pub open spec fn neighborhood_eligible(db: Db, order: int, supplier: int) -> bool {
    other_total(db, order, supplier) > 0 && other_late(db, order, supplier) == 0
}

pub open spec fn neighborhood_waiting(db: Db, order: int, supplier: int) -> int {
    if neighborhood_eligible(db, order, supplier) { finished(db, order) * late_lines(db, order, supplier) } else { 0 }
}

pub proof fn lemma_neighborhood_exists(db: Db, line: LineitemRow)
    ensures
        exists_1(db, line) == (other_total(db, line.l_orderkey, line.l_suppkey) > 0),
        !exists_2(db, line) == (other_late(db, line.l_orderkey, line.l_suppkey) == 0),
{
    let total_predicate = |row: LineitemRow| exists_1_where(line, row);
    let late_predicate = |row: LineitemRow| exists_2_where(line, row);
    lemma_indicator_exists(db.lineitem, total_predicate);
    lemma_indicator_exists(db.lineitem, late_predicate);
    lemma_sum_ext(db.lineitem, |row: LineitemRow| if total_predicate(row) { 1int } else { 0int },
        |row: LineitemRow| if row.l_orderkey == line.l_orderkey && row.l_suppkey != line.l_suppkey { 1int } else { 0int });
    lemma_sum_ext(db.lineitem, |row: LineitemRow| if late_predicate(row) { 1int } else { 0int },
        |row: LineitemRow| if row.l_orderkey == line.l_orderkey && row.l_suppkey != line.l_suppkey && row.l_receiptdate > row.l_commitdate { 1int } else { 0int });
}

pub open spec fn waiting_line_weight(db: Db, supplier: int) -> spec_fn(LineitemRow) -> int {
    |line: LineitemRow| if line.l_suppkey == supplier && line.l_receiptdate > line.l_commitdate && exists_1(db, line) && !exists_2(db, line) { finished(db, line.l_orderkey) } else { 0 }
}

pub proof fn lemma_neighborhood_waiting(db: Db, order: int, supplier: int)
    ensures neighborhood_waiting(db, order, supplier) == sum(db.lineitem, |line: LineitemRow|
        if line.l_orderkey == order { waiting_line_weight(db, supplier)(line) } else { 0 }),
{
    let actual = |line: LineitemRow| if line.l_orderkey == order { waiting_line_weight(db, supplier)(line) } else { 0 };
    let weight = if neighborhood_eligible(db, order, supplier) { finished(db, order) } else { 0int };
    let scaled = |line: LineitemRow| late_weight(order, supplier)(line) * weight;
    assert forall|position: int| #![trigger db.lineitem[position]] 0 <= position < db.lineitem.len() implies actual(db.lineitem[position]) == scaled(db.lineitem[position]) by {
        lemma_neighborhood_exists(db, db.lineitem[position]);
    }
    lemma_sum_ext(db.lineitem, actual, scaled);
    lemma_sum_scale(db.lineitem, late_weight(order, supplier), scaled, weight);
    assert(late_lines(db, order, supplier) * weight == weight * late_lines(db, order, supplier)) by(nonlinear_arith);
}

pub open spec fn delta_keys_covered(keys: Counts, lines: Seq<LineitemDelta>, orders: Seq<OrdersDelta>) -> bool {
    &&& forall|position: int| 0 <= position < lines.len() ==> keys.at(((#[trigger] lines[position]).row.l_orderkey as int, Seq::<u8>::empty())).is_some()
    &&& forall|position: int| 0 <= position < orders.len() && (#[trigger] orders[position]).is_finished != 0 ==> keys.at((orders[position].o_orderkey as int, Seq::<u8>::empty())).is_some()
}

pub proof fn lemma_untouched_neighborhood(db: Db, db2: Db, keys: &Counts, lines: Seq<LineitemDelta>, orders: Seq<OrdersDelta>, full: Seq<(OrdersRow, int)>, order: int, supplier: int)
    requires
        bag_step(db.lineitem, db2.lineitem, lineitem_delta_seq_view(lines)),
        bag_step(db.orders, db2.orders, full), orders_delta_ok(orders, full),
        delta_keys_covered(*keys, lines, orders), keys.at((order, Seq::<u8>::empty())).is_none(),
    ensures
        other_total(db2, order, supplier) == other_total(db, order, supplier),
        other_late(db2, order, supplier) == other_late(db, order, supplier),
        finished(db2, order) == finished(db, order),
{
    let total_weight = |row: LineitemRow| if row.l_orderkey == order && row.l_suppkey != supplier { 1int } else { 0int };
    let late_weight = |row: LineitemRow| if row.l_orderkey == order && row.l_suppkey != supplier && row.l_receiptdate > row.l_commitdate { 1int } else { 0int };
    let finished_weight = |row: OrdersRow| if row.o_orderkey == order && row.o_orderstatus == seq![b'F'] { 1int } else { 0int };
    let total_signed = |delta: (LineitemRow, int)| delta.1 * total_weight(delta.0);
    let late_signed = |delta: (LineitemRow, int)| delta.1 * late_weight(delta.0);
    let finished_signed = |delta: (OrdersRow, int)| delta.1 * finished_weight(delta.0);
    assert forall|position: int| #![trigger lineitem_delta_seq_view(lines)[position]] 0 <= position < lineitem_delta_seq_view(lines).len() implies
        total_signed(lineitem_delta_seq_view(lines)[position]) == 0 && late_signed(lineitem_delta_seq_view(lines)[position]) == 0 by {
        assert(keys.at((lines[position].row.l_orderkey as int, Seq::<u8>::empty())).is_some());
        assert(lines[position].row.l_orderkey as int != order);
    }
    assert forall|position: int| #![trigger full[position]] 0 <= position < full.len() implies finished_signed(full[position]) == 0 by {
        assert(orders[position].o_orderkey as int == full[position].0.o_orderkey);
        if orders[position].is_finished != 0 { assert(keys.at((orders[position].o_orderkey as int, Seq::<u8>::empty())).is_some()); }
    }
    lemma_sum_zero(lineitem_delta_seq_view(lines), total_signed);
    lemma_sum_zero(lineitem_delta_seq_view(lines), late_signed);
    lemma_sum_zero(full, finished_signed);
    assert(sum(db2.lineitem, total_weight) == sum(db.lineitem, total_weight) + sum(lineitem_delta_seq_view(lines), total_signed));
    assert(sum(db2.lineitem, late_weight) == sum(db.lineitem, late_weight) + sum(lineitem_delta_seq_view(lines), late_signed));
    assert(sum(db2.orders, finished_weight) == sum(db.orders, finished_weight) + sum(full, finished_signed));
}

pub open spec fn inside_weight(db: Db, keys: Counts, supplier: int) -> spec_fn(LineitemRow) -> int {
    |line: LineitemRow| if keys.at((line.l_orderkey, Seq::<u8>::empty())).is_some() { waiting_line_weight(db, supplier)(line) } else { 0 }
}

pub open spec fn outside_weight(db: Db, keys: Counts, supplier: int) -> spec_fn(LineitemRow) -> int {
    |line: LineitemRow| if keys.at((line.l_orderkey, Seq::<u8>::empty())).is_none() { waiting_line_weight(db, supplier)(line) } else { 0 }
}

pub open spec fn in_group_weight(db: Db, keys: Counts, supplier: int, key: KeyView) -> spec_fn(LineitemRow) -> int {
    |line: LineitemRow| if (line.l_orderkey, Seq::<u8>::empty()) == key { inside_weight(db, keys, supplier)(line) } else { 0 }
}

#[verifier::rlimit(20)]
pub proof fn lemma_waiting_split(db: Db, keys: &Counts, supplier: int)
    requires keys.wf(),
        forall|position: int| 0 <= position < keys.entries().len() ==> (#[trigger] keys.entries()[position]).0.1 == Seq::<u8>::empty(),
    ensures waiting(db, supplier) == sum(keys.entries(), |row: (KeyView, Wide)| neighborhood_waiting(db, row.0.0, supplier)) + sum(db.lineitem, outside_weight(db, *keys, supplier)),
{
    hide(Tree::at);
    hide(Tree::entries);
    hide(Tree::wf);
    keys.model();
    let key_rows = keys.entries().map_values(|row: (KeyView, Wide)| row.0);
    let key_of = |line: LineitemRow| (line.l_orderkey, Seq::<u8>::empty());
    let inside = inside_weight(db, *keys, supplier);
    let outside = outside_weight(db, *keys, supplier);
    let original = waiting_line_weight(db, supplier);
    let grouped = |key: KeyView| sum(db.lineitem, in_group_weight(db, *keys, supplier, key));
    assert(distinct(key_rows)) by {
        assert forall|first: int, second: int| 0 <= first < key_rows.len() && 0 <= second < key_rows.len() && first != second implies key_rows[first] != key_rows[second] by {
            lemma_key_order(key_rows[first], key_rows[second]);
            if first < second { assert(key_lt(key_rows[first], key_rows[second])); }
            else { assert(key_lt(key_rows[second], key_rows[first])); }
        }
    }
    assert forall|position: int| 0 <= position < db.lineitem.len() && inside(db.lineitem[position]) != 0 implies key_rows.contains(key_of(#[trigger] db.lineitem[position])) by {
        let key = key_of(db.lineitem[position]);
        assert(keys.at(key).is_some());
        let source = choose|source: int| #![auto] 0 <= source < keys.entries().len() && keys.entries()[source].0 == key;
        assert(key_rows[source] == key);
    }
    assert forall|key: KeyView| #[trigger] grouped(key) == sum(db.lineitem, |line: LineitemRow| if key_of(line) == key { inside(line) } else { 0 }) by {
        lemma_sum_ext(db.lineitem, in_group_weight(db, *keys, supplier, key), |line: LineitemRow| if key_of(line) == key { inside(line) } else { 0 });
    }
    lemma_sum_group(db.lineitem, key_rows, key_of, inside, grouped);
    let from_entries = |row: (KeyView, Wide)| grouped(row.0);
    lemma_sum_map(keys.entries(), |row: (KeyView, Wide)| row.0, grouped, from_entries);
    let contributions = |row: (KeyView, Wide)| neighborhood_waiting(db, row.0.0, supplier);
    assert forall|position: int| #![trigger keys.entries()[position]] 0 <= position < keys.entries().len() implies from_entries(keys.entries()[position]) == contributions(keys.entries()[position]) by {
        let key = keys.entries()[position].0;
        assert(key.1 == Seq::<u8>::empty());
        assert(keys.at(key).is_some());
        lemma_neighborhood_waiting(db, key.0, supplier);
        assert(from_entries(keys.entries()[position]) == sum(db.lineitem, in_group_weight(db, *keys, supplier, key)));
        assert(neighborhood_waiting(db, key.0, supplier) == sum(db.lineitem, |line: LineitemRow| if line.l_orderkey == key.0 { waiting_line_weight(db, supplier)(line) } else { 0 }));
        lemma_sum_ext(db.lineitem, in_group_weight(db, *keys, supplier, key),
            |line: LineitemRow| if line.l_orderkey == key.0 { waiting_line_weight(db, supplier)(line) } else { 0 });
    }
    lemma_sum_ext(keys.entries(), from_entries, contributions);
    lemma_sum_add(db.lineitem, inside, outside, original);
}

#[verifier::rlimit(20)]
pub proof fn lemma_waiting_delta(db: Db, db2: Db, keys: &Counts, lines: Seq<LineitemDelta>, orders: Seq<OrdersDelta>, full: Seq<(OrdersRow, int)>, supplier: int)
    requires keys.wf(),
        forall|position: int| 0 <= position < keys.entries().len() ==> (#[trigger] keys.entries()[position]).0.1 == Seq::<u8>::empty(),
        bag_step(db.lineitem, db2.lineitem, lineitem_delta_seq_view(lines)),
        bag_step(db.orders, db2.orders, full), orders_delta_ok(orders, full), delta_keys_covered(*keys, lines, orders),
    ensures waiting(db2, supplier) - waiting(db, supplier) == sum(keys.entries(), |row: (KeyView, Wide)| neighborhood_waiting(db2, row.0.0, supplier) - neighborhood_waiting(db, row.0.0, supplier)),
{
    let before = outside_weight(db, *keys, supplier);
    let after = outside_weight(db2, *keys, supplier);
    assert forall|position: int| #![trigger db2.lineitem[position]] 0 <= position < db2.lineitem.len() implies after(db2.lineitem[position]) == before(db2.lineitem[position]) by {
        let line = db2.lineitem[position];
        if keys.at((line.l_orderkey, Seq::<u8>::empty())).is_none() {
            lemma_untouched_neighborhood(db, db2, keys, lines, orders, full, line.l_orderkey, line.l_suppkey);
            lemma_neighborhood_exists(db, line);
            lemma_neighborhood_exists(db2, line);
        }
    }
    lemma_sum_ext(db2.lineitem, after, before);
    let signed = |delta: (LineitemRow, int)| delta.1 * before(delta.0);
    assert forall|position: int| #![trigger lineitem_delta_seq_view(lines)[position]] 0 <= position < lineitem_delta_seq_view(lines).len() implies signed(lineitem_delta_seq_view(lines)[position]) == 0 by {
        assert(keys.at((lines[position].row.l_orderkey as int, Seq::<u8>::empty())).is_some());
    }
    lemma_sum_zero(lineitem_delta_seq_view(lines), signed);
    assert(sum(db2.lineitem, before) == sum(db.lineitem, before) + sum(lineitem_delta_seq_view(lines), signed));
    lemma_waiting_split(db, keys, supplier);
    lemma_waiting_split(db2, keys, supplier);
    let old_weight = |row: (KeyView, Wide)| neighborhood_waiting(db, row.0.0, supplier);
    let new_weight = |row: (KeyView, Wide)| neighborhood_waiting(db2, row.0.0, supplier);
    let difference = |row: (KeyView, Wide)| neighborhood_waiting(db2, row.0.0, supplier) - neighborhood_waiting(db, row.0.0, supplier);
    lemma_sum_sub(keys.entries(), new_weight, old_weight, difference);
}

}

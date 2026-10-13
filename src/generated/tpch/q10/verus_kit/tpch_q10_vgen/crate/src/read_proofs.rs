use vstd::prelude::*;
use verus_common::sums::*;
use verus_prooflib::ordered::{Index, KeyView, key_lt};
use crate::facts::*;
use crate::paged::*;
use crate::ents::*;
use crate::values::*;
use crate::calendar::*;
use crate::model::*;
use crate::gen_spec::*;
use crate::query_semantics::*;

verus! {

pub open spec fn slice_entry_weight(customer: int, component: int) -> spec_fn((KeyView, Slice)) -> int {
    |entry: (KeyView, Slice)| cell_at(entry.1.cells, customer).component(component)
}

pub open spec fn slice_range_weight(lower: int, upper: int, customer: int, component: int)
    -> spec_fn((KeyView, Slice)) -> int
{
    |entry: (KeyView, Slice)| if lower <= entry.0.0 < upper {
        cell_at(entry.1.cells, customer).component(component)
    } else { 0int }
}

pub open spec fn order_fact_weight(db: Db, customer: int, component: int) -> spec_fn(OrdersRow) -> int {
    |order: OrdersRow| if order.o_custkey == customer {
        sum(db.lineitem, |line: LineitemRow| if line.l_orderkey == order.o_orderkey { line_weight(line, component) } else { 0int })
    } else { 0int }
}

pub open spec fn fact_range_weight(db: Db, grain: int, lower: int, upper: int, customer: int, component: int)
    -> spec_fn(OrdersRow) -> int
{
    |order: OrdersRow| if lower <= period_of(order.o_orderdate, grain) < upper {
        order_fact_weight(db, customer, component)(order)
    } else { 0int }
}

#[verifier::opaque]
pub open spec fn fact_range(db: Db, grain: int, lower: int, upper: int, customer: int, component: int) -> int {
    sum(db.orders, fact_range_weight(db, grain, lower, upper, customer, component))
}

pub proof fn lemma_range_facts(db: Db, slices: &Index<Slice>, grain: int,
    lower: int, upper: int, customer: int, component: int)
    requires slices.wf(), facts_valid(db, *slices, grain),
    ensures sum(slices.entries(), slice_range_weight(lower, upper, customer, component))
        == fact_range(db, grain, lower, upper, customer, component),
{
    hide(period_of);
    hide(month_of);
    hide(line_weight);
    hide(Slice::valid);
    reveal(facts_valid);
    slices.model();
    let entries = slices.entries();
    let entry_class = |entry: (KeyView, Slice)| entry.0.0;
    let order_class = |order: OrdersRow| period_of(order.o_orderdate, grain);
    let entry_count = slice_entry_weight(customer, component);
    let order_count = order_fact_weight(db, customer, component);
    let weight = |key: int| if lower <= key < upper { 1int } else { 0int };
    assert forall|key: int| #[trigger] verus_prooflib::class_total(entries, entry_class, entry_count, key)
        == verus_prooflib::class_total(db.orders, order_class, order_count, key) by {
        verus_prooflib::ordered::lemma_key_contribution(slices, (key, Seq::empty()), entry_count);
        assert forall|position: int| #![trigger entries[position]] 0 <= position < entries.len()
            implies entries[position].0.1 == Seq::<u8>::empty() by {
            assert(slices.at(entries[position].0).is_some());
        }
        lemma_sum_ext(entries,
            |entry: (KeyView, Slice)| if entry_class(entry) == key { entry_count(entry) } else { 0int },
            |entry: (KeyView, Slice)| if entry.0 == (key, Seq::<u8>::empty()) { entry_count(entry) } else { 0int });
        assert(verus_prooflib::class_total(entries, entry_class, entry_count, key)
            == slice_at(*slices, key, customer).component(component));
        assert(slice_at(*slices, key, customer).component(component) == fact_sum(db, key, customer, grain, component));
        reveal(fact_sum);
        lemma_sum_ext_all(db.orders,
            |order: OrdersRow| if order_class(order) == key { order_count(order) } else { 0int },
            |order: OrdersRow| if order.o_custkey == customer && period_of(order.o_orderdate, grain) == key {
                sum(db.lineitem, |line: LineitemRow| if line.l_orderkey == order.o_orderkey { line_weight(line, component) } else { 0int })
            } else { 0int });
        assert(verus_prooflib::class_total(db.orders, order_class, order_count, key)
            == fact_sum(db, key, customer, grain, component));
    }
    verus_prooflib::lemma_equal_classes(entries, db.orders, entry_class, order_class, entry_count, order_count, weight);
    lemma_sum_ext_all(entries, |entry: (KeyView, Slice)| entry_count(entry) * weight(entry_class(entry)),
        slice_range_weight(lower, upper, customer, component));
    lemma_sum_ext_all(db.orders, |order: OrdersRow| order_count(order) * weight(order_class(order)),
        fact_range_weight(db, grain, lower, upper, customer, component));
    reveal(fact_range);
}

pub proof fn lemma_range_split(db: Db, grain: int, lower: int, middle: int, upper: int, customer: int, component: int)
    requires lower <= middle <= upper,
    ensures fact_range(db, grain, lower, upper, customer, component)
        == fact_range(db, grain, lower, middle, customer, component)
            + fact_range(db, grain, middle, upper, customer, component),
{
    let left = fact_range_weight(db, grain, lower, middle, customer, component);
    let right = fact_range_weight(db, grain, middle, upper, customer, component);
    let full = fact_range_weight(db, grain, lower, upper, customer, component);
    lemma_sum_add(db.orders, left, right, full);
    reveal(fact_range);
}

pub proof fn lemma_range_coarsen(db: Db, grain: int, lower: int, upper: int, customer: int, component: int)
    requires grain == 1 || grain == 2,
    ensures fact_range(db, grain, lower * (if grain == 1 { 3int } else { 4int }),
            upper * (if grain == 1 { 3int } else { 4int }), customer, component)
        == fact_range(db, grain + 1, lower, upper, customer, component),
{
    let divisor = if grain == 1 { 3int } else { 4int };
    let fine = fact_range_weight(db, grain, lower * divisor, upper * divisor, customer, component);
    let coarse = fact_range_weight(db, grain + 1, lower, upper, customer, component);
    assert forall|order: OrdersRow| #[trigger] fine(order) == coarse(order) by {
        let month = month_of(order.o_orderdate);
        assert((month / 3) / 4 == month / 12);
        assert((lower * divisor <= period_of(order.o_orderdate, grain) < upper * divisor)
            == (lower <= period_of(order.o_orderdate, grain + 1) < upper));
    }
    lemma_sum_ext_all(db.orders, fine, coarse);
    reveal(fact_range);
}

pub proof fn lemma_month_range(db: Db, lower: int, upper: int, customer: int, component: int)
    requires lower <= upper,
    ensures fact_range(db, 1, lower, upper, customer, component)
        == fact_range(db, 0, month_start(lower), month_start(upper), customer, component),
{
    let months = fact_range_weight(db, 1, lower, upper, customer, component);
    let days = fact_range_weight(db, 0, month_start(lower), month_start(upper), customer, component);
    assert forall|order: OrdersRow| #[trigger] months(order) == days(order) by {
        crate::calendar_proofs::lemma_month_window(order.o_orderdate, lower, upper);
    }
    lemma_sum_ext_all(db.orders, months, days);
    reveal(fact_range);
}

/// A window of three whole months is one slice of the quarters shifted by `grain`'s offset.
pub proof fn lemma_shifted_quarter(db: Db, grain: int, month: int, quarter: int, customer: int, component: int)
    requires (grain == 2 && 3 * quarter == month) || (grain == 4 && 3 * quarter == month + 1) || (grain == 5 && 3 * quarter == month + 2),
    ensures fact_range(db, 1, month, month + 3, customer, component) == fact_range(db, grain, quarter, quarter + 1, customer, component),
{
    let months = fact_range_weight(db, 1, month, month + 3, customer, component);
    let quarters = fact_range_weight(db, grain, quarter, quarter + 1, customer, component);
    assert forall|order: OrdersRow| #[trigger] months(order) == quarters(order) by {
        let value = month_of(order.o_orderdate);
        assert(period_of(order.o_orderdate, grain) == (value + (if grain == 2 { 0int } else if grain == 4 { 1int } else { 2int })) / 3);
        assert((month <= value < month + 3) == (quarter <= period_of(order.o_orderdate, grain) < quarter + 1));
    }
    lemma_sum_ext_all(db.orders, months, quarters);
    reveal(fact_range);
}

pub proof fn lemma_window_range(db: Db, parameters: Params, customer: int, component: int)
    ensures fact_range(db, 0, parameters.p1, parameters.p2, customer, component)
        == window_fact(db, parameters, customer, component),
{
    lemma_sum_ext_all(db.orders, fact_range_weight(db, 0, parameters.p1, parameters.p2, customer, component),
        order_window_weight(db, parameters, customer, component));
    reveal(fact_range);
    reveal(window_fact);
}

pub proof fn lemma_range_tail_zero(rows: Seq<(KeyView, Slice)>, processed: int,
    lower: int, upper: int, customer: int, component: int)
    requires 0 <= processed <= rows.len(),
        forall|position: int| #![trigger rows[position]] processed <= position < rows.len() ==> rows[position].0.0 >= upper,
    ensures sum(rows.take(processed), slice_range_weight(lower, upper, customer, component))
        == sum(rows, slice_range_weight(lower, upper, customer, component)),
{
    let weight = slice_range_weight(lower, upper, customer, component);
    assert forall|position: int| #![trigger rows.skip(processed)[position]]
        0 <= position < rows.skip(processed).len() implies weight(rows.skip(processed)[position]) == 0 by {
        assert(rows.skip(processed)[position] == rows[processed + position]);
    }
    lemma_sum_zero(rows.skip(processed), weight);
    assert(rows =~= rows.take(processed) + rows.skip(processed));
    lemma_sum_concat(rows.take(processed), rows.skip(processed), weight);
}

pub proof fn lemma_fact_cover(db: Db, grain: int, lower: int, first: int, last: int, upper: int,
    customer: int, component: int)
    requires grain == 1 || grain == 2,
        lower <= first * (if grain == 1 { 3int } else { 4int })
            <= last * (if grain == 1 { 3int } else { 4int }) <= upper,
    ensures fact_range(db, grain, lower, upper, customer, component)
        == fact_range(db, grain, lower, first * (if grain == 1 { 3int } else { 4int }), customer, component)
            + fact_range(db, grain + 1, first, last, customer, component)
            + fact_range(db, grain, last * (if grain == 1 { 3int } else { 4int }), upper, customer, component),
{
    let divisor = if grain == 1 { 3int } else { 4int };
    lemma_range_split(db, grain, lower, first * divisor, upper, customer, component);
    lemma_range_split(db, grain, first * divisor, last * divisor, upper, customer, component);
    lemma_range_coarsen(db, grain, first, last, customer, component);
}

pub proof fn lemma_dimension_maximum(index: &Index<i128>, key: KeyView, value: i128)
    requires index.wf(), ranked_values_valid(*index),
        index.entries().len() > 0, index.entries()[0] == (key, value),
    ensures forall|query: KeyView| #[trigger] index.at(query).is_some() ==> index.at(query).unwrap() <= value,
{
    index.model();
    assert(index.at(key) == Some(value));
    let first_tie = choose|tie: i64| #[trigger] rank_view(value as int, tie as int) == key;
    assert forall|query: KeyView| #[trigger] index.at(query).is_some() implies index.at(query).unwrap() <= value by {
        let other = index.at(query).unwrap();
        let other_tie = choose|tie: i64| #[trigger] rank_view(other as int, tie as int) == query;
        let position = choose|position: int| #![trigger index.entries()[position]]
            0 <= position < index.entries().len() && index.entries()[position].0 == query;
        assert(index.entries()[position].1 == other);
        if position > 0 {
            assert(verus_prooflib::ordered::key_lt(key, query));
            verus_prooflib::lemma_i128_rank_order(value, other, first_tie, other_tie);
        }
    }
}

pub proof fn lemma_slice_unseen_bound(slice: crate::output::ReadSlice, customer: i64, seen: Set<i64>)
    requires crate::output::read_slice_valid(slice), !seen.contains(customer),
        forall|position: int| #![trigger slice.slice.ranks.entries()[position]]
            0 <= position < slice.consumed@ ==> seen.contains(slice.slice.ranks.entries()[position].customer),
    ensures cell_at(slice.slice.cells, customer as int).upper as int <= crate::output::head_upper(slice),
        slice.head.is_none() ==> cell_at(slice.slice.cells, customer as int).count == 0,
{
    let ghost cells = crate::output::read_cell_index(slice);
    let ghost ckey = (customer as int, Seq::<u8>::empty());
    if slice.head.is_some() { crate::output::lemma_head_cell(slice); }
    match cells.at(ckey) {
        Some(cell) => {
            let ghost ranks = slice.slice.ranks;
            ranks.model();
            let rank = rank_of(cell);
            assert(ranks.at(rank.kv()) == Some(rank));
            lemma_at(ranks, rank.kv(), rank);
            assert(ranks.entries().contains(rank));
            let position = choose|position: int| #![trigger ranks.entries()[position]]
                0 <= position < ranks.entries().len() && ranks.entries()[position] == rank;
            lemma_cell_key(cells, ckey);
            assert(cell.customer == customer);
            assert(position >= slice.consumed@);
            assert(slice.head.is_some());
            let first = slice.head.unwrap();
            if position > slice.consumed@ {
                assert(key_lt(ranks.entries()[slice.consumed@].kv(), ranks.entries()[position].kv()));
                lemma_rank_order(first, rank);
            }
        },
        None => {},
    }
}

pub proof fn lemma_window_unseen_bound(slices: Seq<crate::output::ReadSlice>, db: Db, parameters: Params,
    customer: i64, seen: Set<i64>)
    requires crate::output::read_slices_valid(slices), !seen.contains(customer),
        crate::output::read_moment_sum(slices, customer as int, 2) == window_fact(db, parameters, customer as int, 2),
        forall|slice_position: int, rank_position: int| #![trigger slices[slice_position].slice.ranks.entries()[rank_position]]
            0 <= slice_position < slices.len() && 0 <= rank_position < slices[slice_position].consumed@
                ==> seen.contains(slices[slice_position].slice.ranks.entries()[rank_position].customer),
    ensures window_fact(db, parameters, customer as int, 2) <= sum(slices, crate::output::head_upper_weight()),
{
    let cells = crate::output::read_moment_weight(customer as int, 2);
    let bounds = |slice: crate::output::ReadSlice| if slices.contains(slice) { crate::output::head_upper(slice) } else { cells(slice) };
    assert forall|slice: crate::output::ReadSlice| #[trigger] cells(slice) <= bounds(slice) by {
        if slices.contains(slice) {
            let position = choose|position: int| #![trigger slices[position]] 0 <= position < slices.len() && slices[position] == slice;
            assert forall|rank_position: int| #![trigger slice.slice.ranks.entries()[rank_position]]
                0 <= rank_position < slice.consumed@ implies seen.contains(slice.slice.ranks.entries()[rank_position].customer) by {
                assert(slice.slice.ranks.entries()[rank_position] == slices[position].slice.ranks.entries()[rank_position]);
            }
            lemma_slice_unseen_bound(slice, customer, seen);
        }
    }
    lemma_sum_le(slices, cells, bounds);
    lemma_sum_ext(slices, bounds, crate::output::head_upper_weight());
}

}

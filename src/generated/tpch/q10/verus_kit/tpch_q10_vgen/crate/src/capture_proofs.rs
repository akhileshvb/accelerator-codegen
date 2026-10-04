use vstd::prelude::*;
use crate::gen_spec::*;
use crate::calendar::period_of;
use crate::model::{fact_sum, line_weight};
use verus_common::sums::*;
use verus_prooflib::{join_sum, left_join_delta, right_join_delta};
use verus_prooflib::ordered::Index;
use crate::maintenance::{Orders, order_group_at, changed_key_weight};
use crate::values::{Moments, moment_at};

verus! {

#[verifier::opaque]
pub open spec fn order_group(rows: Seq<OrdersRow>, key: int, slice: int, customer: int, grain: int) -> int {
    sum(rows, |order: OrdersRow| if order.o_orderkey == key && order.o_custkey == customer && period_of(order.o_orderdate, grain) == slice { 1int } else { 0int })
}

#[verifier::opaque]
pub open spec fn order_delta_group(rows: Seq<(OrdersRow, int)>, key: int, slice: int, customer: int, grain: int) -> int {
    sum(rows, |entry: (OrdersRow, int)| if entry.0.o_orderkey == key && entry.0.o_custkey == customer && period_of(entry.0.o_orderdate, grain) == slice { entry.1 } else { 0int })
}

#[verifier::opaque]
pub open spec fn line_group(rows: Seq<LineitemRow>, key: int, component: int) -> int {
    sum(rows, |line: LineitemRow| if line.l_orderkey == key { line_weight(line, component) } else { 0 })
}

#[verifier::opaque]
pub open spec fn line_delta_group(rows: Seq<(LineitemRow, int)>, key: int, component: int) -> int {
    sum(rows, |entry: (LineitemRow, int)| if entry.0.l_orderkey == key { entry.1 * line_weight(entry.0, component) } else { 0 })
}

pub proof fn lemma_order_group_capture(before: Seq<OrdersRow>, after: Seq<OrdersRow>, changes: Seq<(OrdersRow, int)>, key: int, slice: int, customer: int, grain: int)
    requires bag_step(before, after, changes),
    ensures order_group(after, key, slice, customer, grain)
        == order_group(before, key, slice, customer, grain) + order_delta_group(changes, key, slice, customer, grain),
{
    reveal(order_group);
    reveal(order_delta_group);
    let weight = |order: OrdersRow| if order.o_orderkey == key && order.o_custkey == customer && period_of(order.o_orderdate, grain) == slice { 1int } else { 0int };
    assert(sum(after, weight) == sum(before, weight) + sum(changes, |entry: (OrdersRow, int)| entry.1 * weight(entry.0)));
    lemma_sum_ext_all(changes, |entry: (OrdersRow, int)| entry.1 * weight(entry.0),
        |entry: (OrdersRow, int)| if entry.0.o_orderkey == key && entry.0.o_custkey == customer && period_of(entry.0.o_orderdate, grain) == slice { entry.1 } else { 0int });
}

#[verifier::rlimit(10)]
pub proof fn lemma_keyed_order_delta(keys: &Index<()>, rows: Seq<(OrdersRow, int)>, weight: spec_fn(int) -> int,
    slice: int, customer: int, grain: int)
    requires keys.wf(),
        forall|key: (int, Seq<u8>)| #[trigger] keys.at(key).is_some() ==> key.1 == Seq::<u8>::empty(),
        forall|key: int| #[trigger] order_delta_group(rows, key, slice, customer, grain)
            * weight(key) != 0 ==> keys.at((key, Seq::empty())).is_some(),
    ensures sum(keys.entries(), |entry: ((int, Seq<u8>), ())|
        order_delta_group(rows, entry.0.0, slice, customer, grain) * weight(entry.0.0))
        == sum(rows, |entry: (OrdersRow, int)| if entry.0.o_custkey == customer && period_of(entry.0.o_orderdate, grain) == slice {
            entry.1 * weight(entry.0.o_orderkey) } else { 0 }),
{
    reveal(order_delta_group);
    keys.model();
    let source_key = |entry: (OrdersRow, int)| entry.0.o_orderkey;
    let source_count = |entry: (OrdersRow, int)| if entry.0.o_custkey == customer && period_of(entry.0.o_orderdate, grain) == slice {
        entry.1 * weight(entry.0.o_orderkey) } else { 0 };
    let index_key = |entry: ((int, Seq<u8>), ())| entry.0.0;
    let index_count = |entry: ((int, Seq<u8>), ())| order_delta_group(rows, entry.0.0, slice, customer, grain) * weight(entry.0.0);
    assert forall|key: int| #[trigger] verus_prooflib::class_total(rows, source_key, source_count, key)
        == verus_prooflib::class_total(keys.entries(), index_key, index_count, key) by {
        let base = |entry: (OrdersRow, int)| if entry.0.o_orderkey == key && entry.0.o_custkey == customer
            && period_of(entry.0.o_orderdate, grain) == slice { entry.1 } else { 0 };
        let selected = |entry: (OrdersRow, int)| if source_key(entry) == key { source_count(entry) } else { 0 };
        assert forall|entry: (OrdersRow, int)| #[trigger] selected(entry) == weight(key) * base(entry) by {
            assert(entry.1 * weight(key) == weight(key) * entry.1) by (nonlinear_arith);
        }
        lemma_sum_scale(rows, base, selected, weight(key));
        let index_selected = |entry: ((int, Seq<u8>), ())| if index_key(entry) == key { index_count(entry) } else { 0 };
        verus_prooflib::ordered::lemma_key_contribution(keys, (key, Seq::empty()), index_count);
        assert forall|position: int| #![trigger keys.entries()[position]]
            0 <= position < keys.entries().len() implies index_selected(keys.entries()[position])
                == if keys.entries()[position].0 == (key, Seq::empty()) { index_count(keys.entries()[position]) } else { 0 } by {
            assert(keys.at(keys.entries()[position].0).is_some());
        }
        lemma_sum_ext(keys.entries(), index_selected,
            |entry: ((int, Seq<u8>), ())| if entry.0 == (key, Seq::empty()) { index_count(entry) } else { 0 });
        assert(order_delta_group(rows, key, slice, customer, grain) * weight(key)
            == weight(key) * order_delta_group(rows, key, slice, customer, grain)) by (nonlinear_arith);
    }
    let constant = |key: int| 1int;
    verus_prooflib::lemma_equal_classes(rows, keys.entries(), source_key, index_key, source_count, index_count, constant);
    lemma_sum_ext_all(rows, |entry: (OrdersRow, int)| source_count(entry) * constant(source_key(entry)), source_count);
    lemma_sum_ext_all(keys.entries(), |entry: ((int, Seq<u8>), ())| index_count(entry) * constant(index_key(entry)), index_count);
}

pub proof fn lemma_keyed_line_delta(keys: &Index<()>, rows: Seq<(LineitemRow, int)>, weight: spec_fn(int) -> int, component: int)
    requires keys.wf(),
        forall|key: (int, Seq<u8>)| #[trigger] keys.at(key).is_some() ==> key.1 == Seq::<u8>::empty(),
        forall|key: int| #[trigger] line_delta_group(rows, key, component)
            * weight(key) != 0 ==> keys.at((key, Seq::empty())).is_some(),
    ensures sum(keys.entries(), |entry: ((int, Seq<u8>), ())| line_delta_group(rows, entry.0.0, component) * weight(entry.0.0))
        == sum(rows, |entry: (LineitemRow, int)| entry.1 * line_weight(entry.0, component) * weight(entry.0.l_orderkey)),
{
    reveal(line_delta_group);
    keys.model();
    let source_key = |entry: (LineitemRow, int)| entry.0.l_orderkey;
    let source_count = |entry: (LineitemRow, int)| entry.1 * line_weight(entry.0, component) * weight(entry.0.l_orderkey);
    let index_key = |entry: ((int, Seq<u8>), ())| entry.0.0;
    let index_count = |entry: ((int, Seq<u8>), ())| line_delta_group(rows, entry.0.0, component) * weight(entry.0.0);
    assert forall|key: int| #[trigger] verus_prooflib::class_total(rows, source_key, source_count, key)
        == verus_prooflib::class_total(keys.entries(), index_key, index_count, key) by {
        let base = |entry: (LineitemRow, int)| if entry.0.l_orderkey == key { entry.1 * line_weight(entry.0, component) } else { 0 };
        let selected = |entry: (LineitemRow, int)| if source_key(entry) == key { source_count(entry) } else { 0 };
        assert forall|entry: (LineitemRow, int)| #[trigger] selected(entry) == weight(key) * base(entry) by {
            assert(entry.1 * line_weight(entry.0, component) * weight(key)
                == weight(key) * (entry.1 * line_weight(entry.0, component))) by (nonlinear_arith);
        }
        lemma_sum_scale(rows, base, selected, weight(key));
        let index_selected = |entry: ((int, Seq<u8>), ())| if index_key(entry) == key { index_count(entry) } else { 0 };
        verus_prooflib::ordered::lemma_key_contribution(keys, (key, Seq::empty()), index_count);
        assert forall|position: int| #![trigger keys.entries()[position]]
            0 <= position < keys.entries().len() implies index_selected(keys.entries()[position])
                == if keys.entries()[position].0 == (key, Seq::empty()) { index_count(keys.entries()[position]) } else { 0 } by {
            assert(keys.at(keys.entries()[position].0).is_some());
        }
        lemma_sum_ext(keys.entries(), index_selected,
            |entry: ((int, Seq<u8>), ())| if entry.0 == (key, Seq::empty()) { index_count(entry) } else { 0 });
        assert(line_delta_group(rows, key, component) * weight(key)
            == weight(key) * line_delta_group(rows, key, component)) by (nonlinear_arith);
    }
    let constant = |key: int| 1int;
    verus_prooflib::lemma_equal_classes(rows, keys.entries(), source_key, index_key, source_count, index_count, constant);
    lemma_sum_ext_all(rows, |entry: (LineitemRow, int)| source_count(entry) * constant(source_key(entry)), source_count);
    lemma_sum_ext_all(keys.entries(), |entry: ((int, Seq<u8>), ())| index_count(entry) * constant(index_key(entry)), index_count);
}

#[verifier::opaque]
pub open spec fn fact_pair(order: OrdersRow, line: LineitemRow, slice: int, customer: int, grain: int, component: int) -> int {
    if order.o_custkey == customer && period_of(order.o_orderdate, grain) == slice && line.l_orderkey == order.o_orderkey {
        line_weight(line, component)
    } else { 0 }
}

pub open spec fn fact_weight(slice: int, customer: int, grain: int, component: int) -> spec_fn(LineitemRow, OrdersRow) -> int {
    |line: LineitemRow, order: OrdersRow| fact_pair(order, line, slice, customer, grain, component)
}

pub open spec fn fact_fold(orders: Seq<OrdersRow>, lines: Seq<LineitemRow>, slice: int, customer: int, grain: int, component: int) -> int {
    sum(orders, |order: OrdersRow| sum(lines, |line: LineitemRow| fact_pair(order, line, slice, customer, grain, component)))
}

pub proof fn lemma_fact_fold(db: Db, slice: int, customer: int, grain: int, component: int)
    ensures fact_fold(db.orders, db.lineitem, slice, customer, grain, component) == fact_sum(db, slice, customer, grain, component),
{
    reveal(fact_sum);
    reveal(fact_pair);
    let original = |order: OrdersRow| if order.o_custkey == customer && period_of(order.o_orderdate, grain) == slice {
        sum(db.lineitem, |line: LineitemRow| if line.l_orderkey == order.o_orderkey { line_weight(line, component) } else { 0 })
    } else { 0 };
    let folded = |order: OrdersRow| sum(db.lineitem, |line: LineitemRow| fact_pair(order, line, slice, customer, grain, component));
    assert forall|order: OrdersRow| #[trigger] original(order) == folded(order) by {
        if order.o_custkey == customer && period_of(order.o_orderdate, grain) == slice {
            lemma_sum_ext_all(db.lineitem,
                |line: LineitemRow| if line.l_orderkey == order.o_orderkey { line_weight(line, component) } else { 0 },
                |line: LineitemRow| fact_pair(order, line, slice, customer, grain, component));
        } else {
            lemma_sum_zero(db.lineitem, |line: LineitemRow| fact_pair(order, line, slice, customer, grain, component));
        }
    }
    lemma_sum_ext_all(db.orders, original, folded);
}

pub proof fn lemma_fact_join(db: Db, slice: int, customer: int, grain: int, component: int,
    weight: spec_fn(LineitemRow, OrdersRow) -> int)
    requires forall|line: LineitemRow, order: OrdersRow| #[trigger] weight(line, order)
        == fact_pair(order, line, slice, customer, grain, component),
    ensures join_sum(db.lineitem, db.orders, weight) == fact_sum(db, slice, customer, grain, component),
{
    lemma_fact_fold(db, slice, customer, grain, component);
    let by_line = |line: LineitemRow| sum(db.orders, |order: OrdersRow| weight(line, order));
    let by_order = |order: OrdersRow| sum(db.lineitem, |line: LineitemRow| fact_pair(order, line, slice, customer, grain, component));
    assert forall|order: OrdersRow| #[trigger] by_order(order) == sum(db.lineitem, |line: LineitemRow| weight(line, order)) by {
        lemma_sum_ext_all(db.lineitem, |line: LineitemRow| fact_pair(order, line, slice, customer, grain, component),
            |line: LineitemRow| weight(line, order));
    }
    lemma_sum_swap(db.lineitem, db.orders, weight, by_line, by_order);
    lemma_sum_ext_all(db.lineitem, by_line,
        |entry: LineitemRow| sum(db.orders, |other: OrdersRow| weight(entry, other)));
    lemma_sum_ext_all(db.orders, by_order,
        |order: OrdersRow| sum(db.lineitem, |line: LineitemRow| fact_pair(order, line, slice, customer, grain, component)));
}

pub proof fn lemma_fact_capture(before: Db, after: Db, order_changes: Seq<(OrdersRow, int)>, line_changes: Seq<(LineitemRow, int)>,
    slice: int, customer: int, grain: int, component: int)
    requires bag_step(before.orders, after.orders, order_changes), bag_step(before.lineitem, after.lineitem, line_changes),
    ensures fact_sum(after, slice, customer, grain, component) == fact_sum(before, slice, customer, grain, component)
        + left_join_delta(line_changes, before.orders,
            fact_weight(slice, customer, grain, component))
        + right_join_delta(after.lineitem, order_changes,
            fact_weight(slice, customer, grain, component)),
{
    let weight = fact_weight(slice, customer, grain, component);
    lemma_fact_join(before, slice, customer, grain, component, weight);
    lemma_fact_join(after, slice, customer, grain, component, weight);
    verus_prooflib::lemma_join_bag_step(before.lineitem, after.lineitem, line_changes,
        before.orders, after.orders, order_changes, weight);
    assert(join_sum(before.lineitem, before.orders, weight) == fact_sum(before, slice, customer, grain, component));
    assert(join_sum(after.lineitem, after.orders, weight) == fact_sum(after, slice, customer, grain, component));
}

#[verifier::rlimit(10)]
pub proof fn lemma_right_fact_delta(lines: Seq<LineitemRow>, changes: Seq<(OrdersRow, int)>, slice: int, customer: int, grain: int, component: int)
    ensures right_join_delta(lines, changes, fact_weight(slice, customer, grain, component))
        == sum(changes, |entry: (OrdersRow, int)| if entry.0.o_custkey == customer && period_of(entry.0.o_orderdate, grain) == slice {
            entry.1 * line_group(lines, entry.0.o_orderkey, component) } else { 0 }),
{
    reveal(fact_pair);
    reveal(line_group);
    let weight = fact_weight(slice, customer, grain, component);
    let original = |entry: (OrdersRow, int)| entry.1 * sum(lines,
        |line: LineitemRow| weight(line, entry.0));
    let factored = |entry: (OrdersRow, int)| if entry.0.o_custkey == customer && period_of(entry.0.o_orderdate, grain) == slice {
        entry.1 * line_group(lines, entry.0.o_orderkey, component) } else { 0 };
    assert forall|entry: (OrdersRow, int)| #[trigger] original(entry) == factored(entry) by {
        if entry.0.o_custkey == customer && period_of(entry.0.o_orderdate, grain) == slice {
            lemma_sum_ext_all(lines, |line: LineitemRow| weight(line, entry.0),
                |line: LineitemRow| if line.l_orderkey == entry.0.o_orderkey { line_weight(line, component) } else { 0 });
        } else {
            lemma_sum_zero(lines, |line: LineitemRow| weight(line, entry.0));
        }
    }
    lemma_sum_ext_all(changes, original, factored);
}

#[verifier::rlimit(10)]
pub proof fn lemma_left_fact_delta(orders: Seq<OrdersRow>, changes: Seq<(LineitemRow, int)>, slice: int, customer: int, grain: int, component: int)
    ensures left_join_delta(changes, orders, fact_weight(slice, customer, grain, component))
        == sum(changes, |entry: (LineitemRow, int)| entry.1 * line_weight(entry.0, component)
            * order_group(orders, entry.0.l_orderkey, slice, customer, grain)),
{
    reveal(fact_pair);
    reveal(order_group);
    let weight = fact_weight(slice, customer, grain, component);
    let original = |entry: (LineitemRow, int)| entry.1 * sum(orders,
        |order: OrdersRow| weight(entry.0, order));
    let factored = |entry: (LineitemRow, int)| entry.1 * line_weight(entry.0, component)
        * order_group(orders, entry.0.l_orderkey, slice, customer, grain);
    assert forall|entry: (LineitemRow, int)| #[trigger] original(entry) == factored(entry) by {
        let count = |order: OrdersRow| if order.o_orderkey == entry.0.l_orderkey && order.o_custkey == customer
            && period_of(order.o_orderdate, grain) == slice { 1int } else { 0int };
        let contribution = |order: OrdersRow| weight(entry.0, order);
        lemma_sum_scale(orders, count, contribution, line_weight(entry.0, component));
        assert(entry.1 * (line_weight(entry.0, component) * order_group(orders, entry.0.l_orderkey, slice, customer, grain))
            == entry.1 * line_weight(entry.0, component) * order_group(orders, entry.0.l_orderkey, slice, customer, grain)) by (nonlinear_arith);
    }
    lemma_sum_ext_all(changes, original, factored);
}

#[verifier::rlimit(10)]
pub proof fn lemma_changed_key_sum(before: Db, after: Db, keys: &Index<()>, order_changes: Orders, line_changes: Index<Moments>,
    post_orders: Orders, post_lines: Index<Moments>, order_capture: Seq<(OrdersRow, int)>, line_capture: Seq<(LineitemRow, int)>,
    slice: int, customer: int, grain: int, component: int)
    requires keys.wf(), bag_step(before.orders, after.orders, order_capture), bag_step(before.lineitem, after.lineitem, line_capture),
        forall|key: (int, Seq<u8>)| #[trigger] keys.at(key).is_some() ==> key.1 == Seq::<u8>::empty(),
        forall|key: (int, Seq<u8>)| #![trigger order_changes.at(key), line_changes.at(key)]
            order_changes.at(key).is_some() || line_changes.at(key).is_some() ==> keys.at(key).is_some(),
        forall|key: int| #[trigger] order_group_at(order_changes, key, slice, customer, grain)
            == order_delta_group(order_capture, key, slice, customer, grain),
        forall|key: int| #[trigger] moment_at(line_changes, key).component(component) == line_delta_group(line_capture, key, component),
        forall|key: int| #[trigger] keys.at((key, Seq::empty())).is_some() ==>
            order_group_at(post_orders, key, slice, customer, grain) == order_group(after.orders, key, slice, customer, grain),
        forall|key: int| #[trigger] keys.at((key, Seq::empty())).is_some() ==>
            moment_at(post_lines, key).component(component) == line_group(after.lineitem, key, component),
    ensures sum(keys.entries(), changed_key_weight(order_changes, line_changes, post_orders, post_lines, slice, customer, grain, component))
        == fact_sum(after, slice, customer, grain, component) - fact_sum(before, slice, customer, grain, component),
{
    keys.model();
    let post_weight = |key: int| line_group(after.lineitem, key, component);
    let before_weight = |key: int| order_group(before.orders, key, slice, customer, grain);
    assert forall|key: int| #[trigger] order_delta_group(order_capture, key, slice, customer, grain) * post_weight(key) != 0
        implies keys.at((key, Seq::empty())).is_some() by {
        assert(order_changes.at((key, Seq::empty())).is_some() || line_changes.at((key, Seq::empty())).is_some()
            ==> keys.at((key, Seq::empty())).is_some());
        if !keys.at((key, Seq::empty())).is_some() {
            assert(order_changes.at((key, Seq::empty())).is_none());
            assert(order_group_at(order_changes, key, slice, customer, grain) == 0);
        }
    }
    assert forall|key: int| #[trigger] line_delta_group(line_capture, key, component) * before_weight(key) != 0
        implies keys.at((key, Seq::empty())).is_some() by {
        assert(order_changes.at((key, Seq::empty())).is_some() || line_changes.at((key, Seq::empty())).is_some()
            ==> keys.at((key, Seq::empty())).is_some());
        if !keys.at((key, Seq::empty())).is_some() {
            assert(line_changes.at((key, Seq::empty())).is_none());
            assert(moment_at(line_changes, key).component(component) == 0);
        }
    }
    lemma_keyed_order_delta(keys, order_capture, post_weight, slice, customer, grain);
    lemma_keyed_line_delta(keys, line_capture, before_weight, component);
    lemma_right_fact_delta(after.lineitem, order_capture, slice, customer, grain, component);
    lemma_left_fact_delta(before.orders, line_capture, slice, customer, grain, component);
    lemma_sum_ext_all(order_capture,
        |entry: (OrdersRow, int)| if entry.0.o_custkey == customer && period_of(entry.0.o_orderdate, grain) == slice {
            entry.1 * post_weight(entry.0.o_orderkey) } else { 0 },
        |entry: (OrdersRow, int)| if entry.0.o_custkey == customer && period_of(entry.0.o_orderdate, grain) == slice {
            entry.1 * line_group(after.lineitem, entry.0.o_orderkey, component) } else { 0 });
    lemma_sum_ext_all(line_capture,
        |entry: (LineitemRow, int)| entry.1 * line_weight(entry.0, component) * before_weight(entry.0.l_orderkey),
        |entry: (LineitemRow, int)| entry.1 * line_weight(entry.0, component)
            * order_group(before.orders, entry.0.l_orderkey, slice, customer, grain));
    let right = |entry: ((int, Seq<u8>), ())| order_delta_group(order_capture, entry.0.0, slice, customer, grain) * post_weight(entry.0.0);
    let left = |entry: ((int, Seq<u8>), ())| line_delta_group(line_capture, entry.0.0, component) * before_weight(entry.0.0);
    let combined = changed_key_weight(order_changes, line_changes, post_orders, post_lines, slice, customer, grain, component);
    assert(sum(keys.entries(), right) == right_join_delta(after.lineitem, order_capture, fact_weight(slice, customer, grain, component)));
    assert(sum(keys.entries(), left) == left_join_delta(line_capture, before.orders, fact_weight(slice, customer, grain, component)));
    assert forall|position: int| #![trigger keys.entries()[position]] 0 <= position < keys.entries().len()
        implies combined(keys.entries()[position]) == right(keys.entries()[position]) + left(keys.entries()[position]) by {
        let key = keys.entries()[position].0.0;
        assert(keys.at((key, Seq::empty())).is_some());
        assert(order_group_at(order_changes, key, slice, customer, grain) == order_delta_group(order_capture, key, slice, customer, grain));
        assert(moment_at(line_changes, key).component(component) == line_delta_group(line_capture, key, component));
        assert(order_group_at(post_orders, key, slice, customer, grain) == order_group(after.orders, key, slice, customer, grain));
        assert(moment_at(post_lines, key).component(component) == line_group(after.lineitem, key, component));
        lemma_order_group_capture(before.orders, after.orders, order_capture, key, slice, customer, grain);
        assert(line_delta_group(line_capture, key, component) * before_weight(key)
            == before_weight(key) * line_delta_group(line_capture, key, component)) by (nonlinear_arith);
    }
    let sum_weight = |entry: ((int, Seq<u8>), ())| right(entry) + left(entry);
    lemma_sum_ext(keys.entries(), combined, sum_weight);
    lemma_sum_add(keys.entries(), right, left, sum_weight);
    lemma_fact_capture(before, after, order_capture, line_capture, slice, customer, grain, component);
}

}

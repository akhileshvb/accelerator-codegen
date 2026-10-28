#![verifier::loop_isolation(false)]
use vstd::prelude::*;
use std::collections::HashMap;
use verus_common::sums::*;
use crate::gen_spec::*;
use crate::gen_obligations::*;
use crate::data::*;
use crate::pages::*;
use crate::model::*;
use crate::kernel::{remember, remembered, bounded_keys};

verus! {

pub fn apply_lines(editor: &mut Editor<OrderData>, deltas: &[LineQuantityDelta], seen: &mut HashMap<i128, bool>, keys: &mut Vec<i128>)
    requires old(editor).wf(), remembered(old(seen)@, old(keys)@), bounded_keys(old(keys)@),
        forall|key: i128| #[trigger] old(keys)@.contains(key) ==> old(editor).at(key) is Some,
        forall|key: i128| #[trigger] order_shape(old(editor).at(key)),
    ensures final(editor).wf(), remembered(final(seen)@, final(keys)@),
        forall|key: i128| #[trigger] old(editor).at(key) is Some ==> final(editor).at(key) is Some,
        forall|key: i128| #[trigger] final(keys)@.contains(key) ==> final(editor).at(key) is Some,
        bounded_keys(final(keys)@),
        forall|key: i128| #[trigger] old(keys)@.contains(key) ==> final(keys)@.contains(key),
        forall|key: i128| !final(keys)@.contains(key) ==> #[trigger] final(editor).at(key) == old(editor).at(key),
        forall|key: i128| #[trigger] order_shape(final(editor).at(key)),
        forall|key: i128| #[trigger] line_count(final(editor).at(key)) == line_count(old(editor).at(key)) + sum(line_quantity_delta_seq_view(deltas@), dn(key as int)),
        forall|key: i128| #[trigger] line_sum(final(editor).at(key)) == line_sum(old(editor).at(key)) + sum(line_quantity_delta_seq_view(deltas@), dq(key as int)),
        forall|key: i128, customer: int, date: int, price: int| #[trigger] order_count(final(editor).at(key), customer, date, price) == order_count(old(editor).at(key), customer, date, price),
{
    hide(sum);
    hide(Editor::at);
    hide(Editor::wf);
    proof { empty_sums(line_quantity_delta_seq_view(deltas@.take(0))); }
    let mut index = 0usize;
    while index < deltas.len()
        invariant index <= deltas.len(), editor.wf(), remembered(seen@, keys@), bounded_keys(keys@),
            forall|key: i128| #[trigger] old(editor).at(key) is Some ==> editor.at(key) is Some,
            forall|key: i128| #[trigger] keys@.contains(key) ==> editor.at(key) is Some,
            forall|key: i128| #[trigger] old(keys)@.contains(key) ==> keys@.contains(key),
            forall|key: i128| !keys@.contains(key) ==> #[trigger] editor.at(key) == old(editor).at(key),
            forall|key: i128| #[trigger] order_shape(editor.at(key)),
            forall|key: i128| #[trigger] line_count(editor.at(key)) == line_count(old(editor).at(key)) + sum(line_quantity_delta_seq_view(deltas@.take(index as int)), dn(key as int)),
            forall|key: i128| #[trigger] line_sum(editor.at(key)) == line_sum(old(editor).at(key)) + sum(line_quantity_delta_seq_view(deltas@.take(index as int)), dq(key as int)),
            forall|key: i128, customer: int, date: int, price: int| #[trigger] order_count(editor.at(key), customer, date, price) == order_count(old(editor).at(key), customer, date, price),
        decreases deltas.len() - index,
    {
        let delta = &deltas[index];
        remember(delta.row.l_orderkey as i128, seen, keys);
        change_line(editor, delta.row.l_orderkey, delta.row.l_quantity, delta.sign);
        proof {
            assert forall|key: i128| #[trigger] line_count(editor.at(key)) == line_count(old(editor).at(key)) + sum(line_quantity_delta_seq_view(deltas@.take(index as int + 1)), dn(key as int)) by {
                let item = (line_quantity_input_view(delta.row), delta.sign as int);
                if key as int == delta.row.l_orderkey as int { assert(dn(key as int)(item) == delta.sign as int); } else { assert(dn(key as int)(item) == 0); }
                projected_prefix(deltas@, index as int, |input: LineQuantityDelta| (line_quantity_input_view(input.row), input.sign as int), dn(key as int));
            }
            assert forall|key: i128| #[trigger] line_sum(editor.at(key)) == line_sum(old(editor).at(key)) + sum(line_quantity_delta_seq_view(deltas@.take(index as int + 1)), dq(key as int)) by {
                projected_prefix(deltas@, index as int, |input: LineQuantityDelta| (line_quantity_input_view(input.row), input.sign as int), dq(key as int));
                assert(delta.row.l_quantity as int * delta.sign as int == delta.sign as int * delta.row.l_quantity as int) by (nonlinear_arith);
            }
        }
        index += 1;
    }
    proof { assert(deltas@.take(index as int) =~= deltas@); }
}

pub fn apply_orders(editor: &mut Editor<OrderData>, deltas: &[OrderGroupDelta], seen: &mut HashMap<i128, bool>, keys: &mut Vec<i128>)
    requires old(editor).wf(), remembered(old(seen)@, old(keys)@), bounded_keys(old(keys)@),
        forall|key: i128| #[trigger] old(keys)@.contains(key) ==> old(editor).at(key) is Some,
        forall|key: i128| #[trigger] order_shape(old(editor).at(key)),
    ensures final(editor).wf(), remembered(final(seen)@, final(keys)@),
        forall|key: i128| #[trigger] old(editor).at(key) is Some ==> final(editor).at(key) is Some,
        forall|key: i128| #[trigger] final(keys)@.contains(key) ==> final(editor).at(key) is Some,
        bounded_keys(final(keys)@),
        forall|key: i128| #[trigger] old(keys)@.contains(key) ==> final(keys)@.contains(key),
        forall|key: i128| !final(keys)@.contains(key) ==> #[trigger] final(editor).at(key) == old(editor).at(key),
        forall|key: i128| #[trigger] order_shape(final(editor).at(key)),
        forall|key: i128| #[trigger] line_count(final(editor).at(key)) == line_count(old(editor).at(key)),
        forall|key: i128| #[trigger] line_sum(final(editor).at(key)) == line_sum(old(editor).at(key)),
        forall|key: i128, customer: int, date: int, price: int| #[trigger] order_count(final(editor).at(key), customer, date, price) == order_count(old(editor).at(key), customer, date, price) + sum(order_group_delta_seq_view(deltas@), dorder(key as int, customer, date, price)),
{
    hide(sum);
    hide(Editor::at);
    hide(Editor::wf);
    proof { empty_sums(order_group_delta_seq_view(deltas@.take(0))); }
    let mut index = 0usize;
    while index < deltas.len()
        invariant index <= deltas.len(), editor.wf(), remembered(seen@, keys@), bounded_keys(keys@),
            forall|key: i128| #[trigger] old(editor).at(key) is Some ==> editor.at(key) is Some,
            forall|key: i128| #[trigger] keys@.contains(key) ==> editor.at(key) is Some,
            forall|key: i128| #[trigger] old(keys)@.contains(key) ==> keys@.contains(key),
            forall|key: i128| !keys@.contains(key) ==> #[trigger] editor.at(key) == old(editor).at(key),
            forall|key: i128| #[trigger] order_shape(editor.at(key)),
            forall|key: i128| #[trigger] line_count(editor.at(key)) == line_count(old(editor).at(key)),
            forall|key: i128| #[trigger] line_sum(editor.at(key)) == line_sum(old(editor).at(key)),
            forall|key: i128, customer: int, date: int, price: int| #[trigger] order_count(editor.at(key), customer, date, price) == order_count(old(editor).at(key), customer, date, price) + sum(order_group_delta_seq_view(deltas@.take(index as int)), dorder(key as int, customer, date, price)),
        decreases deltas.len() - index,
    {
        let delta = &deltas[index];
        remember(delta.row.o_orderkey as i128, seen, keys);
        change_order(editor, delta.row.o_orderkey, delta.row.o_custkey, delta.row.o_orderdate, delta.row.o_totalprice, delta.sign);
        proof {
            assert forall|key: i128, customer: int, date: int, price: int| #[trigger] order_count(editor.at(key), customer, date, price) == order_count(old(editor).at(key), customer, date, price) + sum(order_group_delta_seq_view(deltas@.take(index as int + 1)), dorder(key as int, customer, date, price)) by {
                let item = (order_group_input_view(delta.row), delta.sign as int);
                if key as int == delta.row.o_orderkey as int && customer == delta.row.o_custkey as int && date == delta.row.o_orderdate as int && price == delta.row.o_totalprice as int {
                    assert(dorder(key as int, customer, date, price)(item) == delta.sign as int);
                } else { assert(dorder(key as int, customer, date, price)(item) == 0); }
                projected_prefix(deltas@, index as int, |input: OrderGroupDelta| (order_group_input_view(input.row), input.sign as int), dorder(key as int, customer, date, price));
            }
        }
        index += 1;
    }
    proof { assert(deltas@.take(index as int) =~= deltas@); }
}

pub fn apply_customers(editor: &mut Editor<CustomerData>, deltas: &Vec<CustomerNameDelta>, seen: &mut HashMap<i128, bool>, keys: &mut Vec<i128>)
    requires old(editor).wf(), remembered(old(seen)@, old(keys)@), bounded_keys(old(keys)@),
        forall|key: i128| #[trigger] old(keys)@.contains(key) ==> old(editor).at(key) is Some,
        forall|key: i128| #[trigger] customer_shape(old(editor).at(key)),
    ensures final(editor).wf(), remembered(final(seen)@, final(keys)@),
        forall|key: i128| #[trigger] old(editor).at(key) is Some ==> final(editor).at(key) is Some,
        forall|key: i128| #[trigger] final(keys)@.contains(key) ==> final(editor).at(key) is Some,
        bounded_keys(final(keys)@),
        forall|key: i128| #[trigger] old(keys)@.contains(key) ==> final(keys)@.contains(key),
        forall|key: i128| !final(keys)@.contains(key) ==> #[trigger] final(editor).at(key) == old(editor).at(key),
        forall|key: i128| #[trigger] customer_shape(final(editor).at(key)),
        forall|key: i128, name: Seq<u8>| #[trigger] customer_count(final(editor).at(key), name) == customer_count(old(editor).at(key), name) + sum(customer_name_delta_seq_view(deltas@), dcustomer(key as int, name)),
{
    hide(sum);
    hide(Editor::at);
    hide(Editor::wf);
    proof { empty_sums(customer_name_delta_seq_view(deltas@.take(0))); }
    let mut index = 0usize;
    while index < deltas.len()
        invariant index <= deltas.len(), editor.wf(), remembered(seen@, keys@), bounded_keys(keys@),
            forall|key: i128| #[trigger] old(editor).at(key) is Some ==> editor.at(key) is Some,
            forall|key: i128| #[trigger] keys@.contains(key) ==> editor.at(key) is Some,
            forall|key: i128| #[trigger] old(keys)@.contains(key) ==> keys@.contains(key),
            forall|key: i128| !keys@.contains(key) ==> #[trigger] editor.at(key) == old(editor).at(key),
            forall|key: i128| #[trigger] customer_shape(editor.at(key)),
            forall|key: i128, name: Seq<u8>| #[trigger] customer_count(editor.at(key), name) == customer_count(old(editor).at(key), name) + sum(customer_name_delta_seq_view(deltas@.take(index as int)), dcustomer(key as int, name)),
        decreases deltas.len() - index,
    {
        let delta = &deltas[index];
        remember(delta.row.c_custkey as i128, seen, keys);
        change_customer(editor, delta.row.c_custkey, &delta.row.c_name, delta.sign);
        proof {
            assert forall|key: i128, name: Seq<u8>| #[trigger] customer_count(editor.at(key), name) == customer_count(old(editor).at(key), name) + sum(customer_name_delta_seq_view(deltas@.take(index as int + 1)), dcustomer(key as int, name)) by {
                let item = (customer_name_input_view(delta.row), delta.sign as int);
                if key as int == delta.row.c_custkey as int && name == delta.row.c_name@ { assert(dcustomer(key as int, name)(item) == delta.sign as int); } else { assert(dcustomer(key as int, name)(item) == 0); }
                projected_prefix(deltas@, index as int, |input: CustomerNameDelta| (customer_name_input_view(input.row), input.sign as int), dcustomer(key as int, name));
            }
        }
        index += 1;
    }
    proof { assert(deltas@.take(index as int) =~= deltas@); }
}

pub proof fn lemma_clean_ready(db: Db, key: int, row: &OrderData)
    requires row.shape(), row.count@ == sum(db.lineitem, nw(key)), row.quantity@ == sum(db.lineitem, qw(key)),
        forall|person: int, day: int, total: int| #[trigger] row.multiplicity(person, day, total) == sum(db.orders, ow(key, person, day, total)),
    ensures row.count@ >= 0, row.count@ == 0 ==> row.quantity@ == 0,
        forall|index: int| 0 <= index < row.variants.len() ==> #[trigger] row.variants@[index].count@ >= 0,
{
    assert forall|i: int| 0 <= i < db.lineitem.len() implies nw(key)(#[trigger] db.lineitem[i]) >= 0 by {}
    lemma_sum_nonneg(db.lineitem, nw(key));
    if row.count@ == 0 {
        assert forall|i: int| 0 <= i < db.lineitem.len() implies qw(key)(#[trigger] db.lineitem[i]) == 0 by {
            if db.lineitem[i].l_orderkey == key { lemma_sum_pos(db.lineitem, nw(key), i); }
        }
        lemma_sum_zero(db.lineitem, qw(key));
    }
    assert forall|index: int| 0 <= index < row.variants.len() implies #[trigger] row.variants@[index].count@ >= 0 by {
        let variant = row.variants@[index];
        row.variant_count(index);
        let weight = ow(key, variant.customer as int, variant.date as int, variant.price as int);
        assert forall|i: int| 0 <= i < db.orders.len() implies weight(#[trigger] db.orders[i]) >= 0 by {}
        lemma_sum_nonneg(db.orders, weight);
    }
}

pub proof fn lemma_clean_ready_customer(db: Db, customer: int, row: &CustomerData)
    requires row.shape(),
        forall|name: Seq<u8>| #[trigger] row.multiplicity(name) == sum(db.customer, cw(customer, name)),
    ensures forall|index: int| 0 <= index < row.names.len() ==> #[trigger] row.names@[index].count@ >= 0,
{
    assert forall|index: int| 0 <= index < row.names.len() implies #[trigger] row.names@[index].count@ >= 0 by {
        row.name_count(index);
        let weight = cw(customer, row.names@[index].name@);
        assert forall|i: int| 0 <= i < db.customer.len() implies weight(#[trigger] db.customer[i]) >= 0 by {}
        lemma_sum_nonneg(db.customer, weight);
    }
}

/// A duplicate-free list of order keys that all have lines has at most one entry per line.
pub proof fn lemma_distinct_keys_few(db: Db, keys: Seq<i64>)
    requires keys.no_duplicates(), forall|k: i64| keys.contains(k) ==> sum(db.lineitem, nw(k as int)) > 0,
    ensures keys.len() <= db.lineitem.len(),
{
    broadcast use vstd::seq_lib::group_seq_properties;
    let widen = |k: i64| k as int;
    assert(vstd::relations::injective(widen));
    keys.lemma_no_duplicates_injective(widen);
    let wide = keys.map_values(widen);
    wide.unique_seq_to_set();
    let ordered = db.lineitem.map_values(|row: LineitemRow| row.l_orderkey);
    ordered.lemma_cardinality_of_set();
    assert forall|v: int| wide.to_set().contains(v) implies ordered.to_set().contains(v) by {
        assert(wide.contains(v));
        let at = choose|at: int| 0 <= at < wide.len() && wide[at] == v;
        assert(keys.contains(keys[at]));
        if !ordered.contains(v) {
            assert forall|i: int| 0 <= i < db.lineitem.len() implies nw(v)(#[trigger] db.lineitem[i]) == 0 by {
                assert(ordered[i] == db.lineitem[i].l_orderkey);
            }
            lemma_sum_zero(db.lineitem, nw(v));
        }
    }
    vstd::set_lib::lemma_len_subset(wide.to_set(), ordered.to_set());
}

}

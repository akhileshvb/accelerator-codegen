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

pub fn apply_lines(editor: &mut Editor<OrderData>, deltas: &[LineQuantityDelta], seen: &mut HashMap<i128, bool>, keys: &mut Vec<i128>) -> (success: bool)
    requires old(editor).wf(), remembered(old(seen)@, old(keys)@), bounded_keys(old(keys)@),
        forall|key: i128| #[trigger] order_shape(old(editor).at(key)),
    ensures final(editor).wf(), remembered(final(seen)@, final(keys)@),
        bounded_keys(final(keys)@),
        forall|key: i128| #[trigger] old(keys)@.contains(key) ==> final(keys)@.contains(key),
        forall|key: i128| !final(keys)@.contains(key) ==> #[trigger] final(editor).at(key) == old(editor).at(key),
        forall|key: i128| #[trigger] order_shape(final(editor).at(key)),
        success ==> forall|key: i128| #[trigger] line_count(final(editor).at(key)) == line_count(old(editor).at(key)) + sum(line_quantity_delta_seq_view(deltas@), dn(key as int)),
        success ==> forall|key: i128| #[trigger] line_sum(final(editor).at(key)) == line_sum(old(editor).at(key)) + sum(line_quantity_delta_seq_view(deltas@), dq(key as int)),
        forall|key: i128, customer: int, date: int, price: int| #[trigger] order_count(final(editor).at(key), customer, date, price) == order_count(old(editor).at(key), customer, date, price),
{
    hide(sum);
    hide(Editor::at);
    hide(Editor::wf);
    proof { empty_sums(line_quantity_delta_seq_view(deltas@.take(0))); }
    let mut index = 0usize;
    while index < deltas.len()
        invariant index <= deltas.len(), editor.wf(), remembered(seen@, keys@), bounded_keys(keys@),
            forall|key: i128| #[trigger] old(keys)@.contains(key) ==> keys@.contains(key),
            forall|key: i128| !keys@.contains(key) ==> #[trigger] editor.at(key) == old(editor).at(key),
            forall|key: i128| #[trigger] order_shape(editor.at(key)),
            forall|key: i128| #[trigger] line_count(editor.at(key)) == line_count(old(editor).at(key)) + sum(line_quantity_delta_seq_view(deltas@.take(index as int)), dn(key as int)),
            forall|key: i128| #[trigger] line_sum(editor.at(key)) == line_sum(old(editor).at(key)) + sum(line_quantity_delta_seq_view(deltas@.take(index as int)), dq(key as int)),
            forall|key: i128, customer: int, date: int, price: int| #[trigger] order_count(editor.at(key), customer, date, price) == order_count(old(editor).at(key), customer, date, price),
        decreases deltas.len() - index,
    {
        let delta = &deltas[index];
        if delta.sign != 1 && delta.sign != -1 { return false; }
        remember(delta.row.l_orderkey as i128, seen, keys);
        if !change_line(editor, delta.row.l_orderkey, delta.row.l_quantity, delta.sign) { return false; }
        proof {
            assert forall|key: i128| #[trigger] line_count(editor.at(key)) == line_count(old(editor).at(key)) + sum(line_quantity_delta_seq_view(deltas@.take(index as int + 1)), dn(key as int)) by {
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
    true
}

pub fn apply_orders(editor: &mut Editor<OrderData>, deltas: &[OrderGroupDelta], seen: &mut HashMap<i128, bool>, keys: &mut Vec<i128>) -> (success: bool)
    requires old(editor).wf(), remembered(old(seen)@, old(keys)@), bounded_keys(old(keys)@),
        forall|key: i128| #[trigger] order_shape(old(editor).at(key)),
    ensures final(editor).wf(), remembered(final(seen)@, final(keys)@),
        bounded_keys(final(keys)@),
        forall|key: i128| #[trigger] old(keys)@.contains(key) ==> final(keys)@.contains(key),
        forall|key: i128| !final(keys)@.contains(key) ==> #[trigger] final(editor).at(key) == old(editor).at(key),
        forall|key: i128| #[trigger] order_shape(final(editor).at(key)),
        forall|key: i128| #[trigger] line_count(final(editor).at(key)) == line_count(old(editor).at(key)),
        forall|key: i128| #[trigger] line_sum(final(editor).at(key)) == line_sum(old(editor).at(key)),
        success ==> forall|key: i128, customer: int, date: int, price: int| #[trigger] order_count(final(editor).at(key), customer, date, price) == order_count(old(editor).at(key), customer, date, price) + sum(order_group_delta_seq_view(deltas@), dorder(key as int, customer, date, price)),
{
    hide(sum);
    hide(Editor::at);
    hide(Editor::wf);
    proof { empty_sums(order_group_delta_seq_view(deltas@.take(0))); }
    let mut index = 0usize;
    while index < deltas.len()
        invariant index <= deltas.len(), editor.wf(), remembered(seen@, keys@), bounded_keys(keys@),
            forall|key: i128| #[trigger] old(keys)@.contains(key) ==> keys@.contains(key),
            forall|key: i128| !keys@.contains(key) ==> #[trigger] editor.at(key) == old(editor).at(key),
            forall|key: i128| #[trigger] order_shape(editor.at(key)),
            forall|key: i128| #[trigger] line_count(editor.at(key)) == line_count(old(editor).at(key)),
            forall|key: i128| #[trigger] line_sum(editor.at(key)) == line_sum(old(editor).at(key)),
            forall|key: i128, customer: int, date: int, price: int| #[trigger] order_count(editor.at(key), customer, date, price) == order_count(old(editor).at(key), customer, date, price) + sum(order_group_delta_seq_view(deltas@.take(index as int)), dorder(key as int, customer, date, price)),
        decreases deltas.len() - index,
    {
        let delta = &deltas[index];
        if delta.sign != 1 && delta.sign != -1 { return false; }
        remember(delta.row.o_orderkey as i128, seen, keys);
        if !change_order(editor, delta.row.o_orderkey, delta.row.o_custkey, delta.row.o_orderdate, delta.row.o_totalprice, delta.sign) { return false; }
        proof {
            assert forall|key: i128, customer: int, date: int, price: int| #[trigger] order_count(editor.at(key), customer, date, price) == order_count(old(editor).at(key), customer, date, price) + sum(order_group_delta_seq_view(deltas@.take(index as int + 1)), dorder(key as int, customer, date, price)) by {
                projected_prefix(deltas@, index as int, |input: OrderGroupDelta| (order_group_input_view(input.row), input.sign as int), dorder(key as int, customer, date, price));
            }
        }
        index += 1;
    }
    proof { assert(deltas@.take(index as int) =~= deltas@); }
    true
}

pub fn apply_customers(editor: &mut Editor<CustomerData>, deltas: &Vec<CustomerNameDelta>, seen: &mut HashMap<i128, bool>, keys: &mut Vec<i128>) -> (success: bool)
    requires old(editor).wf(), remembered(old(seen)@, old(keys)@), bounded_keys(old(keys)@),
        forall|key: i128| #[trigger] customer_shape(old(editor).at(key)),
    ensures final(editor).wf(), remembered(final(seen)@, final(keys)@),
        bounded_keys(final(keys)@),
        forall|key: i128| #[trigger] old(keys)@.contains(key) ==> final(keys)@.contains(key),
        forall|key: i128| !final(keys)@.contains(key) ==> #[trigger] final(editor).at(key) == old(editor).at(key),
        forall|key: i128| #[trigger] customer_shape(final(editor).at(key)),
        success ==> forall|key: i128, name: Seq<u8>| #[trigger] customer_count(final(editor).at(key), name) == customer_count(old(editor).at(key), name) + sum(customer_name_delta_seq_view(deltas@), dcustomer(key as int, name)),
{
    hide(sum);
    hide(Editor::at);
    hide(Editor::wf);
    proof { empty_sums(customer_name_delta_seq_view(deltas@.take(0))); }
    let mut index = 0usize;
    while index < deltas.len()
        invariant index <= deltas.len(), editor.wf(), remembered(seen@, keys@), bounded_keys(keys@),
            forall|key: i128| #[trigger] old(keys)@.contains(key) ==> keys@.contains(key),
            forall|key: i128| !keys@.contains(key) ==> #[trigger] editor.at(key) == old(editor).at(key),
            forall|key: i128| #[trigger] customer_shape(editor.at(key)),
            forall|key: i128, name: Seq<u8>| #[trigger] customer_count(editor.at(key), name) == customer_count(old(editor).at(key), name) + sum(customer_name_delta_seq_view(deltas@.take(index as int)), dcustomer(key as int, name)),
        decreases deltas.len() - index,
    {
        let delta = &deltas[index];
        if delta.sign != 1 && delta.sign != -1 { return false; }
        remember(delta.row.c_custkey as i128, seen, keys);
        if !change_customer(editor, delta.row.c_custkey, &delta.row.c_name, delta.sign) { return false; }
        proof {
            assert forall|key: i128, name: Seq<u8>| #[trigger] customer_count(editor.at(key), name) == customer_count(old(editor).at(key), name) + sum(customer_name_delta_seq_view(deltas@.take(index as int + 1)), dcustomer(key as int, name)) by {
                projected_prefix(deltas@, index as int, |input: CustomerNameDelta| (customer_name_input_view(input.row), input.sign as int), dcustomer(key as int, name));
            }
        }
        index += 1;
    }
    proof { assert(deltas@.take(index as int) =~= deltas@); }
    true
}

}

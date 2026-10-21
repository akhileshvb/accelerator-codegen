use vstd::prelude::*;
use verus_common::sums::*;
use crate::table::*;
use crate::gen_spec::*;
use crate::gen_obligations::*;
use crate::num::Num;

verus! {

#[verifier::opaque]
pub open spec fn customer_effect(rows: Seq<Cell>, part: Cell, supplier: Cell, order: Cell, key: Major, minor: Major, sign: int, volume: int, measure: bool) -> int {
    sum(rows, |customer: Cell| if key == (customer.first, supplier.first, 0) && minor == (part.first, order.second, 0) {
        sign * part.count@ * supplier.count@ * order.count@ * customer.count@
            * if measure { volume } else { 1 }
    } else { 0 })
}

#[verifier::opaque]
pub open spec fn order_effect(rows: Seq<Cell>, customers: Table, part: Cell, supplier: Cell, key: Major, minor: Major, sign: int, volume: int, measure: bool) -> int {
    sum(rows, |order: Cell| customer_effect(customers.rows((order.first, 0, 0)), part, supplier, order, key, minor, sign, volume, measure))
}

#[verifier::opaque]
pub open spec fn supplier_effect(rows: Seq<Cell>, orders: Seq<Cell>, customers: Table, part: Cell, key: Major, minor: Major, sign: int, volume: int, measure: bool) -> int {
    sum(rows, |supplier: Cell| order_effect(orders, customers, part, supplier, key, minor, sign, volume, measure))
}

#[verifier::opaque]
pub open spec fn part_effect(rows: Seq<Cell>, suppliers: Seq<Cell>, orders: Seq<Cell>, customers: Table, key: Major, minor: Major, sign: int, volume: int, measure: bool) -> int {
    sum(rows, |part: Cell| supplier_effect(suppliers, orders, customers, part, key, minor, sign, volume, measure))
}

pub open spec fn line_effect(parts: Table, suppliers: Table, customers: Table, orders: Table, line: LineitemRow, key: Major, minor: Major, sign: int, measure: bool) -> int {
    part_effect(parts.rows((line.l_partkey as i64, 0, 0)), suppliers.rows((line.l_suppkey as i64, 0, 0)),
        orders.rows((line.l_orderkey as i64, 0, 0)), customers, key, minor, sign,
        line.l_extendedprice * (100 - line.l_discount), measure)
}

pub open spec fn rescan_selected(line: LineInput, required_orders: crate::maintenance::Keys, required_parts: crate::maintenance::Keys, required_suppliers: crate::maintenance::Keys, mode: int) -> bool {
    if mode == 0 { required_orders.has(line.l_orderkey) }
    else if mode == 1 { required_parts.has(line.l_partkey) && !required_orders.has(line.l_orderkey) }
    else { required_suppliers.has(line.l_suppkey) && !required_orders.has(line.l_orderkey) && !required_parts.has(line.l_partkey) }
}

pub open spec fn rescan_weight(line: LineInput, required_orders: crate::maintenance::Keys, required_parts: crate::maintenance::Keys, required_suppliers: crate::maintenance::Keys, mode: int,
    parts: Table, suppliers: Table, customers: Table, orders: Table, before: crate::model::Data, key: Major, minor: Major, measure: bool) -> int {
    if rescan_selected(line, required_orders, required_parts, required_suppliers, mode) {
        line_effect(parts, suppliers, customers, orders, line_input_view(line), key, minor, 1, measure)
            + line_effect(before.parts, before.suppliers, before.customers, before.orders, line_input_view(line), key, minor, -1, measure)
    } else { 0 }
}

#[verifier::opaque]
pub open spec fn rescan_effect(rows: Seq<LineInput>, required_orders: crate::maintenance::Keys, required_parts: crate::maintenance::Keys, required_suppliers: crate::maintenance::Keys, mode: int,
    parts: Table, suppliers: Table, customers: Table, orders: Table, before: crate::model::Data, key: Major, minor: Major, measure: bool) -> int {
    sum(rows, |line: LineInput| rescan_weight(line, required_orders, required_parts, required_suppliers, mode,
        parts, suppliers, customers, orders, before, key, minor, measure))
}

fn add_customers(rows: &[Cell], part: &Cell, supplier: &Cell, order: &Cell, sign: i128, volume: i128, output: &mut Work)
    requires old(output).valid(),
    ensures final(output).valid(),
        forall|key: Major, minor: Major, measure: bool| #[trigger] final(output).at(key, minor, measure)
            == old(output).at(key, minor, measure) + customer_effect(rows@, *part, *supplier, *order, key, minor, sign as int, volume as int, measure),
{
    let ghost initial = *output;
    let mut index = 0;
    proof { reveal(customer_effect); }
    while index < rows.len()
        invariant index <= rows.len(), output.valid(),
            forall|key: Major, minor: Major, measure: bool| #[trigger] output.at(key, minor, measure)
                == initial.at(key, minor, measure) + customer_effect(rows@.take(index as int), *part, *supplier, *order, key, minor, sign as int, volume as int, measure),
        decreases rows.len() - index,
    {
        let customer = &rows[index];
        let count = Num::Small(sign).mul(&part.count).mul(&supplier.count).mul(&order.count).mul(&customer.count);
        let measured = count.mul(&Num::Small(volume));
        output.add((customer.first, supplier.first, 0), (part.first, order.second, 0), count, measured);
        proof {
            assert forall|key: Major, minor: Major, measure: bool| #[trigger] output.at(key, minor, measure)
                == initial.at(key, minor, measure) + customer_effect(rows@.take(index as int + 1), *part, *supplier, *order, key, minor, sign as int, volume as int, measure) by {
                reveal(customer_effect);
                lemma_sum_take(rows@, index as int, |customer: Cell|
                    if key == (customer.first, supplier.first, 0) && minor == (part.first, order.second, 0) {
                        sign as int * part.count@ * supplier.count@ * order.count@ * customer.count@
                            * if measure { volume as int } else { 1 }
                    } else { 0 });
            }
        }
        index += 1;
    }
    proof { assert(rows@.take(rows@.len() as int) =~= rows@); }
}

fn add_orders(rows: &[Cell], customers: &Table, part: &Cell, supplier: &Cell, sign: i128, volume: i128, output: &mut Work)
    requires customers.valid(), old(output).valid(),
    ensures final(output).valid(),
        forall|key: Major, minor: Major, measure: bool| #[trigger] final(output).at(key, minor, measure)
            == old(output).at(key, minor, measure) + order_effect(rows@, *customers, *part, *supplier, key, minor, sign as int, volume as int, measure),
{
    let ghost initial = *output;
    let mut index = 0;
    proof { reveal(order_effect); }
    while index < rows.len()
        invariant index <= rows.len(), customers.valid(), output.valid(),
            forall|key: Major, minor: Major, measure: bool| #[trigger] output.at(key, minor, measure)
                == initial.at(key, minor, measure) + order_effect(rows@.take(index as int), *customers, *part, *supplier, key, minor, sign as int, volume as int, measure),
        decreases rows.len() - index,
    {
        let order = &rows[index];
        let neighbors = customers.get((order.first, 0, 0));
        add_customers(neighbors.as_slice(), part, supplier, order, sign, volume, output);
        proof {
            assert forall|key: Major, minor: Major, measure: bool| #[trigger] output.at(key, minor, measure)
                == initial.at(key, minor, measure) + order_effect(rows@.take(index as int + 1), *customers, *part, *supplier, key, minor, sign as int, volume as int, measure) by {
                reveal(order_effect);
                lemma_sum_take(rows@, index as int, |order: Cell|
                    customer_effect(customers.rows((order.first, 0, 0)), *part, *supplier, order, key, minor, sign as int, volume as int, measure));
            }
        }
        index += 1;
    }
    proof { assert(rows@.take(rows@.len() as int) =~= rows@); }
}

fn add_suppliers(rows: &[Cell], orders: &[Cell], customers: &Table, part: &Cell, sign: i128, volume: i128, output: &mut Work)
    requires customers.valid(), old(output).valid(),
    ensures final(output).valid(),
        forall|key: Major, minor: Major, measure: bool| #[trigger] final(output).at(key, minor, measure)
            == old(output).at(key, minor, measure) + supplier_effect(rows@, orders@, *customers, *part, key, minor, sign as int, volume as int, measure),
{
    let ghost initial = *output;
    let mut index = 0;
    proof { reveal(supplier_effect); }
    while index < rows.len()
        invariant index <= rows.len(), customers.valid(), output.valid(),
            forall|key: Major, minor: Major, measure: bool| #[trigger] output.at(key, minor, measure)
                == initial.at(key, minor, measure) + supplier_effect(rows@.take(index as int), orders@, *customers, *part, key, minor, sign as int, volume as int, measure),
        decreases rows.len() - index,
    {
        add_orders(orders, customers, part, &rows[index], sign, volume, output);
        proof {
            assert forall|key: Major, minor: Major, measure: bool| #[trigger] output.at(key, minor, measure)
                == initial.at(key, minor, measure) + supplier_effect(rows@.take(index as int + 1), orders@, *customers, *part, key, minor, sign as int, volume as int, measure) by {
                reveal(supplier_effect);
                lemma_sum_take(rows@, index as int, |supplier: Cell|
                    order_effect(orders@, *customers, *part, supplier, key, minor, sign as int, volume as int, measure));
            }
        }
        index += 1;
    }
    proof { assert(rows@.take(rows@.len() as int) =~= rows@); }
}

/// extendedprice * (100 - discount): an i64 times a value just past an i64 always fits an i128.
fn line_volume(line: &LineInput) -> (result: i128)
    ensures result as int == line.l_extendedprice as int * (100 - line.l_discount as int),
{
    let price = line.l_extendedprice as i128;
    let rest = 100i128 - line.l_discount as i128;
    proof {
        assert(-9223372036854775808int <= price as int <= 9223372036854775807int);
        assert(-9223372036854775707int <= rest as int <= 9223372036854775908int);
        assert(price as int * rest as int <= 9223372036854775808int * 9223372036854775908int) by (nonlinear_arith)
            requires -9223372036854775808int <= price as int <= 9223372036854775807int, -9223372036854775707int <= rest as int <= 9223372036854775908int;
        assert(price as int * rest as int >= -9223372036854775808int * 9223372036854775908int) by (nonlinear_arith)
            requires -9223372036854775808int <= price as int <= 9223372036854775807int, -9223372036854775707int <= rest as int <= 9223372036854775908int;
    }
    price * rest
}

pub fn line_contribution(parts: &Table, suppliers: &Table, customers: &Table, orders: &Table, line: LineInput, sign: i128, output: &mut Work)
    requires parts.valid(), suppliers.valid(), customers.valid(), orders.valid(), old(output).valid(),
    ensures final(output).valid(),
        forall|key: Major, minor: Major, measure: bool| #[trigger] final(output).at(key, minor, measure)
            == old(output).at(key, minor, measure) + line_effect(*parts, *suppliers, *customers, *orders, line_input_view(line), key, minor, sign as int, measure),
{
    let types = parts.get((line.l_partkey, 0, 0));
    let nations = suppliers.get((line.l_suppkey, 0, 0));
    let order_rows = orders.get((line.l_orderkey, 0, 0));
    let volume = line_volume(&line);
    let ghost initial = *output;
    let mut index = 0;
    proof { reveal(part_effect); }
    while index < types.len()
        invariant index <= types.len(), customers.valid(), output.valid(),
            (*types)@ == parts.rows((line.l_partkey, 0, 0)), (*nations)@ == suppliers.rows((line.l_suppkey, 0, 0)),
            (*order_rows)@ == orders.rows((line.l_orderkey, 0, 0)),
            forall|key: Major, minor: Major, measure: bool| #[trigger] output.at(key, minor, measure)
                == initial.at(key, minor, measure) + part_effect((*types)@.take(index as int), (*nations)@, (*order_rows)@, *customers, key, minor, sign as int, volume as int, measure),
        decreases types.len() - index,
    {
        add_suppliers(nations.as_slice(), order_rows.as_slice(), customers, &types[index], sign, volume, output);
        proof {
            assert forall|key: Major, minor: Major, measure: bool| #[trigger] output.at(key, minor, measure)
                == initial.at(key, minor, measure) + part_effect((*types)@.take(index as int + 1), (*nations)@, (*order_rows)@, *customers, key, minor, sign as int, volume as int, measure) by {
                reveal(part_effect);
                lemma_sum_take((*types)@, index as int, |part: Cell|
                    supplier_effect((*nations)@, (*order_rows)@, *customers, part, key, minor, sign as int, volume as int, measure));
            }
        }
        index += 1;
    }
    proof { assert((*types)@.take((*types)@.len() as int) =~= (*types)@); }
}

}

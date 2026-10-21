use vstd::prelude::*;
use std::collections::HashMap;
use crate::gen_obligations::*;
use crate::storage::*;
use verus_common::sums::*;
use verus_prooflib::wide::Wide;

verus! {

broadcast use vstd::seq_lib::group_seq_properties;

pub open spec fn joined_weight(customers: Table, orders: Table, suppliers: Table,
    line: LineitemInput, nation: i64, day: i32) -> int {
    sum(orders.rows(line.l_orderkey), |order: Cell| if order.day == day {
        order.count as int * customers.at(order.sub, nation, 0).0 * suppliers.at(line.l_suppkey, nation, 0).0
    } else { 0 })
}

pub open spec fn contribution(customers: Table, orders: Table, suppliers: Table,
    line: LineitemInput, nation: i64, day: i32) -> (int, int) {
    let count = joined_weight(customers, orders, suppliers, line, nation, day);
    (count, count * line.l_extendedprice * (100 - line.l_discount as int))
}

pub open spec fn difference(before: (Table, Table, Table), after: (Table, Table, Table),
    line: LineitemInput, nation: i64, day: i32) -> (int, int) {
    let old = contribution(before.0, before.1, before.2, line, nation, day);
    let new = contribution(after.0, after.1, after.2, line, nation, day);
    (new.0 - old.0, new.1 - old.1)
}

pub open spec fn order_weight(customers: Table, suppliers: Table, line: LineitemInput,
    order: Cell, nation: i64, sub: i64, day: i32, factor: int) -> int {
    if sub == 0 && order.day == day {
        factor * order.count * customers.at(order.sub, nation, 0).0 * suppliers.at(line.l_suppkey, nation, 0).0
    } else { 0 }
}

pub open spec fn customer_weight(suppliers: Table, line: LineitemInput, order: Cell,
    customer: Cell, nation: i64, sub: i64, day: i32, factor: int) -> int {
    if sub == 0 && order.day == day && customer.sub == nation {
        factor * order.count * customer.count * suppliers.at(line.l_suppkey, nation, 0).0
    } else { 0 }
}

pub proof fn lemma_customer_weight(customers: Table, suppliers: Table, line: LineitemInput,
    order: Cell, group: Run, nation: i64, sub: i64, day: i32, factor: int)
    requires group.wf(), group.rows@ == customers.rows(order.sub),
        forall|target: i64, date: i32| #[trigger] group.at(target, date) == customers.at(order.sub, target, date),
        forall|index: int| 0 <= index < group.rows@.len() ==> (#[trigger] group.rows@[index]).day == 0,
    ensures sum(group.rows@, |customer: Cell| customer_weight(suppliers, line, order, customer, nation, sub, day, factor)) ==
        order_weight(customers, suppliers, line, order, nation, sub, day, factor),
{
    let base = |customer: Cell| if customer.sub == nation && customer.day == 0 { customer.count as int } else { 0 };
    let weighted = |customer: Cell| customer_weight(suppliers, line, order, customer, nation, sub, day, factor);
    let scalar = if sub == 0 && order.day == day { factor * order.count * suppliers.at(line.l_suppkey, nation, 0).0 } else { 0 };
    assert forall|index: int| 0 <= index < group.rows@.len() implies weighted(#[trigger] group.rows@[index]) == scalar * base(group.rows@[index]) by {
        let customer = group.rows@[index];
        assert(customer.day == 0);
        assert(factor * order.count * customer.count * suppliers.at(line.l_suppkey, nation, 0).0 ==
            (factor * order.count * suppliers.at(line.l_suppkey, nation, 0).0) * customer.count) by (nonlinear_arith);
    }
    lemma_sum_ext(group.rows@, weighted, |customer: Cell| scalar * base(customer));
    lemma_sum_scale(group.rows@, base, |customer: Cell| scalar * base(customer), scalar);
    group.lemma_at(nation, 0);
    assert((factor * order.count * suppliers.at(line.l_suppkey, nation, 0).0) * customers.at(order.sub, nation, 0).0 ==
        factor * order.count * customers.at(order.sub, nation, 0).0 * suppliers.at(line.l_suppkey, nation, 0).0) by (nonlinear_arith);
}

pub proof fn lemma_revenue_sum(rows: Seq<Cell>, weight: spec_fn(Cell) -> int, amount: int)
    ensures sum(rows, |cell: Cell| weight(cell) * amount) == sum(rows, weight) * amount,
{
    assert forall|index: int| 0 <= index < rows.len() implies
        weight(#[trigger] rows[index]) * amount == amount * weight(rows[index]) by {
        assert(weight(rows[index]) * amount == amount * weight(rows[index])) by (nonlinear_arith);
    }
    lemma_sum_scale(rows, weight, |cell: Cell| weight(cell) * amount, amount);
    assert(amount * sum(rows, weight) == sum(rows, weight) * amount) by (nonlinear_arith);
}

pub fn add_key(keys: &mut Vec<i64>, seen: &mut HashMap<i64, bool>, key: i64)
    requires forall|value: i64| #[trigger] old(seen)@.contains_key(value) <==> old(keys)@.contains(value),
    ensures final(keys)@ == if old(seen)@.contains_key(key) { old(keys)@ } else { old(keys)@.push(key) },
        forall|value: i64| #[trigger] final(seen)@.contains_key(value) <==> final(keys)@.contains(value),
        forall|value: i64| #[trigger] final(keys)@.contains(value) <==> old(keys)@.contains(value) || value == key,
{
    if !seen.contains_key(&key) {
        proof { verus_prooflib::lemma_push_membership(keys@, key); }
        seen.insert(key, true);
        keys.push(key);
    }
}

pub fn key_map(keys: &[i64]) -> (out: HashMap<i64, bool>)
    ensures forall|key: i64| #[trigger] out@.contains_key(key) <==> keys@.contains(key),
{
    let mut out = HashMap::new();
    let mut index = 0;
    while index < keys.len()
        invariant index <= keys.len(),
            forall|key: i64| #[trigger] out@.contains_key(key) <==> keys@.take(index as int).contains(key),
        decreases keys.len() - index,
    {
        proof {
            assert(keys@.take(index as int + 1) =~= keys@.take(index as int).push(keys@[index as int]));
            verus_prooflib::lemma_push_membership(keys@.take(index as int), keys@[index as int]);
        }
        out.insert(keys[index], true);
        index += 1;
    }
    proof { assert(keys@.take(keys@.len() as int) =~= keys@); }
    out
}

pub open spec fn affected_order(reverse: Table, customers: Seq<CustomerDelta>, orders: Seq<OrdersDelta>, key: i64) -> bool {
    sum(customers, |delta: CustomerDelta| ind(reverse.at(delta.row.c_custkey, key, 0).0 > 0)) > 0
        || orders.map_values(|delta: OrdersDelta| delta.row.o_orderkey).contains(key)
}

pub open spec fn affected_supplier(suppliers: Seq<SupplierDelta>, key: i64) -> bool {
    suppliers.map_values(|delta: SupplierDelta| delta.row.s_suppkey).contains(key)
}

pub open spec fn extend_distinct(acc: Seq<i64>, keys: Seq<i64>) -> Seq<i64>
    decreases keys.len(),
{
    if keys.len() == 0 { acc } else {
        let before = extend_distinct(acc, keys.drop_last());
        if before.contains(keys.last()) { before } else { before.push(keys.last()) }
    }
}

pub proof fn lemma_extend_distinct(acc: Seq<i64>, keys: Seq<i64>)
    ensures forall|key: i64| #![trigger extend_distinct(acc, keys).contains(key)]
            extend_distinct(acc, keys).contains(key) <==> acc.contains(key) || keys.contains(key),
    decreases keys.len(),
{
    if keys.len() > 0 {
        let init = keys.drop_last();
        lemma_extend_distinct(acc, init);
        let before = extend_distinct(acc, init);
        let last = keys.last();
        verus_prooflib::lemma_push_membership(init, last);
        verus_prooflib::lemma_push_membership(before, last);
        assert(keys =~= init.push(last));
        assert forall|key: i64| #![trigger extend_distinct(acc, keys).contains(key)]
            extend_distinct(acc, keys).contains(key) <==> acc.contains(key) || keys.contains(key) by {
            if keys.contains(key) && key != keys.last() {
                let index = choose|index: int| 0 <= index < keys.len() && keys[index] == key;
                assert(init[index] == key);
            }
            if init.contains(key) {
                let index = choose|index: int| 0 <= index < init.len() && init[index] == key;
                assert(keys[index] == key);
            }
        }
    }
}

pub proof fn lemma_extend_distinct_concat(acc: Seq<i64>, first: Seq<i64>, second: Seq<i64>)
    ensures extend_distinct(acc, first + second) == extend_distinct(extend_distinct(acc, first), second),
    decreases second.len(),
{
    if second.len() == 0 {
        assert(first + second =~= first);
    } else {
        assert((first + second).drop_last() =~= first + second.drop_last());
        assert((first + second).last() == second.last());
        lemma_extend_distinct_concat(acc, first, second.drop_last());
    }
}

pub open spec fn customer_orders(reverse: Table, customer: i64) -> Seq<i64> {
    reverse.rows(customer).map_values(|cell: Cell| cell.sub)
}

pub open spec fn customers_orders(reverse: Table, customers: Seq<CustomerDelta>) -> Seq<i64>
    decreases customers.len(),
{
    if customers.len() == 0 { Seq::empty() } else {
        customers_orders(reverse, customers.drop_last()) + customer_orders(reverse, customers.last().row.c_custkey)
    }
}

pub open spec fn order_delta_keys(orders: Seq<OrdersDelta>) -> Seq<i64> {
    orders.map_values(|delta: OrdersDelta| delta.row.o_orderkey)
}

/// The orders whose lines are rescanned: those of the changed customers, then the changed orders.
pub open spec fn wanted_orders(reverse: Table, customers: Seq<CustomerDelta>, orders: Seq<OrdersDelta>) -> Seq<i64> {
    extend_distinct(extend_distinct(Seq::empty(), customers_orders(reverse, customers)), order_delta_keys(orders))
}

pub open spec fn supplier_delta_keys(suppliers: Seq<SupplierDelta>) -> Seq<i64> {
    suppliers.map_values(|delta: SupplierDelta| delta.row.s_suppkey)
}

pub open spec fn wanted_suppliers(suppliers: Seq<SupplierDelta>) -> Seq<i64> {
    extend_distinct(Seq::empty(), supplier_delta_keys(suppliers))
}

pub proof fn lemma_wanted_suppliers(suppliers: Seq<SupplierDelta>, key: i64)
    ensures wanted_suppliers(suppliers).contains(key) <==> affected_supplier(suppliers, key),
{
    lemma_extend_distinct(Seq::empty(), supplier_delta_keys(suppliers));
}

pub proof fn lemma_customers_orders(reverse: Table, customers: Seq<CustomerDelta>, key: i64)
    requires reverse.wf(), reverse.dimension(),
    ensures customers_orders(reverse, customers).contains(key) <==>
        sum(customers, |delta: CustomerDelta| ind(reverse.at(delta.row.c_custkey, key, 0).0 > 0)) > 0,
    decreases customers.len(),
{
    let weight = |delta: CustomerDelta| ind(reverse.at(delta.row.c_custkey, key, 0).0 > 0);
    if customers.len() > 0 {
        let init = customers.drop_last();
        lemma_customers_orders(reverse, init, key);
        reverse.lemma_support(customers.last().row.c_custkey, key);
        lemma_sum_nonneg(init, weight);
        let first = customers_orders(reverse, init);
        let last = customer_orders(reverse, customers.last().row.c_custkey);
        assert(customers_orders(reverse, customers) == first + last);
        if first.contains(key) { assert((first + last)[first.index_of(key)] == key); }
        if last.contains(key) { assert((first + last)[first.len() + last.index_of(key)] == key); }
        if (first + last).contains(key) {
            let index = choose|index: int| 0 <= index < (first + last).len() && (first + last)[index] == key;
            if index < first.len() { assert(first[index] == key); } else { assert(last[index - first.len()] == key); }
        }
    }
}

pub proof fn lemma_wanted_orders(reverse: Table, customers: Seq<CustomerDelta>, orders: Seq<OrdersDelta>, key: i64)
    requires reverse.wf(), reverse.dimension(),
    ensures wanted_orders(reverse, customers, orders).contains(key) <==> affected_order(reverse, customers, orders, key),
{
    lemma_customers_orders(reverse, customers, key);
    lemma_extend_distinct(Seq::empty(), customers_orders(reverse, customers));
    lemma_extend_distinct(extend_distinct(Seq::empty(), customers_orders(reverse, customers)), order_delta_keys(orders));
}

pub fn order_keys(reverse: &Table, customers: &[CustomerDelta], orders: &[OrdersDelta]) -> (out: Vec<i64>)
    requires reverse.wf(), reverse.dimension(),
    ensures out@ == wanted_orders(*reverse, customers@, orders@),
{
    let mut keys = Vec::new();
    let mut seen = HashMap::new();
    let mut index = 0;
    while index < customers.len()
        invariant reverse.wf(), reverse.dimension(), index <= customers.len(),
            forall|key: i64| #[trigger] seen@.contains_key(key) <==> keys@.contains(key),
            keys@ == extend_distinct(Seq::empty(), customers_orders(*reverse, customers@.take(index as int))),
        decreases customers.len() - index,
    {
        let ghost previous_keys = keys@;
        let customer = customers[index].row.c_custkey;
        proof {
            assert(customers@.take(index as int + 1).drop_last() =~= customers@.take(index as int));
            assert(customers@.take(index as int + 1).last() == customers@[index as int]);
        }
        match reverse.group(customer) {
            Some(group) => {
                let mut variant = 0;
                while variant < group.len()
                    invariant reverse.wf(), reverse.dimension(), group.wf(), variant <= group.rows@.len(),
                        forall|key: i64| #[trigger] seen@.contains_key(key) <==> keys@.contains(key),
                        group.rows@ == reverse.rows(customer),
                        keys@ == extend_distinct(previous_keys, group.rows@.take(variant as int).map_values(|cell: Cell| cell.sub)),
                    decreases group.rows@.len() - variant,
                {
                    add_key(&mut keys, &mut seen, group.cell(variant).sub);
                    proof {
                        assert(group.rows@.take(variant as int + 1).map_values(|cell: Cell| cell.sub).drop_last()
                            =~= group.rows@.take(variant as int).map_values(|cell: Cell| cell.sub));
                        assert(group.rows@.take(variant as int + 1).map_values(|cell: Cell| cell.sub).last() == group.rows@[variant as int].sub);
                    }
                    variant += 1;
                }
                proof {
                    assert(group.rows@.take(group.rows@.len() as int) =~= group.rows@);
                    assert(customer_orders(*reverse, customer) == group.rows@.map_values(|cell: Cell| cell.sub));
                    lemma_extend_distinct_concat(Seq::empty(), customers_orders(*reverse, customers@.take(index as int)), customer_orders(*reverse, customer));
                }
            },
            None => {
                proof {
                    assert(reverse.rows(customer).len() == 0);
                    assert(customer_orders(*reverse, customer) =~= Seq::<i64>::empty());
                    assert(customers_orders(*reverse, customers@.take(index as int + 1)) =~= customers_orders(*reverse, customers@.take(index as int)));
                }
            },
        }
        index += 1;
    }
    proof { assert(customers@.take(customers@.len() as int) =~= customers@); }
    let ghost customer_keys = keys@;
    let mut index = 0;
    while index < orders.len()
        invariant index <= orders.len(),
            forall|key: i64| #[trigger] seen@.contains_key(key) <==> keys@.contains(key),
            keys@ == extend_distinct(customer_keys, order_delta_keys(orders@).take(index as int)),
        decreases orders.len() - index,
    {
        let key = orders[index].row.o_orderkey;
        add_key(&mut keys, &mut seen, key);
        proof {
            assert(order_delta_keys(orders@).take(index as int + 1).drop_last() =~= order_delta_keys(orders@).take(index as int));
            assert(order_delta_keys(orders@).take(index as int + 1).last() == key);
        }
        index += 1;
    }
    proof { assert(order_delta_keys(orders@).take(orders@.len() as int) =~= order_delta_keys(orders@)); }
    keys
}

pub fn supplier_keys(suppliers: &[SupplierDelta]) -> (out: Vec<i64>)
    ensures out@ == wanted_suppliers(suppliers@),
{
    let mut keys = Vec::new();
    let mut seen = HashMap::new();
    let mut index = 0;
    while index < suppliers.len()
        invariant index <= suppliers.len(),
            forall|key: i64| #[trigger] seen@.contains_key(key) <==> keys@.contains(key),
            keys@ == extend_distinct(Seq::empty(), supplier_delta_keys(suppliers@).take(index as int)),
        decreases suppliers.len() - index,
    {
        let key = suppliers[index].row.s_suppkey;
        add_key(&mut keys, &mut seen, key);
        proof {
            assert(supplier_delta_keys(suppliers@).take(index as int + 1).drop_last() =~= supplier_delta_keys(suppliers@).take(index as int));
            assert(supplier_delta_keys(suppliers@).take(index as int + 1).last() == key);
        }
        index += 1;
    }
    proof { assert(supplier_delta_keys(suppliers@).take(suppliers@.len() as int) =~= supplier_delta_keys(suppliers@)); }
    keys
}

pub fn contribute(customers: &Table, orders: &Table, suppliers: &Table,
    line: LineitemInput, factor: i64, changes: &mut Bucket)
    requires customers.wf(), orders.wf(), suppliers.wf(), old(changes).wf(),
        customers.dimension(), orders.counts_nonnegative(), suppliers.dimension(),
    ensures final(changes).wf(), forall|nation: i64, sub: i64, day: i32|
        #[trigger] final(changes).at(nation, sub, day) ==
            (old(changes).at(nation, sub, day).0 + if sub == 0 { factor as int * contribution(*customers, *orders, *suppliers, line, nation, day).0 } else { 0 },
             old(changes).at(nation, sub, day).1 + if sub == 0 { factor as int * contribution(*customers, *orders, *suppliers, line, nation, day).1 } else { 0 }),
{
    let ghost entry = *changes;
    if factor == 0 { return; }
    let order_group = match orders.group(line.l_orderkey) {
        Some(group) => group,
        None => {
            proof {
                assert forall|nation: i64, day: i32| #[trigger] joined_weight(*customers, *orders, *suppliers, line, nation, day) == 0 by {
                    lemma_sum_empty(orders.rows(line.l_orderkey), |order: Cell| if order.day == day {
                        order.count as int * customers.at(order.sub, nation, 0).0 * suppliers.at(line.l_suppkey, nation, 0).0
                    } else { 0 });
                }
                assert forall|nation: i64, sub: i64, day: i32| #[trigger] changes.at(nation, sub, day) ==
                    (entry.at(nation, sub, day).0 + if sub == 0 { factor as int * contribution(*customers, *orders, *suppliers, line, nation, day).0 } else { 0 },
                     entry.at(nation, sub, day).1 + if sub == 0 { factor as int * contribution(*customers, *orders, *suppliers, line, nation, day).1 } else { 0 }) by {
                    assert(joined_weight(*customers, *orders, *suppliers, line, nation, day) == 0);
                    assert(contribution(*customers, *orders, *suppliers, line, nation, day).0 == joined_weight(*customers, *orders, *suppliers, line, nation, day));
                    assert(contribution(*customers, *orders, *suppliers, line, nation, day).1 == joined_weight(*customers, *orders, *suppliers, line, nation, day) * line.l_extendedprice * (100 - line.l_discount as int));
                    assert(0int * (line.l_extendedprice as int) * (100 - line.l_discount as int) == 0) by (nonlinear_arith);
                    assert(contribution(*customers, *orders, *suppliers, line, nation, day) == (0, 0));
                }
            }
            return;
        },
    };
    let supplier_group = match suppliers.group(line.l_suppkey) {
        Some(group) => group,
        None => {
            proof {
                assert forall|nation: i64, day: i32| #[trigger] joined_weight(*customers, *orders, *suppliers, line, nation, day) == 0 by {
                    lemma_sum_zero(orders.rows(line.l_orderkey), |order: Cell| if order.day == day {
                        order.count as int * customers.at(order.sub, nation, 0).0 * suppliers.at(line.l_suppkey, nation, 0).0
                    } else { 0 });
                }
                assert forall|nation: i64, sub: i64, day: i32| #[trigger] changes.at(nation, sub, day) ==
                    (entry.at(nation, sub, day).0 + if sub == 0 { factor as int * contribution(*customers, *orders, *suppliers, line, nation, day).0 } else { 0 },
                     entry.at(nation, sub, day).1 + if sub == 0 { factor as int * contribution(*customers, *orders, *suppliers, line, nation, day).1 } else { 0 }) by {
                    assert(joined_weight(*customers, *orders, *suppliers, line, nation, day) == 0);
                }
            }
            return;
        },
    };
    let amount = Wide::from_i128(line.l_extendedprice as i128).mul(&Wide::from_i128(100i128 - line.l_discount as i128));
    let factor_wide = Wide::from_i128(factor as i128);
    let mut order_index = 0;
    proof {
        assert forall|position: int| 0 <= position < order_group.rows@.len() implies
            (#[trigger] order_group.rows@[position]).count >= 0 by {
            let order = order_group.rows@[position];
            order_group.lemma_row(position);
            assert(orders.at(line.l_orderkey, order.sub, order.day) == (order.count as int, order.revenue as int));
        }
    }
    while order_index < order_group.len()
        invariant customers.wf(), customers.dimension(), orders.wf(), orders.counts_nonnegative(),
            suppliers.wf(), suppliers.dimension(), order_group.wf(), supplier_group.wf(), changes.wf(),
            order_index <= order_group.rows@.len(),
            entry == *old(changes), factor_wide@ == factor as int, amount@ == line.l_extendedprice * (100 - line.l_discount as int),
            order_group.rows@ == orders.rows(line.l_orderkey),
            forall|target: i64, date: i32| #[trigger] supplier_group.at(target, date) == suppliers.at(line.l_suppkey, target, date),
            forall|position: int| 0 <= position < order_group.rows@.len() ==>
                (#[trigger] order_group.rows@[position]).count >= 0,
            forall|nation: i64, sub: i64, day: i32| #[trigger] changes.at(nation, sub, day) ==
                (entry.at(nation, sub, day).0 + sum(order_group.rows@.take(order_index as int), |order: Cell| order_weight(*customers, *suppliers, line, order, nation, sub, day, factor as int)),
                 entry.at(nation, sub, day).1 + sum(order_group.rows@.take(order_index as int), |order: Cell| order_weight(*customers, *suppliers, line, order, nation, sub, day, factor as int) * amount@)),
        decreases order_group.rows@.len() - order_index,
    {
        let order = order_group.cell(order_index);
        let ghost before_order = *changes;
        match customers.group(order.sub) {
            Some(customer_group) => {
                let mut customer_index = 0;
                while customer_index < customer_group.len()
                    invariant customers.wf(), customers.dimension(), orders.wf(), orders.counts_nonnegative(),
                        suppliers.wf(), suppliers.dimension(), order_group.wf(), supplier_group.wf(), customer_group.wf(), changes.wf(),
                        customer_index <= customer_group.rows@.len(),
                        entry == *old(changes), order@.count >= 0, factor_wide@ == factor as int,
                        amount@ == line.l_extendedprice * (100 - line.l_discount as int),
                        order_index < order_group.rows@.len(), order@ == order_group.rows@[order_index as int],
                        order_group.rows@ == orders.rows(line.l_orderkey),
                        forall|position: int| 0 <= position < order_group.rows@.len() ==>
                            (#[trigger] order_group.rows@[position]).count >= 0,
                        customer_group.rows@ == customers.rows(order.sub),
                        forall|target: i64, date: i32| #[trigger] customer_group.at(target, date) == customers.at(order.sub, target, date),
                        forall|target: i64, date: i32| #[trigger] supplier_group.at(target, date) == suppliers.at(line.l_suppkey, target, date),
                        forall|position: int| 0 <= position < customer_group.rows@.len() ==>
                            (#[trigger] customer_group.rows@[position]).day == 0 && customer_group.rows@[position].count > 0,
                        forall|nation: i64, sub: i64, day: i32| #[trigger] before_order.at(nation, sub, day) ==
                            (entry.at(nation, sub, day).0 + sum(order_group.rows@.take(order_index as int), |row: Cell| order_weight(*customers, *suppliers, line, row, nation, sub, day, factor as int)),
                             entry.at(nation, sub, day).1 + sum(order_group.rows@.take(order_index as int), |row: Cell| order_weight(*customers, *suppliers, line, row, nation, sub, day, factor as int) * amount@)),
                        forall|nation: i64, sub: i64, day: i32| #[trigger] changes.at(nation, sub, day) ==
                            (before_order.at(nation, sub, day).0 + sum(customer_group.rows@.take(customer_index as int), |customer: Cell| customer_weight(*suppliers, line, order@, customer, nation, sub, day, factor as int)),
                             before_order.at(nation, sub, day).1 + sum(customer_group.rows@.take(customer_index as int), |customer: Cell| customer_weight(*suppliers, line, order@, customer, nation, sub, day, factor as int) * amount@)),
                    decreases customer_group.rows@.len() - customer_index,
                {
                    let customer = customer_group.cell(customer_index);
                    let supplier = supplier_group.get(customer.sub, 0);
                    let ghost before_customer = *changes;
                    proof { assert(supplier@.count >= 0); }
                    if customer.count.is_pos() && supplier.count.is_pos() && order.count.is_pos() {
                        let count = order.count.mul(&customer.count).mul(&supplier.count).mul(&factor_wide);
                        let revenue = count.mul(&amount);
                        proof {
                            assert(((order@.count * customer@.count) * supplier@.count) * factor as int ==
                                factor as int * order@.count * customer@.count * supplier@.count) by (nonlinear_arith);
                            assert(factor_wide@ == factor as int);
                            assert(count@ == ((order@.count * customer@.count) * supplier@.count) * factor as int);
                            assert(supplier@.count == suppliers.at(line.l_suppkey, customer.sub, 0).0);
                            assert(count@ == customer_weight(*suppliers, line, order@, customer@, customer.sub, 0, order.day, factor as int));
                            assert(revenue@ == customer_weight(*suppliers, line, order@, customer@, customer.sub, 0, order.day, factor as int) * amount@);
                        }
                        changes.add(customer.sub, 0, order.day, &count, &revenue);
                    } else {
                        proof {
                            assert(order@.count == 0 || supplier@.count == 0);
                            assert(factor as int * order@.count * customer@.count * supplier@.count == 0) by (nonlinear_arith)
                                requires order@.count == 0 || supplier@.count == 0;
                        }
                    }
                    proof {
                        assert forall|nation: i64, sub: i64, day: i32| #[trigger] changes.at(nation, sub, day) ==
                            (before_customer.at(nation, sub, day).0 + customer_weight(*suppliers, line, order@, customer@, nation, sub, day, factor as int),
                             before_customer.at(nation, sub, day).1 + customer_weight(*suppliers, line, order@, customer@, nation, sub, day, factor as int) * amount@) by {
                            if nation == customer.sub && sub == 0 && day == order.day {
                                assert(customer_weight(*suppliers, line, order@, customer@, nation, sub, day, factor as int) ==
                                    factor as int * order@.count * customer@.count * supplier@.count);
                                assert(order@.count == 0 || supplier@.count == 0 || (order@.count > 0 && supplier@.count > 0));
                            }
                        }
                        assert forall|nation: i64, sub: i64, day: i32| #[trigger] changes.at(nation, sub, day) ==
                            (before_order.at(nation, sub, day).0 + sum(customer_group.rows@.take(customer_index as int + 1), |row: Cell| customer_weight(*suppliers, line, order@, row, nation, sub, day, factor as int)),
                             before_order.at(nation, sub, day).1 + sum(customer_group.rows@.take(customer_index as int + 1), |row: Cell| customer_weight(*suppliers, line, order@, row, nation, sub, day, factor as int) * amount@)) by {
                            lemma_sum_take(customer_group.rows@, customer_index as int, |row: Cell| customer_weight(*suppliers, line, order@, row, nation, sub, day, factor as int));
                            lemma_sum_take(customer_group.rows@, customer_index as int, |row: Cell| customer_weight(*suppliers, line, order@, row, nation, sub, day, factor as int) * amount@);
                        }
                    }
                    customer_index += 1;
                }
                proof {
                    assert(customer_group.rows@.take(customer_group.rows@.len() as int) =~= customer_group.rows@);
                    assert forall|nation: i64, sub: i64, day: i32| #[trigger] changes.at(nation, sub, day) ==
                        (before_order.at(nation, sub, day).0 + order_weight(*customers, *suppliers, line, order@, nation, sub, day, factor as int),
                         before_order.at(nation, sub, day).1 + order_weight(*customers, *suppliers, line, order@, nation, sub, day, factor as int) * amount@) by {
                        lemma_customer_weight(*customers, *suppliers, line, order@, customer_group, nation, sub, day, factor as int);
                        lemma_revenue_sum(customer_group.rows@, |row: Cell| customer_weight(*suppliers, line, order@, row, nation, sub, day, factor as int), amount@);
                        let weight = |row: Cell| customer_weight(*suppliers, line, order@, row, nation, sub, day, factor as int);
                        lemma_sum_ext_all(customer_group.rows@,
                            |row: Cell| customer_weight(*suppliers, line, order@, row, nation, sub, day, factor as int) * amount@,
                            |row: Cell| weight(row) * amount@);
                        assert(sum(customer_group.rows@, |row: Cell| customer_weight(*suppliers, line, order@, row, nation, sub, day, factor as int) * amount@) == order_weight(*customers, *suppliers, line, order@, nation, sub, day, factor as int) * amount@);
                        assert(changes.at(nation, sub, day).0 == before_order.at(nation, sub, day).0 + sum(customer_group.rows@, |row: Cell| customer_weight(*suppliers, line, order@, row, nation, sub, day, factor as int)));
                        assert(changes.at(nation, sub, day).1 == before_order.at(nation, sub, day).1 + sum(customer_group.rows@, |row: Cell| customer_weight(*suppliers, line, order@, row, nation, sub, day, factor as int) * amount@));
                    }
                }
            },
            None => {
                proof {
                    assert forall|nation: i64, sub: i64, day: i32| #[trigger] order_weight(*customers, *suppliers, line, order@, nation, sub, day, factor as int) == 0 by {
                        assert(customers.at(order.sub, nation, 0) == (0, 0));
                    }
                }
            },
        }
        proof {
            assert forall|nation: i64, sub: i64, day: i32| #[trigger] changes.at(nation, sub, day) ==
                (entry.at(nation, sub, day).0 + sum(order_group.rows@.take(order_index as int + 1), |row: Cell| order_weight(*customers, *suppliers, line, row, nation, sub, day, factor as int)),
                 entry.at(nation, sub, day).1 + sum(order_group.rows@.take(order_index as int + 1), |row: Cell| order_weight(*customers, *suppliers, line, row, nation, sub, day, factor as int) * amount@)) by {
                lemma_sum_take(order_group.rows@, order_index as int, |row: Cell| order_weight(*customers, *suppliers, line, row, nation, sub, day, factor as int));
                lemma_sum_take(order_group.rows@, order_index as int, |row: Cell| order_weight(*customers, *suppliers, line, row, nation, sub, day, factor as int) * amount@);
                assert(changes.at(nation, sub, day).0 == before_order.at(nation, sub, day).0 + order_weight(*customers, *suppliers, line, order@, nation, sub, day, factor as int));
                assert(before_order.at(nation, sub, day).0 == entry.at(nation, sub, day).0 + sum(order_group.rows@.take(order_index as int), |row: Cell| order_weight(*customers, *suppliers, line, row, nation, sub, day, factor as int)));
            }
        }
        order_index += 1;
    }
    proof {
        assert(order_group.rows@.take(order_group.rows@.len() as int) =~= order_group.rows@);
        assert forall|nation: i64, sub: i64, day: i32| #[trigger] changes.at(nation, sub, day) ==
            (entry.at(nation, sub, day).0 + if sub == 0 { factor as int * contribution(*customers, *orders, *suppliers, line, nation, day).0 } else { 0 },
             entry.at(nation, sub, day).1 + if sub == 0 { factor as int * contribution(*customers, *orders, *suppliers, line, nation, day).1 } else { 0 }) by {
            let base = |order: Cell| if order.day == day { order.count as int * customers.at(order.sub, nation, 0).0 * suppliers.at(line.l_suppkey, nation, 0).0 } else { 0 };
            let weight = |order: Cell| order_weight(*customers, *suppliers, line, order, nation, sub, day, factor as int);
            let scalar = if sub == 0 { factor as int } else { 0 };
            assert forall|position: int| 0 <= position < order_group.rows@.len() implies weight(#[trigger] order_group.rows@[position]) == scalar * base(order_group.rows@[position]) by {
                let order = order_group.rows@[position];
                assert(factor as int * order.count * customers.at(order.sub, nation, 0).0 * suppliers.at(line.l_suppkey, nation, 0).0 ==
                    factor as int * (order.count as int * customers.at(order.sub, nation, 0).0 * suppliers.at(line.l_suppkey, nation, 0).0)) by (nonlinear_arith);
            }
            lemma_sum_ext(order_group.rows@, weight, |order: Cell| scalar * base(order));
            lemma_sum_scale(order_group.rows@, base, |order: Cell| scalar * base(order), scalar);
            lemma_revenue_sum(order_group.rows@, weight, amount@);
            lemma_sum_ext_all(order_group.rows@,
                |row: Cell| order_weight(*customers, *suppliers, line, row, nation, sub, day, factor as int) * amount@,
                |row: Cell| weight(row) * amount@);
            assert(changes.at(nation, sub, day).0 == entry.at(nation, sub, day).0 + sum(order_group.rows@, weight));
            assert(changes.at(nation, sub, day).1 == entry.at(nation, sub, day).1 + sum(order_group.rows@, |row: Cell| weight(row) * amount@));
            assert((factor * sum(order_group.rows@, base)) * (line.l_extendedprice * (100 - line.l_discount as int)) ==
                factor * (sum(order_group.rows@, base) * line.l_extendedprice * (100 - line.l_discount as int))) by (nonlinear_arith);
        }
    }
}

}

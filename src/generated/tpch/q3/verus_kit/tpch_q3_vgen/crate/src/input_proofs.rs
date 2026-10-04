use vstd::prelude::*;
use verus_common::sums::*;
use crate::gen_spec::*;
use crate::gen_obligations::*;
use crate::model::*;

verus! {

pub proof fn lemma_scan_domains(db: Db, customers: Seq<CustomerInput>, orders: Seq<OrderInput>, shipments: Seq<ShipmentInput>)
    requires scan_customers_ok(db, customers), scan_orders_ok(db, orders), scan_shipments_ok(db, shipments),
    ensures
        forall|position: int| #![trigger db.customer[position]] 0 <= position < db.customer.len() ==>
            i64::MIN <= db.customer[position].c_custkey <= i64::MAX,
        forall|position: int| #![trigger db.orders[position]] 0 <= position < db.orders.len() ==> {
            let row = db.orders[position];
            i64::MIN <= row.o_custkey <= i64::MAX && i64::MIN <= row.o_orderkey <= i64::MAX
                && i32::MIN <= row.o_orderdate <= i32::MAX && i64::MIN <= row.o_shippriority <= i64::MAX
        },
        forall|position: int| #![trigger db.lineitem[position]] 0 <= position < db.lineitem.len() ==> {
            let row = db.lineitem[position];
            i64::MIN <= row.l_orderkey <= i64::MAX && i32::MIN <= row.l_shipdate <= i32::MAX
        },
{
    verus_prooflib::lemma_scan_property(customer_input_seq_view(customers), db.customer,
        |row: CustomerRow| i64::MIN <= row.c_custkey <= i64::MAX);
    verus_prooflib::lemma_scan_property(order_input_seq_view(orders), db.orders,
        |row: OrdersRow| i64::MIN <= row.o_custkey <= i64::MAX && i64::MIN <= row.o_orderkey <= i64::MAX
            && i32::MIN <= row.o_orderdate <= i32::MAX && i64::MIN <= row.o_shippriority <= i64::MAX);
    verus_prooflib::lemma_scan_property(shipment_input_seq_view(shipments), db.lineitem,
        |row: LineitemRow| i64::MIN <= row.l_orderkey <= i64::MAX && i32::MIN <= row.l_shipdate <= i32::MAX);
}

pub proof fn lemma_customer_scan(db: Db, rows: Seq<CustomerInput>, customer: i64, segment: Seq<u8>)
    requires scan_customers_ok(db, rows),
    ensures customer_prefix(rows, customer, segment) == customer_sum(db, customer as int, segment),
{
    let weight = |row: CustomerRow| if row.c_custkey == customer as int && row.c_mktsegment == segment { 1int } else { 0int };
    let projected = |row: CustomerInput| customer_input_view(row);
    let input_weight = customer_input_weight(customer, segment);
    lemma_sum_map(rows, projected, weight, input_weight);
    assert(sum(customer_input_seq_view(rows), weight) == sum(db.customer, weight));
}

pub proof fn lemma_order_scan(db: Db, rows: Seq<OrderInput>, customer: i64, key: i64, date: i32, priority: i64)
    requires scan_orders_ok(db, rows),
    ensures reverse_prefix(rows, customer, key) == reverse_sum(db, customer as int, key as int),
        order_prefix(rows, key, customer, date, priority) == order_sum(db, key as int, customer as int, date as int, priority as int),
{
    let reverse = |row: OrdersRow| if row.o_custkey == customer as int && row.o_orderkey == key as int { 1int } else { 0int };
    let selected = |row: OrdersRow| if row.o_orderkey == key as int && row.o_custkey == customer as int
        && row.o_orderdate == date as int && row.o_shippriority == priority as int { 1int } else { 0int };
    lemma_sum_map(rows, |row: OrderInput| order_input_view(row), reverse,
        |row: OrderInput| if row.o_custkey == customer && row.o_orderkey == key { 1int } else { 0int });
    lemma_sum_map(rows, |row: OrderInput| order_input_view(row), selected,
        |row: OrderInput| if row.o_orderkey == key && row.o_custkey == customer && row.o_orderdate == date && row.o_shippriority == priority { 1int } else { 0int });
    assert(sum(order_input_seq_view(rows), reverse) == sum(db.orders, reverse));
    assert(sum(order_input_seq_view(rows), selected) == sum(db.orders, selected));
}

pub proof fn lemma_ship_scan(db: Db, rows: Seq<ShipmentInput>, key: i64, date: i32, kind: int)
    requires scan_shipments_ok(db, rows),
    ensures ship_prefix(rows, key, date, kind) == ship_sum(db, key as int, date as int, kind),
{
    let weight = |row: LineitemRow| if row.l_orderkey == key as int && row.l_shipdate == date as int {
        let value = row.l_extendedprice * (100 - row.l_discount);
        if kind == 0 { 1int } else if kind == 1 { value } else { if value > 0 { value } else { 0int } }
    } else { 0int };
    lemma_sum_map(rows, |row: ShipmentInput| shipment_input_view(row), weight,
        |row: ShipmentInput| if row.l_orderkey == key && row.l_shipdate == date {
            let value = row.l_extendedprice as int * (100 - row.l_discount as int);
            if kind == 0 { 1int } else if kind == 1 { value } else { if value > 0 { value } else { 0int } }
        } else { 0int });
    assert(sum(shipment_input_seq_view(rows), weight) == sum(db.lineitem, weight));
}

pub proof fn lemma_order_rescan(db: Db, keys: Seq<i64>, rows: Seq<OrderInput>, customer: i64, key: i64, date: i32, priority: i64)
    requires rescan_orders_by_key_ok(db, keys, rows), keys.contains(key),
    ensures reverse_prefix(rows, customer, key) == reverse_sum(db, customer as int, key as int),
        order_prefix(rows, key, customer, date, priority) == order_sum(db, key as int, customer as int, date as int, priority as int),
{
    let reverse = |row: OrdersRow| if row.o_custkey == customer as int && row.o_orderkey == key as int { 1int } else { 0int };
    let selected = |row: OrdersRow| if row.o_orderkey == key as int && row.o_custkey == customer as int
        && row.o_orderdate == date as int && row.o_shippriority == priority as int { 1int } else { 0int };
    let position = choose|position: int| #![trigger keys[position]] 0 <= position < keys.len() && keys[position] == key;
    assert(seq_view_i64(keys)[position] == key as int);
    assert(seq_view_i64(keys).contains(key as int));
    lemma_sum_map(rows, |row: OrderInput| order_input_view(row), reverse,
        |row: OrderInput| if row.o_custkey == customer && row.o_orderkey == key { 1int } else { 0int });
    lemma_sum_map(rows, |row: OrderInput| order_input_view(row), selected,
        |row: OrderInput| if row.o_orderkey == key && row.o_custkey == customer && row.o_orderdate == date && row.o_shippriority == priority { 1int } else { 0int });
    assert(sum(order_input_seq_view(rows), reverse) == sum(db.orders, |row: OrdersRow| if seq_view_i64(keys).contains(row.o_orderkey) { reverse(row) } else { 0int }));
    assert(sum(order_input_seq_view(rows), selected) == sum(db.orders, |row: OrdersRow| if seq_view_i64(keys).contains(row.o_orderkey) { selected(row) } else { 0int }));
    lemma_sum_ext_all(db.orders, |row: OrdersRow| if seq_view_i64(keys).contains(row.o_orderkey) { reverse(row) } else { 0int }, reverse);
    lemma_sum_ext_all(db.orders, |row: OrdersRow| if seq_view_i64(keys).contains(row.o_orderkey) { selected(row) } else { 0int }, selected);
}

pub proof fn lemma_customer_capture(before: Db, after: Db, customers: Seq<CustomerDelta>, lines: Seq<(LineitemRow, int)>, orders: Seq<(OrdersRow, int)>, customer: i64, segment: Seq<u8>)
    requires capture_ok(before, after, customer_delta_seq_view(customers), lines, orders),
    ensures customer_sum(after, customer as int, segment) == customer_sum(before, customer as int, segment)
        + customer_delta_prefix(customers, customer, segment),
{
    let weight = |row: CustomerRow| if row.c_custkey == customer as int && row.c_mktsegment == segment { 1int } else { 0int };
    let signed = |delta: (CustomerRow, int)| delta.1 * weight(delta.0);
    assert(sum(after.customer, weight) == sum(before.customer, weight) + sum(customer_delta_seq_view(customers), signed));
    lemma_sum_map(customers, |delta: CustomerDelta| (customer_input_view(delta.row), delta.sign as int), signed, customer_delta_weight(customer, segment));
}

}

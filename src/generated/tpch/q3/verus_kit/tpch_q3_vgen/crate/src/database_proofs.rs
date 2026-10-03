use vstd::prelude::*;
use crate::gen_spec::*;
use crate::gen_obligations::*;
use crate::profiles::*;
use verus_common::sums::*;

verus! {

pub open spec fn customer_bounds(row: CustomerRow) -> bool {
    i64::MIN <= row.c_custkey <= i64::MAX
}

pub open spec fn order_bounds(row: OrdersRow) -> bool {
    i64::MIN <= row.o_orderkey <= i64::MAX && i64::MIN <= row.o_custkey <= i64::MAX
        && i64::MIN <= row.o_shippriority <= i64::MAX && i32::MIN <= row.o_orderdate <= i32::MAX
}

pub open spec fn shipment_bounds(row: LineitemRow) -> bool {
    i64::MIN <= row.l_orderkey <= i64::MAX && i32::MIN <= row.l_shipdate <= i32::MAX
}

pub open spec fn db_bounds(db: Db) -> bool {
    &&& forall|position: int| 0 <= position < db.customer.len() ==> customer_bounds(#[trigger] db.customer[position])
    &&& forall|position: int| 0 <= position < db.orders.len() ==> order_bounds(#[trigger] db.orders[position])
    &&& forall|position: int| 0 <= position < db.lineitem.len() ==> shipment_bounds(#[trigger] db.lineitem[position])
}

pub proof fn lemma_zero_bad<Element>(rows: Seq<Element>, good: spec_fn(Element) -> bool)
    requires sum(rows, |row: Element| if good(row) { 0int } else { 1 }) == 0,
    ensures forall|position: int| 0 <= position < rows.len() ==> good(#[trigger] rows[position]),
{
    assert forall|position: int| 0 <= position < rows.len() implies good(#[trigger] rows[position]) by {
        if !good(rows[position]) {
            lemma_sum_ge_term(rows, |row: Element| if good(row) { 0int } else { 1 }, position);
        }
    }
}

pub proof fn lemma_scan_predicate<Element>(before: Seq<Element>, after: Seq<Element>, good: spec_fn(Element) -> bool)
    requires forall|position: int| 0 <= position < before.len() ==> good(#[trigger] before[position]),
        sum(before, |row: Element| if good(row) { 0int } else { 1 }) == sum(after, |row: Element| if good(row) { 0int } else { 1 }),
    ensures forall|position: int| 0 <= position < after.len() ==> good(#[trigger] after[position]),
{
    lemma_sum_zero(before, |row: Element| if good(row) { 0int } else { 1 });
    lemma_zero_bad(after, good);
}

pub proof fn lemma_capture_predicate<Element>(before: Seq<Element>, after: Seq<Element>, changes: Seq<(Element, int)>, good: spec_fn(Element) -> bool)
    requires bag_step(before, after, changes),
        forall|position: int| 0 <= position < before.len() ==> good(#[trigger] before[position]),
        forall|position: int| 0 <= position < changes.len() ==> good((#[trigger] changes[position]).0),
    ensures forall|position: int| 0 <= position < after.len() ==> good(#[trigger] after[position]),
{
    let bad = |row: Element| if good(row) { 0int } else { 1 };
    lemma_sum_zero(before, bad);
    lemma_sum_zero(changes, |entry: (Element, int)| entry.1 * bad(entry.0));
    assert(sum(after, bad) == sum(before, bad) + sum(changes, |entry: (Element, int)| entry.1 * bad(entry.0)));
    lemma_zero_bad(after, good);
}

pub proof fn lemma_scans_bounds(db: Db, customers: Seq<CustomerScan>, orders: Seq<OrdersScan>, shipments: Seq<ShipmentScan>)
    requires scan_customers_ok(db, customers), scan_orders_ok(db, orders), scan_shipments_ok(db, shipments),
    ensures db_bounds(db),
{
    lemma_scan_predicate(customer_scan_seq_view(customers), db.customer, |row: CustomerRow| customer_bounds(row));
    lemma_scan_predicate(orders_scan_seq_view(orders), db.orders, |row: OrdersRow| order_bounds(row));
    lemma_scan_predicate(shipment_scan_seq_view(shipments), db.lineitem, |row: LineitemRow| shipment_bounds(row));
}

pub proof fn lemma_capture_bounds(db: Db, db2: Db, customers: Seq<CustomerDelta>, orders: Seq<OrdersDelta>, shipments: Seq<ShipmentDelta>)
    requires db_bounds(db), capture_ok(db, db2, customer_delta_seq_view(customers), shipment_delta_seq_view(shipments), orders_delta_seq_view(orders)),
    ensures db_bounds(db2),
{
    lemma_capture_predicate(db.customer, db2.customer, customer_delta_seq_view(customers), |row: CustomerRow| customer_bounds(row));
    lemma_capture_predicate(db.orders, db2.orders, orders_delta_seq_view(orders), |row: OrdersRow| order_bounds(row));
    lemma_capture_predicate(db.lineitem, db2.lineitem, shipment_delta_seq_view(shipments), |row: LineitemRow| shipment_bounds(row));
}

pub proof fn lemma_no_line_points(db: Db, key: i64)
    requires db_bounds(db), forall|date: i32| #[trigger] line_count(db, key as int, date as int) - line_count(db, key as int, date as int + 1) == 0,
    ensures line_count(db, key as int, i32::MIN as int) == 0,
{
    hide(sum);
    assert forall|position: int| 0 <= position < db.lineitem.len() implies (#[trigger] db.lineitem[position]).l_orderkey != key by {
        if db.lineitem[position].l_orderkey == key {
            let date = db.lineitem[position].l_shipdate as i32;
            lemma_line_point_count(db, key as int, date as int);
            lemma_sum_ge_term(db.lineitem, |line: LineitemRow| if line.l_orderkey == key && line.l_shipdate == date { 1int } else { 0 }, position);
        }
    }
    lemma_sum_zero(db.lineitem, |line: LineitemRow| if line.l_orderkey == key && line.l_shipdate >= i32::MIN { 1int } else { 0 });
}

pub proof fn lemma_capture_line_point(db: Db, db2: Db, changes: Seq<(LineitemRow, int)>, key: i64, date: i32)
    requires bag_step(db.lineitem, db2.lineitem, changes),
    ensures line_count(db2, key as int, date as int) - line_count(db2, key as int, date as int + 1) ==
        line_count(db, key as int, date as int) - line_count(db, key as int, date as int + 1)
        + sum(changes, |entry: (LineitemRow, int)| if entry.0.l_orderkey == key && entry.0.l_shipdate == date { entry.1 } else { 0 }),
        line_revenue(db2, key as int, date as int) - line_revenue(db2, key as int, date as int + 1) ==
        line_revenue(db, key as int, date as int) - line_revenue(db, key as int, date as int + 1)
        + sum(changes, |entry: (LineitemRow, int)| if entry.0.l_orderkey == key && entry.0.l_shipdate == date {
            entry.1 * entry.0.l_extendedprice * (100 - entry.0.l_discount)
        } else { 0 }),
{
    hide(sum);
    let count_weight = |line: LineitemRow| if line.l_orderkey == key && line.l_shipdate == date { 1int } else { 0 };
    let revenue_weight = |line: LineitemRow| if line.l_orderkey == key && line.l_shipdate == date { line.l_extendedprice * (100 - line.l_discount) } else { 0 };
    assert(sum(db2.lineitem, count_weight) == sum(db.lineitem, count_weight) + sum(changes, |entry: (LineitemRow, int)| entry.1 * count_weight(entry.0)));
    assert(sum(db2.lineitem, revenue_weight) == sum(db.lineitem, revenue_weight) + sum(changes, |entry: (LineitemRow, int)| entry.1 * revenue_weight(entry.0)));
    lemma_sum_ext_all(changes, |entry: (LineitemRow, int)| entry.1 * count_weight(entry.0),
        |entry: (LineitemRow, int)| if entry.0.l_orderkey == key && entry.0.l_shipdate == date { entry.1 } else { 0 });
    assert forall|entry: (LineitemRow, int)| #[trigger] revenue_weight(entry.0) * entry.1 ==
        if entry.0.l_orderkey == key && entry.0.l_shipdate == date {
            entry.1 * entry.0.l_extendedprice * (100 - entry.0.l_discount)
        } else { 0 } by {
        if entry.0.l_orderkey == key && entry.0.l_shipdate == date {
            assert((entry.0.l_extendedprice * (100 - entry.0.l_discount)) * entry.1 ==
                entry.1 * entry.0.l_extendedprice * (100 - entry.0.l_discount)) by (nonlinear_arith);
        }
    }
    lemma_sum_ext_all(changes, |entry: (LineitemRow, int)| entry.1 * revenue_weight(entry.0),
        |entry: (LineitemRow, int)| if entry.0.l_orderkey == key && entry.0.l_shipdate == date {
            entry.1 * entry.0.l_extendedprice * (100 - entry.0.l_discount)
        } else { 0 });
    lemma_line_point_count(db, key as int, date as int);
    lemma_line_point_count(db2, key as int, date as int);
    lemma_line_point_revenue(db, key as int, date as int);
    lemma_line_point_revenue(db2, key as int, date as int);
}

pub proof fn lemma_capture_line_unchanged(db: Db, db2: Db, changes: Seq<(LineitemRow, int)>, key: i64)
    requires bag_step(db.lineitem, db2.lineitem, changes),
        forall|position: int| 0 <= position < changes.len() ==> (#[trigger] changes[position]).0.l_orderkey != key,
    ensures forall|date: int| #![trigger line_count(db2, key as int, date)] #![trigger line_revenue(db2, key as int, date)]
        line_count(db2, key as int, date) == line_count(db, key as int, date)
        && line_revenue(db2, key as int, date) == line_revenue(db, key as int, date),
{
    hide(sum);
    assert forall|date: int| #![trigger line_count(db2, key as int, date)] #![trigger line_revenue(db2, key as int, date)]
        line_count(db2, key as int, date) == line_count(db, key as int, date)
        && line_revenue(db2, key as int, date) == line_revenue(db, key as int, date) by {
        let count_weight = |line: LineitemRow| if line.l_orderkey == key && line.l_shipdate >= date { 1int } else { 0 };
        let revenue_weight = |line: LineitemRow| if line.l_orderkey == key && line.l_shipdate >= date { line.l_extendedprice * (100 - line.l_discount) } else { 0 };
        lemma_sum_zero(changes, |entry: (LineitemRow, int)| entry.1 * count_weight(entry.0));
        lemma_sum_zero(changes, |entry: (LineitemRow, int)| entry.1 * revenue_weight(entry.0));
        assert(sum(db2.lineitem, count_weight) == sum(db.lineitem, count_weight) + sum(changes, |entry: (LineitemRow, int)| entry.1 * count_weight(entry.0)));
        assert(sum(db2.lineitem, revenue_weight) == sum(db.lineitem, revenue_weight) + sum(changes, |entry: (LineitemRow, int)| entry.1 * revenue_weight(entry.0)));
    }
}

}

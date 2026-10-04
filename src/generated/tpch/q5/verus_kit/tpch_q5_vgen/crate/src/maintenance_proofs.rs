use vstd::prelude::*;
use verus_common::sums::*;
use verus_common::bag::*;
use crate::gen_spec::*;
use crate::gen_obligations::*;
use crate::meaning::*;
use crate::storage::*;
use crate::maintenance;
use crate::profile_changes;

verus! {

pub open spec fn selected_key(keys: Map<i64, bool>, key: int) -> bool {
    i64::MIN <= key <= i64::MAX && keys.contains_key(key as i64)
}

pub open spec fn customer_total(db: Db, order: OrdersRow, nation: i64) -> int {
    sum(db.customer, |customer: CustomerRow| ind(customer.c_custkey == order.o_custkey && customer.c_nationkey == nation))
}

pub open spec fn supplier_total(db: Db, line: LineitemRow, nation: i64) -> int {
    sum(db.supplier, |supplier: SupplierRow| ind(supplier.s_suppkey == line.l_suppkey && supplier.s_nationkey == nation))
}

pub open spec fn dimension_weight(db: Db, order: OrdersRow, line: LineitemRow, nation: i64, day: i32) -> int {
    if order.o_orderkey == line.l_orderkey && order.o_orderdate == day {
        customer_total(db, order, nation) * supplier_total(db, line, nation)
    } else { 0 }
}

pub proof fn lemma_factored(db: Db, line: LineitemRow, nation: i64, day: i32)
    ensures line_weight(db, line, nation, day) == sum(db.orders, |order: OrdersRow| dimension_weight(db, order, line, nation, day)),
{
    let original = |order: OrdersRow| if order.o_orderkey == line.l_orderkey && order.o_orderdate == day {
        sum(db.customer, |customer: CustomerRow| if customer.c_custkey == order.o_custkey && customer.c_nationkey == nation {
            sum(db.supplier, |supplier: SupplierRow| ind(supplier.s_suppkey == line.l_suppkey && supplier.s_nationkey == nation))
        } else { 0 })
    } else { 0 };
    let factored = |order: OrdersRow| dimension_weight(db, order, line, nation, day);
    assert forall|order: OrdersRow| #[trigger] original(order) == factored(order) by {
        if order.o_orderkey == line.l_orderkey && order.o_orderdate == day {
            let count = |customer: CustomerRow| ind(customer.c_custkey == order.o_custkey && customer.c_nationkey == nation);
            let scaled = |customer: CustomerRow| if customer.c_custkey == order.o_custkey && customer.c_nationkey == nation {
                sum(db.supplier, |supplier: SupplierRow| ind(supplier.s_suppkey == line.l_suppkey && supplier.s_nationkey == nation))
            } else { 0 };
            lemma_sum_scale(db.customer, count, scaled, supplier_total(db, line, nation));
            assert(supplier_total(db, line, nation) * customer_total(db, order, nation) == customer_total(db, order, nation) * supplier_total(db, line, nation)) by (nonlinear_arith);
        }
    }
    lemma_sum_ext_all(db.orders, original, factored);
}

pub proof fn lemma_unaffected_customer(db: Db, reverse: Table, customers: Seq<CustomerDelta>, orders: Seq<OrdersDelta>, order_keys: Map<i64, bool>, position: int)
    requires reverse_match(reverse, db), projections_representable(db), 0 <= position < db.orders.len(),
        !selected_key(order_keys, db.orders[position].o_orderkey),
        forall|key: i64| #[trigger] order_keys.contains_key(key) <==> maintenance::affected_order(reverse, customers, orders, key),
    ensures forall|index: int| 0 <= index < customers.len() ==> (#[trigger] customers[index]).row.c_custkey != db.orders[position].o_custkey,
{
    let order = db.orders[position];
    assert(order_representable(order));
    let references = |row: OrdersRow| ind(row.o_orderkey == order.o_orderkey && row.o_custkey == order.o_custkey);
    lemma_sum_pos(db.orders, references, position);
    assert(reverse.at(order.o_custkey as i64, order.o_orderkey as i64, 0).0 > 0);
    assert forall|index: int| 0 <= index < customers.len() implies (#[trigger] customers[index]).row.c_custkey != order.o_custkey by {
        if customers[index].row.c_custkey == order.o_custkey {
            let affected = |delta: CustomerDelta| ind(reverse.at(delta.row.c_custkey, order.o_orderkey as i64, 0).0 > 0);
            lemma_sum_pos(customers, affected, index);
            assert(maintenance::affected_order(reverse, customers, orders, order.o_orderkey as i64));
            assert(order_keys.contains_key(order.o_orderkey as i64));
        }
    }
}

pub proof fn lemma_line_stable(db: Db, db2: Db, reverse: Table, customers: Seq<CustomerDelta>, orders: Seq<OrdersDelta>, suppliers: Seq<SupplierDelta>,
    order_keys: Map<i64, bool>, supplier_keys: Map<i64, bool>, line: LineitemRow, nation: i64, day: i32)
    requires reverse_match(reverse, db), projections_representable(db),
        bag_step(db.customer, db2.customer, customer_delta_seq_view(customers)),
        bag_step(db.orders, db2.orders, orders_delta_seq_view(orders)),
        bag_step(db.supplier, db2.supplier, supplier_delta_seq_view(suppliers)),
        forall|key: i64| #[trigger] order_keys.contains_key(key) <==> maintenance::affected_order(reverse, customers, orders, key),
        forall|key: i64| #[trigger] supplier_keys.contains_key(key) <==> maintenance::affected_supplier(suppliers, key),
        !selected_key(order_keys, line.l_orderkey), !selected_key(supplier_keys, line.l_suppkey),
    ensures line_weight(db2, line, nation, day) == line_weight(db, line, nation, day),
{
    let supplier_weight = |supplier: SupplierRow| ind(supplier.s_suppkey == line.l_suppkey && supplier.s_nationkey == nation);
    let supplier_changes = supplier_delta_seq_view(suppliers);
    assert forall|index: int| 0 <= index < suppliers.len() implies (#[trigger] suppliers[index]).row.s_suppkey != line.l_suppkey by {
        if suppliers[index].row.s_suppkey == line.l_suppkey {
            let keys = suppliers.map_values(|delta: SupplierDelta| delta.row.s_suppkey);
            assert(keys[index] == suppliers[index].row.s_suppkey);
            assert(keys.contains(suppliers[index].row.s_suppkey));
            assert(maintenance::affected_supplier(suppliers, suppliers[index].row.s_suppkey));
        }
    }
    lemma_sum_zero(supplier_changes, |delta: (SupplierRow, int)| delta.1 * supplier_weight(delta.0));
    assert(supplier_total(db2, line, nation) == supplier_total(db, line, nation));
    let old_weight = |order: OrdersRow| dimension_weight(db, order, line, nation, day);
    let new_weight = |order: OrdersRow| dimension_weight(db2, order, line, nation, day);
    assert forall|position: int| 0 <= position < db.orders.len() implies old_weight(#[trigger] db.orders[position]) == new_weight(db.orders[position]) by {
        let order = db.orders[position];
        if order.o_orderkey == line.l_orderkey && order.o_orderdate == day {
            lemma_unaffected_customer(db, reverse, customers, orders, order_keys, position);
            let customer_weight = |customer: CustomerRow| ind(customer.c_custkey == order.o_custkey && customer.c_nationkey == nation);
            lemma_sum_zero(customer_delta_seq_view(customers), |delta: (CustomerRow, int)| delta.1 * customer_weight(delta.0));
            assert(customer_total(db2, order, nation) == customer_total(db, order, nation));
        }
    }
    lemma_sum_ext(db.orders, old_weight, new_weight);
    assert forall|index: int| 0 <= index < orders.len() implies (#[trigger] orders[index]).row.o_orderkey != line.l_orderkey by {
        if orders[index].row.o_orderkey == line.l_orderkey {
            let keys = orders.map_values(|delta: OrdersDelta| delta.row.o_orderkey);
            assert(keys[index] == orders[index].row.o_orderkey);
            assert(keys.contains(orders[index].row.o_orderkey));
            assert(maintenance::affected_order(reverse, customers, orders, orders[index].row.o_orderkey));
        }
    }
    lemma_sum_zero(orders_delta_seq_view(orders), |delta: (OrdersRow, int)| delta.1 * new_weight(delta.0));
    assert(sum(db2.orders, new_weight) == sum(db.orders, new_weight));
    lemma_factored(db, line, nation, day);
    lemma_factored(db2, line, nation, day);
}

pub open spec fn line_channel(db: Db, line: LineitemRow, nation: i64, day: i32, revenue: bool) -> int {
    if revenue { line_weight(db, line, nation, day) * line.l_extendedprice * (100 - line.l_discount) }
    else { line_weight(db, line, nation, day) }
}

pub proof fn lemma_runtime_channel(tables: (Table, Table, Table), db: Db, line: LineitemInput, nation: i64, day: i32, revenue: bool)
    requires customers_match(tables.0, db), orders_match(tables.1, db), suppliers_match(tables.2, db), projections_representable(db),
    ensures (if revenue { maintenance::contribution(tables.0, tables.1, tables.2, line, nation, day).1 }
        else { maintenance::contribution(tables.0, tables.1, tables.2, line, nation, day).0 }) ==
        line_channel(db, lineitem_input_view(line), nation, day, revenue),
{
    lemma_line_weight(tables.0, tables.1, tables.2, db, line, nation, day);
}

pub proof fn lemma_selected_covered(keys: Map<i64, bool>, required: Seq<i64>, supplied: Seq<i64>, key: int)
    requires forall|value: i64| #[trigger] keys.contains_key(value) <==> required.contains(value),
        forall|position: int| 0 <= position < required.len() ==> supplied.contains(#[trigger] required[position]),
        selected_key(keys, key),
    ensures seq_view_i64(supplied).contains(key),
{
    let position = choose|position: int| 0 <= position < required.len() && (#[trigger] required[position]) == key as i64;
    assert(supplied.contains(required[position]));
    let supplied_position = choose|supplied_position: int| 0 <= supplied_position < supplied.len() && (#[trigger] supplied[supplied_position]) == key as i64;
    assert(seq_view_i64(supplied)[supplied_position] == key);
}

pub open spec fn selected_line(orders: Map<i64, bool>, suppliers: Map<i64, bool>, line: LineitemRow, supplier_mode: bool) -> bool {
    if supplier_mode { selected_key(suppliers, line.l_suppkey) && !selected_key(orders, line.l_orderkey) }
    else { selected_key(orders, line.l_orderkey) }
}

pub open spec fn database_change(db: Db, db2: Db, orders: Map<i64, bool>, suppliers: Map<i64, bool>, line: LineitemRow,
    supplier_mode: bool, nation: i64, day: i32, revenue: bool) -> int {
    if selected_line(orders, suppliers, line, supplier_mode) {
        line_channel(db2, line, nation, day, revenue) - line_channel(db, line, nation, day, revenue)
    } else { 0 }
}

pub proof fn lemma_rescan_transfer(db: Db, db2: Db, before: (Table, Table, Table), after: (Table, Table, Table),
    orders: Map<i64, bool>, suppliers: Map<i64, bool>, required: Seq<i64>, supplied: Seq<i64>, rows: Seq<LineitemInput>,
    supplier_mode: bool, nation: i64, day: i32, revenue: bool)
    requires projections_representable(db), projections_representable(db2),
        customers_match(before.0, db), orders_match(before.1, db), suppliers_match(before.2, db),
        customers_match(after.0, db2), orders_match(after.1, db2), suppliers_match(after.2, db2),
        forall|key: i64| (if supplier_mode { suppliers.contains_key(key) } else { orders.contains_key(key) }) <==> #[trigger] required.contains(key),
        forall|position: int| 0 <= position < required.len() ==> supplied.contains(#[trigger] required[position]),
        if supplier_mode { rescan_lines_for_suppliers_ok(db2, supplied, rows) } else { rescan_lines_for_orders_ok(db2, supplied, rows) },
    ensures sum(rows, |line: LineitemInput| profile_changes::rescan_weight(before, after, orders, suppliers, line, supplier_mode, nation, day, revenue)) ==
        sum(lineitem_input_seq_view(rows), |line: LineitemRow| database_change(db, db2, orders, suppliers, line, supplier_mode, nation, day, revenue)),
        sum(lineitem_input_seq_view(rows), |line: LineitemRow| database_change(db, db2, orders, suppliers, line, supplier_mode, nation, day, revenue)) ==
        sum(db2.lineitem, |line: LineitemRow| database_change(db, db2, orders, suppliers, line, supplier_mode, nation, day, revenue)),
{
    let runtime = |line: LineitemInput| profile_changes::rescan_weight(before, after, orders, suppliers, line, supplier_mode, nation, day, revenue);
    let database = |line: LineitemRow| database_change(db, db2, orders, suppliers, line, supplier_mode, nation, day, revenue);
    assert forall|line: LineitemInput| #[trigger] runtime(line) == database(lineitem_input_view(line)) by {
        lemma_runtime_channel(before, db, line, nation, day, revenue);
        lemma_runtime_channel(after, db2, line, nation, day, revenue);
    }
    lemma_sum_map(rows, |line: LineitemInput| lineitem_input_view(line), database, runtime);
    if supplier_mode {
        let restricted = |line: LineitemRow| if seq_view_i64(supplied).contains(line.l_suppkey) { database(line) } else { 0 };
        assert(sum(lineitem_input_seq_view(rows), database) == sum(db2.lineitem, restricted));
        assert forall|line: LineitemRow| #[trigger] restricted(line) == database(line) by {
            if selected_line(orders, suppliers, line, supplier_mode) { lemma_selected_covered(suppliers, required, supplied, line.l_suppkey); }
        }
        lemma_sum_ext_all(db2.lineitem, restricted, database);
    } else {
        let restricted = |line: LineitemRow| if seq_view_i64(supplied).contains(line.l_orderkey) { database(line) } else { 0 };
        assert(sum(lineitem_input_seq_view(rows), database) == sum(db2.lineitem, restricted));
        assert forall|line: LineitemRow| #[trigger] restricted(line) == database(line) by {
            if selected_line(orders, suppliers, line, supplier_mode) { lemma_selected_covered(orders, required, supplied, line.l_orderkey); }
        }
        lemma_sum_ext_all(db2.lineitem, restricted, database);
    }
}

pub proof fn lemma_profile_delta(db: Db, db2: Db, reverse: Table, before: (Table, Table, Table), after: (Table, Table, Table),
    customers: Seq<CustomerDelta>, orders: Seq<OrdersDelta>, suppliers: Seq<SupplierDelta>, lines: Seq<LineitemDelta>,
    order_keys: Map<i64, bool>, supplier_keys: Map<i64, bool>,
    required_orders: Seq<i64>, supplied_orders: Seq<i64>, order_rows: Seq<LineitemInput>,
    required_suppliers: Seq<i64>, supplied_suppliers: Seq<i64>, supplier_rows: Seq<LineitemInput>, nation: i64, day: i32, revenue: bool)
    requires projections_representable(db), projections_representable(db2), reverse_match(reverse, db),
        customers_match(before.0, db), orders_match(before.1, db), suppliers_match(before.2, db),
        customers_match(after.0, db2), orders_match(after.1, db2), suppliers_match(after.2, db2),
        bag_step(db.customer, db2.customer, customer_delta_seq_view(customers)),
        bag_step(db.orders, db2.orders, orders_delta_seq_view(orders)),
        bag_step(db.supplier, db2.supplier, supplier_delta_seq_view(suppliers)),
        bag_step(db.lineitem, db2.lineitem, lineitem_delta_seq_view(lines)),
        forall|key: i64| #[trigger] order_keys.contains_key(key) <==> maintenance::affected_order(reverse, customers, orders, key),
        forall|key: i64| #[trigger] supplier_keys.contains_key(key) <==> maintenance::affected_supplier(suppliers, key),
        forall|key: i64| #[trigger] order_keys.contains_key(key) <==> required_orders.contains(key),
        forall|key: i64| #[trigger] supplier_keys.contains_key(key) <==> required_suppliers.contains(key),
        forall|position: int| 0 <= position < required_orders.len() ==> supplied_orders.contains(#[trigger] required_orders[position]),
        forall|position: int| 0 <= position < required_suppliers.len() ==> supplied_suppliers.contains(#[trigger] required_suppliers[position]),
        rescan_lines_for_orders_ok(db2, supplied_orders, order_rows), rescan_lines_for_suppliers_ok(db2, supplied_suppliers, supplier_rows),
    ensures (if revenue { profile(db2, nation, day).1 } else { profile(db2, nation, day).0 }) ==
        (if revenue { profile(db, nation, day).1 } else { profile(db, nation, day).0 }) +
        sum(order_rows, |line: LineitemInput| profile_changes::rescan_weight(before, after, order_keys, supplier_keys, line, false, nation, day, revenue)) +
        sum(supplier_rows, |line: LineitemInput| profile_changes::rescan_weight(before, after, order_keys, supplier_keys, line, true, nation, day, revenue)) +
        sum(lines, |delta: LineitemDelta| profile_changes::captured_weight(before, delta, nation, day, revenue)),
{
    let old_weight = |line: LineitemRow| line_channel(db, line, nation, day, revenue);
    let new_weight = |line: LineitemRow| line_channel(db2, line, nation, day, revenue);
    let in_first = |line: LineitemRow| selected_key(order_keys, line.l_orderkey);
    let in_second = |line: LineitemRow| selected_key(supplier_keys, line.l_suppkey);
    assert forall|line: LineitemRow| !#[trigger] in_first(line) && !#[trigger] in_second(line) implies new_weight(line) == old_weight(line) by {
        lemma_line_stable(db, db2, reverse, customers, orders, suppliers, order_keys, supplier_keys, line, nation, day);
    }
    lemma_rescan_transfer(db, db2, before, after, order_keys, supplier_keys, required_orders, supplied_orders, order_rows, false, nation, day, revenue);
    lemma_rescan_transfer(db, db2, before, after, order_keys, supplier_keys, required_suppliers, supplied_suppliers, supplier_rows, true, nation, day, revenue);
    let first_weight = |line: LineitemRow| if in_first(line) { new_weight(line) - old_weight(line) } else { 0 };
    let second_weight = |line: LineitemRow| if !in_first(line) && in_second(line) { new_weight(line) - old_weight(line) } else { 0 };
    lemma_sum_ext_all(lineitem_input_seq_view(order_rows), first_weight, |line: LineitemRow| database_change(db, db2, order_keys, supplier_keys, line, false, nation, day, revenue));
    lemma_sum_ext_all(db2.lineitem, first_weight, |line: LineitemRow| database_change(db, db2, order_keys, supplier_keys, line, false, nation, day, revenue));
    lemma_sum_ext_all(lineitem_input_seq_view(supplier_rows), second_weight, |line: LineitemRow| database_change(db, db2, order_keys, supplier_keys, line, true, nation, day, revenue));
    lemma_sum_ext_all(db2.lineitem, second_weight, |line: LineitemRow| database_change(db, db2, order_keys, supplier_keys, line, true, nation, day, revenue));
    verus_prooflib::lemma_partitioned_bag_delta(db.lineitem, db2.lineitem, lineitem_delta_seq_view(lines),
        lineitem_input_seq_view(order_rows), lineitem_input_seq_view(supplier_rows), old_weight, new_weight, in_first, in_second);
    let captured = |delta: (LineitemRow, int)| delta.1 * old_weight(delta.0);
    let runtime_capture = |delta: LineitemDelta| profile_changes::captured_weight(before, delta, nation, day, revenue);
    assert forall|delta: LineitemDelta| #[trigger] runtime_capture(delta) == captured((lineitem_input_view(delta.row), delta.sign as int)) by {
        lemma_runtime_channel(before, db, delta.row, nation, day, revenue);
    }
    lemma_sum_map(lines, |delta: LineitemDelta| (lineitem_input_view(delta.row), delta.sign as int), captured, runtime_capture);
    if revenue {
        lemma_sum_ext_all(db.lineitem, old_weight, |line: LineitemRow| line_weight(db, line, nation, day) * line.l_extendedprice * (100 - line.l_discount));
        lemma_sum_ext_all(db2.lineitem, new_weight, |line: LineitemRow| line_weight(db2, line, nation, day) * line.l_extendedprice * (100 - line.l_discount));
    } else {
        lemma_sum_ext_all(db.lineitem, old_weight, |line: LineitemRow| line_weight(db, line, nation, day));
        lemma_sum_ext_all(db2.lineitem, new_weight, |line: LineitemRow| line_weight(db2, line, nation, day));
    }
}

}

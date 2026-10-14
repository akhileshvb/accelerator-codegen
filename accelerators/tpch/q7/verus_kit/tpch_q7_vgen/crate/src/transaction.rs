use vstd::prelude::*;
use verus_common::sums::*;
use verus_common::bag::*;
use verus_prooflib::*;
use crate::gen_spec::*;
use crate::gen_obligations::*;
use crate::storage::*;
use crate::values::*;
use crate::model::*;
use crate::domains::*;
use crate::dimensions::*;
use crate::join_semantics::*;

verus! {
broadcast use vstd::seq_lib::group_seq_properties;

pub proof fn lemma_customer_outside(db: Db, customer: int, second: i64)
    requires database_domain(db), !key_range(customer),
    ensures customer_weight(db, customer, second) == 0int,
{
    let weight = |row: CustomerRow| if row.c_custkey == customer && row.c_nationkey == second { 1int } else { 0int };
    assert forall|position: int| 0 <= position < db.customer.len() implies weight(#[trigger] db.customer[position]) == 0int by {
        assert(key_range(db.customer[position].c_custkey));
    }
    lemma_sum_zero(db.customer, weight);
}

#[verifier::auto_ext_equal(assert, assert_by, ensures)]
pub proof fn lemma_snapshot_order(db: Db, orders: Layers<Counts>, customers: Layers<Counts>, key: i64, second: i64, witness: int)
    requires database_domain(db), 0 <= witness < db.lineitem.len(),
        db.lineitem[witness].l_orderkey == key, 9131 <= db.lineitem[witness].l_shipdate <= 9861,
        forall|customer: i64| #![trigger dimension_value(orders, key, customer)]
            dimension_value(orders, key, customer) == order_count(db, key, customer),
        forall|customer: i64| #![trigger dimension_value(customers, customer, second)]
            dimension_value(customers, customer, second) == customer_count(db, customer, second),
    ensures (match orders.at(key) { Some(bucket) => order_weight(bucket.rows@, customers, second), None => 0int })
        == order_join_weight(db, key as int, second),
{
    reveal(order_count); reveal(customer_count);
    let collapsed = match orders.at(key) { Some(bucket) => bucket.rows@, None => Seq::empty() };
    let left_project = |row: (i64, i128)| row.0;
    let right_project = |row: OrdersRow| if key_range(row.o_custkey) { row.o_custkey as i64 } else { 0i64 };
    let left_multiplicity = |row: (i64, i128)| row.1 as int;
    let right_multiplicity = |row: OrdersRow| if row.o_orderkey == key && key_range(row.o_custkey) { 1int } else { 0int };
    let weight = |customer: i64| dimension_value(customers, customer, second);
    assert forall|customer: i64| #![trigger class_total(collapsed, left_project, left_multiplicity, customer)]
        class_total(collapsed, left_project, left_multiplicity, customer)
            == class_total(db.orders, right_project, right_multiplicity, customer) by {
        let left = |row: (i64, i128)| if left_project(row) == customer { left_multiplicity(row) } else { 0int };
        let right = |row: OrdersRow| if right_project(row) == customer { right_multiplicity(row) } else { 0int };
            lemma_sum_ext_all(collapsed, left, |row: (i64, i128)| if row.0 == customer { row.1 as int } else { 0int });
            if orders.at(key).is_none() { lemma_sum_empty(collapsed, left); }
            assert(class_total(collapsed, left_project, left_multiplicity, customer) == dimension_value(orders, key, customer));
            let expected = |row: OrdersRow| if row.o_orderkey == key && row.o_custkey == customer && scan_relevant_orders_exists_1(db, row) { 1int } else { 0int };
            assert forall|row: OrdersRow| #[trigger] right(row) == expected(row) by {
                if row.o_orderkey == key { assert(scan_relevant_orders_exists_1(db, row)) by {
                    assert(scan_relevant_orders_exists_1_where(row, db.lineitem[witness]));
                } }
            }
            lemma_sum_ext_all(db.orders, right, expected);
    }
    lemma_equal_classes(collapsed, db.orders, left_project, right_project, left_multiplicity, right_multiplicity, weight);
    let left = |row: (i64, i128)| left_multiplicity(row) * weight(left_project(row));
    let expected_left = |row: (i64, i128)| row.1 as int * dimension_value(customers, row.0, second);
    assert forall|row: (i64, i128)| #[trigger] left(row) == expected_left(row) by {}
    lemma_sum_ext_all(collapsed, left, expected_left);
    if orders.at(key).is_none() { lemma_sum_empty(collapsed, expected_left); }
    let right = |row: OrdersRow| right_multiplicity(row) * weight(right_project(row));
    let expected_right = |row: OrdersRow| if row.o_orderkey == key { customer_weight(db, row.o_custkey, second) } else { 0int };
    assert forall|row: OrdersRow| #[trigger] right(row) == expected_right(row) by {
        if key_range(row.o_custkey) {
            let native = row.o_custkey as i64;
            assert(native as int == row.o_custkey);
            assert(customer_count(db, native, second) == customer_weight(db, row.o_custkey, second));
        } else { lemma_customer_outside(db, row.o_custkey, second); }
    }
    lemma_sum_ext_all(db.orders, right, expected_right);
}

pub open spec fn snapshot_weight(suppliers: Layers<Counts>, customers: Layers<Counts>, orders: Layers<Counts>,
    line: LineitemRow, first: i64, second: i64, year: i64, revenue: bool) -> int {
    if 9131 <= line.l_shipdate <= 9861 && key_range(line.l_orderkey) && key_range(line.l_suppkey)
        && key_range(line.l_extendedprice) && key_range(line.l_discount) {
        projected_contribution(suppliers, customers, orders, line.l_orderkey as i64, line.l_suppkey as i64,
            if line.l_shipdate <= 9495 { 1995i64 } else { 1996i64 },
            line.l_extendedprice * (100 - line.l_discount), first, second, year, revenue)
    } else { 0int }
}

pub open spec fn snapshot_sum(db: Db, suppliers: Layers<Counts>, customers: Layers<Counts>, orders: Layers<Counts>,
    first: i64, second: i64, year: i64, revenue: bool) -> int {
    sum(db.lineitem, snapshot_fn(suppliers, customers, orders, first, second, year, revenue))
}

pub open spec fn snapshot_fn(suppliers: Layers<Counts>, customers: Layers<Counts>, orders: Layers<Counts>,
    first: i64, second: i64, year: i64, revenue: bool) -> spec_fn(LineitemRow) -> int {
    |line: LineitemRow| snapshot_weight(suppliers, customers, orders, line, first, second, year, revenue)
}

pub proof fn lemma_snapshot_line(db: Db, suppliers: Layers<Counts>, customers: Layers<Counts>, orders: Layers<Counts>,
    position: int, first: i64, second: i64, year: i64, revenue: bool)
    requires database_domain(db), 0 <= position < db.lineitem.len(),
        forall|key: i64, other: i64| #![trigger dimension_value(suppliers, key, other)]
            dimension_value(suppliers, key, other) == supplier_count(db, key, other),
        forall|key: i64, other: i64| #![trigger dimension_value(customers, key, other)]
            dimension_value(customers, key, other) == customer_count(db, key, other),
        forall|key: i64, other: i64| #![trigger dimension_value(orders, key, other)]
            dimension_value(orders, key, other) == order_count(db, key, other),
    ensures snapshot_weight(suppliers, customers, orders, db.lineitem[position], first, second, year, revenue)
        == fact_line(db, db.lineitem[position], first, second, year, revenue),
{
    reveal(projected_contribution); reveal(fact_line);
    let line = db.lineitem[position];
    if 9131 <= line.l_shipdate <= 9861 {
        assert(line_domain(line));
        let key = line.l_orderkey as i64;
        let supplier = line.l_suppkey as i64;
        assert(key as int == line.l_orderkey && supplier as int == line.l_suppkey);
        lemma_snapshot_order(db, orders, customers, key, second, position);
        lemma_fact_factor(db, line, first, second, year, revenue);
        reveal(supplier_count);
        let order_total = match orders.at(key) { Some(bucket) => order_weight(bucket.rows@, customers, second), None => 0int };
        let supplier_total = dimension_value(suppliers, supplier, first);
        let measure = if revenue { line.l_extendedprice * (100 - line.l_discount) } else { 1int };
        assert(order_total == order_join_weight(db, line.l_orderkey, second));
        lemma_group_coefficient(supplier_total, order_total, 1int, measure);
        assert(supplier_total == sum(db.supplier, |row: SupplierRow| if row.s_suppkey == line.l_suppkey && row.s_nationkey == first { 1int } else { 0int }));
    }
}

pub proof fn lemma_snapshot_sum(db: Db, suppliers: Layers<Counts>, customers: Layers<Counts>, orders: Layers<Counts>,
    first: i64, second: i64, year: i64, revenue: bool)
    requires database_domain(db),
        forall|key: i64, other: i64| #![trigger dimension_value(suppliers, key, other)]
            dimension_value(suppliers, key, other) == supplier_count(db, key, other),
        forall|key: i64, other: i64| #![trigger dimension_value(customers, key, other)]
            dimension_value(customers, key, other) == customer_count(db, key, other),
        forall|key: i64, other: i64| #![trigger dimension_value(orders, key, other)]
            dimension_value(orders, key, other) == order_count(db, key, other),
    ensures snapshot_sum(db, suppliers, customers, orders, first, second, year, revenue) == numeric_group(db, first, second, year, revenue),
{
    reveal(numeric_group);
    let actual = |line: LineitemRow| snapshot_weight(suppliers, customers, orders, line, first, second, year, revenue);
    let expected = |line: LineitemRow| fact_line(db, line, first, second, year, revenue);
    assert forall|position: int| 0 <= position < db.lineitem.len() implies actual(#[trigger] db.lineitem[position]) == expected(db.lineitem[position]) by {
        lemma_snapshot_line(db, suppliers, customers, orders, position, first, second, year, revenue);
    }
    lemma_sum_ext(db.lineitem, actual, expected);
}

pub open spec fn dimension_facts(db: Db, suppliers: Layers<Counts>, customers: Layers<Counts>, orders: Layers<Counts>) -> bool {
    &&& forall|key: i64, other: i64| #![trigger dimension_value(suppliers, key, other)]
        dimension_value(suppliers, key, other) == supplier_count(db, key, other)
    &&& forall|key: i64, other: i64| #![trigger dimension_value(customers, key, other)]
        dimension_value(customers, key, other) == customer_count(db, key, other)
    &&& forall|key: i64, other: i64| #![trigger dimension_value(orders, key, other)]
        dimension_value(orders, key, other) == order_count(db, key, other)
}

pub proof fn lemma_native_keys(keys: Seq<i64>, key: i64)
    ensures seq_view_i64(keys).contains(key as int) == keys.contains(key),
{
    if keys.contains(key) {
        let position = choose|position: int| #![trigger keys[position]] 0 <= position < keys.len() && keys[position] == key;
        assert(seq_view_i64(keys)[position] == key as int);
        assert(seq_view_i64(keys).contains(key as int));
    }
    if seq_view_i64(keys).contains(key as int) {
        let position = choose|position: int| #![trigger seq_view_i64(keys)[position]]
            0 <= position < seq_view_i64(keys).len() && seq_view_i64(keys)[position] == key as int;
        assert(keys[position] == key);
        assert(keys.contains(key));
    }
}

#[verifier::auto_ext_equal(assert, assert_by, ensures)]
pub proof fn lemma_order_cache_equal(before_orders: Layers<Counts>, after_orders: Layers<Counts>,
    before_customers: Layers<Counts>, after_customers: Layers<Counts>, key: i64, second: i64)
    requires before_orders.at(key) matches Some(bucket) ==> counts_positive(bucket),
        forall|customer: i64| #![trigger dimension_value(after_orders, key, customer)]
            dimension_value(after_orders, key, customer) == dimension_value(before_orders, key, customer),
        forall|customer: i64| #![trigger dimension_value(before_orders, key, customer)]
            dimension_value(before_orders, key, customer) > 0 ==>
                dimension_value(after_customers, customer, second) == dimension_value(before_customers, customer, second),
    ensures (match after_orders.at(key) { Some(bucket) => order_weight(bucket.rows@, after_customers, second), None => 0int })
        == (match before_orders.at(key) { Some(bucket) => order_weight(bucket.rows@, before_customers, second), None => 0int }),
{
    reveal(count_at);
    let before = match before_orders.at(key) { Some(bucket) => bucket.rows@, None => Seq::empty() };
    let after = match after_orders.at(key) { Some(bucket) => bucket.rows@, None => Seq::empty() };
    let project = |row: (i64, i128)| row.0;
    let multiplicity = |row: (i64, i128)| row.1 as int;
    let weight = |customer: i64| dimension_value(after_customers, customer, second);
    assert forall|customer: i64| #![trigger class_total(before, project, multiplicity, customer)]
        class_total(before, project, multiplicity, customer) == class_total(after, project, multiplicity, customer) by {
        let part = |row: (i64, i128)| if project(row) == customer { multiplicity(row) } else { 0int };
        let count = count_weight(customer);
        lemma_sum_ext_all(before, part, count);
        lemma_sum_ext_all(after, part, count);
        if before_orders.at(key).is_none() { lemma_sum_empty(before, part); }
        if after_orders.at(key).is_none() { lemma_sum_empty(after, part); }
        assert(class_total(before, project, multiplicity, customer) == dimension_value(before_orders, key, customer));
        assert(class_total(after, project, multiplicity, customer) == dimension_value(after_orders, key, customer));
    }
    lemma_equal_classes(before, after, project, project, multiplicity, multiplicity, weight);
    let weighted = |row: (i64, i128)| multiplicity(row) * weight(project(row));
    let new_weight = |row: (i64, i128)| row.1 as int * dimension_value(after_customers, row.0, second);
    let old_weight = |row: (i64, i128)| row.1 as int * dimension_value(before_customers, row.0, second);
    lemma_sum_ext_all(before, weighted, new_weight);
    lemma_sum_ext_all(after, weighted, new_weight);
    assert forall|position: int| 0 <= position < before.len() implies new_weight(#[trigger] before[position]) == old_weight(before[position]) by {
        let customer = before[position].0;
        let count = count_weight(customer);
        assert forall|offset: int| 0 <= offset < before.len() implies count(#[trigger] before[offset]) >= 0 by {}
        lemma_sum_pos(before, count, position);
        assert(count =~= (|entry: (i64, i128)| if entry.0 == customer { entry.1 as int } else { 0 }));
        assert(sum(before, count) == count_at(before, customer));
        assert(dimension_value(before_orders, key, customer) == count_at(before, customer));
        assert(dimension_value(before_orders, key, customer) > 0);
    }
    lemma_sum_ext(before, new_weight, old_weight);
    if before_orders.at(key).is_none() { lemma_sum_empty(before, old_weight); }
    if after_orders.at(key).is_none() { lemma_sum_empty(after, new_weight); }
}

pub open spec fn unselected_facts(before_suppliers: Layers<Counts>, before_customers: Layers<Counts>, before_orders: Layers<Counts>,
    after_suppliers: Layers<Counts>, after_customers: Layers<Counts>, after_orders: Layers<Counts>,
    order_keys: Seq<i64>, supplier_keys: Seq<i64>) -> bool {
    &&& forall|key: i64, nation: i64| #![trigger dimension_value(after_suppliers, key, nation)]
        !supplier_keys.contains(key) ==> dimension_value(after_suppliers, key, nation) == dimension_value(before_suppliers, key, nation)
    &&& forall|order: i64, customer: i64| #![trigger dimension_value(after_orders, order, customer)]
        !order_keys.contains(order) ==> dimension_value(after_orders, order, customer) == dimension_value(before_orders, order, customer)
    &&& forall|order: i64, customer: i64, nation: i64|
        #![trigger dimension_value(before_orders, order, customer), dimension_value(after_customers, customer, nation)]
        !order_keys.contains(order) && dimension_value(before_orders, order, customer) > 0 ==>
            dimension_value(after_customers, customer, nation) == dimension_value(before_customers, customer, nation)
}

pub proof fn lemma_snapshot_unchanged(before_suppliers: Layers<Counts>, before_customers: Layers<Counts>, before_orders: Layers<Counts>,
    after_suppliers: Layers<Counts>, after_customers: Layers<Counts>, after_orders: Layers<Counts>,
    order_keys: Seq<i64>, supplier_keys: Seq<i64>, line: LineitemRow, first: i64, second: i64, year: i64, revenue: bool)
    requires unselected_facts(before_suppliers, before_customers, before_orders, after_suppliers, after_customers, after_orders, order_keys, supplier_keys),
        forall|key: i64| #![trigger before_orders.at(key)] before_orders.at(key) matches Some(bucket) ==> counts_positive(bucket),
        !seq_view_i64(order_keys).contains(line.l_orderkey), !seq_view_i64(supplier_keys).contains(line.l_suppkey),
    ensures snapshot_weight(after_suppliers, after_customers, after_orders, line, first, second, year, revenue)
        == snapshot_weight(before_suppliers, before_customers, before_orders, line, first, second, year, revenue),
{
    reveal(projected_contribution);
    if 9131 <= line.l_shipdate <= 9861 && key_range(line.l_orderkey) && key_range(line.l_suppkey)
        && key_range(line.l_extendedprice) && key_range(line.l_discount) {
        let order = line.l_orderkey as i64;
        let supplier = line.l_suppkey as i64;
        assert(order as int == line.l_orderkey && supplier as int == line.l_suppkey);
        lemma_native_keys(order_keys, order); lemma_native_keys(supplier_keys, supplier);
        lemma_order_cache_equal(before_orders, after_orders, before_customers, after_customers, order, second);
    }
}

pub proof fn lemma_unselected_facts(db: Db, db2: Db,
    before_suppliers: Layers<Counts>, before_customers: Layers<Counts>, before_orders: Layers<Counts>,
    after_suppliers: Layers<Counts>, after_customers: Layers<Counts>, after_orders: Layers<Counts>,
    suppliers: Seq<SupplierDelta>, customers: Seq<CustomerDelta>, lines: Seq<(LineitemRow, int)>, orders: Seq<(OrdersRow, int)>,
    order_keys: Seq<i64>, supplier_keys: Seq<i64>)
    requires dimension_facts(db, before_suppliers, before_customers, before_orders), dimension_facts(db2, after_suppliers, after_customers, after_orders),
        bag_step(db.supplier, db2.supplier, supplier_delta_seq_view(suppliers)), bag_step(db.customer, db2.customer, customer_delta_seq_view(customers)),
        bag_step(db.lineitem, db2.lineitem, lines), bag_step(db.orders, db2.orders, orders),
        forall|position: int| #![trigger suppliers[position]] 0 <= position < suppliers.len() ==> supplier_keys.contains(suppliers[position].row.s_suppkey),
        forall|position: int| #![trigger orders[position]] 0 <= position < orders.len() ==> seq_view_i64(order_keys).contains(orders[position].0.o_orderkey),
        forall|position: int| #![trigger lines[position]] 0 <= position < lines.len() && 9131 <= lines[position].0.l_shipdate <= 9861 ==>
            seq_view_i64(order_keys).contains(lines[position].0.l_orderkey),
        forall|position: int, order: i64| #![trigger dimension_value(before_orders, order, customers[position].row.c_custkey)]
            0 <= position < customers.len() && dimension_value(before_orders, order, customers[position].row.c_custkey) > 0 ==> order_keys.contains(order),
    ensures unselected_facts(before_suppliers, before_customers, before_orders, after_suppliers, after_customers, after_orders, order_keys, supplier_keys),
{
    assert forall|key: i64, nation: i64| #![trigger dimension_value(after_suppliers, key, nation)]
        !supplier_keys.contains(key) implies dimension_value(after_suppliers, key, nation) == dimension_value(before_suppliers, key, nation) by {
        lemma_supplier_capture(db, db2, suppliers, key, nation);
        lemma_sum_zero(suppliers, |row: SupplierDelta| if row.row.s_suppkey == key && row.row.s_nationkey == nation { row.sign as int } else { 0int });
    }
    assert forall|order: i64, customer: i64| #![trigger dimension_value(after_orders, order, customer)]
        !order_keys.contains(order) implies dimension_value(after_orders, order, customer) == dimension_value(before_orders, order, customer) by {
        lemma_native_keys(order_keys, order);
        lemma_order_untouched(db, db2, lines, orders, order, customer);
    }
    assert forall|order: i64, customer: i64, nation: i64|
        #![trigger dimension_value(before_orders, order, customer), dimension_value(after_customers, customer, nation)]
        !order_keys.contains(order) && dimension_value(before_orders, order, customer) > 0 implies
            dimension_value(after_customers, customer, nation) == dimension_value(before_customers, customer, nation) by {
        assert forall|position: int| 0 <= position < customers.len() implies (#[trigger] customers[position]).row.c_custkey != customer by {}
        lemma_customer_capture(db, db2, customers, customer, nation);
        lemma_sum_zero(customers, |row: CustomerDelta| if row.row.c_custkey == customer && row.row.c_nationkey == nation { row.sign as int } else { 0int });
    }
}

#[verifier::auto_ext_equal(assert, assert_by, ensures)]
pub proof fn lemma_rescan_weight(db: Db, keys: Seq<i64>, rows: Seq<LineInput>, by_order: bool, weight: spec_fn(LineitemRow) -> int)
    requires if by_order { rescan_window_lines_for_orders_ok(db, keys, rows) } else { rescan_window_lines_for_suppliers_ok(db, keys, rows) },
    ensures sum(line_input_seq_view(rows), weight) == sum(db.lineitem, |line: LineitemRow|
        if 9131 <= line.l_shipdate <= 9861 && seq_view_i64(keys).contains(if by_order { line.l_orderkey } else { line.l_suppkey }) { weight(line) } else { 0int }),
{
    let target = |line: LineitemRow| if 9131 <= line.l_shipdate <= 9861 && seq_view_i64(keys).contains(if by_order { line.l_orderkey } else { line.l_suppkey }) { weight(line) } else { 0int };
    if by_order {
        let original = |line: LineitemRow| if (9131 <= line.l_shipdate && line.l_shipdate <= 9861) && seq_view_i64(keys).contains(line.l_orderkey) { weight(line) } else { 0 };
        assert(rescan_window_lines_for_orders_ok(db, keys, rows));
        assert(sum(line_input_seq_view(rows), weight) == sum(db.lineitem, original));
        lemma_sum_ext_all(db.lineitem, original, target);
    } else {
        let original = |line: LineitemRow| if (9131 <= line.l_shipdate && line.l_shipdate <= 9861) && seq_view_i64(keys).contains(line.l_suppkey) { weight(line) } else { 0 };
        assert(rescan_window_lines_for_suppliers_ok(db, keys, rows));
        assert(sum(line_input_seq_view(rows), weight) == sum(db.lineitem, original));
        lemma_sum_ext_all(db.lineitem, original, target);
    }
}

pub proof fn lemma_rescan_selected(db: Db, keys: Seq<i64>, rows: Seq<LineInput>, by_order: bool)
    requires if by_order { rescan_window_lines_for_orders_ok(db, keys, rows) } else { rescan_window_lines_for_suppliers_ok(db, keys, rows) },
    ensures forall|position: int| #![trigger rows[position]] 0 <= position < rows.len() ==>
        9131 <= rows[position].l_shipdate <= 9861
            && keys.contains(if by_order { rows[position].l_orderkey } else { rows[position].l_suppkey }),
{
    reveal(rescan_window_lines_for_orders_ok); reveal(rescan_window_lines_for_suppliers_ok);
    let good = |line: LineitemRow| 9131 <= line.l_shipdate <= 9861
        && seq_view_i64(keys).contains(if by_order { line.l_orderkey } else { line.l_suppkey });
    let bad = |line: LineitemRow| if good(line) { 0int } else { 1int };
    let filtered = |line: LineitemRow| if good(line) { bad(line) } else { 0int };
    lemma_rescan_weight(db, keys, rows, by_order, bad);
    lemma_sum_zero(db.lineitem, filtered);
    lemma_sum_ext_all(db.lineitem, filtered, |line: LineitemRow| if 9131 <= line.l_shipdate <= 9861
        && seq_view_i64(keys).contains(if by_order { line.l_orderkey } else { line.l_suppkey }) { bad(line) } else { 0int });
    assert(sum(line_input_seq_view(rows), bad) == 0int);
    assert forall|position: int| #![trigger rows[position]] 0 <= position < rows.len() implies
        9131 <= rows[position].l_shipdate <= 9861
            && keys.contains(if by_order { rows[position].l_orderkey } else { rows[position].l_suppkey }) by {
        assert(line_input_seq_view(rows)[position] == line_input_view(rows[position]));
        if !good(line_input_view(rows[position])) { lemma_sum_pos(line_input_seq_view(rows), bad, position); }
        lemma_native_keys(keys, if by_order { rows[position].l_orderkey } else { rows[position].l_suppkey });
    }
}

pub open spec fn runtime_difference(before_suppliers: Layers<Counts>, before_customers: Layers<Counts>, before_orders: Layers<Counts>,
    after_suppliers: Layers<Counts>, after_customers: Layers<Counts>, after_orders: Layers<Counts>,
    excluded: Seq<i64>, first: i64, second: i64, year: i64, revenue: bool) -> spec_fn(LineInput) -> int {
    |line: LineInput| if excluded.contains(line.l_orderkey) { 0int } else {
        line_weight(after_suppliers, after_customers, after_orders, line, first, second, year, revenue)
            - line_weight(before_suppliers, before_customers, before_orders, line, first, second, year, revenue)
    }
}

pub open spec fn snapshot_difference(before_suppliers: Layers<Counts>, before_customers: Layers<Counts>, before_orders: Layers<Counts>,
    after_suppliers: Layers<Counts>, after_customers: Layers<Counts>, after_orders: Layers<Counts>,
    excluded: Seq<i64>, first: i64, second: i64, year: i64, revenue: bool) -> spec_fn(LineitemRow) -> int {
    |line: LineitemRow| if seq_view_i64(excluded).contains(line.l_orderkey) { 0int } else {
        snapshot_weight(after_suppliers, after_customers, after_orders, line, first, second, year, revenue)
            - snapshot_weight(before_suppliers, before_customers, before_orders, line, first, second, year, revenue)
    }
}

pub open spec fn selected_difference(keys: Seq<i64>, by_order: bool,
    before_suppliers: Layers<Counts>, before_customers: Layers<Counts>, before_orders: Layers<Counts>,
    after_suppliers: Layers<Counts>, after_customers: Layers<Counts>, after_orders: Layers<Counts>,
    excluded: Seq<i64>, first: i64, second: i64, year: i64, revenue: bool) -> spec_fn(LineitemRow) -> int {
    |line: LineitemRow| if 9131 <= line.l_shipdate <= 9861 && seq_view_i64(keys).contains(if by_order { line.l_orderkey } else { line.l_suppkey }) {
        snapshot_difference(before_suppliers, before_customers, before_orders, after_suppliers, after_customers, after_orders,
            excluded, first, second, year, revenue)(line)
    } else { 0int }
}

#[verifier::auto_ext_equal(assert, assert_by, ensures)]
pub proof fn lemma_rescan_difference(db: Db, keys: Seq<i64>, rows: Seq<LineInput>, by_order: bool,
    before_suppliers: Layers<Counts>, before_customers: Layers<Counts>, before_orders: Layers<Counts>,
    after_suppliers: Layers<Counts>, after_customers: Layers<Counts>, after_orders: Layers<Counts>,
    excluded: Seq<i64>, first: i64, second: i64, year: i64, revenue: bool)
    requires if by_order { rescan_window_lines_for_orders_ok(db, keys, rows) } else { rescan_window_lines_for_suppliers_ok(db, keys, rows) },
    ensures sum(rows, runtime_difference(before_suppliers, before_customers, before_orders, after_suppliers, after_customers, after_orders,
        excluded, first, second, year, revenue)) == sum(db.lineitem, selected_difference(keys, by_order,
            before_suppliers, before_customers, before_orders, after_suppliers, after_customers, after_orders,
            excluded, first, second, year, revenue)),
{
    reveal(rescan_window_lines_for_orders_ok); reveal(rescan_window_lines_for_suppliers_ok);
    reveal(line_weight);
    lemma_rescan_selected(db, keys, rows, by_order);
    let native = runtime_difference(before_suppliers, before_customers, before_orders, after_suppliers, after_customers, after_orders,
        excluded, first, second, year, revenue);
    let projected = snapshot_difference(before_suppliers, before_customers, before_orders, after_suppliers, after_customers, after_orders,
        excluded, first, second, year, revenue);
    let lifted = |line: LineInput| projected(line_input_view(line));
    assert forall|position: int| 0 <= position < rows.len() implies native(#[trigger] rows[position]) == lifted(rows[position]) by {
        lemma_native_keys(excluded, rows[position].l_orderkey);
    }
    lemma_sum_ext(rows, native, lifted);
    lemma_sum_map(rows, |line: LineInput| line_input_view(line), projected, lifted);
    let filtered = selected_difference(keys, by_order, before_suppliers, before_customers, before_orders,
        after_suppliers, after_customers, after_orders, excluded, first, second, year, revenue);
    lemma_rescan_weight(db, keys, rows, by_order, projected);
    lemma_sum_ext_all(db.lineitem, filtered, |line: LineitemRow| if 9131 <= line.l_shipdate <= 9861
        && seq_view_i64(keys).contains(if by_order { line.l_orderkey } else { line.l_suppkey }) { projected(line) } else { 0int });
}

pub proof fn lemma_capture_snapshot(suppliers: Layers<Counts>, customers: Layers<Counts>, orders: Layers<Counts>,
    native: Seq<LineDelta>, full: Seq<(LineitemRow, int)>, first: i64, second: i64, year: i64, revenue: bool)
    requires lineitem_delta_ok(native, full),
    ensures sum(native, |line: LineDelta| captured_weight(suppliers, customers, orders, line, first, second, year, revenue))
        == sum(full, |change: (LineitemRow, int)| change.1 * snapshot_weight(suppliers, customers, orders, change.0, first, second, year, revenue)),
{
    reveal(lineitem_delta_ok);
    reveal(captured_weight);
    let projected = |line: LineDelta| captured_weight(suppliers, customers, orders, line, first, second, year, revenue);
    let original = |change: (LineitemRow, int)| change.1 * snapshot_weight(suppliers, customers, orders, change.0, first, second, year, revenue);
    let indexed = |position: int| original(full[position]);
    assert forall|position: int| 0 <= position < native.len() implies projected(#[trigger] native[position]) == indexed(position) by {
        let line = full[position].0;
        assert(line.l_orderkey == native[position].l_orderkey as int);
        assert(line.l_suppkey == native[position].l_suppkey as int);
        assert(line.l_extendedprice == native[position].l_extendedprice as int);
        assert(line.l_discount == native[position].l_discount as int);
        assert(key_range(line.l_orderkey) && key_range(line.l_suppkey) && key_range(line.l_extendedprice) && key_range(line.l_discount));
        if native[position].ship_1995 != 0 || native[position].ship_1996 != 0 {
            assert(9131 <= line.l_shipdate <= 9861);
            assert((if line.l_shipdate <= 9495 { 1995i64 } else { 1996i64 }) == if native[position].ship_1995 != 0 { 1995i64 } else { 1996i64 });
        }
    }
    lemma_sum_index(native, projected, indexed);
    lemma_sum_index(full, original, indexed);
}

#[verifier::auto_ext_equal(assert, assert_by, ensures)]
pub proof fn lemma_snapshot_transaction(db: Db, db2: Db,
    before_suppliers: Layers<Counts>, before_customers: Layers<Counts>, before_orders: Layers<Counts>,
    after_suppliers: Layers<Counts>, after_customers: Layers<Counts>, after_orders: Layers<Counts>,
    full: Seq<(LineitemRow, int)>, native: Seq<LineDelta>, order_keys: Seq<i64>, order_rows: Seq<LineInput>,
    supplier_keys: Seq<i64>, supplier_rows: Seq<LineInput>, first: i64, second: i64, year: i64, revenue: bool)
    requires unselected_facts(before_suppliers, before_customers, before_orders, after_suppliers, after_customers, after_orders, order_keys, supplier_keys),
        forall|key: i64| #![trigger before_orders.at(key)] before_orders.at(key) matches Some(bucket) ==> counts_positive(bucket),
        bag_step(db.lineitem, db2.lineitem, full), lineitem_delta_ok(native, full),
        rescan_window_lines_for_orders_ok(db2, order_keys, order_rows), rescan_window_lines_for_suppliers_ok(db2, supplier_keys, supplier_rows),
    ensures snapshot_sum(db2, after_suppliers, after_customers, after_orders, first, second, year, revenue)
        == snapshot_sum(db, before_suppliers, before_customers, before_orders, first, second, year, revenue)
        + sum(order_rows, runtime_difference(before_suppliers, before_customers, before_orders, after_suppliers, after_customers, after_orders,
            Seq::empty(), first, second, year, revenue))
        + sum(supplier_rows, runtime_difference(before_suppliers, before_customers, before_orders, after_suppliers, after_customers, after_orders,
            order_keys, first, second, year, revenue))
        + sum(native, |line: LineDelta| captured_weight(before_suppliers, before_customers, before_orders, line, first, second, year, revenue)),
{
    hide(projected_contribution); hide(count_at); hide(order_weight);
    let old_weight = snapshot_fn(before_suppliers, before_customers, before_orders, first, second, year, revenue);
    let new_weight = snapshot_fn(after_suppliers, after_customers, after_orders, first, second, year, revenue);
    let in_first = |line: LineitemRow| seq_view_i64(order_keys).contains(line.l_orderkey);
    let in_second = |line: LineitemRow| seq_view_i64(supplier_keys).contains(line.l_suppkey);
    assert forall|line: LineitemRow| !#[trigger] in_first(line) && !#[trigger] in_second(line) implies new_weight(line) == old_weight(line) by {
        lemma_snapshot_unchanged(before_suppliers, before_customers, before_orders, after_suppliers, after_customers, after_orders,
            order_keys, supplier_keys, line, first, second, year, revenue);
    }
    let first_weight = |line: LineitemRow| if in_first(line) { new_weight(line) - old_weight(line) } else { 0int };
    let second_weight = |line: LineitemRow| if !in_first(line) && in_second(line) { new_weight(line) - old_weight(line) } else { 0int };
    lemma_rescan_difference(db2, order_keys, order_rows, true, before_suppliers, before_customers, before_orders,
        after_suppliers, after_customers, after_orders, Seq::empty(), first, second, year, revenue);
    lemma_rescan_difference(db2, supplier_keys, supplier_rows, false, before_suppliers, before_customers, before_orders,
        after_suppliers, after_customers, after_orders, order_keys, first, second, year, revenue);
    let first_filtered = selected_difference(order_keys, true, before_suppliers, before_customers, before_orders,
        after_suppliers, after_customers, after_orders, Seq::empty(), first, second, year, revenue);
    let second_filtered = selected_difference(supplier_keys, false, before_suppliers, before_customers, before_orders,
        after_suppliers, after_customers, after_orders, order_keys, first, second, year, revenue);
    lemma_sum_ext_all(db2.lineitem, first_filtered, first_weight);
    lemma_sum_ext_all(db2.lineitem, second_filtered, second_weight);
    lemma_rescan_selected(db2, order_keys, order_rows, true);
    lemma_rescan_selected(db2, supplier_keys, supplier_rows, false);
    let first_native = |line: LineInput| first_weight(line_input_view(line));
    let second_native = |line: LineInput| second_weight(line_input_view(line));
    assert forall|position: int| 0 <= position < order_rows.len() implies first_native(#[trigger] order_rows[position])
        == runtime_difference(before_suppliers, before_customers, before_orders, after_suppliers, after_customers, after_orders,
            Seq::empty(), first, second, year, revenue)(order_rows[position]) by {
        reveal(line_weight); lemma_native_keys(order_keys, order_rows[position].l_orderkey);
    }
    assert forall|position: int| 0 <= position < supplier_rows.len() implies second_native(#[trigger] supplier_rows[position])
        == runtime_difference(before_suppliers, before_customers, before_orders, after_suppliers, after_customers, after_orders,
            order_keys, first, second, year, revenue)(supplier_rows[position]) by {
        reveal(line_weight); lemma_native_keys(supplier_keys, supplier_rows[position].l_suppkey); lemma_native_keys(order_keys, supplier_rows[position].l_orderkey);
    }
    lemma_sum_map(order_rows, |line: LineInput| line_input_view(line), first_weight, first_native);
    lemma_sum_map(supplier_rows, |line: LineInput| line_input_view(line), second_weight, second_native);
    lemma_sum_ext(order_rows, first_native, runtime_difference(before_suppliers, before_customers, before_orders, after_suppliers, after_customers, after_orders,
        Seq::empty(), first, second, year, revenue));
    lemma_sum_ext(supplier_rows, second_native, runtime_difference(before_suppliers, before_customers, before_orders, after_suppliers, after_customers, after_orders,
        order_keys, first, second, year, revenue));
    lemma_sum_ext_all(line_input_seq_view(order_rows), first_weight,
        |line: LineitemRow| if in_first(line) { new_weight(line) - old_weight(line) } else { 0int });
    lemma_sum_ext_all(db2.lineitem, first_weight,
        |line: LineitemRow| if in_first(line) { new_weight(line) - old_weight(line) } else { 0int });
    lemma_sum_ext_all(line_input_seq_view(supplier_rows), second_weight,
        |line: LineitemRow| if !in_first(line) && in_second(line) { new_weight(line) - old_weight(line) } else { 0int });
    lemma_sum_ext_all(db2.lineitem, second_weight,
        |line: LineitemRow| if !in_first(line) && in_second(line) { new_weight(line) - old_weight(line) } else { 0int });
    lemma_partitioned_bag_delta(db.lineitem, db2.lineitem, full, line_input_seq_view(order_rows), line_input_seq_view(supplier_rows),
        old_weight, new_weight, in_first, in_second);
    lemma_capture_snapshot(before_suppliers, before_customers, before_orders, native, full, first, second, year, revenue);
    lemma_sum_ext_all(full, |change: (LineitemRow, int)| change.1 * old_weight(change.0),
        |change: (LineitemRow, int)| change.1 * snapshot_weight(before_suppliers, before_customers, before_orders, change.0, first, second, year, revenue));
}

pub proof fn lemma_numeric_transaction(db: Db, db2: Db,
    before_suppliers: Layers<Counts>, before_customers: Layers<Counts>, before_orders: Layers<Counts>,
    after_suppliers: Layers<Counts>, after_customers: Layers<Counts>, after_orders: Layers<Counts>,
    full: Seq<(LineitemRow, int)>, native: Seq<LineDelta>, order_keys: Seq<i64>, order_rows: Seq<LineInput>,
    supplier_keys: Seq<i64>, supplier_rows: Seq<LineInput>, first: i64, second: i64, year: i64, revenue: bool)
    requires database_domain(db), database_domain(db2),
        dimension_facts(db, before_suppliers, before_customers, before_orders), dimension_facts(db2, after_suppliers, after_customers, after_orders),
        unselected_facts(before_suppliers, before_customers, before_orders, after_suppliers, after_customers, after_orders, order_keys, supplier_keys),
        forall|key: i64| #![trigger before_orders.at(key)] before_orders.at(key) matches Some(bucket) ==> counts_positive(bucket),
        bag_step(db.lineitem, db2.lineitem, full), lineitem_delta_ok(native, full),
        rescan_window_lines_for_orders_ok(db2, order_keys, order_rows), rescan_window_lines_for_suppliers_ok(db2, supplier_keys, supplier_rows),
    ensures numeric_group(db2, first, second, year, revenue) == numeric_group(db, first, second, year, revenue)
        + sum(order_rows, runtime_difference(before_suppliers, before_customers, before_orders, after_suppliers, after_customers, after_orders,
            Seq::empty(), first, second, year, revenue))
        + sum(supplier_rows, runtime_difference(before_suppliers, before_customers, before_orders, after_suppliers, after_customers, after_orders,
            order_keys, first, second, year, revenue))
        + sum(native, |line: LineDelta| captured_weight(before_suppliers, before_customers, before_orders, line, first, second, year, revenue)),
{
    hide(numeric_group); hide(fact_line); hide(supplier_count); hide(customer_count); hide(order_count);
    lemma_snapshot_transaction(db, db2, before_suppliers, before_customers, before_orders, after_suppliers, after_customers, after_orders,
        full, native, order_keys, order_rows, supplier_keys, supplier_rows, first, second, year, revenue);
    lemma_snapshot_sum(db, before_suppliers, before_customers, before_orders, first, second, year, revenue);
    lemma_snapshot_sum(db2, after_suppliers, after_customers, after_orders, first, second, year, revenue);
}

}

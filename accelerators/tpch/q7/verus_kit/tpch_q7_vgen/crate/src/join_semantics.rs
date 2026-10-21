use vstd::prelude::*;
use verus_prooflib::wide::Wide;
use verus_common::sums::*;
use verus_prooflib::*;
use crate::gen_spec::*;
use crate::gen_obligations::*;
use crate::model::*;
use crate::storage::*;
use crate::values::*;
use crate::dimensions::*;

verus! {

pub open spec fn customer_weight(db: Db, key: int, second: i64) -> int {
    sum(db.customer, |row: CustomerRow| if row.c_custkey == key && row.c_nationkey == second { 1int } else { 0int })
}

pub open spec fn order_join_weight(db: Db, key: int, second: i64) -> int {
    sum(db.orders, |row: OrdersRow| if row.o_orderkey == key { customer_weight(db, row.o_custkey, second) } else { 0int })
}

pub proof fn lemma_window_member(db: Db, rows: Seq<LineInput>, position: int) -> (witness: int)
    requires scan_window_lines_ok(db, rows), 0 <= position < rows.len(),
    ensures 0 <= witness < db.lineitem.len(), db.lineitem[witness] == line_input_view(rows[position]),
        9131 <= rows[position].l_shipdate <= 9861,
{
    let target = line_input_view(rows[position]);
    let projected = line_input_seq_view(rows);
    let weight = |row: LineitemRow| if row == target { 1int } else { 0int };
    lemma_sum_pos(projected, weight, position);
    let filtered = |row: LineitemRow| if 9131 <= row.l_shipdate <= 9861 { weight(row) } else { 0int };
    assert(sum(line_input_seq_view(rows), weight) == sum(db.lineitem,
        |row: LineitemRow| if 9131 <= row.l_shipdate && row.l_shipdate <= 9861 { weight(row) } else { 0int }));
    lemma_sum_ext_all(db.lineitem, filtered,
        |row: LineitemRow| if 9131 <= row.l_shipdate && row.l_shipdate <= 9861 { weight(row) } else { 0int });
    assert(sum(projected, weight) == sum(db.lineitem, filtered));
    lemma_sum_nonzero_witness(db.lineitem, filtered)
}

pub proof fn lemma_order_fold(db: Db, scanned: Seq<OrderInput>, orders: Layers<Counts>, customers: Layers<Counts>,
    key: i64, second: i64, witness: int)
    requires scan_relevant_orders_ok(db, scanned), 0 <= witness < db.lineitem.len(),
        db.lineitem[witness].l_orderkey == key, 9131 <= db.lineitem[witness].l_shipdate <= 9861,
        forall|customer: i64| #![trigger dimension_value(orders, key, customer)]
            dimension_value(orders, key, customer) == sum(scanned, |row: OrderInput|
                if row.o_orderkey == key && row.o_custkey == customer { 1int } else { 0int }),
        forall|customer: i64| #![trigger dimension_value(customers, customer, second)]
            dimension_value(customers, customer, second) == customer_count(db, customer, second),
    ensures (match orders.at(key) { Some(bucket) => order_weight(bucket.rows@, customers, second), None => 0int })
        == order_join_weight(db, key as int, second),
{
    reveal(customer_count);
    let collapsed = match orders.at(key) { Some(bucket) => bucket.rows@, None => Seq::empty() };
    let left_key = |row: (i64, Wide)| row.0;
    let right_key = |row: OrderInput| row.o_custkey;
    let left_count = |row: (i64, Wide)| row.1@;
    let right_count = |row: OrderInput| if row.o_orderkey == key { 1int } else { 0int };
    let value = |customer: i64| dimension_value(customers, customer, second);
    assert forall|customer: i64| #![trigger class_total(collapsed, left_key, left_count, customer)]
        class_total(collapsed, left_key, left_count, customer) == class_total(scanned, right_key, right_count, customer) by {
        if orders.at(key).is_none() {
            lemma_sum_empty(collapsed, |row: (i64, Wide)| if row.0 == customer { row.1@ } else { 0int });
        }
        lemma_sum_ext_all(collapsed,
            |row: (i64, Wide)| if left_key(row) == customer { left_count(row) } else { 0int },
            |row: (i64, Wide)| if row.0 == customer { row.1@ } else { 0int });
        assert(class_total(collapsed, left_key, left_count, customer) == dimension_value(orders, key, customer));
        lemma_sum_ext_all(scanned,
            |row: OrderInput| if right_key(row) == customer { right_count(row) } else { 0int },
            |row: OrderInput| if row.o_orderkey == key && row.o_custkey == customer { 1int } else { 0int });
    }
    lemma_equal_classes(collapsed, scanned, left_key, right_key, left_count, right_count, value);
    let database_weight = |row: OrdersRow| if row.o_orderkey == key { customer_weight(db, row.o_custkey, second) } else { 0int };
    let runtime_weight = |row: OrderInput| right_count(row) * value(right_key(row));
    assert forall|row: OrderInput| #[trigger] runtime_weight(row) == database_weight(order_input_view(row)) by {
        assert(dimension_value(customers, row.o_custkey, second) == customer_count(db, row.o_custkey, second));
    }
    lemma_sum_map(scanned, |row: OrderInput| order_input_view(row), database_weight, runtime_weight);
    let relevant_weight = |row: OrdersRow| if scan_relevant_orders_exists_1(db, row) { database_weight(row) } else { 0int };
    assert(sum(order_input_seq_view(scanned), database_weight) == sum(db.orders, relevant_weight));
    assert forall|row: OrdersRow| #[trigger] relevant_weight(row) == database_weight(row) by {
        if row.o_orderkey == key {
            assert(scan_relevant_orders_exists_1_where(row, db.lineitem[witness]));
            assert(scan_relevant_orders_exists_1(db, row));
        }
    }
    lemma_sum_ext_all(db.orders, relevant_weight, database_weight);
    assert(order_join_weight(db, key as int, second) == sum(db.orders, database_weight));
    if orders.at(key).is_none() {
        lemma_sum_empty(collapsed, |row: (i64, Wide)| left_count(row) * value(left_key(row)));
    }
    lemma_sum_ext_all(collapsed, |row: (i64, Wide)| left_count(row) * value(left_key(row)),
        |row: (i64, Wide)| row.1@ * dimension_value(customers, row.0, second));
}

pub proof fn lemma_fact_factor(db: Db, line: LineitemRow, first: i64, second: i64, year: i64, revenue: bool)
    ensures fact_line(db, line, first, second, year, revenue)
        == if 9131 <= line.l_shipdate <= 9861 && (if line.l_shipdate <= 9495 { 1995i64 } else { 1996i64 }) == year {
            sum(db.supplier, |row: SupplierRow| if row.s_suppkey == line.l_suppkey && row.s_nationkey == first { 1int } else { 0int })
                * (order_join_weight(db, line.l_orderkey, second) * if revenue { line.l_extendedprice * (100 - line.l_discount) } else { 1int })
        } else { 0int },
{
    reveal(fact_line);
    let measure = if revenue { line.l_extendedprice * (100 - line.l_discount) } else { 1int };
    let customer_measure = |order: OrdersRow| sum(db.customer, |customer: CustomerRow|
        if customer.c_custkey == order.o_custkey && customer.c_nationkey == second { measure } else { 0int });
    assert forall|order: OrdersRow| #[trigger] customer_measure(order) == measure * customer_weight(db, order.o_custkey, second) by {
        lemma_sum_scale(db.customer,
            |customer: CustomerRow| if customer.c_custkey == order.o_custkey && customer.c_nationkey == second { 1int } else { 0int },
            |customer: CustomerRow| if customer.c_custkey == order.o_custkey && customer.c_nationkey == second { measure } else { 0int }, measure);
    }
    let order_measure = |order: OrdersRow| if order.o_orderkey == line.l_orderkey { customer_measure(order) } else { 0int };
    let order_count = |order: OrdersRow| if order.o_orderkey == line.l_orderkey { customer_weight(db, order.o_custkey, second) } else { 0int };
    lemma_sum_scale(db.orders, order_count, order_measure, measure);
    let supplier_count = |row: SupplierRow| if row.s_suppkey == line.l_suppkey && row.s_nationkey == first { 1int } else { 0int };
    let supplier_measure = |row: SupplierRow| if row.s_suppkey == line.l_suppkey && row.s_nationkey == first { sum(db.orders, order_measure) } else { 0int };
    lemma_sum_scale(db.supplier, supplier_count, supplier_measure, sum(db.orders, order_measure));
    lemma_group_coefficient(sum(db.supplier, supplier_count), order_join_weight(db, line.l_orderkey, second), measure, 1int);
    let original_order = |order: OrdersRow| if order.o_orderkey == line.l_orderkey {
        sum(db.customer, |customer: CustomerRow| if customer.c_custkey == order.o_custkey && customer.c_nationkey == second {
            if revenue { line.l_extendedprice * (100 - line.l_discount) } else { 1int }
        } else { 0int })
    } else { 0int };
    assert forall|order: OrdersRow| #[trigger] original_order(order) == order_measure(order) by {
        lemma_sum_ext_all(db.customer,
            |customer: CustomerRow| if customer.c_custkey == order.o_custkey && customer.c_nationkey == second {
                if revenue { line.l_extendedprice * (100 - line.l_discount) } else { 1int }
            } else { 0int },
            |customer: CustomerRow| if customer.c_custkey == order.o_custkey && customer.c_nationkey == second { measure } else { 0int });
    }
    lemma_sum_ext_all(db.orders, original_order, order_measure);
    let original_supplier = |supplier: SupplierRow| if supplier.s_suppkey == line.l_suppkey && supplier.s_nationkey == first {
        sum(db.orders, original_order)
    } else { 0int };
    lemma_sum_ext_all(db.supplier, original_supplier, supplier_measure);
    assert(fact_line(db, line, first, second, year, revenue) == if 9131 <= line.l_shipdate <= 9861
        && (if line.l_shipdate <= 9495 { 1995i64 } else { 1996i64 }) == year { sum(db.supplier, supplier_measure) } else { 0int });
}

pub proof fn lemma_line_factor(db: Db, scanned_orders: Seq<OrderInput>, line: LineInput,
    suppliers: Layers<Counts>, customers: Layers<Counts>, orders: Layers<Counts>,
    first: i64, second: i64, year: i64, revenue: bool, witness: int)
    requires scan_relevant_orders_ok(db, scanned_orders), 0 <= witness < db.lineitem.len(),
        db.lineitem[witness] == line_input_view(line), 9131 <= line.l_shipdate <= 9861,
        dimension_value(suppliers, line.l_suppkey, first) == supplier_count(db, line.l_suppkey, first),
        forall|customer: i64| #![trigger dimension_value(customers, customer, second)]
            dimension_value(customers, customer, second) == customer_count(db, customer, second),
        forall|customer: i64| #![trigger dimension_value(orders, line.l_orderkey, customer)]
            dimension_value(orders, line.l_orderkey, customer) == order_count(db, line.l_orderkey, customer),
    ensures line_weight(suppliers, customers, orders, line, first, second, year, revenue)
        == fact_line(db, line_input_view(line), first, second, year, revenue),
{
    reveal(line_weight);
    reveal(projected_contribution);
    reveal(supplier_count);
    assert forall|customer: i64| #![trigger dimension_value(orders, line.l_orderkey, customer)]
        dimension_value(orders, line.l_orderkey, customer) == sum(scanned_orders, |row: OrderInput|
            if row.o_orderkey == line.l_orderkey && row.o_custkey == customer { 1int } else { 0int }) by {
        lemma_order_scan(db, scanned_orders, line.l_orderkey, customer);
    }
    lemma_order_fold(db, scanned_orders, orders, customers, line.l_orderkey, second, witness);
    lemma_fact_factor(db, line_input_view(line), first, second, year, revenue);
    let measure = if revenue { line.l_extendedprice as int * (100 - line.l_discount as int) } else { 1int };
    lemma_group_coefficient(dimension_value(suppliers, line.l_suppkey, first),
        order_join_weight(db, line.l_orderkey as int, second), 1int, measure);
}

pub proof fn lemma_build_groups(db: Db, scanned_orders: Seq<OrderInput>, lines: Seq<LineInput>,
    suppliers: Layers<Counts>, customers: Layers<Counts>, orders: Layers<Counts>,
    first: i64, second: i64, year: i64, revenue: bool)
    requires scan_relevant_orders_ok(db, scanned_orders), scan_window_lines_ok(db, lines),
        forall|key: i64, other: i64| #![trigger dimension_value(suppliers, key, other)]
            dimension_value(suppliers, key, other) == supplier_count(db, key, other),
        forall|key: i64, other: i64| #![trigger dimension_value(customers, key, other)]
            dimension_value(customers, key, other) == customer_count(db, key, other),
        forall|key: i64, other: i64| #![trigger dimension_value(orders, key, other)]
            dimension_value(orders, key, other) == order_count(db, key, other),
    ensures sum(lines, |line: LineInput| line_weight(suppliers, customers, orders, line, first, second, year, revenue))
        == numeric_group(db, first, second, year, revenue),
{
    reveal(numeric_group);
    let database_weight = |line: LineitemRow| fact_line(db, line, first, second, year, revenue);
    let runtime_weight = |line: LineInput| line_weight(suppliers, customers, orders, line, first, second, year, revenue);
    let projected = line_input_seq_view(lines);
    assert forall|position: int| #![trigger lines[position], projected[position]]
        0 <= position < lines.len() implies runtime_weight(lines[position]) == database_weight(projected[position]) by {
        let witness = lemma_window_member(db, lines, position);
        lemma_line_factor(db, scanned_orders, lines[position], suppliers, customers, orders, first, second, year, revenue, witness);
    }
    lemma_sum_aligned(lines, projected, runtime_weight, database_weight);
    let filtered = |line: LineitemRow| if 9131 <= line.l_shipdate && line.l_shipdate <= 9861 { database_weight(line) } else { 0int };
    assert(sum(projected, database_weight) == sum(db.lineitem, filtered));
    assert forall|line: LineitemRow| #[trigger] filtered(line) == database_weight(line) by {
        reveal(fact_line);
    }
    lemma_sum_ext_all(db.lineitem, filtered, database_weight);
}

pub proof fn lemma_group_install(previous: Layers<Groups>, changes: Sparse<Groups>, next: Layers<Groups>,
    first: i64, second: i64, year: i64, revenue: bool)
    requires next.at(first) == if changes.at(first).is_some() { normalize(changes.at(first)) } else { previous.at(first) },
    ensures group_value(next, first, second, year, revenue)
        == if changes.at(first).is_some() { sparse_group(changes, first, second, year, revenue) }
            else { group_value(previous, first, second, year, revenue) },
{
    if let Some(bucket) = changes.at(first) {
        if bucket.empty() {
            lemma_sum_empty(bucket.rows@, |entry: Cell| if entry.other == second && entry.year == year {
                if revenue { entry.revenue@ } else { entry.count@ }
            } else { 0int });
        }
    }
}

pub proof fn lemma_group_layer_meaning(db: Db, rows: Sparse<Groups>, groups: Layers<Groups>)
    requires forall|first: i64| #![trigger groups.at(first)] groups.at(first) == normalize(rows.at(first)),
        forall|first: i64, second: i64, year: i64, measure: bool|
            #![trigger sparse_group(rows, first, second, year, measure)]
            sparse_group(rows, first, second, year, measure) == numeric_group(db, first, second, year, measure),
    ensures forall|first: i64, second: i64, year: i64, measure: bool|
        #![trigger group_value(groups, first, second, year, measure)]
        group_value(groups, first, second, year, measure) == numeric_group(db, first, second, year, measure),
{
    assert forall|first: i64, second: i64, year: i64, measure: bool|
        #![trigger group_value(groups, first, second, year, measure)]
        group_value(groups, first, second, year, measure) == numeric_group(db, first, second, year, measure) by {
        lemma_group_install(groups, rows, groups, first, second, year, measure);
        if rows.at(first).is_none() { assert(groups.at(first).is_none()); }
        assert(group_value(groups, first, second, year, measure) == sparse_group(rows, first, second, year, measure));
        assert(sparse_group(rows, first, second, year, measure) == numeric_group(db, first, second, year, measure));
    }
}

pub proof fn lemma_group_layer_shape(rows: Sparse<Groups>, groups: Layers<Groups>)
    requires
        sparse_groups_shape(rows),
        forall|key: i64| #![trigger rows.at(key)] rows.at(key) matches Some(bucket) ==> groups_positive(bucket),
        forall|key: i64| #![trigger groups.at(key)] groups.at(key) == normalize(rows.at(key)),
    ensures forall|key: i64| #![trigger groups.at(key)] groups.at(key) matches Some(bucket) ==> groups_valid(bucket),
{
    assert forall|key: i64| #![trigger groups.at(key)] groups.at(key) matches Some(bucket) ==> groups_valid(bucket) by {
        if let Some(bucket) = rows.at(key) { assert(groups_shape(bucket)); assert(groups_positive(bucket)); }
    }
}


pub proof fn lemma_numeric_group_support(db: Db, first: i64, second: i64, year: i64)
    ensures numeric_group(db, first, second, year, false) >= 0,
        numeric_group(db, first, second, year, false) == 0 ==> numeric_group(db, first, second, year, true) == 0,
{
    reveal(numeric_group);
    let counts = |line: LineitemRow| fact_line(db, line, first, second, year, false);
    let revenues = |line: LineitemRow| fact_line(db, line, first, second, year, true);
    assert forall|line: LineitemRow| #[trigger] counts(line) >= 0 by {
        lemma_fact_factor(db, line, first, second, year, false);
        lemma_sum_nonneg(db.supplier, |row: SupplierRow| if row.s_suppkey == line.l_suppkey && row.s_nationkey == first { 1int } else { 0int });
        lemma_order_join_nonneg(db, line.l_orderkey, second);
        let supplier = sum(db.supplier, |row: SupplierRow| if row.s_suppkey == line.l_suppkey && row.s_nationkey == first { 1int } else { 0int });
        let joined = order_join_weight(db, line.l_orderkey, second);
        assert(supplier * (joined * 1) >= 0) by (nonlinear_arith) requires supplier >= 0, joined >= 0;
    }
    lemma_sum_nonneg(db.lineitem, counts);
    if sum(db.lineitem, counts) == 0 {
        assert forall|index: int| 0 <= index < db.lineitem.len() implies revenues(#[trigger] db.lineitem[index]) == 0 by {
            let line = db.lineitem[index];
            if counts(line) > 0 { lemma_sum_pos(db.lineitem, counts, index); }
            assert(counts(line) == 0);
            lemma_fact_factor(db, line, first, second, year, false);
            lemma_fact_factor(db, line, first, second, year, true);
            let supplier = sum(db.supplier, |row: SupplierRow| if row.s_suppkey == line.l_suppkey && row.s_nationkey == first { 1int } else { 0int });
            let joined = order_join_weight(db, line.l_orderkey, second);
            let volume = line.l_extendedprice * (100 - line.l_discount);
            if 9131 <= line.l_shipdate <= 9861 && (if line.l_shipdate <= 9495 { 1995i64 } else { 1996i64 }) == year {
                assert(supplier * (joined * 1) == 0);
                assert(supplier * (joined * volume) == 0) by (nonlinear_arith) requires supplier * (joined * 1) == 0;
            }
        }
        lemma_sum_zero(db.lineitem, revenues);
    }
}

pub proof fn lemma_order_join_nonneg(db: Db, key: int, second: i64)
    ensures order_join_weight(db, key, second) >= 0,
{
    let term = |row: OrdersRow| if row.o_orderkey == key { customer_weight(db, row.o_custkey, second) } else { 0int };
    assert forall|index: int| 0 <= index < db.orders.len() implies term(#[trigger] db.orders[index]) >= 0 by {
        lemma_sum_nonneg(db.customer, |row: CustomerRow| if row.c_custkey == db.orders[index].o_custkey && row.c_nationkey == second { 1int } else { 0int });
    }
    lemma_sum_nonneg(db.orders, term);
}


pub proof fn lemma_group_bucket_positive(db: Db, groups: Sparse<Groups>, first: i64)
    requires sparse_groups_shape(groups),
        forall|second: i64, year: i64, measure: bool| #![trigger sparse_group(groups, first, second, year, measure)]
            sparse_group(groups, first, second, year, measure) == numeric_group(db, first, second, year, measure),
    ensures groups.at(first) matches Some(bucket) ==> groups_valid(bucket),
{
    if let Some(bucket) = groups.at(first) {
        assert(groups_shape(bucket));
        assert forall|offset: int| 0 <= offset < bucket.rows@.len() implies (#[trigger] bucket.rows@[offset]).count@ > 0 by {
            let cell = bucket.rows@[offset];
            lemma_group_entry(bucket.rows@, offset, false);
            lemma_group_entry(bucket.rows@, offset, true);
            assert(sparse_group(groups, first, cell.other, cell.year, false) == group_at(bucket.rows@, cell.other, cell.year, false));
            assert(sparse_group(groups, first, cell.other, cell.year, true) == group_at(bucket.rows@, cell.other, cell.year, true));
            lemma_numeric_group_support(db, first, cell.other, cell.year);
        }
    }
}

}

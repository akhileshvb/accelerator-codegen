use vstd::prelude::*;
use verus_common::sums::*;
use verus_common::bag::*;
use crate::maintenance::Keys;
use crate::contributions::*;
use crate::gen_spec::*;
use crate::gen_obligations::*;
use crate::model::*;
use crate::table::*;

verus! {

pub open spec fn scalar_rows(rows: Seq<Cell>) -> bool {
    forall|index: int| 0 <= index < rows.len() ==>
        (#[trigger] rows[index]).second == 0 && rows[index].third == 0
}

pub open spec fn join_ready(data: Data) -> bool {
    scalar(data.parts) && scalar(data.suppliers) && scalar(data.customers)
        && forall|key: Major, index: int| 0 <= index < data.orders.rows(key).len() ==> (#[trigger] data.orders.rows(key)[index]).third == 0
}

#[verifier::opaque]
pub open spec fn order_projection(rows: Seq<Cell>, customers: Table, customer: i64, year: i64) -> int {
    sum(rows, |order: Cell| if order.second == year {
        order.count@ * customers.at((order.first, 0, 0), (customer, 0, 0), false)
    } else { 0 })
}

proof fn lemma_customer_factor(customers: Table, part: Cell, supplier: Cell, order: Cell,
    key: Major, minor: Major, sign: int, volume: int, measure: bool)
    requires scalar(customers),
    ensures customer_effect(customers.rows((order.first, 0, 0)), part, supplier, order, key, minor, sign, volume, measure)
        == if key.1 == supplier.first && key.2 == 0 && minor == (part.first, order.second, 0) {
            sign * part.count@ * supplier.count@ * order.count@
                * (if measure { volume } else { 1 }) * customers.at((order.first, 0, 0), (key.0, 0, 0), false)
        } else { 0 },
{
    hide(sum);
    reveal(customer_effect);
    let rows = customers.rows((order.first, 0, 0));
    let raw = |customer: Cell| if key == (customer.first, supplier.first, 0) && minor == (part.first, order.second, 0) {
        sign * part.count@ * supplier.count@ * order.count@ * customer.count@
            * if measure { volume } else { 1 }
    } else { 0int };
    if key.1 == supplier.first && key.2 == 0 && minor == (part.first, order.second, 0) {
        let coefficient = sign * part.count@ * supplier.count@ * order.count@
            * if measure { volume } else { 1 };
        let weight = |customer: Cell| cell_weight(customer, (key.0, 0, 0), false);
        let scaled = |customer: Cell| coefficient * weight(customer);
        assert forall|index: int| 0 <= index < rows.len() implies raw(#[trigger] rows[index]) == scaled(rows[index]) by {
            let customer = rows[index];
            assert(customer.second == 0 && customer.third == 0);
            if customer.first == key.0 {
                assert(sign * part.count@ * supplier.count@ * order.count@ * customer.count@
                    * (if measure { volume } else { 1 }) == coefficient * customer.count@) by (nonlinear_arith)
                    requires coefficient == sign * part.count@ * supplier.count@ * order.count@ * (if measure { volume } else { 1 });
            }
        }
        lemma_sum_ext(rows, raw, scaled);
        lemma_sum_scale(rows, weight, scaled, coefficient);
        reveal(Table::at);
        reveal(cell_sum);
    } else {
        lemma_sum_zero(rows, raw);
    }
}

proof fn lemma_order_factor(rows: Seq<Cell>, customers: Table, part: Cell, supplier: Cell,
    key: Major, minor: Major, sign: int, volume: int, measure: bool)
    requires scalar(customers),
    ensures order_effect(rows, customers, part, supplier, key, minor, sign, volume, measure)
        == if key.1 == supplier.first && key.2 == 0 && minor.0 == part.first && minor.2 == 0 {
            sign * part.count@ * supplier.count@ * (if measure { volume } else { 1 })
                * order_projection(rows, customers, key.0, minor.1)
        } else { 0 },
{
    hide(sum);
    reveal(order_effect);
    let raw = |order: Cell| customer_effect(customers.rows((order.first, 0, 0)), part, supplier, order, key, minor, sign, volume, measure);
    let active = key.1 == supplier.first && key.2 == 0 && minor.0 == part.first && minor.2 == 0;
    let coefficient = if active { sign * part.count@ * supplier.count@ * (if measure { volume } else { 1 }) } else { 0int };
    let weight = |order: Cell| if order.second == minor.1 {
        order.count@ * customers.at((order.first, 0, 0), (key.0, 0, 0), false)
    } else { 0int };
    let scaled = |order: Cell| coefficient * weight(order);
    assert forall|index: int| 0 <= index < rows.len() implies raw(#[trigger] rows[index]) == scaled(rows[index]) by {
        let order = rows[index];
        lemma_customer_factor(customers, part, supplier, order, key, minor, sign, volume, measure);
        if active && order.second == minor.1 {
            let count = customers.at((order.first, 0, 0), (key.0, 0, 0), false);
            assert(sign * part.count@ * supplier.count@ * order.count@
                * (if measure { volume } else { 1 }) * count == coefficient * (order.count@ * count)) by (nonlinear_arith)
                requires coefficient == sign * part.count@ * supplier.count@ * (if measure { volume } else { 1 });
        }
    }
    lemma_sum_ext(rows, raw, scaled);
    lemma_sum_scale(rows, weight, scaled, coefficient);
    reveal(order_projection);
}

proof fn lemma_supplier_factor(rows: Seq<Cell>, orders: Seq<Cell>, customers: Table, part: Cell,
    key: Major, minor: Major, sign: int, volume: int, measure: bool)
    requires scalar(customers), scalar_rows(rows),
    ensures supplier_effect(rows, orders, customers, part, key, minor, sign, volume, measure)
        == if key.2 == 0 && minor.0 == part.first && minor.2 == 0 {
            sign * part.count@ * (if measure { volume } else { 1 })
                * order_projection(orders, customers, key.0, minor.1) * cell_sum(rows, (key.1, 0, 0), false)
        } else { 0 },
{
    hide(sum);
    reveal(supplier_effect);
    let raw = |supplier: Cell| order_effect(orders, customers, part, supplier, key, minor, sign, volume, measure);
    let active = key.2 == 0 && minor.0 == part.first && minor.2 == 0;
    let coefficient = if active { sign * part.count@ * (if measure { volume } else { 1 })
        * order_projection(orders, customers, key.0, minor.1) } else { 0int };
    let weight = |supplier: Cell| cell_weight(supplier, (key.1, 0, 0), false);
    let scaled = |supplier: Cell| coefficient * weight(supplier);
    assert forall|index: int| 0 <= index < rows.len() implies raw(#[trigger] rows[index]) == scaled(rows[index]) by {
        let supplier = rows[index];
        lemma_order_factor(orders, customers, part, supplier, key, minor, sign, volume, measure);
        if active && supplier.first == key.1 {
            let projected = order_projection(orders, customers, key.0, minor.1);
            assert(sign * part.count@ * supplier.count@ * (if measure { volume } else { 1 }) * projected
                == coefficient * supplier.count@) by (nonlinear_arith)
                requires coefficient == sign * part.count@ * (if measure { volume } else { 1 }) * projected;
        }
    }
    lemma_sum_ext(rows, raw, scaled);
    lemma_sum_scale(rows, weight, scaled, coefficient);
    reveal(cell_sum);
}

pub proof fn lemma_line_factor(parts: Table, suppliers: Table, customers: Table, orders: Table,
    line: LineitemRow, key: Major, minor: Major, sign: int, measure: bool)
    requires scalar(parts), scalar(suppliers), scalar(customers),
        i64::MIN <= line.l_partkey <= i64::MAX, i64::MIN <= line.l_suppkey <= i64::MAX, i64::MIN <= line.l_orderkey <= i64::MAX,
    ensures line_effect(parts, suppliers, customers, orders, line, key, minor, sign, measure)
        == sign * projected_line_weight(parts, suppliers, customers, orders, line, key, minor)
            * line_measure(line, measure),
        projected_line_weight(parts, suppliers, customers, orders, line, key, minor) ==
            if key.2 == 0 && minor.2 == 0 {
                parts.at((line.l_partkey as i64, 0, 0), (minor.0, 0, 0), false)
                    * suppliers.at((line.l_suppkey as i64, 0, 0), (key.1, 0, 0), false)
                    * order_projection(orders.rows((line.l_orderkey as i64, 0, 0)), customers, key.0, minor.1)
            } else { 0 },
{
    hide(sum);
    let part_rows = parts.rows((line.l_partkey as i64, 0, 0));
    let supplier_rows = suppliers.rows((line.l_suppkey as i64, 0, 0));
    let order_rows = orders.rows((line.l_orderkey as i64, 0, 0));
    reveal(projected_line_weight);
    let volume = line.l_extendedprice * (100 - line.l_discount);
    reveal(part_effect);
    let raw = |part: Cell| supplier_effect(supplier_rows, order_rows, customers, part, key, minor, sign, volume, measure);
    let projected = order_projection(order_rows, customers, key.0, minor.1);
    let supplier_count = cell_sum(supplier_rows, (key.1, 0, 0), false);
    let coefficient = if key.2 == 0 && minor.2 == 0 { sign * (if measure { volume } else { 1 }) * projected * supplier_count } else { 0int };
    let weight = |part: Cell| cell_weight(part, (minor.0, 0, 0), false);
    let scaled = |part: Cell| coefficient * weight(part);
    assert(scalar_rows(supplier_rows));
    assert forall|index: int| 0 <= index < part_rows.len() implies raw(#[trigger] part_rows[index]) == scaled(part_rows[index]) by {
        let part = part_rows[index];
        lemma_supplier_factor(supplier_rows, order_rows, customers, part, key, minor, sign, volume, measure);
        if key.2 == 0 && minor.2 == 0 && part.first == minor.0 {
            assert(sign * part.count@ * (if measure { volume } else { 1 }) * projected * supplier_count
                == coefficient * part.count@) by (nonlinear_arith)
                requires coefficient == sign * (if measure { volume } else { 1 }) * projected * supplier_count;
        }
    }
    lemma_sum_ext(part_rows, raw, scaled);
    lemma_sum_scale(part_rows, weight, scaled, coefficient);
    reveal(Table::at);
    reveal(cell_sum);
    reveal(order_projection);
    assert(line_effect(parts, suppliers, customers, orders, line, key, minor, sign, measure) == sum(part_rows, raw));
    assert(sum(part_rows, weight) == parts.at((line.l_partkey as i64, 0, 0), (minor.0, 0, 0), false));
    assert(supplier_count == suppliers.at((line.l_suppkey as i64, 0, 0), (key.1, 0, 0), false));
    assert(line_effect(parts, suppliers, customers, orders, line, key, minor, sign, measure)
        == coefficient * parts.at((line.l_partkey as i64, 0, 0), (minor.0, 0, 0), false));
    assert(projected_line_weight(parts, suppliers, customers, orders, line, key, minor)
        == if key.2 == 0 && minor.2 == 0 {
            parts.at((line.l_partkey as i64, 0, 0), (minor.0, 0, 0), false) * supplier_count * projected
        } else { 0 });
    if key.2 == 0 && minor.2 == 0 {
        let part_count = parts.at((line.l_partkey as i64, 0, 0), (minor.0, 0, 0), false);
        assert(coefficient * part_count == sign * (part_count * supplier_count * projected) * (if measure { volume } else { 1 })) by (nonlinear_arith)
            requires coefficient == sign * (if measure { volume } else { 1 }) * projected * supplier_count;
        assert(line_effect(parts, suppliers, customers, orders, line, key, minor, sign, measure)
            == sign * projected_line_weight(parts, suppliers, customers, orders, line, key, minor)
                * (if measure { line.l_extendedprice * (100 - line.l_discount) } else { 1 }));
    } else {
        assert(projected_line_weight(parts, suppliers, customers, orders, line, key, minor) == 0);
        assert(line_effect(parts, suppliers, customers, orders, line, key, minor, sign, measure) == 0);
        assert(line_effect(parts, suppliers, customers, orders, line, key, minor, sign, measure)
            == sign * projected_line_weight(parts, suppliers, customers, orders, line, key, minor)
                * (if measure { line.l_extendedprice * (100 - line.l_discount) } else { 1 }));
    }
    assert(line_effect(parts, suppliers, customers, orders, line, key, minor, sign, measure)
        == sign * projected_line_weight(parts, suppliers, customers, orders, line, key, minor) * line_measure(line, measure));
}

pub proof fn lemma_order_projection(data: Data, db: Db, orderkey: i64, customer: i64, year: i64)
    requires dimension_meaning(data, db),
        forall|key: Major, index: int| 0 <= index < data.orders.rows(key).len() ==>
            (#[trigger] data.orders.rows(key)[index]).third == 0,
    ensures order_projection(data.orders.rows((orderkey, 0, 0)), data.customers, customer, year)
        == sum(db.orders, |order: OrdersRow|
            if order.o_orderkey == orderkey as int && year_of(order.o_orderdate) == year as int && year != 0 {
                data.customers.at((order.o_custkey as i64, 0, 0), (customer, 0, 0), false)
            } else { 0 }),
{
    hide(sum);
    reveal(dimension_meaning);
    let rows = data.orders.rows((orderkey, 0, 0));
    let left_project = |cell: Cell| (cell.first as int, cell.second as int);
    let right_project = |order: OrdersRow| (order.o_custkey, year_of(order.o_orderdate));
    let left_multiplicity = |cell: Cell| cell.count@;
    let right_multiplicity = |order: OrdersRow| if order.o_orderkey == orderkey as int && year_of(order.o_orderdate) != 0 { 1int } else { 0int };
    let weight = |class: (int, int)| if class.1 == year as int && i64::MIN <= class.0 <= i64::MAX {
        data.customers.at((class.0 as i64, 0, 0), (customer, 0, 0), false)
    } else { 0int };
    assert forall|class: (int, int)| #[trigger] verus_prooflib::class_total(rows, left_project, left_multiplicity, class)
        == verus_prooflib::class_total(db.orders, right_project, right_multiplicity, class) by {
        reveal(verus_prooflib::class_total);
        let left = |cell: Cell| if left_project(cell) == class { left_multiplicity(cell) } else { 0int };
        let right = |order: OrdersRow| if right_project(order) == class { right_multiplicity(order) } else { 0int };
        if i64::MIN <= class.0 <= i64::MAX && i64::MIN <= class.1 <= i64::MAX {
            let small = (class.0 as i64, class.1 as i64, 0i64);
            assert forall|index: int| 0 <= index < rows.len() implies left(#[trigger] rows[index]) == cell_weight(rows[index], small, false) by {
                assert(rows[index].third == 0);
            }
            lemma_sum_ext(rows, left, |cell: Cell| cell_weight(cell, small, false));
            reveal(Table::at);
            reveal(cell_sum);
            let canonical = |order: OrdersRow| if order.o_orderkey == orderkey as int && order.o_custkey == class.0
                && class.1 != 0 && year_of(order.o_orderdate) == class.1 { 1int } else { 0int };
            lemma_sum_ext_all(db.orders, right, canonical);
            assert(data.orders.at((orderkey, 0, 0), small, false) == sum(db.orders, canonical));
        } else {
            lemma_sum_zero(rows, left);
            assert forall|index: int| 0 <= index < db.orders.len() implies right(#[trigger] db.orders[index]) == 0 by {
                assert(order_represented(db.orders[index]));
            }
            lemma_sum_zero(db.orders, right);
        }
    }
    verus_prooflib::lemma_equal_classes(rows, db.orders, left_project, right_project, left_multiplicity, right_multiplicity, weight);
    let left_weight = |cell: Cell| left_multiplicity(cell) * weight(left_project(cell));
    let left_core = |cell: Cell| if cell.second == year {
        cell.count@ * data.customers.at((cell.first, 0, 0), (customer, 0, 0), false)
    } else { 0int };
    lemma_sum_ext_all(rows, left_weight, left_core);
    let right_weight = |order: OrdersRow| right_multiplicity(order) * weight(right_project(order));
    let right_core = |order: OrdersRow| if order.o_orderkey == orderkey as int && year_of(order.o_orderdate) == year as int && year != 0 {
        data.customers.at((order.o_custkey as i64, 0, 0), (customer, 0, 0), false)
    } else { 0int };
    assert forall|index: int| 0 <= index < db.orders.len() implies right_weight(#[trigger] db.orders[index]) == right_core(db.orders[index]) by {
        assert(order_represented(db.orders[index]));
    }
    lemma_sum_ext(db.orders, right_weight, right_core);
    reveal(order_projection);
}

pub proof fn lemma_line_database(data: Data, db: Db, line: LineitemRow, customer: i64, supplier: i64,
    kind: i64, year: i64, sign: int, measure: bool)
    requires dimension_meaning(data, db), scalar(data.parts), scalar(data.suppliers), scalar(data.customers),
        i64::MIN <= line.l_partkey <= i64::MAX, i64::MIN <= line.l_suppkey <= i64::MAX, i64::MIN <= line.l_orderkey <= i64::MAX,
        forall|key: Major, index: int| 0 <= index < data.orders.rows(key).len() ==> (#[trigger] data.orders.rows(key)[index]).third == 0,
    ensures line_effect(data.parts, data.suppliers, data.customers, data.orders, line, (customer, supplier, 0), (kind, year, 0), sign, measure)
        == sign * line_weight(data, db, line, customer, supplier, kind, year) * line_measure(line, measure),
{
    hide(sum);
    lemma_line_factor(data.parts, data.suppliers, data.customers, data.orders, line, (customer, supplier, 0), (kind, year, 0), sign, measure);
    lemma_order_projection(data, db, line.l_orderkey as i64, customer, year);
    reveal(order_projection);
    reveal(projected_line_weight);
    reveal(line_weight);
    assert(projected_line_weight(data.parts, data.suppliers, data.customers, data.orders, line, (customer, supplier, 0), (kind, year, 0))
        == line_weight(data, db, line, customer, supplier, kind, year));
}

pub proof fn lemma_excluded_line(data: Data, db: Db, line: LineitemRow, customer: i64, supplier: i64, kind: i64, year: i64)
    requires !scan_lines_exists_1(db, line),
    ensures line_weight(data, db, line, customer, supplier, kind, year) == 0,
{
    hide(sum);
    let weight = |order: OrdersRow| if order.o_orderkey == line.l_orderkey && year_of(order.o_orderdate) == year as int && year != 0 {
        data.customers.at((order.o_custkey as i64, 0, 0), (customer, 0, 0), false)
    } else { 0int };
    assert forall|index: int| 0 <= index < db.orders.len() implies weight(#[trigger] db.orders[index]) == 0 by {
        if db.orders[index].o_orderkey == line.l_orderkey && year_of(db.orders[index].o_orderdate) != 0 {
            assert(scan_lines_exists_1_where(line, db.orders[index]));
            assert(scan_lines_exists_1(db, line));
        }
    }
    lemma_sum_zero(db.orders, weight);
    reveal(line_weight);
}

pub proof fn lemma_build_base(data: Data, db: Db, rows: Seq<LineInput>)
    requires dimension_meaning(data, db), scan_lines_ok(db, rows), scalar(data.parts), scalar(data.suppliers), scalar(data.customers),
        forall|key: Major, index: int| 0 <= index < data.orders.rows(key).len() ==> (#[trigger] data.orders.rows(key)[index]).third == 0,
        forall|customer: i64, supplier: i64, kind: i64, year: i64, measure: bool|
            #[trigger] data.base.at((customer, supplier, 0), (kind, year, 0), measure) ==
                sum(line_input_seq_view(rows), |line: LineitemRow| line_effect(data.parts, data.suppliers, data.customers, data.orders,
                    line, (customer, supplier, 0), (kind, year, 0), 1, measure)),
    ensures base_meaning(data, db),
{
    hide(sum);
    reveal(base_meaning);
    let scanned = line_input_seq_view(rows);
    assert forall|customer: i64, supplier: i64, kind: i64, year: i64, measure: bool|
        #[trigger] data.base.at((customer, supplier, 0), (kind, year, 0), measure) ==
            sum(db.lineitem, |line: LineitemRow| line_weight(data, db, line, customer, supplier, kind, year) * line_measure(line, measure)) by {
        let raw = |line: LineitemRow| line_effect(data.parts, data.suppliers, data.customers, data.orders,
            line, (customer, supplier, 0), (kind, year, 0), 1, measure);
        let weight = |line: LineitemRow| line_weight(data, db, line, customer, supplier, kind, year) * line_measure(line, measure);
        assert forall|index: int| 0 <= index < scanned.len() implies raw(#[trigger] scanned[index]) == weight(scanned[index]) by {
            assert(scanned[index] == line_input_view(rows[index]));
            lemma_line_database(data, db, scanned[index], customer, supplier, kind, year, 1, measure);
        }
        lemma_sum_ext(scanned, raw, weight);
        let filtered = |line: LineitemRow| if scan_lines_exists_1(db, line) { weight(line) } else { 0int };
        assert(sum(scanned, weight) == sum(db.lineitem, filtered));
        assert forall|index: int| 0 <= index < db.lineitem.len() implies filtered(#[trigger] db.lineitem[index]) == weight(db.lineitem[index]) by {
            if !scan_lines_exists_1(db, db.lineitem[index]) {
                lemma_excluded_line(data, db, db.lineitem[index], customer, supplier, kind, year);
            }
        }
        lemma_sum_ext(db.lineitem, filtered, weight);
    }
}

pub proof fn lemma_scan_line_domain(db: Db, rows: Seq<LineInput>)
    requires scan_lines_ok(db, rows),
    ensures line_domain(db),
{
    hide(sum);
    let scanned = line_input_seq_view(rows);
    let bad = |line: LineitemRow| if line_represented(line) { 0int } else { 1int };
    assert forall|index: int| 0 <= index < scanned.len() implies bad(#[trigger] scanned[index]) == 0 by {
        assert(scanned[index] == line_input_view(rows[index]));
    }
    lemma_sum_zero(scanned, bad);
    let filtered_bad = |line: LineitemRow| if scan_lines_exists_1(db, line) { bad(line) } else { 0int };
    assert(sum(db.lineitem, filtered_bad) == 0);
    assert forall|index: int| 0 <= index < db.lineitem.len() implies line_in_domain(db, #[trigger] db.lineitem[index]) by {
        if scan_lines_exists_1(db, db.lineitem[index]) && !line_represented(db.lineitem[index]) {
            lemma_sum_pos(db.lineitem, filtered_bad, index);
        }
    }
}

pub proof fn lemma_eligibility_backwards(db: Db, db2: Db, line: LineitemRow, changes: Seq<OrderDelta>, full: Seq<(OrdersRow, int)>, required: Keys)
    requires order_domain(db2), scan_lines_exists_1(db2, line), !required.has(line.l_orderkey as i64),
        orders_delta_ok(changes, full), bag_step(db.orders, db2.orders, full),
        forall|index: int| 0 <= index < changes.len() && ((#[trigger] changes[index]).in_1995 != 0 || changes[index].in_1996 != 0)
            ==> required.has(changes[index].o_orderkey),
    ensures scan_lines_exists_1(db, line), i64::MIN <= line.l_orderkey <= i64::MAX,
{
    hide(sum);
    let selected = choose|index: int| 0 <= index < db2.orders.len() && scan_lines_exists_1_where(line, #[trigger] db2.orders[index]);
    assert(order_represented(db2.orders[selected]));
    assert(i64::MIN <= line.l_orderkey <= i64::MAX);
    let weight = |order: OrdersRow| if scan_lines_exists_1_where(line, order) { 1int } else { 0int };
    let delta_weight = |change: (OrdersRow, int)| change.1 * weight(change.0);
    assert forall|index: int| 0 <= index < full.len() implies delta_weight(#[trigger] full[index]) == 0 by {
        assert(changes[index].o_orderkey as int == full[index].0.o_orderkey);
        if scan_lines_exists_1_where(line, full[index].0) {
            assert(changes[index].in_1995 != 0 || changes[index].in_1996 != 0);
            assert(required.has(changes[index].o_orderkey));
            assert(changes[index].o_orderkey == line.l_orderkey as i64);
        }
    }
    lemma_sum_zero(full, delta_weight);
    assert(sum(db2.orders, weight) == sum(db.orders, weight) + sum(full, delta_weight));
    lemma_sum_pos(db2.orders, weight, selected);
    let old_index = lemma_sum_nonzero_witness(db.orders, weight);
    assert(scan_lines_exists_1_where(line, db.orders[old_index]));
}

pub proof fn lemma_capture_line_domain(db: Db, db2: Db, lines: Seq<LineDelta>, orders: Seq<OrderDelta>, full: Seq<(OrdersRow, int)>,
    required: Keys, delivered: Seq<i64>, rescanned: Seq<LineInput>)
    requires line_domain(db), order_domain(db2), orders_delta_ok(orders, full), bag_step(db.orders, db2.orders, full),
        bag_step(db.lineitem, db2.lineitem, line_delta_seq_view(lines)), rescan_lines_for_orders_ok(db2, delivered, rescanned),
        forall|index: int| 0 <= index < orders.len() && ((#[trigger] orders[index]).in_1995 != 0 || orders[index].in_1996 != 0)
            ==> required.has(orders[index].o_orderkey),
        forall|key: i64| #[trigger] required.has(key) ==> delivered.contains(key),
    ensures line_domain(db2), line_domain_at(db2, db),
{
    hide(sum);
    let bad = |line: LineitemRow| if line_represented(line) { 0int } else { 1int };
    let scanned = line_input_seq_view(rescanned);
    assert forall|index: int| 0 <= index < scanned.len() implies bad(#[trigger] scanned[index]) == 0 by {
        assert(scanned[index] == line_input_view(rescanned[index]));
    }
    lemma_sum_zero(scanned, bad);
    let rescan_bad = |line: LineitemRow| if seq_view_i64(delivered).contains(line.l_orderkey) { bad(line) } else { 0int };
    assert(sum(db2.lineitem, rescan_bad) == 0);
    let outside_bad = |line: LineitemRow| if scan_lines_exists_1(db2, line) && !required.has(line.l_orderkey as i64) {
        bad(line)
    } else { 0int };
    assert forall|index: int| 0 <= index < db.lineitem.len() implies outside_bad(#[trigger] db.lineitem[index]) == 0 by {
        if scan_lines_exists_1(db2, db.lineitem[index]) && !required.has(db.lineitem[index].l_orderkey as i64) {
            lemma_eligibility_backwards(db, db2, db.lineitem[index], orders, full, required);
            assert(line_represented(db.lineitem[index]));
        }
    }
    lemma_sum_zero(db.lineitem, outside_bad);
    let deltas = line_delta_seq_view(lines);
    let old_domain = |line: LineitemRow| line_in_domain(db, line);
    assert forall|index: int| 0 <= index < deltas.len() implies old_domain((#[trigger] deltas[index]).0) by {
        assert(deltas[index].0 == line_input_view(lines[index].row));
        assert(line_represented(deltas[index].0));
    }
    verus_prooflib::lemma_bag_step_preserves_predicate(db.lineitem, db2.lineitem, deltas, old_domain);
    let delta_bad = |change: (LineitemRow, int)| change.1 * outside_bad(change.0);
    assert forall|index: int| 0 <= index < deltas.len() implies delta_bad(#[trigger] deltas[index]) == 0 by {
        assert(deltas[index].0 == line_input_view(lines[index].row));
        assert(line_represented(deltas[index].0));
    }
    lemma_sum_zero(deltas, delta_bad);
    assert(sum(db2.lineitem, outside_bad) == sum(db.lineitem, outside_bad) + sum(deltas, delta_bad));
    assert forall|index: int| 0 <= index < db2.lineitem.len() implies line_in_domain(db2, #[trigger] db2.lineitem[index]) by {
        let line = db2.lineitem[index];
        if scan_lines_exists_1(db2, line) && !line_represented(line) {
            let order_index = choose|offset: int| 0 <= offset < db2.orders.len() && scan_lines_exists_1_where(line, #[trigger] db2.orders[offset]);
            assert(order_represented(db2.orders[order_index]));
            assert(i64::MIN <= line.l_orderkey <= i64::MAX);
            if required.has(line.l_orderkey as i64) {
                assert(delivered.contains(line.l_orderkey as i64));
                let delivered_index = choose|offset: int| 0 <= offset < delivered.len() && #[trigger] delivered[offset] == line.l_orderkey as i64;
                assert(seq_view_i64(delivered)[delivered_index] == line.l_orderkey);
                assert(seq_view_i64(delivered).contains(line.l_orderkey));
                lemma_sum_pos(db2.lineitem, rescan_bad, index);
            } else {
                lemma_sum_pos(db2.lineitem, outside_bad, index);
            }
        }
    }
}

}

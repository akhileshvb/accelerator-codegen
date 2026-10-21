use vstd::prelude::*;
use verus_common::sums::*;
use verus_common::bag::*;
use crate::gen_spec::*;
use crate::gen_obligations::*;
use crate::dictionary::*;
use crate::maintenance::*;
use crate::model::*;
use crate::table::*;
use crate::contributions::*;
use crate::join_proofs::*;

verus! {

pub open spec fn key_member(keys: Keys, key: int) -> bool {
    i64::MIN <= key <= i64::MAX && keys.has(key as i64)
}

pub open spec fn selected(line: LineitemRow, orders: Keys, parts: Keys, suppliers: Keys, mode: int) -> bool {
    if mode == 0 { key_member(orders, line.l_orderkey) }
    else if mode == 1 { key_member(parts, line.l_partkey) && !key_member(orders, line.l_orderkey) }
    else { key_member(suppliers, line.l_suppkey) && !key_member(orders, line.l_orderkey) && !key_member(parts, line.l_partkey) }
}

pub open spec fn selected_key(line: LineitemRow, mode: int) -> int {
    if mode == 0 { line.l_orderkey } else if mode == 1 { line.l_partkey } else { line.l_suppkey }
}

pub open spec fn base_weight(data: Data, db: Db, line: LineitemRow, customer: i64, supplier: i64, kind: i64, year: i64, measure: bool) -> int {
    line_weight(data, db, line, customer, supplier, kind, year) * line_measure(line, measure)
}

pub proof fn lemma_rescan_sum(before: Data, after: Data, db: Db, db2: Db, orders: Keys, parts: Keys, suppliers: Keys,
    delivered: Seq<i64>, rows: Seq<LineInput>, mode: int, customer: i64, supplier: i64, kind: i64, year: i64, measure: bool)
    requires dimension_meaning(before, db), dimension_meaning(after, db2), join_ready(before), join_ready(after),
        line_domain(db2), line_domain_at(db2, db), 0 <= mode <= 2,
        if mode == 0 { rescan_lines_for_orders_ok(db2, delivered, rows) } else if mode == 1 { rescan_lines_for_parts_ok(db2, delivered, rows) }
            else { rescan_lines_for_suppliers_ok(db2, delivered, rows) },
        forall|key: i64| #[trigger] orders.has(key) && mode == 0 ==> delivered.contains(key),
        forall|key: i64| #[trigger] parts.has(key) && mode == 1 ==> delivered.contains(key),
        forall|key: i64| #[trigger] suppliers.has(key) && mode == 2 ==> delivered.contains(key),
    ensures rescan_effect(rows, orders, parts, suppliers, mode, after.parts, after.suppliers, after.customers, after.orders, before,
        (customer, supplier, 0), (kind, year, 0), measure) == sum(db2.lineitem, |line: LineitemRow|
            if selected(line, orders, parts, suppliers, mode) {
                base_weight(after, db2, line, customer, supplier, kind, year, measure) - base_weight(before, db, line, customer, supplier, kind, year, measure)
            } else { 0 }),
{
    hide(sum);
    let raw = |line: LineInput| rescan_weight(line, orders, parts, suppliers, mode, after.parts, after.suppliers, after.customers, after.orders, before,
        (customer, supplier, 0), (kind, year, 0), measure);
    let weight = |line: LineitemRow| if selected(line, orders, parts, suppliers, mode) {
        base_weight(after, db2, line, customer, supplier, kind, year, measure) - base_weight(before, db, line, customer, supplier, kind, year, measure)
    } else { 0int };
    let projected = |line: LineInput| weight(line_input_view(line));
    assert forall|index: int| 0 <= index < rows.len() implies raw(#[trigger] rows[index]) == projected(rows[index]) by {
        let line = line_input_view(rows[index]);
        assert(selected(line, orders, parts, suppliers, mode) == rescan_selected(rows[index], orders, parts, suppliers, mode));
        if rescan_selected(rows[index], orders, parts, suppliers, mode) {
            lemma_line_database(after, db2, line, customer, supplier, kind, year, 1, measure);
            lemma_line_database(before, db, line, customer, supplier, kind, year, -1, measure);
            verus_prooflib::lemma_count_weight_assoc(-1, line_weight(before, db, line, customer, supplier, kind, year), line_measure(line, measure));
        }
    }
    lemma_sum_ext(rows, raw, projected);
    lemma_sum_map(rows, |line: LineInput| line_input_view(line), weight, projected);
    let filtered = |line: LineitemRow| if seq_view_i64(delivered).contains(selected_key(line, mode)) { weight(line) } else { 0int };
    if mode == 0 {
        assert(sum(line_input_seq_view(rows), weight) == sum(db2.lineitem, |line: LineitemRow| if seq_view_i64(delivered).contains(line.l_orderkey) { weight(line) } else { 0int }));
        lemma_sum_ext_all(db2.lineitem, |line: LineitemRow| if seq_view_i64(delivered).contains(line.l_orderkey) { weight(line) } else { 0int }, filtered);
    } else if mode == 1 {
        assert(sum(line_input_seq_view(rows), weight) == sum(db2.lineitem, |line: LineitemRow| if seq_view_i64(delivered).contains(line.l_partkey) { weight(line) } else { 0int }));
        lemma_sum_ext_all(db2.lineitem, |line: LineitemRow| if seq_view_i64(delivered).contains(line.l_partkey) { weight(line) } else { 0int }, filtered);
    } else {
        assert(sum(line_input_seq_view(rows), weight) == sum(db2.lineitem, |line: LineitemRow| if seq_view_i64(delivered).contains(line.l_suppkey) { weight(line) } else { 0int }));
        lemma_sum_ext_all(db2.lineitem, |line: LineitemRow| if seq_view_i64(delivered).contains(line.l_suppkey) { weight(line) } else { 0int }, filtered);
    }
    assert forall|index: int| 0 <= index < db2.lineitem.len() implies filtered(#[trigger] db2.lineitem[index]) == weight(db2.lineitem[index]) by {
        let line = db2.lineitem[index];
        if selected(line, orders, parts, suppliers, mode) {
            if line_represented(line) {
                let key = selected_key(line, mode) as i64;
                assert(if mode == 0 { orders.has(key) } else if mode == 1 { parts.has(key) } else { suppliers.has(key) });
                assert(delivered.contains(key));
                let offset = choose|offset: int| 0 <= offset < delivered.len() && #[trigger] delivered[offset] == key;
                assert(seq_view_i64(delivered)[offset] == selected_key(line, mode));
                assert(seq_view_i64(delivered).contains(selected_key(line, mode)));
            } else {
                assert(!scan_lines_exists_1(db, line) && !scan_lines_exists_1(db2, line));
                lemma_excluded_line(before, db, line, customer, supplier, kind, year);
                lemma_excluded_line(after, db2, line, customer, supplier, kind, year);
            }
        }
    }
    lemma_sum_ext(db2.lineitem, filtered, weight);
    reveal(rescan_effect);
}

pub proof fn lemma_unaffected_weight(before: Data, after: Data, db: Db, db2: Db, line: LineitemRow,
    customer: i64, supplier: i64, kind: i64, year: i64,
    parts: Seq<PartDelta>, suppliers: Seq<SupplierDelta>, customers: Seq<CustomerDelta>, orders: Seq<OrderDelta>, full: Seq<(OrdersRow, int)>,
    required_parts: Keys, required_suppliers: Keys, required_orders: Keys)
    requires dimension_meaning(before, db), dimension_meaning(after, db2), runtime_valid(before), extension(before.dictionary, after.dictionary),
        line_represented(line), !required_parts.has(line.l_partkey as i64), !required_suppliers.has(line.l_suppkey as i64), !required_orders.has(line.l_orderkey as i64),
        bag_step(db.part, db2.part, part_delta_seq_view(parts)), bag_step(db.supplier, db2.supplier, supplier_delta_seq_view(suppliers)),
        bag_step(db.customer, db2.customer, customer_delta_seq_view(customers)), bag_step(db.orders, db2.orders, full), orders_delta_ok(orders, full),
        forall|index: int| 0 <= index < parts.len() ==> required_parts.has((#[trigger] parts[index]).row.p_partkey),
        forall|index: int| 0 <= index < suppliers.len() ==> required_suppliers.has((#[trigger] suppliers[index]).row.s_suppkey),
        forall|index: int| 0 <= index < orders.len() && ((#[trigger] orders[index]).in_1995 != 0 || orders[index].in_1996 != 0) ==> required_orders.has(orders[index].o_orderkey),
        forall|index: int, offset: int| 0 <= index < customers.len() && 0 <= offset < before.orders_by_customer.rows((customers[index].row.c_custkey, 0, 0)).len()
            ==> required_orders.has((#[trigger] before.orders_by_customer.rows((customers[index].row.c_custkey, 0, 0))[offset]).first),
    ensures line_weight(before, db, line, customer, supplier, kind, year) == line_weight(after, db2, line, customer, supplier, kind, year),
{
    hide(sum);
    lemma_runtime_access(before);
    reveal(dimension_meaning);
    let old_part = |row: PartRow| if row.p_partkey == line.l_partkey && text_matches(before.dictionary, 0, kind, row.p_type) { 1int } else { 0int };
    let new_part = |row: PartRow| if row.p_partkey == line.l_partkey && text_matches(after.dictionary, 0, kind, row.p_type) { 1int } else { 0int };
    assert forall|index: int| 0 <= index < db.part.len() implies old_part(#[trigger] db.part[index]) == new_part(db.part[index]) by {
        lemma_known_text_extension(before.dictionary, after.dictionary, 0, db.part[index].p_type);
    }
    lemma_sum_ext(db.part, old_part, new_part);
    let part_changes = part_delta_seq_view(parts);
    let part_delta_weight = |change: (PartRow, int)| change.1 * new_part(change.0);
    assert forall|index: int| 0 <= index < part_changes.len() implies part_delta_weight(#[trigger] part_changes[index]) == 0 by {
        assert(part_changes[index].0 == part_input_view(parts[index].row));
        assert(required_parts.has(parts[index].row.p_partkey));
        assert(parts[index].row.p_partkey as int != line.l_partkey);
    }
    lemma_sum_zero(part_changes, part_delta_weight);
    assert(sum(db2.part, new_part) == sum(db.part, new_part) + sum(part_changes, part_delta_weight));
    assert(before.parts.at((line.l_partkey as i64, 0, 0), (kind, 0, 0), false)
        == after.parts.at((line.l_partkey as i64, 0, 0), (kind, 0, 0), false));
    let supplier_weight = |row: SupplierRow| if row.s_suppkey == line.l_suppkey && row.s_nationkey == supplier as int { 1int } else { 0int };
    let supplier_changes = supplier_delta_seq_view(suppliers);
    let supplier_delta_weight = |change: (SupplierRow, int)| change.1 * supplier_weight(change.0);
    assert forall|index: int| 0 <= index < supplier_changes.len() implies supplier_delta_weight(#[trigger] supplier_changes[index]) == 0 by {
        assert(supplier_changes[index].0 == supplier_input_view(suppliers[index].row));
        assert(required_suppliers.has(suppliers[index].row.s_suppkey));
        assert(suppliers[index].row.s_suppkey as int != line.l_suppkey);
    }
    lemma_sum_zero(supplier_changes, supplier_delta_weight);
    assert(sum(db2.supplier, supplier_weight) == sum(db.supplier, supplier_weight) + sum(supplier_changes, supplier_delta_weight));
    assert(before.suppliers.at((line.l_suppkey as i64, 0, 0), (supplier, 0, 0), false)
        == after.suppliers.at((line.l_suppkey as i64, 0, 0), (supplier, 0, 0), false));
    let old_order = |order: OrdersRow| if order.o_orderkey == line.l_orderkey && year_of(order.o_orderdate) == year as int && year != 0 {
        before.customers.at((order.o_custkey as i64, 0, 0), (customer, 0, 0), false)
    } else { 0int };
    let new_order = |order: OrdersRow| if order.o_orderkey == line.l_orderkey && year_of(order.o_orderdate) == year as int && year != 0 {
        after.customers.at((order.o_custkey as i64, 0, 0), (customer, 0, 0), false)
    } else { 0int };
    let order_delta_weight = |change: (OrdersRow, int)| change.1 * new_order(change.0);
    assert forall|index: int| 0 <= index < full.len() implies order_delta_weight(#[trigger] full[index]) == 0 by {
        if full[index].0.o_orderkey == line.l_orderkey && year_of(full[index].0.o_orderdate) != 0 {
            assert(orders[index].in_1995 != 0 || orders[index].in_1996 != 0);
            assert(orders[index].o_orderkey as int == full[index].0.o_orderkey);
            assert(required_orders.has(orders[index].o_orderkey));
        }
    }
    lemma_sum_zero(full, order_delta_weight);
    assert(sum(db2.orders, new_order) == sum(db.orders, new_order) + sum(full, order_delta_weight));
    assert forall|order_index: int| 0 <= order_index < db.orders.len() implies old_order(#[trigger] db.orders[order_index]) == new_order(db.orders[order_index]) by {
        let order = db.orders[order_index];
        if order.o_orderkey == line.l_orderkey && year_of(order.o_orderdate) == year as int && year != 0 {
            assert(order_represented(order));
            let customer_key = order.o_custkey as i64;
            assert forall|index: int| 0 <= index < customers.len() implies (#[trigger] customers[index]).row.c_custkey != customer_key by {
                if customers[index].row.c_custkey == customer_key {
                    let reverse_weight = |row: OrdersRow| if row.o_orderkey == line.l_orderkey && row.o_custkey == customer_key as int && year_of(row.o_orderdate) != 0 { 1int } else { 0int };
                    lemma_sum_pos(db.orders, reverse_weight, order_index);
                    let order_key = line.l_orderkey as i64;
                    assert(before.orders_by_customer.at((customer_key, 0, 0), (order_key, 0, 0), false) == sum(db.orders, reverse_weight));
                    let reverse_rows = before.orders_by_customer.rows((customer_key, 0, 0));
                    reveal(Table::at);
                    reveal(cell_sum);
                    let reverse_cell_weight = |cell: Cell| cell_weight(cell, (order_key, 0, 0), false);
                    let offset = lemma_sum_nonzero_witness(reverse_rows, reverse_cell_weight);
                    assert(reverse_rows[offset].first == order_key);
                    assert(required_orders.has(reverse_rows[offset].first));
                }
            }
            let customer_weight = |row: CustomerRow| if row.c_custkey == customer_key as int && row.c_nationkey == customer as int { 1int } else { 0int };
            let customer_changes = customer_delta_seq_view(customers);
            let customer_delta_weight = |change: (CustomerRow, int)| change.1 * customer_weight(change.0);
            assert forall|index: int| 0 <= index < customer_changes.len() implies customer_delta_weight(#[trigger] customer_changes[index]) == 0 by {
                assert(customer_changes[index].0 == customer_input_view(customers[index].row));
                assert(customers[index].row.c_custkey != customer_key);
            }
            lemma_sum_zero(customer_changes, customer_delta_weight);
            assert(sum(db2.customer, customer_weight) == sum(db.customer, customer_weight) + sum(customer_changes, customer_delta_weight));
            assert(before.customers.at((customer_key, 0, 0), (customer, 0, 0), false) == after.customers.at((customer_key, 0, 0), (customer, 0, 0), false));
        }
    }
    lemma_sum_ext(db.orders, old_order, new_order);
    reveal(line_weight);
}

pub proof fn lemma_apply_base(before: Data, after: Data, db: Db, db2: Db, lines: Seq<LineDelta>,
    orders: Keys, parts: Keys, suppliers: Keys,
    order_keys: Seq<i64>, order_rows: Seq<LineInput>, part_keys: Seq<i64>, part_rows: Seq<LineInput>, supplier_keys: Seq<i64>, supplier_rows: Seq<LineInput>)
    requires dimension_meaning(before, db), dimension_meaning(after, db2), join_ready(before), join_ready(after), base_meaning(before, db),
        line_domain(db2), line_domain_at(db2, db), bag_step(db.lineitem, db2.lineitem, line_delta_seq_view(lines)),
        rescan_lines_for_orders_ok(db2, order_keys, order_rows), rescan_lines_for_parts_ok(db2, part_keys, part_rows), rescan_lines_for_suppliers_ok(db2, supplier_keys, supplier_rows),
        forall|key: i64| #[trigger] orders.has(key) ==> order_keys.contains(key),
        forall|key: i64| #[trigger] parts.has(key) ==> part_keys.contains(key),
        forall|key: i64| #[trigger] suppliers.has(key) ==> supplier_keys.contains(key),
        forall|index: int, customer: i64, supplier: i64, kind: i64, year: i64|
            0 <= index < db2.lineitem.len() && !key_member(orders, db2.lineitem[index].l_orderkey)
                && !key_member(parts, db2.lineitem[index].l_partkey) && !key_member(suppliers, db2.lineitem[index].l_suppkey)
            ==> #[trigger] line_weight(before, db, db2.lineitem[index], customer, supplier, kind, year)
                == line_weight(after, db2, db2.lineitem[index], customer, supplier, kind, year),
        forall|customer: i64, supplier: i64, kind: i64, year: i64, measure: bool|
            #[trigger] after.base.at((customer, supplier, 0), (kind, year, 0), measure) == before.base.at((customer, supplier, 0), (kind, year, 0), measure)
                + sum(line_delta_seq_view(lines), |change: (LineitemRow, int)| line_effect(before.parts, before.suppliers, before.customers, before.orders,
                    change.0, (customer, supplier, 0), (kind, year, 0), change.1, measure))
                + rescan_effect(order_rows, orders, parts, suppliers, 0, after.parts, after.suppliers, after.customers, after.orders, before, (customer, supplier, 0), (kind, year, 0), measure)
                + rescan_effect(part_rows, orders, parts, suppliers, 1, after.parts, after.suppliers, after.customers, after.orders, before, (customer, supplier, 0), (kind, year, 0), measure)
                + rescan_effect(supplier_rows, orders, parts, suppliers, 2, after.parts, after.suppliers, after.customers, after.orders, before, (customer, supplier, 0), (kind, year, 0), measure),
    ensures base_meaning(after, db2),
{
    hide(sum);
    reveal(base_meaning);
    assert forall|customer: i64, supplier: i64, kind: i64, year: i64, measure: bool|
        #[trigger] after.base.at((customer, supplier, 0), (kind, year, 0), measure) ==
            sum(db2.lineitem, |line: LineitemRow| line_weight(after, db2, line, customer, supplier, kind, year) * line_measure(line, measure)) by {
        let old_weight = |line: LineitemRow| base_weight(before, db, line, customer, supplier, kind, year, measure);
        let new_weight = |line: LineitemRow| base_weight(after, db2, line, customer, supplier, kind, year, measure);
        let difference = |line: LineitemRow| new_weight(line) - old_weight(line);
        let order_weight = |line: LineitemRow| if selected(line, orders, parts, suppliers, 0) { difference(line) } else { 0int };
        let part_weight = |line: LineitemRow| if selected(line, orders, parts, suppliers, 1) { difference(line) } else { 0int };
        let supplier_weight = |line: LineitemRow| if selected(line, orders, parts, suppliers, 2) { difference(line) } else { 0int };
        lemma_rescan_sum(before, after, db, db2, orders, parts, suppliers, order_keys, order_rows, 0, customer, supplier, kind, year, measure);
        lemma_rescan_sum(before, after, db, db2, orders, parts, suppliers, part_keys, part_rows, 1, customer, supplier, kind, year, measure);
        lemma_rescan_sum(before, after, db, db2, orders, parts, suppliers, supplier_keys, supplier_rows, 2, customer, supplier, kind, year, measure);
        lemma_sum_ext_all(db2.lineitem, |line: LineitemRow| if selected(line, orders, parts, suppliers, 0) {
            base_weight(after, db2, line, customer, supplier, kind, year, measure) - base_weight(before, db, line, customer, supplier, kind, year, measure)
        } else { 0int }, order_weight);
        lemma_sum_ext_all(db2.lineitem, |line: LineitemRow| if selected(line, orders, parts, suppliers, 1) {
            base_weight(after, db2, line, customer, supplier, kind, year, measure) - base_weight(before, db, line, customer, supplier, kind, year, measure)
        } else { 0int }, part_weight);
        lemma_sum_ext_all(db2.lineitem, |line: LineitemRow| if selected(line, orders, parts, suppliers, 2) {
            base_weight(after, db2, line, customer, supplier, kind, year, measure) - base_weight(before, db, line, customer, supplier, kind, year, measure)
        } else { 0int }, supplier_weight);
        assert(rescan_effect(order_rows, orders, parts, suppliers, 0, after.parts, after.suppliers, after.customers, after.orders, before,
            (customer, supplier, 0), (kind, year, 0), measure) == sum(db2.lineitem, order_weight));
        assert(rescan_effect(part_rows, orders, parts, suppliers, 1, after.parts, after.suppliers, after.customers, after.orders, before,
            (customer, supplier, 0), (kind, year, 0), measure) == sum(db2.lineitem, part_weight));
        assert(rescan_effect(supplier_rows, orders, parts, suppliers, 2, after.parts, after.suppliers, after.customers, after.orders, before,
            (customer, supplier, 0), (kind, year, 0), measure) == sum(db2.lineitem, supplier_weight));
        let first = |line: LineitemRow| order_weight(line) + part_weight(line);
        let all = |line: LineitemRow| first(line) + supplier_weight(line);
        lemma_sum_add(db2.lineitem, order_weight, part_weight, first);
        lemma_sum_add(db2.lineitem, first, supplier_weight, all);
        assert forall|index: int| 0 <= index < db2.lineitem.len() implies all(#[trigger] db2.lineitem[index]) == difference(db2.lineitem[index]) by {
            let line = db2.lineitem[index];
            if !key_member(orders, line.l_orderkey) && !key_member(parts, line.l_partkey) && !key_member(suppliers, line.l_suppkey) {
                assert(line_weight(before, db, line, customer, supplier, kind, year) == line_weight(after, db2, line, customer, supplier, kind, year));
            }
        }
        lemma_sum_ext(db2.lineitem, all, difference);
        lemma_sum_sub(db2.lineitem, new_weight, old_weight, difference);
        let deltas = line_delta_seq_view(lines);
        let raw_delta = |change: (LineitemRow, int)| line_effect(before.parts, before.suppliers, before.customers, before.orders,
            change.0, (customer, supplier, 0), (kind, year, 0), change.1, measure);
        let signed_delta = |change: (LineitemRow, int)| change.1 * old_weight(change.0);
        assert forall|index: int| 0 <= index < deltas.len() implies raw_delta(#[trigger] deltas[index]) == signed_delta(deltas[index]) by {
            assert(deltas[index].0 == line_input_view(lines[index].row));
            lemma_line_database(before, db, deltas[index].0, customer, supplier, kind, year, deltas[index].1, measure);
            verus_prooflib::lemma_count_weight_assoc(deltas[index].1, line_weight(before, db, deltas[index].0, customer, supplier, kind, year), line_measure(deltas[index].0, measure));
        }
        lemma_sum_ext(deltas, raw_delta, signed_delta);
        assert(sum(db2.lineitem, old_weight) == sum(db.lineitem, old_weight) + sum(deltas, signed_delta));
        lemma_sum_ext_all(db.lineitem, |line: LineitemRow| line_weight(before, db, line, customer, supplier, kind, year) * line_measure(line, measure), old_weight);
        lemma_sum_ext_all(db2.lineitem, new_weight, |line: LineitemRow| line_weight(after, db2, line, customer, supplier, kind, year) * line_measure(line, measure));
        assert(before.base.at((customer, supplier, 0), (kind, year, 0), measure) == sum(db.lineitem, old_weight));
        assert(after.base.at((customer, supplier, 0), (kind, year, 0), measure) == before.base.at((customer, supplier, 0), (kind, year, 0), measure)
            + sum(deltas, signed_delta) + sum(db2.lineitem, order_weight) + sum(db2.lineitem, part_weight) + sum(db2.lineitem, supplier_weight));
        assert(sum(db2.lineitem, difference) == sum(db2.lineitem, order_weight) + sum(db2.lineitem, part_weight) + sum(db2.lineitem, supplier_weight));
        assert(after.base.at((customer, supplier, 0), (kind, year, 0), measure) == sum(db2.lineitem, new_weight));
    }
}

}

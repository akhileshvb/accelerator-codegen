use vstd::prelude::*;
use verus_common::sums::*;
use crate::contributions::*;
use crate::dictionary::*;
use crate::gen_obligations::*;
use crate::gen_spec::*;
use crate::join_proofs::*;
use crate::model::*;
use crate::table::*;

verus! {

pub open spec fn base_support(key: Major, minor: Major, dictionary: Dictionary) -> bool {
    key.2 == 0 && minor.2 == 0 && dictionary.types.known(minor.0) && (minor.1 == 1995 || minor.1 == 1996)
}

pub open spec fn order_years(orders: Table) -> bool {
    forall|key: Major, index: int| 0 <= index < orders.rows(key).len() ==>
        (#[trigger] orders.rows(key)[index]).second == 1995 || orders.rows(key)[index].second == 1996
}

pub proof fn lemma_table_count_nonneg(table: Table, key: Major, minor: Major)
    requires positive(table),
    ensures table.at(key, minor, false) >= 0,
{
    hide(sum);
    reveal(Table::at);
    reveal(cell_sum);
    assert forall|index: int| 0 <= index < table.rows(key).len() implies
        cell_weight(#[trigger] table.rows(key)[index], minor, false) >= 0 by {
        assert(table.rows(key)[index].count@ > 0);
    }
    lemma_sum_nonneg(table.rows(key), |cell: Cell| cell_weight(cell, minor, false));
}

pub proof fn lemma_line_nonneg(data: Data, db: Db, line: LineitemRow, customer: i64, supplier: i64, kind: i64, year: i64)
    requires positive(data.parts), positive(data.suppliers), positive(data.customers),
    ensures line_weight(data, db, line, customer, supplier, kind, year) >= 0,
{
    hide(sum);
    reveal(line_weight);
    lemma_table_count_nonneg(data.parts, (line.l_partkey as i64, 0, 0), (kind, 0, 0));
    lemma_table_count_nonneg(data.suppliers, (line.l_suppkey as i64, 0, 0), (supplier, 0, 0));
    let weight = |order: OrdersRow| if order.o_orderkey == line.l_orderkey && year_of(order.o_orderdate) == year as int && year != 0 {
        data.customers.at((order.o_custkey as i64, 0, 0), (customer, 0, 0), false)
    } else { 0int };
    assert forall|index: int| 0 <= index < db.orders.len() implies weight(#[trigger] db.orders[index]) >= 0 by {
        lemma_table_count_nonneg(data.customers, (db.orders[index].o_custkey as i64, 0, 0), (customer, 0, 0));
    }
    lemma_sum_nonneg(db.orders, weight);
    let part_count = data.parts.at((line.l_partkey as i64, 0, 0), (kind, 0, 0), false);
    let supplier_count = data.suppliers.at((line.l_suppkey as i64, 0, 0), (supplier, 0, 0), false);
    let order_count = sum(db.orders, weight);
    assert(part_count * supplier_count * order_count >= 0) by (nonlinear_arith)
        requires part_count >= 0, supplier_count >= 0, order_count >= 0;
}

pub proof fn lemma_base_count(data: Data, db: Db, customer: i64, supplier: i64, kind: i64, year: i64)
    requires base_meaning(data, db), positive(data.parts), positive(data.suppliers), positive(data.customers),
    ensures data.base.at((customer, supplier, 0), (kind, year, 0), false) >= 0,
        data.base.at((customer, supplier, 0), (kind, year, 0), false) == 0 ==>
            data.base.at((customer, supplier, 0), (kind, year, 0), true) == 0,
{
    hide(sum);
    reveal(base_meaning);
    let count_weight = |line: LineitemRow| line_weight(data, db, line, customer, supplier, kind, year);
    assert forall|index: int| 0 <= index < db.lineitem.len() implies count_weight(#[trigger] db.lineitem[index]) >= 0 by {
        lemma_line_nonneg(data, db, db.lineitem[index], customer, supplier, kind, year);
    }
    lemma_sum_nonneg(db.lineitem, count_weight);
    lemma_sum_ext_all(db.lineitem, |line: LineitemRow| line_weight(data, db, line, customer, supplier, kind, year) * line_measure(line, false), count_weight);
    assert(data.base.at((customer, supplier, 0), (kind, year, 0), false) == sum(db.lineitem, count_weight));
    if data.base.at((customer, supplier, 0), (kind, year, 0), false) == 0 {
        let volume_weight = |line: LineitemRow| line_weight(data, db, line, customer, supplier, kind, year) * line_measure(line, true);
        assert forall|index: int| 0 <= index < db.lineitem.len() implies volume_weight(#[trigger] db.lineitem[index]) == 0 by {
            lemma_sum_ge_term(db.lineitem, count_weight, index);
            assert(count_weight(db.lineitem[index]) == 0);
        }
        lemma_sum_zero(db.lineitem, volume_weight);
    }
}

pub proof fn lemma_line_support(parts: Table, suppliers: Table, customers: Table, orders: Table, dictionary: Dictionary,
    line: LineitemRow, key: Major, minor: Major, sign: int, measure: bool)
    requires scalar(parts), scalar(suppliers), scalar(customers), text_domain(parts, dictionary, 0), order_years(orders),
        i64::MIN <= line.l_partkey <= i64::MAX, i64::MIN <= line.l_suppkey <= i64::MAX, i64::MIN <= line.l_orderkey <= i64::MAX,
        !base_support(key, minor, dictionary),
    ensures line_effect(parts, suppliers, customers, orders, line, key, minor, sign, measure) == 0,
{
    hide(sum);
    lemma_line_factor(parts, suppliers, customers, orders, line, key, minor, sign, measure);
    let part_count = parts.at((line.l_partkey as i64, 0, 0), (minor.0, 0, 0), false);
    let supplier_count = suppliers.at((line.l_suppkey as i64, 0, 0), (key.1, 0, 0), false);
    let order_count = order_projection(orders.rows((line.l_orderkey as i64, 0, 0)), customers, key.0, minor.1);
    if key.2 == 0 && minor.2 == 0 {
        if !(dictionary.types.known(minor.0)) {
            let rows = parts.rows((line.l_partkey as i64, 0, 0));
            assert forall|index: int| 0 <= index < rows.len() implies cell_weight(#[trigger] rows[index], (minor.0, 0, 0), false) == 0 by {
                assert(dictionary.types.known(rows[index].first));
            }
            lemma_sum_zero(rows, |cell: Cell| cell_weight(cell, (minor.0, 0, 0), false));
            reveal(Table::at);
            reveal(cell_sum);
            assert(parts.at((line.l_partkey as i64, 0, 0), (minor.0, 0, 0), false) == 0);
            assert(part_count * supplier_count * order_count == 0) by (nonlinear_arith)
                requires part_count == 0;
        } else {
            let rows = orders.rows((line.l_orderkey as i64, 0, 0));
            lemma_order_year_zero(rows, customers, key.0, minor.1);
            assert(part_count * supplier_count * order_count == 0) by (nonlinear_arith)
                requires order_count == 0;
        }
    }
    assert(projected_line_weight(parts, suppliers, customers, orders, line, key, minor) == 0);
    let projected = projected_line_weight(parts, suppliers, customers, orders, line, key, minor);
    let value = line_measure(line, measure);
    assert(sign * projected * value == 0) by (nonlinear_arith)
        requires projected == 0;
}

pub proof fn lemma_order_year_zero(rows: Seq<Cell>, customers: Table, customer: i64, year: i64)
    requires year != 1995, year != 1996,
        forall|index: int| 0 <= index < rows.len() ==> (#[trigger] rows[index]).second == 1995 || rows[index].second == 1996,
    ensures order_projection(rows, customers, customer, year) == 0,
{
    hide(sum);
    reveal(order_projection);
    lemma_sum_zero(rows, |order: Cell| if order.second == year {
        order.count@ * customers.at((order.first, 0, 0), (customer, 0, 0), false)
    } else { 0int });
}

pub proof fn lemma_build_base_cells(data: Data, db: Db, rows: Seq<LineInput>)
    requires base_meaning(data, db), data.base.valid(), scalar(data.parts), scalar(data.suppliers), scalar(data.customers),
        positive(data.parts), positive(data.suppliers), positive(data.customers), text_domain(data.parts, data.dictionary, 0), order_years(data.orders),
        forall|key: Major, minor: Major, measure: bool| #[trigger] data.base.at(key, minor, measure) ==
            sum(line_input_seq_view(rows), |line: LineitemRow| line_effect(data.parts, data.suppliers, data.customers, data.orders, line, key, minor, 1, measure)),
    ensures positive(data.base), text_domain(data.base, data.dictionary, 0),
        forall|key: Major, index: int| 0 <= index < data.base.rows(key).len() ==>
            base_support(key, minor_of(#[trigger] data.base.rows(key)[index]), data.dictionary),
{
    hide(sum);
    let scanned = line_input_seq_view(rows);
    assert forall|key: Major, minor: Major, measure: bool| !base_support(key, minor, data.dictionary)
        implies #[trigger] data.base.at(key, minor, measure) == 0 by {
        let weight = |line: LineitemRow| line_effect(data.parts, data.suppliers, data.customers, data.orders, line, key, minor, 1, measure);
        assert forall|index: int| 0 <= index < scanned.len() implies weight(#[trigger] scanned[index]) == 0 by {
            assert(scanned[index] == line_input_view(rows[index]));
            lemma_line_support(data.parts, data.suppliers, data.customers, data.orders, data.dictionary, scanned[index], key, minor, 1, measure);
        }
        lemma_sum_zero(scanned, weight);
    }
    lemma_base_cells(data, db);
}

pub proof fn lemma_base_cells(data: Data, db: Db)
    requires data.base.valid(), base_meaning(data, db), positive(data.parts), positive(data.suppliers), positive(data.customers),
        forall|key: Major, minor: Major, measure: bool| !base_support(key, minor, data.dictionary) ==> #[trigger] data.base.at(key, minor, measure) == 0,
    ensures positive(data.base), text_domain(data.base, data.dictionary, 0),
        forall|key: Major, index: int| 0 <= index < data.base.rows(key).len() ==>
            base_support(key, minor_of(#[trigger] data.base.rows(key)[index]), data.dictionary),
{
    hide(sum);
    assert forall|key: Major, index: int| 0 <= index < data.base.rows(key).len() implies
        base_support(key, minor_of(#[trigger] data.base.rows(key)[index]), data.dictionary)
            && data.base.rows(key)[index].count@ > 0 by {
        let cells = data.base.rows(key);
        let cell = cells[index];
        let minor = minor_of(cell);
        reveal(rows_valid);
        assert(rows_valid(cells));
        lemma_cell_lookup(cells, minor, index);
        reveal(Table::at);
        assert(data.base.at(key, minor, false) == cell.count@);
        assert(data.base.at(key, minor, true) == cell.volume@);
        if !base_support(key, minor, data.dictionary) {
            assert(cell.count@ == 0 && cell.volume@ == 0);
        }
        lemma_base_count(data, db, key.0, key.1, minor.0, minor.1);
        assert(cell.count@ >= 0);
        if cell.count@ == 0 { assert(cell.volume@ == 0); }
    }
}

pub open spec fn base_inputs_ready(data: Data) -> bool {
    scalar(data.parts) && scalar(data.suppliers) && scalar(data.customers)
        && text_domain(data.parts, data.dictionary, 0) && order_years(data.orders)
}

pub proof fn lemma_rescan_support(before: Data, data: Data, rows: Seq<LineInput>,
    required_orders: crate::maintenance::Keys, required_parts: crate::maintenance::Keys, required_suppliers: crate::maintenance::Keys,
    mode: int, key: Major, minor: Major, measure: bool)
    requires base_inputs_ready(before), base_inputs_ready(data), extension(before.dictionary, data.dictionary),
        !base_support(key, minor, data.dictionary),
    ensures rescan_effect(rows, required_orders, required_parts, required_suppliers, mode,
        data.parts, data.suppliers, data.customers, data.orders, before, key, minor, measure) == 0,
{
    hide(sum);
    reveal(rescan_effect);
    let weight = |line: LineInput| rescan_weight(line, required_orders, required_parts, required_suppliers, mode,
        data.parts, data.suppliers, data.customers, data.orders, before, key, minor, measure);
    assert forall|index: int| 0 <= index < rows.len() implies weight(#[trigger] rows[index]) == 0 by {
        let line = line_input_view(rows[index]);
        lemma_line_support(data.parts, data.suppliers, data.customers, data.orders, data.dictionary, line, key, minor, 1, measure);
        lemma_line_support(before.parts, before.suppliers, before.customers, before.orders, before.dictionary, line, key, minor, -1, measure);
    }
    lemma_sum_zero(rows, weight);
}

pub proof fn lemma_delta_base_support(before: Data, data: Data, changes: Table, lines: Seq<LineDelta>,
    required_orders: crate::maintenance::Keys, required_parts: crate::maintenance::Keys, required_suppliers: crate::maintenance::Keys,
    order_rows: Seq<LineInput>, part_rows: Seq<LineInput>, supplier_rows: Seq<LineInput>)
    requires base_inputs_ready(before), base_inputs_ready(data), extension(before.dictionary, data.dictionary),
        changes.valid(),
        forall|key: Major, minor: Major, measure: bool| #[trigger] changes.at(key, minor, measure) ==
            sum(line_delta_seq_view(lines), |change: (LineitemRow, int)| line_effect(before.parts, before.suppliers, before.customers, before.orders, change.0, key, minor, change.1, measure))
            + rescan_effect(order_rows, required_orders, required_parts, required_suppliers, 0, data.parts, data.suppliers, data.customers, data.orders, before, key, minor, measure)
            + rescan_effect(part_rows, required_orders, required_parts, required_suppliers, 1, data.parts, data.suppliers, data.customers, data.orders, before, key, minor, measure)
            + rescan_effect(supplier_rows, required_orders, required_parts, required_suppliers, 2, data.parts, data.suppliers, data.customers, data.orders, before, key, minor, measure),
    ensures forall|key: Major, minor: Major, measure: bool| !base_support(key, minor, data.dictionary) ==> #[trigger] changes.at(key, minor, measure) == 0,
        crate::index_proofs::base_key_shape(changes),
{
    hide(sum);
    assert forall|key: Major, minor: Major, measure: bool| !base_support(key, minor, data.dictionary) implies #[trigger] changes.at(key, minor, measure) == 0 by {
        let weight = |change: (LineitemRow, int)| line_effect(before.parts, before.suppliers, before.customers, before.orders, change.0, key, minor, change.1, measure);
        let projected = line_delta_seq_view(lines);
        assert forall|index: int| 0 <= index < projected.len() implies weight(#[trigger] projected[index]) == 0 by {
            assert(projected[index] == (line_input_view(lines[index].row), lines[index].sign as int));
            let change = projected[index];
            lemma_line_support(before.parts, before.suppliers, before.customers, before.orders, before.dictionary, change.0, key, minor, change.1, measure);
        }
        lemma_sum_zero(projected, weight);
        lemma_rescan_support(before, data, order_rows, required_orders, required_parts, required_suppliers, 0, key, minor, measure);
        lemma_rescan_support(before, data, part_rows, required_orders, required_parts, required_suppliers, 1, key, minor, measure);
        lemma_rescan_support(before, data, supplier_rows, required_orders, required_parts, required_suppliers, 2, key, minor, measure);
    }
    assert forall|key: Major, index: int| 0 <= index < changes.rows(key).len() implies
        key.2 == 0 && (#[trigger] changes.rows(key)[index]).third == 0 by {
        let cells = changes.rows(key);
        let minor = minor_of(cells[index]);
        reveal(rows_valid);
        lemma_cell_lookup(cells, minor, index);
        reveal(Table::at);
        assert(changes.at(key, minor, false) == cells[index].count@);
        assert(changes.at(key, minor, true) == cells[index].volume@);
        if key.2 != 0 || minor.2 != 0 {
            assert(cells[index].count@ == 0 && cells[index].volume@ == 0);
        }
    }
}

pub proof fn lemma_apply_base_cells(before: Data, data: Data, db: Db, changes: Table)
    requires runtime_valid(before), data.base.valid(), base_meaning(data, db), extension(before.dictionary, data.dictionary),
        positive(data.parts), positive(data.suppliers), positive(data.customers),
        forall|key: Major, minor: Major, measure: bool| #[trigger] data.base.at(key, minor, measure) == before.base.at(key, minor, measure) + changes.at(key, minor, measure),
        forall|key: Major, minor: Major, measure: bool| !base_support(key, minor, data.dictionary) ==> #[trigger] changes.at(key, minor, measure) == 0,
    ensures positive(data.base), text_domain(data.base, data.dictionary, 0),
        forall|key: Major, index: int| 0 <= index < data.base.rows(key).len() ==>
            base_support(key, minor_of(#[trigger] data.base.rows(key)[index]), data.dictionary),
{
    hide(sum);
    lemma_runtime_access(before);
    assert forall|key: Major, minor: Major, measure: bool| !base_support(key, minor, data.dictionary) implies #[trigger] data.base.at(key, minor, measure) == 0 by {
        let rows = before.base.rows(key);
        assert forall|index: int| 0 <= index < rows.len() implies cell_weight(#[trigger] rows[index], minor, measure) == 0 by {
            assert(base_support(key, minor_of(rows[index]), before.dictionary));
        }
        reveal(Table::at);
        reveal(cell_sum);
        lemma_sum_zero(rows, |cell: Cell| cell_weight(cell, minor, measure));
    }
    lemma_base_cells(data, db);
}

}

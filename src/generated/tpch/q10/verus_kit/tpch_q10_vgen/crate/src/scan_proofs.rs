use vstd::prelude::*;
use verus_common::sums::*;
use crate::gen_spec::*;
use crate::gen_obligations::*;
use crate::maintenance::*;
use crate::model::*;
use crate::calendar::*;
use crate::facts::*;
use crate::calendar_proofs::lemma_period_bounds;
use verus_prooflib::ordered::Index;
use crate::values::{Moments, moment_at};

verus! {

pub open spec fn projected_weight(key: int, component: int) -> spec_fn((int, int, int)) -> int {
    |row: (int, int, int)| if row.0 != key { 0 }
        else if component == 0 { 1 }
        else {
            let revenue = row.1 * (100 - row.2);
            if component == 1 { revenue } else if revenue > 0 { revenue } else { 0 }
        }
}

pub proof fn lemma_scanned_lines(db: Db, rows: Seq<ReturnedLineInput>, key: int, component: int)
    requires scan_returned_lines_ok(db, rows),
    ensures scanned_line_sum(rows, key, component)
        == sum(db.lineitem, |line: LineitemRow| if line.l_orderkey == key { line_weight(line, component) } else { 0 }),
{
    let weight = projected_weight(key, component);
    lemma_sum_map(rows, |row: ReturnedLineInput| returned_line_input_view(row), weight,
        |row: ReturnedLineInput| if row.l_orderkey == key { projected_line_weight(row, component) } else { 0 });
    assert(sum(returned_line_input_seq_view(rows), weight) == sum(db.lineitem,
        |line: LineitemRow| if line.l_returnflag == seq![b'R'] { weight((line.l_orderkey, line.l_extendedprice, line.l_discount)) } else { 0 }));
    lemma_sum_ext_all(db.lineitem,
        |line: LineitemRow| if line.l_returnflag == seq![b'R'] { weight((line.l_orderkey, line.l_extendedprice, line.l_discount)) } else { 0 },
        |line: LineitemRow| if line.l_orderkey == key { line_weight(line, component) } else { 0 });
}

pub proof fn lemma_rescanned_lines(db: Db, keys: Seq<i64>, rows: Seq<ReturnedLineInput>, key: int, component: int)
    requires rescan_returned_lines_for_keys_ok(db, keys, rows), seq_view_i64(keys).contains(key),
    ensures scanned_line_sum(rows, key, component)
        == sum(db.lineitem, |line: LineitemRow| if line.l_orderkey == key { line_weight(line, component) } else { 0 }),
{
    let weight = projected_weight(key, component);
    lemma_sum_map(rows, |row: ReturnedLineInput| returned_line_input_view(row), weight,
        |row: ReturnedLineInput| if row.l_orderkey == key { projected_line_weight(row, component) } else { 0 });
    assert(sum(returned_line_input_seq_view(rows), weight) == sum(db.lineitem,
        |line: LineitemRow| if line.l_returnflag == seq![b'R'] && seq_view_i64(keys).contains(line.l_orderkey) {
            weight((line.l_orderkey, line.l_extendedprice, line.l_discount)) } else { 0 }));
    lemma_sum_ext_all(db.lineitem,
        |line: LineitemRow| if line.l_returnflag == seq![b'R'] && seq_view_i64(keys).contains(line.l_orderkey) {
            weight((line.l_orderkey, line.l_extendedprice, line.l_discount)) } else { 0 },
        |line: LineitemRow| if line.l_orderkey == key { line_weight(line, component) } else { 0 });
}

pub proof fn lemma_rescanned_orders(db: Db, keys: Seq<i64>, rows: Seq<OrderInput>, key: int, customer: int, day: int)
    requires rescan_orders_for_keys_ok(db, keys, rows), seq_view_i64(keys).contains(key),
    ensures sum(rows, |row: OrderInput| if row.o_orderkey == key && row.o_custkey == customer && row.o_orderdate == day { 1int } else { 0int })
        == sum(db.orders, |order: OrdersRow| if order.o_orderkey == key && order.o_custkey == customer && order.o_orderdate == day { 1int } else { 0int }),
{
    let weight = |order: OrdersRow| if order.o_orderkey == key && order.o_custkey == customer && order.o_orderdate == day { 1int } else { 0int };
    lemma_sum_map(rows, |row: OrderInput| order_input_view(row), weight,
        |row: OrderInput| if row.o_orderkey == key && row.o_custkey == customer && row.o_orderdate == day { 1int } else { 0int });
    assert(sum(order_input_seq_view(rows), weight) == sum(db.orders,
        |order: OrdersRow| if seq_view_i64(keys).contains(order.o_orderkey) { weight(order) } else { 0 }));
    lemma_sum_ext_all(db.orders,
        |order: OrdersRow| if seq_view_i64(keys).contains(order.o_orderkey) { weight(order) } else { 0 }, weight);
}

pub proof fn lemma_captured_lines(rows: Seq<LineDelta>, full: Seq<(LineitemRow, int)>, key: int, component: int)
    requires lineitem_delta_ok(rows, full),
    ensures sum(rows, |row: LineDelta| if row.l_orderkey == key { delta_line_weight(row, component) } else { 0 })
        == sum(full, |entry: (LineitemRow, int)| if entry.0.l_orderkey == key { entry.1 * line_weight(entry.0, component) } else { 0 }),
{
    let runtime = |row: LineDelta| if row.l_orderkey == key { delta_line_weight(row, component) } else { 0 };
    let captured = |entry: (LineitemRow, int)| if entry.0.l_orderkey == key { entry.1 * line_weight(entry.0, component) } else { 0 };
    let position_weight = |position: int| captured(full[position]);
    assert forall|position: int| 0 <= position < rows.len() implies
        #[trigger] position_weight(position) == runtime(rows[position]) by {
        assert(rows[position].l_orderkey as int == full[position].0.l_orderkey);
        assert(rows[position].l_extendedprice as int == full[position].0.l_extendedprice);
        assert(rows[position].l_discount as int == full[position].0.l_discount);
        assert((rows[position].is_returned != 0) == (full[position].0.l_returnflag == seq![b'R']));
        assert(rows[position].sign as int == full[position].1);
        let magnitude = if component == 0 { 1int } else {
            let revenue = rows[position].l_extendedprice as int * (100 - rows[position].l_discount as int);
            if component == 1 { revenue } else if revenue > 0 { revenue } else { 0 }
        };
        assert(magnitude * rows[position].sign as int == rows[position].sign as int * magnitude) by (nonlinear_arith);
    }
    lemma_sum_index(rows, runtime, position_weight);
    lemma_sum_index(full, captured, position_weight);
}

pub proof fn lemma_captured_orders(rows: Seq<OrderDelta>, key: int, customer: int, day: int)
    ensures sum(rows, |delta: OrderDelta| if delta.row.o_orderkey == key && delta.row.o_custkey == customer && delta.row.o_orderdate == day { delta.sign as int } else { 0 })
        == sum(order_delta_seq_view(rows), |entry: (OrdersRow, int)|
            if entry.0.o_orderkey == key && entry.0.o_custkey == customer && entry.0.o_orderdate == day { entry.1 } else { 0 }),
{
    let weight = |entry: (OrdersRow, int)|
        if entry.0.o_orderkey == key && entry.0.o_custkey == customer && entry.0.o_orderdate == day { entry.1 } else { 0 };
    lemma_sum_map(rows, |delta: OrderDelta| (order_input_view(delta.row), delta.sign as int), weight,
        |delta: OrderDelta| if delta.row.o_orderkey == key && delta.row.o_custkey == customer && delta.row.o_orderdate == day { delta.sign as int } else { 0 });
}

pub proof fn lemma_order_class(inner: &Index<OrderCell>, customer: int, day: int)
    requires order_leaf_valid(*inner),
    ensures sum(inner.entries(), |entry: ((int, Seq<u8>), OrderCell)|
        if entry.1.customer as int == customer && entry.1.day as int == day { entry.1.count as int } else { 0 })
        == match inner.at(order_key_view(customer, day)) {
            Some(cell) => if cell.customer as int == customer && cell.day as int == day { cell.count as int } else { 0 },
            None => 0,
        },
{
    inner.model();
    let weight = |entry: ((int, Seq<u8>), OrderCell)|
        if entry.1.customer as int == customer && entry.1.day as int == day { entry.1.count as int } else { 0 };
    verus_prooflib::ordered::lemma_key_contribution(inner, order_key_view(customer, day), weight);
    assert forall|position: int| #![trigger inner.entries()[position]]
        0 <= position < inner.entries().len() implies
            weight(inner.entries()[position]) == if inner.entries()[position].0 == order_key_view(customer, day) {
                weight(inner.entries()[position]) } else { 0 } by {
        let entry = inner.entries()[position];
        assert(inner.at(entry.0).is_some());
    }
    lemma_sum_ext(inner.entries(), weight,
        |entry: ((int, Seq<u8>), OrderCell)| if entry.0 == order_key_view(customer, day) { weight(entry) } else { 0 });
}

pub proof fn lemma_order_group_rows<Row>(orders: Orders, rows: Seq<Row>, project: spec_fn(Row) -> (int, int, int), multiplicity: spec_fn(Row) -> int,
    key: int, day: int, customer: int, grain: int)
    requires orders_valid(orders),
        forall|target_customer: int, target_day: int| #[trigger] order_count(orders, key, target_customer, target_day)
            == sum(rows, |row: Row| if project(row).0 == key && project(row).1 == target_customer && project(row).2 == target_day {
                multiplicity(row) } else { 0 }),
    ensures order_group_at(orders, key, day, customer, grain)
        == sum(rows, |row: Row| if project(row).0 == key && project(row).1 == customer && period_of(project(row).2, grain) == day {
            multiplicity(row) } else { 0 }),
{
    let entries = match orders.at((key, Seq::empty())) {
        Some(inner) => inner.entries(),
        None => Seq::empty(),
    };
    let source_class = |row: Row| (project(row).1, project(row).2);
    let entry_class = |entry: ((int, Seq<u8>), OrderCell)| (entry.1.customer as int, entry.1.day as int);
    let source_count = |row: Row| if project(row).0 == key { multiplicity(row) } else { 0 };
    let entry_count = |entry: ((int, Seq<u8>), OrderCell)| entry.1.count as int;
    let weight = |class: (int, int)| if class.0 == customer && period_of(class.1, grain) == day { 1int } else { 0int };
    assert forall|class: (int, int)| #[trigger] verus_prooflib::class_total(rows, source_class, source_count, class)
        == verus_prooflib::class_total(entries, entry_class, entry_count, class) by {
        assert(order_count(orders, key, class.0, class.1)
            == sum(rows, |row: Row| if project(row).0 == key && project(row).1 == class.0 && project(row).2 == class.1 {
                multiplicity(row) } else { 0 }));
        match orders.at((key, Seq::empty())) {
            Some(inner) => {
                assert(orders.at((key, Seq::empty())).is_some());
                lemma_order_class(&inner, class.0, class.1);
            },
            None => {},
        }
        assert(sum(entries, |entry: ((int, Seq<u8>), OrderCell)|
            if entry.1.customer as int == class.0 && entry.1.day as int == class.1 { entry.1.count as int } else { 0 })
            == order_count(orders, key, class.0, class.1));
        lemma_sum_ext_all(rows,
            |row: Row| if source_class(row) == class { source_count(row) } else { 0 },
            |row: Row| if project(row).0 == key && project(row).1 == class.0 && project(row).2 == class.1 { multiplicity(row) } else { 0 });
        lemma_sum_ext_all(entries,
            |entry: ((int, Seq<u8>), OrderCell)| if entry_class(entry) == class { entry_count(entry) } else { 0 },
            |entry: ((int, Seq<u8>), OrderCell)| if entry.1.customer as int == class.0 && entry.1.day as int == class.1 { entry.1.count as int } else { 0 });
    }
    verus_prooflib::lemma_equal_classes(rows, entries, source_class, entry_class, source_count, entry_count, weight);
    lemma_sum_ext_all(rows, |row: Row| source_count(row) * weight(source_class(row)),
        |row: Row| if project(row).0 == key && project(row).1 == customer && period_of(project(row).2, grain) == day { multiplicity(row) } else { 0 });
    lemma_sum_ext_all(entries, |entry: ((int, Seq<u8>), OrderCell)| entry_count(entry) * weight(entry_class(entry)),
        order_group_weight(day, customer, grain));
}

pub proof fn lemma_rescanned_order_group(db: Db, keys: Seq<i64>, rows: Seq<OrderInput>, orders: Orders, key: int, day: int, customer: int, grain: int)
    requires orders_valid(orders), rescan_orders_for_keys_ok(db, keys, rows), seq_view_i64(keys).contains(key),
        forall|target_customer: int, target_day: int| #[trigger] order_count(orders, key, target_customer, target_day)
            == sum(rows, |row: OrderInput| if row.o_orderkey == key && row.o_custkey == target_customer && row.o_orderdate == target_day { 1int } else { 0int }),
    ensures order_group_at(orders, key, day, customer, grain) == crate::capture_proofs::order_group(db.orders, key, day, customer, grain),
{
    reveal(crate::capture_proofs::order_group);
    let projection = |order: OrdersRow| (order.o_orderkey, order.o_custkey, order.o_orderdate);
    let multiplicity = |order: OrdersRow| 1int;
    assert forall|target_customer: int, target_day: int| #[trigger] order_count(orders, key, target_customer, target_day)
        == sum(db.orders, |order: OrdersRow| if projection(order).0 == key && projection(order).1 == target_customer && projection(order).2 == target_day { multiplicity(order) } else { 0int }) by {
        assert(order_count(orders, key, target_customer, target_day)
            == sum(rows, |row: OrderInput| if row.o_orderkey == key && row.o_custkey == target_customer && row.o_orderdate == target_day { 1int } else { 0int }));
        lemma_rescanned_orders(db, keys, rows, key, target_customer, target_day);
        lemma_sum_ext_all(db.orders,
            |order: OrdersRow| if order.o_orderkey == key && order.o_custkey == target_customer && order.o_orderdate == target_day { 1int } else { 0int },
            |order: OrdersRow| if projection(order).0 == key && projection(order).1 == target_customer && projection(order).2 == target_day { multiplicity(order) } else { 0int });
    }
    lemma_order_group_rows(orders, db.orders, projection, multiplicity, key, day, customer, grain);
    lemma_sum_ext_all(db.orders,
        |order: OrdersRow| if projection(order).0 == key && projection(order).1 == customer && period_of(projection(order).2, grain) == day { multiplicity(order) } else { 0int },
        |order: OrdersRow| if order.o_orderkey == key && order.o_custkey == customer && period_of(order.o_orderdate, grain) == day { 1int } else { 0int });
}

pub proof fn lemma_captured_order_group(rows: Seq<OrderDelta>, orders: Orders, key: int, day: int, customer: int, grain: int)
    requires orders_valid(orders),
        forall|target_customer: int, target_day: int| #[trigger] order_count(orders, key, target_customer, target_day)
            == sum(rows, |delta: OrderDelta| if delta.row.o_orderkey == key && delta.row.o_custkey == target_customer && delta.row.o_orderdate == target_day { delta.sign as int } else { 0int }),
    ensures order_group_at(orders, key, day, customer, grain)
        == crate::capture_proofs::order_delta_group(order_delta_seq_view(rows), key, day, customer, grain),
{
    reveal(crate::capture_proofs::order_delta_group);
    let projection = |entry: (OrdersRow, int)| (entry.0.o_orderkey, entry.0.o_custkey, entry.0.o_orderdate);
    let multiplicity = |entry: (OrdersRow, int)| entry.1;
    assert forall|target_customer: int, target_day: int| #[trigger] order_count(orders, key, target_customer, target_day)
        == sum(order_delta_seq_view(rows), |entry: (OrdersRow, int)| if projection(entry).0 == key && projection(entry).1 == target_customer && projection(entry).2 == target_day { multiplicity(entry) } else { 0int }) by {
        assert(order_count(orders, key, target_customer, target_day)
            == sum(rows, |delta: OrderDelta| if delta.row.o_orderkey == key && delta.row.o_custkey == target_customer && delta.row.o_orderdate == target_day { delta.sign as int } else { 0int }));
        lemma_captured_orders(rows, key, target_customer, target_day);
        lemma_sum_ext_all(order_delta_seq_view(rows),
            |entry: (OrdersRow, int)| if entry.0.o_orderkey == key && entry.0.o_custkey == target_customer && entry.0.o_orderdate == target_day { entry.1 } else { 0int },
            |entry: (OrdersRow, int)| if projection(entry).0 == key && projection(entry).1 == target_customer && projection(entry).2 == target_day { multiplicity(entry) } else { 0int });
    }
    lemma_order_group_rows(orders, order_delta_seq_view(rows), projection, multiplicity, key, day, customer, grain);
    lemma_sum_ext_all(order_delta_seq_view(rows),
        |entry: (OrdersRow, int)| if projection(entry).0 == key && projection(entry).1 == customer && period_of(projection(entry).2, grain) == day { multiplicity(entry) } else { 0int },
        |entry: (OrdersRow, int)| if entry.0.o_orderkey == key && entry.0.o_custkey == customer && period_of(entry.0.o_orderdate, grain) == day { entry.1 } else { 0int });
}

pub proof fn lemma_scanned_facts(db: Db, orders: Seq<OrderInput>, rows: Seq<ReturnedLineInput>, day: int, customer: int, grain: int, component: int)
    requires scan_orders_ok(db, orders), scan_returned_lines_ok(db, rows),
    ensures scanned_fact_sum(orders, rows, day, customer, grain, component) == fact_sum(db, day, customer, grain, component),
{
    let weight = |order: OrdersRow| if period_of(order.o_orderdate, grain) == day && order.o_custkey == customer {
        sum(db.lineitem, |line: LineitemRow| if line.l_orderkey == order.o_orderkey { line_weight(line, component) } else { 0 })
    } else { 0 };
    let scanned = |order: OrderInput| if period_of(order.o_orderdate as int, grain) == day && order.o_custkey == customer {
        scanned_line_sum(rows, order.o_orderkey as int, component)
    } else { 0 };
    assert forall|order: OrderInput| #[trigger] scanned(order) == weight(order_input_view(order)) by {
        lemma_scanned_lines(db, rows, order.o_orderkey as int, component);
    }
    lemma_sum_map(orders, |order: OrderInput| order_input_view(order), weight, scanned);
    assert(sum(order_input_seq_view(orders), weight) == sum(db.orders, weight));
    reveal(fact_sum);
    lemma_sum_ext_all(db.orders, weight,
        |order: OrdersRow| if order.o_custkey == customer && period_of(order.o_orderdate, grain) == day {
            sum(db.lineitem, |line: LineitemRow| if line.l_orderkey == order.o_orderkey { line_weight(line, component) } else { 0 })
        } else { 0 });
}

pub proof fn lemma_scanned_bounds(orders: Seq<OrderInput>, rows: Seq<ReturnedLineInput>, day: int, customer: int, grain: int, component: int)
    requires 0 <= grain <= 3,
        day < period_of(i32::MIN as int, grain) || day > period_of(i32::MAX as int, grain),
    ensures scanned_fact_sum(orders, rows, day, customer, grain, component) == 0,
{
    let weight = |order: OrderInput| if period_of(order.o_orderdate as int, grain) == day && order.o_custkey == customer {
        scanned_line_sum(rows, order.o_orderkey as int, component) } else { 0 };
    assert forall|position: int| #![trigger orders[position]]
        0 <= position < orders.len() implies weight(orders[position]) == 0 by {
        lemma_period_bounds(orders[position].o_orderdate as int, grain);
    }
    lemma_sum_zero(orders, weight);
}

pub proof fn lemma_facts_from_scans(db: Db, slices: Index<Slice>, orders: Seq<OrderInput>, rows: Seq<ReturnedLineInput>, grain: int)
    requires scan_orders_ok(db, orders), scan_returned_lines_ok(db, rows), 0 <= grain <= 3,
        slices_valid(slices),
        forall|day: int, customer: int, component: int|
            #[trigger] slice_at(slices, day, customer).component(component)
                == scanned_fact_sum(orders, rows, day, customer, grain, component),
    ensures facts_valid(db, slices, grain),
{
    reveal(facts_valid);
    assert forall|day: int, customer: int, component: int|
        #[trigger] slice_at(slices, day, customer).component(component)
            == fact_sum(db, day, customer, grain, component) by {
        lemma_scanned_facts(db, orders, rows, day, customer, grain, component);
    }
    assert forall|key: (int, Seq<u8>)| #[trigger] slices.at(key).is_some() implies {
        let slice = slices.at(key).unwrap();
        &&& key.1 == Seq::<u8>::empty()
        &&& period_of(i32::MIN as int, grain) <= key.0 <= period_of(i32::MAX as int, grain)
        &&& slice.cells.nonempty()
        &&& slice.valid()
    } by {
        let slice = slices.at(key).unwrap();
        if key.0 < period_of(i32::MIN as int, grain) || key.0 > period_of(i32::MAX as int, grain) {
            let ckey = choose|ckey: (int, Seq<u8>)| slice.cells.at(ckey).is_some();
            lemma_cell_key(slice.cells, ckey);
            let cell = slice.cells.at(ckey).unwrap();
            assert(cell.count > 0);
            lemma_scanned_bounds(orders, rows, key.0, ckey.0, grain, 0);
            assert(slice_at(slices, key.0, ckey.0).count == cell.count as int);
            assert(slice_at(slices, key.0, ckey.0).component(0) == 0);
        }
    }
}

}

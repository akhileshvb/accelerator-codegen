use vstd::prelude::*;
use verus_common::sums::*;
use verus_common::rel::*;
use verus_common::bytes::*;
use crate::gen_spec::*;
use crate::gen_obligations::*;
use crate::kernel::*;
use crate::moments::Orders;

verus! {

pub proof fn lemma_scan_orders(db: Db, high: Seq<OrderKeyInput>, low: Seq<OrderKeyInput>, key: i64)
    requires scan_high_orders_ok(db, high), scan_low_orders_ok(db, low),
    ensures scan_order_count(high, key) == order_count(db, key as int, true),
        scan_order_count(low, key) == order_count(db, key as int, false),
{
    let weight = |value: int| if value == key as int { 1 } else { 0 };
    lemma_sum_map(high, |row: OrderKeyInput| order_key_input_view(row), weight,
        |row: OrderKeyInput| if row.o_orderkey == key { 1 } else { 0 });
    lemma_sum_map(low, |row: OrderKeyInput| order_key_input_view(row), weight,
        |row: OrderKeyInput| if row.o_orderkey == key { 1 } else { 0 });
    let high_weight = |order: OrdersRow| if order.o_orderpriority == seq![b'1', b'-', b'U', b'R', b'G', b'E', b'N', b'T']
        || order.o_orderpriority == seq![b'2', b'-', b'H', b'I', b'G', b'H'] { weight(order.o_orderkey) } else { 0 };
    let low_weight = |order: OrdersRow| if order.o_orderpriority != seq![b'1', b'-', b'U', b'R', b'G', b'E', b'N', b'T']
        && order.o_orderpriority != seq![b'2', b'-', b'H', b'I', b'G', b'H'] { weight(order.o_orderkey) } else { 0 };
    assert(sum(order_key_input_seq_view(high), weight) == sum(db.orders, high_weight));
    assert(sum(order_key_input_seq_view(low), weight) == sum(db.orders, low_weight));
    lemma_sum_ext_all(db.orders, high_weight,
        |order: OrdersRow| if order.o_orderkey == key as int && high_priority(order) == true { 1 } else { 0 });
    lemma_sum_ext_all(db.orders, low_weight,
        |order: OrdersRow| if order.o_orderkey == key as int && high_priority(order) == false { 1 } else { 0 });
}

pub open spec fn line_selected(line: LineitemRow, mode: Seq<u8>, day: int) -> bool {
    line.l_shipmode == mode && line.l_receiptdate == day
        && line.l_shipdate < line.l_commitdate && line.l_commitdate < line.l_receiptdate
}

pub proof fn lemma_cell_transpose(db: Db, mode: Seq<u8>, day: int, high: bool)
    ensures cell_count(db, mode, day, high) == sum(db.lineitem, |line: LineitemRow|
        if line_selected(line, mode, day) { order_count(db, line.l_orderkey, high) } else { 0 }),
{
    let weight = |order: OrdersRow, line: LineitemRow|
        if high_priority(order) == high && order.o_orderkey == line.l_orderkey
            && line_selected(line, mode, day) { 1 } else { 0 };
    let by_order = |order: OrdersRow| if high_priority(order) == high {
        line_count(db, order.o_orderkey, mode, day)
    } else { 0 };
    let by_line = |line: LineitemRow| if line_selected(line, mode, day) {
        order_count(db, line.l_orderkey, high)
    } else { 0 };
    assert forall|order: OrdersRow| #[trigger] by_order(order) == sum(db.lineitem, |line: LineitemRow| weight(order, line)) by {
        if high_priority(order) == high {
            lemma_sum_ext_all(db.lineitem, |line: LineitemRow| weight(order, line),
                |line: LineitemRow| if line.l_orderkey == order.o_orderkey && line_selected(line, mode, day) { 1 } else { 0 });
        } else { lemma_sum_zero(db.lineitem, |line: LineitemRow| weight(order, line)); }
    }
    assert forall|line: LineitemRow| #[trigger] by_line(line) == sum(db.orders, |order: OrdersRow| weight(order, line)) by {
        if line_selected(line, mode, day) {
            lemma_sum_ext_all(db.orders, |order: OrdersRow| weight(order, line),
                |order: OrdersRow| if order.o_orderkey == line.l_orderkey && high_priority(order) == high { 1 } else { 0 });
        } else { lemma_sum_zero(db.orders, |order: OrdersRow| weight(order, line)); }
    }
    lemma_sum_swap(db.orders, db.lineitem, weight, by_order, by_line);
}

pub proof fn lemma_build_cell(db: Db, high_rows: Seq<OrderKeyInput>, low_rows: Seq<OrderKeyInput>,
    lines: Seq<QualifyingLineInput>, orders: Orders, mode: Seq<u8>, day: int, high: bool)
    requires scan_high_orders_ok(db, high_rows), scan_low_orders_ok(db, low_rows),
        scan_qualifying_lines_ok(db, lines),
        forall|key: i64| #![trigger orders.at(key)]
            orders.at(key).high == scan_order_count(high_rows, key)
                && orders.at(key).low == scan_order_count(low_rows, key),
    ensures cell_count(db, mode, day, high) == if i32::MIN <= day <= i32::MAX {
        joined_scan(lines, orders, mode, day as i32, high)
    } else { 0 },
{
    lemma_cell_transpose(db, mode, day, high);
    let weight = |row: (int, int, Seq<u8>)| if row.1 == day && row.2 == mode {
        order_count(db, row.0, high)
    } else { 0 };
    let runtime = |row: QualifyingLineInput| weight(qualifying_line_input_view(row));
    lemma_sum_map(lines, |row: QualifyingLineInput| qualifying_line_input_view(row), weight, runtime);
    assert(sum(qualifying_line_input_seq_view(lines), weight) == sum(lines, runtime));
    assert(sum(qualifying_line_input_seq_view(lines), weight) == sum(db.lineitem, |line: LineitemRow|
        if line.l_commitdate < line.l_receiptdate && line.l_shipdate < line.l_commitdate {
            weight((line.l_orderkey, line.l_receiptdate, line.l_shipmode))
        } else { 0 }));
    lemma_sum_ext_all(db.lineitem, |line: LineitemRow|
        if line.l_commitdate < line.l_receiptdate && line.l_shipdate < line.l_commitdate {
            weight((line.l_orderkey, line.l_receiptdate, line.l_shipmode))
        } else { 0 }, |line: LineitemRow|
        if line_selected(line, mode, day) { order_count(db, line.l_orderkey, high) } else { 0 });
    assert(cell_count(db, mode, day, high) == sum(lines, runtime));
    if i32::MIN <= day <= i32::MAX {
        let narrowed = day as i32;
        let folded = joined_weight(orders, mode, narrowed, high);
        assert forall|row: QualifyingLineInput| #[trigger] runtime(row) == folded(row) by {
            lemma_scan_orders(db, high_rows, low_rows, row.l_orderkey);
        }
        lemma_sum_ext_all(lines, runtime, folded);
        assert(sum(lines, folded) == joined_scan(lines, orders, mode, day as i32, high));
    } else {
        assert forall|row: QualifyingLineInput| #[trigger] runtime(row) == 0 by {}
        lemma_sum_zero(lines, runtime);
    }
}

pub open spec fn sql_nested(db: Db, parameters: Params, mode: Seq<u8>, weight: spec_fn(Tuple) -> int) -> int {
    sum(db.orders, |order: OrdersRow| sum(db.lineitem, |line: LineitemRow|
        if where_clause(db, parameters, order, line) && line.l_shipmode == mode {
            weight(Tuple { orders: order, lineitem: line })
        } else { 0 }))
}

pub proof fn lemma_sql_sum(db: Db, parameters: Params, mode: Seq<u8>, weight: spec_fn(Tuple) -> int)
    ensures group_sum(db, parameters, mode, weight) == sql_nested(db, parameters, mode, weight),
{
    let grouped = |tuple: Tuple| if tuple.lineitem.l_shipmode == mode { weight(tuple) } else { 0 };
    let per_order = |order: OrdersRow| sum(tuples_1(db, parameters, order), grouped);
    verus_prooflib::lemma_flat_map_weight(db.orders,
        |order: OrdersRow| tuples_1(db, parameters, order), grouped, per_order);
    let nested = |order: OrdersRow| sum(db.lineitem, |line: LineitemRow|
        if where_clause(db, parameters, order, line) && line.l_shipmode == mode {
            weight(Tuple { orders: order, lineitem: line })
        } else { 0 });
    assert forall|order: OrdersRow| #[trigger] per_order(order) == nested(order) by {
        let inner = |line: LineitemRow| sum(tuples_2(db, parameters, order, line), grouped);
        verus_prooflib::lemma_flat_map_weight(db.lineitem,
            |line: LineitemRow| tuples_2(db, parameters, order, line), grouped, inner);
        let normalized = |line: LineitemRow| if where_clause(db, parameters, order, line) && line.l_shipmode == mode {
            weight(Tuple { orders: order, lineitem: line })
        } else { 0 };
        assert forall|line: LineitemRow| #[trigger] inner(line) == normalized(line) by {
            lemma_sum_opt(where_clause(db, parameters, order, line), Tuple { orders: order, lineitem: line }, grouped);
        }
        lemma_sum_ext_all(db.lineitem, inner, normalized);
    }
    lemma_sum_ext_all(db.orders, per_order, nested);
}

pub open spec fn range_cells(db: Db, mode: Seq<u8>, start: int, end: int, high: bool) -> int {
    sum(rng(if end > start { end - start } else { 0 }), |offset: int| cell_count(db, mode, start + offset, high))
}

pub open spec fn case_weight(high: bool) -> spec_fn(Tuple) -> int {
    |tuple: Tuple| if high_priority(tuple.orders) == high { 1 } else { 0 }
}

pub proof fn lemma_group_ext(db: Db, parameters: Params, mode: Seq<u8>, left: spec_fn(Tuple) -> int, right: spec_fn(Tuple) -> int)
    requires forall|tuple: Tuple| #![trigger left(tuple)] left(tuple) == right(tuple),
    ensures group_sum(db, parameters, mode, left) == group_sum(db, parameters, mode, right),
{
    lemma_sum_ext_all(tuples(db, parameters),
        |tuple: Tuple| if tuple.lineitem.l_shipmode == mode { left(tuple) } else { 0 },
        |tuple: Tuple| if tuple.lineitem.l_shipmode == mode { right(tuple) } else { 0 });
}

pub proof fn lemma_group_case(db: Db, parameters: Params, mode: Seq<u8>, high: bool)
    ensures group_sum(db, parameters, mode, case_weight(high)) ==
        if high { row_of(db, parameters, mode).high_line_count } else { row_of(db, parameters, mode).low_line_count },
{
    if high {
        let weight = |tuple: Tuple| if tuple.orders.o_orderpriority == seq![b'1', b'-', b'U', b'R', b'G', b'E', b'N', b'T']
            || tuple.orders.o_orderpriority == seq![b'2', b'-', b'H', b'I', b'G', b'H'] { 1 } else { 0 };
        lemma_group_ext(db, parameters, mode, case_weight(high), weight);
    } else {
        let weight = |tuple: Tuple| if tuple.orders.o_orderpriority != seq![b'1', b'-', b'U', b'R', b'G', b'E', b'N', b'T']
            && tuple.orders.o_orderpriority != seq![b'2', b'-', b'H', b'I', b'G', b'H'] { 1 } else { 0 };
        lemma_group_ext(db, parameters, mode, case_weight(high), weight);
    }
}

pub proof fn lemma_range_sql(db: Db, parameters: Params, mode: Seq<u8>, high: bool)
    ensures group_sum(db, parameters, mode, case_weight(high))
        == if mode == parameters.p1 || mode == parameters.p2 {
            range_cells(db, mode, parameters.p3, parameters.p4, high)
        } else { 0 },
{
    let weight = case_weight(high);
    lemma_sql_sum(db, parameters, mode, weight);
    let length = if parameters.p4 > parameters.p3 { parameters.p4 - parameters.p3 } else { 0 };
    let days = rng(length);
    let per_day = |offset: int| cell_count(db, mode, parameters.p3 + offset, high);
    let nested = |order: OrdersRow| sum(days, |offset: int|
        if high_priority(order) == high { line_count(db, order.o_orderkey, mode, parameters.p3 + offset) } else { 0 });
    let cross = |offset: int, order: OrdersRow|
        if high_priority(order) == high { line_count(db, order.o_orderkey, mode, parameters.p3 + offset) } else { 0 };
    assert forall|order: OrdersRow| #[trigger] nested(order) == sum(days, |offset: int| cross(offset, order)) by {
        lemma_sum_ext_all(days, |offset: int| if high_priority(order) == high {
            line_count(db, order.o_orderkey, mode, parameters.p3 + offset)
        } else { 0 }, |offset: int| cross(offset, order));
    }
    assert forall|offset: int| #[trigger] per_day(offset) == sum(db.orders, |order: OrdersRow| cross(offset, order)) by {
        lemma_sum_ext_all(db.orders, |order: OrdersRow| if high_priority(order) == high {
            line_count(db, order.o_orderkey, mode, parameters.p3 + offset)
        } else { 0 }, |order: OrdersRow| cross(offset, order));
    }
    lemma_sum_swap(days, db.orders, cross, per_day, nested);
    let sql_order = |order: OrdersRow| sum(db.lineitem, |line: LineitemRow|
        if where_clause(db, parameters, order, line) && line.l_shipmode == mode {
            weight(Tuple { orders: order, lineitem: line })
        } else { 0 });
    assert forall|order: OrdersRow| #[trigger] nested(order) == if mode == parameters.p1 || mode == parameters.p2 { sql_order(order) } else { nested(order) } by {
        if mode == parameters.p1 || mode == parameters.p2 {
            let cell_weight = |offset: int, line: LineitemRow| if high_priority(order) == high
                && order.o_orderkey == line.l_orderkey && line_selected(line, mode, parameters.p3 + offset) { 1 } else { 0 };
            let by_day = |offset: int| if high_priority(order) == high {
                line_count(db, order.o_orderkey, mode, parameters.p3 + offset)
            } else { 0 };
            let by_line = |line: LineitemRow| sum(days, |offset: int| cell_weight(offset, line));
            assert forall|offset: int| #[trigger] by_day(offset) == sum(db.lineitem, |line: LineitemRow| cell_weight(offset, line)) by {
                if high_priority(order) == high {
                    lemma_sum_ext_all(db.lineitem, |line: LineitemRow| cell_weight(offset, line),
                        |line: LineitemRow| if line.l_orderkey == order.o_orderkey && line_selected(line, mode, parameters.p3 + offset) { 1 } else { 0 });
                } else { lemma_sum_zero(db.lineitem, |line: LineitemRow| cell_weight(offset, line)); }
            }
            lemma_sum_swap(days, db.lineitem, cell_weight, by_day, by_line);
            let sql_line = |line: LineitemRow| if where_clause(db, parameters, order, line) && line.l_shipmode == mode {
                weight(Tuple { orders: order, lineitem: line })
            } else { 0 };
            assert forall|line: LineitemRow| #[trigger] by_line(line) == sql_line(line) by {
                let value = if high_priority(order) == high && order.o_orderkey == line.l_orderkey
                    && line.l_shipmode == mode && line.l_shipdate < line.l_commitdate
                    && line.l_commitdate < line.l_receiptdate { 1int } else { 0int };
                lemma_sum_ext_all(days, |offset: int| cell_weight(offset, line),
                    |offset: int| if parameters.p3 + offset == line.l_receiptdate { value } else { 0 });
                verus_prooflib::lemma_range_single_cell(length, line.l_receiptdate, parameters.p3, value);
            }
            lemma_sum_ext_all(db.lineitem, by_line, sql_line);
        }
    }
    if mode == parameters.p1 || mode == parameters.p2 {
        lemma_sum_ext_all(db.orders, nested, sql_order);
    } else {
        assert forall|order: OrdersRow| #[trigger] sql_order(order) == 0 by {
            lemma_sum_zero(db.lineitem, |line: LineitemRow|
                if where_clause(db, parameters, order, line) && line.l_shipmode == mode {
                    weight(Tuple { orders: order, lineitem: line })
                } else { 0 });
        }
        lemma_sum_zero(db.orders, sql_order);
    }
}

pub proof fn lemma_group_count(db: Db, parameters: Params, mode: Seq<u8>)
    ensures group_count(db, parameters, mode) == row_of(db, parameters, mode).high_line_count
        + row_of(db, parameters, mode).low_line_count,
{
    let high = |tuple: Tuple| if tuple.lineitem.l_shipmode == mode && high_priority(tuple.orders) { 1int } else { 0int };
    let low = |tuple: Tuple| if tuple.lineitem.l_shipmode == mode && !high_priority(tuple.orders) { 1int } else { 0int };
    let count = |tuple: Tuple| if tuple.lineitem.l_shipmode == mode { 1int } else { 0int };
    lemma_sum_add(tuples(db, parameters), high, low, count);
    let high_case = |tuple: Tuple| if tuple.orders.o_orderpriority == seq![b'1', b'-', b'U', b'R', b'G', b'E', b'N', b'T']
            || tuple.orders.o_orderpriority == seq![b'2', b'-', b'H', b'I', b'G', b'H'] { 1 } else { 0 }
    ;
    let low_case = |tuple: Tuple| if tuple.orders.o_orderpriority != seq![b'1', b'-', b'U', b'R', b'G', b'E', b'N', b'T']
            && tuple.orders.o_orderpriority != seq![b'2', b'-', b'H', b'I', b'G', b'H'] { 1 } else { 0 }
    ;
    lemma_sum_ext_all(tuples(db, parameters), high,
        |tuple: Tuple| if tuple.lineitem.l_shipmode == mode { high_case(tuple) } else { 0 });
    lemma_sum_ext_all(tuples(db, parameters), low,
        |tuple: Tuple| if tuple.lineitem.l_shipmode == mode { low_case(tuple) } else { 0 });
    assert(group_count(db, parameters, mode) == sum(tuples(db, parameters), count));
    assert(row_of(db, parameters, mode).high_line_count == sum(tuples(db, parameters), high));
    assert(row_of(db, parameters, mode).low_line_count == sum(tuples(db, parameters), low));
}

pub proof fn lemma_execute_result(db: Db, parameters: Params, out: Seq<OutRow>)
    requires
        forall|index: int| #![trigger out[index]] 0 <= index < out.len() ==>
            out[index] == row_of(db, parameters, out[index].l_shipmode)
                && out[index].high_line_count + out[index].low_line_count > 0,
        forall|left: int, right: int| #![trigger out[left], out[right]] 0 <= left < right < out.len() ==>
            bytes_lt(out[left].l_shipmode, out[right].l_shipmode),
        forall|mode: Seq<u8>| #![trigger group_count(db, parameters, mode)] group_count(db, parameters, mode) > 0 ==>
            exists|index: int| #![trigger out[index]] 0 <= index < out.len() && out[index].l_shipmode == mode,
    ensures query_result(db, parameters, out),
{
    assert forall|index: int| #![trigger out[index]] 0 <= index < out.len() implies row_ok(db, parameters, out[index]) by {
        lemma_group_count(db, parameters, out[index].l_shipmode);
    }
    assert forall|left: int, right: int| #![trigger out[left], out[right]] 0 <= left < right < out.len() implies
        !same_group(out[left], out[right]) && !row_lt(out[right], out[left]) by {
        lemma_bytes_lt_irrefl(out[left].l_shipmode);
        lemma_bytes_lt_asym(out[left].l_shipmode, out[right].l_shipmode);
    }
}

pub proof fn lemma_apply_result(db: Db, post: Db, mode: Seq<u8>, day: int, high: bool,
    before: int, change: int, after: int)
    requires before == cell_count(db, mode, day, high),
        change == cell_count(post, mode, day, high) - cell_count(db, mode, day, high),
        after == before + change,
    ensures after == cell_count(post, mode, day, high),
{
}

}

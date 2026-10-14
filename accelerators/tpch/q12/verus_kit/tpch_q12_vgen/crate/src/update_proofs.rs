use vstd::prelude::*;
use verus_common::sums::*;
use verus_common::rel::*;
use crate::gen_spec::*;
use crate::gen_obligations::*;
use crate::kernel::*;
use crate::moments;

verus! {

broadcast use vstd::seq_lib::group_seq_properties;

pub proof fn lemma_key_view(keys: Seq<i64>, key: i64)
    requires keys.contains(key),
    ensures seq_view_i64(keys).contains(key as int),
{
    let index = choose|index: int| #![trigger keys[index]] 0 <= index < keys.len() && keys[index] == key;
    assert(seq_view_i64(keys)[index] == key as int);
}

pub proof fn lemma_rescan_orders(db: Db, high_keys: Seq<i64>, low_keys: Seq<i64>,
    high_rows: Seq<OrderKeyInput>, low_rows: Seq<OrderKeyInput>, key: i64)
    requires rescan_high_orders_for_keys_ok(db, high_keys, high_rows),
        rescan_low_orders_for_keys_ok(db, low_keys, low_rows), high_keys.contains(key), low_keys.contains(key),
    ensures scan_order_count(high_rows, key) == order_count(db, key as int, true),
        scan_order_count(low_rows, key) == order_count(db, key as int, false),
{
    lemma_key_view(high_keys, key);
    lemma_key_view(low_keys, key);
    let weight = |value: int| if value == key as int { 1 } else { 0 };
    lemma_sum_map(high_rows, |row: OrderKeyInput| order_key_input_view(row), weight,
        |row: OrderKeyInput| if row.o_orderkey == key { 1 } else { 0 });
    lemma_sum_map(low_rows, |row: OrderKeyInput| order_key_input_view(row), weight,
        |row: OrderKeyInput| if row.o_orderkey == key { 1 } else { 0 });
    let high_weight = |order: OrdersRow| if (order.o_orderpriority == seq![b'1', b'-', b'U', b'R', b'G', b'E', b'N', b'T']
        || order.o_orderpriority == seq![b'2', b'-', b'H', b'I', b'G', b'H']) && seq_view_i64(high_keys).contains(order.o_orderkey) {
        weight(order.o_orderkey) } else { 0 };
    let low_weight = |order: OrdersRow| if order.o_orderpriority != seq![b'1', b'-', b'U', b'R', b'G', b'E', b'N', b'T']
        && order.o_orderpriority != seq![b'2', b'-', b'H', b'I', b'G', b'H'] && seq_view_i64(low_keys).contains(order.o_orderkey) {
        weight(order.o_orderkey) } else { 0 };
    assert(sum(order_key_input_seq_view(high_rows), weight) == sum(db.orders, high_weight));
    assert(sum(order_key_input_seq_view(low_rows), weight) == sum(db.orders, low_weight));
    lemma_sum_ext_all(db.orders, high_weight,
        |order: OrdersRow| if order.o_orderkey == key as int && high_priority(order) == true { 1 } else { 0 });
    lemma_sum_ext_all(db.orders, low_weight,
        |order: OrdersRow| if order.o_orderkey == key as int && high_priority(order) == false { 1 } else { 0 });
}

pub proof fn lemma_rescan_lines(db: Db, keys: Seq<i64>, rows: Seq<QualifyingLineInput>, key: i64, mode: Seq<u8>, day: int)
    requires rescan_qualifying_lines_for_keys_ok(db, keys, rows), keys.contains(key),
    ensures line_count(db, key as int, mode, day) == if i32::MIN <= day <= i32::MAX {
        scan_line_count(rows, key, mode, day as i32) } else { 0 },
{
    lemma_key_view(keys, key);
    let weight = |row: (int, int, Seq<u8>)| if row.0 == key as int && row.1 == day && row.2 == mode { 1 } else { 0 };
    let runtime = |row: QualifyingLineInput| weight(qualifying_line_input_view(row));
    let database = |line: LineitemRow| if line.l_commitdate < line.l_receiptdate && line.l_shipdate < line.l_commitdate
        && seq_view_i64(keys).contains(line.l_orderkey) { weight((line.l_orderkey, line.l_receiptdate, line.l_shipmode)) } else { 0 };
    lemma_sum_map(rows, |row: QualifyingLineInput| qualifying_line_input_view(row), weight, runtime);
    assert(sum(qualifying_line_input_seq_view(rows), weight) == sum(db.lineitem, database));
    lemma_sum_ext_all(db.lineitem, database, |line: LineitemRow|
        if line.l_orderkey == key as int && crate::semantics::line_selected(line, mode, day) { 1 } else { 0 });
    if i32::MIN <= day <= i32::MAX {
        lemma_sum_ext_all(rows, runtime, scan_line_weight(key, mode, day as i32));
    } else { lemma_sum_zero(rows, runtime); }
}

pub open spec fn order_delta_count(rows: Seq<OrderDelta>, key: int, high: bool) -> int {
    sum(rows, |row: OrderDelta| if row.o_orderkey as int == key && (row.is_high != 0) == high { row.sign as int } else { 0 })
}

pub open spec fn line_delta_count(rows: Seq<LineDelta>, key: int, mode: Seq<u8>, day: int) -> int {
    sum(rows, |row: LineDelta| if row.row.l_orderkey as int == key
        && crate::semantics::line_selected(line_capture_input_view(row.row), mode, day) { row.sign as int } else { 0 })
}

pub proof fn lemma_capture_orders(db: Db, post: Db, rows: Seq<OrderDelta>, full: Seq<(OrdersRow, int)>, key: int, high: bool)
    requires orders_delta_ok(rows, full), bag_step(db.orders, post.orders, full),
    ensures order_count(post, key, high) == order_count(db, key, high) + order_delta_count(rows, key, high),
        i64::MIN <= key <= i64::MAX ==> order_delta_count(rows, key, high) == moments::order_change(rows, key as i64, high),
        !(i64::MIN <= key <= i64::MAX) ==> order_delta_count(rows, key, high) == 0,
{
    let weight = |order: OrdersRow| if order.o_orderkey == key && high_priority(order) == high { 1 } else { 0 };
    let signed = |change: (OrdersRow, int)| change.1 * weight(change.0);
    let runtime = |row: OrderDelta| if row.o_orderkey as int == key && (row.is_high != 0) == high { row.sign as int } else { 0 };
    assert forall|index: int| #![trigger rows[index]] 0 <= index < rows.len() implies runtime(rows[index]) == signed(full[index]) by {}
    verus_prooflib::lemma_sum_aligned(rows, full, runtime, signed);
    assert(sum(post.orders, weight) == sum(db.orders, weight) + sum(full, signed));
    if i64::MIN <= key <= i64::MAX {
        lemma_sum_ext_all(rows, runtime, |row: OrderDelta|
            if row.o_orderkey == key as i64 && (row.is_high != 0) == high { row.sign as int } else { 0 });
    } else { lemma_sum_zero(rows, runtime); }
}

pub proof fn lemma_capture_lines(db: Db, post: Db, rows: Seq<LineDelta>, key: int, mode: Seq<u8>, day: int)
    requires bag_step(db.lineitem, post.lineitem, line_delta_seq_view(rows)),
    ensures line_count(post, key, mode, day) == line_count(db, key, mode, day) + line_delta_count(rows, key, mode, day),
        i64::MIN <= key <= i64::MAX && i32::MIN <= day <= i32::MAX ==>
            line_delta_count(rows, key, mode, day) == moments::line_change(rows, key as i64, mode, day as i32),
        !(i64::MIN <= key <= i64::MAX) || !(i32::MIN <= day <= i32::MAX) ==> line_delta_count(rows, key, mode, day) == 0,
{
    let weight = |line: LineitemRow| if line.l_orderkey == key && crate::semantics::line_selected(line, mode, day) { 1 } else { 0 };
    let signed = |change: (LineitemRow, int)| change.1 * weight(change.0);
    let runtime = |row: LineDelta| if row.row.l_orderkey as int == key
        && crate::semantics::line_selected(line_capture_input_view(row.row), mode, day) { row.sign as int } else { 0 };
    lemma_sum_map(rows, |row: LineDelta| (line_capture_input_view(row.row), row.sign as int), signed, runtime);
    assert(sum(post.lineitem, weight) == sum(db.lineitem, weight) + sum(line_delta_seq_view(rows), signed));
    if i64::MIN <= key <= i64::MAX && i32::MIN <= day <= i32::MAX {
        lemma_sum_ext_all(rows, runtime, moments::line_change_weight(key as i64, mode, day as i32));
    } else { lemma_sum_zero(rows, runtime); }
}

pub open spec fn effect(db: Db, post: Db, key: int, mode: Seq<u8>, day: int, high: bool) -> int {
    order_count(post, key, high) * line_count(post, key, mode, day)
        - order_count(db, key, high) * line_count(db, key, mode, day)
}

pub proof fn lemma_factor(db: Db, keys: Seq<int>, mode: Seq<u8>, day: int, high: bool)
    requires distinct(keys),
        forall|index: int| #![trigger db.orders[index]] 0 <= index < db.orders.len() ==> keys.contains(db.orders[index].o_orderkey),
    ensures cell_count(db, mode, day, high) == sum(keys, |key: int| order_count(db, key, high) * line_count(db, key, mode, day)),
{
    let weight = |order: OrdersRow| if high_priority(order) == high { line_count(db, order.o_orderkey, mode, day) } else { 0 };
    let per = |key: int| order_count(db, key, high) * line_count(db, key, mode, day);
    let key_fn = |order: OrdersRow| order.o_orderkey;
    assert forall|key: int| #[trigger] per(key) == sum(db.orders, |order: OrdersRow| if key_fn(order) == key { weight(order) } else { 0 }) by {
        let indicator = |order: OrdersRow| if order.o_orderkey == key && high_priority(order) == high { 1 } else { 0 };
        let scaled = |order: OrdersRow| if key_fn(order) == key { weight(order) } else { 0 };
        let factor = line_count(db, key, mode, day);
        assert forall|order: OrdersRow| #[trigger] scaled(order) == factor * indicator(order) by {}
        lemma_sum_scale(db.orders, indicator, scaled, factor);
        assert(order_count(db, key, high) * factor == factor * order_count(db, key, high)) by (nonlinear_arith);
    }
    lemma_sum_group(db.orders, keys, key_fn, weight, per);
}

pub proof fn lemma_global_delta(db: Db, post: Db, rows: Seq<OrderDelta>, full: Seq<(OrdersRow, int)>,
    lines: Seq<LineDelta>, keys: Seq<i64>, mode: Seq<u8>, day: int, high: bool)
    requires orders_delta_ok(rows, full), capture_ok(db, post, line_delta_seq_view(lines), full), keys.no_duplicates(),
        forall|key: i64| #![trigger keys.contains(key)] !keys.contains(key) ==>
            moments::order_change(rows, key, high) == 0
                && (i32::MIN <= day <= i32::MAX ==> moments::line_change(lines, key, mode, day as i32) == 0),
    ensures cell_count(post, mode, day, high) - cell_count(db, mode, day, high)
        == sum(keys, |key: i64| effect(db, post, key as int, mode, day, high)),
{
    let source = db.orders.map_values(|order: OrdersRow| order.o_orderkey)
        + post.orders.map_values(|order: OrdersRow| order.o_orderkey) + seq_view_i64(keys);
    let universe = dedup(source);
    lemma_dedup(source);
    assert(distinct(universe));
    assert(distinct(keys));
    assert forall|index: int| #![trigger db.orders[index]] 0 <= index < db.orders.len() implies universe.contains(db.orders[index].o_orderkey) by {
        assert(source[index] == db.orders[index].o_orderkey);
    }
    assert forall|index: int| #![trigger post.orders[index]] 0 <= index < post.orders.len() implies universe.contains(post.orders[index].o_orderkey) by {
        assert(source[db.orders.len() as int + index] == post.orders[index].o_orderkey);
    }
    let old_weight = |key: int| order_count(db, key, high) * line_count(db, key, mode, day);
    let new_weight = |key: int| order_count(post, key, high) * line_count(post, key, mode, day);
    let delta = |key: int| effect(db, post, key, mode, day, high);
    lemma_factor(db, universe, mode, day, high);
    lemma_factor(post, universe, mode, day, high);
    lemma_sum_sub(universe, new_weight, old_weight, delta);
    assert forall|key: int| #[trigger] delta(key) != 0 implies i64::MIN <= key <= i64::MAX && keys.contains(key as i64) by {
        lemma_capture_orders(db, post, rows, full, key, high);
        lemma_capture_lines(db, post, lines, key, mode, day);
        if !(i64::MIN <= key <= i64::MAX) || !keys.contains(key as i64) {
            assert(order_count(post, key, high) == order_count(db, key, high));
            assert(line_count(post, key, mode, day) == line_count(db, key, mode, day));
        }
    }
    let per = |key: i64| effect(db, post, key as int, mode, day, high);
    let key_fn = |key: int| key as i64;
    assert forall|key: i64| #[trigger] per(key) == sum(universe, |value: int| if key_fn(value) == key { delta(value) } else { 0 }) by {
        let selected = |value: int| if value == key as int { delta(key as int) } else { 0 };
        if keys.contains(key) {
            lemma_key_view(keys, key);
            assert(source.contains(key as int));
            assert(universe.contains(key as int));
        } else { assert(delta(key as int) == 0); }
        lemma_sum_indicator_distinct(universe, key as int, delta(key as int), selected);
        assert forall|value: int| #[trigger] selected(value) == if value as i64 == key { delta(value) } else { 0 } by {}
        lemma_sum_ext_all(universe, selected, |value: int| if key_fn(value) == key { delta(value) } else { 0 });
    }
    lemma_sum_group(universe, keys, key_fn, delta, per);
}

}

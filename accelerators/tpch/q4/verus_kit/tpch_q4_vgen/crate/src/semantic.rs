use vstd::prelude::*;
use verus_common::sums::*;
use verus_common::rel::*;
use verus_common::bytes::*;
use verus_prooflib::wide::Wide;
use crate::gen_spec::*;
use crate::gen_obligations::*;
use crate::rollup::*;
use crate::maintenance::*;

verus! {

broadcast use vstd::seq_lib::group_seq_properties;

pub open spec fn build_weight(row: DatePriorityInput, key: Key, priority: Seq<u8>) -> int {
    if key_height(key) <= 32 && in_block(row.o_orderdate as int, key) && row.o_orderpriority@ == priority { 1 } else { 0 }
}

pub proof fn lemma_build_meaning(db: Db, rows: Seq<DatePriorityInput>, key: Key, priority: Seq<u8>)
    requires scan_qualifying_orders_ok(db, rows),
    ensures sum(rows, |row: DatePriorityInput| build_weight(row, key, priority))
        == if key_height(key) <= 32 { db_count(db, key, priority) } else { 0 },
{
    let weight = |value: (int, Seq<u8>)|
        if key_height(key) <= 32 && in_block(value.0, key) && value.1 == priority { 1int } else { 0int };
    lemma_sum_map(rows, |row: DatePriorityInput| date_priority_input_view(row), weight,
        |row: DatePriorityInput| build_weight(row, key, priority));
    assert(sum(date_priority_input_seq_view(rows), weight) == sum(db.orders,
        |order: OrdersRow| if scan_qualifying_orders_exists_1(db, order) { weight((order.o_orderdate, order.o_orderpriority)) } else { 0 }));
    let rhs = |order: OrdersRow| if scan_qualifying_orders_exists_1(db, order) { weight((order.o_orderdate, order.o_orderpriority)) } else { 0 };
    if key_height(key) <= 32 {
        let target = |order: OrdersRow| if in_block(order.o_orderdate, key) && order.o_orderpriority == priority && exists_1(db, order) { 1int } else { 0int };
        assert forall|order: OrdersRow| #[trigger] rhs(order) == target(order) by {
            assert(scan_qualifying_orders_exists_1(db, order) == exists_1(db, order));
        }
        lemma_sum_ext_all(db.orders, rhs, target);
    } else {
        lemma_sum_zero(db.orders, rhs);
    }
}

pub open spec fn window_count(db: Db, lower: int, upper: int, priority: Seq<u8>) -> int {
    sum(db.orders, |order: OrdersRow| if lower <= order.o_orderdate + 2147483648 < upper
        && order.o_orderpriority == priority && exists_1(db, order) { 1 } else { 0 })
}

pub proof fn lemma_window_empty(db: Db, lower: int, upper: int, priority: Seq<u8>)
    requires upper <= lower,
    ensures window_count(db, lower, upper, priority) == 0,
{
    lemma_sum_zero(db.orders, |order: OrdersRow| if lower <= order.o_orderdate + 2147483648 < upper
        && order.o_orderpriority == priority && exists_1(db, order) { 1int } else { 0int });
}

pub proof fn lemma_window_step(db: Db, start: int, cursor: int, end: int, key: Key, priority: Seq<u8>)
    requires start <= cursor <= end, cursor == key_bucket(key) * block_width(key_height(key)), end == (key_bucket(key) + 1) * block_width(key_height(key)),
    ensures window_count(db, start, end, priority) == window_count(db, start, cursor, priority) + db_count(db, key, priority),
{
    let before = |order: OrdersRow| if start <= order.o_orderdate + 2147483648 < cursor
        && order.o_orderpriority == priority && exists_1(db, order) { 1int } else { 0int };
    let block = |order: OrdersRow| if in_block(order.o_orderdate, key)
        && order.o_orderpriority == priority && exists_1(db, order) { 1int } else { 0int };
    let after = |order: OrdersRow| if start <= order.o_orderdate + 2147483648 < end
        && order.o_orderpriority == priority && exists_1(db, order) { 1int } else { 0int };
    lemma_sum_add(db.orders, before, block, after);
}

pub proof fn lemma_query_count(db: Db, parameters: Params, priority: Seq<u8>)
    ensures group_count(db, parameters, priority)
        == window_count(db, parameters.p1 + 2147483648, parameters.p2 + 2147483648, priority),
{
    let mapper = |order: OrdersRow| tuples_1(db, parameters, order);
    let weight = |tuple: Tuple| if tuple.orders.o_orderpriority == priority { 1int } else { 0int };
    lemma_sum_flat_map(db.orders, mapper, weight);
    let nested = |order: OrdersRow| sum(mapper(order), weight);
    let target = |order: OrdersRow| if parameters.p1 + 2147483648 <= order.o_orderdate + 2147483648 < parameters.p2 + 2147483648
        && order.o_orderpriority == priority && exists_1(db, order) { 1int } else { 0int };
    assert forall|order: OrdersRow| #[trigger] nested(order) == target(order) by {
        lemma_sum_opt(where_clause(db, parameters, order), Tuple { orders: order }, weight);
    }
    lemma_sum_ext_all(db.orders, nested, target);
}

pub proof fn lemma_output_meaning(db: Db, parameters: Params, bucket: Seq<(Vec<u8>, Wide)>, out: Seq<(Vec<u8>, i64)>)
    requires
        forall|priority: Seq<u8>| #[trigger] bucket_count(bucket, priority) == group_count(db, parameters, priority),
        forall|index: int| #![trigger out[index]] 0 <= index < out.len() ==>
            out[index].1 > 0 && out[index].1 as int == bucket_count(bucket, out[index].0@),
        forall|priority: Seq<u8>| #[trigger] bucket_count(bucket, priority) > 0 ==>
            exists|index: int| #![trigger out[index]] 0 <= index < out.len() && out[index].0@ == priority,
        forall|left: int, right: int| #![trigger out[left], out[right]] 0 <= left < right < out.len() ==>
            bytes_lt(out[left].0@, out[right].0@),
    ensures query_result(db, parameters, out_view(out)),
{
    let rows = out_view(out);
    assert forall|index: int| 0 <= index < rows.len() implies row_ok(db, parameters, #[trigger] rows[index]) by {
        assert(rows[index].o_orderpriority == out[index].0@);
    }
    assert forall|left: int, right: int| 0 <= left < right < rows.len() implies
        !same_group(#[trigger] rows[left], #[trigger] rows[right]) && !row_lt(rows[right], rows[left]) by {
        assert(rows[left].o_orderpriority == out[left].0@);
        assert(rows[right].o_orderpriority == out[right].0@);
        lemma_bytes_lt_irrefl(out[left].0@);
        lemma_bytes_lt_asym(out[left].0@, out[right].0@);
    }
    assert forall|priority: Seq<u8>| #[trigger] group_count(db, parameters, priority) > 0 && having(db, parameters, priority)
        implies exists|index: int| 0 <= index < rows.len() && (#[trigger] rows[index]).o_orderpriority == priority by {
        assert(bucket_count(bucket, priority) > 0);
        assert(exists|index: int| #![trigger out[index]] 0 <= index < out.len() && out[index].0@ == priority);
        let index = choose|index: int| 0 <= index < out.len() && out[index].0@ == priority;
        assert(rows.len() == out.len());
        assert(rows[index] == OutRow { o_orderpriority: out[index].0@, order_count: out[index].1 as int });
        assert(rows[index].o_orderpriority == priority);
    }
}

pub proof fn lemma_exists_count(db: Db, key: int)
    ensures witness_count(db, key) >= 0,
        witness_count(db, key) > 0 <==> exists|index: int| #![trigger db.lineitem[index]]
            0 <= index < db.lineitem.len() && db.lineitem[index].l_orderkey == key
                && db.lineitem[index].l_commitdate < db.lineitem[index].l_receiptdate,
{
    let weight = |row: LineitemRow| if row.l_orderkey == key && row.l_commitdate < row.l_receiptdate { 1int } else { 0int };
    lemma_sum_nonneg(db.lineitem, weight);
    if witness_count(db, key) > 0 {
        let index = lemma_sum_nonzero_witness(db.lineitem, weight);
        assert(weight(db.lineitem[index]) == 1);
    } else if exists|index: int| #![trigger db.lineitem[index]] 0 <= index < db.lineitem.len()
        && db.lineitem[index].l_orderkey == key && db.lineitem[index].l_commitdate < db.lineitem[index].l_receiptdate {
        let index = choose|index: int| 0 <= index < db.lineitem.len()
            && db.lineitem[index].l_orderkey == key && db.lineitem[index].l_commitdate < db.lineitem[index].l_receiptdate;
        lemma_sum_pos(db.lineitem, weight, index);
    }
}

pub proof fn lemma_capture_witness(db: Db, next: Db, lines: Seq<LineDelta>, key: int)
    requires bag_step(db.lineitem, next.lineitem, line_delta_seq_view(lines)),
    ensures witness_count(next, key) == witness_count(db, key) + witness_delta(lines, key),
{
    let weight = |row: LineitemRow| if row.l_orderkey == key && row.l_commitdate < row.l_receiptdate { 1int } else { 0int };
    let signed = |delta: (LineitemRow, int)| delta.1 * weight(delta.0);
    assert(sum(next.lineitem, weight) == sum(db.lineitem, weight) + sum(line_delta_seq_view(lines), signed));
    lemma_sum_map(lines, |delta: LineDelta| (line_input_view(delta.row), delta.sign as int), signed,
        |delta: LineDelta| if delta.row.l_orderkey == key && delta.row.l_commitdate < delta.row.l_receiptdate { delta.sign as int } else { 0 });
}

pub proof fn lemma_rescan_witness(db: Db, keys: Seq<i64>, rows: Seq<QualifyingLineKeyInput>, key: i64)
    requires rescan_qualifying_lines_for_keys_ok(db, keys, rows), keys.contains(key),
    ensures post_witness(rows, key) == witness_count(db, key as int),
{
    let index = choose|index: int| 0 <= index < keys.len() && keys[index] == key;
    assert(seq_view_i64(keys)[index] == key as int);
    assert(seq_view_i64(keys).contains(key as int));
    let weight = |value: int| if value == key as int { 1int } else { 0int };
    assert(sum(qualifying_line_key_input_seq_view(rows), weight) == sum(db.lineitem,
        |row: LineitemRow| if row.l_commitdate < row.l_receiptdate && seq_view_i64(keys).contains(row.l_orderkey) { weight(row.l_orderkey) } else { 0 }));
    lemma_sum_map(rows, |row: QualifyingLineKeyInput| qualifying_line_key_input_view(row), weight,
        |row: QualifyingLineKeyInput| if row.l_orderkey == key { 1int } else { 0int });
    lemma_sum_ext_all(db.lineitem,
        |row: LineitemRow| if row.l_commitdate < row.l_receiptdate && seq_view_i64(keys).contains(row.l_orderkey) { weight(row.l_orderkey) } else { 0 },
        |row: LineitemRow| if row.l_orderkey == key && row.l_commitdate < row.l_receiptdate { 1int } else { 0int });
}

pub proof fn lemma_previous_witness(db: Db, next: Db, lines: Seq<LineDelta>, keys: Seq<i64>, rows: Seq<QualifyingLineKeyInput>, key: i64)
    requires bag_step(db.lineitem, next.lineitem, line_delta_seq_view(lines)),
        rescan_qualifying_lines_for_keys_ok(next, keys, rows), keys.contains(key),
    ensures previous_witness(lines, rows, key) == witness_count(db, key as int),
        post_witness(rows, key) == witness_count(next, key as int),
{
    lemma_capture_witness(db, next, lines, key as int);
    lemma_rescan_witness(next, keys, rows, key);
}

pub proof fn lemma_unchanged_witness(db: Db, next: Db, lines: Seq<LineDelta>, key: int)
    requires bag_step(db.lineitem, next.lineitem, line_delta_seq_view(lines)), !line_touched(lines, key),
    ensures witness_count(db, key) == witness_count(next, key),
{
    lemma_capture_witness(db, next, lines, key);
    lemma_sum_zero(lines, |delta: LineDelta|
        if delta.row.l_orderkey == key && delta.row.l_commitdate < delta.row.l_receiptdate { delta.sign as int } else { 0 });
}

pub open spec fn old_weight(db: Db, order: OrdersRow, key: Key, priority: Seq<u8>) -> int {
    if in_block(order.o_orderdate, key) && order.o_orderpriority == priority && exists_1(db, order) { 1 } else { 0 }
}

pub open spec fn difference_weight(db: Db, next: Db, order: OrdersRow, key: Key, priority: Seq<u8>) -> int {
    if in_block(order.o_orderdate, key) && order.o_orderpriority == priority {
        (if exists_1(next, order) { 1int } else { 0int }) - (if exists_1(db, order) { 1int } else { 0int })
    } else { 0 }
}

pub proof fn lemma_apply_meaning(db: Db, next: Db, lines: Seq<LineDelta>, orders: Seq<OrderDelta>,
    order_keys: Seq<i64>, order_rows: Seq<OrderInput>, line_keys: Seq<i64>, line_rows: Seq<QualifyingLineKeyInput>, key: Key, priority: Seq<u8>)
    requires capture_ok(db, next, line_delta_seq_view(lines), order_delta_seq_view(orders)),
        rescan_orders_for_line_changes_ok(next, order_keys, order_rows),
        rescan_qualifying_lines_for_keys_ok(next, line_keys, line_rows),
        coverage(lines, orders, order_keys, line_keys),
    ensures (if key_height(key) <= 32 { db_count(next, key, priority) } else { 0 })
        == (if key_height(key) <= 32 { db_count(db, key, priority) } else { 0 })
            + change_count(lines, orders, order_rows, line_rows, key, priority),
{
    let order_change = |delta: OrderDelta| order_change_weight(lines, line_rows, key, priority, delta);
    let line_change = |row: OrderInput| line_change_weight(lines, line_rows, key, priority, row);
    assert(change_count(lines, orders, order_rows, line_rows, key, priority) == sum(orders, order_change) + sum(order_rows, line_change));
    if key_height(key) > 32 {
        lemma_sum_zero(orders, order_change);
        lemma_sum_zero(order_rows, line_change);
        return;
    }
    let before = |order: OrdersRow| old_weight(db, order, key, priority);
    let diff = |order: OrdersRow| difference_weight(db, next, order, key, priority);
    let after = |order: OrdersRow| old_weight(next, order, key, priority);
    lemma_sum_add(next.orders, before, diff, after);
    let signed = |delta: (OrdersRow, int)| delta.1 * before(delta.0);
    assert(sum(next.orders, before) == sum(db.orders, before) + sum(order_delta_seq_view(orders), signed));
    let runtime_signed = |delta: OrderDelta| signed((order_input_view(delta.row), delta.sign as int));
    lemma_sum_map(orders, |delta: OrderDelta| (order_input_view(delta.row), delta.sign as int), signed, runtime_signed);
    assert forall|index: int| 0 <= index < orders.len() implies runtime_signed(#[trigger] orders[index]) == order_change(orders[index]) by {
        let row = orders[index].row;
        lemma_previous_witness(db, next, lines, line_keys, line_rows, row.o_orderkey);
        lemma_exists_count(db, row.o_orderkey as int);
    }
    lemma_sum_ext(orders, runtime_signed, order_change);
    let restricted = |order: OrdersRow| if seq_view_i64(order_keys).contains(order.o_orderkey) { diff(order) } else { 0 };
    assert(sum(order_input_seq_view(order_rows), diff) == sum(next.orders, restricted));
    let runtime_diff = |row: OrderInput| diff(order_input_view(row));
    lemma_sum_map(order_rows, |row: OrderInput| order_input_view(row), diff, runtime_diff);
    assert forall|order: OrdersRow| #[trigger] restricted(order) == diff(order) by {
        if !seq_view_i64(order_keys).contains(order.o_orderkey) {
            assert(!line_touched(lines, order.o_orderkey)) by {
                if line_touched(lines, order.o_orderkey) {
                    let index = choose|index: int| 0 <= index < lines.len() && lines[index].row.l_orderkey == order.o_orderkey
                        && lines[index].row.l_commitdate < lines[index].row.l_receiptdate;
                    let runtime_key = lines[index].row.l_orderkey;
                    let source = choose|source: int| 0 <= source < order_keys.len() && order_keys[source] == runtime_key;
                    assert(seq_view_i64(order_keys)[source] == order.o_orderkey);
                }
            }
            lemma_unchanged_witness(db, next, lines, order.o_orderkey);
            lemma_exists_count(db, order.o_orderkey);
            lemma_exists_count(next, order.o_orderkey);
        }
    }
    lemma_sum_ext_all(next.orders, restricted, diff);
    assert forall|index: int| 0 <= index < order_rows.len() implies runtime_diff(#[trigger] order_rows[index]) == line_change(order_rows[index]) by {
        let row = order_rows[index];
        if line_touched(lines, row.o_orderkey as int) {
            let source = choose|source: int| 0 <= source < lines.len() && lines[source].row.l_orderkey == row.o_orderkey
                && lines[source].row.l_commitdate < lines[source].row.l_receiptdate;
            lemma_previous_witness(db, next, lines, line_keys, line_rows, row.o_orderkey);
        } else { lemma_unchanged_witness(db, next, lines, row.o_orderkey as int); }
        lemma_exists_count(db, row.o_orderkey as int);
        lemma_exists_count(next, row.o_orderkey as int);
    }
    lemma_sum_ext(order_rows, runtime_diff, line_change);
    lemma_sum_ext_all(db.orders, before,
        |order: OrdersRow| if in_block(order.o_orderdate, key) && order.o_orderpriority == priority && exists_1(db, order) { 1int } else { 0int });
    lemma_sum_ext_all(next.orders, after,
        |order: OrdersRow| if in_block(order.o_orderdate, key) && order.o_orderpriority == priority && exists_1(next, order) { 1int } else { 0int });
}


pub proof fn lemma_indicator_sum_range<A>(rows: Seq<A>, weight: spec_fn(A) -> int)
    requires forall|row: A| 0 <= #[trigger] weight(row) <= 1,
    ensures 0 <= sum(rows, weight) <= rows.len(),
    decreases rows.len(),
{
    if rows.len() > 0 { lemma_indicator_sum_range(rows.drop_last(), weight); }
}

pub proof fn lemma_window_range(db: Db, lower: int, upper: int, priority: Seq<u8>)
    ensures 0 <= window_count(db, lower, upper, priority) <= db.orders.len(),
{
    lemma_indicator_sum_range(db.orders, |order: OrdersRow| if lower <= order.o_orderdate + 2147483648 < upper
        && order.o_orderpriority == priority && exists_1(db, order) { 1int } else { 0int });
}

pub proof fn lemma_db_count_range(db: Db, key: Key, priority: Seq<u8>)
    ensures 0 <= db_count(db, key, priority) <= db.orders.len(),
{
    lemma_indicator_sum_range(db.orders, |order: OrdersRow|
        if in_block(order.o_orderdate, key) && order.o_orderpriority == priority && exists_1(db, order) { 1int } else { 0int });
}

/// Every count of a database within the envelope fits.
pub proof fn lemma_envelope_counts_fit(db: Db, key: Key, priority: Seq<u8>)
    requires envelope(db),
    ensures count_fits(if key_height(key) <= 32 { db_count(db, key, priority) } else { 0 }),
{
    if key_height(key) <= 32 { lemma_db_count_range(db, key, priority); }
}


/// A group whose count exceeds the result's i64 means no answer fits.
pub proof fn lemma_overflow_not_answer_fits(db: Db, parameters: Params, priority: Seq<u8>)
    requires group_count(db, parameters, priority) > i64::MAX,
    ensures !answer_fits(db, parameters),
{
    lemma_query_result_iff(db, parameters, answer(db, parameters));
    let answered = answer(db, parameters);
    assert(query_result(db, parameters, answered));
    let index = choose|index: int| 0 <= index < answered.len() && answered[index].o_orderpriority == priority;
    assert(answered[index].order_count == group_count(db, parameters, priority));
}

}

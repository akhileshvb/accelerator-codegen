use vstd::prelude::*;
use std::collections::HashMap;
use verus_common::sums::*;
use crate::gen_obligations::*;
use crate::gen_spec::*;
use crate::rollup::*;

verus! {

broadcast use vstd::seq_lib::group_seq_properties;

pub open spec fn witness_count(db: Db, key: int) -> int {
    sum(db.lineitem, |line: LineitemRow| if line.l_orderkey == key && line.l_commitdate < line.l_receiptdate { 1 } else { 0 })
}

pub open spec fn witness_delta(rows: Seq<LineDelta>, key: int) -> int {
    sum(rows, |delta: LineDelta| if delta.row.l_orderkey == key && delta.row.l_commitdate < delta.row.l_receiptdate { delta.sign as int } else { 0 })
}

pub open spec fn map_count(map: Map<i64, i128>, key: i64) -> int {
    if map.contains_key(key) { map[key] as int } else { 0 }
}

pub open spec fn line_touched(lines: Seq<LineDelta>, key: int) -> bool {
    exists|index: int| #![trigger lines[index]] 0 <= index < lines.len()
        && lines[index].row.l_orderkey == key
        && lines[index].row.l_commitdate < lines[index].row.l_receiptdate
}

pub open spec fn post_witness(lines: Seq<QualifyingLineKeyInput>, key: i64) -> int {
    sum(lines, |row: QualifyingLineKeyInput| if row.l_orderkey == key { 1int } else { 0int })
}

pub open spec fn previous_witness(lines: Seq<LineDelta>, rescanned: Seq<QualifyingLineKeyInput>, key: i64) -> int {
    post_witness(rescanned, key) - witness_delta(lines, key as int)
}

pub open spec fn order_change_weight(lines: Seq<LineDelta>, line_rows: Seq<QualifyingLineKeyInput>, key: Key, priority: Seq<u8>, delta: OrderDelta) -> int {
    if key_height(key) <= 32 && in_block(delta.row.o_orderdate as int, key) && delta.row.o_orderpriority@ == priority
        && previous_witness(lines, line_rows, delta.row.o_orderkey) > 0 { delta.sign as int } else { 0 }
}

pub open spec fn line_change_weight(lines: Seq<LineDelta>, line_rows: Seq<QualifyingLineKeyInput>, key: Key, priority: Seq<u8>, row: OrderInput) -> int {
    if key_height(key) <= 32 && in_block(row.o_orderdate as int, key) && row.o_orderpriority@ == priority && line_touched(lines, row.o_orderkey as int)
        { (if post_witness(line_rows, row.o_orderkey) > 0 { 1int } else { 0int })
            - (if previous_witness(lines, line_rows, row.o_orderkey) > 0 { 1int } else { 0int }) } else { 0 }
}

pub open spec fn change_count(lines: Seq<LineDelta>, orders: Seq<OrderDelta>,
    order_rows: Seq<OrderInput>, line_rows: Seq<QualifyingLineKeyInput>, key: Key, priority: Seq<u8>) -> int {
    sum(orders, |delta: OrderDelta| order_change_weight(lines, line_rows, key, priority, delta))
        + sum(order_rows, |row: OrderInput| line_change_weight(lines, line_rows, key, priority, row))
}

pub open spec fn maps_ready(lines: Seq<LineDelta>, orders: Seq<OrderDelta>, rows: Seq<QualifyingLineKeyInput>,
    after: Map<i64, i128>, before: Map<i64, i128>, touched: Map<i64, bool>) -> bool {
    &&& forall|key: i64| #[trigger] map_count(after, key) == post_witness(rows, key)
    &&& forall|key: i64| #[trigger] touched.contains_key(key) <==> line_touched(lines, key as int)
    &&& forall|index: int| #![trigger orders[index]] 0 <= index < orders.len() ==>
        map_count(before, orders[index].row.o_orderkey) == previous_witness(lines, rows, orders[index].row.o_orderkey)
    &&& forall|key: i64| #[trigger] touched.contains_key(key) ==>
        map_count(before, key) == previous_witness(lines, rows, key)
}

pub fn bump(map: &mut HashMap<i64, i128>, key: i64, amount: i128) -> (ok: bool)
    ensures ok ==> forall|other: i64| #[trigger] map_count(final(map)@, other)
        == map_count(old(map)@, other) + if other == key { amount as int } else { 0 },
{
    let before = match map.get(&key) { Some(count) => *count, None => 0 };
    match before.checked_add(amount) {
        Some(after) => { map.insert(key, after); true },
        None => false,
    }
}

pub fn get_count(map: &HashMap<i64, i128>, key: i64) -> (out: i128)
    ensures out as int == map_count(map@, key),
{
    match map.get(&key) { Some(count) => *count, None => 0 }
}

pub fn support(lines: &[LineDelta], orders: &Vec<OrderDelta>, include_orders: bool) -> (out: Vec<i64>)
    ensures
        forall|index: int| #![trigger lines@[index]] 0 <= index < lines@.len()
            && lines@[index].row.l_commitdate < lines@[index].row.l_receiptdate ==>
                out@.contains(lines@[index].row.l_orderkey),
        include_orders ==> forall|index: int| #![trigger orders@[index]]
            0 <= index < orders@.len() ==> out@.contains(orders@[index].row.o_orderkey),
{
    let mut seen: HashMap<i64, bool> = HashMap::new();
    let mut out = Vec::new();
    let mut index = 0;
    let mut frontier: Ghost<int> = Ghost(0);
    while index < lines.len()
        invariant index <= lines.len(), frontier@ == index as int,
            forall|key: i64| #[trigger] seen@.contains_key(key) <==> out@.contains(key),
            forall|position: int| #![trigger lines@[position]] 0 <= position < index
                && lines@[position].row.l_commitdate < lines@[position].row.l_receiptdate ==>
                    out@.contains(lines@[position].row.l_orderkey),
        decreases lines.len() - index,
    {
        let row = &lines[index].row;
        if row.l_commitdate < row.l_receiptdate && !seen.contains_key(&row.l_orderkey) {
            seen.insert(row.l_orderkey, true);
            out.push(row.l_orderkey);
        }
        index += 1;
        frontier = Ghost(index as int);
    }
    if include_orders {
        index = 0;
        frontier = Ghost(0);
        while index < orders.len()
            invariant index <= orders.len(), frontier@ == index as int,
                forall|key: i64| #[trigger] seen@.contains_key(key) <==> out@.contains(key),
                forall|position: int| #![trigger lines@[position]] 0 <= position < lines@.len()
                    && lines@[position].row.l_commitdate < lines@[position].row.l_receiptdate ==>
                        out@.contains(lines@[position].row.l_orderkey),
                forall|position: int| #![trigger orders@[position]]
                    0 <= position < index ==> out@.contains(orders@[position].row.o_orderkey),
            decreases orders.len() - index,
        {
            let key = orders[index].row.o_orderkey;
            if !seen.contains_key(&key) { seen.insert(key, true); out.push(key); }
            index += 1;
            frontier = Ghost(index as int);
        }
    }
    out
}

pub fn key_set(keys: &[i64]) -> (out: HashMap<i64, bool>)
    ensures forall|key: i64| #[trigger] out@.contains_key(key) <==> keys@.contains(key),
{
    let mut out = HashMap::new();
    let mut index = 0;
    while index < keys.len()
        invariant index <= keys.len(),
            forall|key: i64| #[trigger] out@.contains_key(key) <==> keys@.take(index as int).contains(key),
        decreases keys.len() - index,
    {
        proof { assert(keys@.take(index as int + 1) =~= keys@.take(index as int).push(keys@[index as int])); }
        out.insert(keys[index], true);
        index += 1;
    }
    out
}

pub fn changes(
    lines: &[LineDelta], orders: &Vec<OrderDelta>,
    order_keys: &[i64], order_rows: &Vec<OrderInput>,
    line_keys: &[i64], line_rows: &[QualifyingLineKeyInput],
) -> (out: Option<Patch>)
    ensures out matches Some(patch) ==> patch_valid(patch)
        && forall|key: Key, priority: Seq<u8>| #[trigger] patch_count(patch, key, priority)
            == change_count(lines@, orders@, order_rows@, line_rows@, key, priority),
        out is Some ==> coverage(lines@, orders@, order_keys@, line_keys@),
{
    let order_set = key_set(order_keys);
    let line_set = key_set(line_keys);
    let mut deltas: HashMap<i64, i128> = HashMap::new();
    let mut after: HashMap<i64, i128> = HashMap::new();
    let mut line_support: HashMap<i64, bool> = HashMap::new();
    let mut index = 0;
    let mut frontier: Ghost<int> = Ghost(0);
    while index < lines.len()
        invariant index <= lines.len(), frontier@ == index as int,
            forall|key: i64| #[trigger] map_count(deltas@, key)
                == witness_delta(lines@.take(frontier@), key as int),
            forall|key: i64| #[trigger] line_support@.contains_key(key) <==> line_touched(lines@.take(frontier@), key as int),
            forall|position: int| #![trigger lines@[position]] 0 <= position < index
                && lines@[position].row.l_commitdate < lines@[position].row.l_receiptdate ==>
                    order_keys@.contains(lines@[position].row.l_orderkey) && line_keys@.contains(lines@[position].row.l_orderkey),
            forall|key: i64| #[trigger] line_set@.contains_key(key) <==> line_keys@.contains(key),
            forall|key: i64| #[trigger] order_set@.contains_key(key) <==> order_keys@.contains(key),
        decreases lines.len() - index,
    {
        let delta = &lines[index];
        if delta.row.l_commitdate < delta.row.l_receiptdate {
            let key = delta.row.l_orderkey;
            if !line_set.contains_key(&key) || !order_set.contains_key(&key) { return None; }
            line_support.insert(key, true);
            if !bump(&mut deltas, key, delta.sign as i128) { return None; }
        }
        proof {
            assert forall|key: i64| #[trigger] map_count(deltas@, key)
                == witness_delta(lines@.take(index as int + 1), key as int) by {
                lemma_sum_take(lines@, index as int, |delta: LineDelta|
                    if delta.row.l_orderkey == key && delta.row.l_commitdate < delta.row.l_receiptdate { delta.sign as int } else { 0 });
            }
            assert forall|key: i64| #[trigger] line_support@.contains_key(key) <==> line_touched(lines@.take(index as int + 1), key as int) by {
                if line_touched(lines@.take(index as int + 1), key as int) {
                    let source = choose|source: int| #![trigger lines@.take(index as int + 1)[source]] 0 <= source < index as int + 1
                        && lines@.take(index as int + 1)[source].row.l_orderkey == key
                        && lines@.take(index as int + 1)[source].row.l_commitdate < lines@.take(index as int + 1)[source].row.l_receiptdate;
                    if source < index {
                        assert(lines@.take(index as int)[source] == lines@[source]);
                        assert(line_touched(lines@.take(index as int), key as int));
                    }
                }
                if line_support@.contains_key(key) {
                    if delta.row.l_commitdate < delta.row.l_receiptdate && key == delta.row.l_orderkey {
                        assert(lines@.take(index as int + 1)[index as int] == lines@[index as int]);
                    } else {
                        assert(line_touched(lines@.take(index as int), key as int));
                        let source = choose|source: int| #![trigger lines@.take(index as int)[source]] 0 <= source < index
                            && lines@.take(index as int)[source].row.l_orderkey == key
                            && lines@.take(index as int)[source].row.l_commitdate < lines@.take(index as int)[source].row.l_receiptdate;
                        assert(lines@.take(index as int + 1)[source] == lines@[source]);
                    }
                }
            }
        }
        index += 1;
        frontier = Ghost(index as int);
    }
    index = 0;
    frontier = Ghost(0);
    while index < line_rows.len()
        invariant index <= line_rows.len(), frontier@ == index as int,
            forall|key: i64| #[trigger] map_count(after@, key)
                == sum(line_rows@.take(frontier@), |row: QualifyingLineKeyInput| if row.l_orderkey == key { 1int } else { 0int }),
            forall|key: i64| #[trigger] map_count(deltas@, key) == witness_delta(lines@, key as int),
            forall|key: i64| #[trigger] line_support@.contains_key(key) <==> line_touched(lines@, key as int),
            forall|position: int| #![trigger lines@[position]] 0 <= position < lines@.len()
                && lines@[position].row.l_commitdate < lines@[position].row.l_receiptdate ==>
                    order_keys@.contains(lines@[position].row.l_orderkey) && line_keys@.contains(lines@[position].row.l_orderkey),
            forall|key: i64| #[trigger] line_set@.contains_key(key) <==> line_keys@.contains(key),
        decreases line_rows.len() - index,
    {
        let key = line_rows[index].l_orderkey;
        if !line_set.contains_key(&key) || !bump(&mut after, key, 1) { return None; }
        proof {
            assert forall|other: i64| #[trigger] map_count(after@, other)
                == sum(line_rows@.take(index as int + 1), |row: QualifyingLineKeyInput| if row.l_orderkey == other { 1int } else { 0int }) by {
                lemma_sum_take(line_rows@, index as int, |row: QualifyingLineKeyInput| if row.l_orderkey == other { 1int } else { 0int });
            }
        }
        index += 1;
        frontier = Ghost(index as int);
    }
    let needed = support(lines, orders, true);
    let mut before: HashMap<i64, i128> = HashMap::new();
    index = 0;
    while index < needed.len()
        invariant index <= needed.len(),
            forall|key: i64| #[trigger] map_count(deltas@, key) == witness_delta(lines@, key as int),
            forall|key: i64| #[trigger] map_count(after@, key) == post_witness(line_rows@, key),
            forall|key: i64| #[trigger] line_support@.contains_key(key) <==> line_touched(lines@, key as int),
            forall|position: int| #![trigger lines@[position]] 0 <= position < lines@.len()
                && lines@[position].row.l_commitdate < lines@[position].row.l_receiptdate ==>
                    order_keys@.contains(lines@[position].row.l_orderkey) && line_keys@.contains(lines@[position].row.l_orderkey)
                        && needed@.contains(lines@[position].row.l_orderkey),
            forall|position: int| #![trigger orders@[position]] 0 <= position < orders@.len() ==> needed@.contains(orders@[position].row.o_orderkey),
            forall|key: i64| #[trigger] line_set@.contains_key(key) <==> line_keys@.contains(key),
            forall|key: i64| #[trigger] needed@.take(index as int).contains(key) ==>
                map_count(before@, key) == previous_witness(lines@, line_rows@, key) && line_keys@.contains(key),
        decreases needed.len() - index,
    {
        let key = needed[index];
        if !line_set.contains_key(&key) { return None; }
        let previous = match get_count(&after, key).checked_sub(get_count(&deltas, key)) {
            Some(value) => value,
            None => return None,
        };
        if previous < 0 { return None; }
        before.insert(key, previous);
        proof { assert(needed@.take(index as int + 1) =~= needed@.take(index as int).push(key)); }
        index += 1;
    }
    proof {
        assert(needed@.take(needed@.len() as int) =~= needed@);
        assert(coverage(lines@, orders@, order_keys@, line_keys@));
        assert(maps_ready(lines@, orders@, line_rows@, after@, before@, line_support@)) by {
            assert forall|key: i64| #[trigger] line_support@.contains_key(key) implies
                map_count(before@, key) == previous_witness(lines@, line_rows@, key) by {
                let source = choose|source: int| 0 <= source < lines@.len() && lines@[source].row.l_orderkey == key
                    && lines@[source].row.l_commitdate < lines@[source].row.l_receiptdate;
                assert(needed@.contains(key));
            }
        }
    }
    let mut patch = empty_patch();
    index = 0;
    frontier = Ghost(0);
    while index < orders.len()
        invariant index <= orders.len(), frontier@ == index as int, patch_valid(patch),
            maps_ready(lines@, orders@, line_rows@, after@, before@, line_support@),
            coverage(lines@, orders@, order_keys@, line_keys@),
            forall|key: Key, priority: Seq<u8>| #[trigger] patch_count(patch, key, priority)
                == sum(orders@.take(frontier@), |delta: OrderDelta|
                    if key_height(key) <= 32 && in_block(delta.row.o_orderdate as int, key)
                        && delta.row.o_orderpriority@ == priority && map_count(before@, delta.row.o_orderkey) > 0
                    { delta.sign as int } else { 0 }),
        decreases orders.len() - index,
    {
        let delta = &orders[index];
        if get_count(&before, delta.row.o_orderkey) > 0 {
            if !add_order(&mut patch, delta.row.o_orderdate, &delta.row.o_orderpriority, delta.sign as i128) { return None; }
        }
        proof {
            assert forall|key: Key, priority: Seq<u8>| #[trigger] patch_count(patch, key, priority)
                == sum(orders@.take(index as int + 1), |delta: OrderDelta|
                    if key_height(key) <= 32 && in_block(delta.row.o_orderdate as int, key)
                        && delta.row.o_orderpriority@ == priority && map_count(before@, delta.row.o_orderkey) > 0
                    { delta.sign as int } else { 0 }) by {
                lemma_sum_take(orders@, index as int, |delta: OrderDelta|
                    if key_height(key) <= 32 && in_block(delta.row.o_orderdate as int, key)
                        && delta.row.o_orderpriority@ == priority && map_count(before@, delta.row.o_orderkey) > 0
                    { delta.sign as int } else { 0 });
            }
        }
        index += 1;
        frontier = Ghost(index as int);
    }
    let order_changes: Ghost<Patch> = Ghost(patch);
    index = 0;
    frontier = Ghost(0);
    while index < order_rows.len()
        invariant index <= order_rows.len(), frontier@ == index as int, patch_valid(patch),
            maps_ready(lines@, orders@, line_rows@, after@, before@, line_support@),
            coverage(lines@, orders@, order_keys@, line_keys@),
            forall|key: Key, priority: Seq<u8>| #[trigger] patch_count(order_changes@, key, priority)
                == sum(orders@, |delta: OrderDelta|
                    if key_height(key) <= 32 && in_block(delta.row.o_orderdate as int, key)
                        && delta.row.o_orderpriority@ == priority && map_count(before@, delta.row.o_orderkey) > 0 { delta.sign as int } else { 0 }),
            forall|key: Key, priority: Seq<u8>| #[trigger] patch_count(patch, key, priority)
                == patch_count(order_changes@, key, priority)
                    + sum(order_rows@.take(frontier@), |row: OrderInput|
                        if key_height(key) <= 32 && in_block(row.o_orderdate as int, key) && row.o_orderpriority@ == priority
                            && line_support@.contains_key(row.o_orderkey)
                        { (if map_count(after@, row.o_orderkey) > 0 { 1int } else { 0int })
                            - (if map_count(before@, row.o_orderkey) > 0 { 1int } else { 0int }) }
                        else { 0 }),
        decreases order_rows.len() - index,
    {
        let row = &order_rows[index];
        if !order_set.contains_key(&row.o_orderkey) { return None; }
        if line_support.contains_key(&row.o_orderkey) {
            let was = get_count(&before, row.o_orderkey) > 0;
            let now = get_count(&after, row.o_orderkey) > 0;
            let amount: i128 = if now && !was { 1 } else if was && !now { -1 } else { 0 };
            if !add_order(&mut patch, row.o_orderdate, &row.o_orderpriority, amount) { return None; }
        }
        proof {
            assert forall|key: Key, priority: Seq<u8>| #[trigger] patch_count(patch, key, priority)
                == patch_count(order_changes@, key, priority)
                    + sum(order_rows@.take(index as int + 1), |row: OrderInput|
                        if key_height(key) <= 32 && in_block(row.o_orderdate as int, key) && row.o_orderpriority@ == priority
                            && line_support@.contains_key(row.o_orderkey)
                        { (if map_count(after@, row.o_orderkey) > 0 { 1int } else { 0int })
                            - (if map_count(before@, row.o_orderkey) > 0 { 1int } else { 0int }) } else { 0 }) by {
                lemma_sum_take(order_rows@, index as int, |row: OrderInput|
                    if key_height(key) <= 32 && in_block(row.o_orderdate as int, key) && row.o_orderpriority@ == priority
                        && line_support@.contains_key(row.o_orderkey)
                    { (if map_count(after@, row.o_orderkey) > 0 { 1int } else { 0int })
                        - (if map_count(before@, row.o_orderkey) > 0 { 1int } else { 0int }) } else { 0 });
            }
        }
        index += 1;
        frontier = Ghost(index as int);
    }
    proof {
        assert forall|key: Key, priority: Seq<u8>| #[trigger] patch_count(patch, key, priority)
            == change_count(lines@, orders@, order_rows@, line_rows@, key, priority) by {
            lemma_sum_ext(orders@,
                |delta: OrderDelta| if key_height(key) <= 32 && in_block(delta.row.o_orderdate as int, key)
                    && delta.row.o_orderpriority@ == priority && map_count(before@, delta.row.o_orderkey) > 0 { delta.sign as int } else { 0 },
                |delta: OrderDelta| order_change_weight(lines@, line_rows@, key, priority, delta));
            lemma_sum_ext(order_rows@,
                |row: OrderInput| if key_height(key) <= 32 && in_block(row.o_orderdate as int, key) && row.o_orderpriority@ == priority && line_support@.contains_key(row.o_orderkey)
                    { (if map_count(after@, row.o_orderkey) > 0 { 1int } else { 0int }) - (if map_count(before@, row.o_orderkey) > 0 { 1int } else { 0int }) } else { 0 },
                |row: OrderInput| line_change_weight(lines@, line_rows@, key, priority, row));
        }
    }
    Some(patch)
}

pub open spec fn coverage(lines: Seq<LineDelta>, orders: Seq<OrderDelta>, order_keys: Seq<i64>, line_keys: Seq<i64>) -> bool {
    &&& forall|index: int| #![trigger lines[index]] 0 <= index < lines.len()
        && lines[index].row.l_commitdate < lines[index].row.l_receiptdate ==>
            order_keys.contains(lines[index].row.l_orderkey) && line_keys.contains(lines[index].row.l_orderkey)
    &&& forall|index: int| #![trigger orders[index]] 0 <= index < orders.len() ==> line_keys.contains(orders[index].row.o_orderkey)
}

}

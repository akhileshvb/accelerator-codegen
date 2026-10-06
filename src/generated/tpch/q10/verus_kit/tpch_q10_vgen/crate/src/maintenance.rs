use vstd::prelude::*;
use std::sync::Arc;
use std::collections::HashMap;
use verus_common::sums::sum;
use verus_prooflib::ordered::{Key, Index};
use crate::gen_obligations::*;
use crate::values::*;
use crate::facts::*;
use crate::dimensions::*;
use crate::calendar::*;
use crate::paged::*;
use crate::customers::*;

verus! {

broadcast use vstd::std_specs::hash::group_hash_axioms;

#[derive(Clone, Copy)]
pub struct OrderCell {
    pub customer: i64,
    pub day: i32,
    pub count: i128,
}

pub type Orders = Index<Index<OrderCell>>;
pub type Patches = Index<Index<Moments>>;

pub open spec fn order_key_view(customer: int, day: int) -> (int, Seq<u8>) {
    (customer, encode_nat((day - i64::MIN as int) as nat, 8))
}

pub fn order_key(customer: i64, day: i32) -> (result: Key)
    ensures result.view() == order_key_view(customer as int, day as int),
{
    let mut bytes = Vec::new();
    append_i64(&mut bytes, day as i64);
    Key::composite(customer, bytes)
}

pub open spec fn order_count(orders: Orders, order: int, customer: int, day: int) -> int {
    match orders.at((order, Seq::empty())) {
        Some(inner) => match inner.at(order_key_view(customer, day)) {
            Some(cell) => if cell.customer as int == customer && cell.day as int == day { cell.count as int } else { 0 },
            None => 0,
        },
        None => 0,
    }
}

pub open spec fn order_leaf_valid(inner: Index<OrderCell>) -> bool {
    &&& inner.wf()
    &&& forall|key: (int, Seq<u8>)| #[trigger] inner.at(key).is_some() ==> {
        let cell = inner.at(key).unwrap();
        &&& cell.count != 0
        &&& key == order_key_view(cell.customer as int, cell.day as int)
    }
}

pub open spec fn orders_valid(orders: Orders) -> bool {
    forall|key: (int, Seq<u8>)| #[trigger] orders.at(key).is_some() ==> {
        let inner = orders.at(key).unwrap();
        &&& key.1 == Seq::<u8>::empty()
        &&& i64::MIN <= key.0 <= i64::MAX
        &&& inner.entries().len() > 0
        &&& order_leaf_valid(inner)
    }
}

#[verifier::rlimit(60)]
pub fn change_order(orders: &mut Orders, row: OrderInput, change: i128) -> (result: bool)
    requires orders_valid(*old(orders)),
    ensures result ==> orders_valid(*final(orders)),
        result ==> forall|order: int, customer: int, day: int| #[trigger] order_count(*final(orders), order, customer, day)
            == order_count(*old(orders), order, customer, day)
                + if order == row.o_orderkey && customer == row.o_custkey && day == row.o_orderdate { change as int } else { 0 },
{
    let ghost before = *orders;
    let outer_key = Key::number(row.o_orderkey);
    let mut inner = match orders.get(&outer_key) {
        Some(value) => {
            let shared = value.share();
            proof {
                assert(order_leaf_valid(*value));
                assert forall|key: (int, Seq<u8>)| #[trigger] shared.at(key) == value.at(key) by {}
                assert(order_leaf_valid(shared));
            }
            shared
        },
        None => Index::new(),
    };
    let ghost before_inner = inner;
    proof {
        assert(order_leaf_valid(inner));
        assert forall|key: (int, Seq<u8>)| #[trigger] inner.at(key)
            == match before.at(outer_key.view()) { Some(original) => original.at(key), None => None } by {}
    }
    let key = order_key(row.o_custkey, row.o_orderdate);
    let old = match inner.get(&key) {
        Some(value) => {
            proof { crate::key_facts::lemma_order_key_injective(value.customer, value.day, row.o_custkey, row.o_orderdate); }
            value.count
        },
        None => 0i128,
    };
    let count = match old.checked_add(change) { Some(value) => value, None => return false };
    if count == 0 { inner.remove(&key); }
    else { inner.put(key, Arc::new(OrderCell { customer: row.o_custkey, day: row.o_orderdate, count })); }
    proof {
        inner.model();
        assert(order_leaf_valid(inner));
        assert forall|customer: int, day: int| #[trigger] inner.at(order_key_view(customer, day)).is_some()
            && inner.at(order_key_view(customer, day)).unwrap().customer as int == customer
            && inner.at(order_key_view(customer, day)).unwrap().day as int == day
            implies customer == row.o_custkey && day == row.o_orderdate || before_inner.at(order_key_view(customer, day)) == inner.at(order_key_view(customer, day)) by {}
    }
    if inner.is_empty() { orders.remove(&outer_key); }
    else { orders.put(outer_key, Arc::new(inner)); }
    proof {
        assert(orders_valid(*orders));
        assert forall|order: int, customer: int, day: int| #[trigger] order_count(*orders, order, customer, day)
            == order_count(before, order, customer, day)
                + if order == row.o_orderkey && customer == row.o_custkey && day == row.o_orderdate { change as int } else { 0 } by {
            if order == row.o_orderkey {
                if customer == row.o_custkey && day == row.o_orderdate {
                    assert(order_count(before, order, customer, day) == old as int);
                    assert(order_count(*orders, order, customer, day) == count as int);
                } else {
                    if order_key_view(customer, day) == key.view() {
                        assert(customer != row.o_custkey || day != row.o_orderdate);
                        assert(order_count(before, order, customer, day) == 0);
                        assert(order_count(*orders, order, customer, day) == 0);
                    } else {
                        assert(inner.at(order_key_view(customer, day)) == before_inner.at(order_key_view(customer, day)));
                    }
                }
            }
        }
    }
    true
}

pub fn collect_orders(rows: &[OrderInput]) -> (result: Option<Orders>)
    ensures result matches Some(orders) ==> orders_valid(orders),
        result matches Some(orders) ==> forall|order: int, customer: int, day: int| #[trigger] order_count(orders, order, customer, day)
            == sum(rows@, |row: OrderInput|
                if row.o_orderkey == order && row.o_custkey == customer && row.o_orderdate == day { 1 } else { 0 }),
{
    let mut orders = Index::new();
    let mut index = 0usize;
    while index < rows.len()
        invariant index <= rows.len(), orders_valid(orders),
            forall|order: int, customer: int, day: int| #[trigger] order_count(orders, order, customer, day)
                == sum(rows@.take(index as int), |row: OrderInput|
                    if row.o_orderkey == order && row.o_custkey == customer && row.o_orderdate == day { 1 } else { 0 }),
        decreases rows.len() - index,
    {
        let ghost processed = index as int;
        if !change_order(&mut orders, rows[index], 1) { return None; }
        proof {
            assert forall|order: int, customer: int, day: int| #[trigger] order_count(orders, order, customer, day)
                == sum(rows@.take(index as int + 1), |row: OrderInput|
                    if row.o_orderkey == order && row.o_custkey == customer && row.o_orderdate == day { 1 } else { 0 }) by {
                verus_common::sums::lemma_sum_take(rows@, index as int, |row: OrderInput|
                    if row.o_orderkey == order && row.o_custkey == customer && row.o_orderdate == day { 1 } else { 0 });
            }
        }
        index += 1;
    }
    proof { assert(rows@.take(index as int) =~= rows@); }
    Some(orders)
}

pub open spec fn projected_line_weight(row: ReturnedLineInput, component: int) -> int {
    if component == 0 { 1 }
    else {
        let revenue = row.l_extendedprice as int * (100 - row.l_discount as int);
        if component == 1 { revenue } else if revenue > 0 { revenue } else { 0 }
    }
}

pub fn collect_lines(rows: &[ReturnedLineInput]) -> (result: Option<Index<Moments>>)
    ensures result matches Some(lines) ==> moment_index_valid(lines),
        result matches Some(lines) ==> forall|key: int, component: int|
        #[trigger] moment_at(lines, key).component(component)
            == sum(rows@, |row: ReturnedLineInput| if row.l_orderkey == key { projected_line_weight(row, component) } else { 0 }),
{
    let mut lines = Index::new();
    let mut index = 0usize;
    while index < rows.len()
        invariant index <= rows.len(), moment_index_valid(lines),
            forall|key: int, component: int| #[trigger] moment_at(lines, key).component(component)
                == sum(rows@.take(index as int), |row: ReturnedLineInput|
                    if row.l_orderkey == key { projected_line_weight(row, component) } else { 0 }),
        decreases rows.len() - index,
    {
        let ghost processed = index as int;
        let row = rows[index];
        if !add_moments(&mut lines, row.l_orderkey, line_moments(row.l_extendedprice, row.l_discount)) { return None; }
        proof {
            assert forall|key: int, component: int| #[trigger] moment_at(lines, key).component(component)
                == sum(rows@.take(index as int + 1), |row: ReturnedLineInput|
                    if row.l_orderkey == key { projected_line_weight(row, component) } else { 0 }) by {
                verus_common::sums::lemma_sum_take(rows@, index as int,
                    |row: ReturnedLineInput| if row.l_orderkey == key { projected_line_weight(row, component) } else { 0 });
            }
        }
        index += 1;
    }
    proof { assert(rows@.take(index as int) =~= rows@); }
    Some(lines)
}

pub fn collect_order_changes(rows: &[OrderDelta]) -> (result: Option<Orders>)
    ensures result matches Some(orders) ==> orders_valid(orders),
        result matches Some(orders) ==> forall|order: int, customer: int, day: int| #[trigger] order_count(orders, order, customer, day)
            == sum(rows@, |delta: OrderDelta|
                if delta.row.o_orderkey == order && delta.row.o_custkey == customer && delta.row.o_orderdate == day
                    { delta.sign as int } else { 0 }),
{
    let mut orders = Index::new();
    let mut index = 0usize;
    while index < rows.len()
        invariant index <= rows.len(), orders_valid(orders),
            forall|order: int, customer: int, day: int| #[trigger] order_count(orders, order, customer, day)
                == sum(rows@.take(index as int), |delta: OrderDelta|
                    if delta.row.o_orderkey == order && delta.row.o_custkey == customer && delta.row.o_orderdate == day
                        { delta.sign as int } else { 0 }),
        decreases rows.len() - index,
    {
        let ghost processed = index as int;
        if !change_order(&mut orders, rows[index].row, rows[index].sign as i128) { return None; }
        proof {
            assert forall|order: int, customer: int, day: int| #[trigger] order_count(orders, order, customer, day)
                == sum(rows@.take(index as int + 1), |delta: OrderDelta|
                    if delta.row.o_orderkey == order && delta.row.o_custkey == customer && delta.row.o_orderdate == day
                        { delta.sign as int } else { 0 }) by {
                verus_common::sums::lemma_sum_take(rows@, index as int, |delta: OrderDelta|
                    if delta.row.o_orderkey == order && delta.row.o_custkey == customer && delta.row.o_orderdate == day
                        { delta.sign as int } else { 0 });
            }
        }
        index += 1;
    }
    proof { assert(rows@.take(index as int) =~= rows@); }
    Some(orders)
}

pub open spec fn delta_line_weight(row: LineDelta, component: int) -> int {
    if row.is_returned == 0 { 0 }
    else {
        let value = if component == 0 { 1 }
            else {
                let revenue = row.l_extendedprice as int * (100 - row.l_discount as int);
                if component == 1 { revenue } else if revenue > 0 { revenue } else { 0 }
            };
        value * row.sign as int
    }
}

pub fn collect_line_changes(rows: &[LineDelta]) -> (result: Option<Index<Moments>>)
    ensures result matches Some(lines) ==> moment_index_valid(lines),
        result matches Some(lines) ==> forall|key: int, component: int|
        #[trigger] moment_at(lines, key).component(component)
            == sum(rows@, |row: LineDelta| if row.l_orderkey == key { delta_line_weight(row, component) } else { 0 }),
{
    let mut lines = Index::new();
    let mut index = 0usize;
    while index < rows.len()
        invariant index <= rows.len(), moment_index_valid(lines),
            forall|key: int, component: int| #[trigger] moment_at(lines, key).component(component)
                == sum(rows@.take(index as int), |row: LineDelta|
                    if row.l_orderkey == key { delta_line_weight(row, component) } else { 0 }),
        decreases rows.len() - index,
    {
        let ghost processed = index as int;
        let row = rows[index];
        if row.is_returned != 0 {
            let change = match line_moments(row.l_extendedprice, row.l_discount).scale(row.sign as i128) {
                Some(value) => value, None => return None,
            };
            if !add_moments(&mut lines, row.l_orderkey, change) { return None; }
        }
        proof {
            assert forall|key: int, component: int| #[trigger] moment_at(lines, key).component(component)
                == sum(rows@.take(index as int + 1), |row: LineDelta|
                    if row.l_orderkey == key { delta_line_weight(row, component) } else { 0 }) by {
                verus_common::sums::lemma_sum_take(rows@, index as int,
                    |row: LineDelta| if row.l_orderkey == key { delta_line_weight(row, component) } else { 0 });
            }
        }
        index += 1;
    }
    proof { assert(rows@.take(index as int) =~= rows@); }
    Some(lines)
}

pub fn order_support(orders: &Orders) -> (result: Index<()>)
    requires orders_valid(*orders),
    ensures forall|key: (int, Seq<u8>)| #[trigger] result.at(key).is_some() ==>
            key.1 == Seq::<u8>::empty() && i64::MIN <= key.0 <= i64::MAX,
        forall|key: (int, Seq<u8>)| #[trigger] orders.at(key).is_some() ==> result.at(key).is_some(),
{
    let order_snapshot = orders.share();
    proof { orders.model(); }
    let ghost order_entries = orders.entries();
    let mut keys = Index::new();
    let ghost mut processed = 0int;
    let mut order_cursor = orders.cursor();
    loop
        invariant 0 <= processed <= order_entries.len(),
            order_cursor.remaining() == order_entries.skip(processed),
            order_entries == orders.entries(), orders.wf(), orders_valid(*orders),
            forall|position: int| #![trigger order_entries[position]] 0 <= position < order_entries.len()
                ==> orders.at(order_entries[position].0).is_some(),
            forall|position: int| #![trigger order_entries[position]] 0 <= position < processed
                ==> keys.at(order_entries[position].0).is_some(),
            forall|key: (int, Seq<u8>)| #[trigger] keys.at(key).is_some() ==>
                key.1 == Seq::<u8>::empty() && i64::MIN <= key.0 <= i64::MAX,
        decreases order_cursor.remaining().len(),
    {
        let (key, _) = match order_cursor.next() {
            Some(row) => row,
            None => {
                proof {
                    assert(processed == order_entries.len());
                    orders.model();
                    assert forall|query: (int, Seq<u8>)| #[trigger] orders.at(query).is_some()
                        implies keys.at(query).is_some() by {
                        let position = choose|position: int| #![trigger order_entries[position]]
                            0 <= position < order_entries.len() && order_entries[position].0 == query;
                    }
                }
                return keys;
            },
        };
        proof { assert(key.view() == order_entries[processed].0); }
        keys.put(key, Arc::new(()));
        proof { processed = processed + 1; }
    }
}

pub fn support(orders: &Orders, lines: &Index<Moments>) -> (result: Index<()>)
    requires orders_valid(*orders), moment_index_valid(*lines),
    ensures forall|key: (int, Seq<u8>)| #[trigger] result.at(key).is_some() ==>
            key.1 == Seq::<u8>::empty() && i64::MIN <= key.0 <= i64::MAX,
        forall|key: (int, Seq<u8>)| #![trigger orders.at(key), lines.at(key)]
            orders.at(key).is_some() || lines.at(key).is_some() ==> result.at(key).is_some(),
{
    let mut keys = order_support(orders);
    let line_snapshot = lines.share();
    proof { lines.model(); }
    let ghost line_entries = lines.entries();
    let ghost mut processed = 0int;
    let mut line_cursor = lines.cursor();
    loop
        invariant 0 <= processed <= line_entries.len(),
            line_cursor.remaining() == line_entries.skip(processed),
            line_entries == lines.entries(), lines.wf(), moment_index_valid(*lines),
            forall|position: int| #![trigger line_entries[position]] 0 <= position < line_entries.len()
                ==> lines.at(line_entries[position].0).is_some(),
            forall|position: int| #![trigger line_entries[position]] 0 <= position < processed
                ==> keys.at(line_entries[position].0).is_some(),
            forall|query: (int, Seq<u8>)| #[trigger] orders.at(query).is_some() ==> keys.at(query).is_some(),
            forall|key: (int, Seq<u8>)| #[trigger] keys.at(key).is_some() ==>
                key.1 == Seq::<u8>::empty() && i64::MIN <= key.0 <= i64::MAX,
        decreases line_cursor.remaining().len(),
    {
        let (key, _) = match line_cursor.next() {
            Some(row) => row,
            None => {
                proof {
                    assert(processed == line_entries.len());
                    lines.model();
                    assert forall|query: (int, Seq<u8>)| #[trigger] lines.at(query).is_some()
                        implies keys.at(query).is_some() by {
                        let position = choose|position: int| #![trigger line_entries[position]]
                            0 <= position < line_entries.len() && line_entries[position].0 == query;
                    }
                }
                return keys;
            },
        };
        proof { assert(key.view() == line_entries[processed].0); }
        keys.put(key, Arc::new(()));
        proof { processed = processed + 1; }
    }
}

pub fn key_vec(keys: &Index<()>) -> Vec<i64> {
    let mut result = Vec::new();
    let mut cursor = keys.cursor();
    loop
        decreases cursor.remaining().len(),
    {
        let (key, _) = match cursor.next() { Some(row) => row, None => break };
        result.push(key.number);
    }
    result
}

pub fn raw_support(orders: &[OrderDelta], lines: &[LineDelta]) -> Index<()> {
    let mut keys = Index::new();
    let mut index = 0usize;
    while index < orders.len()
        invariant index <= orders.len(),
        decreases orders.len() - index,
    {
        let ghost processed = index as int;
        keys.put(Key::number(orders[index].row.o_orderkey), Arc::new(()));
        index += 1;
    }
    index = 0;
    while index < lines.len()
        invariant index <= lines.len(),
        decreases lines.len() - index,
    {
        let ghost processed = index as int;
        if lines[index].is_returned != 0 { keys.put(Key::number(lines[index].l_orderkey), Arc::new(())); }
        index += 1;
    }
    keys
}

pub fn planned_keys(orders: &[OrderDelta], lines: &[LineDelta]) -> Vec<i64> {
    let order_changes = match collect_order_changes(orders) {
        Some(value) => value, None => return key_vec(&raw_support(orders, lines)),
    };
    let line_changes = match collect_line_changes(lines) {
        Some(value) => value, None => return key_vec(&raw_support(orders, lines)),
    };
    key_vec(&support(&order_changes, &line_changes))
}

pub fn key_set(rows: &[i64]) -> (result: Index<()>)
    ensures forall|key: (int, Seq<u8>)| #[trigger] result.at(key).is_some()
        <==> key.1 == Seq::<u8>::empty() && seq_view_i64(rows@).contains(key.0),
{
    let mut result = Index::new();
    let mut index = 0usize;
    while index < rows.len()
        invariant index <= rows.len(),
            forall|key: (int, Seq<u8>)| #[trigger] result.at(key).is_some()
                <==> key.1 == Seq::<u8>::empty() && seq_view_i64(rows@.take(index as int)).contains(key.0),
        decreases rows.len() - index,
    {
        let ghost processed = index as int;
        result.put(Key::number(rows[index]), Arc::new(()));
        proof {
            assert(rows@.take(index as int + 1) =~= rows@.take(index as int).push(rows@[index as int]));
            assert(seq_view_i64(rows@.take(index as int + 1))
                =~= seq_view_i64(rows@.take(index as int)).push(rows@[index as int] as int));
            verus_prooflib::lemma_contains_push(seq_view_i64(rows@.take(index as int)), rows@[index as int] as int);
        }
        index += 1;
    }
    proof { assert(rows@.take(rows@.len() as int) =~= rows@); }
    result
}

pub fn covers(expected: &Index<()>, rows: &[i64]) -> (result: bool)
    ensures result ==> forall|key: (int, Seq<u8>)| #[trigger] expected.at(key).is_some() ==> seq_view_i64(rows@).contains(key.0),
{
    let snapshot = expected.share();
    proof { expected.model(); }
    let ghost entries = expected.entries();
    let available = key_set(rows);
    let mut cursor = expected.cursor();
    let ghost mut processed = 0int;
    loop
        invariant 0 <= processed <= entries.len(), cursor.remaining() == entries.skip(processed),
            entries == expected.entries(), expected.wf(),
            forall|key: (int, Seq<u8>)| #[trigger] available.at(key).is_some()
                <==> key.1 == Seq::<u8>::empty() && seq_view_i64(rows@).contains(key.0),
            forall|position: int| 0 <= position < processed ==> seq_view_i64(rows@).contains((#[trigger] entries[position]).0.0),
        decreases cursor.remaining().len(),
    {
        let (key, _) = match cursor.next() {
            Some(row) => row,
            None => {
                proof {
                    assert(processed == entries.len());
                    expected.model();
                    assert forall|key: (int, Seq<u8>)| #[trigger] expected.at(key).is_some()
                        implies seq_view_i64(rows@).contains(key.0) by {
                        let position = choose|position: int| 0 <= position < entries.len()
                            && (#[trigger] entries[position]).0 == key;
                    }
                }
                return true;
            },
        };
        if available.get(&key).is_none() { return false; }
        proof {
            assert(key.view() == entries[processed].0);
            assert(seq_view_i64(rows@).contains(entries[processed].0.0));
            processed = processed + 1;
        }
    }
}

pub open spec fn patches_valid(patches: Patches) -> bool {
    forall|key: (int, Seq<u8>)| #[trigger] patches.at(key).is_some() ==> {
        &&& key.1 == Seq::<u8>::empty()
        &&& i32::MIN <= key.0 <= i32::MAX
        &&& moment_index_valid(patches.at(key).unwrap())
        &&& patches.at(key).unwrap().entries().len() > 0
    }
}

pub open spec fn patch_at(patches: Patches, day: int, customer: int) -> Moments {
    match patches.at((day, Seq::empty())) {
        Some(inner) => moment_at(inner, customer),
        None => Moments { count: 0, revenue: 0, upper: 0 },
    }
}

pub open spec fn moment_row_weight(customer: int, component: int) -> spec_fn(((int, Seq<u8>), Moments)) -> int {
    |entry: ((int, Seq<u8>), Moments)| if entry.0.0 == customer { entry.1.component(component) } else { 0 }
}

pub open spec fn moment_rows_sum(rows: Seq<((int, Seq<u8>), Moments)>, customer: int, component: int) -> int {
    sum(rows, moment_row_weight(customer, component))
}

pub open spec fn patch_row_weight(day: int, customer: int, grain: int, component: int) -> spec_fn(((int, Seq<u8>), Index<Moments>)) -> int {
    |entry: ((int, Seq<u8>), Index<Moments>)| if period_of(entry.0.0, grain) == day {
        moment_at(entry.1, customer).component(component) } else { 0 }
}

pub open spec fn patch_rows_sum(rows: Seq<((int, Seq<u8>), Index<Moments>)>, day: int, customer: int, grain: int, component: int) -> int {
    sum(rows, patch_row_weight(day, customer, grain, component))
}

pub proof fn lemma_moment_rows(index: &Index<Moments>, customer: int, component: int)
    requires index.wf(), moment_index_valid(*index),
    ensures moment_rows_sum(index.entries(), customer, component) == moment_at(*index, customer).component(component),
{
    index.model();
    let weight = |entry: ((int, Seq<u8>), Moments)| entry.1.component(component);
    verus_prooflib::ordered::lemma_key_contribution(index, (customer, Seq::<u8>::empty()), weight);
    verus_common::sums::lemma_sum_ext(index.entries(),
        moment_row_weight(customer, component),
        |entry: ((int, Seq<u8>), Moments)| if entry.0 == (customer, Seq::<u8>::empty()) { weight(entry) } else { 0 });
    assert(verus_prooflib::ordered::contribution(*index, (customer, Seq::<u8>::empty()), weight)
        == moment_at(*index, customer).component(component));
    verus_common::sums::lemma_sum_ext_all(index.entries(),
        |entry: ((int, Seq<u8>), Moments)| if entry.0 == (customer, Seq::<u8>::empty()) { weight(entry) } else { 0 },
        |row: ((int, Seq<u8>), Moments)| if row.0 == (customer, Seq::<u8>::empty()) { weight(row) } else { 0 });
    assert(sum(index.entries(), |row: ((int, Seq<u8>), Moments)| if row.0 == (customer, Seq::<u8>::empty()) { weight(row) } else { 0 })
        == verus_prooflib::ordered::contribution(*index, (customer, Seq::<u8>::empty()), weight));
}

pub proof fn lemma_patch_rows(index: &Patches, day: int, customer: int, component: int)
    requires index.wf(), patches_valid(*index),
    ensures patch_rows_sum(index.entries(), day, customer, 0, component) == patch_at(*index, day, customer).component(component),
{
    index.model();
    let weight = |entry: ((int, Seq<u8>), Index<Moments>)| moment_at(entry.1, customer).component(component);
    verus_prooflib::ordered::lemma_key_contribution(index, (day, Seq::<u8>::empty()), weight);
    verus_common::sums::lemma_sum_ext(index.entries(),
        patch_row_weight(day, customer, 0, component),
        |entry: ((int, Seq<u8>), Index<Moments>)| if entry.0 == (day, Seq::<u8>::empty()) { weight(entry) } else { 0 });
}

#[verifier::rlimit(20)]
pub fn add_patch(patches: &mut Patches, day: i32, customer: i64, change: Moments) -> (result: bool)
    requires patches_valid(*old(patches)),
    ensures result ==> patches_valid(*final(patches)),
        result ==> forall|target_day: int, target_customer: int, component: int|
            #[trigger] patch_at(*final(patches), target_day, target_customer).component(component)
                == patch_at(*old(patches), target_day, target_customer).component(component)
                    + if target_day == day && target_customer == customer { change.component(component) } else { 0 },
        result ==> forall|target_day: int, target_customer: int, grain: int, component: int|
            #[trigger] patch_rows_sum(final(patches).entries(), target_day, target_customer, grain, component)
                == patch_rows_sum(old(patches).entries(), target_day, target_customer, grain, component)
                    + if period_of(day as int, grain) == target_day && target_customer == customer {
                        change.component(component) } else { 0 },
{
    if change.is_zero() { return true; }
    let before_snapshot = patches.share();
    let ghost before = *patches;
    proof {
        assert forall|query: (int, Seq<u8>)| #[trigger] before_snapshot.at(query) == before.at(query) by {}
    }
    let key = Key::number(day as i64);
    let mut inner = match patches.get(&key) {
        Some(value) => {
            let shared = value.share();
            proof {
                assert(moment_index_valid(*value));
                assert forall|target: (int, Seq<u8>)| #[trigger] shared.at(target) == value.at(target) by {}
                assert(moment_index_valid(shared));
                assert forall|target: int| #[trigger] moment_at(shared, target) == patch_at(before, day as int, target) by {}
            }
            shared
        },
        None => Index::new(),
    };
    let ghost before_inner = inner;
    proof {
        assert(moment_index_valid(inner));
        assert forall|target: int| #[trigger] moment_at(inner, target) == patch_at(before, day as int, target) by {}
    }
    if !add_moments(&mut inner, customer, change) { return false; }
    proof { inner.model(); }
    if inner.is_empty() { patches.remove(&key); }
    else { patches.put(key, Arc::new(inner)); }
    let after_snapshot = patches.share();
    proof {
        assert forall|query: (int, Seq<u8>)| #[trigger] after_snapshot.at(query) == patches.at(query) by {}
        assert forall|query: (int, Seq<u8>)| #![trigger before_snapshot.at(query), after_snapshot.at(query)]
            query != (day as int, Seq::empty()) implies before_snapshot.at(query) == after_snapshot.at(query) by {}
        assert forall|target_day: int, target_customer: int, component: int|
            #[trigger] patch_at(*patches, target_day, target_customer).component(component)
                == patch_at(before, target_day, target_customer).component(component)
                    + if target_day == day && target_customer == customer { change.component(component) } else { 0 } by {
            if target_day == day {
                assert(patch_at(*patches, target_day, target_customer) == moment_at(inner, target_customer));
            }
        }
        assert forall|target_day: int, target_customer: int, grain: int, component: int|
            #[trigger] patch_rows_sum(patches.entries(), target_day, target_customer, grain, component)
                == patch_rows_sum(before_snapshot.entries(), target_day, target_customer, grain, component)
                    + if period_of(day as int, grain) == target_day && target_customer == customer {
                        change.component(component) } else { 0 } by {
            let weight = patch_row_weight(target_day, target_customer, grain, component);
            verus_prooflib::ordered::lemma_sum_unchanged_except(&before_snapshot, &after_snapshot,
                (day as int, Seq::empty()), weight);
            assert(patch_at(*patches, day as int, target_customer).component(component)
                == patch_at(before, day as int, target_customer).component(component)
                    + if target_customer == customer { change.component(component) } else { 0 });
        }
    }
    true
}

pub open spec fn order_patch_weight(cell: OrderCell, day: int, customer: int, lines: Moments, component: int) -> int {
    if cell.day as int == day && cell.customer as int == customer { lines.component(component) * cell.count as int } else { 0 }
}

pub open spec fn order_patch_sum(entries: Seq<((int, Seq<u8>), OrderCell)>, day: int, customer: int, lines: Moments, component: int) -> int {
    sum(entries, |entry: ((int, Seq<u8>), OrderCell)| order_patch_weight(entry.1, day, customer, lines, component))
}

pub open spec fn order_group_weight(day: int, customer: int, grain: int) -> spec_fn(((int, Seq<u8>), OrderCell)) -> int {
    |entry: ((int, Seq<u8>), OrderCell)| if entry.1.customer as int == customer && period_of(entry.1.day as int, grain) == day {
        entry.1.count as int } else { 0 }
}

pub open spec fn order_group_sum(entries: Seq<((int, Seq<u8>), OrderCell)>, day: int, customer: int, grain: int) -> int {
    sum(entries, order_group_weight(day, customer, grain))
}

pub fn join_patch(patches: &mut Patches, orders: &Index<OrderCell>, lines: Moments) -> (result: bool)
    requires patches_valid(*old(patches)),
    ensures result ==> patches_valid(*final(patches)),
        result ==> forall|day: int, customer: int, component: int|
            #[trigger] patch_at(*final(patches), day, customer).component(component)
                == patch_at(*old(patches), day, customer).component(component)
                    + order_patch_sum(orders.entries(), day, customer, lines, component),
        result ==> forall|day: int, customer: int, grain: int, component: int|
            #[trigger] patch_rows_sum(final(patches).entries(), day, customer, grain, component)
                == patch_rows_sum(old(patches).entries(), day, customer, grain, component)
                    + order_group_sum(orders.entries(), day, customer, grain) * lines.component(component),
{
    let ghost before = *patches;
    let ghost entries = orders.entries();
    let ghost processed = 0int;
    let mut cursor = orders.cursor();
    loop
        invariant patches_valid(*patches),
            before == *old(patches),
            entries == orders.entries(),
            0 <= processed <= entries.len(), cursor.remaining() == entries.skip(processed),
            forall|day: int, customer: int, component: int|
                #[trigger] patch_at(*patches, day, customer).component(component)
                    == patch_at(before, day, customer).component(component)
                        + order_patch_sum(entries.take(processed), day, customer, lines, component),
            forall|day: int, customer: int, grain: int, component: int|
                #[trigger] patch_rows_sum(patches.entries(), day, customer, grain, component)
                    == patch_rows_sum(before.entries(), day, customer, grain, component)
                        + order_group_sum(entries.take(processed), day, customer, grain) * lines.component(component),
        decreases cursor.remaining().len(),
    {
        let (_, order) = match cursor.next() {
            Some(row) => row,
            None => {
                proof {
                    assert(processed == entries.len());
                    assert(entries.take(processed) =~= entries);
                    assert forall|day: int, customer: int, component: int|
                        #[trigger] patch_at(*patches, day, customer).component(component)
                            == patch_at(before, day, customer).component(component)
                                + order_patch_sum(orders.entries(), day, customer, lines, component) by {}
                }
                return true;
            },
        };
        let change = match lines.scale(order.count) { Some(value) => value, None => return false };
        if !add_patch(patches, order.day, order.customer, change) { return false; }
        proof {
            assert(*order == entries[processed].1);
            assert forall|day: int, customer: int, component: int|
                #[trigger] patch_at(*patches, day, customer).component(component)
                    == patch_at(before, day, customer).component(component)
                        + order_patch_sum(entries.take(processed + 1), day, customer, lines, component) by {
                verus_common::sums::lemma_sum_take(entries, processed,
                    |entry: ((int, Seq<u8>), OrderCell)| order_patch_weight(entry.1, day, customer, lines, component));
            }
            assert forall|day: int, customer: int, grain: int, component: int|
                #[trigger] patch_rows_sum(patches.entries(), day, customer, grain, component)
                    == patch_rows_sum(before.entries(), day, customer, grain, component)
                        + order_group_sum(entries.take(processed + 1), day, customer, grain) * lines.component(component) by {
                verus_common::sums::lemma_sum_take(entries, processed, order_group_weight(day, customer, grain));
                assert(change.component(component) == lines.component(component) * order.count as int);
                assert((order_group_sum(entries.take(processed), day, customer, grain)
                    + order_group_weight(day, customer, grain)(entries[processed])) * lines.component(component)
                    == order_group_sum(entries.take(processed), day, customer, grain) * lines.component(component)
                        + order_group_weight(day, customer, grain)(entries[processed]) * lines.component(component)) by (nonlinear_arith);
                assert(order.count as int * lines.component(component) == lines.component(component) * order.count as int) by (nonlinear_arith);
            }
            processed = processed + 1;
        }
    }
}

pub open spec fn hash_moment_at<KeyType>(index: Map<KeyType, Moments>, key: KeyType) -> Moments {
    if index.dom().contains(key) { index[key] }
    else { Moments { count: 0, revenue: 0, upper: 0 } }
}

pub open spec fn cell_code(day: i32, customer: i64) -> int {
    day as int * 18446744073709551616int + customer as int - i64::MIN as int
}

pub fn cell_key(day: i32, customer: i64) -> (result: i128)
    ensures result as int == cell_code(day, customer),
{
    day as i128 * 18446744073709551616i128 + (customer as i128 - i64::MIN as i128)
}

pub proof fn lemma_cell_key_injective(left_day: i32, left_customer: i64, right_day: i32, right_customer: i64)
    requires cell_code(left_day, left_customer) == cell_code(right_day, right_customer),
    ensures left_day == right_day, left_customer == right_customer,
{
    verus_prooflib::lemma_numeric_key_parts(left_day as int,
        left_customer as int - i64::MIN as int, 18446744073709551616int);
    verus_prooflib::lemma_numeric_key_parts(right_day as int,
        right_customer as int - i64::MIN as int, 18446744073709551616int);
}

pub open spec fn scanned_line_sum(rows: Seq<ReturnedLineInput>, key: int, component: int) -> int {
    sum(rows, |row: ReturnedLineInput| if row.l_orderkey == key { projected_line_weight(row, component) } else { 0 })
}

pub open spec fn scanned_fact_sum(orders: Seq<OrderInput>, rows: Seq<ReturnedLineInput>, day: int, customer: int, grain: int, component: int) -> int {
    sum(orders, |order: OrderInput| if period_of(order.o_orderdate as int, grain) == day && order.o_custkey == customer {
        scanned_line_sum(rows, order.o_orderkey as int, component)
    } else { 0 })
}

pub proof fn lemma_scanned_fact_split(orders: Seq<OrderInput>, rows: Seq<ReturnedLineInput>, start: int, end: int,
    day: int, customer: int, grain: int, component: int)
    requires 0 <= start <= end <= orders.len(),
    ensures scanned_fact_sum(orders.take(end), rows, day, customer, grain, component)
        == scanned_fact_sum(orders.take(start), rows, day, customer, grain, component)
            + scanned_fact_sum(orders.subrange(start, end), rows, day, customer, grain, component),
{
    assert(orders.take(end) =~= orders.take(start) + orders.subrange(start, end));
    verus_common::sums::lemma_sum_concat(orders.take(start), orders.subrange(start, end),
        |order: OrderInput| if period_of(order.o_orderdate as int, grain) == day && order.o_custkey == customer {
            scanned_line_sum(rows, order.o_orderkey as int, component) } else { 0 });
}

pub fn scan_lines(rows: &[ReturnedLineInput]) -> (result: Option<HashMap<i64, Moments>>)
    ensures result matches Some(lines) ==> forall|key: i64, component: int|
        #[trigger] hash_moment_at(lines@, key).component(component) == scanned_line_sum(rows@, key as int, component),
{
    let mut lines: HashMap<i64, Moments> = HashMap::new();
    let mut index = 0usize;
    while index < rows.len()
        invariant index <= rows.len(),
            forall|key: i64, component: int| #[trigger] hash_moment_at(lines@, key).component(component)
                == sum(rows@.take(index as int), |row: ReturnedLineInput|
                    if row.l_orderkey == key { projected_line_weight(row, component) } else { 0 }),
        decreases rows.len() - index,
    {
        let ghost processed = index as int;
        let row = rows[index];
        let ghost before = lines@;
        let old = match lines.get(&row.l_orderkey) { Some(value) => *value, None => Moments::zero() };
        proof { assert(old == hash_moment_at(before, row.l_orderkey)); }
        let moment = line_moments(row.l_extendedprice, row.l_discount);
        let next = match old.add(moment) {
            Some(value) => value, None => return None,
        };
        lines.insert(row.l_orderkey, next);
        proof {
            assert forall|key: i64, component: int| #[trigger] hash_moment_at(lines@, key).component(component)
                == sum(rows@.take(index as int + 1), |row: ReturnedLineInput|
                    if row.l_orderkey == key { projected_line_weight(row, component) } else { 0 }) by {
                assert(hash_moment_at(before, key).component(component)
                    == sum(rows@.take(index as int), |row: ReturnedLineInput|
                        if row.l_orderkey == key { projected_line_weight(row, component) } else { 0 }));
                assert(hash_moment_at(lines@, key).component(component)
                    == hash_moment_at(before, key).component(component) + if key == row.l_orderkey { moment.component(component) } else { 0 });
                verus_common::sums::lemma_sum_take(rows@, index as int,
                    |row: ReturnedLineInput| if row.l_orderkey == key { projected_line_weight(row, component) } else { 0 });
            }
        }
        index += 1;
    }
    proof { assert(rows@.take(index as int) =~= rows@); }
    Some(lines)
}

pub fn build_chunk_patches(orders: &[OrderInput], Ghost(rows): Ghost<Seq<ReturnedLineInput>>, lines: &HashMap<i64, Moments>) -> (result: Option<Patches>)
    requires forall|key: i64, component: int| #[trigger] hash_moment_at(lines@, key).component(component)
        == scanned_line_sum(rows, key as int, component),
    ensures result matches Some(patches) ==> patches_valid(patches),
        result matches Some(patches) ==> forall|day: int, customer: int, component: int|
            #[trigger] patch_at(patches, day, customer).component(component)
                == scanned_fact_sum(orders@, rows, day, customer, 0, component),
        result matches Some(patches) ==> forall|day: int, customer: int, grain: int, component: int|
            #[trigger] patch_rows_sum(patches.entries(), day, customer, grain, component)
                == scanned_fact_sum(orders@, rows, day, customer, grain, component),
{
    let ghost line_table = lines@;
    let mut patches = Index::new();
    let mut index = 0usize;
    while index < orders.len()
        invariant index <= orders.len(), patches_valid(patches), lines@ == line_table,
            forall|key: i64, component: int| #[trigger] hash_moment_at(lines@, key).component(component)
                == scanned_line_sum(rows, key as int, component),
            forall|day: int, customer: int, component: int|
                #[trigger] patch_at(patches, day, customer).component(component)
                    == scanned_fact_sum(orders@.take(index as int), rows, day, customer, 0, component),
            forall|day: int, customer: int, grain: int, component: int|
                #[trigger] patch_rows_sum(patches.entries(), day, customer, grain, component)
                    == scanned_fact_sum(orders@.take(index as int), rows, day, customer, grain, component),
        decreases orders.len() - index,
    {
        let ghost processed = index as int;
        let row = orders[index];
        let moment = match lines.get(&row.o_orderkey) { Some(line) => *line, None => Moments::zero() };
        proof { assert(moment == hash_moment_at(line_table, row.o_orderkey)); }
        if !add_patch(&mut patches, row.o_orderdate, row.o_custkey, moment) { return None; }
        proof {
            assert forall|day: int, customer: int, component: int|
                #[trigger] patch_at(patches, day, customer).component(component)
                    == scanned_fact_sum(orders@.take(index as int + 1), rows, day, customer, 0, component) by {
                assert(moment.component(component) == scanned_line_sum(rows, row.o_orderkey as int, component));
                verus_common::sums::lemma_sum_take(orders@, index as int,
                    |order: OrderInput| if period_of(order.o_orderdate as int, 0) == day && order.o_custkey == customer {
                        scanned_line_sum(rows, order.o_orderkey as int, component) } else { 0 });
            }
            assert forall|day: int, customer: int, grain: int, component: int|
                #[trigger] patch_rows_sum(patches.entries(), day, customer, grain, component)
                    == scanned_fact_sum(orders@.take(index as int + 1), rows, day, customer, grain, component) by {
                assert(moment.component(component) == scanned_line_sum(rows, row.o_orderkey as int, component));
                verus_common::sums::lemma_sum_take(orders@, index as int,
                    |order: OrderInput| if period_of(order.o_orderdate as int, grain) == day && order.o_custkey == customer {
                        scanned_line_sum(rows, order.o_orderkey as int, component) } else { 0 });
            }
        }
        index += 1;
    }
    proof { assert(orders@.take(index as int) =~= orders@); }
    Some(patches)
}

pub fn merge_moment_patch(target: &mut Index<Moments>, source: &Index<Moments>) -> (result: bool)
    requires moment_index_valid(*old(target)), moment_index_valid(*source),
    ensures result ==> moment_index_valid(*final(target)),
        result ==> final(target).wf(),
        result ==> forall|customer: int, component: int| #[trigger] moment_at(*final(target), customer).component(component)
            == moment_at(*old(target), customer).component(component) + moment_at(*source, customer).component(component),
{
    let target_snapshot = target.share();
    let source_snapshot = source.share();
    proof { source.model(); }
    let ghost before = *target;
    let ghost entries = source.entries();
    let ghost processed = 0int;
    let mut cursor = source.cursor();
    loop
        invariant moment_index_valid(*target), target.wf(), before == *old(target),
            moment_index_valid(*source), source.wf(), entries == source.entries(),
            0 <= processed <= entries.len(), cursor.remaining() == entries.skip(processed),
            forall|position: int| #![trigger entries[position]]
                0 <= position < entries.len() ==> source.at(entries[position].0) == Some(entries[position].1),
            forall|customer: int, component: int| #[trigger] moment_at(*target, customer).component(component)
                == moment_at(before, customer).component(component) + moment_rows_sum(entries.take(processed), customer, component),
        decreases cursor.remaining().len(),
    {
        let (key, change) = match cursor.next() {
            Some(row) => row,
            None => {
                proof {
                    assert(processed == entries.len());
                    assert(entries.take(processed) =~= entries);
                    assert forall|customer: int, component: int| #[trigger] moment_at(*target, customer).component(component)
                        == moment_at(before, customer).component(component) + moment_at(*source, customer).component(component) by {
                        lemma_moment_rows(source, customer, component);
                    }
                }
                return true;
            },
        };
        proof {
            assert((key.view(), *change) == entries[processed]);
            assert(source.at(key.view()).is_some());
            assert(key.view().1 == Seq::<u8>::empty());
        }
        if !add_moments(target, key.number, *change) { return false; }
        proof {
            assert forall|customer: int, component: int| #[trigger] moment_at(*target, customer).component(component)
                == moment_at(before, customer).component(component) + moment_rows_sum(entries.take(processed + 1), customer, component) by {
                verus_common::sums::lemma_sum_take(entries, processed, moment_row_weight(customer, component));
            }
            processed = processed + 1;
        }
    }
}

pub fn coarsen_patches(patches: &Patches, grain: u8) -> (result: Option<Patches>)
    requires patches_valid(*patches), 0 <= grain <= 3,
    ensures result matches Some(coarse) ==> patches_valid(coarse),
        result matches Some(coarse) ==> forall|day: int, customer: int, component: int|
            #[trigger] patch_at(coarse, day, customer).component(component)
                == patch_rows_sum(patches.entries(), day, customer, grain as int, component),
{
    let mut coarse: Index<Index<Moments>> = Index::new();
    let snapshot = patches.share();
    proof {
        patches.model();
        assert forall|position: int| #![trigger patches.entries()[position]]
            0 <= position < patches.entries().len() implies i32::MIN <= patches.entries()[position].0.0 <= i32::MAX by {}
    }
    let ghost entries = patches.entries();
    let ghost processed = 0int;
    let mut outer = patches.cursor();
    loop
        invariant patches_valid(*patches), patches_valid(coarse), 0 <= grain <= 3,
            patches.wf(), entries == patches.entries(),
            0 <= processed <= entries.len(), outer.remaining() == entries.skip(processed),
            forall|position: int| #![trigger entries[position]]
                0 <= position < entries.len() ==> i32::MIN <= entries[position].0.0 <= i32::MAX,
            forall|position: int| #![trigger entries[position]]
                0 <= position < entries.len() ==> patches.at(entries[position].0) == Some(entries[position].1),
            forall|day: int, customer: int, component: int|
                #[trigger] patch_at(coarse, day, customer).component(component)
                    == patch_rows_sum(entries.take(processed), day, customer, grain as int, component),
        decreases outer.remaining().len(),
    {
        let (day, customers) = match outer.next() {
            Some(row) => row,
            None => {
                proof { assert(processed == entries.len()); assert(entries.take(processed) =~= entries); }
                return Some(coarse);
            },
        };
        proof {
            assert((day.view(), *customers) == entries[processed]);
            assert(day.view() == entries[processed].0);
            assert(patches.at(day.view()) == Some(*customers));
            assert(patches.at(day.view()).is_some());
            assert(moment_index_valid(patches.at(day.view()).unwrap()));
            assert(moment_index_valid(*customers));
        }
        let coarse_key = Key::number(period_key(day.number, grain));
        let ghost before_coarse = coarse;
        let mut coarse_cells = match coarse.get(&coarse_key) {
            Some(value) => {
                let shared = value.share();
                proof {
                    assert(moment_index_valid(*value));
                    assert forall|key: (int, Seq<u8>)| #[trigger] shared.at(key) == value.at(key) by {}
                    assert(moment_index_valid(shared));
                    assert forall|customer: int| #[trigger] moment_at(shared, customer)
                        == patch_at(before_coarse, coarse_key.number as int, customer) by {}
                }
                shared
            },
            None => Index::new(),
        };
        proof {
            assert forall|customer: int| #[trigger] moment_at(coarse_cells, customer)
                == patch_at(before_coarse, coarse_key.number as int, customer) by {}
        }
        if !merge_moment_patch(&mut coarse_cells, &*customers) { return None; }
        proof { coarse_cells.model(); }
        if coarse_cells.is_empty() { coarse.remove(&coarse_key); }
        else { coarse.put(coarse_key, Arc::new(coarse_cells)); }
        proof {
            assert forall|target_day: int, customer: int, component: int|
                #[trigger] patch_at(coarse, target_day, customer).component(component)
                    == patch_rows_sum(entries.take(processed + 1), target_day, customer, grain as int, component) by {
                if target_day == coarse_key.number {
                    assert(patch_at(coarse, target_day, customer) == moment_at(coarse_cells, customer));
                    assert(moment_at(coarse_cells, customer).component(component)
                        == patch_at(before_coarse, target_day, customer).component(component)
                            + moment_at(*customers, customer).component(component));
                } else {
                    assert(patch_at(coarse, target_day, customer) == patch_at(before_coarse, target_day, customer));
                }
                verus_common::sums::lemma_sum_take(entries, processed, patch_row_weight(target_day, customer, grain as int, component));
            }
            processed = processed + 1;
        }
    }
}

pub fn install_slice_patches(slices: &mut Index<Slice>, day: i64, customers: &Index<Moments>) -> (result: bool)
    requires slices_valid(*old(slices)), moment_index_valid(*customers),
    ensures result ==> slices_valid(*final(slices)),
        result ==> forall|target_day: int, customer: int, component: int|
            #[trigger] slice_at(*final(slices), target_day, customer).component(component)
                == slice_at(*old(slices), target_day, customer).component(component)
                    + if target_day == day { moment_at(*customers, customer).component(component) } else { 0 },
{
    let snapshot = customers.share();
    proof { customers.model(); }
    let ghost before = *slices;
    let ghost entries = customers.entries();
    let ghost processed = 0int;
    let mut cursor = customers.cursor();
    loop
        invariant slices_valid(*slices), before == *old(slices),
            customers.wf(), moment_index_valid(*customers), entries == customers.entries(),
            0 <= processed <= entries.len(), cursor.remaining() == entries.skip(processed),
            forall|position: int| #![trigger entries[position]]
                0 <= position < entries.len() ==> customers.at(entries[position].0) == Some(entries[position].1),
            forall|target_day: int, customer: int, component: int|
                #[trigger] slice_at(*slices, target_day, customer).component(component)
                    == slice_at(before, target_day, customer).component(component)
                        + if target_day == day { moment_rows_sum(entries.take(processed), customer, component) } else { 0 },
        decreases cursor.remaining().len(),
    {
        let (customer, change) = match cursor.next() {
            Some(row) => row,
            None => {
                proof {
                    assert(processed == entries.len());
                    assert(entries.take(processed) =~= entries);
                    assert forall|target_day: int, customer: int, component: int|
                        #[trigger] slice_at(*slices, target_day, customer).component(component)
                            == slice_at(before, target_day, customer).component(component)
                                + if target_day == day { moment_at(*customers, customer).component(component) } else { 0 } by {
                        lemma_moment_rows(customers, customer, component);
                    }
                }
                return true;
            },
        };
        proof {
            assert((customer.view(), *change) == entries[processed]);
            assert(customers.at(customer.view()).is_some());
            assert(customer.view().1 == Seq::<u8>::empty());
        }
        if !edit_slice(slices, day, customer.number, *change) { return false; }
        proof {
            assert forall|target_day: int, target: int, component: int|
                #[trigger] slice_at(*slices, target_day, target).component(component)
                    == slice_at(before, target_day, target).component(component)
                        + if target_day == day { moment_rows_sum(entries.take(processed + 1), target, component) } else { 0 } by {
                verus_common::sums::lemma_sum_take(entries, processed, moment_row_weight(target, component));
            }
            processed = processed + 1;
        }
    }
}

pub fn install_patches(slices: &mut Index<Slice>, patches: &Patches) -> (result: bool)
    requires slices_valid(*old(slices)), patches_valid(*patches),
    ensures result ==> slices_valid(*final(slices)),
        result ==> forall|day: int, customer: int, component: int|
            #[trigger] slice_at(*final(slices), day, customer).component(component)
                == slice_at(*old(slices), day, customer).component(component)
                    + patch_at(*patches, day, customer).component(component),
{
    let snapshot = patches.share();
    proof { patches.model(); }
    let ghost before = *slices;
    let ghost entries = patches.entries();
    let ghost processed = 0int;
    let mut outer = patches.cursor();
    loop
        invariant slices_valid(*slices),
            before == *old(slices), patches.wf(), patches_valid(*patches), entries == patches.entries(),
            0 <= processed <= entries.len(), outer.remaining() == entries.skip(processed),
            forall|position: int| #![trigger entries[position]]
                0 <= position < entries.len() ==> patches.at(entries[position].0) == Some(entries[position].1),
            forall|day: int, customer: int, component: int|
                #[trigger] slice_at(*slices, day, customer).component(component)
                    == slice_at(before, day, customer).component(component)
                        + patch_rows_sum(entries.take(processed), day, customer, 0, component),
        decreases outer.remaining().len(),
    {
        let (slice_key, customers) = match outer.next() {
            Some(row) => row,
            None => {
                proof {
                    assert(processed == entries.len());
                    assert(entries.take(processed) =~= entries);
                    assert forall|day: int, customer: int, component: int|
                        #[trigger] slice_at(*slices, day, customer).component(component)
                            == slice_at(before, day, customer).component(component)
                                + patch_at(*patches, day, customer).component(component) by {
                        lemma_patch_rows(patches, day, customer, component);
                    }
                }
                return true;
            },
        };
        proof {
            assert((slice_key.view(), *customers) == entries[processed]);
            assert(patches.at(slice_key.view()) == Some(*customers));
            assert(patches.at(slice_key.view()).is_some());
            assert(moment_index_valid(*customers));
        }
        if !install_slice_patches(slices, slice_key.number, &*customers) { return false; }
        proof {
            assert forall|day: int, customer: int, component: int|
                #[trigger] slice_at(*slices, day, customer).component(component)
                    == slice_at(before, day, customer).component(component)
                        + patch_rows_sum(entries.take(processed + 1), day, customer, 0, component) by {
                verus_common::sums::lemma_sum_take(entries, processed, patch_row_weight(day, customer, 0, component));
            }
            processed = processed + 1;
        }
    }
}

pub fn apply_patches(days: &mut Index<Slice>, periods: &mut Periods, patches: &Patches) -> (result: bool)
    requires slices_valid(*old(days)), slices_valid(old(periods).months),
        slices_valid(old(periods).quarters), slices_valid(old(periods).years), patches_valid(*patches),
    ensures result ==> slices_valid(*final(days)), result ==> slices_valid(final(periods).months),
        result ==> slices_valid(final(periods).quarters), result ==> slices_valid(final(periods).years),
        result ==> forall|day: int, customer: int, component: int|
            #[trigger] slice_at(*final(days), day, customer).component(component)
                == slice_at(*old(days), day, customer).component(component) + patch_at(*patches, day, customer).component(component),
        result ==> forall|day: int, customer: int, component: int|
            #[trigger] slice_at(final(periods).months, day, customer).component(component)
                == slice_at(old(periods).months, day, customer).component(component) + patch_rows_sum(patches.entries(), day, customer, 1, component),
        result ==> forall|day: int, customer: int, component: int|
            #[trigger] slice_at(final(periods).quarters, day, customer).component(component)
                == slice_at(old(periods).quarters, day, customer).component(component) + patch_rows_sum(patches.entries(), day, customer, 2, component),
        result ==> forall|day: int, customer: int, component: int|
            #[trigger] slice_at(final(periods).years, day, customer).component(component)
                == slice_at(old(periods).years, day, customer).component(component) + patch_rows_sum(patches.entries(), day, customer, 3, component),
{
    if patches.is_empty() {
        let snapshot = patches.share();
        proof {
            patches.model();
            assert forall|day: int, customer: int, component: int|
                #[trigger] patch_at(*patches, day, customer).component(component) == 0 by {
                lemma_patch_rows(patches, day, customer, component);
            }
        }
        return true;
    }
    if !install_patches(days, patches) { return false; }
    let months = match coarsen_patches(patches, 1) { Some(value) => value, None => return false };
    if !install_patches(&mut periods.months, &months) { return false; }
    let quarters = match coarsen_patches(patches, 2) { Some(value) => value, None => return false };
    if !install_patches(&mut periods.quarters, &quarters) { return false; }
    let years = match coarsen_patches(patches, 3) { Some(value) => value, None => return false };
    install_patches(&mut periods.years, &years)
}

#[verifier::rlimit(20)]
pub fn subtract_order_variant(target: &mut Index<OrderCell>, key: &Key, change: &OrderCell) -> (result: bool)
    requires order_leaf_valid(*old(target)), key.view() == order_key_view(change.customer as int, change.day as int),
    ensures result ==> order_leaf_valid(*final(target)),
        result ==> forall|day: int, customer: int, grain: int|
            #[trigger] order_group_sum(final(target).entries(), day, customer, grain)
                == order_group_sum(old(target).entries(), day, customer, grain)
                    - if change.customer as int == customer && period_of(change.day as int, grain) == day { change.count as int } else { 0 },
{
    let previous = target.share();
    proof {
        assert forall|query: (int, Seq<u8>)| #[trigger] previous.at(query) == target.at(query) by {}
    }
    let after_count = match target.get(key) {
        Some(value) => {
            proof {
                assert(previous.at(key.view()).is_some());
                assert(order_key_view(value.customer as int, value.day as int)
                    == order_key_view(change.customer as int, change.day as int));
                crate::key_facts::lemma_order_key_injective(value.customer, value.day, change.customer, change.day);
            }
            value.count
        },
        None => 0i128,
    };
    proof {
        assert(previous.at(key.view()).is_some() ==> {
            let cell = previous.at(key.view()).unwrap();
            &&& cell.customer == change.customer
            &&& cell.day == change.day
            &&& cell.count == after_count
        });
        assert(previous.at(key.view()).is_none() ==> after_count == 0);
    }
    let before_count = match after_count.checked_sub(change.count) { Some(value) => value, None => return false };
    if before_count < 0 { return false; }
    if before_count == 0 { target.remove(key); }
    else { target.put(key.share(), Arc::new(OrderCell {
        customer: change.customer, day: change.day, count: before_count,
    })); }
    let next = target.share();
    proof {
        assert forall|query: (int, Seq<u8>)| #[trigger] next.at(query) == target.at(query) by {}
        assert(order_leaf_valid(*target));
        assert forall|query: (int, Seq<u8>)| #![trigger previous.at(query), next.at(query)]
            query != key.view() implies previous.at(query) == next.at(query) by {
            assert(next.at(query) == target.at(query));
        }
        assert forall|day: int, customer: int, grain: int|
            #[trigger] order_group_sum(target.entries(), day, customer, grain)
                == order_group_sum(previous.entries(), day, customer, grain)
                    - if change.customer as int == customer && period_of(change.day as int, grain) == day { change.count as int } else { 0 } by {
            let weight = order_group_weight(day, customer, grain);
            verus_prooflib::ordered::lemma_sum_unchanged_except(&previous, &next, key.view(), weight);
        }
    }
    true
}

pub fn subtract_order_variants(target: &mut Index<OrderCell>, changes: &Index<OrderCell>) -> (result: bool)
    requires order_leaf_valid(*old(target)), order_leaf_valid(*changes),
    ensures result ==> order_leaf_valid(*final(target)),
        result ==> forall|day: int, customer: int, grain: int|
            #[trigger] order_group_sum(final(target).entries(), day, customer, grain)
                == order_group_sum(old(target).entries(), day, customer, grain)
                    - order_group_sum(changes.entries(), day, customer, grain),
{
    let changes_snapshot = changes.share();
    let target_snapshot = target.share();
    proof { changes.model(); }
    let ghost before = *target;
    let ghost entries = changes.entries();
    let ghost mut processed = 0int;
    let mut cursor = changes.cursor();
    loop
        invariant order_leaf_valid(*target), before == *old(target),
            changes.wf(), order_leaf_valid(*changes), entries == changes.entries(),
            0 <= processed <= entries.len(), cursor.remaining() == entries.skip(processed),
            forall|position: int| #![trigger entries[position]] 0 <= position < entries.len()
                ==> changes.at(entries[position].0) == Some(entries[position].1),
            forall|day: int, customer: int, grain: int|
                #[trigger] order_group_sum(target.entries(), day, customer, grain)
                    == order_group_sum(before.entries(), day, customer, grain)
                        - order_group_sum(entries.take(processed), day, customer, grain),
        decreases cursor.remaining().len(),
    {
        let (key, change) = match cursor.next() {
            Some(row) => row,
            None => {
                proof { assert(processed == entries.len()); assert(entries.take(processed) =~= entries); }
                return true;
            },
        };
        proof {
            assert((key.view(), *change) == entries[processed]);
            assert(changes.at(key.view()).is_some());
            assert(key.view() == order_key_view(change.customer as int, change.day as int));
        }
        if !subtract_order_variant(target, &key, &*change) { return false; }
        proof {
            assert forall|day: int, customer: int, grain: int|
                #[trigger] order_group_sum(target.entries(), day, customer, grain)
                    == order_group_sum(before.entries(), day, customer, grain)
                        - order_group_sum(entries.take(processed + 1), day, customer, grain) by {
                let weight = order_group_weight(day, customer, grain);
                verus_common::sums::lemma_sum_take(entries, processed, weight);
            }
            processed = processed + 1;
        }
    }
}

pub open spec fn order_group_at(orders: Orders, key: int, day: int, customer: int, grain: int) -> int {
    match orders.at((key, Seq::empty())) {
        Some(inner) => order_group_sum(inner.entries(), day, customer, grain),
        None => 0,
    }
}

pub open spec fn changed_key_weight(order_changes: Orders, line_changes: Index<Moments>, post_orders: Orders, post_lines: Index<Moments>,
    day: int, customer: int, grain: int, component: int) -> spec_fn(((int, Seq<u8>), ())) -> int {
    |entry: ((int, Seq<u8>), ())| {
        let delta = order_group_at(order_changes, entry.0.0, day, customer, grain);
        let post = order_group_at(post_orders, entry.0.0, day, customer, grain);
        delta * moment_at(post_lines, entry.0.0).component(component)
            + (post - delta) * moment_at(line_changes, entry.0.0).component(component)
    }
}

pub fn changed_patches(keys: &Index<()>, order_changes: &Orders, line_changes: &Index<Moments>,
    post_orders: &Orders, post_lines: &Index<Moments>) -> (result: Option<Patches>)
    requires orders_valid(*order_changes), orders_valid(*post_orders),
        forall|key: (int, Seq<u8>)| #[trigger] keys.at(key).is_some() ==> key.1 == Seq::<u8>::empty(),
    ensures result matches Some(patches) ==> patches_valid(patches),
        result matches Some(patches) ==> forall|day: int, customer: int, grain: int, component: int|
            #[trigger] patch_rows_sum(patches.entries(), day, customer, grain, component)
                == sum(keys.entries(), changed_key_weight(*order_changes, *line_changes, *post_orders, *post_lines,
                    day, customer, grain, component)),
{
    let key_snapshot = keys.share();
    proof { keys.model(); }
    let ghost entries = keys.entries();
    let ghost mut processed = 0int;
    let mut patches = Index::new();
    let mut cursor = keys.cursor();
    loop
        invariant patches_valid(patches), keys.wf(), entries == keys.entries(),
            orders_valid(*order_changes), orders_valid(*post_orders),
            0 <= processed <= entries.len(), cursor.remaining() == entries.skip(processed),
            forall|position: int| #![trigger entries[position]] 0 <= position < entries.len()
                ==> keys.at(entries[position].0).is_some(),
            forall|key: (int, Seq<u8>)| #[trigger] keys.at(key).is_some() ==> key.1 == Seq::<u8>::empty(),
            forall|day: int, customer: int, grain: int, component: int|
                #[trigger] patch_rows_sum(patches.entries(), day, customer, grain, component)
                    == sum(entries.take(processed), changed_key_weight(*order_changes, *line_changes, *post_orders, *post_lines,
                        day, customer, grain, component)),
        decreases cursor.remaining().len(),
    {
        let (key, _) = match cursor.next() {
            Some(row) => row,
            None => {
                proof { assert(processed == entries.len()); assert(entries.take(processed) =~= entries); }
                return Some(patches);
            },
        };
        proof { assert(key.view() == entries[processed].0); assert(key.view().1 == Seq::<u8>::empty()); }
        let lines_after = get_moments(post_lines, key.number);
        let delta_lines = get_moments(line_changes, key.number);
        let neg_lines = match delta_lines.scale(-1) { Some(value) => value, None => return None };
        let lines_before = match lines_after.add(neg_lines) { Some(value) => value, None => return None };
        if lines_before.count < 0 || lines_before.upper < 0 || lines_before.revenue > lines_before.upper
            || (lines_before.count == 0 && !lines_before.is_zero()) { return None; }
        let mut orders_before = match post_orders.get(&key) {
            Some(value) => {
                let shared = value.share();
                proof {
                    assert(post_orders.at(key.view()).is_some());
                    assert forall|query: (int, Seq<u8>)| #[trigger] shared.at(query) == value.at(query) by {}
                    assert(order_leaf_valid(shared));
                }
                shared
            },
            None => Index::new(),
        };
        proof {
            assert forall|day: int, customer: int, grain: int|
                #[trigger] order_group_sum(orders_before.entries(), day, customer, grain)
                    == order_group_at(*post_orders, key.number as int, day, customer, grain) by {}
        }
        let ghost original_patches = patches;
        match order_changes.get(&key) {
            Some(changes) => {
                proof { assert(order_changes.at(key.view()).is_some()); assert(order_leaf_valid(*changes)); }
                if !subtract_order_variants(&mut orders_before, &*changes) { return None; }
                if !join_patch(&mut patches, &*changes, lines_after) { return None; }
            },
            None => {},
        }
        proof {
            assert forall|day: int, customer: int, grain: int|
                #[trigger] order_group_sum(orders_before.entries(), day, customer, grain)
                    == order_group_at(*post_orders, key.number as int, day, customer, grain)
                        - order_group_at(*order_changes, key.number as int, day, customer, grain) by {}
            assert forall|day: int, customer: int, grain: int, component: int|
                #[trigger] patch_rows_sum(patches.entries(), day, customer, grain, component)
                    == patch_rows_sum(original_patches.entries(), day, customer, grain, component)
                        + order_group_at(*order_changes, key.number as int, day, customer, grain) * lines_after.component(component) by {}
        }
        if !join_patch(&mut patches, &orders_before, delta_lines) { return None; }
        proof {
            assert forall|day: int, customer: int, grain: int, component: int|
                #[trigger] patch_rows_sum(patches.entries(), day, customer, grain, component)
                    == sum(entries.take(processed + 1), changed_key_weight(*order_changes, *line_changes, *post_orders, *post_lines,
                        day, customer, grain, component)) by {
                verus_common::sums::lemma_sum_take(entries, processed,
                    changed_key_weight(*order_changes, *line_changes, *post_orders, *post_lines, day, customer, grain, component));
            }
            processed = processed + 1;
        }
    }
}

pub fn apply_dimensions(dimensions: &mut Dimensions, customers: &Vec<CustomerDelta>, nations: &Vec<NationDelta>) -> (result: bool)
    requires crate::dimension_updates::shape(*old(dimensions)),
    ensures result ==> crate::dimension_updates::shape(*final(dimensions)),
        result ==> forall|weight: spec_fn(crate::gen_spec::CustomerRow) -> int| #[trigger] table_sum(final(dimensions).customers, weight)
            == table_sum(old(dimensions).customers, weight)
                + sum(customers@, |delta: CustomerDelta| delta.sign as int * weight(customer_input_view(delta.row))),
        result ==> forall|row: crate::gen_spec::NationRow| #[trigger] nation_count(final(dimensions).nations, row)
            == nation_count(old(dimensions).nations, row)
                + sum(nations@, |delta: NationDelta| if nation_input_view(delta.row) == row { delta.sign as int } else { 0 }),
        result ==> forall|key: int| #[trigger] total_at(final(dimensions).nation_totals, key)
            == total_at(old(dimensions).nation_totals, key)
                + sum(nations@, |delta: NationDelta| if delta.row.n_nationkey == key { delta.sign as int } else { 0 }),
{
    let mut customer_changes = Index::new();
    let mut index = 0usize;
    while index < customers.len()
        invariant index <= customers.len(),
            crate::dimension_updates::shape(*dimensions), *dimensions == *old(dimensions),
            customer_index_valid(customer_changes),
            forall|row: crate::gen_spec::CustomerRow| #[trigger] customer_count(customer_changes, row)
                == sum(customers@.take(index as int), |delta: CustomerDelta|
                    if customer_input_view(delta.row) == row { delta.sign as int } else { 0 }),
        decreases customers.len() - index,
    {
        let ghost processed = index as int;
        if !change_customer(&mut customer_changes, &customers[index].row, customers[index].sign as i128) { return false; }
        proof {
            assert forall|row: crate::gen_spec::CustomerRow| #[trigger] customer_count(customer_changes, row)
                == sum(customers@.take(index as int + 1), |delta: CustomerDelta|
                    if customer_input_view(delta.row) == row { delta.sign as int } else { 0 }) by {
                verus_common::sums::lemma_sum_take(customers@, index as int,
                    |delta: CustomerDelta| if customer_input_view(delta.row) == row { delta.sign as int } else { 0 });
            }
        }
        index += 1;
    }
    proof { assert(customers@.take(index as int) =~= customers@); }
    let mut nation_changes = Index::new();
    index = 0;
    while index < nations.len()
        invariant index <= nations.len(),
            crate::dimension_updates::shape(*dimensions), *dimensions == *old(dimensions),
            nation_index_valid(nation_changes),
            customer_index_valid(customer_changes),
            forall|row: crate::gen_spec::CustomerRow| #[trigger] customer_count(customer_changes, row)
                == sum(customers@, |delta: CustomerDelta| if customer_input_view(delta.row) == row { delta.sign as int } else { 0 }),
            forall|row: crate::gen_spec::NationRow| #[trigger] nation_count(nation_changes, row)
                == sum(nations@.take(index as int), |delta: NationDelta|
                    if nation_input_view(delta.row) == row { delta.sign as int } else { 0 }),
        decreases nations.len() - index,
    {
        let ghost processed = index as int;
        if !change_nation(&mut nation_changes, &nations[index].row, nations[index].sign as i128) { return false; }
        proof {
            assert forall|row: crate::gen_spec::NationRow| #[trigger] nation_count(nation_changes, row)
                == sum(nations@.take(index as int + 1), |delta: NationDelta|
                    if nation_input_view(delta.row) == row { delta.sign as int } else { 0 }) by {
                verus_common::sums::lemma_sum_take(nations@, index as int,
                    |delta: NationDelta| if nation_input_view(delta.row) == row { delta.sign as int } else { 0 });
            }
        }
        index += 1;
    }
    let customer_snapshot = customer_changes.share();
    let nation_snapshot = nation_changes.share();
    proof {
        assert(nations@.take(index as int) =~= nations@);
        assert forall|weight: spec_fn(crate::gen_spec::CustomerRow) -> int|
            #[trigger] crate::dimension_updates::customer_index_sum(customer_changes.entries(), weight)
                == sum(customers@, |delta: CustomerDelta| delta.sign as int * weight(customer_input_view(delta.row))) by {
            crate::dimension_updates::lemma_customer_capture_sum(&customer_changes, customers@, weight);
        }
        assert forall|row: crate::gen_spec::NationRow|
            #[trigger] crate::dimension_updates::nation_index_sum(nation_changes.entries(), crate::dimension_updates::nation_eq(row))
                == sum(nations@, |delta: NationDelta| if nation_input_view(delta.row) == row { delta.sign as int } else { 0 }) by {
            let weight = crate::dimension_updates::nation_eq(row);
            crate::dimension_updates::lemma_nation_capture_sum(&nation_changes, nations@, weight);
            assert forall|delta: NationDelta| #[trigger] (delta.sign as int * weight(nation_input_view(delta.row)))
                == (if nation_input_view(delta.row) == row { delta.sign as int } else { 0 }) by {}
            verus_common::sums::lemma_sum_ext_all(nations@,
                |delta: NationDelta| delta.sign as int * weight(nation_input_view(delta.row)),
                |delta: NationDelta| if nation_input_view(delta.row) == row { delta.sign as int } else { 0 });
        }
        assert forall|key: int|
            #[trigger] crate::dimension_updates::nation_index_sum(nation_changes.entries(), crate::dimension_updates::nation_key_weight(key))
                == sum(nations@, |delta: NationDelta| if delta.row.n_nationkey == key { delta.sign as int } else { 0 }) by {
            let weight = crate::dimension_updates::nation_key_weight(key);
            crate::dimension_updates::lemma_nation_capture_sum(&nation_changes, nations@, weight);
            verus_common::sums::lemma_sum_ext_all(nations@,
                |delta: NationDelta| delta.sign as int * weight(nation_input_view(delta.row)),
                |delta: NationDelta| if delta.row.n_nationkey == key { delta.sign as int } else { 0 });
        }
    }
    if !crate::dimension_updates::install_customers(dimensions, &customer_changes) { return false; }
    crate::dimension_updates::install_nations(dimensions, &nation_changes)
}

}

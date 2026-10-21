use vstd::prelude::*;
use crate::custs::Custs;
use std::sync::Arc;
use verus_common::bytes::*;
use verus_common::sums::*;
use verus_prooflib::ordered::{Index, Key};
use crate::gen_obligations::*;
use crate::model::*;
use crate::packed::{PackedOrders, PackedShips, PackedGroups};
use crate::values::*;
use crate::values::{add, mul};
use verus_prooflib::wide::Wide;

verus! {

/// A shared buffer for a segment name: names seen before (a few) are reused, so the cells of
/// millions of customers share their handful of names.
pub fn intern(names: &mut Vec<Arc<Vec<u8>>>, bytes: &[u8]) -> (result: Arc<Vec<u8>>)
    ensures (*result)@ == bytes@,
{
    let mut index = 0;
    while index < names.len()
        invariant index <= names.len(),
        decreases names.len() - index,
    {
        if bytes_eq_exec(&names[index], bytes) { return names[index].clone(); }
        index += 1;
    }
    let fresh = Arc::new(copy_bytes(bytes));
    if names.len() < 32 { names.push(fresh.clone()); }
    fresh
}

pub fn empty_customer() -> (result: CustomerCell)
    ensures result.segments@ == Seq::empty(), result.orders@ == Seq::empty(),
{
    CustomerCell { segments: Vec::new(), orders: Vec::new() }
}

pub fn copy_customer(source: &CustomerCell) -> (result: CustomerCell)
    ensures result.segments@ == source.segments@, result.orders@ == source.orders@,
{
    let mut segments = Vec::new();
    let mut orders = Vec::new();
    let mut index = 0;
    while index < source.segments.len()
        invariant index <= source.segments.len(), segments@ == source.segments@.take(index as int),
        decreases source.segments.len() - index,
    {
        segments.push((source.segments[index].0.clone(), source.segments[index].1));
        index += 1;
    }
    index = 0;
    while index < source.orders.len()
        invariant index <= source.orders.len(), orders@ == source.orders@.take(index as int), segments@ == source.segments@,
        decreases source.orders.len() - index,
    {
        orders.push(source.orders[index]);
        index += 1;
    }
    CustomerCell { segments, orders }
}

/// The count stored for a segment is the segment's total, since segment names are distinct.
proof fn lemma_segment_at(rows: Seq<(Arc<Vec<u8>>, i128)>, low: int, segment: Seq<u8>)
    requires segments_sorted(rows), 0 <= low <= rows.len(),
        forall|index: int| #![trigger rows[index]] 0 <= index < low ==> bytes_lt((*rows[index].0)@, segment),
        forall|index: int| #![trigger rows[index]] low <= index < rows.len() ==> !bytes_lt((*rows[index].0)@, segment),
    ensures segment_count(rows, segment) == if low < rows.len() && (*rows[low].0)@ == segment { rows[low].1 as int } else { 0int },
{
    let weight = segment_weight(segment);
    assert forall|index: int| 0 <= index < rows.len() && (index != low || !(low < rows.len() && (*rows[low].0)@ == segment)) implies weight(#[trigger] rows[index]) == 0 by {
        if index < low { lemma_bytes_lt_irrefl(segment); }
        else if index > low { if (*rows[index].0)@ == segment { assert(bytes_lt((*rows[low].0)@, segment)); } }
    }
    if low < rows.len() && (*rows[low].0)@ == segment { lemma_sum_single_match(rows, weight, low); }
    else { lemma_sum_zero(rows, weight); }
}

/// In a sorted segment list each entry's count is its segment's total.
pub proof fn lemma_segment_entry(rows: Seq<(Arc<Vec<u8>>, i128)>, index: int)
    requires segments_sorted(rows), 0 <= index < rows.len(),
    ensures segment_count(rows, (*rows[index].0)@) == rows[index].1 as int,
{
    let weight = segment_weight((*rows[index].0)@);
    assert forall|other: int| 0 <= other < rows.len() && other != index implies weight(#[trigger] rows[other]) == 0 by {
        lemma_bytes_lt_irrefl((*rows[index].0)@);
        if other < index { assert(bytes_lt((*rows[other].0)@, (*rows[index].0)@)); }
        else { assert(bytes_lt((*rows[index].0)@, (*rows[other].0)@)); }
    }
    lemma_sum_single_match(rows, weight, index);
}

pub fn segment_change(cell: &mut CustomerCell, segment: &Arc<Vec<u8>>, delta: i128) -> (result: bool)
    requires
        segments_sorted(old(cell).segments@),
        forall|index: int| #![trigger old(cell).segments@[index]] 0 <= index < old(cell).segments@.len() ==> old(cell).segments@[index].1 != 0,
    ensures
        final(cell).orders@ == old(cell).orders@,
        segments_sorted(final(cell).segments@),
        forall|index: int| #![trigger final(cell).segments@[index]] 0 <= index < final(cell).segments@.len() ==> final(cell).segments@[index].1 != 0,
        !result ==> final(cell).segments@ == old(cell).segments@,
        !result ==> segment_count(old(cell).segments@, segment@) + delta as int > i128::MAX as int
            || segment_count(old(cell).segments@, segment@) + (delta as int) < i128::MIN as int,
        result ==> forall|target: Seq<u8>| #![trigger segment_count(final(cell).segments@, target)]
            segment_count(final(cell).segments@, target) == segment_count(old(cell).segments@, target)
                + if target == segment@ { delta as int } else { 0 },
        delta >= 0 && (forall|index: int| #![trigger old(cell).segments@[index]] 0 <= index < old(cell).segments@.len() ==> old(cell).segments@[index].1 > 0) ==>
            forall|index: int| #![trigger final(cell).segments@[index]] 0 <= index < final(cell).segments@.len() ==> final(cell).segments@[index].1 > 0,
{
    let ghost before = cell.segments@;
    let mut low = 0;
    let mut high = cell.segments.len();
    while low < high
        invariant
            low <= high <= cell.segments.len(), cell.segments@ == before, segments_sorted(before),
            forall|index: int| #![trigger before[index]] 0 <= index < low ==> bytes_lt((*before[index].0)@, segment@),
            forall|index: int| #![trigger before[index]] high <= index < before.len() ==> !bytes_lt((*before[index].0)@, segment@),
        decreases high - low,
    {
        let middle = low + (high - low) / 2;
        if bytes_lt_exec(&cell.segments[middle].0, segment) {
            proof {
                assert forall|index: int| #![trigger before[index]] 0 <= index <= middle implies bytes_lt((*before[index].0)@, segment@) by {
                    if index < middle { lemma_bytes_lt_trans((*before[index].0)@, (*before[middle as int].0)@, segment@); }
                }
            }
            low = middle + 1;
        } else {
            proof {
                assert forall|index: int| #![trigger before[index]] middle <= index < before.len() implies !bytes_lt((*before[index].0)@, segment@) by {
                    if index > middle && bytes_lt((*before[index].0)@, segment@) {
                        lemma_bytes_lt_trans((*before[middle as int].0)@, (*before[index].0)@, segment@);
                    }
                }
            }
            high = middle;
        }
    }
    let old = if low < cell.segments.len() && bytes_eq_exec(&cell.segments[low].0, segment) {
        cell.segments[low].1
    } else { 0 };
    proof { lemma_segment_at(before, low as int, segment@); }
    let count = match old.checked_add(delta) { Some(value) => value, None => return false };
    let ghost less = |left: (Arc<Vec<u8>>, i128), right: (Arc<Vec<u8>>, i128)| bytes_lt((*left.0)@, (*right.0)@);
    if old != 0 {
        if count == 0 {
            cell.segments.remove(low);
            proof { verus_prooflib::lemma_sorted_remove_at(before, low as int, less); }
        }
        else {
            let text = cell.segments[low].0.clone();
            cell.segments.set(low, (text, count));
            proof { verus_prooflib::lemma_sorted_update_at(before, low as int, cell.segments@[low as int], less); }
        }
    } else if count != 0 {
        cell.segments.insert(low, (segment.clone(), count));
        proof {
            assert forall|index: int| #![trigger before[index]] low <= index < before.len() implies bytes_lt(segment@, (*before[index].0)@) by {
                if (*before[index].0)@ == segment@ {
                    if index == low {} else { assert(bytes_lt((*before[low as int].0)@, segment@)); }
                }
                lemma_bytes_lt_total(segment@, (*before[index].0)@);
            }
            verus_prooflib::lemma_sorted_insert_at(before, low as int, cell.segments@[low as int], less);
        }
    }
    proof {
        assert forall|target: Seq<u8>| #![trigger segment_count(cell.segments@, target)]
            segment_count(cell.segments@, target) == segment_count(before, target)
                + if target == segment@ { delta as int } else { 0 } by {
            let weight = |entry: (Arc<Vec<u8>>, i128)| if (*entry.0)@ == target { entry.1 as int } else { 0 };
            if old != 0 {
                if count == 0 { lemma_sum_remove(before, low as int, weight); }
                else { lemma_sum_update(before, low as int, cell.segments@[low as int], weight); }
            } else if count != 0 {
                verus_prooflib::lemma_sum_insert(before, low as int, cell.segments@[low as int], weight);
            }
        }
        assert forall|index: int| #![trigger cell.segments@[index]] 0 <= index < cell.segments@.len() implies cell.segments@[index].1 != 0 by {
            if old != 0 && count == 0 {
                let prior = if index < low { index } else { index + 1 };
                assert(cell.segments@[index] == before[prior]);
            } else if old != 0 && count != 0 {
                if index != low { assert(cell.segments@[index] == before[index]); }
            } else if count != 0 {
                if index != low {
                    let prior = if index < low { index } else { index - 1 };
                    assert(cell.segments@[index] == before[prior]);
                }
            }
        }
    }
    true
}

/// The count stored for an order key is the key's total, since the keys are distinct.
proof fn lemma_reverse_at(rows: Seq<(i64, i64)>, low: int, key: i64)
    requires reverse_sorted(rows), 0 <= low <= rows.len(),
        forall|index: int| #![trigger rows[index]] 0 <= index < low ==> rows[index].0 < key,
        forall|index: int| #![trigger rows[index]] low <= index < rows.len() ==> rows[index].0 >= key,
    ensures reverse_count(rows, key as int) == if low < rows.len() && rows[low].0 == key { rows[low].1 as int } else { 0int },
{
    let weight = reverse_weight(key as int);
    assert forall|index: int| 0 <= index < rows.len() && (index != low || !(low < rows.len() && rows[low].0 == key)) implies weight(#[trigger] rows[index]) == 0 by {
        if index > low { assert(rows[low].0 < rows[index].0); }
    }
    if low < rows.len() && rows[low].0 == key { lemma_sum_single_match(rows, weight, low); }
    else { lemma_sum_zero(rows, weight); }
}

/// In a sorted reverse list each entry's count is its key's total.
pub proof fn lemma_reverse_entry(rows: Seq<(i64, i64)>, index: int)
    requires reverse_sorted(rows), 0 <= index < rows.len(),
    ensures reverse_count(rows, rows[index].0 as int) == rows[index].1 as int,
{
    let weight = reverse_weight(rows[index].0 as int);
    assert forall|other: int| 0 <= other < rows.len() && other != index implies weight(#[trigger] rows[other]) == 0 by {
        if other < index { assert(rows[other].0 < rows[index].0); }
        else { assert(rows[index].0 < rows[other].0); }
    }
    lemma_sum_single_match(rows, weight, index);
}

pub fn reverse_change(cell: &mut CustomerCell, key: i64, delta: i64) -> (result: bool)
    requires
        reverse_sorted(old(cell).orders@),
        forall|index: int| #![trigger old(cell).orders@[index]] 0 <= index < old(cell).orders@.len() ==> old(cell).orders@[index].1 != 0,
    ensures
        final(cell).segments@ == old(cell).segments@,
        reverse_sorted(final(cell).orders@),
        forall|index: int| #![trigger final(cell).orders@[index]] 0 <= index < final(cell).orders@.len() ==> final(cell).orders@[index].1 != 0,
        !result ==> final(cell).orders@ == old(cell).orders@,
        !result ==> reverse_count(old(cell).orders@, key as int) + delta as int > i64::MAX as int
            || reverse_count(old(cell).orders@, key as int) + (delta as int) < i64::MIN as int,
        result ==> forall|target: int| #![trigger reverse_count(final(cell).orders@, target)]
            reverse_count(final(cell).orders@, target) == reverse_count(old(cell).orders@, target)
                + if target == key { delta as int } else { 0 },
        delta >= 0 && (forall|index: int| #![trigger old(cell).orders@[index]] 0 <= index < old(cell).orders@.len() ==> old(cell).orders@[index].1 > 0) ==>
            forall|index: int| #![trigger final(cell).orders@[index]] 0 <= index < final(cell).orders@.len() ==> final(cell).orders@[index].1 > 0,
{
    let ghost before = cell.orders@;
    let mut low = 0;
    let mut high = cell.orders.len();
    while low < high
        invariant
            low <= high <= cell.orders.len(), cell.orders@ == before, reverse_sorted(before),
            forall|index: int| #![trigger before[index]] 0 <= index < low ==> before[index].0 < key,
            forall|index: int| #![trigger before[index]] high <= index < before.len() ==> before[index].0 >= key,
        decreases high - low,
    {
        let middle = low + (high - low) / 2;
        if cell.orders[middle].0 < key { low = middle + 1; }
        else { high = middle; }
    }
    let old = if low < cell.orders.len() && cell.orders[low].0 == key { cell.orders[low].1 } else { 0 };
    proof { lemma_reverse_at(before, low as int, key); }
    let count = match old.checked_add(delta) { Some(value) => value, None => return false };
    let ghost less = |left: (i64, i64), right: (i64, i64)| left.0 < right.0;
    if old != 0 {
        if count == 0 {
            cell.orders.remove(low);
            proof { verus_prooflib::lemma_sorted_remove_at(before, low as int, less); }
        } else {
            cell.orders.set(low, (key, count));
            proof { verus_prooflib::lemma_sorted_update_at(before, low as int, (key, count), less); }
        }
    } else if count != 0 {
        cell.orders.insert(low, (key, count));
        proof {
            assert forall|index: int| #![trigger before[index]] low <= index < before.len() implies before[index].0 > key by {
                if index > low { assert(before[low as int].0 < before[index].0); }
            }
            verus_prooflib::lemma_sorted_insert_at(before, low as int, (key, count), less);
        }
    }
    proof {
        assert forall|target: int| #![trigger reverse_count(cell.orders@, target)]
            reverse_count(cell.orders@, target) == reverse_count(before, target)
                + if target == key { delta as int } else { 0 } by {
            let weight = |entry: (i64, i64)| if entry.0 == target { entry.1 as int } else { 0 };
            if old != 0 {
                if count == 0 { lemma_sum_remove(before, low as int, weight); }
                else { lemma_sum_update(before, low as int, (key, count), weight); }
            } else if count != 0 { verus_prooflib::lemma_sum_insert(before, low as int, (key, count), weight); }
        }
        assert forall|index: int| #![trigger cell.orders@[index]] 0 <= index < cell.orders@.len() implies cell.orders@[index].1 != 0 by {
            if old != 0 && count == 0 {
                let prior = if index < low { index } else { index + 1 };
                assert(cell.orders@[index] == before[prior]);
            } else if old != 0 && count != 0 {
                if index != low { assert(cell.orders@[index] == before[index]); }
            } else if count != 0 {
                if index != low {
                    let prior = if index < low { index } else { index - 1 };
                    assert(cell.orders@[index] == before[prior]);
                }
            }
        }
    }
    true
}

pub fn empty_page() -> (result: PageWork)
    ensures result.orders@ == Seq::empty(), result.ships@ == Seq::empty(),
{
    PageWork { orders: Vec::new(), ships: Vec::new() }
}

pub fn copy_orders(source: &Vec<OrderCell>) -> (result: Vec<OrderCell>)
    ensures result@ == source@,
{
    let mut result = Vec::new();
    let mut index = 0;
    while index < source.len()
        invariant index <= source.len(), result@ == source@.take(index as int),
        decreases source.len() - index,
    {
        result.push(source[index]);
        index += 1;
    }
    result
}

pub fn copy_ships(source: &Vec<ShipCell>) -> (result: Vec<ShipCell>)
    ensures result@ == source@,
{
    let mut result = Vec::new();
    let mut index = 0;
    while index < source.len()
        invariant index <= source.len(), result@ == source@.take(index as int),
        decreases source.len() - index,
    {
        result.push(source[index].dup());
        proof { assert(source@.take(index as int + 1) =~= source@.take(index as int).push(source@[index as int])); }
        index += 1;
    }
    proof { assert(source@.take(index as int) =~= source@); }
    result
}

pub fn order_less(left: &OrderCell, right: &OrderCell) -> (result: bool)
    ensures result == order_lt(*left, *right),
{
    left.key < right.key || (left.key == right.key && (left.customer < right.customer
        || (left.customer == right.customer && (left.date < right.date
        || (left.date == right.date && left.priority < right.priority)))))
}

pub fn insert_order(rows: &mut Vec<OrderCell>, row: OrderInput) -> (result: bool)
    requires orders_sorted(old(rows)@),
        forall|index: int| #![trigger old(rows)@[index]] 0 <= index < old(rows)@.len() ==> old(rows)@[index].count > 0,
    ensures
        orders_sorted(final(rows)@),
        forall|index: int| #![trigger final(rows)@[index]] 0 <= index < final(rows)@.len() ==> final(rows)@[index].count > 0,
        !result ==> final(rows)@ == old(rows)@,
        !result ==> order_count(old(rows)@, row.o_orderkey as int, row.o_custkey as int, row.o_orderdate as int, row.o_shippriority as int) >= i64::MAX as int,
        forall|page: int| #![trigger orders_owner(final(rows)@, page)]
            orders_owner(old(rows)@, page) && row.o_orderkey as int / 256 == page ==> orders_owner(final(rows)@, page),
        result ==> forall|key: int, customer: int, date: int, priority: int| #![trigger order_count(final(rows)@, key, customer, date, priority)]
            order_count(final(rows)@, key, customer, date, priority) == order_count(old(rows)@, key, customer, date, priority)
                + if key == row.o_orderkey && customer == row.o_custkey && date == row.o_orderdate && priority == row.o_shippriority { 1int } else { 0int },
{
    let ghost before = rows@;
    let mut value = OrderCell { key: row.o_orderkey, customer: row.o_custkey,
        date: row.o_orderdate, priority: row.o_shippriority, count: 1 };
    let mut low = 0;
    let mut high = rows.len();
    while low < high
        invariant low <= high <= rows.len(), rows@ == before, orders_sorted(before),
            forall|index: int| #![trigger before[index]] 0 <= index < low ==> order_lt(before[index], value),
            forall|index: int| #![trigger before[index]] high <= index < before.len() ==> !order_lt(before[index], value),
        decreases high - low,
    {
        let middle = low + (high - low) / 2;
        if order_less(&rows[middle], &value) { low = middle + 1; }
        else { high = middle; }
    }
    let found = low < rows.len() && !order_less(&value, &rows[low]);
    if found {
        value.count = match rows[low].count.checked_add(1) {
            Some(count) => count,
            None => {
                proof {
                    let identity = |entry: OrderCell| if entry.key == row.o_orderkey && entry.customer == row.o_custkey
                        && entry.date == row.o_orderdate && entry.priority == row.o_shippriority { entry.count as int } else { 0int };
                    assert forall|index: int| 0 <= index < before.len() implies identity(#[trigger] before[index]) >= 0 by {}
                    lemma_sum_ge_term(before, identity, low as int);
                }
                return false;
            },
        };
        rows.set(low, value);
        proof { verus_prooflib::lemma_sorted_update_at(before, low as int, value, |left: OrderCell, right: OrderCell| order_lt(left, right)); }
    } else {
        rows.insert(low, value);
        proof {
            assert forall|index: int| #![trigger before[index]] low <= index < before.len() implies order_lt(value, before[index]) by {
                if index > low { assert(order_lt(before[low as int], before[index])); }
            }
            verus_prooflib::lemma_sorted_insert_at(before, low as int, value, |left: OrderCell, right: OrderCell| order_lt(left, right));
        }
    }
    proof {
        assert forall|key: int, customer: int, date: int, priority: int| #![trigger order_count(rows@, key, customer, date, priority)]
            order_count(rows@, key, customer, date, priority) == order_count(before, key, customer, date, priority)
                + if key == row.o_orderkey && customer == row.o_custkey && date == row.o_orderdate && priority == row.o_shippriority { 1int } else { 0int } by {
            let weight = |entry: OrderCell| if entry.key == key && entry.customer == customer && entry.date == date && entry.priority == priority { entry.count as int } else { 0 };
            if found { lemma_sum_update(before, low as int, value, weight); }
            else { verus_prooflib::lemma_sum_insert(before, low as int, value, weight); }
        }
        assert forall|index: int| #![trigger rows@[index]] 0 <= index < rows@.len() implies rows@[index].count > 0 by {
            if index != low {
                let prior = if found || index < low { index } else { index - 1 };
                assert(rows@[index] == before[prior]);
            }
        }
        assert forall|page: int| #![trigger orders_owner(rows@, page)] orders_owner(before, page) && row.o_orderkey as int / 256 == page implies orders_owner(rows@, page) by {
            assert forall|index: int| #![trigger rows@[index]] 0 <= index < rows@.len() implies rows@[index].key as int / 256 == page by {
                if index != low {
                    let prior = if found || index < low { index } else { index - 1 };
                    assert(rows@[index] == before[prior]);
                }
            }
        }
    }
    true
}

pub fn insert_ship(rows: &mut Vec<ShipCell>, row: ShipmentInput) -> (result: bool)
    requires ships_sorted(old(rows)@),
        forall|index: int| #![trigger old(rows)@[index]] 0 <= index < old(rows)@.len() ==> ship_ok(old(rows)@[index]),
    ensures ships_sorted(final(rows)@),
        forall|index: int| #![trigger final(rows)@[index]] 0 <= index < final(rows)@.len() ==> ship_ok(final(rows)@[index]),
        !result ==> final(rows)@ == old(rows)@,
        !result ==> ship_count(old(rows)@, row.l_orderkey as int, row.l_shipdate as int, 0) >= i64::MAX as int,
        forall|page: int| #![trigger ships_owner(final(rows)@, page)]
            ships_owner(old(rows)@, page) && row.l_orderkey as int / 256 == page ==> ships_owner(final(rows)@, page),
        result ==> forall|key: int, date: int, kind: int| #![trigger ship_count(final(rows)@, key, date, kind)]
            0 <= kind <= 2 ==> ship_count(final(rows)@, key, date, kind) == ship_count(old(rows)@, key, date, kind)
                + if key == row.l_orderkey && date == row.l_shipdate {
                    let value = row.l_extendedprice as int * (100 - row.l_discount as int);
                    if kind == 0 { 1 } else if kind == 1 { value } else { if value > 0 { value } else { 0 } }
                } else { 0 },
{
    let ghost before = rows@;
    let revenue = revenue(row.l_extendedprice, row.l_discount);
    let mut value = ShipCell { key: row.l_orderkey, date: row.l_shipdate,
        count: 1, revenue: Wide::from_i128(revenue), positive: Wide::from_i128(if revenue > 0 { revenue } else { 0 }) };
    let mut low = 0;
    let mut high = rows.len();
    while low < high
        invariant low <= high <= rows.len(), rows@ == before, ships_sorted(before),
            forall|index: int| #![trigger before[index]] 0 <= index < low ==> ship_lt(before[index], value),
            forall|index: int| #![trigger before[index]] high <= index < before.len() ==> !ship_lt(before[index], value),
        decreases high - low,
    {
        let middle = low + (high - low) / 2;
        if rows[middle].key < value.key || (rows[middle].key == value.key && rows[middle].date < value.date) {
            low = middle + 1;
        } else { high = middle; }
    }
    let found = low < rows.len() && rows[low].key == value.key && rows[low].date == value.date;
    if found {
        value.count = match rows[low].count.checked_add(1) {
            Some(count) => count,
            None => {
                proof {
                    let identity = |entry: ShipCell| if entry.key == row.l_orderkey && entry.date == row.l_shipdate { entry.count as int } else { 0int };
                    assert forall|index: int| 0 <= index < before.len() implies identity(#[trigger] before[index]) >= 0 by { assert(ship_ok(before[index])); }
                    lemma_sum_ge_term(before, identity, low as int);
                    let (key, date, kind) = (row.l_orderkey as int, row.l_shipdate as int, 0int);
                    let measure = |entry: ShipCell| if entry.key == key && entry.date == date {
                        if kind == 0 { entry.count as int } else if kind == 1 { entry.revenue@ } else { entry.positive@ }
                    } else { 0 };
                    lemma_sum_ext_all(before, measure, identity);
                    assert(ship_count(before, key, date, kind) == sum(before, measure));
                }
                return false;
            },
        };
        proof { assert(ship_ok(rows@[low as int])); }
        value.revenue = rows[low].revenue.add(&value.revenue);
        value.positive = rows[low].positive.add(&value.positive);
        rows.set(low, value);
        proof { verus_prooflib::lemma_sorted_update_at(before, low as int, value, |left: ShipCell, right: ShipCell| ship_lt(left, right)); }
    } else {
        rows.insert(low, value);
        proof {
            assert forall|index: int| #![trigger before[index]] low <= index < before.len() implies ship_lt(value, before[index]) by {
                if index > low { assert(ship_lt(before[low as int], before[index])); }
            }
            verus_prooflib::lemma_sorted_insert_at(before, low as int, value, |left: ShipCell, right: ShipCell| ship_lt(left, right));
        }
    }
    proof {
        assert forall|key: int, date: int, kind: int| #![trigger ship_count(rows@, key, date, kind)]
            0 <= kind <= 2 implies ship_count(rows@, key, date, kind) == ship_count(before, key, date, kind)
                + if key == row.l_orderkey && date == row.l_shipdate {
                    let value = row.l_extendedprice as int * (100 - row.l_discount as int);
                    if kind == 0 { 1 } else if kind == 1 { value } else { if value > 0 { value } else { 0 } }
                } else { 0 } by {
            let weight = |entry: ShipCell| if entry.key == key && entry.date == date {
                if kind == 0 { entry.count as int } else if kind == 1 { entry.revenue@ } else { entry.positive@ }
            } else { 0 };
            if found { lemma_sum_update(before, low as int, value, weight); }
            else { verus_prooflib::lemma_sum_insert(before, low as int, value, weight); }
        }
        assert forall|index: int| #![trigger rows@[index]] 0 <= index < rows@.len() implies ship_ok(rows@[index]) by {
            if index != low {
                let prior = if found || index < low { index } else { index - 1 };
                assert(rows@[index] == before[prior]);
            }
        }
        assert forall|page: int| #![trigger ships_owner(rows@, page)] ships_owner(before, page) && row.l_orderkey as int / 256 == page implies ships_owner(rows@, page) by {
            assert forall|index: int| #![trigger rows@[index]] 0 <= index < rows@.len() implies rows@[index].key as int / 256 == page by {
                if index != low {
                    let prior = if found || index < low { index } else { index - 1 };
                    assert(rows@[index] == before[prior]);
                }
            }
        }
    }
    true
}

pub fn orders_start(rows: &Vec<OrderCell>, key: i64) -> (result: usize)
    requires orders_sorted(rows@),
    ensures result <= rows.len(),
        forall|index: int| #![trigger rows@[index]] 0 <= index < result ==> rows@[index].key < key,
        forall|index: int| #![trigger rows@[index]] result <= index < rows@.len() ==> rows@[index].key >= key,
{
    let mut low = 0;
    let mut high = rows.len();
    while low < high
        invariant low <= high <= rows.len(), orders_sorted(rows@),
            forall|index: int| #![trigger rows@[index]] 0 <= index < low ==> rows@[index].key < key,
            forall|index: int| #![trigger rows@[index]] high <= index < rows@.len() ==> rows@[index].key >= key,
        decreases high - low,
    {
        let middle = low + (high - low) / 2;
        if rows[middle].key < key { low = middle + 1; } else { high = middle; }
    }
    low
}

pub fn ships_start(rows: &Vec<ShipCell>, key: i64) -> (result: usize)
    requires ships_sorted(rows@),
    ensures result <= rows.len(),
        forall|index: int| #![trigger rows@[index]] 0 <= index < result ==> rows@[index].key < key,
        forall|index: int| #![trigger rows@[index]] result <= index < rows@.len() ==> rows@[index].key >= key,
{
    let mut low = 0;
    let mut high = rows.len();
    while low < high
        invariant low <= high <= rows.len(), ships_sorted(rows@),
            forall|index: int| #![trigger rows@[index]] 0 <= index < low ==> rows@[index].key < key,
            forall|index: int| #![trigger rows@[index]] high <= index < rows@.len() ==> rows@[index].key >= key,
        decreases high - low,
    {
        let middle = low + (high - low) / 2;
        if rows[middle].key < key { low = middle + 1; } else { high = middle; }
    }
    low
}

pub fn ships_start_packed(rows: &PackedShips, key: i64) -> (result: usize)
    requires ships_sorted(rows@),
    ensures result <= rows@.len(),
        forall|index: int| #![trigger rows@[index]] 0 <= index < result ==> rows@[index].key < key,
        forall|index: int| #![trigger rows@[index]] result <= index < rows@.len() ==> rows@[index].key >= key,
{
    let mut low = 0;
    let mut high = rows.len();
    while low < high
        invariant low <= high <= rows@.len(), ships_sorted(rows@),
            forall|index: int| #![trigger rows@[index]] 0 <= index < low ==> rows@[index].key < key,
            forall|index: int| #![trigger rows@[index]] high <= index < rows@.len() ==> rows@[index].key >= key,
        decreases high - low,
    {
        let middle = low + (high - low) / 2;
        if rows.get(middle).key < key { low = middle + 1; } else { high = middle; }
    }
    low
}

pub fn copy_groups(source: &Vec<GroupCell>) -> (result: Vec<GroupCell>)
    ensures result@ == source@,
{
    let mut result = Vec::with_capacity(source.len());
    let mut index = 0;
    while index < source.len()
        invariant index <= source.len(), result@ == source@.take(index as int),
        decreases source.len() - index,
    {
        let group = &source[index];
        result.push(GroupCell { segment: group.segment.clone(), id: group.id, weight: group.weight.dup(),
            bound: group.bound, last_ship: group.last_ship });
        proof { assert(source@.take(index as int + 1) =~= source@.take(index as int).push(source@[index as int])); }
        index += 1;
    }
    proof { assert(source@.take(index as int) =~= source@); }
    result
}

pub fn group_less(segment: &[u8], id: GroupId, right: &GroupCell) -> bool {
    bytes_lt_exec(segment, &right.segment) || (bytes_eq_exec(segment, &right.segment)
        && (id.key < right.id.key || (id.key == right.id.key && (id.date < right.id.date
        || (id.date == right.id.date && id.priority < right.id.priority)))))
}

pub fn group_start(rows: &Vec<GroupCell>, segment: &[u8], id: GroupId) -> (result: usize)
    ensures result <= rows.len(),
        groups_sorted(rows@) ==> forall|index: int| #![trigger rows@[index]] 0 <= index < result ==> group_before_id(rows@[index], segment@, id),
        groups_sorted(rows@) ==> forall|index: int| #![trigger rows@[index]] result <= index < rows@.len() ==> !group_before_id(rows@[index], segment@, id),
{
    let mut low = 0;
    let mut high = rows.len();
    while low < high
        invariant low <= high <= rows.len(),
            groups_sorted(rows@) ==> forall|index: int| #![trigger rows@[index]] 0 <= index < low ==> group_before_id(rows@[index], segment@, id),
            groups_sorted(rows@) ==> forall|index: int| #![trigger rows@[index]] high <= index < rows@.len() ==> !group_before_id(rows@[index], segment@, id),
        decreases high - low,
    {
        let middle = low + (high - low) / 2;
        let stored = &rows[middle];
        if bytes_lt_exec(&stored.segment, segment) || (bytes_eq_exec(&stored.segment, segment)
            && (stored.id.key < id.key || (stored.id.key == id.key && (stored.id.date < id.date
            || (stored.id.date == id.date && stored.id.priority < id.priority))))) {
            proof {
                if groups_sorted(rows@) {
                    assert forall|index: int| #![trigger rows@[index]] 0 <= index <= middle implies group_before_id(rows@[index], segment@, id) by {
                        if index < middle {
                            lemma_group_search_order(rows@[index], rows@[middle as int], segment@, id);
                        }
                    }
                }
            }
            low = middle + 1;
        } else {
            proof {
                if groups_sorted(rows@) {
                    assert forall|index: int| #![trigger rows@[index]] middle <= index < rows@.len() implies !group_before_id(rows@[index], segment@, id) by {
                        if index > middle {
                            lemma_group_search_order(rows@[middle as int], rows@[index], segment@, id);
                        }
                    }
                }
            }
            high = middle;
        }
    }
    low
}

pub fn group_start_packed(rows: &PackedGroups, segment: &[u8], id: GroupId) -> (result: usize)
    ensures result <= rows@.len(),
        groups_sorted(rows@) ==> forall|index: int| #![trigger rows@[index]] 0 <= index < result ==> group_before_id(rows@[index], segment@, id),
        groups_sorted(rows@) ==> forall|index: int| #![trigger rows@[index]] result <= index < rows@.len() ==> !group_before_id(rows@[index], segment@, id),
{
    let mut low = 0;
    let mut high = rows.len();
    while low < high
        invariant low <= high <= rows@.len(),
            groups_sorted(rows@) ==> forall|index: int| #![trigger rows@[index]] 0 <= index < low ==> group_before_id(rows@[index], segment@, id),
            groups_sorted(rows@) ==> forall|index: int| #![trigger rows@[index]] high <= index < rows@.len() ==> !group_before_id(rows@[index], segment@, id),
        decreases high - low,
    {
        let middle = low + (high - low) / 2;
        if rows.before_id(middle, segment, id) {
            proof {
                if groups_sorted(rows@) {
                    assert forall|index: int| #![trigger rows@[index]] 0 <= index <= middle implies group_before_id(rows@[index], segment@, id) by {
                        if index < middle {
                            lemma_group_search_order(rows@[index], rows@[middle as int], segment@, id);
                        }
                    }
                }
            }
            low = middle + 1;
        } else {
            proof {
                if groups_sorted(rows@) {
                    assert forall|index: int| #![trigger rows@[index]] middle <= index < rows@.len() implies !group_before_id(rows@[index], segment@, id) by {
                        if index > middle {
                            lemma_group_search_order(rows@[middle as int], rows@[index], segment@, id);
                        }
                    }
                }
            }
            high = middle;
        }
    }
    low
}

pub proof fn lemma_group_search_order(left: GroupCell, right: GroupCell, segment: Seq<u8>, id: GroupId)
    requires group_lt(left, right),
    ensures group_before_id(right, segment, id) ==> group_before_id(left, segment, id),
        !group_before_id(left, segment, id) ==> !group_before_id(right, segment, id),
{
    if bytes_lt((*left.segment)@, (*right.segment)@) && bytes_lt((*right.segment)@, segment) {
        lemma_bytes_lt_trans((*left.segment)@, (*right.segment)@, segment);
    }
}

/// weight * positive, capped to the largest rank bound.
pub fn capped_product(weight: &Wide, positive: &Wide) -> (result: i64)
    requires weight@ > 0, positive@ >= 0,
    ensures result as int == capped(weight@ * positive@), result >= 0,
{
    let product = weight.mul(positive);
    if Wide::from_i128(i64::MAX as i128).lt(&product) { return i64::MAX; }
    match product.to_i128() { Some(value) => value as i64, None => i64::MAX }
}

pub fn increment_group(groups: &mut Vec<GroupCell>, segment: Arc<Vec<u8>>, id: GroupId,
    increment: &Wide, positive: &Wide, last_ship: Option<i32>)
    requires increment@ > 0, positive@ >= 0,
        forall|index: int| #![trigger old(groups)@[index]] 0 <= index < old(groups)@.len() ==> old(groups)@[index].weight@ > 0 && old(groups)@[index].bound >= 0,
    ensures
        forall|index: int| #![trigger final(groups)@[index]] 0 <= index < final(groups)@.len() ==> final(groups)@[index].weight@ > 0 && final(groups)@[index].bound >= 0,
        groups_sorted(old(groups)@) ==> groups_sorted(final(groups)@),
        forall|index: int| #![trigger final(groups)@[index]]
            0 <= index < final(groups)@.len() ==> {
                let row = final(groups)@[index];
                old(groups)@.contains(row) || (group_matches(row, (*segment)@, id)
                    && row.bound as int == capped(row.weight@ * positive@) && row.last_ship == last_ship)
            },
        forall|target: Seq<u8>, key: int, date: int, priority: int| #![trigger column_weight(final(groups)@, target, key, date, priority)]
            column_weight(final(groups)@, target, key, date, priority) == column_weight(old(groups)@, target, key, date, priority)
                + if target == (*segment)@ && key == id.key && date == id.date && priority == id.priority { increment@ } else { 0int },
{
    let position = group_start(groups, &segment, id);
    let found = position < groups.len() && groups[position].id.key == id.key
        && groups[position].id.date == id.date && groups[position].id.priority == id.priority
        && bytes_eq_exec(&segment, &groups[position].segment);
    let weight = if found { groups[position].weight.add(increment) } else { increment.dup() };
    let bound = capped_product(&weight, positive);
    let ghost previous = groups@;
    let group = GroupCell { segment, id, weight, bound, last_ship };
    proof {
        if groups_sorted(previous) {
            let less = |left: GroupCell, right: GroupCell| group_lt(left, right);
            assert forall|offset: int| #![trigger previous[offset]] 0 <= offset < position implies group_lt(previous[offset], group) by {
                assert(group_before_id(previous[offset], (*group.segment)@, id));
            }
            assert forall|offset: int| #![trigger previous[offset]] position + if found { 1int } else { 0int } <= offset < previous.len() implies group_lt(group, previous[offset]) by {
                if found { assert(group_lt(previous[position as int], previous[offset])); }
                else {
                    assert(!group_before_id(previous[offset], (*group.segment)@, id));
                    if group_matches(previous[offset], (*group.segment)@, id) {
                        if offset == position {} else {
                            assert(group_lt(previous[position as int], previous[offset]));
                            assert(group_before_id(previous[position as int], (*group.segment)@, id));
                        }
                    }
                    if (*group.segment)@ != (*previous[offset].segment)@ {
                        lemma_bytes_lt_total((*group.segment)@, (*previous[offset].segment)@);
                    }
                }
            }
            if found { verus_prooflib::lemma_sorted_update_at(previous, position as int, group, less); }
            else { verus_prooflib::lemma_sorted_insert_at(previous, position as int, group, less); }
        }
    }
    if found { groups.set(position, group); } else { groups.insert(position, group); }
    proof {
        assert forall|offset: int| #![trigger groups@[offset]] 0 <= offset < groups@.len() implies groups@[offset].weight@ > 0 && groups@[offset].bound >= 0 by {
            if offset != position {
                let prior = if found || offset < position { offset } else { offset - 1 };
                assert(groups@[offset] == previous[prior]);
            }
        }
        assert forall|offset: int| #![trigger groups@[offset]] 0 <= offset < groups@.len() implies
            previous.contains(groups@[offset]) || (group_matches(groups@[offset], (*group.segment)@, id)
                && groups@[offset].bound as int == capped(groups@[offset].weight@ * positive@) && groups@[offset].last_ship == last_ship) by {
            if offset != position {
                let prior = if found || offset < position { offset } else { offset - 1 };
                assert(groups@[offset] == previous[prior]);
            }
        }
        assert forall|target: Seq<u8>, key: int, date: int, priority: int| #![trigger column_weight(groups@, target, key, date, priority)]
            column_weight(groups@, target, key, date, priority) == column_weight(previous, target, key, date, priority)
                + if target == (*group.segment)@ && key == id.key && date == id.date && priority == id.priority { increment@ } else { 0int } by {
            let measure = |row: GroupCell| if (*row.segment)@ == target && row.id.key == key && row.id.date == date && row.id.priority == priority { row.weight@ } else { 0int };
            if found { lemma_sum_update(previous, position as int, group, measure); }
            else { verus_prooflib::lemma_sum_insert(previous, position as int, group, measure); }
        }
    }
}

pub fn compute_key(groups: &mut Vec<GroupCell>, orders: &Vec<OrderCell>, ships: &Vec<ShipCell>,
    customers: &Custs, key: i64)
    requires customers.wf(),
        orders_sorted(orders@), ships_sorted(ships@),
        forall|index: int| #![trigger ships@[index]] 0 <= index < ships@.len() ==> ship_ok(ships@[index]),
        forall|index: int| #![trigger orders@[index]] 0 <= index < orders@.len() ==> orders@[index].count > 0,
        forall|target: verus_prooflib::ordered::KeyView| #![trigger customers.at(target)] customers.at(target).is_some() ==> customer_shape(customers.at(target).unwrap()),
        forall|index: int| #![trigger old(groups)@[index]] 0 <= index < old(groups)@.len() ==> old(groups)@[index].weight@ > 0 && old(groups)@[index].bound >= 0,
    ensures forall|index: int| #![trigger final(groups)@[index]] 0 <= index < final(groups)@.len() ==> final(groups)@[index].weight@ > 0 && final(groups)@[index].bound >= 0,
        groups_sorted(old(groups)@) ==> groups_sorted(final(groups)@),
        forall|position: int| #![trigger final(groups)@[position]]
            0 <= position < final(groups)@.len() ==>
                old(groups)@.contains(final(groups)@[position])
                || (final(groups)@[position].id.key == key && group_ship_ok(final(groups)@[position], ships@)),
        forall|target: Seq<u8>, target_key: int, date: int, priority: int| #![trigger column_weight(final(groups)@, target, target_key, date, priority)]
            column_weight(final(groups)@, target, target_key, date, priority) == column_weight(old(groups)@, target, target_key, date, priority)
                + if target_key == key { column_join(orders@, *customers, target, target_key, date, priority) } else { 0int },
{
    let ghost original_groups = groups@;
    let mut positive: Wide = Wide::zero();
    let mut last_ship: Option<i32> = None;
    let mut index = ships_start(ships, key);
    proof { lemma_sum_zero(ships@.take(index as int), ship_positive_weight(key as int)); }
    while index < ships.len() && ships[index].key == key
        invariant index <= ships.len(), positive@ >= 0,
            ships_sorted(ships@),
            positive@ == ship_positive(ships@.take(index as int), key as int),
            ship_last_ok(ships@.take(index as int), key as int, last_ship),
            forall|position: int| #![trigger ships@[position]] index <= position < ships@.len() ==> ships@[position].key >= key,
            forall|position: int| #![trigger ships@[position]] 0 <= position < ships@.len() ==> ship_ok(ships@[position]),
            forall|position: int| #![trigger groups@[position]] 0 <= position < groups@.len() ==> groups@[position].weight@ > 0 && groups@[position].bound >= 0,
        decreases ships.len() - index,
    {
        proof {
            lemma_sum_take(ships@, index as int, ship_positive_weight(key as int));
            assert forall|position: int| #![trigger ships@[position]]
                0 <= position < index && ships@[position].key == key implies ships@[position].date <= ships@[index as int].date by {
                assert(ship_lt(ships@[position], ships@[index as int]));
            }
            assert(ships@.take(index as int + 1)[index as int] == ships@[index as int]);
        }
        proof { assert(ship_ok(ships@[index as int])); }
        positive = positive.add(&ships[index].positive);
        last_ship = Some(ships[index].date);
        index += 1;
    }
    proof {
        assert forall|position: int| #![trigger ships@[position]] index <= position < ships@.len() implies ships@[position].key != key by {
            if index < ships.len() && position > index {
                assert(ship_lt(ships@[index as int], ships@[position]));
            }
        }
        verus_prooflib::lemma_sum_prefix_skip_zero(ships@, index as int, ships@.len() as int, ship_positive_weight(key as int));
        assert(ships@.take(ships@.len() as int) =~= ships@);
        assert(ship_last_ok(ships@, key as int, last_ship));
    }
    index = orders_start(orders, key);
    proof {
        assert forall|target: Seq<u8>, date: int, priority: int| #![trigger column_join(orders@.take(index as int), *customers, target, key as int, date, priority)]
            column_join(orders@.take(index as int), *customers, target, key as int, date, priority) == 0 by {
            lemma_sum_zero(orders@.take(index as int), order_group_measure(*customers, target, key as int, date, priority));
        }
    }
    while index < orders.len() && orders[index].key == key
        invariant index <= orders.len(), positive@ >= 0, customers.wf(),
            positive@ == ship_positive(ships@, key as int), ship_last_ok(ships@, key as int, last_ship),
            forall|position: int| #![trigger groups@[position]] 0 <= position < groups@.len() ==>
                original_groups.contains(groups@[position]) || (groups@[position].id.key == key && group_ship_ok(groups@[position], ships@)),
            orders_sorted(orders@),
            forall|position: int| #![trigger orders@[position]] index <= position < orders@.len() ==> orders@[position].key >= key,
            groups_sorted(original_groups) ==> groups_sorted(groups@),
            forall|target: Seq<u8>, target_key: int, date: int, priority: int| #![trigger column_weight(groups@, target, target_key, date, priority)]
                column_weight(groups@, target, target_key, date, priority) == column_weight(original_groups, target, target_key, date, priority)
                    + if target_key == key { column_join(orders@.take(index as int), *customers, target, target_key, date, priority) } else { 0int },
            forall|position: int| #![trigger orders@[position]] 0 <= position < orders@.len() ==> orders@[position].count > 0,
            forall|target: verus_prooflib::ordered::KeyView| #![trigger customers.at(target)] customers.at(target).is_some() ==> customer_shape(customers.at(target).unwrap()),
            forall|position: int| #![trigger groups@[position]] 0 <= position < groups@.len() ==> groups@[position].weight@ > 0 && groups@[position].bound >= 0,
        decreases orders.len() - index,
    {
        let order = orders[index];
        let ghost before_order = groups@;
        proof {
            assert(forall|target: Seq<u8>, target_key: int, date: int, priority: int| #![trigger column_weight(before_order, target, target_key, date, priority)]
                column_weight(before_order, target, target_key, date, priority) == column_weight(original_groups, target, target_key, date, priority)
                    + if target_key == key { column_join(orders@.take(index as int), *customers, target, target_key, date, priority) } else { 0int });
        }
        if let Some(customer) = customers.get(order.customer) {
            let mut segment_index = 0;
            while segment_index < customer.segments.len()
                invariant segment_index <= customer.segments.len(), customer_shape(*customer), customers.wf(), order.count > 0, order.key == key, positive@ >= 0,
                    positive@ == ship_positive(ships@, key as int), ship_last_ok(ships@, key as int, last_ship),
                    forall|position: int| #![trigger groups@[position]] 0 <= position < groups@.len() ==>
                        original_groups.contains(groups@[position]) || (groups@[position].id.key == key && group_ship_ok(groups@[position], ships@)),
                    groups_sorted(original_groups) ==> groups_sorted(groups@),
                    forall|target: Seq<u8>, target_key: int, date: int, priority: int| #![trigger column_weight(groups@, target, target_key, date, priority)]
                        column_weight(groups@, target, target_key, date, priority) == column_weight(before_order, target, target_key, date, priority)
                            + if target_key == order.key && date == order.date && priority == order.priority {
                                order.count as int * segment_count(customer.segments@.take(segment_index as int), target)
                            } else { 0int },
                    forall|position: int| #![trigger groups@[position]] 0 <= position < groups@.len() ==> groups@[position].weight@ > 0 && groups@[position].bound >= 0,
                decreases customer.segments.len() - segment_index,
            {
                let ghost previous_groups = groups@;
                let ghost processed = segment_index as int;
                let segment = customer.segments[segment_index].0.clone();
                let ghost selected_segment = (*segment)@;
                let id = GroupId { key, date: order.date, priority: order.priority };
                let increment = Wide::from_i128(order.count as i128).mul(&Wide::from_i128(customer.segments[segment_index].1));
                proof {
                    assert(increment@ > 0) by (nonlinear_arith)
                        requires order.count > 0, customer.segments@[segment_index as int].1 > 0,
                            increment@ == order.count as int * customer.segments@[segment_index as int].1 as int;
                }
                increment_group(groups, segment, id, &increment, &positive, last_ship);
                proof {
                    assert forall|position: int| #![trigger groups@[position]] 0 <= position < groups@.len() implies
                        original_groups.contains(groups@[position]) || (groups@[position].id.key == key && group_ship_ok(groups@[position], ships@)) by {
                        if previous_groups.contains(groups@[position]) {
                            let prior = choose|prior: int| #![trigger previous_groups[prior]]
                                0 <= prior < previous_groups.len() && previous_groups[prior] == groups@[position];
                        }
                    }
                    assert forall|target: Seq<u8>, target_key: int, date: int, priority: int| #![trigger column_weight(groups@, target, target_key, date, priority)]
                        column_weight(groups@, target, target_key, date, priority) == column_weight(before_order, target, target_key, date, priority)
                            + if target_key == order.key && date == order.date && priority == order.priority {
                                order.count as int * segment_count(customer.segments@.take(processed + 1), target)
                            } else { 0int } by {
                        let measure = |entry: (Arc<Vec<u8>>, i128)| if (*entry.0)@ == target { entry.1 as int } else { 0int };
                        lemma_sum_take(customer.segments@, processed, measure);
                        assert(segment_count(customer.segments@.take(processed + 1), target) == segment_count(customer.segments@.take(processed), target)
                            + if selected_segment == target { customer.segments@[processed].1 as int } else { 0int });
                        assert(column_weight(groups@, target, target_key, date, priority) == column_weight(previous_groups, target, target_key, date, priority)
                            + if target == selected_segment && target_key == order.key && date == order.date && priority == order.priority { increment@ } else { 0int });
                        assert(column_weight(previous_groups, target, target_key, date, priority) == column_weight(before_order, target, target_key, date, priority)
                            + if target_key == order.key && date == order.date && priority == order.priority {
                                order.count as int * segment_count(customer.segments@.take(processed), target)
                            } else { 0int });
                        if target_key == order.key && date == order.date && priority == order.priority && target == (*customer.segments@[processed].0)@ {
                            assert(order.count as int * segment_count(customer.segments@.take(processed + 1), target)
                                == order.count as int * segment_count(customer.segments@.take(processed), target) + increment@) by (nonlinear_arith)
                                requires segment_count(customer.segments@.take(processed + 1), target) == segment_count(customer.segments@.take(processed), target) + customer.segments@[processed].1 as int,
                                    increment@ == order.count as int * customer.segments@[processed].1 as int;
                        }
                    }
                }
                segment_index += 1;
            }
            proof { assert(customer.segments@.take(segment_index as int) =~= customer.segments@); }
        }
        proof {
            assert forall|target: Seq<u8>, target_key: int, date: int, priority: int| #![trigger column_weight(groups@, target, target_key, date, priority)]
                column_weight(groups@, target, target_key, date, priority) == column_weight(original_groups, target, target_key, date, priority)
                    + if target_key == key { column_join(orders@.take(index as int + 1), *customers, target, target_key, date, priority) } else { 0int } by {
                lemma_sum_take(orders@, index as int, order_group_measure(*customers, target, target_key, date, priority));
            }
        }
        index += 1;
    }
    proof {
        assert forall|target: Seq<u8>, target_key: int, date: int, priority: int| #![trigger column_weight(groups@, target, target_key, date, priority)]
            column_weight(groups@, target, target_key, date, priority) == column_weight(original_groups, target, target_key, date, priority)
                + if target_key == key { column_join(orders@, *customers, target, target_key, date, priority) } else { 0int } by {
            if target_key == key {
                assert forall|position: int| #![trigger orders@[position]] index <= position < orders@.len() implies orders@[position].key != key by {
                    if index < orders@.len() && position > index { assert(order_lt(orders@[index as int], orders@[position])); }
                }
                verus_prooflib::lemma_sum_prefix_skip_zero(orders@, index as int, orders@.len() as int, order_group_measure(*customers, target, target_key, date, priority));
                assert(orders@.take(orders@.len() as int) =~= orders@);
            }
        }
    }
}

}

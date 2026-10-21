use vstd::prelude::*;
use crate::custs::Custs;
use crate::paged::{Paged, PageCursor, entry_sorted, Ent};
use crate::rank_ent::*;
use std::sync::Arc;
use verus_common::bytes::*;
use verus_common::sums::*;
use verus_prooflib::ordered::{Index, KeyView};
use crate::gen_spec::*;
use crate::gen_obligations::*;
use crate::values::*;
use crate::packed::{PackedOrders, PackedShips, PackedGroups};
use verus_prooflib::wide::Wide;
pub use verus_prooflib::dyadic_width as pow2;

verus! {

pub struct CustomerCell {
    pub segments: Vec<(Arc<Vec<u8>>, i128)>,
    pub orders: Vec<(i64, i128)>,
}

#[derive(Clone, Copy)]
pub struct OrderCell {
    pub key: i64,
    pub customer: i64,
    pub date: i32,
    pub priority: i64,
    pub count: i128,
}

pub struct ShipCell {
    pub key: i64,
    pub date: i32,
    pub count: i128,
    pub revenue: Wide,
    pub positive: Wide,
}

pub struct GroupCell {
    pub segment: Arc<Vec<u8>>,
    pub id: GroupId,
    pub weight: Wide,
    pub bound: i64,
    pub last_ship: Option<i32>,
}

pub struct Page {
    pub orders: Arc<PackedOrders>,
    pub ships: Arc<PackedShips>,
    pub groups: Arc<PackedGroups>,
}

pub struct PageWork {
    pub orders: Vec<OrderCell>,
    pub ships: Vec<ShipCell>,
}

pub open spec fn segment_weight(segment: Seq<u8>) -> spec_fn((Arc<Vec<u8>>, i128)) -> int {
    |entry: (Arc<Vec<u8>>, i128)| if (*entry.0)@ == segment { entry.1 as int } else { 0 }
}

pub open spec fn segment_count(rows: Seq<(Arc<Vec<u8>>, i128)>, segment: Seq<u8>) -> int {
    sum(rows, segment_weight(segment))
}

pub open spec fn reverse_weight(key: int) -> spec_fn((i64, i128)) -> int {
    |entry: (i64, i128)| if entry.0 == key { entry.1 as int } else { 0 }
}

pub open spec fn reverse_count(rows: Seq<(i64, i128)>, key: int) -> int {
    sum(rows, reverse_weight(key))
}

pub open spec fn order_reverse_weight(customer: i64, key: i64) -> spec_fn(OrderCell) -> int {
    |row: OrderCell| if row.key == key && row.customer == customer { row.count as int } else { 0int }
}

pub open spec fn order_reverse_count(rows: Seq<OrderCell>, customer: i64, key: i64) -> int {
    sum(rows, order_reverse_weight(customer, key))
}

pub open spec fn segments_sorted(rows: Seq<(Arc<Vec<u8>>, i128)>) -> bool {
    forall|left: int, right: int| #![trigger rows[left], rows[right]]
        0 <= left < right < rows.len() ==> bytes_lt((*rows[left].0)@, (*rows[right].0)@)
}

pub open spec fn reverse_sorted(rows: Seq<(i64, i128)>) -> bool {
    forall|left: int, right: int| #![trigger rows[left], rows[right]]
        0 <= left < right < rows.len() ==> rows[left].0 < rows[right].0
}

pub open spec fn orders_sorted(rows: Seq<OrderCell>) -> bool {
    forall|left: int, right: int| #![trigger rows[left], rows[right]]
        0 <= left < right < rows.len() ==> order_lt(rows[left], rows[right])
}

pub open spec fn ships_sorted(rows: Seq<ShipCell>) -> bool {
    forall|left: int, right: int| #![trigger rows[left], rows[right]]
        0 <= left < right < rows.len() ==> ship_lt(rows[left], rows[right])
}

pub open spec fn groups_sorted(rows: Seq<GroupCell>) -> bool {
    forall|left: int, right: int| #![trigger rows[left], rows[right]]
        0 <= left < right < rows.len() ==> group_lt(rows[left], rows[right])
}

pub open spec fn orders_owner(rows: Seq<OrderCell>, page: int) -> bool {
    forall|index: int| #![trigger rows[index]] 0 <= index < rows.len() ==> rows[index].key as int / 256 == page
}

pub open spec fn ships_owner(rows: Seq<ShipCell>, page: int) -> bool {
    forall|index: int| #![trigger rows[index]] 0 <= index < rows.len() ==> rows[index].key as int / 256 == page
}

pub open spec fn work_page_shape(page: PageWork, number: int) -> bool {
    &&& orders_sorted(page.orders@) && ships_sorted(page.ships@)
    &&& orders_owner(page.orders@, number) && ships_owner(page.ships@, number)
    &&& forall|index: int| #![trigger page.orders@[index]] 0 <= index < page.orders@.len() ==> page.orders@[index].count > 0
    &&& forall|index: int| #![trigger page.ships@[index]] 0 <= index < page.ships@.len() ==> ship_ok(page.ships@[index])
}

pub open spec fn work_database_ok(work: PageWork, number: int, db: Db) -> bool {
    &&& work_page_shape(work, number)
    &&& forall|key: int, customer: int, date: int, priority: int| #![trigger order_count(work.orders@, key, customer, date, priority)]
        order_count(work.orders@, key, customer, date, priority) == if key / 256 == number { order_sum(db, key, customer, date, priority) } else { 0int }
    &&& forall|key: int, date: int, kind: int| #![trigger ship_count(work.ships@, key, date, kind)]
        0 <= kind <= 2 ==> ship_count(work.ships@, key, date, kind) == if key / 256 == number { ship_sum(db, key, date, kind) } else { 0int }
}

pub open spec fn page_database_ok(page: Page, number: int, db: Db) -> bool {
    &&& page_shape(page, number, db)
    &&& forall|key: int, customer: int, date: int, priority: int| #![trigger order_count((*page.orders)@, key, customer, date, priority)]
        order_count((*page.orders)@, key, customer, date, priority) == if key / 256 == number { order_sum(db, key, customer, date, priority) } else { 0int }
    &&& forall|key: int, date: int, kind: int| #![trigger ship_count((*page.ships)@, key, date, kind)]
        0 <= kind <= 2 ==> ship_count((*page.ships)@, key, date, kind) == if key / 256 == number { ship_sum(db, key, date, kind) } else { 0int }
    &&& forall|segment: Seq<u8>, key: int, date: int, priority: int| #![trigger column_weight((*page.groups)@, segment, key, date, priority)]
        column_weight((*page.groups)@, segment, key, date, priority) == if key / 256 == number { weight_sum(db, segment, key, date, priority) } else { 0int }
}

/// No customer count is negative (with `signed_customer_shape`: every count is positive).
pub open spec fn customer_counts_nonneg(cell: CustomerCell) -> bool {
    &&& forall|index: int| #![trigger cell.segments@[index]] 0 <= index < cell.segments@.len() ==> cell.segments@[index].1 >= 0
    &&& forall|index: int| #![trigger cell.orders@[index]] 0 <= index < cell.orders@.len() ==> cell.orders@[index].1 >= 0
}

pub open spec fn signed_customer_shape(cell: CustomerCell) -> bool {
    &&& segments_sorted(cell.segments@) && reverse_sorted(cell.orders@)
    &&& forall|index: int| #![trigger cell.segments@[index]] 0 <= index < cell.segments@.len() ==> cell.segments@[index].1 != 0
    &&& forall|index: int| #![trigger cell.orders@[index]] 0 <= index < cell.orders@.len() ==> cell.orders@[index].1 != 0
}

pub open spec fn order_count(rows: Seq<OrderCell>, key: int, customer: int, date: int, priority: int) -> int {
    sum(rows, |row: OrderCell| if row.key == key && row.customer == customer
        && row.date == date && row.priority == priority { row.count as int } else { 0 })
}

pub open spec fn ship_count(rows: Seq<ShipCell>, key: int, date: int, kind: int) -> int {
    sum(rows, |row: ShipCell| if row.key == key && row.date == date {
        if kind == 0 { row.count as int } else if kind == 1 { row.revenue@ } else { row.positive@ }
    } else { 0 })
}

/// A shipment cell: counted rows and nonnegative positive revenue.
pub open spec fn ship_ok(cell: ShipCell) -> bool {
    cell.count > 0 && cell.positive@ >= 0
}

impl ShipCell {
    pub fn dup(&self) -> (result: ShipCell)
        ensures result == *self,
    {
        ShipCell { key: self.key, date: self.date, count: self.count, revenue: self.revenue.dup(), positive: self.positive.dup() }
    }
}

/// A group's weight times its shipments' positive revenue, capped to the largest rank bound.
pub open spec fn capped(value: int) -> int {
    if value > i64::MAX as int { i64::MAX as int } else { value }
}

pub open spec fn ship_positive_weight(key: int) -> spec_fn(ShipCell) -> int {
    |row: ShipCell| if row.key == key { row.positive@ } else { 0int }
}

pub open spec fn ship_positive(rows: Seq<ShipCell>, key: int) -> int {
    sum(rows, ship_positive_weight(key))
}

pub open spec fn ship_last_ok(rows: Seq<ShipCell>, key: int, last: Option<i32>) -> bool {
    match last {
        None => forall|position: int| #![trigger rows[position]]
            0 <= position < rows.len() ==> rows[position].key != key,
        Some(date) => {
            &&& exists|position: int| #![trigger rows[position]]
                0 <= position < rows.len() && rows[position].key == key && rows[position].date == date
            &&& forall|position: int| #![trigger rows[position]]
                0 <= position < rows.len() && rows[position].key == key ==> rows[position].date <= date
        },
    }
}

pub open spec fn group_ship_ok(group: GroupCell, ships: Seq<ShipCell>) -> bool {
    group.bound as int == capped(group.weight@ * ship_positive(ships, group.id.key as int))
        && ship_last_ok(ships, group.id.key as int, group.last_ship)
}

pub open spec fn next_order_key(orders: Seq<OrderCell>, position: int) -> int {
    if position < orders.len() { orders[position].key as int } else { i64::MAX as int + 1 }
}

pub open spec fn column_weight(rows: Seq<GroupCell>, segment: Seq<u8>, key: int, date: int, priority: int) -> int {
    sum(rows, column_measure(segment, key, date, priority))
}

pub open spec fn column_measure(segment: Seq<u8>, key: int, date: int, priority: int) -> spec_fn(GroupCell) -> int {
    |row: GroupCell| if (*row.segment)@ == segment && row.id.key == key
        && row.id.date == date && row.id.priority == priority { row.weight@ } else { 0 }
}

pub open spec fn customer_sum(db: Db, customer: int, segment: Seq<u8>) -> int {
    sum(db.customer, |row: CustomerRow| if row.c_custkey == customer && row.c_mktsegment == segment { 1 } else { 0 })
}

pub open spec fn reverse_sum(db: Db, customer: int, key: int) -> int {
    sum(db.orders, |row: OrdersRow| if row.o_custkey == customer && row.o_orderkey == key { 1 } else { 0 })
}

pub open spec fn order_sum(db: Db, key: int, customer: int, date: int, priority: int) -> int {
    sum(db.orders, |row: OrdersRow| if row.o_orderkey == key && row.o_custkey == customer
        && row.o_orderdate == date && row.o_shippriority == priority { 1 } else { 0 })
}

pub open spec fn ship_sum(db: Db, key: int, date: int, kind: int) -> int {
    sum(db.lineitem, |row: LineitemRow| if row.l_orderkey == key && row.l_shipdate == date {
        let value = row.l_extendedprice * (100 - row.l_discount);
        if kind == 0 { 1 } else if kind == 1 { value } else { if value > 0 { value } else { 0 } }
    } else { 0 })
}

pub open spec fn weight_sum(db: Db, segment: Seq<u8>, key: int, date: int, priority: int) -> int {
    sum(db.orders, |row: OrdersRow| if row.o_orderkey == key && row.o_orderdate == date
        && row.o_shippriority == priority { customer_sum(db, row.o_custkey, segment) } else { 0 })
}

pub open spec fn positive_sum(db: Db, key: int) -> int {
    sum(db.lineitem, |row: LineitemRow| if row.l_orderkey == key {
        let value = row.l_extendedprice * (100 - row.l_discount);
        if value > 0 { value } else { 0 }
    } else { 0 })
}

pub open spec fn customer_at(customers: Custs, customer: int, segment: Seq<u8>) -> int {
    match customers.at((customer, Seq::empty())) {
        Some(cell) => segment_count(cell.segments@, segment),
        None => 0,
    }
}

pub open spec fn order_group_measure(customers: Custs, segment: Seq<u8>, key: int, date: int, priority: int) -> spec_fn(OrderCell) -> int {
    |row: OrderCell| if row.key == key && row.date == date && row.priority == priority {
        row.count as int * customer_at(customers, row.customer as int, segment)
    } else { 0int }
}

pub open spec fn column_join(rows: Seq<OrderCell>, customers: Custs, segment: Seq<u8>, key: int, date: int, priority: int) -> int {
    sum(rows, order_group_measure(customers, segment, key, date, priority))
}

pub open spec fn scratch_customer_at(work: Map<i64, CustomerCell>, customer: i64, segment: Seq<u8>) -> int {
    if work.dom().contains(customer) {
        segment_count(work[customer].segments@, segment)
    } else { 0 }
}

pub open spec fn scratch_reverse_at(work: Map<i64, CustomerCell>, customer: i64, key: i64) -> int {
    if work.dom().contains(customer) {
        reverse_count(work[customer].orders@, key as int)
    } else { 0 }
}

pub open spec fn scratch_order_at(work: Map<i64, PageWork>, key: i64, customer: i64, date: i32, priority: i64) -> int {
    let number = ((key as int) / 256) as i64;
    if work.dom().contains(number) {
        order_count(work[number].orders@, key as int, customer as int, date as int, priority as int)
    } else { 0 }
}

pub open spec fn scratch_ship_at(work: Map<i64, PageWork>, key: i64, date: i32, kind: int) -> int {
    let number = ((key as int) / 256) as i64;
    if work.dom().contains(number) {
        ship_count(work[number].ships@, key as int, date as int, kind)
    } else { 0 }
}

pub open spec fn customer_prefix(rows: Seq<CustomerInput>, customer: i64, segment: Seq<u8>) -> int {
    sum(rows, customer_input_weight(customer, segment))
}

pub open spec fn customer_input_weight(customer: i64, segment: Seq<u8>) -> spec_fn(CustomerInput) -> int {
    |row: CustomerInput| if row.c_custkey == customer && row.c_mktsegment@ == segment { 1int } else { 0int }
}

pub open spec fn reverse_input_weight(customer: i64, key: i64) -> spec_fn(OrderInput) -> int {
    |row: OrderInput| if row.o_custkey == customer && row.o_orderkey == key { 1 } else { 0 }
}

pub open spec fn reverse_prefix(rows: Seq<OrderInput>, customer: i64, key: i64) -> int {
    sum(rows, reverse_input_weight(customer, key))
}

pub open spec fn order_input_weight(key: i64, customer: i64, date: i32, priority: i64) -> spec_fn(OrderInput) -> int {
    |row: OrderInput| if row.o_orderkey == key && row.o_custkey == customer
        && row.o_orderdate == date && row.o_shippriority == priority { 1 } else { 0 }
}

pub open spec fn order_prefix(rows: Seq<OrderInput>, key: i64, customer: i64, date: i32, priority: i64) -> int {
    sum(rows, order_input_weight(key, customer, date, priority))
}

pub open spec fn ship_input_weight(key: i64, date: i32, kind: int) -> spec_fn(ShipmentInput) -> int {
    |row: ShipmentInput| if row.l_orderkey == key && row.l_shipdate == date {
        let value = row.l_extendedprice as int * (100 - row.l_discount as int);
        if kind == 0 { 1 } else if kind == 1 { value } else { if value > 0 { value } else { 0 } }
    } else { 0 }
}

pub open spec fn ship_prefix(rows: Seq<ShipmentInput>, key: i64, date: i32, kind: int) -> int {
    sum(rows, ship_input_weight(key, date, kind))
}

pub open spec fn customer_overlay(work: Map<i64, CustomerCell>, before: Custs, customer: i64, segment: Seq<u8>) -> int {
    if work.dom().contains(customer) { scratch_customer_at(work, customer, segment) }
    else { customer_at(before, customer as int, segment) }
}

pub open spec fn reverse_overlay(work: Map<i64, CustomerCell>, before: Custs, customer: i64, key: i64) -> int {
    if work.dom().contains(customer) { scratch_reverse_at(work, customer, key) }
    else { reverse_at(before, customer as int, key as int) }
}

pub open spec fn customer_delta_prefix(rows: Seq<CustomerDelta>, customer: i64, segment: Seq<u8>) -> int {
    sum(rows, customer_delta_weight(customer, segment))
}

pub open spec fn customer_delta_weight(customer: i64, segment: Seq<u8>) -> spec_fn(CustomerDelta) -> int {
    |delta: CustomerDelta| if delta.row.c_custkey == customer && delta.row.c_mktsegment@ == segment { delta.sign as int } else { 0int }
}

pub open spec fn reverse_at(customers: Custs, customer: int, key: int) -> int {
    match customers.at((customer, Seq::empty())) {
        Some(cell) => reverse_count(cell.orders@, key),
        None => 0,
    }
}

pub open spec fn order_at(pages: Index<Page>, key: int, customer: int, date: int, priority: int) -> int {
    match pages.at((key / 256, Seq::empty())) {
        Some(page) => order_count((*page.orders)@, key, customer, date, priority),
        None => 0,
    }
}

pub open spec fn ship_at(pages: Index<Page>, key: int, date: int, kind: int) -> int {
    match pages.at((key / 256, Seq::empty())) {
        Some(page) => ship_count((*page.ships)@, key, date, kind),
        None => 0,
    }
}

pub open spec fn group_at(pages: Index<Page>, segment: Seq<u8>, key: int, date: int, priority: int) -> int {
    match pages.at((key / 256, Seq::empty())) {
        Some(page) => column_weight((*page.groups)@, segment, key, date, priority),
        None => 0,
    }
}

pub open spec fn group_lt(left: GroupCell, right: GroupCell) -> bool {
    bytes_lt((*left.segment)@, (*right.segment)@) || ((*left.segment)@ == (*right.segment)@
        && (left.id.key < right.id.key || (left.id.key == right.id.key
        && (left.id.date < right.id.date || (left.id.date == right.id.date && left.id.priority < right.id.priority)))))
}

pub open spec fn group_before_id(row: GroupCell, segment: Seq<u8>, id: GroupId) -> bool {
    bytes_lt((*row.segment)@, segment) || ((*row.segment)@ == segment
        && (row.id.key < id.key || (row.id.key == id.key
        && (row.id.date < id.date || (row.id.date == id.date && row.id.priority < id.priority)))))
}

pub open spec fn group_matches(row: GroupCell, segment: Seq<u8>, id: GroupId) -> bool {
    (*row.segment)@ == segment && row.id.key == id.key && row.id.date == id.date && row.id.priority == id.priority
}

pub open spec fn order_lt(left: OrderCell, right: OrderCell) -> bool {
    left.key < right.key || (left.key == right.key && (left.customer < right.customer
        || (left.customer == right.customer && (left.date < right.date
        || (left.date == right.date && left.priority < right.priority)))))
}

pub open spec fn ship_lt(left: ShipCell, right: ShipCell) -> bool {
    left.key < right.key || (left.key == right.key && left.date < right.date)
}

pub open spec fn covers(group: GroupCell, node: int) -> bool {
    match group.last_ship {
        None => false,
        Some(last) => group.id.date as int + 1 <= last as int - 1 && {
            let first = (group.id.date as int + 1 - i32::MIN as int) / 128;
            let final_leaf = (last as int - 1 - i32::MIN as int) / 128;
            let level = node / 67108864;
            let offset = node % 67108864;
            let width = pow2(level);
            0 <= level <= 26 && first <= offset * width && (offset + 1) * width - 1 <= final_leaf
                && (level == 26 || !(first <= (offset / 2) * (2 * width)
                    && (offset / 2 + 1) * (2 * width) - 1 <= final_leaf))
        },
    }
}

pub open spec fn customer_shape(cell: CustomerCell) -> bool {
    &&& forall|index: int| #![trigger cell.segments@[index]] 0 <= index < cell.segments@.len() ==> cell.segments@[index].1 > 0
    &&& forall|left: int, right: int| #![trigger cell.segments@[left], cell.segments@[right]]
        0 <= left < right < cell.segments@.len() ==> bytes_lt((*cell.segments@[left].0)@, (*cell.segments@[right].0)@)
    &&& forall|index: int| #![trigger cell.orders@[index]] 0 <= index < cell.orders@.len() ==> cell.orders@[index].1 > 0
    &&& forall|left: int, right: int| #![trigger cell.orders@[left], cell.orders@[right]]
        0 <= left < right < cell.orders@.len() ==> cell.orders@[left].0 < cell.orders@[right].0
}

pub open spec fn customer_database_ok(customers: Custs, db: Db) -> bool {
    &&& customers.wf()
    &&& forall|customer: int, segment: Seq<u8>| #![trigger customer_at(customers, customer, segment)]
        customer_at(customers, customer, segment) == customer_sum(db, customer, segment)
    &&& forall|customer: int, key: int| #![trigger reverse_at(customers, customer, key)]
        reverse_at(customers, customer, key) == reverse_sum(db, customer, key)
    &&& forall|target: KeyView| #![trigger customers.at(target)] customers.at(target).is_some() ==>
        target.1.len() == 0 && customer_shape(customers.at(target).unwrap())
}

pub open spec fn page_shape(page: Page, number: int, db: Db) -> bool {
    &&& forall|index: int| #![trigger (*page.orders)@[index]] 0 <= index < (*page.orders)@.len() ==> {
        let row = (*page.orders)@[index]; row.key as int / 256 == number && row.count > 0
    }
    &&& forall|left: int, right: int| #![trigger (*page.orders)@[left], (*page.orders)@[right]]
        0 <= left < right < (*page.orders)@.len() ==> order_lt((*page.orders)@[left], (*page.orders)@[right])
    &&& forall|index: int| #![trigger (*page.ships)@[index]] 0 <= index < (*page.ships)@.len() ==> {
        let row = (*page.ships)@[index]; row.key as int / 256 == number && ship_ok(row)
    }
    &&& forall|left: int, right: int| #![trigger (*page.ships)@[left], (*page.ships)@[right]]
        0 <= left < right < (*page.ships)@.len() ==> ship_lt((*page.ships)@[left], (*page.ships)@[right])
    &&& forall|left: int, right: int| #![trigger (*page.groups)@[left], (*page.groups)@[right]]
        0 <= left < right < (*page.groups)@.len() ==> group_lt((*page.groups)@[left], (*page.groups)@[right])
    &&& forall|index: int| #![trigger (*page.groups)@[index]] 0 <= index < (*page.groups)@.len() ==> {
        let group = (*page.groups)@[index];
        &&& group.id.key as int / 256 == number
        &&& group.weight@ > 0
        &&& group.bound as int == capped(group.weight@ * positive_sum(db, group.id.key as int))
        &&& match group.last_ship {
            None => forall|position: int| #![trigger db.lineitem[position]] 0 <= position < db.lineitem.len() ==> db.lineitem[position].l_orderkey != group.id.key,
            Some(last) => {
                &&& exists|position: int| #![trigger db.lineitem[position]] 0 <= position < db.lineitem.len()
                    && db.lineitem[position].l_orderkey == group.id.key && db.lineitem[position].l_shipdate == last
                &&& forall|position: int| #![trigger db.lineitem[position]] 0 <= position < db.lineitem.len()
                    && db.lineitem[position].l_orderkey == group.id.key ==> db.lineitem[position].l_shipdate <= last
            },
        }
    }
}

pub open spec fn listed(ranks: Index<Paged<RankEnt>>, node: int, segment: Seq<u8>, group: GroupCell) -> bool {
    rank_at(ranks, (node, segment), group_rank(group))
}

pub open spec fn group_rank(group: GroupCell) -> KeyView {
    rank_of(group.bound as int, group.id)
}

pub open spec fn rank_at(ranks: Index<Paged<RankEnt>>, outer: KeyView, key: KeyView) -> bool {
    match ranks.at(outer) {
        Some(index) => index.at(key).is_some(),
        None => false,
    }
}

pub open spec fn ranks_wf(ranks: Index<Paged<RankEnt>>) -> bool {
    ranks.wf() && forall|outer: KeyView| #![trigger ranks.at(outer)]
        ranks.at(outer).is_some() ==> ranks.at(outer).unwrap().wf()
}

pub open spec fn rank_present(groups: Seq<GroupCell>, outer: KeyView, rank: KeyView) -> bool {
    exists|position: int| #![trigger groups[position]] 0 <= position < groups.len() && {
        let group = groups[position];
        (*group.segment)@ == outer.1 && covers(group, outer.0) && group_rank(group) == rank
    }
}

pub open spec fn groups_in_pages(pages: Index<Page>, groups: Seq<GroupCell>) -> bool {
    forall|group: GroupCell| #![trigger groups.contains(group)] groups.contains(group) ==
        (exists|page_key: KeyView, position: int| #![trigger pages.at(page_key), (*pages.at(page_key).unwrap().groups)@[position]]
            pages.at(page_key).is_some() && 0 <= position < (*pages.at(page_key).unwrap().groups)@.len()
                && (*pages.at(page_key).unwrap().groups)@[position] == group)
}

pub open spec fn rank_database_ok(pages: Index<Page>, ranks: Index<Paged<RankEnt>>) -> bool {
    &&& ranks_wf(ranks)
    &&& forall|page_key: KeyView, position: int, node: int| #![trigger listed(ranks, node, (*(*pages.at(page_key).unwrap().groups)@[position].segment)@, (*pages.at(page_key).unwrap().groups)@[position])]
        pages.at(page_key).is_some() && 0 <= position < (*pages.at(page_key).unwrap().groups)@.len() ==> {
            let group = (*pages.at(page_key).unwrap().groups)@[position];
            listed(ranks, node, (*group.segment)@, group) == covers(group, node)
        }
    &&& forall|outer: KeyView| #![trigger ranks.at(outer)] ranks.at(outer).is_some() ==> {
        let inner = ranks.at(outer).unwrap();
        &&& inner.wf()
        &&& forall|rank: KeyView| #![trigger inner.at(rank)] inner.at(rank).is_some() ==>
            exists|page_key: KeyView, position: int| #![trigger pages.at(page_key), (*pages.at(page_key).unwrap().groups)@[position]]
                pages.at(page_key).is_some() && 0 <= position < (*pages.at(page_key).unwrap().groups)@.len() && {
                    let group = (*pages.at(page_key).unwrap().groups)@[position];
                    (*group.segment)@ == outer.1 && covers(group, outer.0) && rank == group_rank(group)
                }
    }
}

pub open spec fn state_ok(customers: Custs, pages: Index<Page>, ranks: Index<Paged<RankEnt>>, db: Db) -> bool {
    &&& database_domains(db)
    &&& customers.wf() && pages.wf() && ranks.wf()
    &&& forall|customer: int, segment: Seq<u8>| #![trigger customer_at(customers, customer, segment)]
        customer_at(customers, customer, segment) == customer_sum(db, customer, segment)
    &&& forall|customer: int, key: int| #![trigger reverse_at(customers, customer, key)] reverse_at(customers, customer, key) == reverse_sum(db, customer, key)
    &&& forall|key: int, customer: int, date: int, priority: int| #![trigger order_at(pages, key, customer, date, priority)]
        order_at(pages, key, customer, date, priority) == order_sum(db, key, customer, date, priority)
    &&& forall|key: int, date: int, kind: int| #![trigger ship_at(pages, key, date, kind)]
        0 <= kind <= 2 ==> ship_at(pages, key, date, kind) == ship_sum(db, key, date, kind)
    &&& forall|segment: Seq<u8>, key: int, date: int, priority: int| #![trigger group_at(pages, segment, key, date, priority)]
        group_at(pages, segment, key, date, priority) == weight_sum(db, segment, key, date, priority)
    &&& forall|key: KeyView| #![trigger customers.at(key)] customers.at(key).is_some() ==> key.1.len() == 0 && customer_shape(customers.at(key).unwrap())
    &&& forall|key: KeyView| #![trigger pages.at(key)] pages.at(key).is_some() ==> key.1.len() == 0 && page_shape(pages.at(key).unwrap(), key.0, db)
    &&& forall|page_key: KeyView, position: int, node: int| #![trigger listed(ranks, node, (*(*pages.at(page_key).unwrap().groups)@[position].segment)@, (*pages.at(page_key).unwrap().groups)@[position])]
        pages.at(page_key).is_some() && 0 <= position < (*pages.at(page_key).unwrap().groups)@.len() ==> {
            let group = (*pages.at(page_key).unwrap().groups)@[position];
            listed(ranks, node, (*group.segment)@, group) == covers(group, node)
        }
    &&& forall|outer: KeyView| #![trigger ranks.at(outer)] ranks.at(outer).is_some() ==> {
        let inner = ranks.at(outer).unwrap();
        &&& inner.wf()
        &&& forall|rank: KeyView| #![trigger inner.at(rank)] inner.at(rank).is_some() ==>
            exists|page_key: KeyView, position: int| #![trigger pages.at(page_key), (*pages.at(page_key).unwrap().groups)@[position]]
                pages.at(page_key).is_some() && 0 <= position < (*pages.at(page_key).unwrap().groups)@.len() && {
                    let group = (*pages.at(page_key).unwrap().groups)@[position];
                    (*group.segment)@ == outer.1 && covers(group, outer.0)
                    && rank == group_rank(group)
                }
    }
}

pub open spec fn order_domain(row: OrdersRow) -> bool {
    i64::MIN <= row.o_orderkey <= i64::MAX && i64::MIN <= row.o_custkey <= i64::MAX
        && i32::MIN <= row.o_orderdate <= i32::MAX && i64::MIN <= row.o_shippriority <= i64::MAX
}

pub open spec fn shipment_domain(row: LineitemRow) -> bool {
    i64::MIN <= row.l_orderkey <= i64::MAX && i32::MIN <= row.l_shipdate <= i32::MAX
}

pub open spec fn database_domains(db: Db) -> bool {
    &&& forall|position: int| #![trigger db.customer[position]] 0 <= position < db.customer.len() ==>
        i64::MIN <= db.customer[position].c_custkey <= i64::MAX
    &&& forall|position: int| #![trigger db.orders[position]] 0 <= position < db.orders.len() ==> order_domain(db.orders[position])
    &&& forall|position: int| #![trigger db.lineitem[position]] 0 <= position < db.lineitem.len() ==> shipment_domain(db.lineitem[position])
}

}

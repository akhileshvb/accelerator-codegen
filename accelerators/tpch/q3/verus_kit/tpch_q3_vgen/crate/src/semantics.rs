use vstd::prelude::*;
use crate::custs::Custs;
use crate::paged::{Paged, PageCursor, entry_sorted, Ent};
use crate::rank_ent::*;
use verus_common::sums::*;
use verus_common::rel::*;
use crate::gen_spec::*;
use crate::gen_obligations::*;
use crate::values::group_bytes;
use crate::model::*;
use crate::packed::{group_equiv, groups_equiv};
use verus_prooflib::ordered::Index;
use verus_prooflib::{class_total, lemma_equal_classes};

verus! {

pub proof fn lemma_work_scan(db: Db, customers: Seq<CustomerInput>, orders: Seq<OrderInput>, shipments: Seq<ShipmentInput>,
    work: Map<i64, PageWork>, number: i64)
    requires scan_customers_ok(db, customers), scan_orders_ok(db, orders), scan_shipments_ok(db, shipments),
        work.contains_key(number), work_page_shape(work[number], number as int),
        forall|key: i64, customer: i64, date: i32, priority: i64| #![trigger scratch_order_at(work, key, customer, date, priority)]
            key as int / 256 == number as int ==> scratch_order_at(work, key, customer, date, priority) == order_prefix(orders, key, customer, date, priority),
        forall|key: i64, date: i32, kind: int| #![trigger scratch_ship_at(work, key, date, kind)]
            key as int / 256 == number as int && 0 <= kind <= 2 ==> scratch_ship_at(work, key, date, kind) == ship_prefix(shipments, key, date, kind),
    ensures work_database_ok(work[number], number as int, db),
{
    crate::input_proofs::lemma_scan_domains(db, customers, orders, shipments);
    let page = work[number];
    assert forall|key: int, customer: int, date: int, priority: int| #![trigger order_count(page.orders@, key, customer, date, priority)]
        order_count(page.orders@, key, customer, date, priority) == if key / 256 == number { order_sum(db, key, customer, date, priority) } else { 0int } by {
        let weight = |row: OrderCell| if row.key == key && row.customer == customer && row.date == date && row.priority == priority { row.count as int } else { 0int };
        if key / 256 != number {
            lemma_sum_zero(page.orders@, weight);
        } else if i64::MIN <= key <= i64::MAX && i64::MIN <= customer <= i64::MAX
            && i32::MIN <= date <= i32::MAX && i64::MIN <= priority <= i64::MAX {
            assert((key / 256) as i64 == number);
            assert(key == (key as i64) as int && customer == (customer as i64) as int
                && date == (date as i32) as int && priority == (priority as i64) as int);
            crate::input_proofs::lemma_order_scan(db, orders, customer as i64, key as i64, date as i32, priority as i64);
            assert(order_count(page.orders@, key, customer, date, priority) == scratch_order_at(work, key as i64, customer as i64, date as i32, priority as i64));
        } else {
            lemma_sum_zero(page.orders@, weight);
            lemma_sum_zero(db.orders, |row: OrdersRow| if row.o_orderkey == key && row.o_custkey == customer
                && row.o_orderdate == date && row.o_shippriority == priority { 1int } else { 0int });
        }
    }
    assert forall|key: int, date: int, kind: int| #![trigger ship_count(page.ships@, key, date, kind)]
        0 <= kind <= 2 implies ship_count(page.ships@, key, date, kind) == if key / 256 == number { ship_sum(db, key, date, kind) } else { 0int } by {
        let weight = |row: ShipCell| if row.key == key && row.date == date {
            if kind == 0 { row.count as int } else if kind == 1 { row.revenue as int } else { row.positive as int }
        } else { 0int };
        if key / 256 != number {
            lemma_sum_zero(page.ships@, weight);
        } else if i64::MIN <= key <= i64::MAX && i32::MIN <= date <= i32::MAX {
            assert((key / 256) as i64 == number);
            assert(key == (key as i64) as int && date == (date as i32) as int);
            crate::input_proofs::lemma_ship_scan(db, shipments, key as i64, date as i32, kind);
            assert(ship_count(page.ships@, key, date, kind) == scratch_ship_at(work, key as i64, date as i32, kind));
        } else {
            lemma_sum_zero(page.ships@, weight);
            lemma_sum_zero(db.lineitem, |row: LineitemRow| if row.l_orderkey == key && row.l_shipdate == date {
                let value = row.l_extendedprice * (100 - row.l_discount);
                if kind == 0 { 1int } else if kind == 1 { value } else { if value > 0 { value } else { 0int } }
            } else { 0int });
        }
    }
}

pub proof fn lemma_work_join(work: PageWork, number: int, customers: Custs, db: Db,
    segment: Seq<u8>, key: int, date: int, priority: int)
    requires work_database_ok(work, number, db), customer_database_ok(customers, db),
    ensures column_join(work.orders@, customers, segment, key, date, priority)
        == if key / 256 == number { weight_sum(db, segment, key, date, priority) } else { 0int },
{
    let left = work.orders@;
    let left_project = |row: OrderCell| order_cell_projection(row);
    let right_project = |row: OrdersRow| order_row_projection(row);
    let left_count = |row: OrderCell| row.count as int;
    let right_count = |row: OrdersRow| if row.o_orderkey / 256 == number { 1int } else { 0int };
    assert forall|identity: (int, int, int, int)| #![trigger class_total(left, left_project, left_count, identity)]
        class_total(left, left_project, left_count, identity) == class_total(db.orders, right_project, right_count, identity) by {
        let left_selected = |row: OrderCell| if left_project(row) == identity { left_count(row) } else { 0int };
        let right_selected = |row: OrdersRow| if right_project(row) == identity { right_count(row) } else { 0int };
        assert(order_count(left, identity.0, identity.1, identity.2, identity.3)
            == if identity.0 / 256 == number { order_sum(db, identity.0, identity.1, identity.2, identity.3) } else { 0int });
        lemma_sum_ext_all(left, left_selected,
            |row: OrderCell| if row.key == identity.0 && row.customer == identity.1 && row.date == identity.2 && row.priority == identity.3 { row.count as int } else { 0int });
        if identity.0 / 256 == number {
            lemma_sum_ext_all(db.orders, right_selected,
                |row: OrdersRow| if row.o_orderkey == identity.0 && row.o_custkey == identity.1 && row.o_orderdate == identity.2 && row.o_shippriority == identity.3 { 1int } else { 0int });
        } else { lemma_sum_zero(db.orders, right_selected); }
    }
    let weight = |identity: (int, int, int, int)| if identity.0 == key && identity.2 == date && identity.3 == priority {
        customer_sum(db, identity.1, segment)
    } else { 0int };
    lemma_equal_classes(left, db.orders, left_project, right_project, left_count, right_count, weight);
    let left_weight = |row: OrderCell| left_count(row) * weight(left_project(row));
    let right_weight = |row: OrdersRow| right_count(row) * weight(right_project(row));
    lemma_sum_ext_all(left, left_weight, order_group_measure(customers, segment, key, date, priority));
    if key / 256 == number {
        lemma_sum_ext_all(db.orders, right_weight,
            |row: OrdersRow| if row.o_orderkey == key && row.o_orderdate == date && row.o_shippriority == priority { customer_sum(db, row.o_custkey, segment) } else { 0int });
    } else { lemma_sum_zero(db.orders, right_weight); }
}

pub proof fn lemma_work_positive(work: PageWork, number: int, db: Db, key: int)
    requires work_database_ok(work, number, db), key / 256 == number,
    ensures ship_positive(work.ships@, key) == positive_sum(db, key),
{
    let left = work.ships@;
    let left_project = |row: ShipCell| (row.key as int, row.date as int);
    let right_project = |row: LineitemRow| (row.l_orderkey, row.l_shipdate);
    let left_count = |row: ShipCell| row.positive as int;
    let right_count = |row: LineitemRow| if row.l_orderkey / 256 == number {
        let value = row.l_extendedprice * (100 - row.l_discount);
        if value > 0 { value } else { 0int }
    } else { 0int };
    assert forall|identity: (int, int)| #![trigger class_total(left, left_project, left_count, identity)]
        class_total(left, left_project, left_count, identity) == class_total(db.lineitem, right_project, right_count, identity) by {
        lemma_sum_ext_all(left, |row: ShipCell| if left_project(row) == identity { left_count(row) } else { 0int },
            |row: ShipCell| if row.key == identity.0 && row.date == identity.1 { row.positive as int } else { 0int });
        let right_selected = |row: LineitemRow| if right_project(row) == identity { right_count(row) } else { 0int };
        assert(ship_count(left, identity.0, identity.1, 2)
            == if identity.0 / 256 == number { ship_sum(db, identity.0, identity.1, 2) } else { 0int });
        lemma_ship_measures(left, db, identity.0, identity.1, 2);
        if identity.0 / 256 == number {
            lemma_sum_ext_all(db.lineitem, right_selected,
                |row: LineitemRow| if row.l_orderkey == identity.0 && row.l_shipdate == identity.1 {
                    let value = row.l_extendedprice * (100 - row.l_discount);
                    if value > 0 { value } else { 0int }
                } else { 0int });
        } else { lemma_sum_zero(db.lineitem, right_selected); }
    }
    let weight = |identity: (int, int)| if identity.0 == key { 1int } else { 0int };
    lemma_equal_classes(left, db.lineitem, left_project, right_project, left_count, right_count, weight);
    lemma_sum_ext_all(left, |row: ShipCell| left_count(row) * weight(left_project(row)), ship_positive_weight(key));
    lemma_sum_ext_all(db.lineitem, |row: LineitemRow| right_count(row) * weight(right_project(row)),
        |row: LineitemRow| if row.l_orderkey == key {
            let value = row.l_extendedprice * (100 - row.l_discount);
            if value > 0 { value } else { 0int }
        } else { 0int });
}

pub proof fn lemma_work_ship_presence(work: PageWork, number: int, db: Db, key: int, date: int)
    requires work_database_ok(work, number, db), key / 256 == number,
    ensures (exists|position: int| #![trigger work.ships@[position]] 0 <= position < work.ships@.len()
        && work.ships@[position].key == key && work.ships@[position].date == date)
        == (exists|position: int| #![trigger db.lineitem[position]] 0 <= position < db.lineitem.len()
            && db.lineitem[position].l_orderkey == key && db.lineitem[position].l_shipdate == date),
{
    let left = work.ships@;
    let left_weight = |row: ShipCell| if row.key == key && row.date == date { row.count as int } else { 0int };
    let right_weight = |row: LineitemRow| if row.l_orderkey == key && row.l_shipdate == date { 1int } else { 0int };
    assert(ship_count(left, key, date, 0) == ship_sum(db, key, date, 0));
    lemma_ship_measures(left, db, key, date, 0);
    assert(sum(left, left_weight) == sum(db.lineitem, right_weight));
    if exists|position: int| #![trigger left[position]] 0 <= position < left.len() && left[position].key == key && left[position].date == date {
        let position = choose|position: int| #![trigger left[position]] 0 <= position < left.len() && left[position].key == key && left[position].date == date;
        lemma_sum_pos(left, left_weight, position);
    } else { lemma_sum_zero(left, left_weight); }
    if exists|position: int| #![trigger db.lineitem[position]] 0 <= position < db.lineitem.len()
        && db.lineitem[position].l_orderkey == key && db.lineitem[position].l_shipdate == date {
        let position = choose|position: int| #![trigger db.lineitem[position]] 0 <= position < db.lineitem.len()
            && db.lineitem[position].l_orderkey == key && db.lineitem[position].l_shipdate == date;
        lemma_sum_pos(db.lineitem, right_weight, position);
    } else { lemma_sum_zero(db.lineitem, right_weight); }
}

pub proof fn lemma_ship_measures(rows: Seq<ShipCell>, db: Db, key: int, date: int, kind: int)
    ensures
        ship_count(rows, key, date, kind) == sum(rows, |row: ShipCell| if row.key == key && row.date == date {
            if kind == 0 { row.count as int } else if kind == 1 { row.revenue as int } else { row.positive as int }
        } else { 0int }),
        ship_sum(db, key, date, kind) == sum(db.lineitem, |row: LineitemRow| if row.l_orderkey == key && row.l_shipdate == date {
            let value = row.l_extendedprice * (100 - row.l_discount);
            if kind == 0 { 1int } else if kind == 1 { value } else { if value > 0 { value } else { 0int } }
        } else { 0int }),
        kind == 0 ==> ship_count(rows, key, date, kind) == sum(rows, |row: ShipCell| if row.key == key && row.date == date { row.count as int } else { 0int }),
        kind == 0 ==> ship_sum(db, key, date, kind) == sum(db.lineitem, |row: LineitemRow| if row.l_orderkey == key && row.l_shipdate == date { 1int } else { 0int }),
        kind == 2 ==> ship_count(rows, key, date, kind) == sum(rows, |row: ShipCell| if row.key == key && row.date == date { row.positive as int } else { 0int }),
        kind == 2 ==> ship_sum(db, key, date, kind) == sum(db.lineitem, |row: LineitemRow| if row.l_orderkey == key && row.l_shipdate == date {
            let value = row.l_extendedprice * (100 - row.l_discount); if value > 0 { value } else { 0int }
        } else { 0int }),
{
    let left_weight = |row: ShipCell| if row.key == key && row.date == date {
        if kind == 0 { row.count as int } else if kind == 1 { row.revenue as int } else { row.positive as int }
    } else { 0int };
    let right_weight = |row: LineitemRow| if row.l_orderkey == key && row.l_shipdate == date {
        let value = row.l_extendedprice * (100 - row.l_discount);
        if kind == 0 { 1int } else if kind == 1 { value } else { if value > 0 { value } else { 0int } }
    } else { 0int };
    if kind == 0 {
        lemma_sum_ext_all(rows, left_weight, |row: ShipCell| if row.key == key && row.date == date { row.count as int } else { 0int });
        lemma_sum_ext_all(db.lineitem, right_weight, |row: LineitemRow| if row.l_orderkey == key && row.l_shipdate == date { 1int } else { 0int });
    }
    if kind == 2 {
        lemma_sum_ext_all(rows, left_weight, |row: ShipCell| if row.key == key && row.date == date { row.positive as int } else { 0int });
        lemma_sum_ext_all(db.lineitem, right_weight, |row: LineitemRow| if row.l_orderkey == key && row.l_shipdate == date {
            let value = row.l_extendedprice * (100 - row.l_discount); if value > 0 { value } else { 0int }
        } else { 0int });
    }
}

pub proof fn lemma_work_last(work: PageWork, number: int, db: Db, key: int, last: Option<i32>)
    requires work_database_ok(work, number, db), key / 256 == number, ship_last_ok(work.ships@, key, last),
    ensures match last {
        None => forall|position: int| #![trigger db.lineitem[position]] 0 <= position < db.lineitem.len() ==> db.lineitem[position].l_orderkey != key,
        Some(date) => {
            &&& exists|position: int| #![trigger db.lineitem[position]] 0 <= position < db.lineitem.len()
                && db.lineitem[position].l_orderkey == key && db.lineitem[position].l_shipdate == date
            &&& forall|position: int| #![trigger db.lineitem[position]] 0 <= position < db.lineitem.len()
                && db.lineitem[position].l_orderkey == key ==> db.lineitem[position].l_shipdate <= date
        },
    },
{
    if let Some(date) = last { lemma_work_ship_presence(work, number, db, key, date as int); }
    assert forall|position: int| #![trigger db.lineitem[position]] 0 <= position < db.lineitem.len() && db.lineitem[position].l_orderkey == key implies
        last.is_some() && db.lineitem[position].l_shipdate <= last.unwrap() by {
        let date = db.lineitem[position].l_shipdate;
        lemma_work_ship_presence(work, number, db, key, date);
        let source = choose|source: int| #![trigger work.ships@[source]] 0 <= source < work.ships@.len()
            && work.ships@[source].key == key && work.ships@[source].date == date;
    }
}

pub proof fn lemma_column_weight_equiv(left: Seq<GroupCell>, right: Seq<GroupCell>, segment: Seq<u8>, key: int, date: int, priority: int)
    requires groups_equiv(left, right),
    ensures column_weight(left, segment, key, date, priority) == column_weight(right, segment, key, date, priority),
    decreases left.len(),
{
    if left.len() > 0 {
        let left_prefix = left.drop_last();
        let right_prefix = right.drop_last();
        assert(groups_equiv(left_prefix, right_prefix)) by {
            assert forall|index: int| #![trigger left_prefix[index]] 0 <= index < left_prefix.len() implies group_equiv(left_prefix[index], right_prefix[index]) by {
                assert(left_prefix[index] == left[index]);
                assert(right_prefix[index] == right[index]);
            }
        }
        lemma_column_weight_equiv(left_prefix, right_prefix, segment, key, date, priority);
        let measure = column_measure(segment, key, date, priority);
        lemma_sum_push(left_prefix, left.last(), measure);
        lemma_sum_push(right_prefix, right.last(), measure);
        assert(left =~= left_prefix.push(left.last()));
        assert(right =~= right_prefix.push(right.last()));
        assert(group_equiv(left.last(), right.last()));
    }
}

pub proof fn lemma_page_publication_equiv(page: Page, groups: Seq<GroupCell>, work: PageWork, number: int, customers: Custs, db: Db)
    requires work_database_ok(work, number, db), customer_database_ok(customers, db),
        (*page.orders)@ == work.orders@, (*page.ships)@ == work.ships@,
        groups_equiv((*page.groups)@, groups), groups_sorted(groups),
        forall|position: int| #![trigger groups[position]] 0 <= position < groups.len() ==> {
            let group = groups[position];
            &&& group.weight > 0 && group.bound >= 0 && group_ship_ok(group, work.ships@)
            &&& exists|source: int| #![trigger work.orders@[source]] 0 <= source < work.orders@.len()
                && work.orders@[source].key == group.id.key
        },
        forall|segment: Seq<u8>, key: int, date: int, priority: int| #![trigger column_weight(groups, segment, key, date, priority)]
            column_weight(groups, segment, key, date, priority) == column_join(work.orders@, customers, segment, key, date, priority),
    ensures page_database_ok(page, number, db),
{
    let view = (*page.groups)@;
    assert forall|left: int, right: int| #![trigger view[left], view[right]] 0 <= left < right < view.len() implies group_lt(view[left], view[right]) by {
        assert(group_equiv(view[left], groups[left]) && group_equiv(view[right], groups[right]));
        assert(group_lt(groups[left], groups[right]));
    }
    assert forall|position: int| #![trigger view[position]] 0 <= position < view.len() implies {
        let group = view[position];
        &&& group.weight > 0 && group.bound >= 0 && group_ship_ok(group, work.ships@)
        &&& exists|source: int| #![trigger work.orders@[source]] 0 <= source < work.orders@.len()
            && work.orders@[source].key == group.id.key
    } by {
        assert(group_equiv(view[position], groups[position]));
        let source = choose|source: int| #![trigger work.orders@[source]] 0 <= source < work.orders@.len()
            && work.orders@[source].key == groups[position].id.key;
        assert(work.orders@[source].key == view[position].id.key);
    }
    assert forall|segment: Seq<u8>, key: int, date: int, priority: int| #![trigger column_weight(view, segment, key, date, priority)]
        column_weight(view, segment, key, date, priority) == column_join(work.orders@, customers, segment, key, date, priority) by {
        lemma_column_weight_equiv(view, groups, segment, key, date, priority);
    }
    lemma_page_publication(page, work, number, customers, db);
}

pub proof fn lemma_page_publication(page: Page, work: PageWork, number: int, customers: Custs, db: Db)
    requires work_database_ok(work, number, db), customer_database_ok(customers, db),
        (*page.orders)@ == work.orders@, (*page.ships)@ == work.ships@, groups_sorted((*page.groups)@),
        forall|position: int| #![trigger (*page.groups)@[position]] 0 <= position < (*page.groups)@.len() ==> {
            let group = (*page.groups)@[position];
            &&& group.weight > 0 && group.bound >= 0 && group_ship_ok(group, work.ships@)
            &&& exists|source: int| #![trigger work.orders@[source]] 0 <= source < work.orders@.len()
                && work.orders@[source].key == group.id.key
        },
        forall|segment: Seq<u8>, key: int, date: int, priority: int| #![trigger column_weight((*page.groups)@, segment, key, date, priority)]
            column_weight((*page.groups)@, segment, key, date, priority) == column_join(work.orders@, customers, segment, key, date, priority),
    ensures page_database_ok(page, number, db),
{
    assert forall|position: int| #![trigger (*page.groups)@[position]] 0 <= position < (*page.groups)@.len() implies {
        let group = (*page.groups)@[position];
        &&& group.id.key as int / 256 == number
        &&& group.weight > 0
        &&& group.bound == group.weight * positive_sum(db, group.id.key as int)
        &&& match group.last_ship {
            None => forall|source: int| #![trigger db.lineitem[source]] 0 <= source < db.lineitem.len() ==> db.lineitem[source].l_orderkey != group.id.key,
            Some(last) => {
                &&& exists|source: int| #![trigger db.lineitem[source]] 0 <= source < db.lineitem.len()
                    && db.lineitem[source].l_orderkey == group.id.key && db.lineitem[source].l_shipdate == last
                &&& forall|source: int| #![trigger db.lineitem[source]] 0 <= source < db.lineitem.len()
                    && db.lineitem[source].l_orderkey == group.id.key ==> db.lineitem[source].l_shipdate <= last
            },
        }
    } by {
        let group = (*page.groups)@[position];
        let source = choose|source: int| #![trigger work.orders@[source]] 0 <= source < work.orders@.len()
            && work.orders@[source].key == group.id.key;
        lemma_work_positive(work, number, db, group.id.key as int);
        lemma_work_last(work, number, db, group.id.key as int, group.last_ship);
    }
    assert forall|segment: Seq<u8>, key: int, date: int, priority: int| #![trigger column_weight((*page.groups)@, segment, key, date, priority)]
        column_weight((*page.groups)@, segment, key, date, priority) == if key / 256 == number { weight_sum(db, segment, key, date, priority) } else { 0int } by {
        lemma_work_join(work, number, customers, db, segment, key, date, priority);
    }
}

pub proof fn lemma_page_group_unique(pages: Index<Page>, db: Db, left_key: verus_prooflib::ordered::KeyView,
    left_position: int, right_key: verus_prooflib::ordered::KeyView, right_position: int)
    requires
        forall|target: verus_prooflib::ordered::KeyView| #![trigger pages.at(target)] pages.at(target).is_some() ==>
            target.1.len() == 0 && page_shape(pages.at(target).unwrap(), target.0, db),
        pages.at(left_key).is_some(), pages.at(right_key).is_some(),
        0 <= left_position < (*pages.at(left_key).unwrap().groups)@.len(),
        0 <= right_position < (*pages.at(right_key).unwrap().groups)@.len(),
        (*(*pages.at(left_key).unwrap().groups)@[left_position].segment)@ == (*(*pages.at(right_key).unwrap().groups)@[right_position].segment)@,
        group_bytes((*pages.at(left_key).unwrap().groups)@[left_position].id) == group_bytes((*pages.at(right_key).unwrap().groups)@[right_position].id),
    ensures (*pages.at(left_key).unwrap().groups)@[left_position] == (*pages.at(right_key).unwrap().groups)@[right_position],
{
    let left = (*pages.at(left_key).unwrap().groups)@[left_position];
    let right = (*pages.at(right_key).unwrap().groups)@[right_position];
    crate::values::lemma_group_identity(left.id, right.id);
    assert(left_key == right_key);
    verus_common::bytes::lemma_bytes_lt_irrefl((*left.segment)@);
    if left_position < right_position { assert(group_lt(left, right)); }
    if right_position < left_position { assert(group_lt(right, left)); }
    assert(left_position == right_position);
}

pub proof fn lemma_rank_publication(pages: Index<Page>, ranks: Index<Paged<RankEnt>>, groups: Seq<GroupCell>, db: Db)
    requires ranks_wf(ranks), groups_in_pages(pages, groups),
        forall|target: verus_prooflib::ordered::KeyView| #![trigger pages.at(target)] pages.at(target).is_some() ==>
            target.1.len() == 0 && page_shape(pages.at(target).unwrap(), target.0, db),
        forall|outer: verus_prooflib::ordered::KeyView, rank: verus_prooflib::ordered::KeyView| #![trigger rank_at(ranks, outer, rank)]
            rank_at(ranks, outer, rank) == rank_present(groups, outer, rank),
    ensures rank_database_ok(pages, ranks),
{
    assert forall|page_key: verus_prooflib::ordered::KeyView, position: int, node: int|
        #![trigger listed(ranks, node, (*(*pages.at(page_key).unwrap().groups)@[position].segment)@, (*pages.at(page_key).unwrap().groups)@[position])]
        pages.at(page_key).is_some() && 0 <= position < (*pages.at(page_key).unwrap().groups)@.len() implies {
            let group = (*pages.at(page_key).unwrap().groups)@[position];
            listed(ranks, node, (*group.segment)@, group) == covers(group, node)
        } by {
        let group = (*pages.at(page_key).unwrap().groups)@[position];
        let outer = (node, (*group.segment)@);
        let rank = group_rank(group);
        assert(groups.contains(group));
        if covers(group, node) {
            let source = choose|source: int| #![trigger groups[source]] 0 <= source < groups.len() && groups[source] == group;
            assert(rank_present(groups, outer, rank));
        }
        if rank_at(ranks, outer, rank) {
            let source = choose|source: int| #![trigger groups[source]] 0 <= source < groups.len()
                && (*groups[source].segment)@ == outer.1 && covers(groups[source], outer.0) && group_rank(groups[source]) == rank;
            let candidate = groups[source];
            assert(groups.contains(candidate));
            let (candidate_key, candidate_position) = choose|candidate_key: verus_prooflib::ordered::KeyView, candidate_position: int|
                #![trigger pages.at(candidate_key), (*pages.at(candidate_key).unwrap().groups)@[candidate_position]]
                pages.at(candidate_key).is_some() && 0 <= candidate_position < (*pages.at(candidate_key).unwrap().groups)@.len()
                    && (*pages.at(candidate_key).unwrap().groups)@[candidate_position] == candidate;
            lemma_page_group_unique(pages, db, page_key, position, candidate_key, candidate_position);
            assert(candidate == group);
        }
    }
    assert forall|outer: verus_prooflib::ordered::KeyView| #![trigger ranks.at(outer)] ranks.at(outer).is_some() implies {
        let inner = ranks.at(outer).unwrap();
        &&& inner.wf()
        &&& forall|rank: verus_prooflib::ordered::KeyView| #![trigger inner.at(rank)] inner.at(rank).is_some() ==>
            exists|page_key: verus_prooflib::ordered::KeyView, position: int| #![trigger pages.at(page_key), (*pages.at(page_key).unwrap().groups)@[position]]
                pages.at(page_key).is_some() && 0 <= position < (*pages.at(page_key).unwrap().groups)@.len() && {
                    let group = (*pages.at(page_key).unwrap().groups)@[position];
                    (*group.segment)@ == outer.1 && covers(group, outer.0) && rank == group_rank(group)
                }
    } by {
        let inner = ranks.at(outer).unwrap();
        assert forall|rank: verus_prooflib::ordered::KeyView| #![trigger inner.at(rank)] inner.at(rank).is_some() implies
            exists|page_key: verus_prooflib::ordered::KeyView, position: int| #![trigger pages.at(page_key), (*pages.at(page_key).unwrap().groups)@[position]]
                pages.at(page_key).is_some() && 0 <= position < (*pages.at(page_key).unwrap().groups)@.len() && {
                    let group = (*pages.at(page_key).unwrap().groups)@[position];
                    (*group.segment)@ == outer.1 && covers(group, outer.0) && rank == group_rank(group)
                } by {
            assert(rank_at(ranks, outer, rank));
            let source = choose|source: int| #![trigger groups[source]] 0 <= source < groups.len()
                && (*groups[source].segment)@ == outer.1 && covers(groups[source], outer.0) && group_rank(groups[source]) == rank;
            assert(groups.contains(groups[source]));
            let (page_key, position) = choose|page_key: verus_prooflib::ordered::KeyView, position: int|
                #![trigger pages.at(page_key), (*pages.at(page_key).unwrap().groups)@[position]]
                pages.at(page_key).is_some() && 0 <= position < (*pages.at(page_key).unwrap().groups)@.len()
                    && (*pages.at(page_key).unwrap().groups)@[position] == groups[source];
        }
    }
}

pub open spec fn line_measure(row: LineitemRow, kind: int) -> int {
    if kind == 0 { 1 } else { row.l_extendedprice * (100 - row.l_discount) }
}

pub open spec fn line_suffix(db: Db, key: int, cutoff: int, kind: int) -> int {
    sum(db.lineitem, |row: LineitemRow| if row.l_orderkey == key && row.l_shipdate > cutoff { line_measure(row, kind) } else { 0 })
}

pub open spec fn shipment_suffix(rows: Seq<ShipCell>, key: int, cutoff: int, kind: int) -> int {
    sum(rows, shipment_weight(key, cutoff, kind))
}

pub open spec fn shipment_weight(key: int, cutoff: int, kind: int) -> spec_fn(ShipCell) -> int {
    |row: ShipCell| if row.key == key && row.date > cutoff {
        if kind == 0 { row.count as int } else { row.revenue as int }
    } else { 0int }
}

pub open spec fn selected_weight(key: int, date: int, priority: int, kind: int) -> spec_fn(Tuple) -> int {
    |tuple: Tuple| if tuple.lineitem.l_orderkey == key && tuple.orders.o_orderdate == date
        && tuple.orders.o_shippriority == priority { line_measure(tuple.lineitem, kind) } else { 0 }
}

pub open spec fn order_matches(order: OrdersRow, key: int, date: int, priority: int) -> bool {
    order.o_orderkey == key && order.o_orderdate == date && order.o_shippriority == priority
}

pub open spec fn customer_matches(customer: CustomerRow, order: OrdersRow, segment: Seq<u8>) -> bool {
    customer.c_custkey == order.o_custkey && customer.c_mktsegment == segment
}

pub proof fn lemma_order_factor(db: Db, parameters: Params, customer: CustomerRow, order: OrdersRow,
    key: int, date: int, priority: int, kind: int)
    ensures sum(tuples_2(db, parameters, customer, order), selected_weight(key, date, priority, kind)) ==
        if customer_matches(customer, order, parameters.p1) && order_matches(order, key, date, priority) && date < parameters.p2 {
            line_suffix(db, key, parameters.p2, kind)
        } else { 0 },
{
    let weight = selected_weight(key, date, priority, kind);
    let mapper = |line: LineitemRow| tuples_3(db, parameters, customer, order, line);
    let nested = |line: LineitemRow| sum(mapper(line), weight);
    let active = customer_matches(customer, order, parameters.p1) && order_matches(order, key, date, priority) && date < parameters.p2;
    let reduced = |line: LineitemRow| if active && line.l_orderkey == key && line.l_shipdate > parameters.p2 { line_measure(line, kind) } else { 0int };
    lemma_sum_flat_map(db.lineitem, mapper, weight);
    assert forall|line: LineitemRow| #![trigger nested(line)] nested(line) == reduced(line) by {
        let tuple = Tuple { customer, orders: order, lineitem: line };
        lemma_sum_opt(where_clause(db, parameters, customer, order, line), tuple, weight);
    }
    lemma_sum_ext_all(db.lineitem, nested, reduced);
    if active {
        lemma_sum_ext_all(db.lineitem, reduced,
            |line: LineitemRow| if line.l_orderkey == key && line.l_shipdate > parameters.p2 { line_measure(line, kind) } else { 0int });
    } else { lemma_sum_zero(db.lineitem, reduced); }
}

pub proof fn lemma_customer_factor(db: Db, parameters: Params, customer: CustomerRow,
    key: int, date: int, priority: int, kind: int)
    ensures sum(tuples_1(db, parameters, customer), selected_weight(key, date, priority, kind)) ==
        sum(db.orders, |order: OrdersRow| if customer_matches(customer, order, parameters.p1)
            && order_matches(order, key, date, priority) && date < parameters.p2 { line_suffix(db, key, parameters.p2, kind) } else { 0int }),
{
    let weight = selected_weight(key, date, priority, kind);
    let mapper = |order: OrdersRow| tuples_2(db, parameters, customer, order);
    let nested = |order: OrdersRow| sum(mapper(order), weight);
    let reduced = |order: OrdersRow| if customer_matches(customer, order, parameters.p1)
        && order_matches(order, key, date, priority) && date < parameters.p2 { line_suffix(db, key, parameters.p2, kind) } else { 0int };
    lemma_sum_flat_map(db.orders, mapper, weight);
    assert forall|order: OrdersRow| #![trigger nested(order)] nested(order) == reduced(order) by {
        lemma_order_factor(db, parameters, customer, order, key, date, priority, kind);
    }
    lemma_sum_ext_all(db.orders, nested, reduced);
}

pub proof fn lemma_query_factor(db: Db, parameters: Params, key: int, date: int, priority: int, kind: int)
    ensures sum(tuples(db, parameters), selected_weight(key, date, priority, kind)) ==
        if date < parameters.p2 { weight_sum(db, parameters.p1, key, date, priority) * line_suffix(db, key, parameters.p2, kind) } else { 0 },
{
    let weight = selected_weight(key, date, priority, kind);
    let mapper = |customer: CustomerRow| tuples_1(db, parameters, customer);
    let nested = |customer: CustomerRow| sum(mapper(customer), weight);
    let suffix = line_suffix(db, key, parameters.p2, kind);
    let pair = |customer: CustomerRow, order: OrdersRow| if customer_matches(customer, order, parameters.p1)
        && order_matches(order, key, date, priority) && date < parameters.p2 { suffix } else { 0int };
    let customer_nested = |customer: CustomerRow| sum(db.orders, |order: OrdersRow| pair(customer, order));
    lemma_sum_flat_map(db.customer, mapper, weight);
    assert forall|customer: CustomerRow| #![trigger nested(customer)] nested(customer) == customer_nested(customer) by {
        lemma_customer_factor(db, parameters, customer, key, date, priority, kind);
        let reduced = |order: OrdersRow| if customer_matches(customer, order, parameters.p1)
            && order_matches(order, key, date, priority) && date < parameters.p2 { line_suffix(db, key, parameters.p2, kind) } else { 0int };
        lemma_sum_ext_all(db.orders, reduced, |order: OrdersRow| pair(customer, order));
    }
    lemma_sum_ext_all(db.customer, nested, customer_nested);
    let order_nested = |order: OrdersRow| sum(db.customer, |customer: CustomerRow| pair(customer, order));
    lemma_sum_swap(db.customer, db.orders, pair, customer_nested, order_nested);
    let order_reduced = |order: OrdersRow| if order_matches(order, key, date, priority) && date < parameters.p2 {
        suffix * customer_sum(db, order.o_custkey, parameters.p1)
    } else { 0int };
    assert forall|order: OrdersRow| #![trigger order_nested(order)] order_nested(order) == order_reduced(order) by {
        let raw = |customer: CustomerRow| if customer_matches(customer, order, parameters.p1) { 1int } else { 0int };
        let scaled = |customer: CustomerRow| pair(customer, order);
        if order_matches(order, key, date, priority) && date < parameters.p2 {
            lemma_sum_scale(db.customer, raw, scaled, suffix);
            lemma_sum_ext_all(db.customer, raw, |customer: CustomerRow| if customer.c_custkey == order.o_custkey && customer.c_mktsegment == parameters.p1 { 1int } else { 0int });
        } else { lemma_sum_zero(db.customer, scaled); }
    }
    lemma_sum_ext_all(db.orders, order_nested, order_reduced);
    if date < parameters.p2 {
        let raw = |order: OrdersRow| if order_matches(order, key, date, priority) { customer_sum(db, order.o_custkey, parameters.p1) } else { 0int };
        lemma_sum_scale(db.orders, raw, order_reduced, suffix);
        lemma_sum_ext_all(db.orders, raw, |order: OrdersRow| if order.o_orderkey == key && order.o_orderdate == date && order.o_shippriority == priority { customer_sum(db, order.o_custkey, parameters.p1) } else { 0int });
        assert(suffix * weight_sum(db, parameters.p1, key, date, priority) == weight_sum(db, parameters.p1, key, date, priority) * suffix) by (nonlinear_arith);
    } else { lemma_sum_zero(db.orders, order_reduced); }
}

pub proof fn lemma_sql_group(db: Db, parameters: Params, key: int, date: int, priority: int)
    ensures
        group_count(db, parameters, key, date, priority) == if date < parameters.p2 {
            weight_sum(db, parameters.p1, key, date, priority) * line_suffix(db, key, parameters.p2, 0)
        } else { 0 },
        group_sum(db, parameters, key, date, priority, |tuple: Tuple| tuple.lineitem.l_extendedprice * (100 - tuple.lineitem.l_discount)) == if date < parameters.p2 {
            weight_sum(db, parameters.p1, key, date, priority) * line_suffix(db, key, parameters.p2, 1)
        } else { 0 },
{
    lemma_query_factor(db, parameters, key, date, priority, 0);
    lemma_query_factor(db, parameters, key, date, priority, 1);
    let count_weight = |tuple: Tuple| if tuple.lineitem.l_orderkey == key && tuple.orders.o_orderdate == date && tuple.orders.o_shippriority == priority { 1int } else { 0int };
    let revenue = |tuple: Tuple| tuple.lineitem.l_extendedprice * (100 - tuple.lineitem.l_discount);
    let revenue_weight = |tuple: Tuple| if tuple.lineitem.l_orderkey == key && tuple.orders.o_orderdate == date && tuple.orders.o_shippriority == priority { revenue(tuple) } else { 0int };
    assert(selected_weight(key, date, priority, 0) =~= count_weight);
    assert(selected_weight(key, date, priority, 1) =~= revenue_weight);
    assert(group_sum(db, parameters, key, date, priority, revenue) == sum(tuples(db, parameters), revenue_weight));
    lemma_sum_ext_all(tuples(db, parameters), selected_weight(key, date, priority, 0),
        |tuple: Tuple| if tuple.lineitem.l_orderkey == key && tuple.orders.o_orderdate == date && tuple.orders.o_shippriority == priority { 1int } else { 0int });
    lemma_sum_ext_all(tuples(db, parameters), selected_weight(key, date, priority, 1),
        |tuple: Tuple| if tuple.lineitem.l_orderkey == key && tuple.orders.o_orderdate == date && tuple.orders.o_shippriority == priority { tuple.lineitem.l_extendedprice * (100 - tuple.lineitem.l_discount) } else { 0int });
}

pub proof fn lemma_positive_revenue(db: Db, key: int)
    ensures positive_sum(db, key) >= 0,
{
    let weight = |row: LineitemRow| if row.l_orderkey == key {
        let value = row.l_extendedprice * (100 - row.l_discount);
        if value > 0 { value } else { 0int }
    } else { 0int };
    lemma_sum_nonneg(db.lineitem, weight);
}

pub proof fn lemma_page_bounds(page: &Page, db: Db, number: int)
    requires page_shape(*page, number, db),
    ensures forall|index: int| #![trigger (*page.groups)@[index]] 0 <= index < (*page.groups)@.len() ==>
        (*page.groups)@[index].weight > 0 && (*page.groups)@[index].bound >= 0,
{
    assert forall|index: int| #![trigger (*page.groups)@[index]] 0 <= index < (*page.groups)@.len() implies
        (*page.groups)@[index].weight > 0 && (*page.groups)@[index].bound >= 0 by {
        let group = (*page.groups)@[index];
        lemma_positive_revenue(db, group.id.key as int);
        assert(group.bound >= 0) by (nonlinear_arith)
            requires group.weight > 0, positive_sum(db, group.id.key as int) >= 0,
                group.bound as int == group.weight as int * positive_sum(db, group.id.key as int);
    }
}

pub open spec fn order_cell_projection(row: OrderCell) -> (int, int, int, int) {
    (row.key as int, row.customer as int, row.date as int, row.priority as int)
}

pub open spec fn order_row_projection(row: OrdersRow) -> (int, int, int, int) {
    (row.o_orderkey, row.o_custkey, row.o_orderdate, row.o_shippriority)
}

pub proof fn lemma_page_reverse(customers: Custs, pages: Index<Page>, ranks: Index<Paged<RankEnt>>,
    db: Db, page: &Page, number: int, customer: i64, key: i64)
    requires state_ok(customers, pages, ranks, db), pages.at((number, Seq::empty())) == Some(*page),
    ensures sum((*page.orders)@, |row: OrderCell| if row.key == key && row.customer == customer { row.count as int } else { 0 })
        == if key as int / 256 == number { reverse_at(customers, customer as int, key as int) } else { 0 },
{
    let left = (*page.orders)@;
    let left_project = |row: OrderCell| order_cell_projection(row);
    let right_project = |row: OrdersRow| order_row_projection(row);
    let left_count = |row: OrderCell| row.count as int;
    let right_count = |row: OrdersRow| if row.o_orderkey / 256 == number { 1int } else { 0int };
    assert forall|identity: (int, int, int, int)| #![trigger class_total(left, left_project, left_count, identity)]
        class_total(left, left_project, left_count, identity) == class_total(db.orders, right_project, right_count, identity) by {
        let left_selected = |row: OrderCell| if left_project(row) == identity { left_count(row) } else { 0int };
        let right_selected = |row: OrdersRow| if right_project(row) == identity { right_count(row) } else { 0int };
        if identity.0 / 256 == number {
            assert(order_at(pages, identity.0, identity.1, identity.2, identity.3) == order_sum(db, identity.0, identity.1, identity.2, identity.3));
            lemma_sum_ext_all(left, left_selected,
                |row: OrderCell| if row.key == identity.0 && row.customer == identity.1 && row.date == identity.2 && row.priority == identity.3 { row.count as int } else { 0int });
            lemma_sum_ext_all(db.orders, right_selected,
                |row: OrdersRow| if row.o_orderkey == identity.0 && row.o_custkey == identity.1 && row.o_orderdate == identity.2 && row.o_shippriority == identity.3 { 1int } else { 0int });
        } else {
            assert forall|index: int| #![trigger left[index]] 0 <= index < left.len() implies left_selected(left[index]) == 0 by {
                assert(left[index].key as int / 256 == number);
            }
            lemma_sum_zero(left, left_selected);
            lemma_sum_zero(db.orders, right_selected);
        }
    }
    let weight = |identity: (int, int, int, int)| if identity.0 == key && identity.1 == customer { 1int } else { 0int };
    lemma_equal_classes(left, db.orders, left_project, right_project, left_count, right_count, weight);
    let left_weight = |row: OrderCell| left_count(row) * weight(left_project(row));
    let right_weight = |row: OrdersRow| right_count(row) * weight(right_project(row));
    assert(sum(left, left_weight) == sum(db.orders, right_weight));
    lemma_sum_ext_all(left, left_weight, |row: OrderCell| if row.key == key && row.customer == customer { row.count as int } else { 0int });
    if key as int / 256 == number {
        lemma_sum_ext_all(db.orders, right_weight, |row: OrdersRow| if row.o_orderkey == key && row.o_custkey == customer { 1int } else { 0int });
        lemma_sum_ext_all(db.orders,
            |row: OrdersRow| if row.o_orderkey == key && row.o_custkey == customer { 1int } else { 0int },
            |row: OrdersRow| if row.o_custkey == customer as int && row.o_orderkey == key as int { 1int } else { 0int });
        assert(reverse_at(customers, customer as int, key as int) == reverse_sum(db, customer as int, key as int));
    } else { lemma_sum_zero(db.orders, right_weight); }
}

pub proof fn lemma_missing_page_reverse(customers: Custs, pages: Index<Page>, ranks: Index<Paged<RankEnt>>,
    db: Db, number: int, customer: i64, key: i64)
    requires state_ok(customers, pages, ranks, db), pages.at((number, Seq::empty())).is_none(),
        key as int / 256 == number,
    ensures reverse_at(customers, customer as int, key as int) == 0,
{
    let weight = |row: OrdersRow| if row.o_custkey == customer && row.o_orderkey == key { 1int } else { 0int };
    assert forall|index: int| #![trigger db.orders[index]] 0 <= index < db.orders.len() implies weight(db.orders[index]) == 0 by {
        let row = db.orders[index];
        if row.o_orderkey == key && row.o_custkey == customer {
            let selected = |entry: OrdersRow| if entry.o_orderkey == key && entry.o_custkey == customer
                && entry.o_orderdate == row.o_orderdate && entry.o_shippriority == row.o_shippriority { 1int } else { 0int };
            assert(order_at(pages, key as int, customer as int, row.o_orderdate, row.o_shippriority) == 0);
            lemma_sum_ge_term(db.orders, selected, index);
            assert(sum(db.orders, selected) == order_sum(db, key as int, customer as int, row.o_orderdate, row.o_shippriority));
        }
    }
    lemma_sum_zero(db.orders, weight);
}

pub proof fn lemma_shipment_suffix(customers: Custs, pages: Index<Page>, ranks: Index<Paged<RankEnt>>,
    db: Db, page: &Page, number: int, key: int, cutoff: int, kind: int)
    requires state_ok(customers, pages, ranks, db), pages.at((number, Seq::empty())) == Some(*page),
        key / 256 == number, 0 <= kind <= 1,
    ensures shipment_suffix((*page.ships)@, key, cutoff, kind) == line_suffix(db, key, cutoff, kind),
{
    let left = (*page.ships)@;
    let left_project = |row: ShipCell| (row.key as int, row.date as int);
    let right_project = |row: LineitemRow| (row.l_orderkey, row.l_shipdate);
    let left_count = |row: ShipCell| if kind == 0 { row.count as int } else { row.revenue as int };
    let right_count = |row: LineitemRow| if row.l_orderkey / 256 == number { line_measure(row, kind) } else { 0int };
    assert forall|identity: (int, int)| #![trigger class_total(left, left_project, left_count, identity)]
        class_total(left, left_project, left_count, identity) == class_total(db.lineitem, right_project, right_count, identity) by {
        let left_selected = |row: ShipCell| if left_project(row) == identity { left_count(row) } else { 0int };
        let right_selected = |row: LineitemRow| if right_project(row) == identity { right_count(row) } else { 0int };
        if identity.0 / 256 == number {
            assert(ship_at(pages, identity.0, identity.1, kind) == ship_sum(db, identity.0, identity.1, kind));
            lemma_sum_ext_all(left, left_selected, |row: ShipCell| if row.key == identity.0 && row.date == identity.1 {
                if kind == 0 { row.count as int } else if kind == 1 { row.revenue as int } else { row.positive as int }
            } else { 0int });
            lemma_sum_ext_all(db.lineitem, right_selected, |row: LineitemRow| if row.l_orderkey == identity.0 && row.l_shipdate == identity.1 {
                let value = row.l_extendedprice * (100 - row.l_discount);
                if kind == 0 { 1int } else if kind == 1 { value } else { if value > 0 { value } else { 0int } }
            } else { 0int });
        } else {
            assert forall|index: int| #![trigger left[index]] 0 <= index < left.len() implies left_selected(left[index]) == 0 by {
                assert(left[index].key as int / 256 == number);
            }
            lemma_sum_zero(left, left_selected);
            lemma_sum_zero(db.lineitem, right_selected);
        }
    }
    let weight = |identity: (int, int)| if identity.0 == key && identity.1 > cutoff { 1int } else { 0int };
    lemma_equal_classes(left, db.lineitem, left_project, right_project, left_count, right_count, weight);
    let left_weight = |row: ShipCell| left_count(row) * weight(left_project(row));
    let right_weight = |row: LineitemRow| right_count(row) * weight(right_project(row));
    assert(sum(left, left_weight) == sum(db.lineitem, right_weight));
    lemma_sum_ext_all(left, left_weight, |row: ShipCell| if row.key == key && row.date > cutoff {
        if kind == 0 { row.count as int } else { row.revenue as int }
    } else { 0int });
    lemma_sum_ext_all(db.lineitem, right_weight, |row: LineitemRow| if row.l_orderkey == key && row.l_shipdate > cutoff { line_measure(row, kind) } else { 0int });
}

pub proof fn lemma_group_weight(customers: Custs, pages: Index<Page>, ranks: Index<Paged<RankEnt>>,
    db: Db, page: &Page, number: int, position: int)
    requires state_ok(customers, pages, ranks, db), pages.at((number, Seq::empty())) == Some(*page),
        0 <= position < (*page.groups)@.len(),
    ensures ({ let group = (*page.groups)@[position];
        group.weight as int == weight_sum(db, (*group.segment)@, group.id.key as int, group.id.date as int, group.id.priority as int) }),
{
    let groups = (*page.groups)@;
    let group = groups[position];
    let weight = |row: GroupCell| if (*row.segment)@ == (*group.segment)@ && row.id.key == group.id.key as int
        && row.id.date == group.id.date as int && row.id.priority == group.id.priority as int { row.weight as int } else { 0int };
    assert forall|index: int| #![trigger groups[index]] 0 <= index < groups.len() && index != position implies weight(groups[index]) == 0 by {
        if index < position { assert(group_lt(groups[index], group)); }
        else { assert(group_lt(group, groups[index])); }
        verus_common::bytes::lemma_bytes_lt_irrefl((*group.segment)@);
    }
    lemma_sum_single_match(groups, weight, position);
    assert(group_at(pages, (*group.segment)@, group.id.key as int, group.id.date as int, group.id.priority as int) == sum(groups, weight));
}

pub proof fn lemma_suffix_bound(db: Db, key: int, cutoff: int)
    ensures line_suffix(db, key, cutoff, 1) <= positive_sum(db, key),
{
    let actual = |row: LineitemRow| if row.l_orderkey == key && row.l_shipdate > cutoff { line_measure(row, 1) } else { 0int };
    let positive = |row: LineitemRow| if row.l_orderkey == key {
        let value = row.l_extendedprice * (100 - row.l_discount);
        if value > 0 { value } else { 0int }
    } else { 0int };
    lemma_sum_le(db.lineitem, actual, positive);
}

}

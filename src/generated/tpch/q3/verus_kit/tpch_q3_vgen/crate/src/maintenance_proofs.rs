use vstd::prelude::*;
use verus_common::sums::*;
use crate::gen_spec::*;
use crate::gen_obligations::*;
use crate::model::*;
use verus_prooflib::ordered::{Index, KeyView};

verus! {

pub open spec fn page_numbers(keys: Seq<i64>) -> Set<int> {
    keys.map_values(|key: i64| key as int).to_set()
}

pub proof fn lemma_page_numbers(keys: Seq<i64>, number: int)
    ensures page_numbers(keys).contains(number) == (i64::MIN <= number <= i64::MAX && keys.contains(number as i64)),
{
    let projected = keys.map_values(|key: i64| key as int);
    if projected.contains(number) {
        let position = choose|position: int| #![trigger projected[position]] 0 <= position < projected.len() && projected[position] == number;
        assert(keys[position] as int == number);
        assert(keys.contains(number as i64));
    }
    if i64::MIN <= number <= i64::MAX && keys.contains(number as i64) {
        let position = choose|position: int| #![trigger keys[position]] 0 <= position < keys.len() && keys[position] == number as i64;
        assert(projected[position] == number);
        assert(projected.contains(number));
    }
}

pub proof fn lemma_page_tail(keys: Seq<i64>, index: int)
    requires keys.no_duplicates(), 0 <= index < keys.len(),
    ensures page_numbers(keys.skip(index + 1)) == page_numbers(keys.skip(index)).remove(keys[index] as int),
{
    let current = page_numbers(keys.skip(index));
    let next = page_numbers(keys.skip(index + 1));
    assert(next =~= current.remove(keys[index] as int)) by {
        assert forall|number: int| #![trigger next.contains(number)] next.contains(number) == current.remove(keys[index] as int).contains(number) by {
            lemma_page_numbers(keys.skip(index), number);
            lemma_page_numbers(keys.skip(index + 1), number);
            if keys.skip(index + 1).contains(number as i64) {
                let source = choose|source: int| #![trigger keys.skip(index + 1)[source]] 0 <= source < keys.skip(index + 1).len() && keys.skip(index + 1)[source] == number as i64;
                assert(keys[index + 1 + source] == number as i64);
                assert(keys[index + 1 + source] != keys[index]);
                assert(keys.skip(index)[source + 1] == number as i64);
                assert(keys.skip(index).contains(number as i64));
            }
            if current.contains(number) && number != keys[index] as int {
                let source = choose|source: int| #![trigger keys.skip(index)[source]] 0 <= source < keys.skip(index).len() && keys.skip(index)[source] == number as i64;
                assert(source > 0);
                assert(keys.skip(index + 1)[source - 1] == number as i64);
                assert(keys.skip(index + 1).contains(number as i64));
            }
        }
    }
}

pub open spec fn page_partial(pages: Index<Page>, before: Db, after: Db, pending: Set<int>) -> bool {
    &&& pages.wf()
    &&& forall|target: KeyView| #![trigger pages.at(target)] pages.at(target).is_some() ==>
        target.1.len() == 0 && page_database_ok(pages.at(target).unwrap(), target.0, if pending.contains(target.0) { before } else { after })
    &&& forall|key: int, customer: int, date: int, priority: int| #![trigger order_at(pages, key, customer, date, priority)]
        order_at(pages, key, customer, date, priority) == order_sum(if pending.contains(key / 256) { before } else { after }, key, customer, date, priority)
    &&& forall|key: int, date: int, kind: int| #![trigger ship_at(pages, key, date, kind)] 0 <= kind <= 2 ==>
        ship_at(pages, key, date, kind) == ship_sum(if pending.contains(key / 256) { before } else { after }, key, date, kind)
    &&& forall|segment: Seq<u8>, key: int, date: int, priority: int| #![trigger group_at(pages, segment, key, date, priority)]
        group_at(pages, segment, key, date, priority) == weight_sum(if pending.contains(key / 256) { before } else { after }, segment, key, date, priority)
}

pub proof fn lemma_partial_share(source: Index<Page>, copy: Index<Page>, before: Db, after: Db, pending: Set<int>)
    requires page_partial(source, before, after, pending), copy.wf(),
        forall|target: KeyView| #![trigger copy.at(target)] copy.at(target) == source.at(target),
    ensures page_partial(copy, before, after, pending),
{
    assert forall|key: int, customer: int, date: int, priority: int| #![trigger order_at(copy, key, customer, date, priority)]
        order_at(copy, key, customer, date, priority) == order_sum(if pending.contains(key / 256) { before } else { after }, key, customer, date, priority) by {
        assert(copy.at((key / 256, Seq::<u8>::empty())) == source.at((key / 256, Seq::<u8>::empty())));
        assert(order_at(source, key, customer, date, priority) == order_sum(if pending.contains(key / 256) { before } else { after }, key, customer, date, priority));
    }
    assert forall|key: int, date: int, kind: int| #![trigger ship_at(copy, key, date, kind)] 0 <= kind <= 2 implies
        ship_at(copy, key, date, kind) == ship_sum(if pending.contains(key / 256) { before } else { after }, key, date, kind) by {
        assert(copy.at((key / 256, Seq::<u8>::empty())) == source.at((key / 256, Seq::<u8>::empty())));
        assert(ship_at(source, key, date, kind) == ship_sum(if pending.contains(key / 256) { before } else { after }, key, date, kind));
    }
    assert forall|segment: Seq<u8>, key: int, date: int, priority: int| #![trigger group_at(copy, segment, key, date, priority)]
        group_at(copy, segment, key, date, priority) == weight_sum(if pending.contains(key / 256) { before } else { after }, segment, key, date, priority) by {
        assert(copy.at((key / 256, Seq::<u8>::empty())) == source.at((key / 256, Seq::<u8>::empty())));
        assert(group_at(source, segment, key, date, priority) == weight_sum(if pending.contains(key / 256) { before } else { after }, segment, key, date, priority));
    }
}

pub proof fn lemma_partial_step(pages: Index<Page>, next: Index<Page>, before: Db, after: Db, pending: Set<int>, key: i64, page: Page)
    requires page_partial(pages, before, after, pending), pending.contains(key as int), page_database_ok(page, key as int, after),
        forall|target: KeyView| #![trigger next.at(target)] next.at(target) == if target == (key as int, Seq::<u8>::empty()) {
            if (*page.orders)@.len() == 0 && (*page.ships)@.len() == 0 && (*page.groups)@.len() == 0 { None } else { Some(page) }
        } else { pages.at(target) }, next.wf(),
    ensures page_partial(next, before, after, pending.remove(key as int)),
{
    let remaining = pending.remove(key as int);
    assert forall|target: KeyView| #![trigger next.at(target)] next.at(target).is_some() implies
        target.1.len() == 0 && page_database_ok(next.at(target).unwrap(), target.0, if remaining.contains(target.0) { before } else { after }) by {
        if target == (key as int, Seq::<u8>::empty()) { assert(next.at(target) == Some(page)); }
        else {
            assert(next.at(target) == pages.at(target));
            assert(target.1 =~= Seq::<u8>::empty());
            assert(target.0 != key);
        }
    }
    assert forall|orderkey: int, customer: int, date: int, priority: int| #![trigger order_at(next, orderkey, customer, date, priority)]
        order_at(next, orderkey, customer, date, priority) == order_sum(if remaining.contains(orderkey / 256) { before } else { after }, orderkey, customer, date, priority) by {
        if orderkey / 256 == key {
            assert(order_count((*page.orders)@, orderkey, customer, date, priority) == order_sum(after, orderkey, customer, date, priority));
            if next.at((key as int, Seq::empty())).is_none() {
                assert((*page.orders)@.len() == 0);
                lemma_sum_zero((*page.orders)@, |row: OrderCell| if row.key == orderkey && row.customer == customer && row.date == date && row.priority == priority { row.count as int } else { 0int });
            }
        } else {
            assert(next.at((orderkey / 256, Seq::<u8>::empty())) == pages.at((orderkey / 256, Seq::<u8>::empty())));
            assert(order_at(pages, orderkey, customer, date, priority) == order_sum(if pending.contains(orderkey / 256) { before } else { after }, orderkey, customer, date, priority));
        }
    }
    assert forall|orderkey: int, date: int, kind: int| #![trigger ship_at(next, orderkey, date, kind)] 0 <= kind <= 2 implies
        ship_at(next, orderkey, date, kind) == ship_sum(if remaining.contains(orderkey / 256) { before } else { after }, orderkey, date, kind) by {
        if orderkey / 256 == key {
            assert(ship_count((*page.ships)@, orderkey, date, kind) == ship_sum(after, orderkey, date, kind));
            if next.at((key as int, Seq::empty())).is_none() {
                assert((*page.ships)@.len() == 0);
                lemma_sum_zero((*page.ships)@, |row: ShipCell| if row.key == orderkey && row.date == date { if kind == 0 { row.count as int } else if kind == 1 { row.revenue as int } else { row.positive as int } } else { 0int });
            }
        } else {
            assert(next.at((orderkey / 256, Seq::<u8>::empty())) == pages.at((orderkey / 256, Seq::<u8>::empty())));
            assert(ship_at(pages, orderkey, date, kind) == ship_sum(if pending.contains(orderkey / 256) { before } else { after }, orderkey, date, kind));
        }
    }
    assert forall|segment: Seq<u8>, orderkey: int, date: int, priority: int| #![trigger group_at(next, segment, orderkey, date, priority)]
        group_at(next, segment, orderkey, date, priority) == weight_sum(if remaining.contains(orderkey / 256) { before } else { after }, segment, orderkey, date, priority) by {
        if orderkey / 256 == key {
            assert(column_weight((*page.groups)@, segment, orderkey, date, priority) == weight_sum(after, segment, orderkey, date, priority));
            if next.at((key as int, Seq::empty())).is_none() {
                assert((*page.groups)@.len() == 0);
                lemma_sum_zero((*page.groups)@, column_measure(segment, orderkey, date, priority));
            }
        } else {
            assert(next.at((orderkey / 256, Seq::<u8>::empty())) == pages.at((orderkey / 256, Seq::<u8>::empty())));
            assert(group_at(pages, segment, orderkey, date, priority) == weight_sum(if pending.contains(orderkey / 256) { before } else { after }, segment, orderkey, date, priority));
        }
    }
}

pub proof fn lemma_unselected(before: Db, after: Db, customers: Seq<CustomerDelta>, lines: Seq<(LineitemRow, int)>, orders: Seq<(OrdersRow, int)>, selected: Set<int>, number: int)
    requires database_domains(before), capture_ok(before, after, customer_delta_seq_view(customers), lines, orders), !selected.contains(number),
        forall|position: int| #![trigger lines[position]] 0 <= position < lines.len() ==> selected.contains(lines[position].0.l_orderkey / 256),
        forall|position: int| #![trigger orders[position]] 0 <= position < orders.len() ==> selected.contains(orders[position].0.o_orderkey / 256),
        forall|position: int, key: i64| #![trigger customers[position], reverse_sum(after, customers[position].row.c_custkey as int, key as int)]
            0 <= position < customers.len() && reverse_sum(after, customers[position].row.c_custkey as int, key as int) > 0 ==> selected.contains(key as int / 256),
    ensures
        forall|position: int| #![trigger lines[position]] 0 <= position < lines.len() ==> lines[position].0.l_orderkey / 256 != number,
        forall|position: int| #![trigger orders[position]] 0 <= position < orders.len() ==> orders[position].0.o_orderkey / 256 != number,
        forall|position: int, key: int| #![trigger customers[position], reverse_sum(before, customers[position].row.c_custkey as int, key)]
            0 <= position < customers.len() && key / 256 == number ==> reverse_sum(before, customers[position].row.c_custkey as int, key) == 0,
{
    assert forall|position: int, key: int| #![trigger customers[position], reverse_sum(before, customers[position].row.c_custkey as int, key)]
        0 <= position < customers.len() && key / 256 == number implies reverse_sum(before, customers[position].row.c_custkey as int, key) == 0 by {
        let customer = customers[position].row.c_custkey as int;
        let weight = |row: OrdersRow| if row.o_custkey == customer && row.o_orderkey == key { 1int } else { 0int };
        lemma_sum_nonneg(before.orders, weight);
        if key < i64::MIN || key > i64::MAX { lemma_sum_zero(before.orders, weight); }
        else {
            lemma_fact_locality(before, after, customer_delta_seq_view(customers), lines, orders, key, customer, 0, 0, 0);
            if reverse_sum(before, customer, key) > 0 { assert(selected.contains(key / 256)); }
        }
    }
}

pub proof fn lemma_initial_pages(customer_index: Index<CustomerCell>, pages: Index<Page>, ranks: Index<Index<()>>, before: Db, after: Db,
    customers: Seq<CustomerDelta>, lines: Seq<(LineitemRow, int)>, orders: Seq<(OrdersRow, int)>, selected: Set<int>)
    requires state_ok(customer_index, pages, ranks, before), capture_ok(before, after, customer_delta_seq_view(customers), lines, orders),
        forall|position: int| #![trigger lines[position]] 0 <= position < lines.len() ==> selected.contains(lines[position].0.l_orderkey / 256),
        forall|position: int| #![trigger orders[position]] 0 <= position < orders.len() ==> selected.contains(orders[position].0.o_orderkey / 256),
        forall|position: int, key: i64| #![trigger customers[position], reverse_sum(after, customers[position].row.c_custkey as int, key as int)]
            0 <= position < customers.len() && reverse_sum(after, customers[position].row.c_custkey as int, key as int) > 0 ==> selected.contains(key as int / 256),
    ensures page_partial(pages, before, after, selected),
{
    assert forall|target: KeyView| #![trigger pages.at(target)] pages.at(target).is_some() implies target.1.len() == 0 &&
        page_database_ok(pages.at(target).unwrap(), target.0, if selected.contains(target.0) { before } else { after }) by {
        assert(target.1 =~= Seq::<u8>::empty());
        let page = pages.at(target).unwrap();
        lemma_state_page(customer_index, pages, ranks, before, target.0, page);
        if !selected.contains(target.0) {
            lemma_unselected(before, after, customers, lines, orders, selected, target.0);
            lemma_page_frame(page, target.0, before, after, customers, lines, orders);
        }
    }
    assert forall|key: int, customer: int, date: int, priority: int| #![trigger order_at(pages, key, customer, date, priority)]
        order_at(pages, key, customer, date, priority) == order_sum(if selected.contains(key / 256) { before } else { after }, key, customer, date, priority) by {
        if !selected.contains(key / 256) {
            lemma_unselected(before, after, customers, lines, orders, selected, key / 256);
            lemma_fact_locality(before, after, customer_delta_seq_view(customers), lines, orders, key, customer, date, priority, 0);
        }
    }
    assert forall|key: int, date: int, kind: int| #![trigger ship_at(pages, key, date, kind)] 0 <= kind <= 2 implies
        ship_at(pages, key, date, kind) == ship_sum(if selected.contains(key / 256) { before } else { after }, key, date, kind) by {
        if !selected.contains(key / 256) {
            lemma_unselected(before, after, customers, lines, orders, selected, key / 256);
            lemma_fact_locality(before, after, customer_delta_seq_view(customers), lines, orders, key, 0, date, 0, kind);
        }
    }
    assert forall|segment: Seq<u8>, key: int, date: int, priority: int| #![trigger group_at(pages, segment, key, date, priority)]
        group_at(pages, segment, key, date, priority) == weight_sum(if selected.contains(key / 256) { before } else { after }, segment, key, date, priority) by {
        if !selected.contains(key / 256) {
            lemma_unselected(before, after, customers, lines, orders, selected, key / 256);
            lemma_weight_locality(before, after, customers, lines, orders, segment, key, date, priority);
        }
    }
}

pub open spec fn database_last(db: Db, key: int, last: Option<i32>) -> bool {
    match last {
        None => forall|position: int| #![trigger db.lineitem[position]] 0 <= position < db.lineitem.len() ==> db.lineitem[position].l_orderkey != key,
        Some(date) => {
            &&& exists|position: int| #![trigger db.lineitem[position]] 0 <= position < db.lineitem.len() && db.lineitem[position].l_orderkey == key && db.lineitem[position].l_shipdate == date
            &&& forall|position: int| #![trigger db.lineitem[position]] 0 <= position < db.lineitem.len() && db.lineitem[position].l_orderkey == key ==> db.lineitem[position].l_shipdate <= date
        },
    }
}

pub proof fn lemma_line_locality(before: Db, after: Db, customers: Seq<(CustomerRow, int)>, lines: Seq<(LineitemRow, int)>, orders: Seq<(OrdersRow, int)>, key: int, last: Option<i32>)
    requires capture_ok(before, after, customers, lines, orders), database_last(before, key, last),
        forall|position: int| #![trigger lines[position]] 0 <= position < lines.len() ==> lines[position].0.l_orderkey != key,
    ensures database_last(after, key, last), positive_sum(before, key) == positive_sum(after, key),
{
    let positive = |row: LineitemRow| if row.l_orderkey == key {
        let value = row.l_extendedprice * (100 - row.l_discount);
        if value > 0 { value } else { 0int }
    } else { 0int };
    lemma_sum_zero(lines, |entry: (LineitemRow, int)| entry.1 * positive(entry.0));
    assert(sum(after.lineitem, positive) == sum(before.lineitem, positive) + sum(lines, |entry: (LineitemRow, int)| entry.1 * positive(entry.0)));
    match last {
        None => {
            verus_prooflib::lemma_bag_step_property(before.lineitem, after.lineitem, lines, |row: LineitemRow| row.l_orderkey != key);
        },
        Some(date) => {
            verus_prooflib::lemma_bag_step_property(before.lineitem, after.lineitem, lines, |row: LineitemRow| row.l_orderkey != key || row.l_shipdate <= date);
            let selected = |row: LineitemRow| if row.l_orderkey == key && row.l_shipdate == date { 1int } else { 0int };
            let position = choose|position: int| #![trigger before.lineitem[position]] 0 <= position < before.lineitem.len() && before.lineitem[position].l_orderkey == key && before.lineitem[position].l_shipdate == date;
            lemma_sum_pos(before.lineitem, selected, position);
            lemma_sum_zero(lines, |entry: (LineitemRow, int)| entry.1 * selected(entry.0));
            assert(sum(after.lineitem, selected) == sum(before.lineitem, selected) + sum(lines, |entry: (LineitemRow, int)| entry.1 * selected(entry.0)));
            let source = lemma_sum_nonzero_witness(after.lineitem, selected);
            assert(after.lineitem[source].l_orderkey == key && after.lineitem[source].l_shipdate == date);
        },
    }
}

pub proof fn lemma_weight_locality(before: Db, after: Db, customers: Seq<CustomerDelta>, lines: Seq<(LineitemRow, int)>, orders: Seq<(OrdersRow, int)>, segment: Seq<u8>, key: int, date: int, priority: int)
    requires database_domains(before), capture_ok(before, after, customer_delta_seq_view(customers), lines, orders),
        forall|position: int| #![trigger orders[position]] 0 <= position < orders.len() ==> orders[position].0.o_orderkey != key,
        forall|position: int| #![trigger customers[position]] 0 <= position < customers.len() ==> reverse_sum(before, customers[position].row.c_custkey as int, key) == 0,
    ensures weight_sum(before, segment, key, date, priority) == weight_sum(after, segment, key, date, priority),
{
    let after_weight = |row: OrdersRow| if row.o_orderkey == key && row.o_orderdate == date && row.o_shippriority == priority { customer_sum(after, row.o_custkey, segment) } else { 0int };
    let before_weight = |row: OrdersRow| if row.o_orderkey == key && row.o_orderdate == date && row.o_shippriority == priority { customer_sum(before, row.o_custkey, segment) } else { 0int };
    lemma_sum_zero(orders, |entry: (OrdersRow, int)| entry.1 * after_weight(entry.0));
    assert(sum(after.orders, after_weight) == sum(before.orders, after_weight) + sum(orders, |entry: (OrdersRow, int)| entry.1 * after_weight(entry.0)));
    assert forall|position: int| #![trigger before.orders[position]] 0 <= position < before.orders.len() implies after_weight(before.orders[position]) == before_weight(before.orders[position]) by {
        let row = before.orders[position];
        if row.o_orderkey == key && row.o_orderdate == date && row.o_shippriority == priority {
            let customer = row.o_custkey as i64;
            let reverse = |entry: OrdersRow| if entry.o_custkey == row.o_custkey && entry.o_orderkey == key { 1int } else { 0int };
            lemma_sum_pos(before.orders, reverse, position);
            assert(reverse_sum(before, row.o_custkey, key) > 0);
            assert forall|source: int| #![trigger customers[source]] 0 <= source < customers.len() implies customers[source].row.c_custkey != customer by {
                assert(reverse_sum(before, customers[source].row.c_custkey as int, key) == 0);
            }
            crate::input_proofs::lemma_customer_capture(before, after, customers, lines, orders, customer, segment);
            lemma_sum_zero(customers, customer_delta_weight(customer, segment));
        }
    }
    lemma_sum_ext(before.orders, after_weight, before_weight);
}

pub proof fn lemma_state_work(customers: Index<CustomerCell>, pages: Index<Page>, ranks: Index<Index<()>>, db: Db, number: int, work: PageWork)
    requires state_ok(customers, pages, ranks, db), work_page_shape(work, number),
        match pages.at((number, Seq::empty())) {
            Some(page) => work.orders@ == (*page.orders)@ && work.ships@ == (*page.ships)@,
            None => work.orders@.len() == 0 && work.ships@.len() == 0,
        },
    ensures work_database_ok(work, number, db),
{
    assert forall|key: int, customer: int, date: int, priority: int| #![trigger order_count(work.orders@, key, customer, date, priority)]
        order_count(work.orders@, key, customer, date, priority) == if key / 256 == number { order_sum(db, key, customer, date, priority) } else { 0int } by {
        if key / 256 == number { assert(order_at(pages, key, customer, date, priority) == order_sum(db, key, customer, date, priority)); }
        else { lemma_sum_zero(work.orders@, |row: OrderCell| if row.key == key && row.customer == customer && row.date == date && row.priority == priority { row.count as int } else { 0int }); }
    }
    assert forall|key: int, date: int, kind: int| #![trigger ship_count(work.ships@, key, date, kind)]
        0 <= kind <= 2 implies ship_count(work.ships@, key, date, kind) == if key / 256 == number { ship_sum(db, key, date, kind) } else { 0int } by {
        if key / 256 == number { assert(ship_at(pages, key, date, kind) == ship_sum(db, key, date, kind)); }
        else { lemma_sum_zero(work.ships@, |row: ShipCell| if row.key == key && row.date == date { if kind == 0 { row.count as int } else if kind == 1 { row.revenue as int } else { row.positive as int } } else { 0int }); }
    }
}

pub proof fn lemma_columns_frame(work: PageWork, number: int, before: Db, after: Db, customers: Seq<(CustomerRow, int)>, lines: Seq<(LineitemRow, int)>, orders: Seq<(OrdersRow, int)>)
    requires work_database_ok(work, number, before), capture_ok(before, after, customers, lines, orders),
        forall|position: int| #![trigger lines[position]] 0 <= position < lines.len() ==> lines[position].0.l_orderkey / 256 != number,
        forall|position: int| #![trigger orders[position]] 0 <= position < orders.len() ==> orders[position].0.o_orderkey / 256 != number,
    ensures work_database_ok(work, number, after),
{
    assert forall|key: int, customer: int, date: int, priority: int| #![trigger order_count(work.orders@, key, customer, date, priority)]
        order_count(work.orders@, key, customer, date, priority) == if key / 256 == number { order_sum(after, key, customer, date, priority) } else { 0int } by {
        if key / 256 == number { lemma_fact_locality(before, after, customers, lines, orders, key, customer, date, priority, 0); }
    }
    assert forall|key: int, date: int, kind: int| #![trigger ship_count(work.ships@, key, date, kind)]
        0 <= kind <= 2 implies ship_count(work.ships@, key, date, kind) == if key / 256 == number { ship_sum(after, key, date, kind) } else { 0int } by {
        if key / 256 == number { lemma_fact_locality(before, after, customers, lines, orders, key, 0, date, 0, kind); }
    }
}

pub proof fn lemma_state_page(customers: Index<CustomerCell>, pages: Index<Page>, ranks: Index<Index<()>>, db: Db, number: int, page: Page)
    requires state_ok(customers, pages, ranks, db), pages.at((number, Seq::empty())) == Some(page),
    ensures page_database_ok(page, number, db),
{
    assert forall|key: int, customer: int, date: int, priority: int| #![trigger order_count((*page.orders)@, key, customer, date, priority)]
        order_count((*page.orders)@, key, customer, date, priority) == if key / 256 == number { order_sum(db, key, customer, date, priority) } else { 0int } by {
        if key / 256 == number { assert(order_at(pages, key, customer, date, priority) == order_sum(db, key, customer, date, priority)); }
        else { lemma_sum_zero((*page.orders)@, |row: OrderCell| if row.key == key && row.customer == customer && row.date == date && row.priority == priority { row.count as int } else { 0int }); }
    }
    assert forall|key: int, date: int, kind: int| #![trigger ship_count((*page.ships)@, key, date, kind)]
        0 <= kind <= 2 implies ship_count((*page.ships)@, key, date, kind) == if key / 256 == number { ship_sum(db, key, date, kind) } else { 0int } by {
        if key / 256 == number { assert(ship_at(pages, key, date, kind) == ship_sum(db, key, date, kind)); }
        else { lemma_sum_zero((*page.ships)@, |row: ShipCell| if row.key == key && row.date == date { if kind == 0 { row.count as int } else if kind == 1 { row.revenue as int } else { row.positive as int } } else { 0int }); }
    }
    assert forall|segment: Seq<u8>, key: int, date: int, priority: int| #![trigger column_weight((*page.groups)@, segment, key, date, priority)]
        column_weight((*page.groups)@, segment, key, date, priority) == if key / 256 == number { weight_sum(db, segment, key, date, priority) } else { 0int } by {
        if key / 256 == number { assert(group_at(pages, segment, key, date, priority) == weight_sum(db, segment, key, date, priority)); }
        else { lemma_sum_zero((*page.groups)@, column_measure(segment, key, date, priority)); }
    }
}

pub proof fn lemma_page_frame(page: Page, number: int, before: Db, after: Db, customers: Seq<CustomerDelta>, lines: Seq<(LineitemRow, int)>, orders: Seq<(OrdersRow, int)>)
    requires database_domains(before), page_database_ok(page, number, before), capture_ok(before, after, customer_delta_seq_view(customers), lines, orders),
        forall|position: int| #![trigger lines[position]] 0 <= position < lines.len() ==> lines[position].0.l_orderkey / 256 != number,
        forall|position: int| #![trigger orders[position]] 0 <= position < orders.len() ==> orders[position].0.o_orderkey / 256 != number,
        forall|position: int, key: int| #![trigger customers[position], reverse_sum(before, customers[position].row.c_custkey as int, key)]
            0 <= position < customers.len() && key / 256 == number ==> reverse_sum(before, customers[position].row.c_custkey as int, key) == 0,
    ensures page_database_ok(page, number, after),
{
    assert forall|key: int, customer: int, date: int, priority: int| #![trigger order_count((*page.orders)@, key, customer, date, priority)]
        order_count((*page.orders)@, key, customer, date, priority) == if key / 256 == number { order_sum(after, key, customer, date, priority) } else { 0int } by {
        if key / 256 == number { lemma_fact_locality(before, after, customer_delta_seq_view(customers), lines, orders, key, customer, date, priority, 0); }
    }
    assert forall|key: int, date: int, kind: int| #![trigger ship_count((*page.ships)@, key, date, kind)]
        0 <= kind <= 2 implies ship_count((*page.ships)@, key, date, kind) == if key / 256 == number { ship_sum(after, key, date, kind) } else { 0int } by {
        if key / 256 == number { lemma_fact_locality(before, after, customer_delta_seq_view(customers), lines, orders, key, 0, date, 0, kind); }
    }
    assert forall|position: int| #![trigger (*page.groups)@[position]] 0 <= position < (*page.groups)@.len() implies {
        let group = (*page.groups)@[position];
        &&& group.bound == group.weight * positive_sum(after, group.id.key as int)
        &&& database_last(after, group.id.key as int, group.last_ship)
    } by {
        let group = (*page.groups)@[position];
        lemma_line_locality(before, after, customer_delta_seq_view(customers), lines, orders, group.id.key as int, group.last_ship);
    }
    assert forall|segment: Seq<u8>, key: int, date: int, priority: int| #![trigger column_weight((*page.groups)@, segment, key, date, priority)]
        column_weight((*page.groups)@, segment, key, date, priority) == if key / 256 == number { weight_sum(after, segment, key, date, priority) } else { 0int } by {
        if key / 256 == number { lemma_weight_locality(before, after, customers, lines, orders, segment, key, date, priority); }
    }
}

pub proof fn lemma_work_domains(work: PageWork, number: int, db: Db)
    requires database_domains(db), work_page_shape(work, number),
        forall|key: i64, customer: i64, date: i32, priority: i64| #![trigger order_count(work.orders@, key as int, customer as int, date as int, priority as int)]
            key as int / 256 == number ==> order_count(work.orders@, key as int, customer as int, date as int, priority as int)
                == order_sum(db, key as int, customer as int, date as int, priority as int),
        forall|key: i64, date: i32, kind: int| #![trigger ship_count(work.ships@, key as int, date as int, kind)]
            key as int / 256 == number && 0 <= kind <= 2 ==>
                ship_count(work.ships@, key as int, date as int, kind) == ship_sum(db, key as int, date as int, kind),
    ensures work_database_ok(work, number, db),
{
    assert forall|key: int, customer: int, date: int, priority: int| #![trigger order_count(work.orders@, key, customer, date, priority)]
        order_count(work.orders@, key, customer, date, priority) == if key / 256 == number { order_sum(db, key, customer, date, priority) } else { 0int } by {
        if key / 256 != number || key < i64::MIN || key > i64::MAX || customer < i64::MIN || customer > i64::MAX
            || date < i32::MIN || date > i32::MAX || priority < i64::MIN || priority > i64::MAX {
            lemma_sum_zero(work.orders@, |row: OrderCell| if row.key == key && row.customer == customer && row.date == date && row.priority == priority { row.count as int } else { 0int });
            if key / 256 == number {
                lemma_sum_zero(db.orders, |row: OrdersRow| if row.o_orderkey == key && row.o_custkey == customer && row.o_orderdate == date && row.o_shippriority == priority { 1int } else { 0int });
            }
        } else {
            assert(order_count(work.orders@, key as i64 as int, customer as i64 as int, date as i32 as int, priority as i64 as int)
                == order_sum(db, key, customer, date, priority));
        }
    }
    assert forall|key: int, date: int, kind: int| #![trigger ship_count(work.ships@, key, date, kind)]
        0 <= kind <= 2 implies ship_count(work.ships@, key, date, kind) == if key / 256 == number { ship_sum(db, key, date, kind) } else { 0int } by {
        if key / 256 != number || key < i64::MIN || key > i64::MAX || date < i32::MIN || date > i32::MAX {
            lemma_sum_zero(work.ships@, |row: ShipCell| if row.key == key && row.date == date {
                if kind == 0 { row.count as int } else if kind == 1 { row.revenue as int } else { row.positive as int }
            } else { 0int });
            if key / 256 == number {
                lemma_sum_zero(db.lineitem, |row: LineitemRow| if row.l_orderkey == key && row.l_shipdate == date {
                    let value = row.l_extendedprice * (100 - row.l_discount);
                    if kind == 0 { 1int } else if kind == 1 { value } else { if value > 0 { value } else { 0int } }
                } else { 0int });
            }
        } else { assert(ship_count(work.ships@, key as i64 as int, date as i32 as int, kind) == ship_sum(db, key, date, kind)); }
    }
}

pub proof fn lemma_ship_rescan(db: Db, keys: Seq<i64>, rows: Seq<ShipmentInput>, key: i64, date: i32, kind: int)
    requires rescan_shipments_by_key_ok(db, keys, rows), keys.contains(key),
    ensures ship_prefix(rows, key, date, kind) == ship_sum(db, key as int, date as int, kind),
{
    let weight = |row: LineitemRow| if row.l_orderkey == key as int && row.l_shipdate == date as int {
        let value = row.l_extendedprice * (100 - row.l_discount);
        if kind == 0 { 1int } else if kind == 1 { value } else { if value > 0 { value } else { 0int } }
    } else { 0int };
    let position = choose|position: int| #![trigger keys[position]] 0 <= position < keys.len() && keys[position] == key;
    assert(seq_view_i64(keys)[position] == key as int);
    assert(seq_view_i64(keys).contains(key as int));
    lemma_sum_map(rows, |row: ShipmentInput| shipment_input_view(row), weight,
        |row: ShipmentInput| if row.l_orderkey == key && row.l_shipdate == date {
            let value = row.l_extendedprice as int * (100 - row.l_discount as int);
            if kind == 0 { 1int } else if kind == 1 { value } else { if value > 0 { value } else { 0int } }
        } else { 0int });
    assert(sum(shipment_input_seq_view(rows), weight) == sum(db.lineitem,
        |row: LineitemRow| if seq_view_i64(keys).contains(row.l_orderkey) { weight(row) } else { 0int }));
    lemma_sum_ext_all(db.lineitem,
        |row: LineitemRow| if seq_view_i64(keys).contains(row.l_orderkey) { weight(row) } else { 0int }, weight);
}

pub proof fn lemma_fact_locality(before: Db, after: Db, customers: Seq<(CustomerRow, int)>,
    lines: Seq<(LineitemRow, int)>, orders: Seq<(OrdersRow, int)>, key: int, customer: int, date: int, priority: int, kind: int)
    requires capture_ok(before, after, customers, lines, orders),
        forall|position: int| #![trigger lines[position]] 0 <= position < lines.len() ==> lines[position].0.l_orderkey != key,
        forall|position: int| #![trigger orders[position]] 0 <= position < orders.len() ==> orders[position].0.o_orderkey != key,
    ensures order_sum(after, key, customer, date, priority) == order_sum(before, key, customer, date, priority),
        reverse_sum(after, customer, key) == reverse_sum(before, customer, key),
        ship_sum(after, key, date, kind) == ship_sum(before, key, date, kind),
{
    let selected = |row: OrdersRow| if row.o_orderkey == key && row.o_custkey == customer && row.o_orderdate == date && row.o_shippriority == priority { 1int } else { 0int };
    let reverse = |row: OrdersRow| if row.o_custkey == customer && row.o_orderkey == key { 1int } else { 0int };
    let shipment = |row: LineitemRow| if row.l_orderkey == key && row.l_shipdate == date {
        let value = row.l_extendedprice * (100 - row.l_discount);
        if kind == 0 { 1int } else if kind == 1 { value } else { if value > 0 { value } else { 0int } }
    } else { 0int };
    lemma_sum_zero(orders, |entry: (OrdersRow, int)| entry.1 * selected(entry.0));
    lemma_sum_zero(orders, |entry: (OrdersRow, int)| entry.1 * reverse(entry.0));
    lemma_sum_zero(lines, |entry: (LineitemRow, int)| entry.1 * shipment(entry.0));
    assert(sum(after.orders, selected) == sum(before.orders, selected) + sum(orders, |entry: (OrdersRow, int)| entry.1 * selected(entry.0)));
    assert(sum(after.orders, reverse) == sum(before.orders, reverse) + sum(orders, |entry: (OrdersRow, int)| entry.1 * reverse(entry.0)));
    assert(sum(after.lineitem, shipment) == sum(before.lineitem, shipment) + sum(lines, |entry: (LineitemRow, int)| entry.1 * shipment(entry.0)));
}

pub proof fn lemma_transaction_domains(before: Db, after: Db, customers: Seq<CustomerDelta>,
    lines: Seq<(LineitemRow, int)>, orders: Seq<(OrdersRow, int)>, facts: Set<i64>,
    order_keys: Seq<i64>, order_rows: Seq<OrderInput>, ship_keys: Seq<i64>, ship_rows: Seq<ShipmentInput>)
    requires database_domains(before), capture_ok(before, after, customer_delta_seq_view(customers), lines, orders),
        rescan_orders_by_key_ok(after, order_keys, order_rows), rescan_shipments_by_key_ok(after, ship_keys, ship_rows),
        facts.subset_of(order_keys.to_set()), facts.subset_of(ship_keys.to_set()),
        forall|position: int| #![trigger orders[position]] 0 <= position < orders.len() ==>
            i64::MIN <= orders[position].0.o_orderkey <= i64::MAX && facts.contains(orders[position].0.o_orderkey as i64),
        forall|position: int| #![trigger lines[position]] 0 <= position < lines.len() ==>
            i64::MIN <= lines[position].0.l_orderkey <= i64::MAX && facts.contains(lines[position].0.l_orderkey as i64),
    ensures database_domains(after),
{
    verus_prooflib::lemma_bag_step_property(before.customer, after.customer, customer_delta_seq_view(customers),
        |row: CustomerRow| i64::MIN <= row.c_custkey <= i64::MAX);
    let order_bad = |row: OrdersRow| if !order_domain(row) { 1int } else { 0int };
    lemma_sum_zero(order_input_seq_view(order_rows), order_bad);
    let scoped_order_bad = |row: OrdersRow| if seq_view_i64(order_keys).contains(row.o_orderkey) { order_bad(row) } else { 0int };
    assert(sum(after.orders, scoped_order_bad) == 0);
    let ship_bad = |row: LineitemRow| if !shipment_domain(row) { 1int } else { 0int };
    lemma_sum_zero(shipment_input_seq_view(ship_rows), ship_bad);
    let scoped_ship_bad = |row: LineitemRow| if seq_view_i64(ship_keys).contains(row.l_orderkey) { ship_bad(row) } else { 0int };
    assert(sum(after.lineitem, scoped_ship_bad) == 0);
    verus_prooflib::lemma_bag_step_property(before.orders, after.orders, orders,
        |row: OrdersRow| i64::MIN <= row.o_orderkey <= i64::MAX && (facts.contains(row.o_orderkey as i64) || order_domain(row)));
    verus_prooflib::lemma_bag_step_property(before.lineitem, after.lineitem, lines,
        |row: LineitemRow| i64::MIN <= row.l_orderkey <= i64::MAX && (facts.contains(row.l_orderkey as i64) || shipment_domain(row)));
    assert forall|position: int| #![trigger after.orders[position]] 0 <= position < after.orders.len() implies order_domain(after.orders[position]) by {
        let row = after.orders[position];
        if facts.contains(row.o_orderkey as i64) {
            assert(order_keys.contains(row.o_orderkey as i64));
            let source = choose|source: int| #![trigger order_keys[source]] 0 <= source < order_keys.len() && order_keys[source] == row.o_orderkey;
            assert(seq_view_i64(order_keys)[source] == row.o_orderkey);
            assert(seq_view_i64(order_keys).contains(row.o_orderkey));
            if !order_domain(row) { lemma_sum_pos(after.orders, scoped_order_bad, position); }
        }
    }
    assert forall|position: int| #![trigger after.lineitem[position]] 0 <= position < after.lineitem.len() implies shipment_domain(after.lineitem[position]) by {
        let row = after.lineitem[position];
        if facts.contains(row.l_orderkey as i64) {
            assert(ship_keys.contains(row.l_orderkey as i64));
            let source = choose|source: int| #![trigger ship_keys[source]] 0 <= source < ship_keys.len() && ship_keys[source] == row.l_orderkey;
            assert(seq_view_i64(ship_keys)[source] == row.l_orderkey);
            assert(seq_view_i64(ship_keys).contains(row.l_orderkey));
            if !shipment_domain(row) { lemma_sum_pos(after.lineitem, scoped_ship_bad, position); }
        }
    }
}

}

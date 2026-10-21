use vstd::prelude::*;
use crate::custs::Custs;
use vstd::hash_map::HashMapWithView;
use std::sync::Arc;
use verus_prooflib::ordered::{Index, Key};
use crate::gen_obligations::*;
use crate::gen_spec::*;
use crate::kernel::State;
use crate::columns::*;
use crate::model::*;
use crate::values::*;
use crate::ranks;
use verus_common::sums::*;

verus! {

pub proof fn lemma_page_groups_put(before: Index<Page>, after: Index<Page>, prior: Seq<GroupCell>, page: Page, key: i64)
    requires groups_in_pages(before, prior), before.at((key as int, Seq::empty())).is_none(),
        forall|target: verus_prooflib::ordered::KeyView| #![trigger after.at(target)]
            after.at(target) == if target == (key as int, Seq::<u8>::empty()) { Some(page) } else { before.at(target) },
    ensures groups_in_pages(after, prior + (*page.groups)@),
{
    let added = (*page.groups)@;
    let combined = prior + added;
    assert forall|group: GroupCell| #![trigger combined.contains(group)] combined.contains(group) ==
        (exists|page_key: verus_prooflib::ordered::KeyView, position: int| #![trigger after.at(page_key), (*after.at(page_key).unwrap().groups)@[position]]
            after.at(page_key).is_some() && 0 <= position < (*after.at(page_key).unwrap().groups)@.len()
                && (*after.at(page_key).unwrap().groups)@[position] == group) by {
        if combined.contains(group) {
            let source = choose|source: int| #![trigger combined[source]] 0 <= source < combined.len() && combined[source] == group;
            if source < prior.len() {
                assert(prior.contains(group));
                let (page_key, position) = choose|page_key: verus_prooflib::ordered::KeyView, position: int|
                    #![trigger before.at(page_key), (*before.at(page_key).unwrap().groups)@[position]]
                    before.at(page_key).is_some() && 0 <= position < (*before.at(page_key).unwrap().groups)@.len()
                        && (*before.at(page_key).unwrap().groups)@[position] == group;
                assert(after.at(page_key) == before.at(page_key));
                assert(exists|target: verus_prooflib::ordered::KeyView, offset: int| #![trigger after.at(target), (*after.at(target).unwrap().groups)@[offset]]
                    after.at(target).is_some() && 0 <= offset < (*after.at(target).unwrap().groups)@.len()
                        && (*after.at(target).unwrap().groups)@[offset] == group);
            } else {
                assert(added[source - prior.len()] == group);
                assert(after.at((key as int, Seq::empty())) == Some(page));
                assert((*after.at((key as int, Seq::empty())).unwrap().groups)@[source - prior.len()] == group);
                assert(exists|target: verus_prooflib::ordered::KeyView, offset: int| #![trigger after.at(target), (*after.at(target).unwrap().groups)@[offset]]
                    after.at(target).is_some() && 0 <= offset < (*after.at(target).unwrap().groups)@.len()
                        && (*after.at(target).unwrap().groups)@[offset] == group);
            }
        }
        if exists|page_key: verus_prooflib::ordered::KeyView, position: int| #![trigger after.at(page_key), (*after.at(page_key).unwrap().groups)@[position]]
            after.at(page_key).is_some() && 0 <= position < (*after.at(page_key).unwrap().groups)@.len()
                && (*after.at(page_key).unwrap().groups)@[position] == group {
            let (page_key, position) = choose|page_key: verus_prooflib::ordered::KeyView, position: int|
                #![trigger after.at(page_key), (*after.at(page_key).unwrap().groups)@[position]]
                after.at(page_key).is_some() && 0 <= position < (*after.at(page_key).unwrap().groups)@.len()
                    && (*after.at(page_key).unwrap().groups)@[position] == group;
            if page_key == (key as int, Seq::<u8>::empty()) {
                assert(combined[prior.len() + position] == group);
                assert(combined.contains(group));
            } else {
                assert(prior.contains(group));
                let source = choose|source: int| #![trigger prior[source]] 0 <= source < prior.len() && prior[source] == group;
                assert(combined[source] == group);
                assert(combined.contains(group));
            }
        }
    }
}

pub fn publish_customers(mut cells: HashMapWithView<i64, CustomerCell>, keys: &Vec<i64>) -> (result: Option<Custs>)
    requires
        keys@.no_duplicates(), keys@.to_set() == cells@.dom(),
        forall|key: i64| #![trigger cells@[key]] cells@.contains_key(key) ==> customer_shape(cells@[key]),
    ensures result matches Some(index) ==> {
        &&& index.wf()
        &&& forall|target: verus_prooflib::ordered::KeyView| #![trigger index.at(target)]
            index.at(target) == if target.1 == Seq::<u8>::empty() && i64::MIN <= target.0 <= i64::MAX
                && cells@.contains_key(target.0 as i64) { Some(cells@[target.0 as i64]) } else { None }
        &&& forall|customer: i64, segment: Seq<u8>| #![trigger customer_at(index, customer as int, segment)]
            customer_at(index, customer as int, segment) == scratch_customer_at(cells@, customer, segment)
        &&& forall|customer: i64, key: i64| #![trigger reverse_at(index, customer as int, key as int)]
            reverse_at(index, customer as int, key as int) == scratch_reverse_at(cells@, customer, key)
        &&& forall|target: verus_prooflib::ordered::KeyView| #![trigger index.at(target)]
            index.at(target).is_some() ==> customer_shape(index.at(target).unwrap())
    },
{
    let ghost original = cells@;
    let mut result = Custs::new();
    let mut position = 0;
    while position < keys.len()
        invariant position <= keys.len(), result.wf(), keys@.no_duplicates(), keys@.to_set() == original.dom(),
            forall|key: i64| #![trigger original[key]] original.contains_key(key) ==> customer_shape(original[key]),
            forall|key: i64| #![trigger cells@.contains_key(key)]
                cells@.contains_key(key) == (original.contains_key(key) && !keys@.take(position as int).contains(key)),
            forall|key: i64| #![trigger cells@[key]] cells@.contains_key(key) ==> cells@[key] == original[key],
            forall|target: verus_prooflib::ordered::KeyView| #![trigger result.at(target)]
                result.at(target) == if target.1 == Seq::<u8>::empty() && i64::MIN <= target.0 <= i64::MAX
                    && keys@.take(position as int).contains(target.0 as i64) { Some(original[target.0 as i64]) } else { None },
        decreases keys.len() - position,
    {
        let key = keys[position];
        let ghost before = cells@;
        proof { verus_prooflib::lemma_prefix_fresh(keys@, position as int); }
        let cell = match cells.remove(&key) { Some(cell) => cell, None => return None };
        result.put(key, Arc::new(cell));
        proof {
            assert(keys@.take(position as int + 1) =~= keys@.take(position as int).push(key));
            verus_prooflib::lemma_push_membership(keys@.take(position as int), key);
            assert forall|target: i64| #![trigger cells@.contains_key(target)]
                cells@.contains_key(target) == (original.contains_key(target) && !keys@.take(position as int + 1).contains(target)) by {
                assert(before.contains_key(target) == (original.contains_key(target) && !keys@.take(position as int).contains(target)));
                assert(keys@.take(position as int + 1).contains(target)
                    == (keys@.take(position as int).contains(target) || target == key));
            }
        }
        position += 1;
    }
    proof { assert(keys@.take(position as int) =~= keys@); }
    Some(result)
}

pub fn make_groups(work: &PageWork, customers: &Custs) -> (result: Option<Vec<GroupCell>>)
    requires customers.wf(), orders_sorted(work.orders@), ships_sorted(work.ships@),
        forall|position: int| #![trigger work.orders@[position]] 0 <= position < work.orders@.len() ==> work.orders@[position].count > 0,
        forall|position: int| #![trigger work.ships@[position]] 0 <= position < work.ships@.len() ==> ship_ok(work.ships@[position]),
        forall|target: verus_prooflib::ordered::KeyView| #![trigger customers.at(target)]
            customers.at(target).is_some() ==> customer_shape(customers.at(target).unwrap()),
    ensures result matches Some(groups) ==> {
        &&& groups_sorted(groups@)
        &&& forall|position: int| #![trigger groups@[position]] 0 <= position < groups@.len() ==> {
            let group = groups@[position];
            &&& group.weight > 0 && group.bound >= 0 && group_ship_ok(group, work.ships@)
            &&& exists|source: int| #![trigger work.orders@[source]]
                0 <= source < work.orders@.len() && work.orders@[source].key == group.id.key
        }
        &&& forall|target: Seq<u8>, key: int, date: int, priority: int| #![trigger column_weight(groups@, target, key, date, priority)]
            column_weight(groups@, target, key, date, priority) == column_join(work.orders@, *customers, target, key, date, priority)
    },
{
    let mut groups = Vec::new();
    let mut order_index = 0;
    proof {
        assert forall|target: Seq<u8>, key: int, date: int, priority: int| #![trigger column_join(work.orders@, *customers, target, key, date, priority)]
            key < next_order_key(work.orders@, 0) implies column_join(work.orders@, *customers, target, key, date, priority) == 0 by {
            assert forall|position: int| #![trigger work.orders@[position]] 0 <= position < work.orders@.len() implies work.orders@[position].key != key by {
                if position > 0 { assert(order_lt(work.orders@[0], work.orders@[position])); }
            }
            lemma_sum_zero(work.orders@, order_group_measure(*customers, target, key, date, priority));
        }
    }
    while order_index < work.orders.len()
        invariant order_index <= work.orders.len(), customers.wf(), orders_sorted(work.orders@), ships_sorted(work.ships@),
            groups_sorted(groups@),
            forall|position: int| #![trigger work.orders@[position]] 0 <= position < work.orders@.len() ==> work.orders@[position].count > 0,
            forall|position: int| #![trigger work.ships@[position]] 0 <= position < work.ships@.len() ==> ship_ok(work.ships@[position]),
            forall|target: verus_prooflib::ordered::KeyView| #![trigger customers.at(target)] customers.at(target).is_some() ==> customer_shape(customers.at(target).unwrap()),
            forall|position: int| #![trigger groups@[position]] 0 <= position < groups@.len() ==> {
                let group = groups@[position];
                &&& group.weight > 0 && group.bound >= 0 && group_ship_ok(group, work.ships@)
                &&& group.id.key < next_order_key(work.orders@, order_index as int)
                &&& exists|source: int| #![trigger work.orders@[source]]
                    0 <= source < work.orders@.len() && work.orders@[source].key == group.id.key
            },
            forall|target: Seq<u8>, key: int, date: int, priority: int| #![trigger column_weight(groups@, target, key, date, priority)]
                column_weight(groups@, target, key, date, priority) == if key < next_order_key(work.orders@, order_index as int) {
                    column_join(work.orders@, *customers, target, key, date, priority)
                } else { 0int },
            forall|position: int| #![trigger work.orders@[position]] 0 <= position < order_index ==>
                work.orders@[position].key < next_order_key(work.orders@, order_index as int),
        decreases work.orders.len() - order_index,
    {
        let ghost previous_index = order_index as int;
        let ghost previous_groups = groups@;
        let orderkey = work.orders[order_index].key;
        if !compute_key(&mut groups, &work.orders, &work.ships, customers, orderkey) { return None; }
        proof {
            assert forall|position: int| #![trigger groups@[position]] 0 <= position < groups@.len() implies {
                let group = groups@[position];
                &&& group.weight > 0 && group.bound >= 0 && group_ship_ok(group, work.ships@)
                &&& group.id.key <= orderkey
                &&& exists|source: int| #![trigger work.orders@[source]]
                    0 <= source < work.orders@.len() && work.orders@[source].key == group.id.key
            } by {
                if previous_groups.contains(groups@[position]) {
                    let prior = choose|prior: int| #![trigger previous_groups[prior]]
                        0 <= prior < previous_groups.len() && previous_groups[prior] == groups@[position];
                } else { assert(work.orders@[previous_index].key == groups@[position].id.key); }
            }
            assert forall|target: Seq<u8>, key: int, date: int, priority: int| #![trigger column_weight(groups@, target, key, date, priority)]
                column_weight(groups@, target, key, date, priority) == if key <= orderkey {
                    column_join(work.orders@, *customers, target, key, date, priority)
                } else { 0int } by {
                assert(column_weight(previous_groups, target, key, date, priority) == if key < orderkey {
                    column_join(work.orders@, *customers, target, key, date, priority)
                } else { 0int });
            }
        }
        order_index += 1;
        while order_index < work.orders.len() && work.orders[order_index].key == orderkey
            invariant previous_index < order_index <= work.orders.len(), orders_sorted(work.orders@),
                groups_sorted(groups@),
                forall|position: int| #![trigger groups@[position]] 0 <= position < groups@.len() ==> {
                    let group = groups@[position];
                    &&& group.weight > 0 && group.bound >= 0 && group_ship_ok(group, work.ships@)
                    &&& group.id.key <= orderkey
                    &&& exists|source: int| #![trigger work.orders@[source]]
                        0 <= source < work.orders@.len() && work.orders@[source].key == group.id.key
                },
                forall|target: Seq<u8>, key: int, date: int, priority: int| #![trigger column_weight(groups@, target, key, date, priority)]
                    column_weight(groups@, target, key, date, priority) == if key <= orderkey {
                        column_join(work.orders@, *customers, target, key, date, priority)
                    } else { 0int },
                forall|position: int| #![trigger work.orders@[position]] 0 <= position < order_index ==> work.orders@[position].key <= orderkey,
                forall|position: int| #![trigger work.orders@[position]] order_index <= position < work.orders@.len() ==> work.orders@[position].key >= orderkey,
            decreases work.orders.len() - order_index,
        {
            order_index += 1;
        }
        proof {
            assert forall|target: Seq<u8>, key: int, date: int, priority: int| #![trigger column_weight(groups@, target, key, date, priority)]
                column_weight(groups@, target, key, date, priority) == if key < next_order_key(work.orders@, order_index as int) {
                    column_join(work.orders@, *customers, target, key, date, priority)
                } else { 0int } by {
                if orderkey < key < next_order_key(work.orders@, order_index as int) {
                    assert forall|position: int| #![trigger work.orders@[position]] 0 <= position < work.orders@.len() implies work.orders@[position].key != key by {
                        if position >= order_index && order_index < work.orders.len() && position > order_index {
                            assert(order_lt(work.orders@[order_index as int], work.orders@[position]));
                        }
                    }
                    lemma_sum_zero(work.orders@, order_group_measure(*customers, target, key, date, priority));
                }
            }
        }
    }
    proof {
        assert forall|target: Seq<u8>, key: int, date: int, priority: int| #![trigger column_weight(groups@, target, key, date, priority)]
            column_weight(groups@, target, key, date, priority) == column_join(work.orders@, *customers, target, key, date, priority) by {
            if key > i64::MAX {
                lemma_sum_zero(work.orders@, order_group_measure(*customers, target, key, date, priority));
            }
        }
    }
    Some(groups)
}

pub open spec fn chunk_of(page: i64) -> int { ((page as u64) % 16) as int }

pub open spec fn chunk_done(key: int, chunk: int) -> bool {
    i64::MIN <= key <= i64::MAX && chunk_of((key / 256) as i64) < chunk
}

pub open spec fn in_chunk(key: i64, chunk: int) -> bool { chunk_of(((key as int) / 256) as i64) == chunk }

pub fn chunk_exec(page: i64) -> (result: i64)
    ensures result == chunk_of(page), 0 <= result < 16,
{
    ((page as u64) % 16) as i64
}

pub fn construct(customers: Vec<CustomerInput>, orders: &[OrderInput], shipments: &[ShipmentInput], Ghost(db): Ghost<Db>) -> (result: Option<State>)
    requires scan_customers_ok(db, customers@), scan_orders_ok(db, orders@), scan_shipments_ok(db, shipments@),
    ensures result matches Some(state) ==> state.db() == db,
{
    let mut customer_work: HashMapWithView<i64, CustomerCell> = HashMapWithView::new();
    let mut customer_keys = Vec::new();
    let mut names: Vec<Arc<Vec<u8>>> = Vec::new();
    let mut index: usize = 0;
    while index < customers.len()
        invariant
            index <= customers.len(),
            customer_keys@.no_duplicates(), customer_keys@.to_set() == customer_work@.dom(),
            forall|key: i64| #![trigger customer_work@[key]] customer_work@.dom().contains(key) ==>
                customer_shape(customer_work@[key]) && customer_work@[key].orders@.len() == 0,
            forall|customer: i64, segment: Seq<u8>| #![trigger scratch_customer_at(customer_work@, customer, segment)]
                scratch_customer_at(customer_work@, customer, segment) == customer_prefix(customers@.take(index as int), customer, segment),
        decreases customers.len() - index,
    {
        let ghost processed = index as int;
        let ghost before = customer_work@;
        let row = &customers[index];
        let mut cell = match customer_work.remove(&row.c_custkey) {
            Some(cell) => cell,
            None => {
                proof {
                    verus_prooflib::lemma_unique_push(customer_keys@, row.c_custkey);
                    verus_prooflib::lemma_push_to_set(customer_keys@, row.c_custkey);
                }
                customer_keys.push(row.c_custkey); empty_customer()
            },
        };
        let ghost previous = cell;
        proof {
            assert forall|segment: Seq<u8>| #![trigger segment_count(previous.segments@, segment)]
                segment_count(previous.segments@, segment) == scratch_customer_at(before, row.c_custkey, segment) by {}
        }
        let name = intern(&mut names, &row.c_mktsegment);
        if !segment_change(&mut cell, &name, 1i128) { return None; }
        customer_work.insert(row.c_custkey, cell);
        proof {
            assert(customer_work@ == before.insert(row.c_custkey, cell));
            assert forall|customer: i64, segment: Seq<u8>| #![trigger scratch_customer_at(customer_work@, customer, segment)]
                scratch_customer_at(customer_work@, customer, segment) == customer_prefix(customers@.take(processed + 1), customer, segment) by {
                let weight = |entry: CustomerInput| if entry.c_custkey == customer && entry.c_mktsegment@ == segment { 1int } else { 0int };
                lemma_sum_take(customers@, processed, weight);
                assert(scratch_customer_at(before, customer, segment) == customer_prefix(customers@.take(processed), customer, segment));
                assert(customer_prefix(customers@.take(processed + 1), customer, segment) == customer_prefix(customers@.take(processed), customer, segment) + weight(*row));
                if customer == row.c_custkey {
                    assert(segment_count(cell.segments@, segment) == segment_count(previous.segments@, segment)
                        + if row.c_mktsegment@ == segment { 1int } else { 0int });
                }
            }
        }
        index += 1;
    }
    proof { assert(customers@.take(index as int) =~= customers@); }
    index = 0;
    while index < orders.len()
        invariant
            index <= orders.len(),
            customer_keys@.no_duplicates(), customer_keys@.to_set() == customer_work@.dom(),
            forall|key: i64| #![trigger customer_work@[key]] customer_work@.dom().contains(key) ==> customer_shape(customer_work@[key]),
            forall|customer: i64, segment: Seq<u8>| #![trigger scratch_customer_at(customer_work@, customer, segment)]
                scratch_customer_at(customer_work@, customer, segment) == customer_prefix(customers@, customer, segment),
            forall|customer: i64, key: i64| #![trigger scratch_reverse_at(customer_work@, customer, key)]
                scratch_reverse_at(customer_work@, customer, key) == reverse_prefix(orders@.take(index as int), customer, key),
        decreases orders.len() - index,
    {
        let ghost processed = index as int;
        let ghost previous_customers = customer_work@;
        let row = orders[index];
        let mut customer = match customer_work.remove(&row.o_custkey) {
            Some(cell) => cell,
            None => {
                proof {
                    verus_prooflib::lemma_unique_push(customer_keys@, row.o_custkey);
                    verus_prooflib::lemma_push_to_set(customer_keys@, row.o_custkey);
                }
                customer_keys.push(row.o_custkey); empty_customer()
            },
        };
        let ghost previous_customer = customer;
        if !reverse_change(&mut customer, row.o_orderkey, 1) { return None; }
        customer_work.insert(row.o_custkey, customer);
        proof {
            assert(customer_work@ == previous_customers.insert(row.o_custkey, customer));
            assert forall|target_customer: i64, segment: Seq<u8>| #![trigger scratch_customer_at(customer_work@, target_customer, segment)]
                scratch_customer_at(customer_work@, target_customer, segment) == scratch_customer_at(previous_customers, target_customer, segment) by {}
            assert forall|target_customer: i64, key: i64| #![trigger scratch_reverse_at(customer_work@, target_customer, key)]
                scratch_reverse_at(customer_work@, target_customer, key) == reverse_prefix(orders@.take(processed + 1), target_customer, key) by {
                let weight = |entry: OrderInput| if entry.o_custkey == target_customer && entry.o_orderkey == key { 1int } else { 0int };
                lemma_sum_take(orders@, processed, weight);
                assert(scratch_reverse_at(previous_customers, target_customer, key) == reverse_prefix(orders@.take(processed), target_customer, key));
                assert(reverse_prefix(orders@.take(processed + 1), target_customer, key) == reverse_prefix(orders@.take(processed), target_customer, key) + weight(row));
                if target_customer == row.o_custkey {
                    assert(reverse_count(previous_customer.orders@, key as int) == scratch_reverse_at(previous_customers, target_customer, key));
                    assert(reverse_count(customer.orders@, key as int) == reverse_count(previous_customer.orders@, key as int) + if key == row.o_orderkey { 1int } else { 0int });
                }
            }
        }
        index += 1;
    }
    proof { assert(orders@.take(index as int) =~= orders@); }

    let customer_index = match publish_customers(customer_work, &customer_keys) { Some(index) => index, None => return None };
    proof {
        crate::input_proofs::lemma_scan_domains(db, customers@, orders@, shipments@);
        assert forall|customer: i64, segment: Seq<u8>| #![trigger customer_at(customer_index, customer as int, segment)]
            customer_at(customer_index, customer as int, segment) == customer_sum(db, customer as int, segment) by {
            crate::input_proofs::lemma_customer_scan(db, customers@, customer, segment);
        }
        assert forall|customer: i64, key: i64| #![trigger reverse_at(customer_index, customer as int, key as int)]
            reverse_at(customer_index, customer as int, key as int) == reverse_sum(db, customer as int, key as int) by {
            crate::input_proofs::lemma_order_scan(db, orders@, customer, key, 0, 0);
        }
        assert forall|customer: int, segment: Seq<u8>| #![trigger customer_at(customer_index, customer, segment)]
            customer_at(customer_index, customer, segment) == customer_sum(db, customer, segment) by {
            if i64::MIN <= customer <= i64::MAX {
                assert(customer == (customer as i64) as int);
                assert(customer_at(customer_index, customer, segment) == customer_sum(db, (customer as i64) as int, segment));
            } else {
                lemma_sum_zero(db.customer, |row: CustomerRow| if row.c_custkey == customer && row.c_mktsegment == segment { 1int } else { 0int });
            }
        }
        assert forall|customer: int, key: int| #![trigger reverse_at(customer_index, customer, key)]
            reverse_at(customer_index, customer, key) == reverse_sum(db, customer, key) by {
            if i64::MIN <= customer <= i64::MAX && i64::MIN <= key <= i64::MAX {
                assert(customer == (customer as i64) as int && key == (key as i64) as int);
                assert(reverse_at(customer_index, customer, key) == reverse_sum(db, (customer as i64) as int, (key as i64) as int));
            } else {
                lemma_sum_zero(db.orders, |row: OrdersRow| if row.o_custkey == customer && row.o_orderkey == key { 1int } else { 0int });
                if customer_index.at((customer, Seq::empty())).is_some() {
                    lemma_sum_zero(customer_index.at((customer, Seq::empty())).unwrap().orders@,
                        |entry: (i64, i64)| if entry.0 == key { entry.1 as int } else { 0int });
                }
            }
        }
        assert(customer_database_ok(customer_index, db));
    }
    let mut page_index = Index::new();
    let mut rank_index = Index::new();
    let ghost published_groups = Seq::<GroupCell>::empty();
    let mut chunk: i64 = 0;
    while chunk < 16
        invariant 0 <= chunk <= 16,
            scan_customers_ok(db, customers@), scan_orders_ok(db, orders@), scan_shipments_ok(db, shipments@),
            customer_database_ok(customer_index, db),
            forall|target: verus_prooflib::ordered::KeyView| #![trigger customer_index.at(target)] customer_index.at(target).is_some() ==> customer_shape(customer_index.at(target).unwrap()),
            page_index.wf(), ranks_wf(rank_index),
            groups_in_pages(page_index, published_groups),
            forall|outer: verus_prooflib::ordered::KeyView, rank: verus_prooflib::ordered::KeyView| #![trigger rank_at(rank_index, outer, rank)]
                rank_at(rank_index, outer, rank) == rank_present(published_groups, outer, rank),
            forall|target: verus_prooflib::ordered::KeyView| #![trigger page_index.at(target)] page_index.at(target).is_some() ==>
                (target.1.len() == 0 && i64::MIN <= target.0 <= i64::MAX && chunk_of(target.0 as i64) < chunk as int
                    && page_database_ok(page_index.at(target).unwrap(), target.0, db)),
            forall|key: int, customer: int, date: int, priority: int| #![trigger order_at(page_index, key, customer, date, priority)]
                chunk_done(key, chunk as int) ==> order_at(page_index, key, customer, date, priority) == order_sum(db, key, customer, date, priority),
            forall|key: int, date: int, kind: int| #![trigger ship_at(page_index, key, date, kind)]
                chunk_done(key, chunk as int) && 0 <= kind <= 2 ==> ship_at(page_index, key, date, kind) == ship_sum(db, key, date, kind),
            forall|segment: Seq<u8>, key: int, date: int, priority: int| #![trigger group_at(page_index, segment, key, date, priority)]
                chunk_done(key, chunk as int) ==> group_at(page_index, segment, key, date, priority) == weight_sum(db, segment, key, date, priority),
        decreases 16 - chunk,
    {
        let ghost base_index = page_index;
        proof { assert(base_index == page_index); }
        let mut pages_work: HashMapWithView<i64, PageWork> = HashMapWithView::new();
        let mut page_keys: Vec<i64> = Vec::new();
        index = 0;
        while index < orders.len()
            invariant
                index <= orders.len(), 0 <= chunk < 16,
                scan_customers_ok(db, customers@), scan_orders_ok(db, orders@), scan_shipments_ok(db, shipments@),
                page_keys@.no_duplicates(), page_keys@.to_set() == pages_work@.dom(),
                forall|key: i64| #![trigger pages_work@.dom().contains(key)] pages_work@.dom().contains(key) ==>
                    work_page_shape(pages_work@[key], key as int) && pages_work@[key].ships@.len() == 0 && chunk_of(key) == chunk as int,
                forall|key: i64, customer: i64, date: i32, priority: i64| #![trigger scratch_order_at(pages_work@, key, customer, date, priority)]
                    in_chunk(key, chunk as int) ==> scratch_order_at(pages_work@, key, customer, date, priority) == order_prefix(orders@.take(index as int), key, customer, date, priority),
            decreases orders.len() - index,
        {
            let ghost processed = index as int;
            let ghost previous_pages = pages_work@;
            let row = orders[index];
            let page = page_id(row.o_orderkey);
            if chunk_exec(page) == chunk {
                let mut work = match pages_work.remove(&page) {
                    Some(work) => work,
                    None => {
                        proof {
                            verus_prooflib::lemma_unique_push(page_keys@, page);
                            verus_prooflib::lemma_push_to_set(page_keys@, page);
                        }
                        page_keys.push(page); empty_page()
                    },
                };
                let ghost previous_work = work;
                if !insert_order(&mut work.orders, row) { return None; }
                pages_work.insert(page, work);
                proof {
                    assert(pages_work@ == previous_pages.insert(page, work));
                    assert forall|key: i64, customer: i64, date: i32, priority: i64| #![trigger scratch_order_at(pages_work@, key, customer, date, priority)]
                        in_chunk(key, chunk as int) implies scratch_order_at(pages_work@, key, customer, date, priority) == order_prefix(orders@.take(processed + 1), key, customer, date, priority) by {
                        let weight = |entry: OrderInput| if entry.o_orderkey == key && entry.o_custkey == customer && entry.o_orderdate == date && entry.o_shippriority == priority { 1int } else { 0int };
                        lemma_sum_take(orders@, processed, weight);
                        assert(scratch_order_at(previous_pages, key, customer, date, priority) == order_prefix(orders@.take(processed), key, customer, date, priority));
                        assert(order_prefix(orders@.take(processed + 1), key, customer, date, priority) == order_prefix(orders@.take(processed), key, customer, date, priority) + weight(row));
                        if (key as int / 256) as i64 == page {
                            assert(order_count(previous_work.orders@, key as int, customer as int, date as int, priority as int) == scratch_order_at(previous_pages, key, customer, date, priority));
                            assert(order_count(work.orders@, key as int, customer as int, date as int, priority as int) == order_count(previous_work.orders@, key as int, customer as int, date as int, priority as int)
                                + if key == row.o_orderkey && customer == row.o_custkey && date == row.o_orderdate && priority == row.o_shippriority { 1int } else { 0int });
                        }
                    }
                }
            } else {
                proof {
                    assert forall|key: i64, customer: i64, date: i32, priority: i64| #![trigger scratch_order_at(pages_work@, key, customer, date, priority)]
                        in_chunk(key, chunk as int) implies scratch_order_at(pages_work@, key, customer, date, priority) == order_prefix(orders@.take(processed + 1), key, customer, date, priority) by {
                        let weight = |entry: OrderInput| if entry.o_orderkey == key && entry.o_custkey == customer && entry.o_orderdate == date && entry.o_shippriority == priority { 1int } else { 0int };
                        lemma_sum_take(orders@, processed, weight);
                        assert(row.o_orderkey != key);
                    }
                }
            }
            index += 1;
        }
        proof { assert(orders@.take(index as int) =~= orders@); }
        index = 0;
        while index < shipments.len()
            invariant
                index <= shipments.len(), 0 <= chunk < 16,
                scan_customers_ok(db, customers@), scan_orders_ok(db, orders@), scan_shipments_ok(db, shipments@),
                page_keys@.no_duplicates(), page_keys@.to_set() == pages_work@.dom(),
                forall|key: i64| #![trigger pages_work@.dom().contains(key)] pages_work@.dom().contains(key) ==>
                    work_page_shape(pages_work@[key], key as int) && chunk_of(key) == chunk as int,
                forall|key: i64, customer: i64, date: i32, priority: i64| #![trigger scratch_order_at(pages_work@, key, customer, date, priority)]
                    in_chunk(key, chunk as int) ==> scratch_order_at(pages_work@, key, customer, date, priority) == order_prefix(orders@, key, customer, date, priority),
                forall|key: i64, date: i32, kind: int| #![trigger scratch_ship_at(pages_work@, key, date, kind)]
                    in_chunk(key, chunk as int) && 0 <= kind <= 2 ==> scratch_ship_at(pages_work@, key, date, kind) == ship_prefix(shipments@.take(index as int), key, date, kind),
            decreases shipments.len() - index,
        {
            let ghost processed = index as int;
            let ghost previous_pages = pages_work@;
            let row = shipments[index];
            let page = page_id(row.l_orderkey);
            if chunk_exec(page) == chunk {
                let mut work = match pages_work.remove(&page) {
                    Some(work) => work,
                    None => {
                        proof {
                            verus_prooflib::lemma_unique_push(page_keys@, page);
                            verus_prooflib::lemma_push_to_set(page_keys@, page);
                        }
                        page_keys.push(page); empty_page()
                    },
                };
                let ghost previous_work = work;
                if !insert_ship(&mut work.ships, row) { return None; }
                pages_work.insert(page, work);
                proof {
                    assert(pages_work@ == previous_pages.insert(page, work));
                    assert forall|key: i64, customer: i64, date: i32, priority: i64| #![trigger scratch_order_at(pages_work@, key, customer, date, priority)]
                        in_chunk(key, chunk as int) implies scratch_order_at(pages_work@, key, customer, date, priority) == order_prefix(orders@, key, customer, date, priority) by {
                        assert(scratch_order_at(previous_pages, key, customer, date, priority) == order_prefix(orders@, key, customer, date, priority));
                    }
                    assert forall|key: i64, date: i32, kind: int| #![trigger scratch_ship_at(pages_work@, key, date, kind)]
                        in_chunk(key, chunk as int) && 0 <= kind <= 2 implies scratch_ship_at(pages_work@, key, date, kind) == ship_prefix(shipments@.take(processed + 1), key, date, kind) by {
                        let weight = |entry: ShipmentInput| if entry.l_orderkey == key && entry.l_shipdate == date {
                            let value = entry.l_extendedprice as int * (100 - entry.l_discount as int);
                            if kind == 0 { 1int } else if kind == 1 { value } else { if value > 0 { value } else { 0int } }
                        } else { 0int };
                        lemma_sum_take(shipments@, processed, weight);
                        assert(scratch_ship_at(previous_pages, key, date, kind) == ship_prefix(shipments@.take(processed), key, date, kind));
                        assert(ship_prefix(shipments@.take(processed + 1), key, date, kind) == ship_prefix(shipments@.take(processed), key, date, kind) + weight(row));
                        if (key as int / 256) as i64 == page {
                            assert(ship_count(previous_work.ships@, key as int, date as int, kind) == scratch_ship_at(previous_pages, key, date, kind));
                            assert(ship_count(work.ships@, key as int, date as int, kind) == ship_count(previous_work.ships@, key as int, date as int, kind)
                                + if key == row.l_orderkey && date == row.l_shipdate {
                                    let value = row.l_extendedprice as int * (100 - row.l_discount as int);
                                    if kind == 0 { 1int } else if kind == 1 { value } else { if value > 0 { value } else { 0int } }
                                } else { 0int });
                        }
                    }
                }
            } else {
                proof {
                    assert forall|key: i64, date: i32, kind: int| #![trigger scratch_ship_at(pages_work@, key, date, kind)]
                        in_chunk(key, chunk as int) && 0 <= kind <= 2 implies scratch_ship_at(pages_work@, key, date, kind) == ship_prefix(shipments@.take(processed + 1), key, date, kind) by {
                        let weight = |entry: ShipmentInput| if entry.l_orderkey == key && entry.l_shipdate == date {
                            let value = entry.l_extendedprice as int * (100 - entry.l_discount as int);
                            if kind == 0 { 1int } else if kind == 1 { value } else { if value > 0 { value } else { 0int } }
                        } else { 0int };
                        lemma_sum_take(shipments@, processed, weight);
                        assert(row.l_orderkey != key);
                    }
                }
            }
            index += 1;
        }
        proof { assert(shipments@.take(index as int) =~= shipments@); }
        let ghost completed_pages = pages_work@;
        proof {
            assert forall|number: i64| #![trigger completed_pages[number]] completed_pages.contains_key(number)
                implies work_database_ok(completed_pages[number], number as int, db) by {
                crate::semantics::lemma_work_scan(db, customers@, orders@, shipments@, completed_pages, number);
            }
        }
        proof {
            assert forall|key: i64| #![trigger completed_pages.dom().contains(key)] completed_pages.dom().contains(key) implies chunk_of(key) == chunk as int by {
                assert(pages_work@.dom().contains(key));
            }
        }
        proof {
            assert forall|key: i64, customer: i64, date: i32, priority: i64| #![trigger scratch_order_at(completed_pages, key, customer, date, priority)]
                in_chunk(key, chunk as int) implies scratch_order_at(completed_pages, key, customer, date, priority) == order_prefix(orders@, key, customer, date, priority) by {}
            assert forall|key: i64, date: i32, kind: int| #![trigger scratch_ship_at(completed_pages, key, date, kind)]
                in_chunk(key, chunk as int) && 0 <= kind <= 2 implies scratch_ship_at(completed_pages, key, date, kind) == ship_prefix(shipments@, key, date, kind) by {}
        }
        let mut position = 0;
        while position < page_keys.len()
            invariant position <= page_keys.len(), 0 <= chunk < 16,
                forall|key: int, customer: int, date: int, priority: int| #![trigger order_at(base_index, key, customer, date, priority)]
                    chunk_done(key, chunk as int) ==> order_at(base_index, key, customer, date, priority) == order_sum(db, key, customer, date, priority),
                forall|key: int, date: int, kind: int| #![trigger ship_at(base_index, key, date, kind)]
                    chunk_done(key, chunk as int) && 0 <= kind <= 2 ==> ship_at(base_index, key, date, kind) == ship_sum(db, key, date, kind),
                forall|segment: Seq<u8>, key: int, date: int, priority: int| #![trigger group_at(base_index, segment, key, date, priority)]
                    chunk_done(key, chunk as int) ==> group_at(base_index, segment, key, date, priority) == weight_sum(db, segment, key, date, priority),
                forall|key: i64, customer: i64, date: i32, priority: i64| #![trigger scratch_order_at(completed_pages, key, customer, date, priority)]
                    in_chunk(key, chunk as int) ==> scratch_order_at(completed_pages, key, customer, date, priority) == order_prefix(orders@, key, customer, date, priority),
                forall|key: i64, date: i32, kind: int| #![trigger scratch_ship_at(completed_pages, key, date, kind)]
                    in_chunk(key, chunk as int) && 0 <= kind <= 2 ==> scratch_ship_at(completed_pages, key, date, kind) == ship_prefix(shipments@, key, date, kind),
                customer_database_ok(customer_index, db),
                page_index.wf(), ranks_wf(rank_index),
                page_keys@.no_duplicates(), page_keys@.to_set() == completed_pages.dom(),
                forall|key: i64| #![trigger completed_pages.dom().contains(key)] completed_pages.dom().contains(key) ==> chunk_of(key) == chunk as int,
                groups_in_pages(page_index, published_groups),
                forall|outer: verus_prooflib::ordered::KeyView, rank: verus_prooflib::ordered::KeyView| #![trigger rank_at(rank_index, outer, rank)]
                    rank_at(rank_index, outer, rank) == rank_present(published_groups, outer, rank),
                forall|target: verus_prooflib::ordered::KeyView| #![trigger page_index.at(target)]
                    !(target.1 =~= Seq::<u8>::empty() && i64::MIN <= target.0 <= i64::MAX && page_keys@.take(position as int).contains(target.0 as i64))
                    ==> page_index.at(target) == base_index.at(target),
                forall|target: verus_prooflib::ordered::KeyView| #![trigger page_index.at(target)]
                    (target.1 =~= Seq::<u8>::empty() && i64::MIN <= target.0 <= i64::MAX && page_keys@.take(position as int).contains(target.0 as i64))
                    ==> page_index.at(target).is_some(),
                forall|target: verus_prooflib::ordered::KeyView| #![trigger base_index.at(target)] base_index.at(target).is_some() ==>
                    (target.1.len() == 0 && i64::MIN <= target.0 <= i64::MAX && chunk_of(target.0 as i64) < chunk as int
                        && page_database_ok(base_index.at(target).unwrap(), target.0, db)),
                forall|target: verus_prooflib::ordered::KeyView| #![trigger page_index.at(target)] page_index.at(target).is_some() ==>
                    (target.1.len() == 0 && i64::MIN <= target.0 <= i64::MAX && chunk_of(target.0 as i64) <= chunk as int
                        && page_database_ok(page_index.at(target).unwrap(), target.0, db)),
                forall|key: i64| #![trigger pages_work@[key]] pages_work@.contains_key(key) ==> work_database_ok(pages_work@[key], key as int, db),
                forall|key: i64| #![trigger pages_work@[key]] pages_work@.dom().contains(key) ==> work_page_shape(pages_work@[key], key as int),
                forall|key: i64| #![trigger pages_work@.dom().contains(key)] pages_work@.dom().contains(key) ==> completed_pages.dom().contains(key),
                scan_customers_ok(db, customers@), scan_orders_ok(db, orders@), scan_shipments_ok(db, shipments@),
                forall|target: verus_prooflib::ordered::KeyView| #![trigger customer_index.at(target)] customer_index.at(target).is_some() ==> customer_shape(customer_index.at(target).unwrap()),
            decreases page_keys.len() - position,
        {
            let key = page_keys[position];
            let ghost previous_pages = page_index;
            let ghost previous_ranks = rank_index;
            let ghost previous_groups = published_groups;
            proof { verus_prooflib::lemma_prefix_fresh(page_keys@, position as int); }
            let work = match pages_work.remove(&key) { Some(work) => work, None => return None };
            let ghost original_work = work;
            let groups = match make_groups(&work, &customer_index) { Some(groups) => groups, None => return None };
            let packed_orders = crate::packed::PackedOrders::pack(&work.orders, key);
            let packed_ships = crate::packed::PackedShips::pack(&work.ships, key);
            let packed_groups = crate::packed::PackedGroups::pack(&groups, key);
            let page = Page { orders: Arc::new(packed_orders), ships: Arc::new(packed_ships), groups: Arc::new(packed_groups) };
            let page_groups = page.groups.to_vec();
            ranks::append_groups(&mut rank_index, &page_groups);
            proof { published_groups = published_groups + page_groups@; }
            proof { crate::semantics::lemma_page_publication_equiv(page, groups@, original_work, key as int, customer_index, db); }
            proof {
                assert(page_keys@.contains(key)) by {
                    page_keys@.to_set_ensures();
                    assert(page_keys@.take(position as int + 1)[position as int] == key);
                    assert(page_keys@[position as int] == key);
                }
                assert(completed_pages.dom().contains(key));
                assert(chunk_of(key) == chunk as int);
                assert(base_index.at((key as int, Seq::<u8>::empty())).is_none()) by {
                    if base_index.at((key as int, Seq::<u8>::empty())).is_some() { assert(chunk_of(key) < chunk as int); }
                }
                assert(previous_pages.at((key as int, Seq::<u8>::empty())).is_none()) by {
                    assert(!page_keys@.take(position as int).contains(key));
                }
            }
            page_index.put(Key::number(key), Arc::new(page));
            proof {
                assert(page_index.at((key as int, Seq::<u8>::empty())) == Some(page));
                lemma_page_groups_put(previous_pages, page_index, previous_groups, page, key);
                assert(page_keys@.take(position as int + 1) =~= page_keys@.take(position as int).push(key));
                verus_prooflib::lemma_push_membership(page_keys@.take(position as int), key);
                assert forall|outer: verus_prooflib::ordered::KeyView, rank: verus_prooflib::ordered::KeyView| #![trigger rank_at(rank_index, outer, rank)]
                    rank_at(rank_index, outer, rank) == rank_present(published_groups, outer, rank) by {
                    ranks::lemma_rank_concat(previous_groups, (*page.groups)@, outer, rank);
                    assert(rank_at(previous_ranks, outer, rank) == rank_present(previous_groups, outer, rank));
                }
            }
            position += 1;
        }
        proof {
            assert(page_keys@.take(position as int) =~= page_keys@);
            assert forall|number: i64| #![trigger page_keys@.contains(number)] page_keys@.contains(number) implies chunk_of(number) == chunk as int by {
                page_keys@.to_set_ensures();
                assert(completed_pages.dom().contains(number));
            }
            // the chunk's pages are in; the pages of earlier chunks are untouched
            assert forall|target: verus_prooflib::ordered::KeyView| #![trigger page_index.at(target)] page_index.at(target).is_some() implies
                (target.1.len() == 0 && i64::MIN <= target.0 <= i64::MAX && chunk_of(target.0 as i64) < chunk as int + 1
                    && page_database_ok(page_index.at(target).unwrap(), target.0, db)) by {
                if target.1 =~= Seq::<u8>::empty() && i64::MIN <= target.0 <= i64::MAX && page_keys@.contains(target.0 as i64) {
                    assert(completed_pages.dom().contains(target.0 as i64));
                } else { assert(page_index.at(target) == base_index.at(target)); }
            }
            // sums of the chunk's keys
            crate::input_proofs::lemma_scan_domains(db, customers@, orders@, shipments@);
            assert forall|key: int, customer: int, date: int, priority: int| #![trigger order_at(page_index, key, customer, date, priority)]
                chunk_done(key, chunk as int + 1) implies order_at(page_index, key, customer, date, priority) == order_sum(db, key, customer, date, priority) by {
                if chunk_done(key, chunk as int) {
                    assert(page_index.at((key / 256, Seq::<u8>::empty())) == base_index.at((key / 256, Seq::<u8>::empty()))) by {
                        assert(!page_keys@.contains((key / 256) as i64));
                    }
                    assert(order_at(base_index, key, customer, date, priority) == order_sum(db, key, customer, date, priority));
                } else if page_index.at((key / 256, Seq::empty())).is_some() {
                    assert(page_database_ok(page_index.at((key / 256, Seq::empty())).unwrap(), key / 256, db));
                    assert(order_count((*page_index.at((key / 256, Seq::empty())).unwrap().orders)@, key, customer, date, priority)
                        == order_sum(db, key, customer, date, priority));
                } else if i64::MIN <= customer <= i64::MAX && i32::MIN <= date <= i32::MAX && i64::MIN <= priority <= i64::MAX {
                    assert(i64::MIN <= key / 256 <= i64::MAX);
                    assert(!completed_pages.contains_key((key / 256) as i64)) by {
                        assert(!page_keys@.contains((key / 256) as i64));
                    }
                    crate::input_proofs::lemma_order_scan(db, orders@, customer as i64, key as i64, date as i32, priority as i64);
                    assert(in_chunk(key as i64, chunk as int));
                    assert(scratch_order_at(completed_pages, key as i64, customer as i64, date as i32, priority as i64) == 0);
                    assert(scratch_order_at(completed_pages, key as i64, customer as i64, date as i32, priority as i64)
                        == order_prefix(orders@, key as i64, customer as i64, date as i32, priority as i64));
                } else {
                    lemma_sum_zero(db.orders, |row: OrdersRow| if row.o_orderkey == key && row.o_custkey == customer
                        && row.o_orderdate == date && row.o_shippriority == priority { 1int } else { 0int });
                }
            }
            assert forall|key: int, date: int, kind: int| #![trigger ship_at(page_index, key, date, kind)]
                chunk_done(key, chunk as int + 1) && 0 <= kind <= 2 implies ship_at(page_index, key, date, kind) == ship_sum(db, key, date, kind) by {
                if chunk_done(key, chunk as int) {
                    assert(page_index.at((key / 256, Seq::<u8>::empty())) == base_index.at((key / 256, Seq::<u8>::empty()))) by {
                        assert(!page_keys@.contains((key / 256) as i64));
                    }
                    assert(ship_at(base_index, key, date, kind) == ship_sum(db, key, date, kind));
                } else if page_index.at((key / 256, Seq::empty())).is_some() {
                    assert(page_database_ok(page_index.at((key / 256, Seq::empty())).unwrap(), key / 256, db));
                    assert(ship_count((*page_index.at((key / 256, Seq::empty())).unwrap().ships)@, key, date, kind) == ship_sum(db, key, date, kind));
                } else if i32::MIN <= date <= i32::MAX {
                    assert(i64::MIN <= key / 256 <= i64::MAX);
                    assert(!completed_pages.contains_key((key / 256) as i64)) by {
                        assert(!page_keys@.contains((key / 256) as i64));
                    }
                    crate::input_proofs::lemma_ship_scan(db, shipments@, key as i64, date as i32, kind);
                    assert(in_chunk(key as i64, chunk as int));
                    assert(scratch_ship_at(completed_pages, key as i64, date as i32, kind) == 0);
                    assert(scratch_ship_at(completed_pages, key as i64, date as i32, kind) == ship_prefix(shipments@, key as i64, date as i32, kind));
                } else {
                    lemma_sum_zero(db.lineitem, |row: LineitemRow| if row.l_orderkey == key && row.l_shipdate == date {
                        let value = row.l_extendedprice * (100 - row.l_discount);
                        if kind == 0 { 1int } else if kind == 1 { value } else { if value > 0 { value } else { 0int } }
                    } else { 0int });
                }
            }
            assert forall|segment: Seq<u8>, key: int, date: int, priority: int| #![trigger group_at(page_index, segment, key, date, priority)]
                chunk_done(key, chunk as int + 1) implies group_at(page_index, segment, key, date, priority) == weight_sum(db, segment, key, date, priority) by {
                if chunk_done(key, chunk as int) {
                    assert(page_index.at((key / 256, Seq::<u8>::empty())) == base_index.at((key / 256, Seq::<u8>::empty()))) by {
                        assert(!page_keys@.contains((key / 256) as i64));
                    }
                    assert(group_at(base_index, segment, key, date, priority) == weight_sum(db, segment, key, date, priority));
                } else if page_index.at((key / 256, Seq::empty())).is_some() {
                    assert(page_database_ok(page_index.at((key / 256, Seq::empty())).unwrap(), key / 256, db));
                    assert(column_weight((*page_index.at((key / 256, Seq::empty())).unwrap().groups)@, segment, key, date, priority)
                        == weight_sum(db, segment, key, date, priority));
                } else {
                    assert forall|position: int| #![trigger db.orders[position]] 0 <= position < db.orders.len() implies db.orders[position].o_orderkey != key by {
                        let row = db.orders[position];
                        if row.o_orderkey == key {
                            let weight = |entry: OrdersRow| if entry.o_orderkey == key && entry.o_custkey == row.o_custkey
                                && entry.o_orderdate == row.o_orderdate && entry.o_shippriority == row.o_shippriority { 1int } else { 0int };
                            lemma_sum_pos(db.orders, weight, position);
                            assert(chunk_done(key, chunk as int + 1));
                            assert(order_at(page_index, key, row.o_custkey, row.o_orderdate, row.o_shippriority) == 0);
                            assert(order_sum(db, key, row.o_custkey, row.o_orderdate, row.o_shippriority) == 0);
                        }
                    }
                    lemma_sum_zero(db.orders, |row: OrdersRow| if row.o_orderkey == key && row.o_orderdate == date && row.o_shippriority == priority {
                        customer_sum(db, row.o_custkey, segment)
                    } else { 0int });
                }
            }
        }
        chunk += 1;
    }
    proof {
        crate::semantics::lemma_rank_publication(page_index, rank_index, published_groups, db);
        crate::input_proofs::lemma_scan_domains(db, customers@, orders@, shipments@);
        assert forall|key: int, customer: int, date: int, priority: int| #![trigger order_at(page_index, key, customer, date, priority)]
            order_at(page_index, key, customer, date, priority) == order_sum(db, key, customer, date, priority) by {
            if chunk_done(key, 16) {}
            else if page_index.at((key / 256, Seq::<u8>::empty())).is_some() {
                assert(page_database_ok(page_index.at((key / 256, Seq::empty())).unwrap(), key / 256, db));
            } else {
                lemma_sum_zero(db.orders, |row: OrdersRow| if row.o_orderkey == key && row.o_custkey == customer
                    && row.o_orderdate == date && row.o_shippriority == priority { 1int } else { 0int });
            }
        }
        assert forall|key: int, date: int, kind: int| #![trigger ship_at(page_index, key, date, kind)]
            0 <= kind <= 2 implies ship_at(page_index, key, date, kind) == ship_sum(db, key, date, kind) by {
            if chunk_done(key, 16) {}
            else if page_index.at((key / 256, Seq::<u8>::empty())).is_some() {
                assert(page_database_ok(page_index.at((key / 256, Seq::empty())).unwrap(), key / 256, db));
            } else {
                lemma_sum_zero(db.lineitem, |row: LineitemRow| if row.l_orderkey == key && row.l_shipdate == date {
                    let value = row.l_extendedprice * (100 - row.l_discount);
                    if kind == 0 { 1int } else if kind == 1 { value } else { if value > 0 { value } else { 0int } }
                } else { 0int });
            }
        }
        assert forall|segment: Seq<u8>, key: int, date: int, priority: int| #![trigger group_at(page_index, segment, key, date, priority)]
            group_at(page_index, segment, key, date, priority) == weight_sum(db, segment, key, date, priority) by {
            if chunk_done(key, 16) {}
            else if page_index.at((key / 256, Seq::<u8>::empty())).is_some() {
                assert(page_database_ok(page_index.at((key / 256, Seq::empty())).unwrap(), key / 256, db));
            } else {
                assert forall|position: int| #![trigger db.orders[position]] 0 <= position < db.orders.len() implies db.orders[position].o_orderkey != key by {}
                lemma_sum_zero(db.orders, |row: OrdersRow| if row.o_orderkey == key && row.o_orderdate == date && row.o_shippriority == priority {
                    customer_sum(db, row.o_custkey, segment)
                } else { 0int });
            }
        }
        assert(state_ok(customer_index, page_index, rank_index, db));
    }
    Some(State::from_parts(customer_index, page_index, rank_index, Ghost(db)))
}

}

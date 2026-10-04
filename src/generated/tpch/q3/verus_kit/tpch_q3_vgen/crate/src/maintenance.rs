use vstd::prelude::*;
use vstd::seq_lib::*;
use vstd::hash_map::HashMapWithView;
use std::sync::Arc;
use verus_prooflib::ordered::{Index, Key};
use crate::gen_spec::*;
use crate::gen_obligations::*;
use crate::kernel::State;
use crate::columns::*;
use crate::model::*;
use crate::values::*;
use crate::ranks;
use verus_common::sums::*;

verus! {

pub open spec fn customer_phase(work: Map<i64, CustomerCell>, before: Index<CustomerCell>, db: Db) -> bool {
    forall|customer: i64, segment: Seq<u8>| #![trigger customer_overlay(work, before, customer, segment)]
        customer_overlay(work, before, customer, segment) == customer_sum(db, customer as int, segment)
}

pub proof fn lemma_customer_frame(before: Map<i64, CustomerCell>, after: Map<i64, CustomerCell>,
    base: Index<CustomerCell>, db: Db, key: i64, cell: CustomerCell)
    requires customer_phase(before, base, db), after == before.insert(key, cell),
        forall|segment: Seq<u8>| #![trigger segment_count(cell.segments@, segment)]
            segment_count(cell.segments@, segment) == customer_overlay(before, base, key, segment),
    ensures customer_phase(after, base, db),
{
    assert forall|customer: i64, segment: Seq<u8>| #![trigger customer_overlay(after, base, customer, segment)]
        customer_overlay(after, base, customer, segment) == customer_sum(db, customer as int, segment) by {
        assert(customer_overlay(before, base, customer, segment) == customer_sum(db, customer as int, segment));
    }
}

pub fn publish_changes(mut work: HashMapWithView<i64, CustomerCell>, keys: &Vec<i64>, before: &Index<CustomerCell>)
    -> (result: Result<Index<CustomerCell>, u32>)
    requires before.wf(), keys@.no_duplicates(), keys@.to_set() == work@.dom(),
        forall|key: i64| #![trigger work@[key]] work@.contains_key(key) ==> signed_customer_shape(work@[key]),
        forall|target: verus_prooflib::ordered::KeyView| #![trigger before.at(target)]
            before.at(target).is_some() ==> target.1.len() == 0 && customer_shape(before.at(target).unwrap()),
    ensures result matches Ok(after) ==> {
        &&& after.wf()
        &&& forall|target: verus_prooflib::ordered::KeyView| #![trigger after.at(target)] after.at(target).is_some() ==>
            target.1.len() == 0 && customer_shape(after.at(target).unwrap())
        &&& forall|customer: i64, segment: Seq<u8>| #![trigger customer_at(after, customer as int, segment)]
            customer_at(after, customer as int, segment) == customer_overlay(work@, *before, customer, segment)
        &&& forall|customer: i64, key: i64| #![trigger reverse_at(after, customer as int, key as int)]
            reverse_at(after, customer as int, key as int) == reverse_overlay(work@, *before, customer, key)
        &&& forall|target: verus_prooflib::ordered::KeyView| #![trigger after.at(target)]
            target.1.len() != 0 || target.0 < i64::MIN || target.0 > i64::MAX ==> after.at(target) == before.at(target)
    },
{
    hide(customer_at);
    hide(reverse_at);
    hide(customer_overlay);
    hide(reverse_overlay);
    let ghost original = work@;
    let mut after = before.share();
    proof {
        assert forall|target: verus_prooflib::ordered::KeyView| #![trigger after.at(target)] after.at(target) == before.at(target) by {
            assert(after.tree() == before.tree());
        }
    }
    let mut position = 0;
    while position < keys.len()
        invariant position <= keys.len(), keys@.no_duplicates(), keys@.to_set() == original.dom(), after.wf(),
            forall|key: i64| #![trigger original[key]] original.contains_key(key) ==> signed_customer_shape(original[key]),
            forall|key: i64| #![trigger work@.contains_key(key)]
                work@.contains_key(key) == (original.contains_key(key) && !keys@.take(position as int).contains(key)),
            forall|key: i64| #![trigger work@[key]] work@.contains_key(key) ==> work@[key] == original[key],
            forall|target: verus_prooflib::ordered::KeyView| #![trigger before.at(target)]
                before.at(target).is_some() ==> target.1.len() == 0 && customer_shape(before.at(target).unwrap()),
            forall|target: verus_prooflib::ordered::KeyView| #![trigger after.at(target)] after.at(target) ==
                if target.1.len() == 0 && i64::MIN <= target.0 <= i64::MAX && keys@.take(position as int).contains(target.0 as i64) {
                    let cell = original[target.0 as i64];
                    if cell.segments@.len() == 0 && cell.orders@.len() == 0 { None } else { Some(cell) }
                } else { before.at(target) },
            forall|target: verus_prooflib::ordered::KeyView| #![trigger after.at(target)] after.at(target).is_some() ==>
                target.1.len() == 0 && customer_shape(after.at(target).unwrap()),
        decreases keys.len() - position,
    {
        let key = keys[position];
        let ghost prior = work@;
        let ghost previous_index = after;
        proof { verus_prooflib::lemma_prefix_fresh(keys@, position as int); }
        let cell = match work.remove(&key) { Some(cell) => cell, None => return Err(1) };
        proof { assert(cell == original[key]); }
        if !valid_customer(&cell) { return Err(3); }
        if cell.segments.len() == 0 && cell.orders.len() == 0 { after.remove(&Key::number(key)); }
        else { after.put(Key::number(key), Arc::new(cell)); }
        proof {
            assert(keys@.take(position as int + 1) =~= keys@.take(position as int).push(key));
            verus_prooflib::lemma_push_membership(keys@.take(position as int), key);
            assert forall|target: verus_prooflib::ordered::KeyView| #![trigger after.at(target)] after.at(target) ==
                if target.1.len() == 0 && i64::MIN <= target.0 <= i64::MAX && keys@.take(position as int + 1).contains(target.0 as i64) {
                    let selected = original[target.0 as i64];
                    if selected.segments@.len() == 0 && selected.orders@.len() == 0 { None } else { Some(selected) }
                } else { before.at(target) } by {
                if target.1.len() == 0 { assert(target.1 =~= Seq::<u8>::empty()); }
                if target == (key as int, Seq::<u8>::empty()) { assert(original[target.0 as i64] == cell); }
                else {
                    assert(after.at(target) == previous_index.at(target));
                    if target.1.len() == 0 && i64::MIN <= target.0 <= i64::MAX {
                        assert(target.0 as i64 != key);
                        assert(keys@.take(position as int + 1).contains(target.0 as i64) == keys@.take(position as int).contains(target.0 as i64));
                    }
                }
            }
            assert forall|target: i64| #![trigger work@.contains_key(target)]
                work@.contains_key(target) == (original.contains_key(target) && !keys@.take(position as int + 1).contains(target)) by {
                assert(prior.contains_key(target) == (original.contains_key(target) && !keys@.take(position as int).contains(target)));
            }
        }
        position += 1;
    }
    proof {
        assert(keys@.take(position as int) =~= keys@);
        reveal(customer_at); reveal(reverse_at); reveal(customer_overlay); reveal(reverse_overlay);
        assert forall|customer: i64, segment: Seq<u8>| #![trigger customer_at(after, customer as int, segment)]
            customer_at(after, customer as int, segment) == customer_overlay(original, *before, customer, segment) by {
            if original.contains_key(customer) && original[customer].segments@.len() == 0 && original[customer].orders@.len() == 0 {
                lemma_sum_zero(original[customer].segments@, |entry: (Arc<Vec<u8>>, i64)| if (*entry.0)@ == segment { entry.1 as int } else { 0int });
            }
        }
        assert forall|customer: i64, key: i64| #![trigger reverse_at(after, customer as int, key as int)]
            reverse_at(after, customer as int, key as int) == reverse_overlay(original, *before, customer, key) by {
            if original.contains_key(customer) && original[customer].segments@.len() == 0 && original[customer].orders@.len() == 0 {
                lemma_sum_zero(original[customer].orders@, |entry: (i64, i64)| if entry.0 == key as int { entry.1 as int } else { 0int });
            }
        }
    }
    Ok(after)
}

pub fn add_key(set: &mut HashMapWithView<i64, ()>, keys: &mut Vec<i64>, key: i64)
    requires old(keys)@.no_duplicates(), old(keys)@.to_set() == old(set)@.dom(),
    ensures final(keys)@.no_duplicates(), final(keys)@.to_set() == final(set)@.dom(),
        final(set)@.dom() == old(set)@.dom().insert(key),
{
    if !set.contains_key(&key) {
        proof {
            verus_prooflib::lemma_unique_push(keys@, key);
            verus_prooflib::lemma_push_to_set(keys@, key);
        }
        set.insert(key, ());
        keys.push(key);
    }
}

pub fn fact_keys(lines: &[ShipmentKeyDelta], orders: &[OrderKeyDelta]) -> (result: Vec<i64>)
    ensures result@.no_duplicates(), result@.to_set() == lines@.map_values(|row: ShipmentKeyDelta| row.l_orderkey).to_set()
        .union(orders@.map_values(|row: OrderKeyDelta| row.o_orderkey).to_set()),
{
    let mut seen: HashMapWithView<i64, ()> = HashMapWithView::new();
    let mut keys = Vec::new();
    let mut index = 0;
    while index < lines.len()
        invariant index <= lines.len(), keys@.no_duplicates(), keys@.to_set() == seen@.dom(),
            seen@.dom() == lines@.take(index as int).map_values(|row: ShipmentKeyDelta| row.l_orderkey).to_set(),
        decreases lines.len() - index,
    {
        let ghost processed = index as int;
        add_key(&mut seen, &mut keys, lines[index].l_orderkey);
        proof {
            let projected = lines@.take(index as int).map_values(|row: ShipmentKeyDelta| row.l_orderkey);
            assert(lines@.take(index as int + 1).map_values(|row: ShipmentKeyDelta| row.l_orderkey)
                =~= projected.push(lines@[index as int].l_orderkey));
            verus_prooflib::lemma_push_to_set(projected, lines@[index as int].l_orderkey);
        }
        index += 1;
    }
    proof { assert(lines@.take(index as int) =~= lines@); }
    index = 0;
    while index < orders.len()
        invariant index <= orders.len(), keys@.no_duplicates(), keys@.to_set() == seen@.dom(),
            seen@.dom() == lines@.map_values(|row: ShipmentKeyDelta| row.l_orderkey).to_set()
                .union(orders@.take(index as int).map_values(|row: OrderKeyDelta| row.o_orderkey).to_set()),
        decreases orders.len() - index,
    {
        let ghost processed = index as int;
        add_key(&mut seen, &mut keys, orders[index].o_orderkey);
        proof {
            let projected = orders@.take(index as int).map_values(|row: OrderKeyDelta| row.o_orderkey);
            assert(orders@.take(index as int + 1).map_values(|row: OrderKeyDelta| row.o_orderkey)
                =~= projected.push(orders@[index as int].o_orderkey));
            verus_prooflib::lemma_push_to_set(projected, orders@[index as int].o_orderkey);
            assert(lines@.map_values(|row: ShipmentKeyDelta| row.l_orderkey).to_set().union(projected.to_set().insert(orders@[index as int].o_orderkey))
                =~= lines@.map_values(|row: ShipmentKeyDelta| row.l_orderkey).to_set().union(projected.to_set()).insert(orders@[index as int].o_orderkey));
        }
        index += 1;
    }
    proof { assert(orders@.take(index as int) =~= orders@); }
    keys
}

pub fn key_set(keys: &[i64]) -> (result: HashMapWithView<i64, ()>)
    ensures result@.dom() == keys@.to_set(),
{
    let mut set = HashMapWithView::new();
    let mut index = 0;
    while index < keys.len()
        invariant index <= keys.len(), set@.dom() == keys@.take(index as int).to_set(),
        decreases keys.len() - index,
    {
        set.insert(keys[index], ());
        proof {
            assert(keys@.take(index as int + 1) =~= keys@.take(index as int).push(keys@[index as int]));
            verus_prooflib::lemma_push_to_set(keys@.take(index as int), keys@[index as int]);
        }
        index += 1;
    }
    proof { assert(keys@.take(index as int) =~= keys@); }
    set
}

pub fn take_customer(work: &mut HashMapWithView<i64, CustomerCell>, keys: &mut Vec<i64>,
    before: &Index<CustomerCell>, key: i64) -> (result: CustomerCell)
    requires
        old(keys)@.no_duplicates(), old(keys)@.to_set() == old(work)@.dom(),
        forall|target: i64| #![trigger old(work)@[target]] old(work)@.dom().contains(target) ==> signed_customer_shape(old(work)@[target]),
        forall|target: verus_prooflib::ordered::KeyView| #![trigger before.at(target)] before.at(target).is_some() ==> customer_shape(before.at(target).unwrap()),
    ensures
        final(keys)@.no_duplicates(), final(keys)@.to_set() == final(work)@.dom().insert(key),
        signed_customer_shape(result), final(work)@ == old(work)@.remove(key),
        forall|segment: Seq<u8>| #![trigger segment_count(result.segments@, segment)]
            segment_count(result.segments@, segment) == customer_overlay(old(work)@, *before, key, segment),
        forall|orderkey: i64| #![trigger reverse_count(result.orders@, orderkey as int)]
            reverse_count(result.orders@, orderkey as int) == reverse_overlay(old(work)@, *before, key, orderkey),
{
    match work.remove(&key) {
        Some(cell) => cell,
        None => {
            proof {
                verus_prooflib::lemma_unique_push(keys@, key);
                verus_prooflib::lemma_push_to_set(keys@, key);
            }
            keys.push(key);
            match before.get(&Key::number(key)) {
                Some(cell) => copy_customer(&cell),
                None => empty_customer(),
            }
        },
    }
}

pub fn maintain(state: &State, customer_delta: &Vec<CustomerDelta>,
    lineitem_delta: &[ShipmentKeyDelta], orders_delta: &[OrderKeyDelta],
    orders_keys: &[i64], order_rows: &[OrderInput], ships_keys: &[i64], ship_rows: &[ShipmentInput],
    Ghost(lineitem_full): Ghost<Seq<(LineitemRow, int)>>,
    Ghost(orders_full): Ghost<Seq<(OrdersRow, int)>>,
    Ghost(db2): Ghost<Db>) -> (result: Result<State, u32>)
    requires
        lineitem_delta_ok(lineitem_delta@, lineitem_full),
        orders_delta_ok(orders_delta@, orders_full),
        capture_ok(state.db(), db2, customer_delta_seq_view(customer_delta@), lineitem_full, orders_full),
        rescan_orders_by_key_ok(db2, orders_keys@, order_rows@),
        rescan_shipments_by_key_ok(db2, ships_keys@, ship_rows@),
    ensures result matches Ok(next) ==> next.db() == db2,
{
    state.facts();
    let facts = fact_keys(lineitem_delta, orders_delta);
    let ghost fact_keys_view = facts@;
    let fact_set = key_set(&facts);
    let order_coverage = key_set(orders_keys);
    let ship_coverage = key_set(ships_keys);
    let mut index = 0;
    while index < facts.len()
        invariant index <= facts.len(),
            order_coverage@.dom() == orders_keys@.to_set(), ship_coverage@.dom() == ships_keys@.to_set(),
            forall|position: int| #![trigger facts@[position]] 0 <= position < index ==>
                orders_keys@.contains(facts@[position]) && ships_keys@.contains(facts@[position]),
        decreases facts.len() - index,
    {
        if !order_coverage.contains_key(&facts[index]) || !ship_coverage.contains_key(&facts[index]) { return Err(1); }
        proof { assert(orders_keys@.contains(facts@[index as int]) && ships_keys@.contains(facts@[index as int])); }
        index += 1;
    }
    proof {
        assert(fact_set@.dom().subset_of(orders_keys@.to_set()));
        assert(fact_set@.dom().subset_of(ships_keys@.to_set()));
        assert forall|position: int| #![trigger orders_full[position]] 0 <= position < orders_full.len() implies
            i64::MIN <= orders_full[position].0.o_orderkey <= i64::MAX && fact_set@.dom().contains(orders_full[position].0.o_orderkey as i64) by {
            assert(orders_delta@[position].o_orderkey == orders_full[position].0.o_orderkey);
            assert(orders_delta@.map_values(|row: OrderKeyDelta| row.o_orderkey)[position] == orders_delta@[position].o_orderkey);
        }
        assert forall|position: int| #![trigger lineitem_full[position]] 0 <= position < lineitem_full.len() implies
            i64::MIN <= lineitem_full[position].0.l_orderkey <= i64::MAX && fact_set@.dom().contains(lineitem_full[position].0.l_orderkey as i64) by {
            assert(lineitem_delta@[position].l_orderkey == lineitem_full[position].0.l_orderkey);
            assert(lineitem_delta@.map_values(|row: ShipmentKeyDelta| row.l_orderkey)[position] == lineitem_delta@[position].l_orderkey);
        }
        crate::maintenance_proofs::lemma_transaction_domains(state.db(), db2, customer_delta@, lineitem_full, orders_full,
            fact_set@.dom(), orders_keys@, order_rows@, ships_keys@, ship_rows@);
    }
    let mut customers_work: HashMapWithView<i64, CustomerCell> = HashMapWithView::new();
    let mut customer_keys = Vec::new();
    let mut changed_customer_set: HashMapWithView<i64, ()> = HashMapWithView::new();
    let mut changed_customer_keys = Vec::new();
    index = 0;
    while index < customer_delta.len()
        invariant
            index <= customer_delta.len(),
            customer_keys@.no_duplicates(), customer_keys@.to_set() == customers_work@.dom(),
            changed_customer_keys@.no_duplicates(), changed_customer_keys@.to_set() == changed_customer_set@.dom(),
            changed_customer_set@.dom() == customer_delta@.take(index as int).map_values(|delta: CustomerDelta| delta.row.c_custkey).to_set(),
            state_ok(state.customers, state.pages, state.ranks, state.db()),
            forall|key: i64| #![trigger customers_work@[key]] customers_work@.dom().contains(key) ==> signed_customer_shape(customers_work@[key]),
            forall|customer: i64, segment: Seq<u8>| #![trigger customer_overlay(customers_work@, state.customers, customer, segment)]
                customer_overlay(customers_work@, state.customers, customer, segment)
                    == customer_at(state.customers, customer as int, segment)
                        + customer_delta_prefix(customer_delta@.take(index as int), customer, segment),
            forall|customer: i64, key: i64| #![trigger reverse_overlay(customers_work@, state.customers, customer, key)]
                reverse_overlay(customers_work@, state.customers, customer, key) == reverse_at(state.customers, customer as int, key as int),
        decreases customer_delta.len() - index,
    {
        let ghost processed = index as int;
        let ghost previous_work = customers_work@;
        let delta = &customer_delta[index];
        let key = delta.row.c_custkey;
        add_key(&mut changed_customer_set, &mut changed_customer_keys, key);
        proof {
            let prefix = customer_delta@.take(index as int).map_values(|delta: CustomerDelta| delta.row.c_custkey);
            assert(customer_delta@.take(index as int + 1).map_values(|delta: CustomerDelta| delta.row.c_custkey) =~= prefix.push(key));
            verus_prooflib::lemma_push_to_set(prefix, key);
        }
        let mut cell = take_customer(&mut customers_work, &mut customer_keys, &state.customers, key);
        let ghost previous_cell = cell;
        if !segment_change(&mut cell, &delta.row.c_mktsegment, delta.sign) { return Err(2); }
        customers_work.insert(key, cell);
        proof {
            assert(customers_work@ == previous_work.insert(key, cell));
            assert forall|customer: i64, orderkey: i64| #![trigger reverse_overlay(customers_work@, state.customers, customer, orderkey)]
                reverse_overlay(customers_work@, state.customers, customer, orderkey) == reverse_overlay(previous_work, state.customers, customer, orderkey) by {
                if customer == key {
                    assert(reverse_count(previous_cell.orders@, orderkey as int) == reverse_overlay(previous_work, state.customers, customer, orderkey));
                    assert(cell.orders@ == previous_cell.orders@);
                }
            }
            assert forall|customer: i64, segment: Seq<u8>| #![trigger customer_overlay(customers_work@, state.customers, customer, segment)]
                customer_overlay(customers_work@, state.customers, customer, segment)
                    == customer_at(state.customers, customer as int, segment) + customer_delta_prefix(customer_delta@.take(processed + 1), customer, segment) by {
                let weight = |entry: CustomerDelta| if entry.row.c_custkey == customer && entry.row.c_mktsegment@ == segment { entry.sign as int } else { 0int };
                lemma_sum_take(customer_delta@, processed, weight);
                assert(customer_overlay(previous_work, state.customers, customer, segment)
                    == customer_at(state.customers, customer as int, segment) + customer_delta_prefix(customer_delta@.take(processed), customer, segment));
                if customer == key {
                    assert(segment_count(previous_cell.segments@, segment) == customer_overlay(previous_work, state.customers, key, segment));
                    assert(segment_count(cell.segments@, segment) == segment_count(previous_cell.segments@, segment)
                        + if segment == delta.row.c_mktsegment@ { delta.sign as int } else { 0int });
                }
            }
        }
        index += 1;
    }
    proof {
        assert(customer_delta@.take(index as int) =~= customer_delta@);
        assert(changed_customer_set@.dom() == customer_delta@.map_values(|delta: CustomerDelta| delta.row.c_custkey).to_set());
        assert(changed_customer_keys@.to_set() == customer_delta@.map_values(|delta: CustomerDelta| delta.row.c_custkey).to_set());
        assert forall|customer: i64, segment: Seq<u8>| #![trigger customer_overlay(customers_work@, state.customers, customer, segment)]
            customer_overlay(customers_work@, state.customers, customer, segment) == customer_sum(db2, customer as int, segment) by {
            crate::input_proofs::lemma_customer_capture(state.db(), db2, customer_delta@, lineitem_full, orders_full, customer, segment);
        }
    }
    let mut page_set: HashMapWithView<i64, ()> = HashMapWithView::new();
    let mut page_keys = Vec::new();
    index = 0;
    while index < facts.len()
        invariant index <= facts.len(), page_keys@.no_duplicates(), page_keys@.to_set() == page_set@.dom(),
            page_set@.dom() == facts@.take(index as int).map_values(|key: i64| (key as int / 256) as i64).to_set(),
            customer_phase(customers_work@, state.customers, db2),
            customer_keys@.no_duplicates(), customer_keys@.to_set() == customers_work@.dom(),
            forall|position: int| #![trigger facts@[position]] 0 <= position < index ==>
                page_set@.dom().contains((facts@[position] as int / 256) as i64),
        decreases facts.len() - index,
    {
        add_key(&mut page_set, &mut page_keys, page_id(facts[index]));
        proof {
            let prefix = facts@.take(index as int).map_values(|key: i64| (key as int / 256) as i64);
            assert(facts@.take(index as int + 1).map_values(|key: i64| (key as int / 256) as i64) =~= prefix.push((facts@[index as int] as int / 256) as i64));
            verus_prooflib::lemma_push_to_set(prefix, (facts@[index as int] as int / 256) as i64);
        }
        index += 1;
    }
    proof {
        assert forall|key: i64| #![trigger fact_set@.dom().contains(key)] fact_set@.dom().contains(key) implies
            page_set@.dom().contains((key as int / 256) as i64) by {
            assert(facts@.contains(key));
            let position = choose|position: int| #![trigger facts@[position]] 0 <= position < facts@.len() && facts@[position] == key;
        }
    }
    let mut pages_work: HashMapWithView<i64, PageWork> = HashMapWithView::new();
    index = 0;
    while index < page_keys.len()
        invariant index <= page_keys.len(),
            pages_work@.dom() == page_keys@.take(index as int).to_set(),
            customer_phase(customers_work@, state.customers, db2),
            customer_keys@.no_duplicates(), customer_keys@.to_set() == customers_work@.dom(),
            page_keys@.no_duplicates(), page_keys@.to_set() == page_set@.dom(),
            state_ok(state.customers, state.pages, state.ranks, state.db()),
            forall|target: i64| #![trigger customers_work@[target]] customers_work@.dom().contains(target) ==> signed_customer_shape(customers_work@[target]),
            forall|target: i64| #![trigger pages_work@[target]] pages_work@.dom().contains(target) ==> work_page_shape(pages_work@[target], target as int),
            forall|key: i64, customer: i64, date: i32, priority: i64| #![trigger scratch_order_at(pages_work@, key, customer, date, priority)]
                page_keys@.take(index as int).contains((key as int / 256) as i64) ==>
                    scratch_order_at(pages_work@, key, customer, date, priority) ==
                        if fact_set@.dom().contains(key) { 0int } else { order_at(state.pages, key as int, customer as int, date as int, priority as int) },
            forall|customer: i64, key: i64| #![trigger reverse_overlay(customers_work@, state.customers, customer, key)]
                reverse_overlay(customers_work@, state.customers, customer, key) ==
                    if fact_set@.dom().contains(key) && page_keys@.take(index as int).contains((key as int / 256) as i64) { 0int }
                    else { reverse_at(state.customers, customer as int, key as int) },
            forall|key: i64, date: i32, kind: int| #![trigger scratch_ship_at(pages_work@, key, date, kind)]
                page_keys@.take(index as int).contains((key as int / 256) as i64) && 0 <= kind <= 2 ==>
                    scratch_ship_at(pages_work@, key, date, kind) ==
                        if fact_set@.dom().contains(key) { 0int } else { ship_at(state.pages, key as int, date as int, kind) },
        decreases page_keys.len() - index,
    {
        let page_key = page_keys[index];
        let ghost previous_pages = pages_work@;
        let ghost previous_customers = customers_work@;
        proof {
            verus_prooflib::lemma_prefix_fresh(page_keys@, index as int);
            assert(forall|customer: i64, key: i64| #![trigger reverse_overlay(previous_customers, state.customers, customer, key)]
                reverse_overlay(previous_customers, state.customers, customer, key) ==
                    if fact_set@.dom().contains(key) && page_keys@.take(index as int).contains((key as int / 256) as i64) { 0int }
                    else { reverse_at(state.customers, customer as int, key as int) });
        }
        let mut work = empty_page();
        if let Some(page) = state.pages.get(&Key::number(page_key)) {
            let mut row_index = 0;
            while row_index < page.orders.len()
                invariant row_index <= page.orders.len(), page_shape(*page, page_key as int, state.db()), work_page_shape(work, page_key as int),
                    customer_phase(customers_work@, state.customers, db2),
                    customer_keys@.no_duplicates(), customer_keys@.to_set() == customers_work@.dom(),
                    state_ok(state.customers, state.pages, state.ranks, state.db()),
                    forall|target: i64| #![trigger customers_work@[target]] customers_work@.dom().contains(target) ==> signed_customer_shape(customers_work@[target]),
                    forall|key: i64, customer: i64, date: i32, priority: i64| #![trigger order_count(work.orders@, key as int, customer as int, date as int, priority as int)]
                        order_count(work.orders@, key as int, customer as int, date as int, priority as int) ==
                            if fact_set@.dom().contains(key) { 0int } else { order_count((*page.orders)@.take(row_index as int), key as int, customer as int, date as int, priority as int) },
                    forall|customer: i64, key: i64| #![trigger reverse_overlay(customers_work@, state.customers, customer, key)]
                        reverse_overlay(customers_work@, state.customers, customer, key) ==
                            reverse_overlay(previous_customers, state.customers, customer, key)
                                - if fact_set@.dom().contains(key) { order_reverse_count((*page.orders)@.take(row_index as int), customer, key) } else { 0int },
                    forall|position: int, pending: int| #![trigger work.orders@[position], (*page.orders)@[pending]]
                        0 <= position < work.orders@.len() && row_index <= pending < (*page.orders)@.len() ==>
                            order_lt(work.orders@[position], (*page.orders)@[pending]),
                decreases page.orders.len() - row_index,
            {
                let ghost processed = row_index as int;
                let ghost before_orders = work.orders@;
                let ghost before_customers = customers_work@;
                let row = page.orders[row_index];
                if fact_set.contains_key(&row.key) {
                    let mut customer = take_customer(&mut customers_work, &mut customer_keys, &state.customers, row.customer);
                    let ghost previous_customer = customer;
                    if !reverse_change(&mut customer, row.key, -row.count) { return Err(2); }
                    customers_work.insert(row.customer, customer);
                    proof {
                        assert(customers_work@ =~= before_customers.insert(row.customer, customer));
                        lemma_customer_frame(before_customers, customers_work@, state.customers, db2, row.customer, customer);
                    }
                    proof {
                        assert forall|target: i64, key: i64| #![trigger reverse_overlay(customers_work@, state.customers, target, key)]
                            reverse_overlay(customers_work@, state.customers, target, key) == reverse_overlay(before_customers, state.customers, target, key)
                                - if target == row.customer && key == row.key { row.count as int } else { 0int } by {
                            if target == row.customer {
                                assert(reverse_count(previous_customer.orders@, key as int) == reverse_overlay(before_customers, state.customers, target, key));
                            }
                        }
                    }
                } else {
                    let ghost before = work.orders@;
                    proof {
                        assert forall|position: int| #![trigger before[position]] 0 <= position < before.len() implies order_lt(before[position], row) by {
                            assert(order_lt(work.orders@[position], (*page.orders)@[row_index as int]));
                        }
                        verus_prooflib::lemma_sorted_insert_at(before, before.len() as int, row, |left: OrderCell, right: OrderCell| order_lt(left, right));
                    }
                    work.orders.push(row);
                    proof {
                        assert(work.orders@ =~= before.insert(before.len() as int, row));
                        assert forall|position: int, pending: int| #![trigger work.orders@[position], (*page.orders)@[pending]]
                            0 <= position < work.orders@.len() && row_index + 1 <= pending < (*page.orders)@.len() implies
                                order_lt(work.orders@[position], (*page.orders)@[pending]) by {
                            if position == before.len() { assert(order_lt((*page.orders)@[row_index as int], (*page.orders)@[pending])); }
                            else { assert(order_lt(before[position], (*page.orders)@[pending])); }
                        }
                    }
                }
                proof {
                    assert forall|key: i64, customer: i64, date: i32, priority: i64| #![trigger order_count(work.orders@, key as int, customer as int, date as int, priority as int)]
                        order_count(work.orders@, key as int, customer as int, date as int, priority as int) ==
                            if fact_set@.dom().contains(key) { 0int } else { order_count((*page.orders)@.take(processed + 1), key as int, customer as int, date as int, priority as int) } by {
                        let weight = |entry: OrderCell| if entry.key == key as int && entry.customer == customer as int && entry.date == date as int && entry.priority == priority as int { entry.count as int } else { 0int };
                        assert(order_count(before_orders, key as int, customer as int, date as int, priority as int) ==
                            if fact_set@.dom().contains(key) { 0int } else { order_count((*page.orders)@.take(processed), key as int, customer as int, date as int, priority as int) });
                        lemma_sum_take((*page.orders)@, processed, weight);
                        if !fact_set@.dom().contains(row.key) { lemma_sum_push(before_orders, row, weight); }
                    }
                    assert forall|customer: i64, key: i64| #![trigger reverse_overlay(customers_work@, state.customers, customer, key)]
                        reverse_overlay(customers_work@, state.customers, customer, key) ==
                            reverse_overlay(previous_customers, state.customers, customer, key)
                                - if fact_set@.dom().contains(key) { order_reverse_count((*page.orders)@.take(processed + 1), customer, key) } else { 0int } by {
                        let weight = |entry: OrderCell| if entry.key == key && entry.customer == customer { entry.count as int } else { 0int };
                        lemma_sum_take((*page.orders)@, processed, weight);
                        assert(reverse_overlay(before_customers, state.customers, customer, key) ==
                            reverse_overlay(previous_customers, state.customers, customer, key)
                                - if fact_set@.dom().contains(key) { order_reverse_count((*page.orders)@.take(processed), customer, key) } else { 0int });
                    }
                }
                row_index += 1;
            }
            proof { assert((*page.orders)@.take(row_index as int) =~= (*page.orders)@); }
            let ghost completed_customers = customers_work@;
            row_index = 0;
            while row_index < page.ships.len()
                invariant row_index <= page.ships.len(), page_shape(*page, page_key as int, state.db()), work_page_shape(work, page_key as int),
                    customers_work@ == completed_customers,
                    forall|key: i64, customer: i64, date: i32, priority: i64| #![trigger order_count(work.orders@, key as int, customer as int, date as int, priority as int)]
                        order_count(work.orders@, key as int, customer as int, date as int, priority as int) ==
                            if fact_set@.dom().contains(key) { 0int } else { order_count((*page.orders)@, key as int, customer as int, date as int, priority as int) },
                    forall|key: i64, date: i32, kind: int| #![trigger ship_count(work.ships@, key as int, date as int, kind)]
                        0 <= kind <= 2 ==> ship_count(work.ships@, key as int, date as int, kind) ==
                            if fact_set@.dom().contains(key) { 0int } else { ship_count((*page.ships)@.take(row_index as int), key as int, date as int, kind) },
                    forall|position: int, pending: int| #![trigger work.ships@[position], (*page.ships)@[pending]]
                        0 <= position < work.ships@.len() && row_index <= pending < (*page.ships)@.len() ==>
                            ship_lt(work.ships@[position], (*page.ships)@[pending]),
                decreases page.ships.len() - row_index,
            {
                let ghost processed = row_index as int;
                let ghost before_ships = work.ships@;
                let row = page.ships[row_index];
                if !fact_set.contains_key(&row.key) {
                    let ghost before = work.ships@;
                    proof {
                        assert forall|position: int| #![trigger before[position]] 0 <= position < before.len() implies ship_lt(before[position], row) by {
                            assert(ship_lt(work.ships@[position], (*page.ships)@[row_index as int]));
                        }
                        verus_prooflib::lemma_sorted_insert_at(before, before.len() as int, row, |left: ShipCell, right: ShipCell| ship_lt(left, right));
                    }
                    work.ships.push(row);
                    proof {
                        assert(work.ships@ =~= before.insert(before.len() as int, row));
                        assert forall|position: int, pending: int| #![trigger work.ships@[position], (*page.ships)@[pending]]
                            0 <= position < work.ships@.len() && row_index + 1 <= pending < (*page.ships)@.len() implies
                                ship_lt(work.ships@[position], (*page.ships)@[pending]) by {
                            if position == before.len() { assert(ship_lt((*page.ships)@[row_index as int], (*page.ships)@[pending])); }
                            else { assert(ship_lt(before[position], (*page.ships)@[pending])); }
                        }
                    }
                }
                proof {
                    assert forall|key: i64, date: i32, kind: int| #![trigger ship_count(work.ships@, key as int, date as int, kind)]
                        0 <= kind <= 2 implies ship_count(work.ships@, key as int, date as int, kind) ==
                            if fact_set@.dom().contains(key) { 0int } else { ship_count((*page.ships)@.take(processed + 1), key as int, date as int, kind) } by {
                        let weight = |entry: ShipCell| if entry.key == key as int && entry.date == date as int {
                            if kind == 0 { entry.count as int } else if kind == 1 { entry.revenue as int } else { entry.positive as int }
                        } else { 0int };
                        assert(ship_count(before_ships, key as int, date as int, kind) ==
                            if fact_set@.dom().contains(key) { 0int } else { ship_count((*page.ships)@.take(processed), key as int, date as int, kind) });
                        lemma_sum_take((*page.ships)@, processed, weight);
                        if !fact_set@.dom().contains(row.key) { lemma_sum_push(before_ships, row, weight); }
                    }
                }
                row_index += 1;
            }
            proof {
                assert((*page.ships)@.take(row_index as int) =~= (*page.ships)@);
                assert forall|customer: i64, key: i64| #![trigger reverse_overlay(customers_work@, state.customers, customer, key)]
                    reverse_overlay(customers_work@, state.customers, customer, key) ==
                        if fact_set@.dom().contains(key) && (key as int / 256) as i64 == page_key { 0int }
                        else { reverse_overlay(previous_customers, state.customers, customer, key) } by {
                    crate::semantics::lemma_page_reverse(state.customers, state.pages, state.ranks, state.db(), &*page, page_key as int, customer, key);
                    assert(reverse_overlay(previous_customers, state.customers, customer, key) ==
                        if fact_set@.dom().contains(key) && page_keys@.take(index as int).contains((key as int / 256) as i64) { 0int }
                        else { reverse_at(state.customers, customer as int, key as int) });
                    assert(reverse_overlay(completed_customers, state.customers, customer, key) == reverse_overlay(previous_customers, state.customers, customer, key)
                        - if fact_set@.dom().contains(key) { order_reverse_count((*page.orders)@, customer, key) } else { 0int });
                }
            }
        } else {
            proof {
                assert forall|customer: i64, key: i64| #![trigger reverse_overlay(customers_work@, state.customers, customer, key)]
                    reverse_overlay(customers_work@, state.customers, customer, key) ==
                        if fact_set@.dom().contains(key) && (key as int / 256) as i64 == page_key { 0int }
                        else { reverse_overlay(previous_customers, state.customers, customer, key) } by {
                    if (key as int / 256) as i64 == page_key {
                        crate::semantics::lemma_missing_page_reverse(state.customers, state.pages, state.ranks, state.db(), page_key as int, customer, key);
                    }
                }
            }
        }
        pages_work.insert(page_key, work);
        proof {
            assert(page_keys@.take(index as int + 1) =~= page_keys@.take(index as int).push(page_key));
            verus_prooflib::lemma_push_to_set(page_keys@.take(index as int), page_key);
        }
        proof {
            assert(page_keys@.take(index as int + 1) =~= page_keys@.take(index as int).push(page_key));
            verus_prooflib::lemma_push_to_set(page_keys@.take(index as int), page_key);
            verus_prooflib::lemma_push_membership(page_keys@.take(index as int), page_key);
            assert forall|customer: i64, key: i64| #![trigger reverse_overlay(customers_work@, state.customers, customer, key)]
                reverse_overlay(customers_work@, state.customers, customer, key) ==
                    if fact_set@.dom().contains(key) && page_keys@.take(index as int + 1).contains((key as int / 256) as i64) { 0int }
                    else { reverse_at(state.customers, customer as int, key as int) } by {
                assert(reverse_overlay(customers_work@, state.customers, customer, key) ==
                    if fact_set@.dom().contains(key) && (key as int / 256) as i64 == page_key { 0int }
                    else { reverse_overlay(previous_customers, state.customers, customer, key) });
            }
            assert forall|key: i64, customer: i64, date: i32, priority: i64| #![trigger scratch_order_at(pages_work@, key, customer, date, priority)]
                page_keys@.take(index as int + 1).contains((key as int / 256) as i64) implies
                    scratch_order_at(pages_work@, key, customer, date, priority) ==
                        if fact_set@.dom().contains(key) { 0int } else { order_at(state.pages, key as int, customer as int, date as int, priority as int) } by {
                if (key as int / 256) as i64 != page_key {
                    assert(scratch_order_at(pages_work@, key, customer, date, priority) == scratch_order_at(previous_pages, key, customer, date, priority));
                }
            }
            assert forall|key: i64, date: i32, kind: int| #![trigger scratch_ship_at(pages_work@, key, date, kind)]
                page_keys@.take(index as int + 1).contains((key as int / 256) as i64) && 0 <= kind <= 2 implies
                    scratch_ship_at(pages_work@, key, date, kind) ==
                        if fact_set@.dom().contains(key) { 0int } else { ship_at(state.pages, key as int, date as int, kind) } by {
                if (key as int / 256) as i64 != page_key {
                    assert(scratch_ship_at(pages_work@, key, date, kind) == scratch_ship_at(previous_pages, key, date, kind));
                }
            }
        }
        index += 1;
    }
    proof {
        assert(page_keys@.take(index as int) =~= page_keys@);
    }
    index = 0;
    while index < order_rows.len()
        invariant
            index <= order_rows.len(),
            pages_work@.dom() == page_set@.dom(),
            customer_phase(customers_work@, state.customers, db2),
            customer_keys@.no_duplicates(), customer_keys@.to_set() == customers_work@.dom(),
            state_ok(state.customers, state.pages, state.ranks, state.db()),
            forall|target: i64| #![trigger customers_work@[target]] customers_work@.dom().contains(target) ==> signed_customer_shape(customers_work@[target]),
            forall|target: i64| #![trigger pages_work@[target]] pages_work@.dom().contains(target) ==> work_page_shape(pages_work@[target], target as int),
            forall|key: i64, customer: i64, date: i32, priority: i64| #![trigger scratch_order_at(pages_work@, key, customer, date, priority)]
                page_set@.dom().contains(((key as int / 256) as i64)) ==>
                    scratch_order_at(pages_work@, key, customer, date, priority) ==
                        if fact_set@.dom().contains(key) { order_prefix(order_rows@.take(index as int), key, customer, date, priority) }
                        else { order_at(state.pages, key as int, customer as int, date as int, priority as int) },
            forall|customer: i64, key: i64| #![trigger reverse_overlay(customers_work@, state.customers, customer, key)]
                reverse_overlay(customers_work@, state.customers, customer, key) ==
                    if fact_set@.dom().contains(key) { reverse_prefix(order_rows@.take(index as int), customer, key) }
                    else { reverse_at(state.customers, customer as int, key as int) },
            forall|key: i64, date: i32, kind: int| #![trigger scratch_ship_at(pages_work@, key, date, kind)]
                page_set@.dom().contains((key as int / 256) as i64) && 0 <= kind <= 2 ==>
                    scratch_ship_at(pages_work@, key, date, kind) ==
                        if fact_set@.dom().contains(key) { 0int } else { ship_at(state.pages, key as int, date as int, kind) },
        decreases order_rows.len() - index,
    {
        let ghost processed = index as int;
        let ghost previous_pages = pages_work@;
        let ghost previous_customers = customers_work@;
        let row = order_rows[index];
        if fact_set.contains_key(&row.o_orderkey) {
            let key = page_id(row.o_orderkey);
            let mut page = match pages_work.remove(&key) { Some(page) => page, None => return Err(1) };
            let ghost previous_page = page;
            if !insert_order(&mut page.orders, row) { return Err(2); }
            pages_work.insert(key, page);
            proof {
                assert(pages_work@ == previous_pages.insert(key, page));
                assert forall|target: i64, date: i32, kind: int| #![trigger scratch_ship_at(pages_work@, target, date, kind)]
                    scratch_ship_at(pages_work@, target, date, kind) == scratch_ship_at(previous_pages, target, date, kind) by {
                    if (target as int / 256) as i64 == key {
                        assert(page.ships@ == previous_page.ships@);
                        assert(ship_count(previous_page.ships@, target as int, date as int, kind) == scratch_ship_at(previous_pages, target, date, kind));
                    }
                }
                assert forall|target: i64, customer: i64, date: i32, priority: i64| #![trigger scratch_order_at(pages_work@, target, customer, date, priority)]
                    scratch_order_at(pages_work@, target, customer, date, priority) == scratch_order_at(previous_pages, target, customer, date, priority)
                        + if target == row.o_orderkey && customer == row.o_custkey && date == row.o_orderdate && priority == row.o_shippriority { 1int } else { 0int } by {
                    if (target as int / 256) as i64 == key {
                        assert(order_count(previous_page.orders@, target as int, customer as int, date as int, priority as int) == scratch_order_at(previous_pages, target, customer, date, priority));
                        assert(order_count(page.orders@, target as int, customer as int, date as int, priority as int) == order_count(previous_page.orders@, target as int, customer as int, date as int, priority as int)
                            + if target == row.o_orderkey && customer == row.o_custkey && date == row.o_orderdate && priority == row.o_shippriority { 1int } else { 0int });
                    }
                }
            }
            let mut customer = take_customer(&mut customers_work, &mut customer_keys, &state.customers, row.o_custkey);
            let ghost previous_customer = customer;
            if !reverse_change(&mut customer, row.o_orderkey, 1) { return Err(2); }
            customers_work.insert(row.o_custkey, customer);
            proof {
                assert(customers_work@ =~= previous_customers.insert(row.o_custkey, customer));
                lemma_customer_frame(previous_customers, customers_work@, state.customers, db2, row.o_custkey, customer);
            }
            proof {
                assert(customers_work@ == previous_customers.insert(row.o_custkey, customer));
                assert forall|target: i64, orderkey: i64| #![trigger reverse_overlay(customers_work@, state.customers, target, orderkey)]
                    reverse_overlay(customers_work@, state.customers, target, orderkey) == reverse_overlay(previous_customers, state.customers, target, orderkey)
                        + if target == row.o_custkey && orderkey == row.o_orderkey { 1int } else { 0int } by {
                    if target == row.o_custkey {
                        assert(reverse_count(previous_customer.orders@, orderkey as int) == reverse_overlay(previous_customers, state.customers, target, orderkey));
                        assert(reverse_count(customer.orders@, orderkey as int) == reverse_count(previous_customer.orders@, orderkey as int)
                            + if orderkey == row.o_orderkey { 1int } else { 0int });
                    }
                }
            }
        }
        proof {
            assert forall|key: i64, customer: i64, date: i32, priority: i64| #![trigger scratch_order_at(pages_work@, key, customer, date, priority)]
                page_set@.dom().contains(((key as int / 256) as i64)) implies
                    scratch_order_at(pages_work@, key, customer, date, priority) ==
                        if fact_set@.dom().contains(key) { order_prefix(order_rows@.take(processed + 1), key, customer, date, priority) }
                        else { order_at(state.pages, key as int, customer as int, date as int, priority as int) } by {
                let weight = |entry: OrderInput| if entry.o_orderkey == key && entry.o_custkey == customer && entry.o_orderdate == date && entry.o_shippriority == priority { 1int } else { 0int };
                lemma_sum_take(order_rows@, processed, weight);
                assert(scratch_order_at(previous_pages, key, customer, date, priority) ==
                    if fact_set@.dom().contains(key) { order_prefix(order_rows@.take(processed), key, customer, date, priority) }
                    else { order_at(state.pages, key as int, customer as int, date as int, priority as int) });
            }
            assert forall|customer: i64, key: i64| #![trigger reverse_overlay(customers_work@, state.customers, customer, key)]
                reverse_overlay(customers_work@, state.customers, customer, key) ==
                    if fact_set@.dom().contains(key) { reverse_prefix(order_rows@.take(processed + 1), customer, key) }
                    else { reverse_at(state.customers, customer as int, key as int) } by {
                let weight = |entry: OrderInput| if entry.o_custkey == customer && entry.o_orderkey == key { 1int } else { 0int };
                lemma_sum_take(order_rows@, processed, weight);
                assert(reverse_overlay(previous_customers, state.customers, customer, key) ==
                    if fact_set@.dom().contains(key) { reverse_prefix(order_rows@.take(processed), customer, key) }
                    else { reverse_at(state.customers, customer as int, key as int) });
            }
        }
        index += 1;
    }
    proof { assert(order_rows@.take(index as int) =~= order_rows@); }
    index = 0;
    while index < ship_rows.len()
        invariant
            index <= ship_rows.len(),
            pages_work@.dom() == page_set@.dom(),
            customer_phase(customers_work@, state.customers, db2),
            customer_keys@.no_duplicates(), customer_keys@.to_set() == customers_work@.dom(),
            forall|key: i64, customer: i64, date: i32, priority: i64| #![trigger scratch_order_at(pages_work@, key, customer, date, priority)]
                page_set@.dom().contains((key as int / 256) as i64) ==>
                    scratch_order_at(pages_work@, key, customer, date, priority) ==
                        if fact_set@.dom().contains(key) { order_prefix(order_rows@, key, customer, date, priority) }
                        else { order_at(state.pages, key as int, customer as int, date as int, priority as int) },
            forall|customer: i64, key: i64| #![trigger reverse_overlay(customers_work@, state.customers, customer, key)]
                reverse_overlay(customers_work@, state.customers, customer, key) ==
                    if fact_set@.dom().contains(key) { reverse_prefix(order_rows@, customer, key) }
                    else { reverse_at(state.customers, customer as int, key as int) },
            state_ok(state.customers, state.pages, state.ranks, state.db()),
            forall|target: i64| #![trigger customers_work@[target]] customers_work@.dom().contains(target) ==> signed_customer_shape(customers_work@[target]),
            forall|target: i64| #![trigger pages_work@[target]] pages_work@.dom().contains(target) ==> work_page_shape(pages_work@[target], target as int),
            forall|key: i64, date: i32, kind: int| #![trigger scratch_ship_at(pages_work@, key, date, kind)]
                page_set@.dom().contains(((key as int / 256) as i64)) && 0 <= kind <= 2 ==>
                    scratch_ship_at(pages_work@, key, date, kind) ==
                        if fact_set@.dom().contains(key) { ship_prefix(ship_rows@.take(index as int), key, date, kind) }
                        else { ship_at(state.pages, key as int, date as int, kind) },
        decreases ship_rows.len() - index,
    {
        let ghost processed = index as int;
        let ghost previous_pages = pages_work@;
        let row = ship_rows[index];
        if fact_set.contains_key(&row.l_orderkey) {
            let key = page_id(row.l_orderkey);
            let mut page = match pages_work.remove(&key) { Some(page) => page, None => return Err(1) };
            let ghost previous_page = page;
            if !insert_ship(&mut page.ships, row) { return Err(2); }
            pages_work.insert(key, page);
            proof {
                assert(pages_work@ == previous_pages.insert(key, page));
                assert forall|target: i64, customer: i64, date: i32, priority: i64| #![trigger scratch_order_at(pages_work@, target, customer, date, priority)]
                    scratch_order_at(pages_work@, target, customer, date, priority) == scratch_order_at(previous_pages, target, customer, date, priority) by {
                    if (target as int / 256) as i64 == key {
                        assert(page.orders@ == previous_page.orders@);
                        assert(order_count(previous_page.orders@, target as int, customer as int, date as int, priority as int) == scratch_order_at(previous_pages, target, customer, date, priority));
                    }
                }
                assert forall|target: i64, date: i32, kind: int| #![trigger scratch_ship_at(pages_work@, target, date, kind)]
                    0 <= kind <= 2 implies scratch_ship_at(pages_work@, target, date, kind) == scratch_ship_at(previous_pages, target, date, kind)
                        + if target == row.l_orderkey && date == row.l_shipdate {
                            let value = row.l_extendedprice as int * (100 - row.l_discount as int);
                            if kind == 0 { 1int } else if kind == 1 { value } else { if value > 0 { value } else { 0int } }
                        } else { 0int } by {
                    if (target as int / 256) as i64 == key {
                        assert(ship_count(previous_page.ships@, target as int, date as int, kind) == scratch_ship_at(previous_pages, target, date, kind));
                        assert(ship_count(page.ships@, target as int, date as int, kind) == ship_count(previous_page.ships@, target as int, date as int, kind)
                            + if target == row.l_orderkey && date == row.l_shipdate {
                                let value = row.l_extendedprice as int * (100 - row.l_discount as int);
                                if kind == 0 { 1int } else if kind == 1 { value } else { if value > 0 { value } else { 0int } }
                            } else { 0int });
                    }
                }
            }
        }
        proof {
            assert forall|key: i64, date: i32, kind: int| #![trigger scratch_ship_at(pages_work@, key, date, kind)]
                page_set@.dom().contains(((key as int / 256) as i64)) && 0 <= kind <= 2 implies
                    scratch_ship_at(pages_work@, key, date, kind) ==
                        if fact_set@.dom().contains(key) { ship_prefix(ship_rows@.take(processed + 1), key, date, kind) }
                        else { ship_at(state.pages, key as int, date as int, kind) } by {
                let weight = |entry: ShipmentInput| if entry.l_orderkey == key && entry.l_shipdate == date {
                    let value = entry.l_extendedprice as int * (100 - entry.l_discount as int);
                    if kind == 0 { 1int } else if kind == 1 { value } else { if value > 0 { value } else { 0int } }
                } else { 0int };
                lemma_sum_take(ship_rows@, processed, weight);
                assert(scratch_ship_at(previous_pages, key, date, kind) ==
                    if fact_set@.dom().contains(key) { ship_prefix(ship_rows@.take(processed), key, date, kind) }
                    else { ship_at(state.pages, key as int, date as int, kind) });
            }
        }
        index += 1;
    }
    let ghost completed_customers = customers_work@;
    let ghost completed_pages = pages_work@;
    proof {
        assert(ship_rows@.take(index as int) =~= ship_rows@);
        assert forall|customer: i64, key: i64| #![trigger reverse_overlay(completed_customers, state.customers, customer, key)]
            reverse_overlay(completed_customers, state.customers, customer, key) == reverse_sum(db2, customer as int, key as int) by {
            if fact_set@.dom().contains(key) {
                crate::input_proofs::lemma_order_rescan(db2, orders_keys@, order_rows@, customer, key, 0, 0);
            } else {
                crate::maintenance_proofs::lemma_fact_locality(state.db(), db2, customer_delta_seq_view(customer_delta@),
                    lineitem_full, orders_full, key as int, customer as int, 0, 0, 0);
            }
        }
        assert forall|key: i64, customer: i64, date: i32, priority: i64| #![trigger scratch_order_at(completed_pages, key, customer, date, priority)]
            page_set@.dom().contains((key as int / 256) as i64) implies
                scratch_order_at(completed_pages, key, customer, date, priority) == order_sum(db2, key as int, customer as int, date as int, priority as int) by {
            if fact_set@.dom().contains(key) {
                crate::input_proofs::lemma_order_rescan(db2, orders_keys@, order_rows@, customer, key, date, priority);
            } else {
                crate::maintenance_proofs::lemma_fact_locality(state.db(), db2, customer_delta_seq_view(customer_delta@),
                    lineitem_full, orders_full, key as int, customer as int, date as int, priority as int, 0);
            }
        }
        assert forall|key: i64, date: i32, kind: int| #![trigger scratch_ship_at(completed_pages, key, date, kind)]
            page_set@.dom().contains((key as int / 256) as i64) && 0 <= kind <= 2 implies
                scratch_ship_at(completed_pages, key, date, kind) == ship_sum(db2, key as int, date as int, kind) by {
            if fact_set@.dom().contains(key) {
                crate::maintenance_proofs::lemma_ship_rescan(db2, ships_keys@, ship_rows@, key, date, kind);
            } else {
                crate::maintenance_proofs::lemma_fact_locality(state.db(), db2, customer_delta_seq_view(customer_delta@),
                    lineitem_full, orders_full, key as int, 0, date as int, 0, kind);
            }
        }
        assert forall|number: i64| #![trigger completed_pages[number]] completed_pages.contains_key(number) implies work_database_ok(completed_pages[number], number as int, db2) by {
            let work = completed_pages[number];
            assert forall|key: i64, customer: i64, date: i32, priority: i64| #![trigger order_count(work.orders@, key as int, customer as int, date as int, priority as int)]
                key as int / 256 == number implies order_count(work.orders@, key as int, customer as int, date as int, priority as int)
                    == order_sum(db2, key as int, customer as int, date as int, priority as int) by {
                assert(scratch_order_at(completed_pages, key, customer, date, priority) == order_sum(db2, key as int, customer as int, date as int, priority as int));
            }
            assert forall|key: i64, date: i32, kind: int| #![trigger ship_count(work.ships@, key as int, date as int, kind)]
                key as int / 256 == number && 0 <= kind <= 2 implies ship_count(work.ships@, key as int, date as int, kind) == ship_sum(db2, key as int, date as int, kind) by {
                assert(scratch_ship_at(completed_pages, key, date, kind) == ship_sum(db2, key as int, date as int, kind));
            }
            crate::maintenance_proofs::lemma_work_domains(work, number as int, db2);
        }
    }
    let customers = match publish_changes(customers_work, &customer_keys, &state.customers) {
        Ok(customers) => customers, Err(error) => return Err(error),
    };
    proof {
        assert forall|customer: int, segment: Seq<u8>| #![trigger customer_at(customers, customer, segment)]
            customer_at(customers, customer, segment) == customer_sum(db2, customer, segment) by {
            if i64::MIN <= customer <= i64::MAX {
                assert(customer_at(customers, customer, segment) == customer_overlay(completed_customers, state.customers, customer as i64, segment));
                assert(customer_overlay(completed_customers, state.customers, customer as i64, segment) == customer_sum(db2, customer, segment));
            } else {
                assert(customers.at((customer, Seq::<u8>::empty())) == state.customers.at((customer, Seq::<u8>::empty())));
                assert(customer_at(customers, customer, segment) == customer_at(state.customers, customer, segment));
                lemma_sum_zero(db2.customer, |row: CustomerRow| if row.c_custkey == customer && row.c_mktsegment == segment { 1int } else { 0int });
                lemma_sum_zero(state.db().customer, |row: CustomerRow| if row.c_custkey == customer && row.c_mktsegment == segment { 1int } else { 0int });
            }
        }
        assert forall|customer: int, key: int| #![trigger reverse_at(customers, customer, key)]
            reverse_at(customers, customer, key) == reverse_sum(db2, customer, key) by {
            if i64::MIN <= customer <= i64::MAX && i64::MIN <= key <= i64::MAX {
                assert(reverse_at(customers, customer, key) == reverse_overlay(completed_customers, state.customers, customer as i64, key as i64));
                assert(reverse_overlay(completed_customers, state.customers, customer as i64, key as i64) == reverse_sum(db2, customer, key));
            } else {
                lemma_sum_zero(db2.orders, |row: OrdersRow| if row.o_custkey == customer && row.o_orderkey == key { 1int } else { 0int });
                if customer < i64::MIN || customer > i64::MAX {
                    assert(customers.at((customer, Seq::<u8>::empty())) == state.customers.at((customer, Seq::<u8>::empty())));
                    assert(reverse_at(customers, customer, key) == reverse_at(state.customers, customer, key));
                    lemma_sum_zero(state.db().orders, |row: OrdersRow| if row.o_custkey == customer && row.o_orderkey == key { 1int } else { 0int });
                } else if customers.at((customer, Seq::empty())).is_some() {
                    let cell = customers.at((customer, Seq::empty())).unwrap();
                    lemma_sum_zero(cell.orders@, |entry: (i64, i64)| if entry.0 == key { entry.1 as int } else { 0int });
                }
            }
        }
        assert(customer_database_ok(customers, db2));
    }
    let ghost fact_domain = fact_set@.dom();
    proof {
        assert forall|position: int| #![trigger orders_full[position]] 0 <= position < orders_full.len() implies
            completed_pages.contains_key((orders_full[position].0.o_orderkey / 256) as i64) by {
            assert(fact_domain.contains(orders_full[position].0.o_orderkey as i64));
            assert(page_set@.dom().contains((orders_full[position].0.o_orderkey / 256) as i64));
        }
        assert forall|position: int| #![trigger lineitem_full[position]] 0 <= position < lineitem_full.len() implies
            completed_pages.contains_key((lineitem_full[position].0.l_orderkey / 256) as i64) by {
            assert(fact_domain.contains(lineitem_full[position].0.l_orderkey as i64));
            assert(page_set@.dom().contains((lineitem_full[position].0.l_orderkey / 256) as i64));
        }
    }
    let mut affected = fact_set;
    let mut affected_keys = facts;
    index = 0;
    while index < changed_customer_keys.len()
        invariant index <= changed_customer_keys.len(), affected_keys@.no_duplicates(), affected_keys@.to_set() == affected@.dom(),
            fact_domain.subset_of(affected@.dom()),
            customer_database_ok(customers, db2),
            forall|customer: i64, key: i64| #![trigger reverse_at(customers, customer as int, key as int)]
                changed_customer_keys@.take(index as int).contains(customer) && reverse_at(customers, customer as int, key as int) > 0 ==> affected@.contains_key(key),
        decreases changed_customer_keys.len() - index,
    {
        let customer_key = changed_customer_keys[index];
        if let Some(customer) = customers.get(&Key::number(customer_key)) {
            let mut order_index = 0;
            while order_index < customer.orders.len()
                invariant order_index <= customer.orders.len(), affected_keys@.no_duplicates(), affected_keys@.to_set() == affected@.dom(),
                    fact_domain.subset_of(affected@.dom()),
                    customer_database_ok(customers, db2), customers.at((customer_key as int, Seq::empty())) == Some(*customer),
                    forall|position: int| #![trigger customer.orders@[position]] 0 <= position < order_index ==> affected@.contains_key(customer.orders@[position].0),
                    forall|target: i64, key: i64| #![trigger reverse_at(customers, target as int, key as int)]
                        changed_customer_keys@.take(index as int).contains(target) && reverse_at(customers, target as int, key as int) > 0 ==> affected@.contains_key(key),
                decreases customer.orders.len() - order_index,
            {
                add_key(&mut affected, &mut affected_keys, customer.orders[order_index].0);
                order_index += 1;
            }
            proof {
                assert forall|key: i64| #![trigger reverse_at(customers, customer_key as int, key as int)]
                    reverse_at(customers, customer_key as int, key as int) > 0 implies affected@.contains_key(key) by {
                    let rows = customer.orders@;
                    let weight = |entry: (i64, i64)| if entry.0 == key as int { entry.1 as int } else { 0int };
                    let position = lemma_sum_nonzero_witness(rows, weight);
                    assert(rows[position].0 == key);
                }
            }
        }
        proof {
            assert(changed_customer_keys@.take(index as int + 1) =~= changed_customer_keys@.take(index as int).push(customer_key));
            verus_prooflib::lemma_push_membership(changed_customer_keys@.take(index as int), customer_key);
        }
        index += 1;
    }
    proof { assert(changed_customer_keys@.take(index as int) =~= changed_customer_keys@); }
    let mut selected_pages: HashMapWithView<i64, ()> = HashMapWithView::new();
    let mut selected_page_keys = Vec::new();
    index = 0;
    while index < affected_keys.len()
        invariant index <= affected_keys.len(), selected_page_keys@.no_duplicates(), selected_page_keys@.to_set() == selected_pages@.dom(),
            forall|position: int| #![trigger affected_keys@[position]] 0 <= position < index ==>
                selected_pages@.contains_key((affected_keys@[position] as int / 256) as i64),
        decreases affected_keys.len() - index,
    {
        add_key(&mut selected_pages, &mut selected_page_keys, page_id(affected_keys[index]));
        index += 1;
    }
    let ghost selected = crate::maintenance_proofs::page_numbers(selected_page_keys@);
    proof {
        assert forall|position: int| #![trigger orders_full[position]] 0 <= position < orders_full.len() implies selected.contains(orders_full[position].0.o_orderkey / 256) by {
            let key = orders_full[position].0.o_orderkey as i64;
            assert(fact_domain.contains(key));
            assert(affected_keys@.contains(key));
            let source = choose|source: int| #![trigger affected_keys@[source]] 0 <= source < affected_keys@.len() && affected_keys@[source] == key;
            assert(selected_page_keys@.contains((key as int / 256) as i64));
            crate::maintenance_proofs::lemma_page_numbers(selected_page_keys@, key as int / 256);
        }
        assert forall|position: int| #![trigger lineitem_full[position]] 0 <= position < lineitem_full.len() implies selected.contains(lineitem_full[position].0.l_orderkey / 256) by {
            let key = lineitem_full[position].0.l_orderkey as i64;
            assert(fact_domain.contains(key));
            assert(affected_keys@.contains(key));
            let source = choose|source: int| #![trigger affected_keys@[source]] 0 <= source < affected_keys@.len() && affected_keys@[source] == key;
            assert(selected_page_keys@.contains((key as int / 256) as i64));
            crate::maintenance_proofs::lemma_page_numbers(selected_page_keys@, key as int / 256);
        }
        assert forall|position: int, key: i64| #![trigger customer_delta@[position], reverse_sum(db2, customer_delta@[position].row.c_custkey as int, key as int)]
            0 <= position < customer_delta@.len() && reverse_sum(db2, customer_delta@[position].row.c_custkey as int, key as int) > 0 implies selected.contains(key as int / 256) by {
            let customer = customer_delta@[position].row.c_custkey;
            broadcast use vstd::seq_lib::group_seq_properties;
            let projected = customer_delta@.map_values(|delta: CustomerDelta| delta.row.c_custkey);
            assert(projected[position] == customer);
            assert(projected.contains(customer));
            assert(changed_customer_keys@.to_set() == projected.to_set());
            projected.to_set_ensures();
            changed_customer_keys@.to_set_ensures();
            assert(projected.to_set().contains(customer));
            assert(changed_customer_keys@.to_set().contains(customer));
            assert(changed_customer_keys@.contains(customer));
            assert(reverse_at(customers, customer as int, key as int) == reverse_sum(db2, customer as int, key as int));
            assert(affected_keys@.contains(key));
            let source = choose|source: int| #![trigger affected_keys@[source]] 0 <= source < affected_keys@.len() && affected_keys@[source] == key;
            assert(selected_page_keys@.contains((key as int / 256) as i64));
            crate::maintenance_proofs::lemma_page_numbers(selected_page_keys@, key as int / 256);
        }
        crate::maintenance_proofs::lemma_initial_pages(state.customers, state.pages, state.ranks, state.db(), db2,
            customer_delta@, lineitem_full, orders_full, selected);
        crate::maintenance_ranks::lemma_rank_exact(state.pages, state.ranks);
        assert forall|target: verus_prooflib::ordered::KeyView| #![trigger state.pages.at(target)] state.pages.at(target).is_some() implies {
            &&& target.1.len() == 0
            &&& forall|position: int| #![trigger (*state.pages.at(target).unwrap().groups)@[position]]
                0 <= position < (*state.pages.at(target).unwrap().groups)@.len() ==>
                    (*state.pages.at(target).unwrap().groups)@[position].id.key as int / 256 == target.0
                        && (*state.pages.at(target).unwrap().groups)@[position].bound >= 0
        } by {
            crate::semantics::lemma_page_bounds(&state.pages.at(target).unwrap(), state.db(), target.0);
        }
    }
    let mut pages = state.pages.share();
    let mut ranks = state.ranks.share();
    proof {
        assert forall|target: verus_prooflib::ordered::KeyView| #![trigger pages.at(target)] pages.at(target) == state.pages.at(target) by { assert(pages.tree() == state.pages.tree()); }
        assert forall|outer: verus_prooflib::ordered::KeyView, rank: verus_prooflib::ordered::KeyView| #![trigger rank_at(ranks, outer, rank)]
            rank_at(ranks, outer, rank) == rank_at(state.ranks, outer, rank) by { assert(ranks.tree() == state.ranks.tree()); }
        assert forall|outer: verus_prooflib::ordered::KeyView| #![trigger ranks.at(outer)] ranks.at(outer) == state.ranks.at(outer) by { assert(ranks.tree() == state.ranks.tree()); }
        crate::maintenance_ranks::lemma_rank_share(state.pages, state.ranks, pages, ranks);
        crate::maintenance_proofs::lemma_partial_share(state.pages, pages, state.db(), db2, selected);
        assert(selected_page_keys@.skip(0) =~= selected_page_keys@);
    }
    index = 0;
    while index < selected_page_keys.len()
        invariant index <= selected_page_keys.len(), selected_page_keys@.no_duplicates(),
            state_ok(state.customers, state.pages, state.ranks, state.db()), customer_database_ok(customers, db2), database_domains(db2),
            capture_ok(state.db(), db2, customer_delta_seq_view(customer_delta@), lineitem_full, orders_full),
            crate::maintenance_proofs::page_partial(pages, state.db(), db2, crate::maintenance_proofs::page_numbers(selected_page_keys@.skip(index as int))),
            crate::maintenance_ranks::rank_exact(pages, ranks), crate::maintenance_ranks::page_rank_shape(pages),
            pages_work@ == completed_pages,
            forall|number: i64| #![trigger completed_pages[number]] completed_pages.contains_key(number) ==> work_database_ok(completed_pages[number], number as int, db2),
            forall|position: int| #![trigger orders_full[position]] 0 <= position < orders_full.len() ==>
                i64::MIN <= orders_full[position].0.o_orderkey <= i64::MAX && completed_pages.contains_key((orders_full[position].0.o_orderkey / 256) as i64),
            forall|position: int| #![trigger lineitem_full[position]] 0 <= position < lineitem_full.len() ==>
                i64::MIN <= lineitem_full[position].0.l_orderkey <= i64::MAX && completed_pages.contains_key((lineitem_full[position].0.l_orderkey / 256) as i64),
        decreases selected_page_keys.len() - index,
    {
        let key = selected_page_keys[index];
        let old_page = state.pages.get(&Key::number(key));
        let work = match pages_work.get(&key) {
            Some(work) => PageWork { orders: copy_orders(&work.orders), ships: copy_ships(&work.ships) },
            None => match old_page {
                Some(page) => PageWork { orders: copy_orders(&page.orders), ships: copy_ships(&page.ships) },
                None => empty_page(),
            },
        };
        proof {
            if completed_pages.contains_key(key) {
                assert(work.orders@ == completed_pages[key].orders@ && work.ships@ == completed_pages[key].ships@);
                assert(work_database_ok(work, key as int, db2));
            } else {
                assert(work_page_shape(work, key as int));
                crate::maintenance_proofs::lemma_state_work(state.customers, state.pages, state.ranks, state.db(), key as int, work);
                crate::maintenance_proofs::lemma_columns_frame(work, key as int, state.db(), db2,
                    customer_delta_seq_view(customer_delta@), lineitem_full, orders_full);
            }
        }
        let groups = match crate::construction::make_groups(&work, &customers) { Some(groups) => groups, None => return Err(2) };
        let ghost completed_work = work;
        let page = Page { orders: Arc::new(work.orders), ships: Arc::new(work.ships), groups: Arc::new(groups) };
        proof { crate::semantics::lemma_page_publication(page, completed_work, key as int, customers, db2); }
        let ghost previous = pages;
        let ghost pending = crate::maintenance_proofs::page_numbers(selected_page_keys@.skip(index as int));
        crate::maintenance_ranks::publish_page(&mut pages, &mut ranks, key, page);
        proof {
            crate::maintenance_proofs::lemma_page_numbers(selected_page_keys@.skip(index as int), key as int);
            assert(selected_page_keys@.skip(index as int)[0] == key);
            crate::maintenance_proofs::lemma_partial_step(previous, pages, state.db(), db2, pending, key, page);
            crate::maintenance_proofs::lemma_page_tail(selected_page_keys@, index as int);
        }
        index += 1;
    }
    proof {
        assert(selected_page_keys@.skip(index as int) =~= Seq::<i64>::empty());
        assert(crate::maintenance_proofs::page_numbers(selected_page_keys@.skip(index as int)) =~= Set::<int>::empty());
        crate::maintenance_ranks::lemma_rank_finish(pages, ranks, db2);
        assert(state_ok(customers, pages, ranks, db2));
    }
    Ok(State::from_parts(customers, pages, ranks, Ghost(db2)))
}

}

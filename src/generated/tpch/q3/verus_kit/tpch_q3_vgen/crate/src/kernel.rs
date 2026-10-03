#![allow(unused_imports)]
use vstd::prelude::*;
use std::sync::Arc;
use crate::gen_spec::*;
use crate::gen_obligations::*;
use crate::maps::*;
use crate::joins::*;
use crate::joins::add;
use crate::profiles::*;
use crate::segments::*;
use crate::output::*;
use crate::query_proofs::*;
use crate::database_proofs::*;
use verus_common::sums::*;

verus! {

broadcast use vstd::seq_lib::group_seq_properties;

pub struct State {
    database: Ghost<Db>,
    customers: Index<Counts>,
    joins: JoinIndex,
    shipments: Index<Vec<Bucket>>,
    names: Index<Vec<Name>>,
    texts: Index<Vec<u8>>,
    next_segment: i64,
}

impl State {
    pub closed spec fn db(self) -> Db { self.database@ }

    pub(crate) closed spec fn c_at(self, key: i64, segment: i64) -> int { c_index(self.customers, key, segment) }

    #[verifier::type_invariant]
    pub closed spec fn invariant(self) -> bool {
        &&& self.customers.valid() && self.shipments.valid()
        &&& join_index_ok(self.joins)
        &&& dictionary_ok(self.names, self.texts, self.next_segment)
        &&& forall|key: i64| (#[trigger] self.customers.at(key)) matches Some(rows) ==>
            counts_ok(rows@)
        &&& forall|key: i64, segment: i64| #[trigger] self.c_at(key, segment) ==
            match self.texts.at(segment) {
                Some(text) => customer_count(self.db(), key as int, text@),
                None => 0,
            }
        &&& forall|position: int| 0 <= position < self.db().customer.len() ==>
            has_text(self.texts, (#[trigger] self.db().customer[position]).c_mktsegment)
            && i64::MIN <= self.db().customer[position].c_custkey <= i64::MAX
        &&& forall|position: int| 0 <= position < self.db().orders.len() ==>
            i64::MIN <= (#[trigger] self.db().orders[position]).o_orderkey <= i64::MAX
            && i64::MIN <= self.db().orders[position].o_custkey <= i64::MAX
            && i64::MIN <= self.db().orders[position].o_shippriority <= i64::MAX
            && i32::MIN <= self.db().orders[position].o_orderdate <= i32::MAX
        &&& forall|position: int| 0 <= position < self.db().lineitem.len() ==>
            i64::MIN <= (#[trigger] self.db().lineitem[position]).l_orderkey <= i64::MAX
            && i32::MIN <= self.db().lineitem[position].l_shipdate <= i32::MAX
        &&& forall|segment: i64, date: i32, key: i64, priority: i64|
            #[trigger] h_at(self.joins, segment, date, key, priority) ==
            match self.texts.at(segment) {
                Some(text) => join_count(self.db(), text@, date as int, key as int, priority as int),
                None => 0,
            }
        &&& forall|segment: i64| (#[trigger] self.joins.at(segment)).is_some() ==> self.texts.at(segment).is_some()
        &&& forall|key: i64| (#[trigger] self.shipments.at(key)) matches Some(rows) ==>
            rows.len() > 0 && profile_ok(self.db(), key, rows@)
        &&& forall|key: i64| #[trigger] line_count(self.db(), key as int, i32::MIN as int) > 0
            <==> self.shipments.at(key).is_some()
    }
}

pub fn finish_customers(base: &Index<Counts>, mut changes: Work<Counts>) -> (result: Option<Index<Counts>>)
    requires counts_work_ok(changes),
        forall|key: i64| (#[trigger] base.at(key)) matches Some(rows) ==> counts_ok(rows@),
    ensures
        result matches Some(index) ==> index.valid(),
        result matches Some(index) ==> forall|key: i64, segment: i64| #[trigger] c_index(index, key, segment) ==
            c_index(*base, key, segment) + work_count(changes, key, segment),
        result matches Some(index) ==> forall|key: i64| (#[trigger] index.at(key)) matches Some(rows) ==> counts_ok(rows@),
{
    let ghost original = changes;
    let mut delta = Run::new();
    let mut position = 0;
    while position < changes.keys.len()
        invariant position <= changes.keys.len(), run_ok(delta), counts_work_ok(original),
            changes.keys@ == original.keys@, work_directory(changes),
            changes.data@ == original.data@.remove_keys(original.keys@.take(position as int).to_set()),
            delta.model().dom() == original.keys@.take(position as int).to_set(),
            forall|key: i64, segment: i64| #[trigger] c_overlay(*base, delta, key, segment) ==
                c_index(*base, key, segment) + if original.keys@.take(position as int).contains(key) { work_count(original, key, segment) } else { 0 },
            forall|key: i64| (#[trigger] overlay_at(*base, delta, key)) matches Some(rows) ==> counts_unique(rows@),
            forall|key: i64| (#[trigger] delta.model().contains_key(key)) && delta.model()[key].is_some() ==> counts_ok(delta.model()[key].unwrap()@),
            forall|key: i64| (#[trigger] base.at(key)) matches Some(rows) ==> counts_ok(rows@),
        decreases changes.keys.len() - position,
    {
        let key = changes.keys[position];
        proof {
            assert(!original.keys@.take(position as int).contains(key));
            assert(original.data@.contains_key(key));
        }
        let mut rows = match base.get(key) { Some(value) => copy_counts(&value), None => Vec::new() };
        let additions = match changes.take(key) { Some(value) => value, None => return None };
        proof { assert(additions == original.data@[key]); assert(counts_unique(rows@)); }
        let mut addition = 0;
        while addition < additions.len()
            invariant addition <= additions.len(), run_ok(delta), counts_unique(rows@),
                counts_work_ok(original), changes.keys@ == original.keys@,
                additions == original.data@[key],
                changes.data@ == original.data@.remove_keys(original.keys@.take(position as int).to_set()).remove(key),
                delta.model().dom() == original.keys@.take(position as int).to_set(),
                forall|other_key: i64, segment: i64| #[trigger] c_overlay(*base, delta, other_key, segment) ==
                    c_index(*base, other_key, segment) + if original.keys@.take(position as int).contains(other_key) { work_count(original, other_key, segment) } else { 0 },
                forall|segment: i64| #[trigger] count_at(rows@, segment) == c_index(*base, key, segment) + count_at(additions@.take(addition as int), segment),
            decreases additions.len() - addition,
        {
            if add(&mut rows, additions[addition].0, additions[addition].1).is_none() { return None; }
            proof {
                assert forall|segment: i64| #[trigger] count_at(rows@, segment) ==
                    c_index(*base, key, segment) + count_at(additions@.take(addition as int + 1), segment) by {
                    lemma_sum_take(additions@, addition as int,
                        |item: (i64, i128)| if item.0 == segment { item.1 as int } else { 0 });
                }
            }
            addition += 1;
        }
        proof { assert(additions@.take(addition as int) =~= additions@); }
        let live = match positive(rows) { Some(value) => value, None => return None };
        let ghost previous_delta = delta;
        let ghost live_rows = live@;
        delta.put(key, if live.len() == 0 { None } else { Some(Arc::new(live)) });
        proof {
            assert(original.keys@.take(position as int + 1) =~= original.keys@.take(position as int).push(key));
            assert(delta.model().dom() =~= original.keys@.take(position as int + 1).to_set());
            assert(changes.data@ =~= original.data@.remove_keys(original.keys@.take(position as int + 1).to_set()));
            assert forall|other_key: i64, segment: i64| #[trigger] c_overlay(*base, delta, other_key, segment) ==
                c_index(*base, other_key, segment) + if original.keys@.take(position as int + 1).contains(other_key) { work_count(original, other_key, segment) } else { 0 } by {
                if other_key == key {
                    assert(count_at(live_rows, segment) == c_index(*base, key, segment) + work_count(original, key, segment));
                    assert(c_overlay(*base, delta, key, segment) == count_at(live_rows, segment));
                } else {
                    assert(c_overlay(*base, delta, other_key, segment) == c_overlay(*base, previous_delta, other_key, segment));
                    assert(c_overlay(*base, previous_delta, other_key, segment) == c_index(*base, other_key, segment)
                        + if original.keys@.take(position as int).contains(other_key) { work_count(original, other_key, segment) } else { 0 });
                }
            }
        }
        position += 1;
    }
    proof { assert(original.keys@.take(position as int) =~= original.keys@); }
    let index = match base.with(delta) { Some(value) => value, None => return None };
    proof {
        assert forall|key: i64, segment: i64| #[trigger] c_index(index, key, segment) ==
            c_index(*base, key, segment) + work_count(original, key, segment) by {
            assert(c_index(index, key, segment) == c_overlay(*base, delta, key, segment));
            assert(c_overlay(*base, delta, key, segment) == c_index(*base, key, segment) +
                if original.keys@.contains(key) { work_count(original, key, segment) } else { 0 });
            if !original.keys@.contains(key) { assert(!original.data@.contains_key(key)); }
        }
        assert forall|key: i64| (#[trigger] index.at(key)).is_some() implies counts_ok(index.at(key).unwrap()@) by {
            if !delta.model().contains_key(key) { assert(!original.data@.contains_key(key)); }
        }
    }
    Some(index)
}

pub open spec fn shipment_index_ok(index: Index<Vec<Bucket>>, db: Db) -> bool {
    &&& index.valid()
    &&& forall|key: i64| (#[trigger] index.at(key)) matches Some(rows) ==> rows.len() > 0 && profile_ok(db, key, rows@)
    &&& forall|key: i64| #[trigger] line_count(db, key as int, i32::MIN as int) > 0 <==> index.at(key).is_some()
}

pub open spec fn customer_index_ok(index: Index<Counts>, texts: Index<Vec<u8>>, db: Db) -> bool {
    &&& index.valid()
    &&& forall|key: i64| (#[trigger] index.at(key)) matches Some(rows) ==> counts_ok(rows@)
    &&& customer_db_counts(index, db, |segment: i64| texts.at(segment))
    &&& forall|position: int| 0 <= position < db.customer.len() ==> has_text(texts, (#[trigger] db.customer[position]).c_mktsegment)
}

pub open spec fn shipment_finish_ok(base: Index<Vec<Bucket>>, work: Work<Vec<Bucket>>, db: Db) -> bool {
    &&& bucket_work_ok(work)
    &&& forall|key: i64| (#[trigger] work.data@.contains_key(key)) ==> buckets_db(db, key, work.data@[key]@)
    &&& forall|key: i64| !work.data@.contains_key(key) ==> (
        ((#[trigger] base.at(key)) matches Some(rows) ==> rows.len() > 0 && profile_ok(db, key, rows@))
        && (line_count(db, key as int, i32::MIN as int) > 0 <==> base.at(key).is_some()))
    &&& forall|position: int| 0 <= position < db.lineitem.len() ==>
        i32::MIN <= (#[trigger] db.lineitem[position]).l_shipdate <= i32::MAX
}

pub fn finish_shipments(base: &Index<Vec<Bucket>>, mut work: Work<Vec<Bucket>>, Ghost(db): Ghost<Db>) -> (result: Option<Index<Vec<Bucket>>>)
    requires shipment_finish_ok(*base, work, db),
    ensures result matches Some(index) ==> shipment_index_ok(index, db),
{
    let ghost original = work;
    let mut delta = Run::new();
    let mut position = 0;
    while position < work.keys.len()
        invariant position <= work.keys.len(), run_ok(delta), shipment_finish_ok(*base, original, db),
            work_directory(work), work.keys@ == original.keys@,
            work.data@ == original.data@.remove_keys(original.keys@.take(position as int).to_set()),
            forall|key: i64| original.keys@.take(position as int).contains(key) || !original.data@.contains_key(key) ==>
                ((#[trigger] overlay_at(*base, delta, key)) matches Some(rows) ==> rows.len() > 0 && profile_ok(db, key, rows@)),
            forall|key: i64| original.keys@.take(position as int).contains(key) || !original.data@.contains_key(key) ==>
                ((#[trigger] overlay_at(*base, delta, key)).is_some() <==> line_count(db, key as int, i32::MIN as int) > 0),
        decreases work.keys.len() - position,
    {
        let key = work.keys[position];
        proof {
            assert(!original.keys@.take(position as int).contains(key));
            assert(original.data@.contains_key(key));
        }
        let rows = match work.take(key) { Some(value) => value, None => return None };
        let ghost raw = rows@;
        let profile = match suffix(rows) { Some(value) => value, None => return None };
        proof { lemma_suffix_db(db, key, raw, profile@); }
        let ghost previous_delta = delta;
        delta.put(key, if profile.len() == 0 { None } else { Some(Arc::new(profile)) });
        proof {
            assert(original.keys@.take(position as int + 1) =~= original.keys@.take(position as int).push(key));
            assert(work.data@ =~= original.data@.remove_keys(original.keys@.take(position as int + 1).to_set()));
            assert forall|other: i64| original.keys@.take(position as int + 1).contains(other) || !original.data@.contains_key(other) implies
                (((#[trigger] overlay_at(*base, delta, other)) matches Some(rows) ==> rows.len() > 0 && profile_ok(db, other, rows@))
                && (overlay_at(*base, delta, other).is_some() <==> line_count(db, other as int, i32::MIN as int) > 0)) by {
                if other != key { assert(overlay_at(*base, delta, other) == overlay_at(*base, previous_delta, other)); }
            }
        }
        position += 1;
    }
    proof { assert(original.keys@.take(position as int) =~= original.keys@); }
    let index = match base.with(delta) { Some(value) => value, None => return None };
    proof {
        assert forall|key: i64| (#[trigger] index.at(key)).is_some() implies
            index.at(key).unwrap().len() > 0 && profile_ok(db, key, index.at(key).unwrap()@) by {
            assert(index.at(key) == overlay_at(*base, delta, key));
        }
        assert forall|key: i64| #[trigger] line_count(db, key as int, i32::MIN as int) > 0 <==> index.at(key).is_some() by {
            assert(index.at(key) == overlay_at(*base, delta, key));
        }
    }
    Some(index)
}

pub fn build(
    customers: Vec<CustomerScan>, orders: &[OrdersScan], shipments: &[ShipmentScan],
    Ghost(db): Ghost<Db>,
) -> (result: Option<State>)
    requires scan_customers_ok(db, customers@), scan_orders_ok(db, orders@), scan_shipments_ok(db, shipments@),
    ensures result matches Some(state) ==> state.db() == db && state.invariant(),
{
    proof { lemma_scans_bounds(db, customers@, orders@, shipments@); }
    let empty_customers = Index::new();
    let empty_joins = Index::new();
    let empty_shipments = Index::new();
    let empty_names = Index::new();
    let empty_texts = Index::new();
    let mut name_delta = Run::new();
    let mut text_delta: Run<Vec<u8>> = Run::new();
    let mut next_segment = 0i64;
    let mut customer_work: Work<Counts> = Work::new();
    let mut position = 0;
    let mut processed = Ghost(0int);
    while position < customers.len()
        invariant
            db_bounds(db),
            position <= customers.len(), processed@ == position as int,
            run_ok(name_delta), run_ok(text_delta),
            counts_work_ok(customer_work),
            dictionary_overlay_ok(empty_names, name_delta, empty_texts, text_delta, next_segment),
            customer_prefix(customer_work, |id: i64| overlay_at(empty_texts, text_delta, id),
                customer_scan_seq_view(customers@).take(processed@),
                |row: CustomerRow| row.c_custkey, |row: CustomerRow| row.c_mktsegment, |row: CustomerRow| 1int),
            customer_texts_known(|id: i64| overlay_at(empty_texts, text_delta, id),
                customer_scan_seq_view(customers@).take(processed@), |row: CustomerRow| row.c_mktsegment),
        decreases customers.len() - position,
    {
        let row = &customers[position];
        let ghost previous_work = customer_work;
        let ghost previous_texts = text_delta;
        let segment = match intern(&empty_names, &empty_texts, &mut name_delta, &mut text_delta, &mut next_segment, row.c_mktsegment.as_slice()) {
            Some(value) => value, None => return None,
        };
        if add_work(&mut customer_work, row.c_custkey, segment, 1).is_none() { return None; }
        proof {
            lemma_dictionary_unique(empty_names, name_delta, empty_texts, text_delta, next_segment);
            lemma_customer_prefix_step(previous_work, customer_work,
                |id: i64| overlay_at(empty_texts, previous_texts, id),
                |id: i64| overlay_at(empty_texts, text_delta, id),
                customer_scan_seq_view(customers@).take(processed@), customer_scan_seq_view(customers@)[processed@],
                row.c_custkey, segment, 1,
                |item: CustomerRow| item.c_custkey, |item: CustomerRow| item.c_mktsegment, |item: CustomerRow| 1int);
            assert(customer_scan_seq_view(customers@).take(processed@ + 1) =~=
                customer_scan_seq_view(customers@).take(processed@).push(customer_scan_seq_view(customers@)[processed@]));
        }
        position += 1;
        processed = Ghost(position as int);
    }
    let ghost customer_totals = customer_work;
    proof {
        assert(customer_scan_seq_view(customers@).take(processed@) =~= customer_scan_seq_view(customers@));
        use_type_invariant(&empty_customers);
        empty_customers.lemma_empty();
    }
    let customer_index = match finish_customers(&empty_customers, customer_work) { Some(value) => value, None => return None };
    proof {
        assert forall|key: i64, segment: i64| #[trigger] c_index(customer_index, key, segment) ==
            match overlay_at(empty_texts, text_delta, segment) { Some(text) => customer_count(db, key as int, text@), None => 0 } by {
            if let Some(text) = overlay_at(empty_texts, text_delta, segment) {
                let key_of = |row: CustomerRow| row.c_custkey;
                let text_of = |row: CustomerRow| row.c_mktsegment;
                let amount_of = |row: CustomerRow| 1int;
                let actual = |row: CustomerRow| if key_of(row) == key && text_of(row) == text@ { amount_of(row) } else { 0 };
                let expected = |row: CustomerRow| if row.c_custkey == key && row.c_mktsegment == text@ { 1int } else { 0 };
                lemma_sum_ext_all(customer_scan_seq_view(customers@), actual, expected);
                assert(work_count(customer_totals, key, segment) == sum(customer_scan_seq_view(customers@), actual));
                assert(sum(customer_scan_seq_view(customers@), expected) == sum(db.customer, expected));
            }
        }
        assert(customer_db_counts(customer_index, db, |id: i64| overlay_at(empty_texts, text_delta, id)));
    }
    let names = match empty_names.with(name_delta) { Some(value) => value, None => return None };
    let texts = match empty_texts.with(text_delta) { Some(value) => value, None => return None };
    proof {
        let known = |row: CustomerRow| has_text(texts, row.c_mktsegment);
        assert forall|position: int| 0 <= position < customer_scan_seq_view(customers@).len() implies
            known(#[trigger] customer_scan_seq_view(customers@)[position]) by {
            let row = customer_scan_seq_view(customers@)[position];
            let segment = choose|segment: i64| (#[trigger] overlay_at(empty_texts, text_delta, segment)).is_some()
                && overlay_at(empty_texts, text_delta, segment).unwrap()@ == row.c_mktsegment;
            assert(texts.at(segment).is_some());
        }
        lemma_scan_predicate(customer_scan_seq_view(customers@), db.customer, known);
        assert(customer_index_ok(customer_index, texts, db));
        lemma_dictionary_finished(empty_names, name_delta, empty_texts, text_delta, next_segment, names, texts);
    }
    let mut join_work = Work::new();
    position = 0;
    processed = Ghost(0int);
    while position < orders.len()
        invariant
            db_bounds(db),
            customer_index_ok(customer_index, texts, db), dictionary_ok(names, texts, next_segment),
            position <= orders.len(), processed@ == position as int,
            run_ok(name_delta), run_ok(text_delta),
            join_work_ok(join_work),
            customer_db_counts(customer_index, db, |id: i64| overlay_at(empty_texts, text_delta, id)),
            forall|segment: i64, date: i32, key: i64, priority: i64|
                #[trigger] h_work(join_work, segment, date, key, priority) ==
                match overlay_at(empty_texts, text_delta, segment) {
                    Some(text) => sum(orders_scan_seq_view(orders@).take(processed@), |row: OrdersRow|
                        if row.o_orderdate == date && row.o_orderkey == key && row.o_shippriority == priority {
                            customer_count(db, row.o_custkey, text@)
                        } else { 0 }),
                    None => 0,
                },
        decreases orders.len() - position,
    {
        let row = orders[position];
        let ghost previous_joins = join_work;
        if let Some(segments) = customer_index.get(row.o_custkey) {
            let mut segment_position = 0;
            while segment_position < segments.len()
                invariant segment_position <= segments.len(),
                    customer_index_ok(customer_index, texts, db), dictionary_ok(names, texts, next_segment), db_bounds(db),
                    run_ok(name_delta), run_ok(text_delta),
                    join_work_ok(join_work),
                    customer_index.at(row.o_custkey) == Some(segments),
                    forall|segment: i64, date: i32, key: i64, priority: i64| #[trigger] h_work(join_work, segment, date, key, priority) ==
                        h_work(previous_joins, segment, date, key, priority)
                        + if date == row.o_orderdate && key == row.o_orderkey && priority == row.o_shippriority {
                            count_at(segments@.take(segment_position as int), segment)
                        } else { 0 },
                decreases segments.len() - segment_position,
            {
                let item = segments[segment_position];
                if crate::joins::change(&mut join_work, item.0, row.o_orderdate, row.o_orderkey, row.o_shippriority, item.1).is_none() { return None; }
                proof {
                    assert forall|segment: i64, date: i32, key: i64, priority: i64| #[trigger] h_work(join_work, segment, date, key, priority) ==
                        h_work(previous_joins, segment, date, key, priority)
                        + if date == row.o_orderdate && key == row.o_orderkey && priority == row.o_shippriority {
                            count_at(segments@.take(segment_position as int + 1), segment)
                        } else { 0 } by {
                        lemma_sum_take(segments@, segment_position as int, |entry: (i64, i128)| if entry.0 == segment { entry.1 as int } else { 0 });
                    }
                }
                segment_position += 1;
            }
            proof { assert(segments@.take(segment_position as int) =~= segments@); }
        }
        proof {
            assert forall|segment: i64, date: i32, key: i64, priority: i64| #[trigger] h_work(join_work, segment, date, key, priority) ==
                match overlay_at(empty_texts, text_delta, segment) {
                    Some(text) => sum(orders_scan_seq_view(orders@).take(processed@ + 1), |item: OrdersRow|
                        if item.o_orderdate == date && item.o_orderkey == key && item.o_shippriority == priority { customer_count(db, item.o_custkey, text@) } else { 0 }),
                    None => 0,
                } by {
                assert(h_work(join_work, segment, date, key, priority) == h_work(previous_joins, segment, date, key, priority)
                    + if date == row.o_orderdate && key == row.o_orderkey && priority == row.o_shippriority { c_index(customer_index, row.o_custkey, segment) } else { 0 });
                if let Some(text) = overlay_at(empty_texts, text_delta, segment) {
                    lemma_sum_take(orders_scan_seq_view(orders@), processed@, |item: OrdersRow|
                        if item.o_orderdate == date && item.o_orderkey == key && item.o_shippriority == priority { customer_count(db, item.o_custkey, text@) } else { 0 });
                }
            }
        }
        position += 1;
        processed = Ghost(position as int);
    }
    let join_index = match finish(&empty_joins, join_work) { Some(value) => value, None => return None };
    proof {
        assert(orders_scan_seq_view(orders@).take(processed@) =~= orders_scan_seq_view(orders@));
        assert forall|segment: i64, date: i32, key: i64, priority: i64| #[trigger] h_at(join_index, segment, date, key, priority) ==
            match texts.at(segment) { Some(text) => join_count(db, text@, date as int, key as int, priority as int), None => 0 } by {
            assert(h_at(join_index, segment, date, key, priority) == h_at(empty_joins, segment, date, key, priority) + h_work(join_work, segment, date, key, priority));
            assert(h_at(empty_joins, segment, date, key, priority) == 0);
            if let Some(text) = texts.at(segment) {
                let weight = |row: OrdersRow| if row.o_orderdate == date && row.o_orderkey == key && row.o_shippriority == priority {
                    customer_count(db, row.o_custkey, text@)
                } else { 0 };
                assert(sum(orders_scan_seq_view(orders@), weight) == sum(db.orders, weight));
            }
        }
        assert forall|segment: i64| (#[trigger] join_index.at(segment)).is_some() implies texts.at(segment).is_some() by {
            if texts.at(segment).is_none() {
                let group = lemma_join_live(join_index, segment);
                assert(h_at(join_index, segment, group.0, group.1, group.2) == 0);
            }
        }
    }
    let mut shipment_work: Work<Vec<Bucket>> = Work::new();
    position = 0;
    processed = Ghost(0int);
    while position < shipments.len()
        invariant
            db_bounds(db),
            customer_index_ok(customer_index, texts, db), dictionary_ok(names, texts, next_segment),
            join_index_ok(join_index),
            forall|segment: i64, date: i32, key: i64, priority: i64| #[trigger] h_at(join_index, segment, date, key, priority) ==
                match texts.at(segment) { Some(text) => join_count(db, text@, date as int, key as int, priority as int), None => 0 },
            forall|segment: i64| (#[trigger] join_index.at(segment)).is_some() ==> texts.at(segment).is_some(),
            position <= shipments.len(), processed@ == position as int,
            run_ok(name_delta), run_ok(text_delta),
            bucket_work_ok(shipment_work),
            forall|key: i64, date: i32| #[trigger] bucket_count(shipment_work, key, date) ==
                sum(shipment_scan_seq_view(shipments@).take(processed@), |row: LineitemRow|
                    if row.l_orderkey == key && row.l_shipdate == date { 1int } else { 0 }),
            forall|key: i64, date: i32| #[trigger] bucket_revenue(shipment_work, key, date) ==
                sum(shipment_scan_seq_view(shipments@).take(processed@), |row: LineitemRow|
                    if row.l_orderkey == key && row.l_shipdate == date {
                        row.l_extendedprice * (100 - row.l_discount)
                    } else { 0 }),
        decreases shipments.len() - position,
    {
        let row = shipments[position];
        let revenue = match numerator(row.l_extendedprice, row.l_discount) { Some(value) => value, None => return None };
        if change_work(&mut shipment_work, row.l_orderkey, row.l_shipdate, 1, revenue).is_none() { return None; }
        proof {
            assert forall|key: i64, date: i32| #[trigger] bucket_count(shipment_work, key, date) ==
                sum(shipment_scan_seq_view(shipments@).take(processed@ + 1), |item: LineitemRow|
                    if item.l_orderkey == key && item.l_shipdate == date { 1int } else { 0 }) by {
                lemma_sum_take(shipment_scan_seq_view(shipments@), processed@,
                    |item: LineitemRow| if item.l_orderkey == key && item.l_shipdate == date { 1int } else { 0 });
            }
            assert forall|key: i64, date: i32| #[trigger] bucket_revenue(shipment_work, key, date) ==
                sum(shipment_scan_seq_view(shipments@).take(processed@ + 1), |item: LineitemRow|
                    if item.l_orderkey == key && item.l_shipdate == date {
                        item.l_extendedprice * (100 - item.l_discount)
                    } else { 0 }) by {
                lemma_sum_take(shipment_scan_seq_view(shipments@), processed@,
                    |item: LineitemRow| if item.l_orderkey == key && item.l_shipdate == date {
                        item.l_extendedprice * (100 - item.l_discount)
                    } else { 0 });
            }
        }
        position += 1;
        processed = Ghost(position as int);
    }
    proof {
        assert(shipment_scan_seq_view(shipments@).take(processed@) =~= shipment_scan_seq_view(shipments@));
        assert forall|key: i64, date: i32| #![trigger bucket_count(shipment_work, key, date)] #![trigger bucket_revenue(shipment_work, key, date)]
            bucket_count(shipment_work, key, date) == line_count(db, key as int, date as int) - line_count(db, key as int, date as int + 1)
            && bucket_revenue(shipment_work, key, date) == line_revenue(db, key as int, date as int) - line_revenue(db, key as int, date as int + 1) by {
            let count_weight = |line: LineitemRow| if line.l_orderkey == key && line.l_shipdate == date { 1int } else { 0 };
            let revenue_weight = |line: LineitemRow| if line.l_orderkey == key && line.l_shipdate == date { line.l_extendedprice * (100 - line.l_discount) } else { 0 };
            assert(sum(shipment_scan_seq_view(shipments@), count_weight) == sum(db.lineitem, count_weight));
            assert(sum(shipment_scan_seq_view(shipments@), revenue_weight) == sum(db.lineitem, revenue_weight));
            lemma_line_point_count(db, key as int, date as int);
            lemma_line_point_revenue(db, key as int, date as int);
        }
        assert forall|key: i64| (#[trigger] shipment_work.data@.contains_key(key)) implies buckets_db(db, key, shipment_work.data@[key]@) by {
            assert forall|date: i32| #![trigger date_count(shipment_work.data@[key]@, date)] #![trigger date_revenue(shipment_work.data@[key]@, date)]
                date_count(shipment_work.data@[key]@, date) == line_count(db, key as int, date as int) - line_count(db, key as int, date as int + 1)
                && date_revenue(shipment_work.data@[key]@, date) == line_revenue(db, key as int, date as int) - line_revenue(db, key as int, date as int + 1) by {
                assert(bucket_count(shipment_work, key, date) == date_count(shipment_work.data@[key]@, date));
                assert(bucket_revenue(shipment_work, key, date) == date_revenue(shipment_work.data@[key]@, date));
            }
        }
        assert forall|key: i64| !(#[trigger] shipment_work.data@.contains_key(key)) implies line_count(db, key as int, i32::MIN as int) == 0 by {
            assert forall|date: i32| #[trigger] line_count(db, key as int, date as int) - line_count(db, key as int, date as int + 1) == 0 by {
                assert(bucket_count(shipment_work, key, date) == 0);
            }
            lemma_no_line_points(db, key);
        }
        assert(shipment_finish_ok(empty_shipments, shipment_work, db));
    }
    let shipment_index = match finish_shipments(&empty_shipments, shipment_work, Ghost(db)) { Some(value) => value, None => return None };
    Some(State {
        database: Ghost(db), customers: customer_index, joins: join_index, shipments: shipment_index,
        names, texts, next_segment,
    })
}

proof fn lemma_execute_no_joins(state: State, segment: i64, parameters: Params)
    requires state.invariant(), state.joins.at(segment).is_none(),
        state.texts.at(segment).is_some(), state.texts.at(segment).unwrap()@ == parameters.p1,
    ensures query_result(state.db(), parameters, Seq::<OutRow>::empty()),
{
    hide(sum);
    assert forall|key: int, date: int, priority: int| #[trigger] group_count(state.db(), parameters, key, date, priority) == 0 by {
        if i64::MIN <= key <= i64::MAX && i32::MIN <= date <= i32::MAX && i64::MIN <= priority <= i64::MAX {
            assert((key as i64) as int == key);
            assert((date as i32) as int == date);
            assert((priority as i64) as int == priority);
            assert(h_at(state.joins, segment, date as i32, key as i64, priority as i64) == 0);
            assert(join_count(state.db(), parameters.p1, date, key, priority) == 0);
        } else {
            lemma_sum_zero(state.db().orders, |row: OrdersRow|
                if row.o_orderdate == date && row.o_orderkey == key && row.o_shippriority == priority {
                    customer_count(state.db(), row.o_custkey, parameters.p1)
                } else { 0 });
        }
        lemma_group_count_factor(state.db(), parameters, key, date, priority);
    }
    lemma_empty_query(state.db(), parameters);
}

pub fn execute(state: &State, p1: &[u8], p2: i32) -> (result: Option<Vec<Row>>)
    ensures result matches Some(out) ==> query_result(state.db(), params(p1@, p2 as int), out_view(out@)),
{
    hide(sum);
    hide(tuples);
    hide(h_at);
    hide(customer_count);
    hide(join_count);
    hide(profile_ok);
    proof { use_type_invariant(state); }
    let mut output = Vec::new();
    let segment = match find(&state.names, p1) {
        Some(value) => value,
        None => {
            proof {
                assert forall|position: int| 0 <= position < state.db().customer.len() implies
                    (#[trigger] state.db().customer[position]).c_mktsegment != p1@ by {
                    let text = state.db().customer[position].c_mktsegment;
                    if text == p1@ {
                        let id = choose|id: i64| (#[trigger] state.texts.at(id)).is_some() && state.texts.at(id).unwrap()@ == text;
                        lemma_dictionary_text_match(state.names, state.texts, state.next_segment, text, id);
                    }
                }
                lemma_segment_absent(state.db(), params(p1@, p2 as int));
                assert(out_view(output@) =~= Seq::<OutRow>::empty());
            }
            return Some(output);
        },
    };
    proof { lemma_dictionary_named_text(state.names, state.texts, state.next_segment, p1@, segment); }
    let dates = match state.joins.get(segment) {
        Some(value) => value,
        None => {
            proof {
                lemma_execute_no_joins(*state, segment, params(p1@, p2 as int));
                assert(out_view(output@) =~= Seq::<OutRow>::empty());
            }
            return Some(output);
        },
    };
    let date_keys = dates.keys();
    let ghost parameters = params(p1@, p2 as int);
    let mut full = Ghost(Seq::<Row>::empty());
    let mut date_position = 0;
    while date_position < date_keys.len()
        invariant date_position <= date_keys.len(), output.len() <= 10,
            state.invariant(), state.joins.at(segment) == Some(dates),
            state.texts.at(segment).is_some(), state.texts.at(segment).unwrap()@ == p1@,
            parameters == params(p1@, p2 as int),
            date_keys@.no_duplicates(),
            forall|date: i64| (#[trigger] dates.at(date)).is_some() <==> date_keys@.contains(date),
            candidates(state.db(), parameters, full@), output@ == top_ten(full@),
            candidate_frontier(full@, date_keys@.take(date_position as int), None, Seq::empty(), None, Seq::empty()),
            candidate_coverage(*state, segment, parameters, full@, date_keys@.take(date_position as int), None, Seq::empty(), None, Seq::empty()),
        decreases date_keys.len() - date_position,
    {
        let date = date_keys[date_position];
        if date < p2 as i64 {
            let groups = match dates.get(date) { Some(value) => value, None => return None };
            proof { assert(i32::MIN <= date <= i32::MAX); }
            let keys = groups.keys();
            let mut group_position = 0;
            while group_position < keys.len()
                invariant group_position <= keys.len(), output.len() <= 10,
                    i32::MIN <= date <= i32::MAX,
                    state.invariant(), state.joins.at(segment) == Some(dates), dates.at(date) == Some(groups),
                    state.texts.at(segment).is_some(), state.texts.at(segment).unwrap()@ == p1@,
                    parameters == params(p1@, p2 as int), date < p2,
                    date_position < date_keys.len(), date_keys@[date_position as int] == date,
                    date_keys@.no_duplicates(),
                    forall|other_date: i64| (#[trigger] dates.at(other_date)).is_some() <==> date_keys@.contains(other_date),
                    keys@.no_duplicates(),
                    forall|key: i64| (#[trigger] groups.at(key)).is_some() <==> keys@.contains(key),
                    candidates(state.db(), parameters, full@), output@ == top_ten(full@),
                    candidate_frontier(full@, date_keys@.take(date_position as int), Some(date), keys@.take(group_position as int), None, Seq::empty()),
                    candidate_coverage(*state, segment, parameters, full@, date_keys@.take(date_position as int), Some(date), keys@.take(group_position as int), None, Seq::empty()),
                decreases keys.len() - group_position,
            {
                let key = keys[group_position];
                if let Some(profile) = state.shipments.get(key) {
                    proof { lemma_profile_sorted(state.db(), key, profile@); }
                    let selected = after(&profile, p2);
                    proof { lemma_profile_after(state.db(), key, profile@, p2, selected); }
                    if let Some(lines) = selected {
                        let counts = match groups.get(key) { Some(value) => value, None => return None };
                        let mut priority_position = 0;
                        while priority_position < counts.len()
                            invariant priority_position <= counts.len(), output.len() <= 10,
                                i32::MIN <= date <= i32::MAX,
                                state.invariant(), state.joins.at(segment) == Some(dates), dates.at(date) == Some(groups), groups.at(key) == Some(counts), counts_ok(counts@),
                                state.texts.at(segment).is_some(), state.texts.at(segment).unwrap()@ == p1@,
                                parameters == params(p1@, p2 as int), date < p2,
                                date_position < date_keys.len(), date_keys@[date_position as int] == date, date_keys@.no_duplicates(),
                                group_position < keys.len(), keys@[group_position as int] == key, keys@.no_duplicates(),
                                forall|other_date: i64| (#[trigger] dates.at(other_date)).is_some() <==> date_keys@.contains(other_date),
                                forall|other_key: i64| (#[trigger] groups.at(other_key)).is_some() <==> keys@.contains(other_key),
                                lines.count > 0, lines.count == line_count(state.db(), key as int, p2 as int + 1),
                                lines.revenue == line_revenue(state.db(), key as int, p2 as int + 1),
                                candidates(state.db(), parameters, full@), output@ == top_ten(full@),
                                candidate_frontier(full@, date_keys@.take(date_position as int), Some(date), keys@.take(group_position as int), Some(key), counts@.take(priority_position as int)),
                                candidate_coverage(*state, segment, parameters, full@, date_keys@.take(date_position as int), Some(date), keys@.take(group_position as int), Some(key), counts@.take(priority_position as int)),
                            decreases counts.len() - priority_position,
                        {
                            let item = counts[priority_position];
                            let revenue = match item.1.checked_mul(lines.revenue) { Some(value) => value, None => return None };
                            let ghost before = full@;
                            let ghost before_output = output@;
                            proof {
                                lemma_count_item(counts@, priority_position as int);
                                lemma_h_at(state.joins, segment, date as i32, key, item.0);
                                lemma_candidate_row(*state, segment, parameters, date as i32, key, item.0, item.1, lines, revenue);
                                assert(!processed_group(date_keys@.take(date_position as int), Some(date), keys@.take(group_position as int), Some(key), counts@.take(priority_position as int), date as i32, key, item.0));
                                lemma_candidate_step(*state, segment, parameters, before, (key, revenue, date as i32, item.0),
                                    date_keys@.take(date_position as int), date, keys@.take(group_position as int), key, counts@.take(priority_position as int), item);
                                assert(counts@.take(priority_position as int + 1) =~= counts@.take(priority_position as int).push(item));
                            }
                            select(&mut output, (key, revenue, date as i32, item.0));
                            full = Ghost(insert_row(before, (key, revenue, date as i32, item.0)));
                            proof { lemma_selection_prefix(before, (key, revenue, date as i32, item.0), before_output, output@); }
                            priority_position += 1;
                        }
                        proof {
                            assert(counts@.take(priority_position as int) =~= counts@);
                            assert forall|priority: i64| #[trigger] ready(*state, segment, parameters, date as i32, key, priority) implies
                                contains_group(full@, state.db(), parameters, date as i32, key, priority) by {
                                lemma_h_at(state.joins, segment, date as i32, key, priority);
                                assert(count_at(counts@, priority) == h_at(state.joins, segment, date as i32, key, priority));
                                let position = lemma_sum_nonzero_witness(counts@, count_weight(priority));
                                assert(counts@[position].0 == priority);
                                assert(processed_group(date_keys@.take(date_position as int), Some(date), keys@.take(group_position as int), Some(key), counts@, date as i32, key, priority));
                            }
                        }
                    }
                } else {
                    proof {
                        lemma_sum_nonneg(state.db().lineitem, |line: LineitemRow| if line.l_orderkey == key && line.l_shipdate >= i32::MIN { 1int } else { 0 });
                        assert(line_count(state.db(), key as int, i32::MIN as int) == 0);
                        lemma_line_empty(state.db(), key, p2 as int + 1);
                    }
                }
                proof {
                    assert(keys@.take(group_position as int + 1) =~= keys@.take(group_position as int).push(key));
                    assert forall|query_date: i32, query_key: i64, query_priority: i64|
                        #[trigger] ready(*state, segment, parameters, query_date, query_key, query_priority)
                        && processed_group(date_keys@.take(date_position as int), Some(date), keys@.take(group_position as int + 1), None, Seq::empty(), query_date, query_key, query_priority)
                        implies contains_group(full@, state.db(), parameters, query_date, query_key, query_priority) by {
                        if query_date as i64 == date && query_key == key {
                            assert(contains_group(full@, state.db(), parameters, query_date, query_key, query_priority));
                        } else {
                            assert(processed_group(date_keys@.take(date_position as int), Some(date), keys@.take(group_position as int), None, Seq::empty(), query_date, query_key, query_priority));
                        }
                    }
                }
                group_position += 1;
            }
            proof { assert(keys@.take(group_position as int) =~= keys@); }
        }
        proof {
            assert(date_keys@.take(date_position as int + 1) =~= date_keys@.take(date_position as int).push(date));
            assert forall|query_date: i32, query_key: i64, query_priority: i64|
                #[trigger] ready(*state, segment, parameters, query_date, query_key, query_priority)
                && date_keys@.take(date_position as int + 1).contains(query_date as i64)
                implies contains_group(full@, state.db(), parameters, query_date, query_key, query_priority) by {
                if query_date as i64 == date {
                    lemma_h_at(state.joins, segment, query_date, query_key, query_priority);
                    assert(contains_group(full@, state.db(), parameters, query_date, query_key, query_priority));
                } else {
                    assert(date_keys@.take(date_position as int).contains(query_date as i64));
                }
            }
        }
        date_position += 1;
    }
    proof {
        assert(date_keys@.take(date_position as int) =~= date_keys@);
        assert forall|date: i32, key: i64, priority: i64| #[trigger] ready(*state, segment, parameters, date, key, priority) implies
            contains_group(full@, state.db(), parameters, date, key, priority) by {
            lemma_h_at(state.joins, segment, date, key, priority);
            assert(dates.at(date as i64).is_some());
            assert(date_keys@.contains(date as i64));
        }
        lemma_execute_complete(*state, segment, parameters, full@, output@);
    }
    Some(output)
}

pub fn rescan_customer_orders_keys(
    state: &State, customer_delta: &Vec<CustomerDelta>, lineitem_delta: &[ShipmentDelta], orders_delta: &[OrdersDelta],
) -> (result: Vec<i64>) {
    let mut keys: Work<bool> = Work::new();
    let mut position = 0;
    let mut processed = Ghost(0int);
    while position < customer_delta.len()
        invariant position <= customer_delta.len(), processed@ == position as int,
        decreases customer_delta.len() - position,
    {
        keys.put(customer_delta[position].row.c_custkey, true);
        position += 1;
        processed = Ghost(position as int);
    }
    keys.keys
}

pub fn apply(
    state: &State, customer_delta: &Vec<CustomerDelta>, lineitem_delta: &[ShipmentDelta], orders_delta: &[OrdersDelta],
    customer_orders_keys: &[i64], customer_orders_rows: &[CustomerOrdersRescan], Ghost(db2): Ghost<Db>,
) -> (result: Result<State, u32>)
    requires
        capture_ok(state.db(), db2, customer_delta_seq_view(customer_delta@), shipment_delta_seq_view(lineitem_delta@), orders_delta_seq_view(orders_delta@)),
        rescan_customer_orders_ok(db2, customer_orders_keys@, customer_orders_rows@),
    ensures result matches Ok(next) ==> next.db() == db2 && next.invariant(),
{
    match apply_inner(state, customer_delta, lineitem_delta, orders_delta, customer_orders_keys, customer_orders_rows, Ghost(db2)) {
        Some(next) => Ok(next), None => Err(1),
    }
}

include!("maintenance.rs");
include!("maintenance_proofs.rs");
include!("execute_proofs.rs");

}

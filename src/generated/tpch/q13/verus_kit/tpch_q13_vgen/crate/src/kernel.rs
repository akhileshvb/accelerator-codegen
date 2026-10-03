#![allow(unused_imports)]
use vstd::prelude::*;
use verus_common::sums::*;
use verus_common::rel::*;
use crate::gen_spec::*;
use crate::gen_obligations::*;
use crate::aggregate::*;
use crate::facts::*;

verus! {

pub open spec fn customer_count(db: Db, key: int) -> int {
    sum(db.customer, |row: CustomerRow| ind(row.c_custkey == key))
}

pub open spec fn order_count(db: Db, key: int) -> int {
    sum(db.orders, |row: OrdersRow| ind(c_orders_on_orders(
        db, params(), CustomerRow { c_custkey: key }, row)))
}

pub open spec fn histogram_count(db: Db, bucket: int) -> int {
    sum(customer_keys(db), |key: int| contribution(db, key, bucket))
}

pub struct State {
    customers: Layers,
    orders: Layers,
    histogram: Layers,
    db: Ghost<Db>,
}

impl State {
    pub closed spec fn db(self) -> Db { self.db@ }

    #[verifier::type_invariant]
    spec fn wf(self) -> bool {
        &&& self.customers.wf()
        &&& self.orders.wf()
        &&& self.histogram.wf()
        &&& forall|key: int| #[trigger] self.customers.at(key)
            == customer_count(self.db(), key)
        &&& forall|key: int| #[trigger] self.orders.at(key)
            == order_count(self.db(), key)
        &&& forall|bucket: int| #[trigger] self.histogram.at(bucket)
            == histogram_count(self.db(), bucket)
        &&& forall|key: int| #[trigger] self.customers.at(key) >= 0
            && self.customers.at(key) <= i64::MAX
        &&& forall|key: int| #[trigger] self.orders.at(key) >= 0
            && self.orders.at(key) <= i64::MAX
        &&& forall|bucket: int| #[trigger] self.histogram.at(bucket) >= 0
            && self.histogram.at(bucket) <= i64::MAX
    }
}

pub fn build(
    customers: &[CustomerScan],
    qualifying_orders: &[QualifyingOrderScan],
    Ghost(db): Ghost<Db>,
) -> (result: Option<State>)
    requires scan_customers_ok(db, customers@),
        scan_qualifying_orders_ok(db, qualifying_orders@),
    ensures result matches Some(state) ==> state.db() == db,
{
    let mut customer_counts = Sparse::new();
    let mut index = 0usize;
    let mut processed = Ghost(0int);
    while index < customers.len()
        invariant
            index <= customers.len(), processed@ == index as int,
            customer_counts.wf(),
            forall|key: int| #[trigger] customer_counts.at(key)
                == sum(customer_scan_seq_view(customers@).take(processed@),
                    |row: CustomerRow| ind(row.c_custkey == key)),
            forall|key: i128| #[trigger] customer_counts.contains(key as int) ==> customer_counts.at(key as int) > 0,
        decreases customers.len() - index,
    {
        let ghost previous = customer_counts;
        if !customer_counts.add(customers[index].c_custkey as i128, 1) { return None; }
        proof {
            assert forall|key: int| #[trigger] customer_counts.at(key)
                == sum(customer_scan_seq_view(customers@).take(index as int + 1),
                    |row: CustomerRow| ind(row.c_custkey == key)) by {
                lemma_sum_take(customer_scan_seq_view(customers@), index as int,
                    |row: CustomerRow| ind(row.c_custkey == key));
            }
            assert forall|key: i128| #[trigger] customer_counts.contains(key as int) implies customer_counts.at(key as int) > 0 by {
                if key == customers@[index as int].c_custkey as i128 {
                    lemma_sum_nonneg(customer_scan_seq_view(customers@).take(index as int),
                        |row: CustomerRow| ind(row.c_custkey == key as int));
                }
            }
        }
        index += 1;
        processed = Ghost(index as int);
    }
    proof {
        assert(customer_scan_seq_view(customers@).take(processed@) =~= customer_scan_seq_view(customers@));
    }
    let mut order_counts = Sparse::new();
    index = 0;
    processed = Ghost(0int);
    while index < qualifying_orders.len()
        invariant
            index <= qualifying_orders.len(), processed@ == index as int,
            order_counts.wf(),
            customer_counts.wf(),
            forall|key: int| #[trigger] customer_counts.at(key)
                == sum(customer_scan_seq_view(customers@), |row: CustomerRow| ind(row.c_custkey == key)),
            forall|key: i128| #[trigger] customer_counts.contains(key as int) ==> customer_counts.at(key as int) > 0,
            forall|key: int| #[trigger] order_counts.at(key)
                == sum(qualifying_order_scan_seq_view(qualifying_orders@).take(processed@),
                    |row: int| ind(row == key)),
        decreases qualifying_orders.len() - index,
    {
        if !order_counts.add(qualifying_orders[index].o_custkey as i128, 1) { return None; }
        proof {
            assert forall|key: int| #[trigger] order_counts.at(key)
                == sum(qualifying_order_scan_seq_view(qualifying_orders@).take(index as int + 1),
                    |row: int| ind(row == key)) by {
                lemma_sum_take(qualifying_order_scan_seq_view(qualifying_orders@), index as int,
                    |row: int| ind(row == key));
            }
        }
        index += 1;
        processed = Ghost(index as int);
    }
    proof {
        assert(qualifying_order_scan_seq_view(qualifying_orders@).take(processed@)
            =~= qualifying_order_scan_seq_view(qualifying_orders@));
    }
    if !customer_counts.valid_counts() || !order_counts.valid_counts() { return None; }
    proof {
        assert forall|key: int| #[trigger] customer_counts.at(key) == customer_count(db, key) by {
            let weight = |row: CustomerRow| ind(row.c_custkey == key);
            assert(sum(customer_scan_seq_view(customers@), weight) == sum(db.customer, weight));
        }
        assert forall|key: int| #[trigger] order_counts.at(key) == order_count(db, key) by {
            let weight = |row: int| ind(row == key);
            let projected = |row: OrdersRow| if !verus_common::sqlfns::like_match(row.o_comment,
                seq![b'%', b'u', b'n', b'u', b's', b'u', b'a', b'l', b'%', b'a', b'c', b'c', b'o', b'u', b'n', b't', b's', b'%'])
                { weight(row.o_custkey) } else { 0 };
            assert(sum(qualifying_order_scan_seq_view(qualifying_orders@), weight) == sum(db.orders, projected));
            lemma_sum_ext_all(db.orders, projected, |row: OrdersRow|
                ind(c_orders_on_orders(db, params(), CustomerRow { c_custkey: key }, row)));
        }
    }
    let mut histogram = Sparse::new();
    index = 0;
    processed = Ghost(0int);
    while index < customer_counts.len()
        invariant index <= customer_counts.keys().len(), processed@ == index as int,
            customer_counts.wf(), order_counts.wf(), histogram.wf(),
            forall|key: int| #[trigger] customer_counts.at(key) == customer_count(db, key),
            forall|key: int| #[trigger] order_counts.at(key) == order_count(db, key),
            forall|key: i128| #[trigger] customer_counts.contains(key as int) ==> customer_counts.at(key as int) > 0,
            forall|key: int| 0 <= #[trigger] customer_counts.at(key) <= i64::MAX,
            forall|key: int| 0 <= #[trigger] order_counts.at(key) <= i64::MAX,
            forall|bucket: int| #[trigger] histogram.at(bucket)
                == sum(customer_counts.keys().take(processed@), |key: i128|
                    ind(customer_counts.at(key as int) * order_counts.at(key as int) == bucket)),
        decreases customer_counts.keys().len() - index,
    {
        let key = customer_counts.key(index);
        let count = match customer_counts.get(key).checked_mul(order_counts.get(key)) {
            Some(value) => value,
            None => return None,
        };
        if !histogram.add(count, 1) { return None; }
        proof {
            assert forall|bucket: int| #[trigger] histogram.at(bucket)
                == sum(customer_counts.keys().take(index as int + 1), |key: i128|
                    ind(customer_counts.at(key as int) * order_counts.at(key as int) == bucket)) by {
                lemma_sum_take(customer_counts.keys(), index as int, |key: i128|
                    ind(customer_counts.at(key as int) * order_counts.at(key as int) == bucket));
            }
        }
        index += 1;
        processed = Ghost(index as int);
    }
    if !histogram.valid_counts() { return None; }
    proof {
        assert(customer_counts.keys().take(processed@) =~= customer_counts.keys());
        customer_counts.lemma_keys();
        lemma_build_facts(db, customer_counts, order_counts, histogram);
    }
    Some(State {
        customers: Layers::new(customer_counts),
        orders: Layers::new(order_counts),
        histogram: Layers::new(histogram),
        db: Ghost(db),
    })
}

pub fn execute(state: &State) -> (result: Option<Vec<(i64, i64)>>)
    ensures result matches Some(out) ==>
        query_result(state.db(), params(), out_view(out@)),
{
    proof { use_type_invariant(state); }
    let histogram = state.histogram.materialize();
    let mut rows: Vec<(i64, i64)> = Vec::new();
    let mut index = 0usize;
    while index < histogram.len()
        invariant state.wf(), index <= histogram.keys().len(), histogram.wf(),
            forall|bucket: int| #[trigger] histogram.at(bucket) == histogram_count(state.db(), bucket),
            forall|position: int| 0 <= position < rows@.len() ==> {
                &&& (#[trigger] rows@[position]).1 > 0
                &&& rows@[position].1 as int == histogram.at(rows@[position].0 as int)
            },
            forall|bucket: i128| #[trigger] histogram.keys().take(index as int).contains(bucket)
                && histogram.at(bucket as int) > 0 ==>
                    exists|position: int| 0 <= position < rows@.len() && (#[trigger] rows@[position]).0 as int == bucket as int,
            forall|first: int, second: int| #![trigger rows@[first], rows@[second]]
                0 <= first < second < rows@.len() ==> rows@[first].0 != rows@[second].0,
            forall|position: int| 0 <= position < rows@.len() ==>
                #[trigger] histogram.keys().take(index as int).contains(rows@[position].0 as i128),
        decreases histogram.keys().len() - index,
    {
        let bucket = histogram.key(index);
        let frequency = histogram.get(bucket);
        let ghost previous_rows = rows@;
        proof {
            histogram.lemma_keys();
            lemma_key_not_in_prefix(histogram.keys(), index as int);
        }
        if frequency < 0 || frequency > i64::MAX as i128 { return None; }
        if frequency > 0 {
            if bucket < 0 || bucket > i64::MAX as i128 { return None; }
            rows.push((bucket as i64, frequency as i64));
        }
        proof {
            assert(histogram.keys().take(index as int + 1) =~= histogram.keys().take(index as int).push(bucket));
            lemma_contains_push(histogram.keys().take(index as int), bucket);
            assert forall|candidate: i128| #[trigger] histogram.keys().take(index as int + 1).contains(candidate)
                && histogram.at(candidate as int) > 0 implies
                    exists|position: int| 0 <= position < rows@.len()
                        && (#[trigger] rows@[position]).0 as int == candidate as int by {
                if candidate == bucket {
                    assert(rows@[rows@.len() as int - 1].0 as int == candidate as int);
                } else {
                    let position = choose|position: int| 0 <= position < previous_rows.len()
                        && (#[trigger] previous_rows[position]).0 as int == candidate as int;
                    assert(rows@[position] == previous_rows[position]);
                }
            }
            assert forall|position: int| 0 <= position < rows@.len() implies
                #[trigger] histogram.keys().take(index as int + 1).contains(rows@[position].0 as i128) by {
                if position < previous_rows.len() { assert(rows@[position] == previous_rows[position]); }
            }
            assert forall|first: int, second: int| #![trigger rows@[first], rows@[second]]
                0 <= first < second < rows@.len() implies rows@[first].0 != rows@[second].0 by {
                if second < previous_rows.len() {
                    assert(rows@[first] == previous_rows[first] && rows@[second] == previous_rows[second]);
                } else {
                    assert(histogram.keys().take(index as int).contains(previous_rows[first].0 as i128));
                }
            }
        }
        index += 1;
    }
    proof {
        assert(histogram.keys().take(index as int) =~= histogram.keys());
        assert forall|bucket: int| #[trigger] histogram_count(state.db(), bucket) > 0 implies
            exists|position: int| 0 <= position < rows@.len() && (#[trigger] rows@[position]).0 as int == bucket by {
            histogram.lemma_support(bucket);
            assert(histogram.keys().contains(bucket as i128));
            assert(histogram.at((bucket as i128) as int) > 0);
        }
    }
    let ghost unsorted = rows@;
    crate::output::sort(&mut rows);
    proof {
        assert forall|row: (i64, i64)| #[trigger] rows@.contains(row) == unsorted.contains(row) by {
            lemma_bag_contains(rows@, row);
            lemma_bag_contains(unsorted, row);
        }
        assert forall|position: int| 0 <= position < rows@.len() implies {
            &&& (#[trigger] rows@[position]).1 > 0
            &&& rows@[position].1 as int == histogram_count(state.db(), rows@[position].0 as int)
        } by {
            assert(rows@.contains(rows@[position]));
            let original = choose|original: int| 0 <= original < unsorted.len() && unsorted[original] == rows@[position];
        }
        assert forall|bucket: int| #[trigger] histogram_count(state.db(), bucket) > 0 implies
            exists|position: int| 0 <= position < rows@.len() && (#[trigger] rows@[position]).0 as int == bucket by {
            let original = choose|original: int| 0 <= original < unsorted.len() && (#[trigger] unsorted[original]).0 as int == bucket;
            assert(unsorted.contains(unsorted[original]));
            assert(rows@.contains(unsorted[original]));
            let position = choose|position: int| 0 <= position < rows@.len() && rows@[position] == unsorted[original];
        }
        lemma_execute_facts(state.db(), rows@);
    }
    Some(rows)
}

pub fn apply(
    state: &State,
    customer_delta: &[CustomerDelta],
    orders_delta: &[OrderDelta],
    Ghost(orders_full): Ghost<Seq<(OrdersRow, int)>>,
    Ghost(db2): Ghost<Db>,
) -> (result: Result<State, u32>)
    requires orders_delta_ok(orders_delta@, orders_full),
        capture_ok(state.db(), db2, customer_delta_seq_view(customer_delta@), orders_full),
    ensures result matches Ok(next) ==> next.db() == db2,
{
    proof { use_type_invariant(state); }
    let mut customer_changes = Sparse::new();
    let mut order_changes = Sparse::new();
    let mut touched = Sparse::new();
    let mut index = 0usize;
    let mut processed = Ghost(0int);
    while index < customer_delta.len()
        invariant state.wf(), index <= customer_delta.len(), processed@ == index as int,
            customer_changes.wf(), touched.wf(),
            forall|key: int| #[trigger] customer_changes.at(key)
                == sum(customer_delta_seq_view(customer_delta@).take(processed@),
                    |change: (CustomerRow, int)| change.1 * ind(change.0.c_custkey == key)),
            forall|key: int| #[trigger] customer_changes.at(key) != 0 ==> touched.contains(key),
        decreases customer_delta.len() - index,
    {
        let key = customer_delta[index].row.c_custkey as i128;
        if !customer_changes.add(key, customer_delta[index].sign as i128) { return Err(1); }
        touched.set(key, 1);
        proof {
            assert forall|key: int| #[trigger] customer_changes.at(key)
                == sum(customer_delta_seq_view(customer_delta@).take(index as int + 1),
                    |change: (CustomerRow, int)| change.1 * ind(change.0.c_custkey == key)) by {
                lemma_sum_take(customer_delta_seq_view(customer_delta@), index as int,
                    |change: (CustomerRow, int)| change.1 * ind(change.0.c_custkey == key));
            }
        }
        index += 1;
        processed = Ghost(index as int);
    }
    proof {
        assert(customer_delta_seq_view(customer_delta@).take(processed@) =~= customer_delta_seq_view(customer_delta@));
    }
    index = 0;
    processed = Ghost(0int);
    while index < orders_delta.len()
        invariant state.wf(), index <= orders_delta.len(), processed@ == index as int,
            customer_changes.wf(), order_changes.wf(), touched.wf(),
            forall|key: int| #[trigger] customer_changes.at(key) == customer_change(customer_delta@, key),
            forall|key: int| #[trigger] customer_changes.at(key) != 0 ==> touched.contains(key),
            forall|key: int| #[trigger] order_changes.at(key) != 0 ==> touched.contains(key),
            forall|key: int| #[trigger] order_changes.at(key)
                == sum(orders_delta@.take(processed@), |change: OrderDelta|
                    change.sign as int * ind(change.o_custkey as int == key && change.qualifies != 0)),
        decreases orders_delta.len() - index,
    {
        if orders_delta[index].qualifies != 0 {
            let key = orders_delta[index].o_custkey as i128;
            if !order_changes.add(key, orders_delta[index].sign as i128) { return Err(1); }
            touched.set(key, 1);
        }
        proof {
            assert forall|key: int| #[trigger] order_changes.at(key)
                == sum(orders_delta@.take(index as int + 1), |change: OrderDelta|
                    change.sign as int * ind(change.o_custkey as int == key && change.qualifies != 0)) by {
                lemma_sum_take(orders_delta@, index as int, |change: OrderDelta|
                    change.sign as int * ind(change.o_custkey as int == key && change.qualifies != 0));
            }
        }
        index += 1;
        processed = Ghost(index as int);
    }
    proof { assert(orders_delta@.take(processed@) =~= orders_delta@); }
    let mut next_customers = Sparse::new();
    let mut next_orders = Sparse::new();
    let mut histogram_changes = Sparse::new();
    proof {
        assert forall|key: int| #[trigger] customer_changes.at(key)
            == customer_count(db2, key) - customer_count(state.db(), key) by {
            lemma_capture_counts(state.db(), db2, customer_delta@, orders_delta@, orders_full, key);
        }
        assert forall|key: int| #[trigger] order_changes.at(key)
            == order_count(db2, key) - order_count(state.db(), key) by {
            lemma_capture_counts(state.db(), db2, customer_delta@, orders_delta@, orders_full, key);
        }
    }
    index = 0;
    processed = Ghost(0int);
    while index < touched.len()
        invariant state.wf(), index <= touched.keys().len(), processed@ == index as int,
            touched.wf(), next_customers.wf(), next_orders.wf(), histogram_changes.wf(),
            customer_changes.wf(), order_changes.wf(),
            forall|key: int| #[trigger] customer_changes.at(key)
                == customer_count(db2, key) - customer_count(state.db(), key),
            forall|key: int| #[trigger] order_changes.at(key)
                == order_count(db2, key) - order_count(state.db(), key),
            forall|key: int| #[trigger] customer_changes.at(key) != 0 ==> touched.contains(key),
            forall|key: int| #[trigger] order_changes.at(key) != 0 ==> touched.contains(key),
            forall|key: int| #[trigger] next_customers.contains(key)
                == (has_key(touched.keys().take(processed@), key) && customer_changes.at(key) != 0),
            forall|key: int| #[trigger] next_orders.contains(key)
                == (has_key(touched.keys().take(processed@), key) && order_changes.at(key) != 0),
            forall|key: int| #[trigger] next_customers.contains(key) ==> {
                &&& next_customers.at(key) == customer_count(db2, key)
                &&& 0 <= next_customers.at(key) <= i64::MAX
            },
            forall|key: int| #[trigger] next_orders.contains(key) ==> {
                &&& next_orders.at(key) == order_count(db2, key)
                &&& 0 <= next_orders.at(key) <= i64::MAX
            },
            forall|bucket: int| #[trigger] histogram_changes.at(bucket)
                == sum(touched.keys().take(processed@), |key: i128|
                    contribution(db2, key as int, bucket) - contribution(state.db(), key as int, bucket)),
        decreases touched.keys().len() - index,
    {
        let key = touched.key(index);
        let customer_change = customer_changes.get(key);
        let order_change = order_changes.get(key);
        let ghost previous_histogram = histogram_changes;
        if customer_change != 0 || order_change != 0 {
            let old_customers = state.customers.get(key);
            let old_orders = state.orders.get(key);
            let new_customers = match old_customers.checked_add(customer_change) {
                Some(value) => value,
                None => return Err(1),
            };
            let new_orders = match old_orders.checked_add(order_change) {
                Some(value) => value,
                None => return Err(1),
            };
            if new_customers < 0 || new_customers > i64::MAX as i128
                || new_orders < 0 || new_orders > i64::MAX as i128 { return Err(2); }
            proof {
                assert(new_customers as int == customer_count(db2, key as int));
                assert(new_orders as int == order_count(db2, key as int));
            }
            if old_customers > 0 {
                let bucket = match old_customers.checked_mul(old_orders) {
                    Some(value) => value,
                    None => return Err(1),
                };
                if !histogram_changes.add(bucket, -1) { return Err(1); }
            }
            proof {
                assert forall|bucket: int| #[trigger] histogram_changes.at(bucket)
                    == previous_histogram.at(bucket) - contribution(state.db(), key as int, bucket) by {}
            }
            if new_customers > 0 {
                let bucket = match new_customers.checked_mul(new_orders) {
                    Some(value) => value,
                    None => return Err(1),
                };
                if !histogram_changes.add(bucket, 1) { return Err(1); }
            }
            proof {
                assert forall|bucket: int| #[trigger] histogram_changes.at(bucket)
                    == previous_histogram.at(bucket) + contribution(db2, key as int, bucket)
                        - contribution(state.db(), key as int, bucket) by {}
            }
            if customer_change != 0 { next_customers.set(key, new_customers); }
            if order_change != 0 { next_orders.set(key, new_orders); }
        }
        proof {
            lemma_has_key_step(touched.keys(), index as int);
            assert forall|bucket: int| #[trigger] histogram_changes.at(bucket)
                == sum(touched.keys().take(index as int + 1), |key: i128|
                    contribution(db2, key as int, bucket) - contribution(state.db(), key as int, bucket)) by {
                lemma_sum_take(touched.keys(), index as int, |key: i128|
                    contribution(db2, key as int, bucket) - contribution(state.db(), key as int, bucket));
            }
        }
        index += 1;
        processed = Ghost(index as int);
    }
    proof {
        touched.lemma_keys();
        assert(touched.keys().take(processed@) =~= touched.keys());
        assert forall|key: int| !#[trigger] cast_keys(touched.keys()).contains(key) implies
            customer_count(state.db(), key) == customer_count(db2, key)
                && order_count(state.db(), key) == order_count(db2, key) by {
            touched.lemma_support(key);
            lemma_cast_keys(touched.keys());
            assert(!touched.contains(key));
            assert(customer_changes.at(key) == 0);
            assert(order_changes.at(key) == 0);
        }
        assert forall|bucket: int| #[trigger] histogram_changes.at(bucket)
            == histogram_count(db2, bucket) - histogram_count(state.db(), bucket) by {
            lemma_apply_facts(state.db(), db2, touched.keys(), bucket);
        }
        assert forall|key: int| #[trigger] next_customers.contains(key) == (customer_changes.at(key) != 0) by {
            touched.lemma_support(key);
        }
        assert forall|key: int| #[trigger] next_orders.contains(key) == (order_changes.at(key) != 0) by {
            touched.lemma_support(key);
        }
    }
    let mut next_histogram = Sparse::new();
    index = 0;
    while index < histogram_changes.len()
        invariant state.wf(), index <= histogram_changes.keys().len(), histogram_changes.wf(), next_histogram.wf(),
            next_customers.wf(), next_orders.wf(),
            forall|key: int| #[trigger] next_customers.contains(key) ==> {
                &&& next_customers.at(key) == customer_count(db2, key)
                &&& 0 <= next_customers.at(key) <= i64::MAX
            },
            forall|key: int| #[trigger] next_orders.contains(key) ==> {
                &&& next_orders.at(key) == order_count(db2, key)
                &&& 0 <= next_orders.at(key) <= i64::MAX
            },
            forall|key: int| !#[trigger] next_customers.contains(key) ==> customer_count(db2, key) == state.customers.at(key),
            forall|key: int| !#[trigger] next_orders.contains(key) ==> order_count(db2, key) == state.orders.at(key),
            forall|bucket: int| #[trigger] histogram_changes.at(bucket)
                == histogram_count(db2, bucket) - histogram_count(state.db(), bucket),
            forall|bucket: int| #[trigger] next_histogram.contains(bucket)
                == (has_key(histogram_changes.keys().take(index as int), bucket) && histogram_changes.at(bucket) != 0),
            forall|bucket: int| #[trigger] next_histogram.contains(bucket) ==> {
                &&& next_histogram.at(bucket) == histogram_count(db2, bucket)
                &&& 0 <= next_histogram.at(bucket) <= i64::MAX
            },
        decreases histogram_changes.keys().len() - index,
    {
        let bucket = histogram_changes.key(index);
        let change = histogram_changes.get(bucket);
        if change != 0 {
            let value = match state.histogram.get(bucket).checked_add(change) {
                Some(value) => value,
                None => return Err(1),
            };
            if value < 0 || value > i64::MAX as i128 { return Err(2); }
            next_histogram.set(bucket, value);
        }
        proof { lemma_has_key_step(histogram_changes.keys(), index as int); }
        index += 1;
    }
    proof {
        assert(histogram_changes.keys().take(index as int) =~= histogram_changes.keys());
        assert forall|bucket: int| !#[trigger] next_histogram.contains(bucket) implies
            histogram_count(db2, bucket) == state.histogram.at(bucket) by {
            histogram_changes.lemma_support(bucket);
            assert(histogram_changes.at(bucket) == 0);
        }
    }
    let customers = state.customers.extend(next_customers);
    let orders = state.orders.extend(next_orders);
    let histogram = state.histogram.extend(next_histogram);
    proof {
        assert forall|key: int| #[trigger] customers.at(key) == customer_count(db2, key) by {}
        assert forall|key: int| #[trigger] orders.at(key) == order_count(db2, key) by {}
        assert forall|bucket: int| #[trigger] histogram.at(bucket) == histogram_count(db2, bucket) by {}
    }
    Ok(State {
        customers,
        orders,
        histogram,
        db: Ghost(db2),
    })
}

}

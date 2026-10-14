use vstd::prelude::*;
use std::collections::HashMap;
use verus_common::bytes::*;
use verus_common::sums::*;
use crate::gen_spec::*;
use crate::gen_obligations::*;
use crate::pages::*;
use crate::moments::*;
use crate::algebra::*;
use crate::query_proofs::*;

verus! {

broadcast use { vstd::std_specs::hash::group_hash_axioms, vstd::seq_lib::group_seq_properties };

pub open spec fn scratch_value(values: Map<u128, Moment>, key: u128) -> (int, int) {
    if values.dom().contains(key) { values[key].model() } else { (0, 0) }
}

pub open spec fn order_change(values: Map<i64, i128>, key: i64) -> int {
    if values.dom().contains(key) { values[key] as int } else { 0 }
}

pub open spec fn order_delta_sum(rows: Seq<OrdersDelta>, key: i64) -> int {
    sum(orders_delta_seq_view(rows), |entry: (OrdersRow, int)| entry.1 * ind(entry.0.o_custkey == key))
}

pub open spec fn crossing_model(before: Pages, after: Pages, crossings: Map<i64, i128>, keys: Seq<i64>) -> bool {
    &&& forall|key: i64| #[trigger] crossings.dom().contains(key) <==>
        (before.value(key as u64 as u128).0 == 0) != (after.value(key as u64 as u128).0 == 0)
    &&& forall|key: i64| crossings.dom().contains(key) ==> #[trigger] crossings[key] ==
        ind(after.value(key as u64 as u128).0 == 0) - ind(before.value(key as u64 as u128).0 == 0)
    &&& forall|key: i64| #[trigger] crossings.dom().contains(key) ==> keys.contains(key)
}

pub open spec fn order_delta_model(before: Pages, after: Pages, delta: Seq<OrdersDelta>) -> bool {
    forall|key: i64| #[trigger] after.value(key as u64 as u128) ==
        (before.value(key as u64 as u128).0 + order_delta_sum(delta, key), 0int)
}

pub struct State {
    database: Ghost<Db>,
    orders: Pages,
    positive: Pages,
    histogram: Pages,
}

impl State {
    pub closed spec fn db(self) -> Db { self.database@ }

    #[verifier::type_invariant]
    pub closed spec fn valid(self) -> bool {
        &&& forall|key: i64| #[trigger] self.orders.value(key as u64 as u128) == (orders_at(self.db(), key), 0int)
        &&& forall|code: u32| code <= 65792 ==>
            #[trigger] self.positive.value(code as u128) == positive_moment(self.db(), code)
        &&& forall|code: u32, depth: u32, interval: u64|
            code <= 65792 && depth <= 50 && interval < (1u64 << depth) ==>
            #[trigger] self.histogram.value(histogram_key(code, depth, interval)) == histogram_moment(self.db(), code, depth, interval)
        &&& forall|index: int| 0 <= index < self.db().customer.len() ==>
            (#[trigger] self.db().customer[index]).c_acctbal < LIMIT
        &&& forall|index: int| 0 <= index < self.db().customer.len() && (#[trigger] self.db().customer[index]).c_acctbal > 0 ==>
            i64::MIN <= self.db().customer[index].c_custkey <= i64::MAX
        &&& forall|index: int| 0 <= index < self.db().orders.len() ==>
            i64::MIN <= (#[trigger] self.db().orders[index]).o_custkey <= i64::MAX
    }
}

proof fn lemma_orders_eligibility(db: Db, orders: Pages, row: CustomerRow)
    requires i64::MIN <= row.c_custkey <= i64::MAX,
        forall|key: i64| #[trigger] orders.value(key as u64 as u128) == (orders_at(db, key), 0int),
    ensures eligible(orders, row) == !exists_1(db, row),
{
    let key = row.c_custkey as i64;
    assert(key as int == row.c_custkey);
    assert(orders.value(key as u64 as u128).0 == orders_at(db, key));
    lemma_sum_support(db.orders, |order: OrdersRow| ind(order.o_custkey == key),
        |order: OrdersRow| order.o_custkey == row.c_custkey);
}

proof fn lemma_db_histogram(db: Db, orders: Pages, code: u32, depth: u32, interval: u64)
    requires forall|key: i64| #[trigger] orders.value(key as u64 as u128) == (orders_at(db, key), 0int),
        forall|index: int| 0 <= index < db.customer.len() && (#[trigger] db.customer[index]).c_acctbal > 0 ==>
            i64::MIN <= db.customer[index].c_custkey <= i64::MAX,
    ensures histogram_moment(db, code, depth, interval) ==
        moment_sum(db.customer, |row: CustomerRow| interval_weight(row, code, depth, interval) && eligible(orders, row)),
{
    let first = |row: CustomerRow| histogram_weight(db, row, code, depth, interval);
    let second = |row: CustomerRow| interval_weight(row, code, depth, interval) && eligible(orders, row);
    assert forall|index: int| 0 <= index < db.customer.len() implies first(#[trigger] db.customer[index]) == second(db.customer[index]) by {
        if db.customer[index].c_acctbal > 0 { lemma_orders_eligibility(db, orders, db.customer[index]); }
    }
    lemma_moment_ext(db.customer, first, second);
}

proof fn lemma_seq_i64_key(keys: Seq<i64>, key: i64)
    ensures keys.contains(key) == seq_view_i64(keys).contains(key as int),
{
    if keys.contains(key) {
        let index = choose|index: int| 0 <= index < keys.len() && (#[trigger] keys[index]) == key;
        assert(seq_view_i64(keys)[index] == key as int);
    }
    if seq_view_i64(keys).contains(key as int) {
        let index = choose|index: int| 0 <= index < seq_view_i64(keys).len() && (#[trigger] seq_view_i64(keys)[index]) == key as int;
        assert(keys[index] == key);
    }
}

proof fn lemma_customer_prefix(rows: Seq<CustomerInput>, index: int, predicate: spec_fn(CustomerRow) -> bool)
    requires 0 <= index < rows.len(),
    ensures moment_sum(customer_input_seq_view(rows.take(index + 1)), predicate) ==
        plus(moment_sum(customer_input_seq_view(rows.take(index)), predicate),
            (ind(predicate(customer_input_view(rows[index]))),
                if predicate(customer_input_view(rows[index])) { rows[index].c_acctbal as int } else { 0int })),
{
    lemma_mapped_prefix(rows, index, |row: CustomerInput| customer_input_view(row), |row: CustomerRow| ind(predicate(row)));
    lemma_mapped_prefix(rows, index, |row: CustomerInput| customer_input_view(row), |row: CustomerRow| if predicate(row) { row.c_acctbal } else { 0 });
}

proof fn lemma_delta_prefix(rows: Seq<CustomerDelta>, index: int, predicate: spec_fn(CustomerRow) -> bool)
    requires 0 <= index < rows.len(),
    ensures signed_moment(customer_delta_seq_view(rows.take(index + 1)), predicate) ==
        plus(signed_moment(customer_delta_seq_view(rows.take(index)), predicate),
            (rows[index].sign * ind(predicate(customer_input_view(rows[index].row))),
                if predicate(customer_input_view(rows[index].row)) { rows[index].sign * rows[index].row.c_acctbal } else { 0int })),
{
    let view = |delta: CustomerDelta| (customer_input_view(delta.row), delta.sign as int);
    lemma_mapped_prefix(rows, index, view, |entry: (CustomerRow, int)| entry.1 * ind(predicate(entry.0)));
    lemma_mapped_prefix(rows, index, view, |entry: (CustomerRow, int)| if predicate(entry.0) { entry.1 * entry.0.c_acctbal } else { 0 });
}

proof fn lemma_crossing_prefix(rows: Seq<CustomerInput>, index: int, before: Pages, after: Pages,
    code: u32, depth: u32, interval: u64)
    requires 0 <= index < rows.len(),
    ensures crossing_moment(customer_input_seq_view(rows.take(index + 1)), before, after, code, depth, interval) ==
        plus(crossing_moment(customer_input_seq_view(rows.take(index)), before, after, code, depth, interval),
            if interval_weight(customer_input_view(rows[index]), code, depth, interval) {
                (ind(eligible(after, customer_input_view(rows[index]))) - ind(eligible(before, customer_input_view(rows[index]))),
                 (ind(eligible(after, customer_input_view(rows[index]))) - ind(eligible(before, customer_input_view(rows[index])))) * rows[index].c_acctbal)
            } else { (0int, 0int) }),
{
    lemma_mapped_prefix(rows, index, |row: CustomerInput| customer_input_view(row), |row: CustomerRow|
        if interval_weight(row, code, depth, interval) { ind(eligible(after, row)) - ind(eligible(before, row)) } else { 0 });
    lemma_mapped_prefix(rows, index, |row: CustomerInput| customer_input_view(row), |row: CustomerRow|
        if interval_weight(row, code, depth, interval) { (ind(eligible(after, row)) - ind(eligible(before, row))) * row.c_acctbal } else { 0 });
}

proof fn lemma_build_fact(db: Db, orders: Pages, positive: Pages, histogram: Pages,
    order_rows: Seq<OrderInput>, customer_rows: Seq<CustomerInput>)
    requires scan_order_customer_keys_ok(db, order_rows), scan_positive_customers_ok(db, customer_rows),
        forall|position: int| 0 <= position < customer_rows.len() ==> (#[trigger] customer_rows[position]).c_acctbal < LIMIT,
        forall|key: i64| #[trigger] orders.value(key as u64 as u128) ==
            (sum(order_input_seq_view(order_rows), |row: OrdersRow| ind(row.o_custkey == key)), 0int),
        forall|code: u32| code <= 65792 ==> #[trigger] positive.value(code as u128) ==
            moment_sum(customer_input_seq_view(customer_rows), |row: CustomerRow| positive_weight(row, code)),
        forall|code: u32, depth: u32, interval: u64|
            code <= 65792 && depth <= 50 && interval < (1u64 << depth) ==>
            #[trigger] histogram.value(histogram_key(code, depth, interval)) ==
                moment_sum(customer_input_seq_view(customer_rows),
                    |row: CustomerRow| interval_weight(row, code, depth, interval) && eligible(orders, row)),
    ensures (State { database: Ghost(db), orders, positive, histogram }).valid(),
{
    assert forall|key: i64| #[trigger] orders.value(key as u64 as u128) == (orders_at(db, key), 0int) by {
        let weight = |row: OrdersRow| ind(row.o_custkey == key);
        assert(sum(order_input_seq_view(order_rows), weight) == sum(db.orders, weight));
    }
    let invalid_order = |row: OrdersRow| ind(!(i64::MIN <= row.o_custkey <= i64::MAX));
    lemma_sum_zero(order_input_seq_view(order_rows), invalid_order);
    assert(sum(order_input_seq_view(order_rows), invalid_order) == sum(db.orders, invalid_order));
    assert forall|index: int| 0 <= index < db.orders.len() implies i64::MIN <= (#[trigger] db.orders[index]).o_custkey <= i64::MAX by {
        if !(i64::MIN <= db.orders[index].o_custkey <= i64::MAX) { lemma_sum_pos(db.orders, invalid_order, index); }
    }
    let filter = |row: CustomerRow| row.c_acctbal > 0;
    assert forall|weight: spec_fn(CustomerRow) -> int| #[trigger] sum(customer_input_seq_view(customer_rows), weight) ==
        sum(db.customer, |row: CustomerRow| if filter(row) { weight(row) } else { 0 }) by {
        assert(sum(customer_input_seq_view(customer_rows), weight) == sum(db.customer, |row: CustomerRow| if row.c_acctbal > 0 { weight(row) } else { 0 }));
        lemma_sum_ext_all(db.customer, |row: CustomerRow| if row.c_acctbal > 0 { weight(row) } else { 0 },
            |row: CustomerRow| if filter(row) { weight(row) } else { 0 });
    }
    lemma_scan_property(db.customer, customer_input_seq_view(customer_rows), |row: CustomerRow| row.c_acctbal > 0,
        |row: CustomerRow| row.c_acctbal < LIMIT && i64::MIN <= row.c_custkey <= i64::MAX);
    assert forall|index: int| 0 <= index < db.customer.len() implies (#[trigger] db.customer[index]).c_acctbal < LIMIT by { }
    assert forall|code: u32| code <= 65792 implies #[trigger] positive.value(code as u128) == positive_moment(db, code) by {
        let count = |row: CustomerRow| ind(positive_weight(row, code));
        let total = |row: CustomerRow| if positive_weight(row, code) { row.c_acctbal } else { 0int };
        lemma_moment_weights(customer_input_seq_view(customer_rows), |row: CustomerRow| positive_weight(row, code), count, total);
        lemma_moment_weights(db.customer, |row: CustomerRow| positive_weight(row, code), count, total);
        assert(sum(customer_input_seq_view(customer_rows), count) == sum(db.customer, |row: CustomerRow| if row.c_acctbal > 0 { count(row) } else { 0 }));
        assert(sum(customer_input_seq_view(customer_rows), total) == sum(db.customer, |row: CustomerRow| if row.c_acctbal > 0 { total(row) } else { 0 }));
        lemma_sum_ext_all(db.customer, count, |row: CustomerRow| if row.c_acctbal > 0 { count(row) } else { 0 });
        lemma_sum_ext_all(db.customer, total, |row: CustomerRow| if row.c_acctbal > 0 { total(row) } else { 0 });
    }
    assert forall|code: u32, depth: u32, interval: u64|
        code <= 65792 && depth <= 50 && interval < (1u64 << depth) implies
        #[trigger] histogram.value(histogram_key(code, depth, interval)) == histogram_moment(db, code, depth, interval) by {
        let count = |row: CustomerRow| ind(interval_weight(row, code, depth, interval) && eligible(orders, row));
        let total = |row: CustomerRow| if interval_weight(row, code, depth, interval) && eligible(orders, row) { row.c_acctbal } else { 0int };
        let database_count = |row: CustomerRow| ind(histogram_weight(db, row, code, depth, interval));
        let database_total = |row: CustomerRow| if histogram_weight(db, row, code, depth, interval) { row.c_acctbal } else { 0int };
        lemma_moment_weights(customer_input_seq_view(customer_rows),
            |row: CustomerRow| interval_weight(row, code, depth, interval) && eligible(orders, row), count, total);
        lemma_moment_weights(db.customer, |row: CustomerRow| histogram_weight(db, row, code, depth, interval), database_count, database_total);
        assert(sum(customer_input_seq_view(customer_rows), count) == sum(db.customer, |row: CustomerRow| if row.c_acctbal > 0 { count(row) } else { 0 }));
        assert(sum(customer_input_seq_view(customer_rows), total) == sum(db.customer, |row: CustomerRow| if row.c_acctbal > 0 { total(row) } else { 0 }));
        assert forall|index: int| 0 <= index < db.customer.len() implies
            database_count(#[trigger] db.customer[index]) == if db.customer[index].c_acctbal > 0 { count(db.customer[index]) } else { 0 } by {
            if db.customer[index].c_acctbal > 0 { lemma_orders_eligibility(db, orders, db.customer[index]); }
        }
        assert forall|index: int| 0 <= index < db.customer.len() implies
            database_total(#[trigger] db.customer[index]) == if db.customer[index].c_acctbal > 0 { total(db.customer[index]) } else { 0 } by { }
        lemma_sum_ext(db.customer, database_count, |row: CustomerRow| if row.c_acctbal > 0 { count(row) } else { 0 });
        lemma_sum_ext(db.customer, database_total, |row: CustomerRow| if row.c_acctbal > 0 { total(row) } else { 0 });
    }
}

proof fn lemma_apply_fact(state: State, db2: Db, orders: Pages, positive: Pages, histogram: Pages,
    customer_delta: Seq<CustomerDelta>, orders_delta: Seq<OrdersDelta>, rescan_keys: Seq<i64>, rescan_rows: Seq<CustomerInput>)
    requires state.valid(),
        capture_ok(state.db(), db2, customer_delta_seq_view(customer_delta), orders_delta_seq_view(orders_delta)),
        rescan_positive_customers_by_key_ok(db2, rescan_keys, rescan_rows),
        forall|position: int| 0 <= position < customer_delta.len() ==> (#[trigger] customer_delta[position]).row.c_acctbal < LIMIT,
        order_delta_model(state.orders, orders, orders_delta),
        forall|key: i64| (state.orders.value(key as u64 as u128).0 == 0) != (orders.value(key as u64 as u128).0 == 0) ==>
            #[trigger] rescan_keys.contains(key),
        forall|code: u32| code <= 65792 ==> #[trigger] positive.value(code as u128) ==
            plus(state.positive.value(code as u128), signed_moment(customer_delta_seq_view(customer_delta), |row: CustomerRow| positive_weight(row, code))),
        forall|code: u32, depth: u32, interval: u64|
            code <= 65792 && depth <= 50 && interval < (1u64 << depth) ==>
            #[trigger] histogram.value(histogram_key(code, depth, interval)) ==
                plus(state.histogram.value(histogram_key(code, depth, interval)),
                    plus(signed_moment(customer_delta_seq_view(customer_delta),
                        |row: CustomerRow| interval_weight(row, code, depth, interval) && eligible(state.orders, row)),
                        crossing_moment(customer_input_seq_view(rescan_rows), state.orders, orders, code, depth, interval))),
    ensures (State { database: Ghost(db2), orders, positive, histogram }).valid(),
{
    let before = state.db();
    let delta = customer_delta_seq_view(customer_delta);
    assert forall|key: i64| #[trigger] orders.value(key as u64 as u128) == (orders_at(db2, key), 0int) by {
        let weight = |row: OrdersRow| ind(row.o_custkey == key);
        assert(sum(db2.orders, weight) == sum(before.orders, weight) +
            sum(orders_delta_seq_view(orders_delta), |entry: (OrdersRow, int)| entry.1 * weight(entry.0)));
        lemma_sum_ext_all(orders_delta_seq_view(orders_delta), |entry: (OrdersRow, int)| entry.1 * weight(entry.0),
            |entry: (OrdersRow, int)| entry.1 * ind(entry.0.o_custkey == key));
    }
    lemma_capture_property(before.orders, db2.orders, orders_delta_seq_view(orders_delta),
        |row: OrdersRow| i64::MIN <= row.o_custkey <= i64::MAX);
    let good = |row: CustomerRow| row.c_acctbal < LIMIT &&
        (row.c_acctbal > 0 ==> i64::MIN <= row.c_custkey <= i64::MAX);
    lemma_capture_property(before.customer, db2.customer, delta, good);
    assert forall|code: u32| code <= 65792 implies #[trigger] positive.value(code as u128) == positive_moment(db2, code) by {
        lemma_capture_moment(before.customer, db2.customer, delta, |row: CustomerRow| positive_weight(row, code));
    }
    assert forall|code: u32, depth: u32, interval: u64|
        code <= 65792 && depth <= 50 && interval < (1u64 << depth) implies
        #[trigger] histogram.value(histogram_key(code, depth, interval)) == histogram_moment(db2, code, depth, interval) by {
        lemma_db_histogram(before, state.orders, code, depth, interval);
        lemma_db_histogram(db2, orders, code, depth, interval);
        lemma_capture_histogram(before.customer, db2.customer, delta, state.orders, orders, code, depth, interval);
        let count = |row: CustomerRow| if interval_weight(row, code, depth, interval) {
            ind(eligible(orders, row)) - ind(eligible(state.orders, row))
        } else { 0int };
        let total = |row: CustomerRow| if interval_weight(row, code, depth, interval) {
            (ind(eligible(orders, row)) - ind(eligible(state.orders, row))) * row.c_acctbal
        } else { 0int };
        let filtered_count = |row: CustomerRow| if row.c_acctbal > 0 && seq_view_i64(rescan_keys).contains(row.c_custkey) { count(row) } else { 0int };
        let filtered_total = |row: CustomerRow| if row.c_acctbal > 0 && seq_view_i64(rescan_keys).contains(row.c_custkey) { total(row) } else { 0int };
        assert(sum(customer_input_seq_view(rescan_rows), count) == sum(db2.customer, filtered_count));
        assert(sum(customer_input_seq_view(rescan_rows), total) == sum(db2.customer, filtered_total));
        assert forall|index: int| 0 <= index < db2.customer.len() implies count(#[trigger] db2.customer[index]) == filtered_count(db2.customer[index]) by {
            let row = db2.customer[index];
            if row.c_acctbal > 0 {
                let key = row.c_custkey as i64;
                assert(key as int == row.c_custkey);
                lemma_seq_i64_key(rescan_keys, key);
                if !rescan_keys.contains(key) {
                    assert(eligible(orders, row) == eligible(state.orders, row));
                }
            }
        }
        assert forall|index: int| 0 <= index < db2.customer.len() implies total(#[trigger] db2.customer[index]) == filtered_total(db2.customer[index]) by {
            assert(count(db2.customer[index]) == filtered_count(db2.customer[index]));
        }
        lemma_sum_ext(db2.customer, count, filtered_count);
        lemma_sum_ext(db2.customer, total, filtered_total);
        assert(crossing_moment(customer_input_seq_view(rescan_rows), state.orders, orders, code, depth, interval) ==
            crossing_moment(db2.customer, state.orders, orders, code, depth, interval));
    }
}

pub fn build(order_customer_keys: &[OrderInput], positive_customers: Vec<CustomerInput>, Ghost(db): Ghost<Db>)
    -> (result: Option<State>)
    requires scan_order_customer_keys_ok(db, order_customer_keys@), scan_positive_customers_ok(db, positive_customers@),
    ensures result matches Some(state) ==> state.db() == db,
{
    let mut orders = Pages::new();
    let mut positive = Pages::new();
    let mut histogram = Pages::new();
    let mut index = 0usize;
    let mut processed = Ghost(0int);
    proof { reveal(State::valid); }
    while index < order_customer_keys.len()
        invariant index <= order_customer_keys.len(), processed@ == index as int,
            forall|key: i64| #[trigger] orders.value(key as u64 as u128) ==
                (sum(order_input_seq_view(order_customer_keys@.take(processed@)), |row: OrdersRow| ind(row.o_custkey == key)), 0int),
        decreases order_customer_keys.len() - index,
    {
        let previous = Ghost(orders);
        if !orders.add(order_customer_keys[index].o_custkey as u64 as u128, Moment { count: 1, total: 0 }) { return None; }
        proof {
            assert forall|key: i64| #[trigger] orders.value(key as u64 as u128) ==
                (sum(order_input_seq_view(order_customer_keys@.take(processed@ + 1)), |row: OrdersRow| ind(row.o_custkey == key)), 0int) by {
                assert(previous@.value(key as u64 as u128) ==
                    (sum(order_input_seq_view(order_customer_keys@.take(processed@)), |row: OrdersRow| ind(row.o_custkey == key)), 0int));
                lemma_signed_key_injective(key, order_customer_keys@[index as int].o_custkey);
                lemma_mapped_prefix(order_customer_keys@, processed@, |row: OrderInput| order_input_view(row),
                    |row: OrdersRow| ind(row.o_custkey == key));
            }
        }
        index += 1;
        processed = Ghost(index as int);
    }
    proof { assert(order_customer_keys@.take(processed@) =~= order_customer_keys@); }
    index = 0;
    processed = Ghost(0int);
    while index < positive_customers.len()
        invariant index <= positive_customers.len(), processed@ == index as int,
            forall|position: int| 0 <= position < processed@ ==> (#[trigger] positive_customers@[position]).c_acctbal < LIMIT,
            forall|key: i64| #[trigger] orders.value(key as u64 as u128) ==
                (sum(order_input_seq_view(order_customer_keys@), |row: OrdersRow| ind(row.o_custkey == key)), 0int),
            forall|code: u32| code <= 65792 ==> #[trigger] positive.value(code as u128) ==
                moment_sum(customer_input_seq_view(positive_customers@.take(processed@)), |row: CustomerRow| positive_weight(row, code)),
            forall|code: u32, depth: u32, interval: u64|
                code <= 65792 && depth <= 50 && interval < (1u64 << depth) ==>
                #[trigger] histogram.value(histogram_key(code, depth, interval)) ==
                    moment_sum(customer_input_seq_view(positive_customers@.take(processed@)),
                        |row: CustomerRow| interval_weight(row, code, depth, interval) && eligible(orders, row)),
        decreases positive_customers.len() - index,
    {
        let previous_positive = Ghost(positive);
        let previous_histogram = Ghost(histogram);
        let row = &positive_customers[index];
        if row.c_acctbal > 0 {
            if row.c_acctbal >= LIMIT { return None; }
            let code = prefix(&row.c_phone);
            if !positive.add(code as u128, Moment { count: 1, total: row.c_acctbal as i128 }) { return None; }
            if orders.get(row.c_custkey as u64 as u128).count == 0 {
                if !update_histogram(&mut histogram, code, row.c_acctbal, 1) { return None; }
            }
        }
        proof {
            let input = customer_input_view(positive_customers@[index as int]);
            assert forall|code: u32| code <= 65792 implies #[trigger] positive.value(code as u128) ==
                moment_sum(customer_input_seq_view(positive_customers@.take(processed@ + 1)), |row: CustomerRow| positive_weight(row, code)) by {
                assert(previous_positive@.value(code as u128) ==
                    moment_sum(customer_input_seq_view(positive_customers@.take(processed@)), |row: CustomerRow| positive_weight(row, code)));
                if row.c_acctbal > 0 {
                    lemma_country_injective(prefix_spec(row.c_phone@), code);
                }
                lemma_customer_prefix(positive_customers@, processed@, |row: CustomerRow| positive_weight(row, code));
            }
            assert forall|code: u32, depth: u32, interval: u64|
                code <= 65792 && depth <= 50 && interval < (1u64 << depth) implies
                #[trigger] histogram.value(histogram_key(code, depth, interval)) ==
                moment_sum(customer_input_seq_view(positive_customers@.take(processed@ + 1)),
                        |row: CustomerRow| interval_weight(row, code, depth, interval) && eligible(orders, row)) by {
                lemma_histogram_key_bound(code, depth, interval);
                assert(previous_histogram@.value(histogram_key(code, depth, interval)) ==
                    moment_sum(customer_input_seq_view(positive_customers@.take(processed@)),
                        |row: CustomerRow| interval_weight(row, code, depth, interval) && eligible(orders, row)));
                if row.c_acctbal > 0 {
                    lemma_country_injective(prefix_spec(row.c_phone@), code);
                    lemma_ancestor_at(prefix_spec(row.c_phone@), row.c_acctbal, 1, code, depth, interval);
                }
                lemma_customer_prefix(positive_customers@, processed@,
                    |row: CustomerRow| interval_weight(row, code, depth, interval) && eligible(orders, row));
            }
        }
        index += 1;
        processed = Ghost(index as int);
    }
    proof {
        assert(positive_customers@.take(processed@) =~= positive_customers@);
        lemma_build_fact(db, orders, positive, histogram, order_customer_keys@, positive_customers@);
    }
    Some(State { database: Ghost(db), orders, positive, histogram })
}

fn bytes_before(left: &[u8], right: &[u8]) -> (result: bool)
    ensures result == bytes_lt(left@, right@),
{
    bytes_lt_exec(left, right)
}

fn find_insert_position(output: &[(Vec<u8>, i64, i128)], text: &[u8]) -> (position: usize)
    requires
        forall|i: int, j: int| 0 <= i < j < out_view(output@).len() ==>
            !same_group(#[trigger] out_view(output@)[i], #[trigger] out_view(output@)[j]) &&
            !row_lt(out_view(output@)[j], out_view(output@)[i]),
        forall|i: int| 0 <= i < out_view(output@).len() ==>
            (#[trigger] out_view(output@)[i]).cntrycode != text@,
    ensures
        position <= output.len(),
        forall|i: int| 0 <= i < position as int ==>
            bytes_lt((#[trigger] out_view(output@)[i]).cntrycode, text@),
        position < output.len() ==>
            bytes_lt(text@, out_view(output@)[position as int].cntrycode),
{
    let mut position = 0usize;
    while position < output.len()
        invariant position <= output.len(),
            forall|i: int| 0 <= i < out_view(output@).len() ==>
                (#[trigger] out_view(output@)[i]).cntrycode != text@,
            forall|i: int| 0 <= i < position as int ==>
                bytes_lt((#[trigger] out_view(output@)[i]).cntrycode, text@),
        decreases output.len() - position,
    {
        if bytes_before(text, &output[position].0) {
            proof {
                assert(bytes_lt(text@, out_view(output@)[position as int].cntrycode));
            }
            return position;
        }
        proof {
            assert(0 <= position as int);
            assert((position as int) < out_view(output@).len());
            assert((#[trigger] out_view(output@)[position as int]).cntrycode != text@);
            assert(!bytes_lt(text@, out_view(output@)[position as int].cntrycode));
            lemma_bytes_lt_total(text@, out_view(output@)[position as int].cntrycode);
            assert(bytes_lt(out_view(output@)[position as int].cntrycode, text@));
        }
        position += 1;
    }
    position
}

fn insert_output(output: &mut Vec<(Vec<u8>, i64, i128)>, row: (Vec<u8>, i64, i128), position: usize)
    requires position <= old(output)@.len(),
    ensures final(output)@ == old(output)@.insert(position as int, row),
        out_view(final(output)@) == out_view(old(output)@).insert(position as int,
            OutRow { cntrycode: row.0@, numcust: row.1 as int, totacctbal: row.2 as int }),
{
    output.insert(position, row);
    proof {
        assert(final(output)@ =~= old(output)@.insert(position as int, row));
        assert(out_view(final(output)@) =~= out_view(old(output)@).insert(position as int,
            OutRow { cntrycode: row.0@, numcust: row.1 as int, totacctbal: row.2 as int }));
    }
}

fn add_parameter(codes: &mut Vec<u32>, parameter: &[u8])
    requires old(codes)@.no_duplicates(),
        forall|position: int| 0 <= position < old(codes)@.len() ==> (#[trigger] old(codes)@[position]) <= 65792,
    ensures final(codes)@.no_duplicates(),
        forall|position: int| 0 <= position < final(codes)@.len() ==> (#[trigger] final(codes)@[position]) <= 65792,
        forall|code: u32| #[trigger] final(codes)@.contains(code) <==> old(codes)@.contains(code) ||
            (parameter.len() <= 2 && code == prefix_spec(parameter@)),
{
    if parameter.len() > 2 { return; }
    let code = prefix(parameter);
    let mut index = 0usize;
    while index < codes.len()
        invariant index <= codes.len(), codes@ == old(codes)@, codes@.no_duplicates(), code == prefix_spec(parameter@),
            parameter.len() <= 2,
            forall|position: int| 0 <= position < codes.len() ==> (#[trigger] codes@[position]) <= 65792,
            forall|position: int| 0 <= position < index ==> (#[trigger] codes@[position]) != code,
        decreases codes.len() - index,
    {
        if codes[index] == code { return; }
        index += 1;
    }
    proof { assert(!codes@.contains(code)); }
    codes.push(code);
}

pub fn execute(state: &State, p1: &[u8], p2: &[u8], p3: &[u8], p4: &[u8], p5: &[u8], p6: &[u8], p7: &[u8])
    -> (result: Option<Vec<(Vec<u8>, i64, i128)>>)
    ensures result matches Some(out) ==> query_result(state.db(), params(p1@, p2@, p3@, p4@, p5@, p6@, p7@), out_view(out@)),
{
    proof { use_type_invariant(state); }
    let parameters = Ghost(params(p1@, p2@, p3@, p4@, p5@, p6@, p7@));
    let mut codes = Vec::new();
    add_parameter(&mut codes, p1);
    add_parameter(&mut codes, p2);
    add_parameter(&mut codes, p3);
    add_parameter(&mut codes, p4);
    add_parameter(&mut codes, p5);
    add_parameter(&mut codes, p6);
    add_parameter(&mut codes, p7);
    proof { assert(codes_cover(codes@, parameters@)); }
    let mut population = Moment::zero();
    let mut index = 0usize;
    while index < codes.len()
        invariant index <= codes.len(), codes@.no_duplicates(), codes_cover(codes@, parameters@), state.valid(),
            parameters@ == params(p1@, p2@, p3@, p4@, p5@, p6@, p7@),
            population.model() == population_sums(state.positive, codes@.take(index as int)),
            forall|position: int| 0 <= position < codes.len() ==> (#[trigger] codes@[position]) <= 65792,
        decreases codes.len() - index,
    {
        population = match population.add(state.positive.get(codes[index] as u128)) { Some(value) => value, None => return None };
        proof {
            lemma_sum_take(codes@, index as int, |code: u32| state.positive.value(code as u128).0);
            lemma_sum_take(codes@, index as int, |code: u32| state.positive.value(code as u128).1);
        }
        index += 1;
    }
    proof {
        assert(codes@.take(index as int) =~= codes@);
        lemma_population(state.db(), state.positive, codes@, parameters@);
        assert(scalar_1_group_sum(state.db(), parameters@,
            |entry: Scalar1Tuple| entry.customer_2.c_acctbal) == population.total as int);
    }
    let mut output: Vec<(Vec<u8>, i64, i128)> = Vec::new();
    if population.count == 0 {
        proof { lemma_empty_result(state.db(), parameters@); }
        return Some(output);
    }
    if population.count < 0 || population.total <= 0 { return None; }
    proof {
        lemma_population(state.db(), state.positive, codes@, parameters@);
        assert(scalar_1_group_sum(state.db(), parameters@,
            |entry: Scalar1Tuple| entry.customer_2.c_acctbal) > 0);
    }
    let threshold = population.total / population.count;
    proof { assert(output_part(state.db(), parameters@, state.histogram, output@, codes@, 0, threshold)); }
    index = 0;
    while index < codes.len()
        invariant index <= codes.len(), codes@.no_duplicates(), codes_cover(codes@, parameters@),
            state.valid(),
            threshold >= 0,
            threshold == scalar_1_group_sum(state.db(), parameters@,
                |entry: Scalar1Tuple| entry.customer_2.c_acctbal) /
                scalar_1_group_count(state.db(), parameters@),
            scalar_1_group_count(state.db(), parameters@) > 0,
            scalar_1_group_sum(state.db(), parameters@,
                |entry: Scalar1Tuple| entry.customer_2.c_acctbal) > 0,
            output_part(state.db(), parameters@, state.histogram, output@, codes@, index as int, threshold),
            forall|position: int| 0 <= position < codes.len() ==> (#[trigger] codes@[position]) <= 65792,
        decreases codes.len() - index,
    {
        let code = codes[index];
        let value = match suffix(&state.histogram, code, threshold) { Some(value) => value, None => return None };
        if value.count < 0 || value.count > i64::MAX as i128 { return None; }
        if value.count > 0 {
            let text = decode(code);
            let old_rows = Ghost(output@);
            let old_out = Ghost(out_view(output@));
            proof {
                lemma_output_country_distinct(state.db(), parameters@, state.histogram,
                    output@, codes@, index as int, threshold, code);
                assert(text@ == country(code));
                assert forall|i: int| 0 <= i < out_view(output@).len() implies
                    (#[trigger] out_view(output@)[i]).cntrycode != text@ by {
                    assert(out_view(output@)[i].cntrycode != country(code));
                }
                assert forall|i: int, j: int| 0 <= i < j < out_view(output@).len() implies
                    !same_group(#[trigger] out_view(output@)[i], #[trigger] out_view(output@)[j]) &&
                    !row_lt(out_view(output@)[j], out_view(output@)[i]) by { }
            }
            let position = find_insert_position(&output, &text);
            proof {
                assert(value.model() == suffix_moment(state.histogram, code, threshold, 50));
                if position < output.len() {
                    assert(output_part(state.db(), parameters@, state.histogram,
                        output@, codes@, index as int, threshold));
                    lemma_output_tail(state.db(), parameters@, state.histogram,
                        output@, codes@, index as int, threshold,
                        position as int, text@);
                }
                assert forall|i: int| position as int <= i < output@.len() implies
                    bytes_lt(text@, (#[trigger] out_view(output@)[i]).cntrycode) by {
                    if position < output.len() {
                        assert(bytes_lt(text@, out_view(output@)[i].cntrycode));
                    } else {
                        assert(position as int == output@.len());
                    }
                }
                lemma_code_matches(code, codes@, parameters@);
                let total = scalar_1_group_sum(state.db(), parameters@,
                    |entry: Scalar1Tuple| entry.customer_2.c_acctbal);
                let count = scalar_1_group_count(state.db(), parameters@);
                lemma_suffix_group(state.db(), state.histogram, code, threshold,
                    parameters@, total, count);
                assert(row_ok(state.db(), parameters@,
                    OutRow { cntrycode: text@, numcust: value.count as int, totacctbal: value.total as int }));
            }
            insert_output(&mut output, (text, value.count as i64, value.total), position);
            proof {
                let inserted = OutRow { cntrycode: text@, numcust: value.count as int, totacctbal: value.total as int };
                lemma_output_insert_view(state.db(), parameters@, state.histogram,
                    old_rows@, old_out@, codes@, index as int, threshold,
                    code, (text, value.count as i64, value.total),
                    inserted, position as int, output@);
                assert(output_part(state.db(), parameters@, state.histogram,
                    output@, codes@, (index as int) + 1, threshold));
            }
        }
        index += 1;
    }
    proof {
        assert(codes@.take(index as int) =~= codes@);
        lemma_execute_final(state.db(), parameters@, state.histogram, output@, codes@, threshold);
    }
    Some(output)
}

fn order_changes(rows: &[OrdersDelta]) -> (result: Option<(HashMap<i64, i128>, Vec<i64>)>)
    ensures result matches Some((counts, keys)) ==> (
        keys@.no_duplicates() && counts@.dom() == keys@.to_set() &&
        forall|key: i64| #[trigger] order_change(counts@, key) == order_delta_sum(rows@, key)),
{
    let mut counts: HashMap<i64, i128> = HashMap::new();
    let mut keys = Vec::new();
    let mut index = 0usize;
    let mut processed = Ghost(0int);
    while index < rows.len()
        invariant index <= rows.len(), processed@ == index as int,
            keys@.no_duplicates(), counts@.dom() == keys@.to_set(),
            forall|key: i64| #[trigger] order_change(counts@, key) == order_delta_sum(rows@.take(processed@), key),
        decreases rows.len() - index,
    {
        let previous = Ghost(counts@);
        let previous_keys = Ghost(keys@);
        let key = rows[index].row.o_custkey;
        let old_count = match counts.get(&key) { Some(value) => *value, None => { keys.push(key); 0 } };
        let count = match old_count.checked_add(rows[index].sign as i128) { Some(value) => value, None => return None };
        counts.insert(key, count);
        proof {
            if !previous@.dom().contains(key) { lemma_push_set(previous_keys@, key); }
            assert forall|other: i64| #[trigger] order_change(counts@, other) == order_delta_sum(rows@.take(processed@ + 1), other) by {
                assert(order_change(previous@, other) == order_delta_sum(rows@.take(processed@), other));
                lemma_mapped_prefix(rows@, processed@, |delta: OrdersDelta| (order_input_view(delta.row), delta.sign as int),
                    |entry: (OrdersRow, int)| entry.1 * ind(entry.0.o_custkey == other));
            }
        }
        index += 1;
        processed = Ghost(index as int);
    }
    proof { assert(rows@.take(processed@) =~= rows@); }
    Some((counts, keys))
}

pub fn rescan_positive_customers_by_key_keys(state: &State, customer_delta: &Vec<CustomerDelta>, orders_delta: &[OrdersDelta])
    -> (result: Vec<i64>)
{
    let mut result = Vec::new();
    let (changes, keys) = match order_changes(orders_delta) { Some(value) => value, None => return result };
    let mut index = 0usize;
    while index < keys.len()
        invariant index <= keys.len(),
        decreases keys.len() - index,
    {
        let key = keys[index];
        let before = state.orders.get(key as u64 as u128).count;
        let change = match changes.get(&key) { Some(value) => *value, None => 0 };
        if let Some(after) = before.checked_add(change) {
            if after >= 0 && (before == 0) != (after == 0) { result.push(key); }
        }
        index += 1;
    }
    result
}

fn contains_key(keys: &[i64], key: i64) -> (result: bool)
    ensures result == keys@.contains(key),
{
    let mut index = 0usize;
    while index < keys.len()
        invariant index <= keys.len(),
            forall|position: int| 0 <= position < index ==> (#[trigger] keys@[position]) != key,
        decreases keys.len() - index,
    {
        if keys[index] == key { return true; }
        index += 1;
    }
    false
}

fn finish_changes(target: &mut Pages, changes: &HashMap<u128, Moment>, keys: &Vec<u128>) -> (result: bool)
    requires keys@.no_duplicates(), changes@.dom() == keys@.to_set(),
        forall|index: int| 0 <= index < keys.len() ==> (#[trigger] keys@[index]) < KEY_LIMIT,
    ensures result ==> forall|key: u128| key < KEY_LIMIT ==>
        #[trigger] final(target).value(key) == plus(old(target).value(key), scratch_value(changes@, key)),
{
    let initial = Ghost(*target);
    let mut index = 0usize;
    while index < keys.len()
        invariant index <= keys.len(), initial@ == *old(target),
            keys@.no_duplicates(), changes@.dom() == keys@.to_set(),
            forall|position: int| 0 <= position < keys.len() ==> (#[trigger] keys@[position]) < KEY_LIMIT,
            forall|key: u128| key < KEY_LIMIT ==> #[trigger] target.value(key) ==
                if keys@.take(index as int).contains(key) { plus(initial@.value(key), scratch_value(changes@, key)) }
                else { initial@.value(key) },
        decreases keys.len() - index,
    {
        let key = keys[index];
        proof { assert(!keys@.take(index as int).contains(key)); }
        let change = match changes.get(&key) { Some(value) => *value, None => Moment::zero() };
        let after = match target.get(key).add(change) { Some(value) => value, None => return false };
        if after.count < 0 || after.total < 0 || (after.count == 0) != (after.total == 0) { return false; }
        if change.count != 0 || change.total != 0 { target.set(key, after); }
        proof {
            assert(keys@.take(index as int + 1) =~= keys@.take(index as int).push(key));
            assert forall|other: u128| other < KEY_LIMIT implies #[trigger] target.value(other) ==
                if keys@.take(index as int + 1).contains(other) { plus(initial@.value(other), scratch_value(changes@, other)) }
                else { initial@.value(other) } by {
                if other == key { assert(scratch_value(changes@, other) == change.model()); }
            }
        }
        index += 1;
    }
    proof { assert(keys@.take(index as int) =~= keys@); }
    true
}

fn scratch_add(changes: &mut HashMap<u128, Moment>, keys: &mut Vec<u128>, key: u128, change: Moment) -> (result: bool)
    requires old(keys)@.no_duplicates(), old(changes)@.dom() == old(keys)@.to_set(), key < KEY_LIMIT,
        forall|position: int| 0 <= position < old(keys)@.len() ==> (#[trigger] old(keys)@[position]) < KEY_LIMIT,
    ensures result ==> (
        final(keys)@.no_duplicates() && final(changes)@.dom() == final(keys)@.to_set() &&
        (forall|position: int| 0 <= position < final(keys)@.len() ==> (#[trigger] final(keys)@[position]) < KEY_LIMIT) &&
        final(changes)@.dom() == old(changes)@.dom().insert(key) &&
        forall|other: u128| #[trigger] scratch_value(final(changes)@, other) ==
            if other == key { plus(scratch_value(old(changes)@, other), change.model()) }
            else { scratch_value(old(changes)@, other) }),
{
    let original = Ghost(changes@);
    let original_keys = Ghost(keys@);
    let before = match changes.get(&key) { Some(value) => *value, None => { keys.push(key); Moment::zero() } };
    match before.add(change) {
        Some(value) => {
            changes.insert(key, value);
            proof {
                if !original@.dom().contains(key) {
                    lemma_push_set(original_keys@, key);
                    assert(keys@.no_duplicates());
                    assert(keys@.to_set() =~= original_keys@.to_set().insert(key));
                }
                assert forall|other: u128| #[trigger] scratch_value(changes@, other) ==
                    if other == key { plus(scratch_value(original@, other), change.model()) }
                    else { scratch_value(original@, other) } by { }
            }
            true
        },
        None => false,
    }
}

fn scratch_histogram(changes: &mut HashMap<u128, Moment>, keys: &mut Vec<u128>, code: u32, balance: i64, sign: i128)
    -> (result: bool)
    requires code <= 65792, 0 < balance < LIMIT, -1 <= sign <= 1,
        old(keys)@.no_duplicates(), old(changes)@.dom() == old(keys)@.to_set(),
        forall|position: int| 0 <= position < old(keys)@.len() ==> (#[trigger] old(keys)@[position]) < KEY_LIMIT,
    ensures result ==> (
        final(keys)@.no_duplicates() && final(changes)@.dom() == final(keys)@.to_set() &&
        (forall|position: int| 0 <= position < final(keys)@.len() ==> (#[trigger] final(keys)@[position]) < KEY_LIMIT) &&
        forall|key: u128| #[trigger] scratch_value(final(changes)@, key) ==
            plus(scratch_value(old(changes)@, key), ancestor_moment(code, balance, sign, key, 51))),
{
    let initial = Ghost(changes@);
    let mut depth = 0u32;
    while depth <= 50
        invariant depth <= 51, keys@.no_duplicates(), changes@.dom() == keys@.to_set(),
            initial@ == old(changes)@, code <= 65792, 0 < balance < LIMIT, -1 <= sign <= 1,
            forall|position: int| 0 <= position < keys@.len() ==> (#[trigger] keys@[position]) < KEY_LIMIT,
            forall|key: u128| #[trigger] scratch_value(changes@, key) ==
                plus(scratch_value(initial@, key), ancestor_moment(code, balance, sign, key, depth as int)),
        decreases 51 - depth,
    {
        proof { lemma_interval_bound(balance, depth); lemma_signed_balance(sign, balance); }
        let key = hkey(code, depth, (balance as u64) >> (50 - depth));
        if !scratch_add(changes, keys, key, Moment { count: sign, total: sign * balance as i128 }) { return false; }
        proof {
            assert forall|other: u128| #[trigger] scratch_value(changes@, other) ==
                plus(scratch_value(initial@, other), ancestor_moment(code, balance, sign, other, depth as int + 1)) by {
                lemma_ancestor_step(code, balance, sign, other, depth);
            }
        }
        depth += 1;
    }
    true
}

pub fn apply(state: &State, customer_delta: &Vec<CustomerDelta>, orders_delta: &[OrdersDelta],
    positive_customers_by_key_keys: &[i64], positive_customers_by_key_rows: &Vec<CustomerInput>, Ghost(db2): Ghost<Db>)
    -> (result: Result<State, u32>)
    requires capture_ok(state.db(), db2, customer_delta_seq_view(customer_delta@), orders_delta_seq_view(orders_delta@)),
        rescan_positive_customers_by_key_ok(db2, positive_customers_by_key_keys@, positive_customers_by_key_rows@),
    ensures result matches Ok(next) ==> next.db() == db2,
{
    proof { use_type_invariant(state); }
    let (changes, keys) = match order_changes(orders_delta) { Some(value) => value, None => return Err(1) };
    let mut orders = state.orders.share();
    let mut crossings: HashMap<i64, i128> = HashMap::new();
    let mut index = 0usize;
    while index < keys.len()
        invariant index <= keys.len(), keys@.no_duplicates(), changes@.dom() == keys@.to_set(), state.valid(),
            forall|key: i64| #[trigger] order_change(changes@, key) == order_delta_sum(orders_delta@, key),
            forall|key: i64| #[trigger] orders.value(key as u64 as u128) ==
                (state.orders.value(key as u64 as u128).0 + if keys@.take(index as int).contains(key) { order_change(changes@, key) } else { 0int }, 0int),
            forall|key: i64| #[trigger] crossings@.dom().contains(key) <==>
                keys@.take(index as int).contains(key) &&
                (state.orders.value(key as u64 as u128).0 == 0) != (state.orders.value(key as u64 as u128).0 + order_change(changes@, key) == 0),
            forall|key: i64| crossings@.dom().contains(key) ==> #[trigger] crossings@[key] ==
                ind(state.orders.value(key as u64 as u128).0 + order_change(changes@, key) == 0) - ind(state.orders.value(key as u64 as u128).0 == 0),
            forall|key: i64| #[trigger] crossings@.dom().contains(key) ==> positive_customers_by_key_keys@.contains(key),
        decreases keys.len() - index,
    {
        let previous = Ghost(orders);
        let key = keys[index];
        proof { assert(!keys@.take(index as int).contains(key)); }
        let before = state.orders.get(key as u64 as u128).count;
        let change = match changes.get(&key) { Some(value) => *value, None => 0 };
        let after = match before.checked_add(change) { Some(value) => value, None => return Err(1) };
        if after < 0 { return Err(2); }
        if (before == 0) != (after == 0) {
            if !contains_key(positive_customers_by_key_keys, key) { return Err(3); }
            crossings.insert(key, if after == 0 { 1 } else { -1 });
        }
        if change != 0 { orders.set(key as u64 as u128, Moment { count: after, total: 0 }); }
        proof {
            assert(keys@.take(index as int + 1) =~= keys@.take(index as int).push(key));
            assert forall|other: i64| #[trigger] orders.value(other as u64 as u128) ==
                (state.orders.value(other as u64 as u128).0 + if keys@.take(index as int + 1).contains(other) { order_change(changes@, other) } else { 0int }, 0int) by {
                lemma_signed_key_injective(key, other);
                assert(previous@.value(other as u64 as u128) ==
                    (state.orders.value(other as u64 as u128).0 + if keys@.take(index as int).contains(other) { order_change(changes@, other) } else { 0int }, 0int));
            }
        }
        index += 1;
    }
    proof { assert(keys@.take(index as int) =~= keys@); }
    proof {
        assert(crossing_model(state.orders, orders, crossings@, positive_customers_by_key_keys@));
        assert forall|key: i64| #[trigger] orders.value(key as u64 as u128) ==
            (state.orders.value(key as u64 as u128).0 + order_delta_sum(orders_delta@, key), 0int) by {
            assert(order_change(changes@, key) == order_delta_sum(orders_delta@, key));
            assert(orders.value(key as u64 as u128) ==
                (state.orders.value(key as u64 as u128).0 + if keys@.contains(key) { order_change(changes@, key) } else { 0int }, 0int));
            if !keys@.contains(key) { assert(!changes@.dom().contains(key)); }
        }
    }
    let mut pchanges: HashMap<u128, Moment> = HashMap::new();
    let mut pkeys = Vec::new();
    let mut hchanges: HashMap<u128, Moment> = HashMap::new();
    let mut hkeys = Vec::new();
    index = 0;
    let mut processed = Ghost(0int);
    while index < customer_delta.len()
        invariant index <= customer_delta.len(), processed@ == index as int,
            state.valid(),
            order_delta_model(state.orders, orders, orders_delta@),
            capture_ok(state.db(), db2, customer_delta_seq_view(customer_delta@), orders_delta_seq_view(orders_delta@)),
            rescan_positive_customers_by_key_ok(db2, positive_customers_by_key_keys@, positive_customers_by_key_rows@),
            crossing_model(state.orders, orders, crossings@, positive_customers_by_key_keys@),
            forall|position: int| 0 <= position < processed@ ==> (#[trigger] customer_delta@[position]).row.c_acctbal < LIMIT,
            pkeys@.no_duplicates(), pchanges@.dom() == pkeys@.to_set(),
            hkeys@.no_duplicates(), hchanges@.dom() == hkeys@.to_set(),
            forall|position: int| 0 <= position < pkeys@.len() ==> (#[trigger] pkeys@[position]) < KEY_LIMIT,
            forall|position: int| 0 <= position < hkeys@.len() ==> (#[trigger] hkeys@[position]) < KEY_LIMIT,
            forall|code: u32| code <= 65792 ==> #[trigger] scratch_value(pchanges@, code as u128) ==
                signed_moment(customer_delta_seq_view(customer_delta@.take(processed@)), |row: CustomerRow| positive_weight(row, code)),
            forall|code: u32, depth: u32, interval: u64|
                code <= 65792 && depth <= 50 && interval < (1u64 << depth) ==>
                #[trigger] scratch_value(hchanges@, histogram_key(code, depth, interval)) ==
                    signed_moment(customer_delta_seq_view(customer_delta@.take(processed@)),
                        |row: CustomerRow| interval_weight(row, code, depth, interval) && eligible(state.orders, row)),
        decreases customer_delta.len() - index,
    {
        let previous_positive = Ghost(pchanges@);
        let previous_histogram = Ghost(hchanges@);
        let delta = &customer_delta[index];
        if delta.sign != -1 && delta.sign != 1 { return Err(4); }
        let row = &delta.row;
        if row.c_acctbal > 0 {
            if row.c_acctbal >= LIMIT { return Err(5); }
            let code = prefix(&row.c_phone);
            let sign = delta.sign as i128;
            proof { lemma_signed_balance(sign, row.c_acctbal); }
            if !scratch_add(&mut pchanges, &mut pkeys, code as u128,
                Moment { count: sign, total: sign * row.c_acctbal as i128 }) { return Err(1); }
            if state.orders.get(row.c_custkey as u64 as u128).count == 0 {
                if !scratch_histogram(&mut hchanges, &mut hkeys, code, row.c_acctbal, sign) { return Err(1); }
            }
        }
        proof {
            assert forall|code: u32| code <= 65792 implies #[trigger] scratch_value(pchanges@, code as u128) ==
                signed_moment(customer_delta_seq_view(customer_delta@.take(processed@ + 1)), |row: CustomerRow| positive_weight(row, code)) by {
                assert(scratch_value(previous_positive@, code as u128) ==
                    signed_moment(customer_delta_seq_view(customer_delta@.take(processed@)), |row: CustomerRow| positive_weight(row, code)));
                if row.c_acctbal > 0 { lemma_country_injective(prefix_spec(row.c_phone@), code); }
                lemma_delta_prefix(customer_delta@, processed@, |row: CustomerRow| positive_weight(row, code));
            }
            assert forall|code: u32, depth: u32, interval: u64|
                code <= 65792 && depth <= 50 && interval < (1u64 << depth) implies
                #[trigger] scratch_value(hchanges@, histogram_key(code, depth, interval)) ==
                    signed_moment(customer_delta_seq_view(customer_delta@.take(processed@ + 1)),
                        |row: CustomerRow| interval_weight(row, code, depth, interval) && eligible(state.orders, row)) by {
                assert(scratch_value(previous_histogram@, histogram_key(code, depth, interval)) ==
                    signed_moment(customer_delta_seq_view(customer_delta@.take(processed@)),
                        |row: CustomerRow| interval_weight(row, code, depth, interval) && eligible(state.orders, row)));
                if row.c_acctbal > 0 {
                    lemma_country_injective(prefix_spec(row.c_phone@), code);
                    lemma_ancestor_at(prefix_spec(row.c_phone@), row.c_acctbal, delta.sign as i128, code, depth, interval);
                }
                let input = customer_input_view(delta.row);
                let predicate = |row: CustomerRow| interval_weight(row, code, depth, interval) && eligible(state.orders, row);
                if row.c_acctbal > 0 && eligible(state.orders, input) {
                    assert(scratch_value(hchanges@, histogram_key(code, depth, interval)) ==
                        plus(scratch_value(previous_histogram@, histogram_key(code, depth, interval)),
                            ancestor_moment(prefix_spec(row.c_phone@), row.c_acctbal, delta.sign as i128, histogram_key(code, depth, interval), 51)));
                } else {
                    assert(scratch_value(hchanges@, histogram_key(code, depth, interval)) == scratch_value(previous_histogram@, histogram_key(code, depth, interval)));
                }
                lemma_delta_prefix(customer_delta@, processed@,
                    |row: CustomerRow| interval_weight(row, code, depth, interval) && eligible(state.orders, row));
            }
        }
        index += 1;
        processed = Ghost(index as int);
    }
    proof { assert(customer_delta@.take(processed@) =~= customer_delta@); }
    index = 0;
    processed = Ghost(0int);
    while index < positive_customers_by_key_rows.len()
        invariant index <= positive_customers_by_key_rows.len(), processed@ == index as int,
            state.valid(), order_delta_model(state.orders, orders, orders_delta@),
            capture_ok(state.db(), db2, customer_delta_seq_view(customer_delta@), orders_delta_seq_view(orders_delta@)),
            rescan_positive_customers_by_key_ok(db2, positive_customers_by_key_keys@, positive_customers_by_key_rows@),
            forall|position: int| 0 <= position < customer_delta@.len() ==> (#[trigger] customer_delta@[position]).row.c_acctbal < LIMIT,
            crossing_model(state.orders, orders, crossings@, positive_customers_by_key_keys@),
            hkeys@.no_duplicates(), hchanges@.dom() == hkeys@.to_set(),
            pkeys@.no_duplicates(), pchanges@.dom() == pkeys@.to_set(),
            forall|position: int| 0 <= position < pkeys@.len() ==> (#[trigger] pkeys@[position]) < KEY_LIMIT,
            forall|position: int| 0 <= position < hkeys@.len() ==> (#[trigger] hkeys@[position]) < KEY_LIMIT,
            forall|code: u32| code <= 65792 ==> #[trigger] scratch_value(pchanges@, code as u128) ==
                signed_moment(customer_delta_seq_view(customer_delta@), |row: CustomerRow| positive_weight(row, code)),
            forall|code: u32, depth: u32, interval: u64|
                code <= 65792 && depth <= 50 && interval < (1u64 << depth) ==>
                #[trigger] scratch_value(hchanges@, histogram_key(code, depth, interval)) ==
                    plus(signed_moment(customer_delta_seq_view(customer_delta@),
                        |row: CustomerRow| interval_weight(row, code, depth, interval) && eligible(state.orders, row)),
                        crossing_moment(customer_input_seq_view(positive_customers_by_key_rows@.take(processed@)),
                            state.orders, orders, code, depth, interval)),
        decreases positive_customers_by_key_rows.len() - index,
    {
        let previous = Ghost(hchanges@);
        let row = &positive_customers_by_key_rows[index];
        if row.c_acctbal > 0 {
            if row.c_acctbal >= LIMIT { return Err(5); }
            if let Some(sign) = crossings.get(&row.c_custkey) {
                proof { assert(-1 <= *sign <= 1); }
                let code = prefix(&row.c_phone);
                if !scratch_histogram(&mut hchanges, &mut hkeys, code, row.c_acctbal, *sign) { return Err(1); }
            }
        }
        proof {
            assert forall|code: u32, depth: u32, interval: u64|
                code <= 65792 && depth <= 50 && interval < (1u64 << depth) implies
                #[trigger] scratch_value(hchanges@, histogram_key(code, depth, interval)) ==
                    plus(signed_moment(customer_delta_seq_view(customer_delta@),
                        |row: CustomerRow| interval_weight(row, code, depth, interval) && eligible(state.orders, row)),
                        crossing_moment(customer_input_seq_view(positive_customers_by_key_rows@.take(processed@ + 1)),
                            state.orders, orders, code, depth, interval)) by {
                lemma_histogram_key_bound(code, depth, interval);
                assert(scratch_value(previous@, histogram_key(code, depth, interval)) ==
                    plus(signed_moment(customer_delta_seq_view(customer_delta@),
                        |row: CustomerRow| interval_weight(row, code, depth, interval) && eligible(state.orders, row)),
                        crossing_moment(customer_input_seq_view(positive_customers_by_key_rows@.take(processed@)),
                            state.orders, orders, code, depth, interval)));
                if row.c_acctbal > 0 {
                    lemma_prefix_model(row.c_phone@);
                    lemma_country_injective(prefix_spec(row.c_phone@), code);
                    assert(crossings@.dom().contains(row.c_custkey) <==>
                        eligible(state.orders, customer_input_view(*row)) != eligible(orders, customer_input_view(*row)));
                    if crossings@.dom().contains(row.c_custkey) {
                        lemma_ancestor_at(prefix_spec(row.c_phone@), row.c_acctbal, crossings@[row.c_custkey], code, depth, interval);
                        assert(scratch_value(hchanges@, histogram_key(code, depth, interval)) ==
                            plus(scratch_value(previous@, histogram_key(code, depth, interval)),
                                ancestor_moment(prefix_spec(row.c_phone@), row.c_acctbal, crossings@[row.c_custkey], histogram_key(code, depth, interval), 51)));
                    } else {
                        assert(scratch_value(hchanges@, histogram_key(code, depth, interval)) == scratch_value(previous@, histogram_key(code, depth, interval)));
                    }
                }
                let input = customer_input_view(*row);
                let difference = ind(eligible(orders, input)) - ind(eligible(state.orders, input));
                assert(scratch_value(hchanges@, histogram_key(code, depth, interval)) ==
                    plus(scratch_value(previous@, histogram_key(code, depth, interval)),
                        if interval_weight(input, code, depth, interval) { (difference, difference * row.c_acctbal) } else { (0int, 0int) }));
                lemma_crossing_prefix(positive_customers_by_key_rows@, processed@, state.orders, orders, code, depth, interval);
            }
        }
        index += 1;
        processed = Ghost(index as int);
    }
    proof { assert(positive_customers_by_key_rows@.take(processed@) =~= positive_customers_by_key_rows@); }
    let mut positive = state.positive.share();
    let mut histogram = state.histogram.share();
    if !finish_changes(&mut positive, &pchanges, &pkeys) { return Err(6); }
    if !finish_changes(&mut histogram, &hchanges, &hkeys) { return Err(6); }
    proof {
        assert forall|code: u32, depth: u32, interval: u64|
            code <= 65792 && depth <= 50 && interval < (1u64 << depth) implies
            #[trigger] histogram.value(histogram_key(code, depth, interval)) ==
                plus(state.histogram.value(histogram_key(code, depth, interval)),
                    plus(signed_moment(customer_delta_seq_view(customer_delta@),
                        |row: CustomerRow| interval_weight(row, code, depth, interval) && eligible(state.orders, row)),
                        crossing_moment(customer_input_seq_view(positive_customers_by_key_rows@), state.orders, orders, code, depth, interval))) by {
            lemma_histogram_key_bound(code, depth, interval);
        }
        assert forall|key: i64| (state.orders.value(key as u64 as u128).0 == 0) != (orders.value(key as u64 as u128).0 == 0) implies
            #[trigger] positive_customers_by_key_keys@.contains(key) by {
            assert(crossings@.dom().contains(key));
        }
        lemma_apply_fact(*state, db2, orders, positive, histogram, customer_delta@, orders_delta@,
            positive_customers_by_key_keys@, positive_customers_by_key_rows@);
    }
    Ok(State { database: Ghost(db2), orders, positive, histogram })
}

}

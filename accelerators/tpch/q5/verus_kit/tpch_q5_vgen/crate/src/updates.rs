use vstd::prelude::*;
use verus_common::sums::*;
use verus_common::bag::*;
use crate::gen_spec::*;
use crate::gen_obligations::*;
use crate::meaning::*;
use crate::storage::*;
use verus_prooflib::wide::Wide;

verus! {

pub fn customers(before: &Table, changes: &[CustomerDelta], Ghost(db): Ghost<Db>, Ghost(db2): Ghost<Db>) -> (after: Table)
    requires customers_match(*before, db), bag_step(db.customer, db2.customer, customer_delta_seq_view(changes@)),
    ensures customers_match(after, db2),
{
    let mut bucket = Bucket::new();
    let mut index = 0;
    let mut processed: Ghost<int> = Ghost(0int);
    while index < changes.len()
        invariant index <= changes.len(), processed@ == index, bucket.wf(), customers_match(*before, db),
            forall|key: i64, sub: i64, day: i32| #[trigger] bucket.at(key, sub, day) ==
                (sum(changes@.take(processed@), |delta: CustomerDelta| delta.sign * ind(delta.row.c_custkey == key && delta.row.c_nationkey == sub && day == 0)), 0),
        decreases changes.len() - index,
    {
        let delta = &changes[index];
        bucket.add(delta.row.c_custkey, delta.row.c_nationkey, 0, &Wide::from_i128(delta.sign as i128), &Wide::zero());
        proof {
            assert forall|key: i64, sub: i64, day: i32| #[trigger] bucket.at(key, sub, day) ==
                (sum(changes@.take(index as int + 1), |delta: CustomerDelta| delta.sign * ind(delta.row.c_custkey == key && delta.row.c_nationkey == sub && day == 0)), 0) by {
                lemma_sum_take(changes@, index as int, |delta: CustomerDelta| delta.sign * ind(delta.row.c_custkey == key && delta.row.c_nationkey == sub && day == 0));
            }
        }
        index += 1;
        processed = Ghost(index as int);
    }
    let mut after = before.share();
    proof {
        assert(changes@.take(changes@.len() as int) =~= changes@);
        assert forall|key: i64, sub: i64, day: i32| (before.at(key, sub, day).0 + #[trigger] bucket.at(key, sub, day).0, before.at(key, sub, day).1 + bucket.at(key, sub, day).1) ==
            (if day == 0 { customer_count(db2, key, sub) } else { 0 }, 0) by {
            let weight = |row: CustomerRow| ind(row.c_custkey == key && row.c_nationkey == sub && day == 0);
            let delta_weight = |delta: CustomerDelta| delta.sign * ind(delta.row.c_custkey == key && delta.row.c_nationkey == sub && day == 0);
            let captured_weight = |delta: (CustomerRow, int)| delta.1 * weight(delta.0);
            lemma_sum_map(changes@, |delta: CustomerDelta| (customer_input_view(delta.row), delta.sign as int), captured_weight, delta_weight);
            assert(sum(db2.customer, weight) == sum(db.customer, weight) + sum(customer_delta_seq_view(changes@), captured_weight));
            if day == 0 {
                lemma_sum_ext_all(db.customer, weight, |row: CustomerRow| ind(row.c_custkey == key && row.c_nationkey == sub));
                lemma_sum_ext_all(db2.customer, weight, |row: CustomerRow| ind(row.c_custkey == key && row.c_nationkey == sub));
            } else { lemma_sum_zero(db.customer, weight); lemma_sum_zero(db2.customer, weight); }
        }
        assert forall|key: i64, sub: i64, day: i32| before.at(key, sub, day).0 + #[trigger] bucket.at(key, sub, day).0 >= 0 by {
            lemma_sum_nonneg(db2.customer, |row: CustomerRow| ind(row.c_custkey == key && row.c_nationkey == sub));
        }
    }
    merge(&mut after, &bucket, true);
    proof {
        assert forall|key: i64, sub: i64, day: i32| #[trigger] after.at(key, sub, day) ==
            (before.at(key, sub, day).0 + bucket.at(key, sub, day).0, before.at(key, sub, day).1 + bucket.at(key, sub, day).1) by {}
        assert(after.dimension());
    }
    after
}

pub fn suppliers(before: &Table, changes: &[SupplierDelta], Ghost(db): Ghost<Db>, Ghost(db2): Ghost<Db>) -> (after: Table)
    requires suppliers_match(*before, db), bag_step(db.supplier, db2.supplier, supplier_delta_seq_view(changes@)),
    ensures suppliers_match(after, db2),
{
    let mut bucket = Bucket::new();
    let mut index = 0;
    let mut processed: Ghost<int> = Ghost(0int);
    while index < changes.len()
        invariant index <= changes.len(), processed@ == index, bucket.wf(), suppliers_match(*before, db),
            forall|key: i64, sub: i64, day: i32| #[trigger] bucket.at(key, sub, day) ==
                (sum(changes@.take(processed@), |delta: SupplierDelta| delta.sign * ind(delta.row.s_suppkey == key && delta.row.s_nationkey == sub && day == 0)), 0),
        decreases changes.len() - index,
    {
        let delta = &changes[index];
        bucket.add(delta.row.s_suppkey, delta.row.s_nationkey, 0, &Wide::from_i128(delta.sign as i128), &Wide::zero());
        proof {
            assert forall|key: i64, sub: i64, day: i32| #[trigger] bucket.at(key, sub, day) ==
                (sum(changes@.take(index as int + 1), |delta: SupplierDelta| delta.sign * ind(delta.row.s_suppkey == key && delta.row.s_nationkey == sub && day == 0)), 0) by {
                lemma_sum_take(changes@, index as int, |delta: SupplierDelta| delta.sign * ind(delta.row.s_suppkey == key && delta.row.s_nationkey == sub && day == 0));
            }
        }
        index += 1;
        processed = Ghost(index as int);
    }
    let mut after = before.share();
    proof {
        assert(changes@.take(changes@.len() as int) =~= changes@);
        assert forall|key: i64, sub: i64, day: i32| (before.at(key, sub, day).0 + #[trigger] bucket.at(key, sub, day).0, before.at(key, sub, day).1 + bucket.at(key, sub, day).1) ==
            (if day == 0 { supplier_count(db2, key, sub) } else { 0 }, 0) by {
            let weight = |row: SupplierRow| ind(row.s_suppkey == key && row.s_nationkey == sub && day == 0);
            let delta_weight = |delta: SupplierDelta| delta.sign * ind(delta.row.s_suppkey == key && delta.row.s_nationkey == sub && day == 0);
            let captured_weight = |delta: (SupplierRow, int)| delta.1 * weight(delta.0);
            lemma_sum_map(changes@, |delta: SupplierDelta| (supplier_input_view(delta.row), delta.sign as int), captured_weight, delta_weight);
            assert(sum(db2.supplier, weight) == sum(db.supplier, weight) + sum(supplier_delta_seq_view(changes@), captured_weight));
            if day == 0 {
                lemma_sum_ext_all(db.supplier, weight, |row: SupplierRow| ind(row.s_suppkey == key && row.s_nationkey == sub));
                lemma_sum_ext_all(db2.supplier, weight, |row: SupplierRow| ind(row.s_suppkey == key && row.s_nationkey == sub));
            } else { lemma_sum_zero(db.supplier, weight); lemma_sum_zero(db2.supplier, weight); }
        }
        assert forall|key: i64, sub: i64, day: i32| before.at(key, sub, day).0 + #[trigger] bucket.at(key, sub, day).0 >= 0 by {
            lemma_sum_nonneg(db2.supplier, |row: SupplierRow| ind(row.s_suppkey == key && row.s_nationkey == sub));
        }
    }
    merge(&mut after, &bucket, true);
    proof {
        assert forall|key: i64, sub: i64, day: i32| #[trigger] after.at(key, sub, day) ==
            (before.at(key, sub, day).0 + bucket.at(key, sub, day).0, before.at(key, sub, day).1 + bucket.at(key, sub, day).1) by {}
        assert(after.dimension());
    }
    after
}

pub fn orders(before: &Table, changes: &[OrdersDelta], Ghost(db): Ghost<Db>, Ghost(db2): Ghost<Db>) -> (after: Table)
    requires orders_match(*before, db), bag_step(db.orders, db2.orders, orders_delta_seq_view(changes@)),
    ensures orders_match(after, db2),
{
    let mut bucket = Bucket::new();
    let mut index = 0;
    let mut processed: Ghost<int> = Ghost(0int);
    while index < changes.len()
        invariant index <= changes.len(), processed@ == index, bucket.wf(), orders_match(*before, db),
            forall|key: i64, sub: i64, day: i32| #[trigger] bucket.at(key, sub, day) ==
                (sum(changes@.take(processed@), |delta: OrdersDelta| delta.sign * ind(delta.row.o_orderkey == key && delta.row.o_custkey == sub && delta.row.o_orderdate == day)), 0),
        decreases changes.len() - index,
    {
        let delta = &changes[index];
        bucket.add(delta.row.o_orderkey, delta.row.o_custkey, delta.row.o_orderdate, &Wide::from_i128(delta.sign as i128), &Wide::zero());
        proof {
            assert forall|key: i64, sub: i64, day: i32| #[trigger] bucket.at(key, sub, day) ==
                (sum(changes@.take(index as int + 1), |delta: OrdersDelta| delta.sign * ind(delta.row.o_orderkey == key && delta.row.o_custkey == sub && delta.row.o_orderdate == day)), 0) by {
                lemma_sum_take(changes@, index as int, |delta: OrdersDelta| delta.sign * ind(delta.row.o_orderkey == key && delta.row.o_custkey == sub && delta.row.o_orderdate == day));
            }
        }
        index += 1;
        processed = Ghost(index as int);
    }
    let mut after = before.share();
    proof {
        assert(changes@.take(changes@.len() as int) =~= changes@);
        assert forall|key: i64, sub: i64, day: i32| (before.at(key, sub, day).0 + #[trigger] bucket.at(key, sub, day).0, before.at(key, sub, day).1 + bucket.at(key, sub, day).1) ==
            (order_count(db2, key, sub, day), 0) by {
            let weight = |row: OrdersRow| ind(row.o_orderkey == key && row.o_custkey == sub && row.o_orderdate == day);
            let delta_weight = |delta: OrdersDelta| delta.sign * ind(delta.row.o_orderkey == key && delta.row.o_custkey == sub && delta.row.o_orderdate == day);
            let captured_weight = |delta: (OrdersRow, int)| delta.1 * weight(delta.0);
            lemma_sum_map(changes@, |delta: OrdersDelta| (orders_input_view(delta.row), delta.sign as int), captured_weight, delta_weight);
            assert(sum(db2.orders, weight) == sum(db.orders, weight) + sum(orders_delta_seq_view(changes@), captured_weight));
        }
        assert forall|key: i64, sub: i64, day: i32| before.at(key, sub, day).0 + #[trigger] bucket.at(key, sub, day).0 >= 0 by {
            lemma_sum_nonneg(db2.orders, |row: OrdersRow| ind(row.o_orderkey == key && row.o_custkey == sub && row.o_orderdate == day));
        }
    }
    merge(&mut after, &bucket, true);
    proof {
        assert forall|key: i64, sub: i64, day: i32| #[trigger] after.at(key, sub, day) ==
            (before.at(key, sub, day).0 + bucket.at(key, sub, day).0, before.at(key, sub, day).1 + bucket.at(key, sub, day).1) by {}
    }
    after
}

pub fn reverse(before: &Table, changes: &[OrdersDelta], Ghost(db): Ghost<Db>, Ghost(db2): Ghost<Db>) -> (after: Table)
    requires reverse_match(*before, db), bag_step(db.orders, db2.orders, orders_delta_seq_view(changes@)),
    ensures reverse_match(after, db2),
{
    let mut bucket = Bucket::new();
    let mut index = 0;
    let mut processed: Ghost<int> = Ghost(0int);
    while index < changes.len()
        invariant index <= changes.len(), processed@ == index, bucket.wf(), reverse_match(*before, db),
            forall|key: i64, sub: i64, day: i32| #[trigger] bucket.at(key, sub, day) ==
                (sum(changes@.take(processed@), |delta: OrdersDelta| delta.sign * ind(delta.row.o_custkey == key && delta.row.o_orderkey == sub && day == 0)), 0),
        decreases changes.len() - index,
    {
        let delta = &changes[index];
        bucket.add(delta.row.o_custkey, delta.row.o_orderkey, 0, &Wide::from_i128(delta.sign as i128), &Wide::zero());
        proof {
            assert forall|key: i64, sub: i64, day: i32| #[trigger] bucket.at(key, sub, day) ==
                (sum(changes@.take(index as int + 1), |delta: OrdersDelta| delta.sign * ind(delta.row.o_custkey == key && delta.row.o_orderkey == sub && day == 0)), 0) by {
                lemma_sum_take(changes@, index as int, |delta: OrdersDelta| delta.sign * ind(delta.row.o_custkey == key && delta.row.o_orderkey == sub && day == 0));
            }
        }
        index += 1;
        processed = Ghost(index as int);
    }
    let mut after = before.share();
    proof {
        assert(changes@.take(changes@.len() as int) =~= changes@);
        assert forall|key: i64, sub: i64, day: i32| (before.at(key, sub, day).0 + #[trigger] bucket.at(key, sub, day).0, before.at(key, sub, day).1 + bucket.at(key, sub, day).1) ==
            (if day == 0 { order_references(db2, key, sub) } else { 0 }, 0) by {
            let weight = |row: OrdersRow| ind(row.o_custkey == key && row.o_orderkey == sub && day == 0);
            let delta_weight = |delta: OrdersDelta| delta.sign * ind(delta.row.o_custkey == key && delta.row.o_orderkey == sub && day == 0);
            let captured_weight = |delta: (OrdersRow, int)| delta.1 * weight(delta.0);
            lemma_sum_map(changes@, |delta: OrdersDelta| (orders_input_view(delta.row), delta.sign as int), captured_weight, delta_weight);
            assert(sum(db2.orders, weight) == sum(db.orders, weight) + sum(orders_delta_seq_view(changes@), captured_weight));
            if day == 0 {
                lemma_sum_ext_all(db.orders, weight, |row: OrdersRow| ind(row.o_orderkey == sub && row.o_custkey == key));
                lemma_sum_ext_all(db2.orders, weight, |row: OrdersRow| ind(row.o_orderkey == sub && row.o_custkey == key));
            } else { lemma_sum_zero(db.orders, weight); lemma_sum_zero(db2.orders, weight); }
        }
        assert forall|key: i64, sub: i64, day: i32| before.at(key, sub, day).0 + #[trigger] bucket.at(key, sub, day).0 >= 0 by {
            lemma_sum_nonneg(db2.orders, |row: OrdersRow| ind(row.o_orderkey == sub && row.o_custkey == key));
        }
    }
    merge(&mut after, &bucket, true);
    proof {
        assert forall|key: i64, sub: i64, day: i32| #[trigger] after.at(key, sub, day) ==
            (before.at(key, sub, day).0 + bucket.at(key, sub, day).0, before.at(key, sub, day).1 + bucket.at(key, sub, day).1) by {}
        assert(after.dimension());
    }
    after
}
}

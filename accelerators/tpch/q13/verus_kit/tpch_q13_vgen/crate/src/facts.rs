use vstd::prelude::*;
use verus_common::sums::*;
use verus_common::rel::*;
use verus_common::order::*;
use crate::gen_spec::*;
use crate::gen_obligations::*;
use crate::kernel::*;
use verus_common::sqlfns::like_match;
pub use crate::bag_proofs::*;

verus! {

pub open spec fn customer_keys(db: Db) -> Seq<int> {
    dedup(db.customer.map_values(|row: CustomerRow| row.c_custkey))
}

pub open spec fn cast_keys(keys: Seq<i128>) -> Seq<int> {
    keys.map_values(|key: i128| key as int)
}

pub open spec fn contribution(db: Db, p: Seq<u8>, key: int, bucket: int) -> int {
    ind(customer_count(db, key) > 0 && customer_count(db, key) * order_count(db, p, key) == bucket)
}

pub open spec fn customer_change(rows: Seq<CustomerDelta>, key: int) -> int {
    sum(customer_delta_seq_view(rows), |change: (CustomerRow, int)| change.1 * ind(change.0.c_custkey == key))
}

pub open spec fn order_change(rows: Seq<OrderDelta>, p: Seq<u8>, key: int) -> int {
    sum(rows, |change: OrderDelta| change.sign as int * ind(change.o_custkey as int == key && !like_match(change.o_comment@, p)))
}

pub proof fn lemma_capture_counts(before: Db, after: Db, p: Seq<u8>, customers: Seq<CustomerDelta>,
    orders: Seq<OrderDelta>, full: Seq<(OrdersRow, int)>, key: int)
    requires orders_delta_ok(orders, full),
        capture_ok(before, after, customer_delta_seq_view(customers), full),
    ensures customer_count(after, key) == customer_count(before, key) + customer_change(customers, key),
        order_count(after, p, key) == order_count(before, p, key) + order_change(orders, p, key),
{
    let customer_weight = |row: CustomerRow| ind(row.c_custkey == key);
    assert(sum(after.customer, customer_weight) == sum(before.customer, customer_weight)
        + sum(customer_delta_seq_view(customers), |change: (CustomerRow, int)| change.1 * customer_weight(change.0)));
    lemma_sum_ext_all(before.customer, customer_weight, |row: CustomerRow| ind(row.c_custkey == key));
    lemma_sum_ext_all(after.customer, customer_weight, |row: CustomerRow| ind(row.c_custkey == key));
    lemma_sum_ext_all(customer_delta_seq_view(customers),
        |change: (CustomerRow, int)| change.1 * customer_weight(change.0),
        |change: (CustomerRow, int)| change.1 * ind(change.0.c_custkey == key));
    let order_weight = |row: OrdersRow| ind(c_orders_on_orders(before, params(p), CustomerRow { c_custkey: key }, row));
    assert(sum(after.orders, order_weight) == sum(before.orders, order_weight)
        + sum(full, |change: (OrdersRow, int)| change.1 * order_weight(change.0)));
    lemma_sum_ext_all(after.orders, order_weight,
        |row: OrdersRow| ind(c_orders_on_orders(after, params(p), CustomerRow { c_custkey: key }, row)));
    lemma_sum_ext_all(before.orders, order_weight,
        |row: OrdersRow| ind(c_orders_on_orders(before, params(p), CustomerRow { c_custkey: key }, row)));
    let projected = |change: OrderDelta| change.sign as int * ind(change.o_custkey as int == key && !like_match(change.o_comment@, p));
    let actual = |change: (OrdersRow, int)| change.1 * order_weight(change.0);
    assert forall|index: int| 0 <= index < orders.len() implies
        projected(#[trigger] orders[index]) == actual(full[index]) by {}
    lemma_sum_aligned(orders, full, projected, actual);
}

pub proof fn lemma_apply_facts(before: Db, after: Db, p: Seq<u8>, touched: Seq<i128>, bucket: int)
    requires no_dups(touched),
        forall|key: int| !#[trigger] cast_keys(touched).contains(key) ==>
            customer_count(before, key) == customer_count(after, key)
                && order_count(before, p, key) == order_count(after, p, key),
    ensures histogram_count(after, p, bucket) == histogram_count(before, p, bucket)
        + sum(touched, |key: i128| contribution(after, p, key as int, bucket)
            - contribution(before, p, key as int, bucket)),
{
    lemma_cast_keys(touched);
    let changed = touched.map_values(|key: i128| key as int);
    let universe = dedup(customer_keys(before) + customer_keys(after) + changed);
    lemma_contains_concat(customer_keys(before), customer_keys(after));
    lemma_contains_concat(customer_keys(before) + customer_keys(after), changed);
    lemma_dedup(customer_keys(before) + customer_keys(after) + changed);
    lemma_dedup(before.customer.map_values(|row: CustomerRow| row.c_custkey));
    lemma_dedup(after.customer.map_values(|row: CustomerRow| row.c_custkey));
    let old_weight = |key: int| contribution(before, p, key, bucket);
    let new_weight = |key: int| contribution(after, p, key, bucket);
    let difference = |key: int| new_weight(key) - old_weight(key);
    assert forall|key: int| #[trigger] old_weight(key) != 0 implies
        customer_keys(before).contains(key) && universe.contains(key) by {
        lemma_customer_support(before, key);
    }
    assert forall|key: int| #[trigger] new_weight(key) != 0 implies
        customer_keys(after).contains(key) && universe.contains(key) by {
        lemma_customer_support(after, key);
    }
    assert forall|key: int| #[trigger] difference(key) != 0 implies
        changed.contains(key) && universe.contains(key) by {
        if !changed.contains(key) { assert(new_weight(key) == old_weight(key)); }
    }
    lemma_sum_support(customer_keys(before), universe, old_weight);
    lemma_sum_support(customer_keys(after), universe, new_weight);
    lemma_sum_sub(universe, new_weight, old_weight, difference);
    lemma_sum_support(changed, universe, difference);
    lemma_sum_map(touched, |key: i128| key as int, difference,
        |key: i128| contribution(after, p, key as int, bucket) - contribution(before, p, key as int, bucket));
}

pub proof fn lemma_execute_facts(db: Db, p: Seq<u8>, rows: Seq<(i64, i64)>)
    requires
        forall|index: int| 0 <= index < rows.len() ==> {
            &&& (#[trigger] rows[index]).1 > 0
            &&& rows[index].1 as int == histogram_count(db, p, rows[index].0 as int)
        },
        forall|bucket: int| #[trigger] histogram_count(db, p, bucket) > 0 ==>
            exists|index: int| 0 <= index < rows.len() && (#[trigger] rows[index]).0 as int == bucket,
        sorted_by(rows, |left: (i64, i64), right: (i64, i64)| crate::output::better(left, right)),
    ensures query_result(db, params(p), out_view(rows)),
{
    assert forall|bucket: int| #[trigger] group_count(db, params(p), bucket) == histogram_count(db, p, bucket) by {
        crate::join::lemma_query_count(db, p, bucket);
    }
    assert forall|index: int| 0 <= index < out_view(rows).len() implies
        row_ok(db, params(p), #[trigger] out_view(rows)[index]) by {}
    assert forall|first: int, second: int| 0 <= first < second < out_view(rows).len() implies
        !same_group(#[trigger] out_view(rows)[first], #[trigger] out_view(rows)[second])
            && !row_lt(out_view(rows)[second], out_view(rows)[first]) by {
        if rows[first].0 == rows[second].0 { assert(rows[first].1 == rows[second].1); }
    }
    assert forall|bucket: int| #[trigger] group_count(db, params(p), bucket) > 0 implies
        exists|index: int| 0 <= index < out_view(rows).len()
            && (#[trigger] out_view(rows)[index]).c_count == bucket by {
        let index = choose|index: int| 0 <= index < rows.len() && (#[trigger] rows[index]).0 as int == bucket;
        assert(out_view(rows)[index].c_count == bucket);
    }
}

pub proof fn lemma_cast_keys(keys: Seq<i128>)
    requires no_dups(keys),
    ensures no_dups(cast_keys(keys)),
        forall|key: int| #[trigger] cast_keys(keys).contains(key)
            <==> i128::MIN <= key <= i128::MAX && keys.contains(key as i128),
{
    let mapped = keys.map_values(|key: i128| key as int);
    assert forall|key: int| #[trigger] mapped.contains(key)
        <==> i128::MIN <= key <= i128::MAX && keys.contains(key as i128) by {
        if mapped.contains(key) {
            let index = choose|index: int| 0 <= index < mapped.len() && mapped[index] == key;
            assert(keys.contains(keys[index]));
        }
        if i128::MIN <= key <= i128::MAX && keys.contains(key as i128) {
            let index = choose|index: int| 0 <= index < keys.len() && keys[index] == key as i128;
            assert(mapped[index] == key);
        }
    }
}

pub proof fn lemma_customer_support(db: Db, key: int)
    ensures customer_count(db, key) >= 0,
        customer_keys(db).contains(key) <==> customer_count(db, key) > 0,
{
    let keys = db.customer.map_values(|row: CustomerRow| row.c_custkey);
    lemma_dedup(keys);
    let weight = |row: CustomerRow| ind(row.c_custkey == key);
    lemma_indicator(db.customer, weight);
    if keys.contains(key) {
        let index = choose|index: int| 0 <= index < keys.len() && keys[index] == key;
        assert(db.customer[index].c_custkey == key);
        lemma_sum_pos(db.customer, weight, index);
    }
    if customer_count(db, key) > 0 {
        let index = lemma_sum_nonzero_witness(db.customer, weight);
        assert(keys[index] == key);
        assert(keys.contains(key));
    }
}


pub open spec fn has_key(keys: Seq<i128>, key: int) -> bool {
    i128::MIN <= key <= i128::MAX && keys.contains(key as i128)
}

/// The histogram is the sum over any duplicate-free list of exactly the keys with customers.
pub proof fn lemma_seen_hist(db: Db, p: Seq<u8>, seen: Seq<i128>, bucket: int)
    requires no_dups(seen),
        forall|key: int| customer_count(db, key) > 0 <==> has_key(seen, key),
    ensures histogram_count(db, p, bucket) == sum(seen, |key: i128| contribution(db, p, key as int, bucket)),
{
    lemma_cast_keys(seen);
    lemma_dedup(db.customer.map_values(|row: CustomerRow| row.c_custkey));
    let keys = cast_keys(seen);
    let weight = |key: int| contribution(db, p, key, bucket);
    assert forall|key: int| #[trigger] weight(key) != 0 implies
        keys.contains(key) && customer_keys(db).contains(key) by {
        lemma_customer_support(db, key);
        assert(customer_count(db, key) > 0);
        assert(has_key(seen, key));
    }
    lemma_sum_support(keys, customer_keys(db), weight);
    let mapped = |key: i128| weight(key as int);
    lemma_sum_map(seen, |key: i128| key as int, weight, mapped);
    lemma_sum_ext(seen, mapped, |key: i128| contribution(db, p, key as int, bucket));
}


}

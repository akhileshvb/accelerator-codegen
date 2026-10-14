use vstd::prelude::*;
use crate::gen_obligations::*;
use crate::storage::*;
use crate::values::*;
use crate::values::value;
use crate::model::*;
use crate::joins::*;
use verus_common::sums::*;

verus! {
broadcast use vstd::seq_lib::group_seq_properties;

pub open spec fn sparse_member(map: Sparse<Layers<i128>>, customer: i64, order: i64) -> bool {
    match map.at(customer) {
        Some(bucket) => match bucket.at(order) { Some(value) => value > 0, None => false },
        None => false,
    }
}

pub fn build_orders(relevant_orders: &[OrderInput])
    -> (result: Option<(Sparse<Counts>, Sparse<Layers<i128>>)>)
    ensures result matches Some((orders, reverse)) ==> {
        &&& orders.valid() && reverse.valid()
        &&& forall|key: i64, other: i64| #![trigger sparse_count(orders, key, other)]
            sparse_count(orders, key, other) == sum(relevant_orders@, |row: OrderInput|
                if row.o_orderkey == key && row.o_custkey == other { 1int } else { 0int })
        &&& forall|customer: i64, order: i64| #![trigger sparse_member(reverse, customer, order)]
            sparse_member(reverse, customer, order) == (sparse_count(orders, order, customer) > 0)
        &&& forall|key: i64| #![trigger reverse.at(key)]
            reverse.at(key) matches Some(bucket) ==> bucket.valid()
        &&& forall|key: i64| #![trigger orders.at(key)]
            orders.at(key) matches Some(bucket) ==> count_keys(bucket.rows@).no_duplicates()
        &&& forall|key: i64| #![trigger orders.at(key)]
            orders.at(key) matches Some(bucket) ==> counts_positive(bucket)
    },
{
    let empty_counts = Layers::new(Sparse::new());
    let empty_members = Layers::new(Sparse::new());
    let mut order_rows: Sparse<Counts> = Sparse::new();
    let mut reverse_rows: Sparse<Layers<i128>> = Sparse::new();
    let mut index = 0;
    while index < relevant_orders.len()
        invariant index <= relevant_orders@.len(), order_rows.valid(), reverse_rows.valid(),
            forall|key: i64| #![trigger empty_counts.at(key)] empty_counts.at(key).is_none(),
            forall|key: i64| #![trigger patched_counts_unique(empty_counts, order_rows, key)]
                patched_counts_unique(empty_counts, order_rows, key),
            forall|key: i64| #![trigger patched_counts_positive(empty_counts, order_rows, key)]
                patched_counts_positive(empty_counts, order_rows, key),
            forall|key: i64| #![trigger empty_members.at(key)] empty_members.at(key).is_none(),
            forall|key: i64| #![trigger reverse_rows.at(key)]
                reverse_rows.at(key) matches Some(bucket) ==> bucket.valid(),
            forall|key: i64, other: i64| #![trigger sparse_count(order_rows, key, other)]
                sparse_count(order_rows, key, other) >= 0,
            forall|customer: i64, order: i64|
                #![trigger patched_member(empty_members, reverse_rows, customer, order)]
                patched_member(empty_members, reverse_rows, customer, order)
                    == (sparse_count(order_rows, order, customer) > 0),
            forall|key: i64, other: i64| #![trigger sparse_count(order_rows, key, other)]
                sparse_count(order_rows, key, other)
                == sum(relevant_orders@.take(index as int), |row: OrderInput|
                    if row.o_orderkey == key && row.o_custkey == other { 1int } else { 0int }),
        decreases relevant_orders@.len() - index,
    {
        let ghost prefix_rows = order_rows;
        let ghost prefix_reverse = reverse_rows;
        value!(count_patch(&empty_counts, &mut order_rows, relevant_orders[index].o_orderkey, relevant_orders[index].o_custkey, 1));
        value!(membership_patch(&empty_members, &mut reverse_rows, relevant_orders[index].o_custkey, relevant_orders[index].o_orderkey, true));
        proof {
            assert forall|key: i64, other: i64| #![trigger sparse_count(order_rows, key, other)]
                sparse_count(order_rows, key, other) >= 0
                    && sparse_count(order_rows, key, other) == sparse_count(prefix_rows, key, other)
                        + if key == relevant_orders@[index as int].o_orderkey
                            && other == relevant_orders@[index as int].o_custkey { 1int } else { 0int } by {
                assert(patched_count(empty_counts, order_rows, key, other) == sparse_count(order_rows, key, other));
                assert(patched_count(empty_counts, prefix_rows, key, other) == sparse_count(prefix_rows, key, other));
                assert(sparse_count(prefix_rows, key, other) >= 0);
            }
            assert forall|key: i64, other: i64| #![trigger sparse_count(order_rows, key, other)]
                sparse_count(order_rows, key, other) == sum(relevant_orders@.take(index as int + 1),
                    |row: OrderInput| if row.o_orderkey == key && row.o_custkey == other { 1int } else { 0int }) by {
                lemma_sum_take(relevant_orders@, index as int,
                    |row: OrderInput| if row.o_orderkey == key && row.o_custkey == other { 1int } else { 0int });
            }
            assert forall|customer: i64, order: i64|
                #![trigger patched_member(empty_members, reverse_rows, customer, order)]
                patched_member(empty_members, reverse_rows, customer, order)
                    == (sparse_count(order_rows, order, customer) > 0) by {
                assert(patched_member(empty_members, reverse_rows, customer, order)
                    == if customer == relevant_orders@[index as int].o_custkey
                            && order == relevant_orders@[index as int].o_orderkey { true }
                        else { patched_member(empty_members, prefix_reverse, customer, order) });
                assert(sparse_count(order_rows, order, customer)
                    == sparse_count(prefix_rows, order, customer)
                        + if customer == relevant_orders@[index as int].o_custkey
                            && order == relevant_orders@[index as int].o_orderkey { 1int } else { 0int });
            }
        }
        index += 1;
    }
    proof {
        assert(relevant_orders@.take(relevant_orders@.len() as int) == relevant_orders@);
        assert forall|key: i64| #![trigger order_rows.at(key)]
            order_rows.at(key) matches Some(bucket) ==> count_keys(bucket.rows@).no_duplicates() by {
            assert(patched_counts_unique(empty_counts, order_rows, key));
        }
        assert forall|key: i64| #![trigger order_rows.at(key)]
            order_rows.at(key) matches Some(bucket) ==> counts_positive(bucket) by {
            assert(patched_counts_positive(empty_counts, order_rows, key));
        }
        assert forall|customer: i64, order: i64| #![trigger sparse_member(reverse_rows, customer, order)]
            sparse_member(reverse_rows, customer, order) == (sparse_count(order_rows, order, customer) > 0) by {
            assert(sparse_member(reverse_rows, customer, order)
                == patched_member(empty_members, reverse_rows, customer, order));
        }
    }
    Some((order_rows, reverse_rows))
}

pub proof fn lemma_reverse_install(rows: Sparse<Layers<i128>>, layers: Layers<Layers<i128>>, customer: i64, order: i64)
    requires
        layers.at(customer) == normalize(rows.at(customer)),
        rows.at(customer) matches Some(bucket) ==> bucket.valid(),
    ensures member_value(layers, customer, order) == sparse_member(rows, customer, order),
{
    if let Some(bucket) = rows.at(customer) {
        if bucket.empty() { bucket.empty_lookup(order); }
    }
}

pub proof fn lemma_member_install(previous: Layers<Layers<i128>>, changes: Sparse<Layers<i128>>,
    next: Layers<Layers<i128>>, customer: i64, order: i64)
    requires next.at(customer) == if changes.at(customer).is_some() { normalize(changes.at(customer)) } else { previous.at(customer) },
        changes.at(customer) matches Some(bucket) ==> bucket.valid(),
    ensures member_value(next, customer, order) == patched_member(previous, changes, customer, order),
{
    if let Some(bucket) = changes.at(customer) { if bucket.empty() { bucket.empty_lookup(order); } }
}

pub fn clear_reverse_order(previous: &Layers<Layers<i128>>, changes: &mut Sparse<Layers<i128>>, rows: &Counts, order: i64)
    -> (result: Option<()>)
    requires old(changes).valid(),
        forall|customer: i64| #![trigger patched_member(*previous, *old(changes), customer, order)]
            patched_member(*previous, *old(changes), customer, order) ==> count_at(rows.rows@, customer) > 0,
        forall|customer: i64| #![trigger old(changes).at(customer)]
            old(changes).at(customer) matches Some(bucket) ==> bucket.valid(),
    ensures final(changes).valid(),
        forall|customer: i64| #![trigger final(changes).at(customer)]
            final(changes).at(customer) matches Some(bucket) ==> bucket.valid(),
        result.is_some() ==> forall|customer: i64, key: i64| #![trigger patched_member(*previous, *final(changes), customer, key)]
            patched_member(*previous, *final(changes), customer, key) ==
                (key != order && patched_member(*previous, *old(changes), customer, key)),
{
    let ghost initial = *changes;
    let mut offset = 0;
    while offset < rows.rows.len()
        invariant offset <= rows.rows@.len(), changes.valid(), initial == *old(changes),
            forall|customer: i64| #![trigger changes.at(customer)]
                changes.at(customer) matches Some(bucket) ==> bucket.valid(),
            forall|customer: i64, key: i64| #![trigger patched_member(*previous, *changes, customer, key)]
                patched_member(*previous, *changes, customer, key) ==
                    (patched_member(*previous, initial, customer, key)
                        && !(key == order && count_keys(rows.rows@.take(offset as int)).contains(customer))),
        decreases rows.rows@.len() - offset,
    {
        let customer = rows.rows[offset].0;
        value!(membership_patch(previous, changes, customer, order, false));
        proof {
            assert(count_keys(rows.rows@.take(offset as int + 1)) =~= count_keys(rows.rows@.take(offset as int)).push(customer));
            verus_prooflib::lemma_contains_push(count_keys(rows.rows@.take(offset as int)), customer);
        }
        offset += 1;
    }
    proof {
        assert(rows.rows@.take(rows.rows@.len() as int) == rows.rows@);
        assert forall|customer: i64, key: i64| #![trigger patched_member(*previous, *changes, customer, key)]
            patched_member(*previous, *changes, customer, key) ==
                (key != order && patched_member(*previous, initial, customer, key)) by {
            if key == order && patched_member(*previous, initial, customer, key) {
                let weight = |entry: (i64, i128)| if entry.0 == customer { entry.1 as int } else { 0int };
                let position = lemma_sum_nonzero_witness(rows.rows@, weight);
                assert(rows.rows@[position].0 == customer);
                assert(count_keys(rows.rows@)[position] == customer);
                assert(count_keys(rows.rows@).contains(customer));
            }
        }
    }
    Some(())
}

}

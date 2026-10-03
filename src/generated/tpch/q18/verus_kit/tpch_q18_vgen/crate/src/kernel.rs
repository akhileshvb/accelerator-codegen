#![verifier::loop_isolation(false)]
use vstd::prelude::*;
use std::collections::HashMap;
use std::sync::Arc;
use verus_common::sums::*;
use crate::gen_spec::*;
use crate::gen_obligations::*;
use crate::data::*;
use crate::pages::*;
use crate::output::*;
use crate::model::*;
use crate::maintenance::*;
#[path = "kernel_result.rs"]
mod result_proof;

verus! {

pub struct State {
    db: Ghost<Db>,
    orders: Table<OrderData>,
    customers: Table<CustomerData>,
    buckets: Table<KeyBucket>,
    directory: Arc<Vec<i128>>,
}

impl State {
    pub closed spec fn db(self) -> Db { self.db@ }

    pub closed spec fn order_at(self, key: int) -> Option<OrderData> {
        if i64::MIN <= key <= i64::MAX { self.orders.at(key as i128) } else { None }
    }

    pub closed spec fn customer_at(self, key: int) -> Option<CustomerData> {
        if i64::MIN <= key <= i64::MAX { self.customers.at(key as i128) } else { None }
    }

    pub closed spec fn n(self, key: int) -> int { line_count(self.order_at(key)) }
    pub closed spec fn quantity(self, key: int) -> int { line_sum(self.order_at(key)) }

    pub closed spec fn om(self, key: int, customer: int, date: int, price: int) -> int {
        order_count(self.order_at(key), customer, date, price)
    }

    pub closed spec fn cm(self, customer: int, name: Seq<u8>) -> int {
        customer_count(self.customer_at(customer), name)
    }

    pub closed spec fn bucket_has(self, quantity: i128, key: i64) -> bool {
        match self.buckets.at(quantity) { Some(bucket) => bucket.view().contains(key), None => false }
    }

    #[verifier::type_invariant]
    closed spec fn wf(self) -> bool {
        &&& self.orders.wf() && self.customers.wf() && self.buckets.wf()
        &&& forall|key: i128| #[trigger] self.orders.at(key) matches Some(row) ==>
            i64::MIN <= key <= i64::MAX && row.wf()
        &&& forall|key: i128| #[trigger] self.customers.at(key) matches Some(row) ==>
            i64::MIN <= key <= i64::MAX && row.wf()
        &&& forall|key: int| #[trigger] self.n(key) ==
            sum(self.db().lineitem, nw(key))
        &&& forall|key: int| #[trigger] self.quantity(key) ==
            sum(self.db().lineitem, qw(key))
        &&& forall|key: int, customer: int, date: int, price: int| #[trigger] self.om(key, customer, date, price) ==
            sum(self.db().orders, ow(key, customer, date, price))
        &&& forall|customer: int, name: Seq<u8>| #[trigger] self.cm(customer, name) ==
            sum(self.db().customer, cw(customer, name))
        &&& forall|quantity: i128| #[trigger] self.buckets.at(quantity) matches Some(bucket) ==>
            bucket.wf() && bucket.len > 0 && bucket.view().no_duplicates()
        &&& forall|quantity: i128, key: i64| #[trigger] self.bucket_has(quantity, key) ==
            (self.order_at(key as int) matches Some(row) && row.count > 0 &&
                row.variants.len() > 0 && row.quantity == quantity)
        &&& forall|quantity: i128| #[trigger] self.buckets.at(quantity) is Some <==>
            self.directory@.contains(quantity)
        &&& forall|left: int, right: int| 0 <= left < right < self.directory.len() ==>
            #[trigger] self.directory@[left] < #[trigger] self.directory@[right]
    }

    proof fn query_counts(self, parameters: Params, name: Seq<u8>, customer: int, key: int, date: int, price: int)
        requires self.wf(),
        ensures
            group_count(self.db(), parameters, name, customer, key, date, price) ==
                if self.n(key) > 0 && self.quantity(key) > parameters.p1 {
                    self.n(key) * self.om(key, customer, date, price) * self.cm(customer, name)
                } else { 0 },
            group_sum(self.db(), parameters, name, customer, key, date, price, |tuple: Tuple| tuple.lineitem.l_quantity) ==
                if self.n(key) > 0 && self.quantity(key) > parameters.p1 {
                    self.quantity(key) * self.om(key, customer, date, price) * self.cm(customer, name)
                } else { 0 },
    {
        execute_facts(self.db(), parameters, name, customer, key, date, price);
    }
}

pub fn remember(key: i128, seen: &mut HashMap<i128, bool>, keys: &mut Vec<i128>)
    requires old(keys)@.no_duplicates(), old(seen)@.dom() == old(keys)@.to_set(),
    ensures final(keys)@.no_duplicates(), final(seen)@.dom() == final(keys)@.to_set(),
        bounded_keys(old(keys)@) && i64::MIN <= key <= i64::MAX ==> bounded_keys(final(keys)@),
        final(keys)@ == if old(keys)@.contains(key) { old(keys)@ } else { old(keys)@.push(key) },
        final(keys)@.to_set() == old(keys)@.to_set().insert(key),
        forall|other: i128| #[trigger] final(keys)@.contains(other) <==> old(keys)@.contains(other) || other == key,
{
    broadcast use vstd::std_specs::hash::group_hash_axioms, vstd::seq_lib::group_seq_properties;
    if !seen.contains_key(&key) {
        proof { push_set(keys@, key); }
        seen.insert(key, true);
        keys.push(key);
    }
}

pub open spec fn order_sums(orders: Editor<OrderData>, lines: Seq<LineitemRow>, groups: Seq<OrdersRow>) -> bool {
    &&& forall|key: i128| #[trigger] line_count(orders.at(key)) == sum(lines, nw(key as int))
    &&& forall|key: i128| #[trigger] line_sum(orders.at(key)) == sum(lines, qw(key as int))
    &&& forall|key: i128, customer: int, date: int, price: int| #[trigger] order_count(orders.at(key), customer, date, price) ==
        sum(groups, ow(key as int, customer, date, price))
}

pub open spec fn customer_sums(customers: Editor<CustomerData>, names: Seq<CustomerRow>) -> bool {
    forall|key: i128, name: Seq<u8>| #[trigger] customer_count(customers.at(key), name) == sum(names, cw(key as int, name))
}

pub open spec fn remembered(seen: Map<i128, bool>, keys: Seq<i128>) -> bool {
    keys.no_duplicates() && seen.dom() == keys.to_set()
}

pub open spec fn bounded_keys(keys: Seq<i128>) -> bool {
    forall|position: int| 0 <= position < keys.len() ==> i64::MIN <= #[trigger] keys[position] <= i64::MAX
}

pub open spec fn grouped_has(groups: Map<i128, Vec<i64>>, quantity: i128, key: i64) -> bool {
    groups.contains_key(quantity) && groups[quantity]@.contains(key)
}

pub open spec fn coherent(orders: Editor<OrderData>, customers: Editor<CustomerData>) -> bool {
    &&& orders.wf() && customers.wf()
    &&& forall|key: i128| #[trigger] order_shape(orders.at(key))
    &&& forall|key: i128| #[trigger] customer_shape(customers.at(key))
}

pub open spec fn bucket_shape(value: Option<KeyBucket>) -> bool {
    match value { Some(bucket) => bucket.wf(), None => true }
}

pub open spec fn sound_buckets(buckets: Editor<KeyBucket>) -> bool {
    buckets.wf() && forall|quantity: i128| #[trigger] bucket_shape(buckets.at(quantity))
}

pub open spec fn bucket_valid(value: Option<KeyBucket>) -> bool {
    match value { Some(bucket) => bucket.wf() && bucket.len > 0 && bucket.view().no_duplicates(), None => true }
}

pub open spec fn consistent_buckets(buckets: Editor<KeyBucket>) -> bool {
    buckets.wf() && forall|quantity: i128| #[trigger] bucket_valid(buckets.at(quantity))
}

pub open spec fn bucket_member(buckets: Editor<KeyBucket>, quantity: i128, key: i64) -> bool {
    match buckets.at(quantity) { Some(bucket) => bucket.view().contains(key), None => false }
}

pub open spec fn totals(orders: Editor<OrderData>, customers: Editor<CustomerData>, db: Db) -> bool {
    order_sums(orders, db.lineitem, db.orders) && customer_sums(customers, db.customer)
}

pub open spec fn orders_valid_prefix(orders: Editor<OrderData>, keys: Seq<i128>, index: int) -> bool {
    forall|key: i128| !keys.contains(key) || keys.take(index).contains(key) ==> #[trigger] order_valid(orders.at(key), key)
}

pub open spec fn customers_valid_prefix(customers: Editor<CustomerData>, keys: Seq<i128>, index: int) -> bool {
    forall|key: i128| !keys.contains(key) || keys.take(index).contains(key) ==> #[trigger] customer_valid(customers.at(key), key)
}

pub open spec fn directory_value(changed: Option<Vec<i128>>, original: Arc<Vec<i128>>) -> Seq<i128> {
    match changed { Some(values) => values@, None => original@ }
}

pub fn build(
    line_quantities: &[LineQuantityScan],
    order_groups: &[OrderGroupScan],
    customer_names: Vec<CustomerNameScan>,
    Ghost(db): Ghost<Db>,
) -> (r: Option<State>)
    requires scan_line_quantities_ok(db, line_quantities@),
        scan_order_groups_ok(db, order_groups@), scan_customer_names_ok(db, customer_names@),
    ensures r matches Some(s) ==> s.db() == db,
{
    hide(sum);
    hide(Editor::at);
    hide(Editor::wf);
    hide(Table::at);
    hide(Table::wf);
    broadcast use vstd::std_specs::hash::group_hash_axioms, vstd::seq_lib::group_seq_properties;
    proof {
        build_facts(db, line_quantities@, order_groups@, customer_names@);
        empty_sums(line_quantity_scan_seq_view(line_quantities@.take(0)));
        empty_sums(order_group_scan_seq_view(order_groups@.take(0)));
        empty_sums(customer_name_scan_seq_view(customer_names@.take(0)));
        empty_sums(Seq::<OrderVariant>::empty());
        empty_sums(Seq::<NameVariant>::empty());
    }
    let size = if line_quantities.len() > order_groups.len() { line_quantities.len() } else { order_groups.len() };
    let mut orders = Editor::new(Table::<OrderData>::empty(size));
    let mut customers = Editor::new(Table::<CustomerData>::empty(customer_names.len()));
    let mut order_keys = Vec::new();
    let mut order_seen = HashMap::new();
    proof { empty_set::<i128>(); }
    let mut index = 0usize;
    while index < line_quantities.len()
        invariant index <= line_quantities.len(), coherent(orders, customers),
            valid_orders(orders), valid_customers(customers), remembered(order_seen@, order_keys@),
            forall|key: i128| #[trigger] orders.at(key) is Some <==> order_keys@.contains(key),
            forall|key: i128, customer: int, date: int, price: int| #[trigger] order_count(orders.at(key), customer, date, price) == 0,
            forall|key: i128| #[trigger] line_count(orders.at(key)) ==
                sum(line_quantity_scan_seq_view(line_quantities@.take(index as int)), nw(key as int)),
            forall|key: i128| #[trigger] line_sum(orders.at(key)) ==
                sum(line_quantity_scan_seq_view(line_quantities@.take(index as int)), qw(key as int)),
        decreases line_quantities.len() - index,
    {
        let ghost processed = index as int;
        let row = &line_quantities[index];
        remember(row.l_orderkey as i128, &mut order_seen, &mut order_keys);
        if !change_line(&mut orders, row.l_orderkey, row.l_quantity, 1) { return None; }
        proof {
            assert forall|key: i128| #[trigger] line_count(orders.at(key)) ==
                sum(line_quantity_scan_seq_view(line_quantities@.take(processed + 1)), nw(key as int)) by {
                projected_prefix(line_quantities@, processed, |input: LineQuantityScan| line_quantity_scan_view(input), nw(key as int));
            }
            assert forall|key: i128| #[trigger] line_sum(orders.at(key)) ==
                sum(line_quantity_scan_seq_view(line_quantities@.take(processed + 1)), qw(key as int)) by {
                projected_prefix(line_quantities@, processed, |input: LineQuantityScan| line_quantity_scan_view(input), qw(key as int));
            }
        }
        index += 1;
    }
    proof { assert(line_quantities@.take(index as int) =~= line_quantities@); }
    index = 0;
    while index < order_groups.len()
        invariant index <= order_groups.len(), coherent(orders, customers),
            valid_orders(orders), valid_customers(customers), remembered(order_seen@, order_keys@),
            forall|key: i128| #[trigger] orders.at(key) is Some <==> order_keys@.contains(key),
            forall|key: i128| #[trigger] line_count(orders.at(key)) == sum(line_quantity_scan_seq_view(line_quantities@), nw(key as int)),
            forall|key: i128| #[trigger] line_sum(orders.at(key)) == sum(line_quantity_scan_seq_view(line_quantities@), qw(key as int)),
            forall|key: i128, customer: int, date: int, price: int|
                #[trigger] order_count(orders.at(key), customer, date, price) ==
                sum(order_group_scan_seq_view(order_groups@.take(index as int)), ow(key as int, customer, date, price)),
        decreases order_groups.len() - index,
    {
        let ghost processed = index as int;
        let row = &order_groups[index];
        remember(row.o_orderkey as i128, &mut order_seen, &mut order_keys);
        if !change_order(&mut orders, row.o_orderkey, row.o_custkey, row.o_orderdate, row.o_totalprice, 1) { return None; }
        proof {
            assert forall|key: i128, customer: int, date: int, price: int| #[trigger]
                order_count(orders.at(key), customer, date, price) ==
                sum(order_group_scan_seq_view(order_groups@.take(processed + 1)), ow(key as int, customer, date, price)) by {
                projected_prefix(order_groups@, processed, |input: OrderGroupScan| order_group_scan_view(input), ow(key as int, customer, date, price));
            }
        }
        index += 1;
    }
    proof { assert(order_groups@.take(index as int) =~= order_groups@); }
    index = 0;
    while index < customer_names.len()
        invariant index <= customer_names.len(), coherent(orders, customers),
            valid_orders(orders), valid_customers(customers), remembered(order_seen@, order_keys@),
            forall|key: i128| #[trigger] orders.at(key) is Some <==> order_keys@.contains(key),
            order_sums(orders, line_quantity_scan_seq_view(line_quantities@), order_group_scan_seq_view(order_groups@)),
            forall|key: i128, name: Seq<u8>| #[trigger] customer_count(customers.at(key), name) ==
                sum(customer_name_scan_seq_view(customer_names@.take(index as int)), cw(key as int, name)),
        decreases customer_names.len() - index,
    {
        let ghost processed = index as int;
        let row = &customer_names[index];
        if !change_customer(&mut customers, row.c_custkey, &row.c_name, 1) { return None; }
        proof {
            assert forall|key: i128, name: Seq<u8>| #[trigger] customer_count(customers.at(key), name) ==
                sum(customer_name_scan_seq_view(customer_names@.take(processed + 1)), cw(key as int, name)) by {
                projected_prefix(customer_names@, processed, |input: CustomerNameScan| customer_name_scan_view(input), cw(key as int, name));
            }
        }
        index += 1;
    }
    proof { assert(customer_names@.take(index as int) =~= customer_names@); }
    let mut grouped: HashMap<i128, Vec<i64>> = HashMap::new();
    let mut quantities = Vec::new();
    index = 0;
    while index < order_keys.len()
        invariant index <= order_keys.len(), coherent(orders, customers),
            valid_orders(orders), valid_customers(customers), remembered(order_seen@, order_keys@),
            forall|key: i128| #[trigger] orders.at(key) is Some <==> order_keys@.contains(key),
            order_sums(orders, line_quantity_scan_seq_view(line_quantities@), order_group_scan_seq_view(order_groups@)),
            customer_sums(customers, customer_name_scan_seq_view(customer_names@)),
            quantities@.no_duplicates(), grouped@.dom() == quantities@.to_set(),
            forall|quantity: i128| #[trigger] grouped@.contains_key(quantity) ==> grouped@[quantity].len() > 0 && grouped@[quantity]@.no_duplicates(),
            forall|quantity: i128, key: i64| #[trigger] grouped_has(grouped@, quantity, key) <==>
                order_keys@.take(index as int).contains(key as i128) && eligible_order(orders.at(key as i128), quantity),
        decreases order_keys.len() - index,
    {
        let key = order_keys[index];
        let ghost prior_groups = grouped@;
        let ghost prior_orders = orders;
        proof { assert(order_keys@.contains(key)); }
        if let Some(row) = orders.get(key) {
            proof { assert(row.wf()); assert(i64::MIN <= key <= i64::MAX); }
            if row.count > 0 && row.variants.len() > 0 {
                let mut value = row.copy();
                if !grouped.contains_key(&row.quantity) {
                    grouped.insert(row.quantity, Vec::new());
                    proof { push_set(quantities@, row.quantity); }
                    quantities.push(row.quantity);
                }
                if let Some(mut bucket) = grouped.remove(&row.quantity) {
                    proof {
                        assert(!order_keys@.take(index as int).contains(key));
                        assert(!grouped_has(prior_groups, row.quantity, key as i64));
                        assert(bucket@ == if prior_groups.contains_key(row.quantity) { prior_groups[row.quantity]@ } else { Seq::<i64>::empty() });
                        assert(!bucket@.contains(key as i64));
                        push_set(bucket@, key as i64);
                    }
                    value.slot = Some(bucket.len());
                    bucket.push(key as i64);
                    grouped.insert(row.quantity, bucket);
                }
                put_order_metadata(&mut orders, key, value);
            }
        }
        proof {
            assert(order_keys@.take(index as int + 1) =~= order_keys@.take(index as int).push(key));
            assert forall|quantity: i128, candidate: i64| #[trigger] grouped_has(grouped@, quantity, candidate) <==>
                grouped_has(prior_groups, quantity, candidate) ||
                    (candidate as i128 == key && eligible_order(prior_orders.at(key), quantity)) by {}
            assert forall|quantity: i128, candidate: i64| #[trigger] grouped_has(grouped@, quantity, candidate) <==>
                order_keys@.take(index as int + 1).contains(candidate as i128) && eligible_order(orders.at(candidate as i128), quantity) by {}
        }
        index += 1;
    }
    proof {
        assert(order_keys@.take(index as int) =~= order_keys@);
        assert forall|quantity: i128, key: i64| #[trigger] grouped_has(grouped@, quantity, key) <==> eligible_order(orders.at(key as i128), quantity) by {
            if eligible_order(orders.at(key as i128), quantity) { assert(order_keys@.contains(key as i128)); }
        }
    }
    let ghost all_grouped = grouped@;
    let mut buckets = Editor::new(Table::<KeyBucket>::empty(quantities.len()));
    index = 0;
    while index < quantities.len()
        invariant index <= quantities.len(), coherent(orders, customers), sound_buckets(buckets),
            valid_orders(orders), valid_customers(customers),
            order_sums(orders, line_quantity_scan_seq_view(line_quantities@), order_group_scan_seq_view(order_groups@)),
            customer_sums(customers, customer_name_scan_seq_view(customer_names@)),
            quantities@.no_duplicates(), all_grouped.dom() == quantities@.to_set(),
            forall|quantity: i128| #[trigger] grouped@.contains_key(quantity) <==> all_grouped.contains_key(quantity) && !quantities@.take(index as int).contains(quantity),
            forall|quantity: i128| #[trigger] grouped@.contains_key(quantity) ==> grouped@[quantity] == all_grouped[quantity],
            forall|quantity: i128| #[trigger] all_grouped.contains_key(quantity) ==> all_grouped[quantity].len() > 0 && all_grouped[quantity]@.no_duplicates(),
            forall|quantity: i128, key: i64| #[trigger] grouped_has(all_grouped, quantity, key) <==> eligible_order(orders.at(key as i128), quantity),
            forall|quantity: i128| #[trigger] buckets.at(quantity) is Some <==> quantities@.take(index as int).contains(quantity),
            forall|quantity: i128| #[trigger] buckets.at(quantity) matches Some(bucket) ==> bucket.len > 0 && bucket.view().no_duplicates() && bucket.view() == all_grouped[quantity]@,
        decreases quantities.len() - index,
    {
        let quantity = quantities[index];
        proof { assert(grouped@.contains_key(quantity)); }
        if let Some(keys) = grouped.remove(&quantity) {
            put_valid(&mut buckets, quantity, Arc::new(KeyBucket::from_keys(&keys)), Ghost(|row: Option<KeyBucket>| bucket_shape(row)));
        }
        index += 1;
    }
    proof { assert(quantities@.take(index as int) =~= quantities@); }
    let directory = Arc::new(sort_quantities(&quantities, 0, quantities.len()));
    let orders = orders.finish();
    let customers = customers.finish();
    let buckets = buckets.finish();
    proof {
        assert forall|quantity: i128, key: i64| #[trigger] eligible_order(orders.at(key as i128), quantity) ==
            (buckets.at(quantity) matches Some(bucket) && bucket.view().contains(key)) by {
            assert(grouped_has(all_grouped, quantity, key) == eligible_order(orders.at(key as i128), quantity));
            if let Some(bucket) = buckets.at(quantity) {
                assert(bucket.view() == all_grouped[quantity]@);
            } else {
                assert(!all_grouped.contains_key(quantity));
            }
        }
        assert forall|quantity: i128| #[trigger] buckets.at(quantity) is Some <==> directory@.contains(quantity) by {}
        assert forall|quantity: i128| #[trigger] buckets.at(quantity) matches Some(bucket) ==> bucket.wf() && bucket.len > 0 && bucket.view().no_duplicates() by {
            assert(bucket_shape(buckets.at(quantity)));
        }
        assert forall|key: i128| #[trigger] orders.at(key) matches Some(row) ==> i64::MIN <= key <= i64::MAX && row.wf() by {}
        assert forall|key: i128| #[trigger] customers.at(key) matches Some(row) ==> i64::MIN <= key <= i64::MAX && row.wf() by {}
        assert forall|key: int| line_count(if i64::MIN <= key <= i64::MAX { orders.at(key as i128) } else { None }) == #[trigger] sum(db.lineitem, nw(key)) by {
            if key < i64::MIN || key > i64::MAX { line_range(line_quantities@, key); }
        }
        assert forall|key: int| line_sum(if i64::MIN <= key <= i64::MAX { orders.at(key as i128) } else { None }) == #[trigger] sum(db.lineitem, qw(key)) by {
            if key < i64::MIN || key > i64::MAX { line_range(line_quantities@, key); }
        }
        assert forall|key: int, customer: int, date: int, price: int| order_count(if i64::MIN <= key <= i64::MAX { orders.at(key as i128) } else { None }, customer, date, price) == #[trigger] sum(db.orders, ow(key, customer, date, price)) by {
            if key < i64::MIN || key > i64::MAX { order_range(order_groups@, key, customer, date, price); }
        }
        assert forall|customer: int, name: Seq<u8>| customer_count(if i64::MIN <= customer <= i64::MAX { customers.at(customer as i128) } else { None }, name) == #[trigger] sum(db.customer, cw(customer, name)) by {
            if customer < i64::MIN || customer > i64::MAX { customer_range(customer_names@, customer, name); }
        }
        assert forall|left: int, right: int| 0 <= left < right < directory.len() implies #[trigger] directory@[left] < #[trigger] directory@[right] by {}
        let candidate = State { db: Ghost(db), orders, customers, buckets, directory };
        assert forall|key: int| #[trigger] candidate.n(key) == sum(db.lineitem, nw(key)) by {}
        assert forall|key: int| #[trigger] candidate.quantity(key) == sum(db.lineitem, qw(key)) by {}
        assert forall|key: int, customer: int, date: int, price: int| #[trigger] candidate.om(key, customer, date, price) == sum(db.orders, ow(key, customer, date, price)) by {}
        assert forall|customer: int, name: Seq<u8>| #[trigger] candidate.cm(customer, name) == sum(db.customer, cw(customer, name)) by {}
        assert forall|quantity: i128, key: i64| #[trigger] candidate.bucket_has(quantity, key) == eligible_order(orders.at(key as i128), quantity) by {}
        assert(candidate.wf());
    }
    Some(State { db: Ghost(db), orders, customers, buckets, directory })
}

#[verifier::rlimit(30)]
pub fn execute(s: &State, p1: i64) -> (r: Option<Vec<Row>>)
    ensures r matches Some(out) ==> query_result(s.db(), params(p1 as int), out_view(out@)),
{
    hide(sum);
    proof { use_type_invariant(s); }
    let mut out = Vec::new();
    let mut quantity_index = upper_bound(&s.directory, p1 as i128);
    let ghost parameters = params(p1 as int);
    proof { result_proof::empty_cursor(*s, parameters, quantity_index as int); }
    while quantity_index < s.directory.len()
        invariant quantity_index <= s.directory.len(),
            result_proof::model(*s, parameters, out_view(out@), (quantity_index as int, 0, 0, 0)),
            forall|position: int| quantity_index <= position < s.directory.len() ==> #[trigger] s.directory@[position] > p1,
        decreases s.directory.len() - quantity_index,
    {
        let quantity = s.directory[quantity_index];
        let bucket = match s.buckets.get(quantity) { Some(value) => value, None => return None };
        let mut key_index = 0usize;
        while key_index < bucket.len
            invariant key_index <= bucket.len,
                result_proof::model(*s, parameters, out_view(out@), (quantity_index as int, key_index as int, 0, 0)),
            decreases bucket.len - key_index,
        {
            let key = bucket.get(key_index);
            let row = match s.orders.get(key as i128) { Some(value) => value, None => return None };
            let mut order_index = 0usize;
            while order_index < row.variants.len()
                invariant order_index <= row.variants.len(),
                    result_proof::model(*s, parameters, out_view(out@), (quantity_index as int, key_index as int, order_index as int, 0)),
                decreases row.variants.len() - order_index,
            {
                let order = &row.variants[order_index];
                if let Some(customer) = s.customers.get(order.customer as i128) {
                    let mut name_index = 0usize;
                    while name_index < customer.names.len()
                        invariant name_index <= customer.names.len(),
                            result_proof::model(*s, parameters, out_view(out@), (quantity_index as int, key_index as int, order_index as int, name_index as int)),
                        decreases customer.names.len() - name_index,
                    {
                        let name = &customer.names[name_index];
                        let subtotal = match quantity.checked_mul(order.count) { Some(value) => value, None => return None };
                        let total = match subtotal.checked_mul(name.count) { Some(value) => value, None => return None };
                        let ghost previous = out_view(out@);
                        let ghost location = (quantity_index as int, key_index as int, order_index as int, name_index as int);
                        out.push((copy_bytes(&name.name), order.customer, key, order.date, order.price as i128, total));
                        proof {
                            assert(result_proof::slot(*s, parameters, location) == Some(row_view(out@.last())));
                            assert(out_view(out@) =~= previous.push(row_view(out@.last())));
                            result_proof::append(*s, parameters, previous, location);
                        }
                        name_index += 1;
                    }
                    proof {
                        let from = (quantity_index as int, key_index as int, order_index as int, name_index as int);
                        let to = (quantity_index as int, key_index as int, order_index as int + 1, 0int);
                        assert forall|location: result_proof::Cursor| #[trigger] result_proof::slot(*s, parameters, location) is Some implies result_proof::before(location, from) == result_proof::before(location, to) by {}
                        result_proof::shift(*s, parameters, out_view(out@), from, to);
                    }
                } else {
                    proof {
                        let from = (quantity_index as int, key_index as int, order_index as int, 0int);
                        let to = (quantity_index as int, key_index as int, order_index as int + 1, 0int);
                        assert forall|location: result_proof::Cursor| #[trigger] result_proof::slot(*s, parameters, location) is Some implies result_proof::before(location, from) == result_proof::before(location, to) by {}
                        result_proof::shift(*s, parameters, out_view(out@), from, to);
                    }
                }
                order_index += 1;
            }
            proof {
                let from = (quantity_index as int, key_index as int, order_index as int, 0int);
                let to = (quantity_index as int, key_index as int + 1, 0int, 0int);
                assert forall|location: result_proof::Cursor| #[trigger] result_proof::slot(*s, parameters, location) is Some implies result_proof::before(location, from) == result_proof::before(location, to) by {}
                result_proof::shift(*s, parameters, out_view(out@), from, to);
            }
            key_index += 1;
        }
        proof {
            let from = (quantity_index as int, key_index as int, 0int, 0int);
            let to = (quantity_index as int + 1, 0int, 0int, 0int);
            assert forall|location: result_proof::Cursor| #[trigger] result_proof::slot(*s, parameters, location) is Some implies result_proof::before(location, from) == result_proof::before(location, to) by {}
            result_proof::shift(*s, parameters, out_view(out@), from, to);
        }
        quantity_index += 1;
    }
    let ghost source = out_view(out@);
    proof { assert(source.no_duplicates()); }
    let out = sorted(out);
    proof {
        assert forall|left: int, right: int| 0 <= left < right < out.len() implies !row_lt(#[trigger] out_view(out@)[right], #[trigger] out_view(out@)[left]) by {}
        result_proof::finish(*s, parameters, source, out_view(out@));
    }
    Some(out)
}

#[verifier::rlimit(30)]
pub fn apply(
    s: &State,
    customer_delta: &Vec<CustomerNameDelta>,
    lineitem_delta: &[LineQuantityDelta],
    orders_delta: &[OrderGroupDelta],
    Ghost(db2): Ghost<Db>,
) -> (r: Result<State, u32>)
    requires capture_ok(s.db(), db2, customer_name_delta_seq_view(customer_delta@),
        line_quantity_delta_seq_view(lineitem_delta@), order_group_delta_seq_view(orders_delta@)),
    ensures r matches Ok(s2) ==> s2.db() == db2,
{
    hide(sum);
    hide(Editor::at);
    hide(Editor::wf);
    hide(Table::at);
    hide(Table::wf);
    proof {
        use_type_invariant(s);
        empty_sums(line_quantity_delta_seq_view(lineitem_delta@.take(0)));
        empty_sums(order_group_delta_seq_view(orders_delta@.take(0)));
        empty_sums(customer_name_delta_seq_view(customer_delta@.take(0)));
    }
    let mut orders = Editor::new(s.orders.share());
    proof {
        apply_canonical(s.db(), db2, customer_name_delta_seq_view(customer_delta@),
            line_quantity_delta_seq_view(lineitem_delta@), order_group_delta_seq_view(orders_delta@));
    }
    let mut customers = Editor::new(s.customers.share());
    let mut buckets = Editor::new(s.buckets.share());
    proof {
        assert forall|key: i128| #[trigger] order_shape(orders.at(key)) by {
            assert(orders.at(key) == s.orders.at(key));
            if let Some(row) = s.orders.at(key) { assert(row.wf()); }
        }
        assert forall|key: i128| #[trigger] customer_shape(customers.at(key)) by {
            assert(customers.at(key) == s.customers.at(key));
            if let Some(row) = s.customers.at(key) { assert(row.wf()); }
        }
        assert forall|quantity: i128| #[trigger] bucket_shape(buckets.at(quantity)) by {
            assert(buckets.at(quantity) == s.buckets.at(quantity));
            if let Some(bucket) = s.buckets.at(quantity) { assert(bucket.wf()); }
        }
    }
    let mut order_keys = Vec::new();
    let mut order_seen = HashMap::new();
    let mut customer_keys = Vec::new();
    let mut customer_seen = HashMap::new();
    let mut quantities = Vec::new();
    let mut quantity_seen = HashMap::new();
    proof { empty_set::<i128>(); }
    if !apply_lines(&mut orders, lineitem_delta, &mut order_seen, &mut order_keys) { return Err(1); }
    if !apply_orders(&mut orders, orders_delta, &mut order_seen, &mut order_keys) { return Err(1); }
    if !apply_customers(&mut customers, customer_delta, &mut customer_seen, &mut customer_keys) { return Err(1); }
    proof {
        assert forall|key: i128| #[trigger] line_count(orders.at(key)) == sum(db2.lineitem, nw(key as int)) by {
            if key < i64::MIN || key > i64::MAX { assert(s.orders.at(key) is None); }
            assert(s.n(key as int) == line_count(s.orders.at(key)));
        }
        assert forall|key: i128| #[trigger] line_sum(orders.at(key)) == sum(db2.lineitem, qw(key as int)) by {
            if key < i64::MIN || key > i64::MAX { assert(s.orders.at(key) is None); }
            assert(s.quantity(key as int) == line_sum(s.orders.at(key)));
        }
        assert forall|key: i128, customer: int, date: int, price: int| #[trigger] order_count(orders.at(key), customer, date, price) == sum(db2.orders, ow(key as int, customer, date, price)) by {
            if key < i64::MIN || key > i64::MAX { assert(s.orders.at(key) is None); }
            assert(s.om(key as int, customer, date, price) == order_count(s.orders.at(key), customer, date, price));
        }
        assert forall|key: i128, name: Seq<u8>| #[trigger] customer_count(customers.at(key), name) == sum(db2.customer, cw(key as int, name)) by {
            if key < i64::MIN || key > i64::MAX { assert(s.customers.at(key) is None); }
            assert(s.cm(key as int, name) == customer_count(s.customers.at(key), name));
        }
        assert forall|key: i128| !order_keys@.contains(key) implies #[trigger] order_valid(orders.at(key), key) by {
            assert(orders.at(key) == s.orders.at(key));
            if let Some(row) = s.orders.at(key) { assert(row.wf()); }
        }
        assert(orders_valid_prefix(orders, order_keys@, 0));
        assert(customers_valid_prefix(customers, customer_keys@, 0));
        assert(consistent_buckets(buckets));
        assert forall|quantity: i128, key: i64| #[trigger] bucket_member(buckets, quantity, key) == s.bucket_has(quantity, key) by {}
        assert forall|quantity: i128, key: i64| !order_keys@.contains(key as i128) ==> #[trigger] eligible_order(orders.at(key as i128), quantity) == s.bucket_has(quantity, key) by {}
    }
    let mut index = 0usize;
    while index < order_keys.len()
        invariant index <= order_keys.len(), coherent(orders, customers), consistent_buckets(buckets),
            remembered(order_seen@, order_keys@), remembered(customer_seen@, customer_keys@), remembered(quantity_seen@, quantities@),
            bounded_keys(order_keys@), bounded_keys(customer_keys@), totals(orders, customers, db2),
            orders_valid_prefix(orders, order_keys@, 0), customers_valid_prefix(customers, customer_keys@, 0),
            forall|quantity: i128, key: i64| !order_keys@.contains(key as i128) ==> #[trigger] eligible_order(orders.at(key as i128), quantity) == s.bucket_has(quantity, key),
            forall|quantity: i128, key: i64| #[trigger] bucket_member(buckets, quantity, key) <==>
                s.bucket_has(quantity, key) && !order_keys@.take(index as int).contains(key as i128),
            forall|quantity: i128| (#[trigger] buckets.at(quantity) is Some) != (s.buckets.at(quantity) is Some) ==> quantities@.contains(quantity),
        decreases order_keys.len() - index,
    {
        let key = order_keys[index];
        let ghost before_buckets = buckets;
        if let Some(old) = s.orders.get(key) {
            if old.count > 0 && old.variants.len() > 0 {
                let current = match orders.get(key) { Some(value) => value, None => return Err(2) };
                let slot = match current.slot { Some(value) => value, None => return Err(2) };
                let original = match buckets.get(old.quantity) { Some(value) => value, None => return Err(2) };
                proof { assert(bucket_valid(buckets.at(old.quantity))); }
                if slot >= original.len || original.get(slot) as i128 != key { return Err(2); }
                let mut bucket = original.share();
                let ghost original_view = bucket.view();
                proof {
                    assert(bucket_valid(buckets.at(old.quantity)));
                    assert(original.view()[slot as int] == key as i64);
                    swap_remove_unique(original_view, slot as int);
                }
                let moved = bucket.remove(slot);
                let ghost removed_view = bucket.view();
                proof {
                    assert(original_view == original.view());
                    assert(before_buckets.at(old.quantity) == Some(*original));
                    assert forall|candidate: i64| #[trigger] bucket.view().contains(candidate) <==>
                        original_view.contains(candidate) && candidate as i128 != key by {}
                }
                if moved as i128 != key {
                    let value = match orders.get(moved as i128) { Some(value) => value, None => return Err(2) };
                    let mut updated = value.copy();
                    updated.slot = Some(slot);
                    put_order_metadata(&mut orders, moved as i128, updated);
                }
                let mut updated = current.copy();
                updated.slot = None;
                put_order_metadata(&mut orders, key, updated);
                if bucket.len == 0 { erase_valid(&mut buckets, old.quantity, Ghost(|row: Option<KeyBucket>| bucket_valid(row))); }
                else { put_valid(&mut buckets, old.quantity, Arc::new(bucket), Ghost(|row: Option<KeyBucket>| bucket_valid(row))); }
                remember(old.quantity, &mut quantity_seen, &mut quantities);
                proof {
                    assert forall|candidate: i64| #[trigger] bucket_member(buckets, old.quantity, candidate) == removed_view.contains(candidate) by {
                        if removed_view.len() == 0 { assert(!removed_view.contains(candidate)); }
                    }
                    assert forall|quantity: i128, candidate: i64| #[trigger] bucket_member(buckets, quantity, candidate) <==>
                        bucket_member(before_buckets, quantity, candidate) && candidate as i128 != key by {
                        if quantity == old.quantity {
                            assert(bucket_member(buckets, quantity, candidate) == removed_view.contains(candidate));
                            assert(bucket_member(before_buckets, quantity, candidate) == original_view.contains(candidate));
                            assert(removed_view.contains(candidate) <==> original_view.contains(candidate) && candidate as i128 != key);
                        } else {
                            assert(buckets.at(quantity) == before_buckets.at(quantity));
                            assert(bucket_member(buckets, quantity, candidate) == bucket_member(before_buckets, quantity, candidate));
                        }
                        if quantity != old.quantity && candidate as i128 == key {
                            assert(!s.bucket_has(quantity, candidate));
                            assert(!bucket_member(before_buckets, quantity, candidate));
                            assert(!bucket_member(buckets, quantity, candidate));
                        }
                        assert(bucket_member(buckets, quantity, candidate) <==>
                            bucket_member(before_buckets, quantity, candidate) && candidate as i128 != key);
                    }
                }
            }
        }
        proof {
            assert(order_keys@.take(index as int + 1) =~= order_keys@.take(index as int).push(key));
            assert forall|quantity: i128, candidate: i64| #[trigger] bucket_member(buckets, quantity, candidate) <==>
                bucket_member(before_buckets, quantity, candidate) && candidate as i128 != key by {
                if candidate as i128 == key { assert(!bucket_member(buckets, quantity, candidate)); }
            }
            assert forall|quantity: i128, candidate: i64| #[trigger] bucket_member(buckets, quantity, candidate) <==>
                s.bucket_has(quantity, candidate) && !order_keys@.take(index as int + 1).contains(candidate as i128) by {
                broadcast use vstd::seq_lib::group_seq_properties;
                assert(bucket_member(before_buckets, quantity, candidate) <==>
                    s.bucket_has(quantity, candidate) && !order_keys@.take(index as int).contains(candidate as i128));
            }
        }
        index += 1;
    }
    proof { assert(order_keys@.take(index as int) =~= order_keys@); }
    index = 0;
    while index < order_keys.len()
        invariant index <= order_keys.len(), coherent(orders, customers), consistent_buckets(buckets),
            remembered(order_seen@, order_keys@), remembered(customer_seen@, customer_keys@), remembered(quantity_seen@, quantities@),
            bounded_keys(order_keys@), bounded_keys(customer_keys@), totals(orders, customers, db2),
            orders_valid_prefix(orders, order_keys@, index as int), customers_valid_prefix(customers, customer_keys@, 0),
            forall|quantity: i128, key: i64| !order_keys@.contains(key as i128) ==> #[trigger] eligible_order(orders.at(key as i128), quantity) == s.bucket_has(quantity, key),
            forall|quantity: i128, key: i64| #[trigger] bucket_member(buckets, quantity, key) <==>
                (if order_keys@.take(index as int).contains(key as i128) { eligible_order(orders.at(key as i128), quantity) }
                else { !order_keys@.contains(key as i128) && s.bucket_has(quantity, key) }),
            forall|quantity: i128| (#[trigger] buckets.at(quantity) is Some) != (s.buckets.at(quantity) is Some) ==> quantities@.contains(quantity),
        decreases order_keys.len() - index,
    {
        let key = order_keys[index];
        let ghost before_buckets = buckets;
        let current = match orders.get(key) { Some(value) => value, None => return Err(2) };
        proof { assert(order_shape(orders.at(key))); }
        let mut value = current.copy();
        if !value.clean() { return Err(2); }
        let ghost cleaned = value;
        if value.count > 0 && value.variants.len() > 0 {
            proof { assert(bucket_valid(buckets.at(value.quantity))); assert(bucket_shape(buckets.at(value.quantity))); }
            let mut bucket = match buckets.get(value.quantity) { Some(value) => value.share(), None => KeyBucket::empty() };
            let ghost original_view = bucket.view();
            proof {
                assert forall|candidate: i64| #[trigger] original_view.contains(candidate) == bucket_member(before_buckets, value.quantity, candidate) by {}
                assert(!order_keys@.take(index as int).contains(key));
                assert(!bucket_member(buckets, value.quantity, key as i64));
                assert(!bucket.view().contains(key as i64));
                push_set(bucket.view(), key as i64);
            }
            value.slot = Some(bucket.len);
            if !bucket.push(key as i64) { return Err(1); }
            let ghost pushed_view = bucket.view();
            put_valid(&mut buckets, value.quantity, Arc::new(bucket), Ghost(|row: Option<KeyBucket>| bucket_valid(row)));
            remember(value.quantity, &mut quantity_seen, &mut quantities);
            proof {
                assert forall|quantity: i128, candidate: i64| #[trigger] bucket_member(buckets, quantity, candidate) <==>
                    bucket_member(before_buckets, quantity, candidate) || (candidate as i128 == key && quantity == value.quantity) by {
                    if quantity == value.quantity {
                        broadcast use vstd::seq_lib::group_seq_properties;
                        assert(bucket_member(buckets, quantity, candidate) == pushed_view.contains(candidate));
                        assert(pushed_view == original_view.push(key as i64));
                        assert(original_view.contains(candidate) == bucket_member(before_buckets, quantity, candidate));
                    } else {
                        assert(buckets.at(quantity) == before_buckets.at(quantity));
                        assert(bucket_member(buckets, quantity, candidate) == bucket_member(before_buckets, quantity, candidate));
                    }
                }
            }
        } else { value.slot = None; }
        proof {
            assert forall|person: int, day: int, total: int| #[trigger] value.multiplicity(person, day, total) == order_count(orders.at(key), person, day, total) by {
                assert(value.multiplicity(person, day, total) == cleaned.multiplicity(person, day, total));
                assert(cleaned.multiplicity(person, day, total) == current.multiplicity(person, day, total));
            }
        }
        install_clean_order(&mut orders, key, value);
        proof {
            assert(order_keys@.take(index as int + 1) =~= order_keys@.take(index as int).push(key));
            assert forall|quantity: i128, candidate: i64| #[trigger] bucket_member(buckets, quantity, candidate) <==>
                bucket_member(before_buckets, quantity, candidate) || (candidate as i128 == key && eligible_order(orders.at(key), quantity)) by {
                assert(eligible_order(orders.at(key), quantity) == (cleaned.count > 0 && cleaned.variants.len() > 0 && cleaned.quantity == quantity));
            }
        }
        index += 1;
    }
    proof {
        assert(order_keys@.take(index as int) =~= order_keys@);
        assert forall|key: i128| #[trigger] orders.at(key) matches Some(row) ==> i64::MIN <= key <= i64::MAX && row.wf() by {
            assert(order_valid(orders.at(key), key));
        }
    }
    index = 0;
    while index < customer_keys.len()
        invariant index <= customer_keys.len(), coherent(orders, customers), consistent_buckets(buckets),
            bounded_keys(customer_keys@), valid_orders(orders), totals(orders, customers, db2),
            customers_valid_prefix(customers, customer_keys@, index as int),
            forall|quantity: i128, key: i64| #[trigger] bucket_member(buckets, quantity, key) <==> eligible_order(orders.at(key as i128), quantity),
            forall|quantity: i128| (#[trigger] buckets.at(quantity) is Some) != (s.buckets.at(quantity) is Some) ==> quantities@.contains(quantity),
        decreases customer_keys.len() - index,
    {
        let key = customer_keys[index];
        let current = match customers.get(key) { Some(value) => value, None => return Err(2) };
        proof { assert(customer_shape(customers.at(key))); }
        let mut value = current.copy();
        if !value.clean() { return Err(2); }
        install_clean_customer(&mut customers, key, value);
        proof { assert(customer_keys@.take(index as int + 1) =~= customer_keys@.take(index as int).push(key)); }
        index += 1;
    }
    proof {
        assert(customer_keys@.take(index as int) =~= customer_keys@);
        assert forall|key: i128| #[trigger] customers.at(key) matches Some(row) ==> i64::MIN <= key <= i64::MAX && row.wf() by {
            assert(customer_valid(customers.at(key), key));
        }
    }
    let mut changed_directory: Option<Vec<i128>> = None;
    index = 0;
    while index < quantities.len()
        invariant index <= quantities.len(), coherent(orders, customers), consistent_buckets(buckets),
            valid_orders(orders), valid_customers(customers), totals(orders, customers, db2),
            remembered(quantity_seen@, quantities@),
            forall|quantity: i128, key: i64| #[trigger] bucket_member(buckets, quantity, key) <==> eligible_order(orders.at(key as i128), quantity),
            forall|quantity: i128| (#[trigger] buckets.at(quantity) is Some) != (s.buckets.at(quantity) is Some) ==> quantities@.contains(quantity),
            changed_directory matches Some(values) ==> increasing(values@),
            directory_value(changed_directory, s.directory).no_duplicates(),
            forall|quantity: i128| #[trigger] directory_value(changed_directory, s.directory).contains(quantity) <==>
                (if quantities@.take(index as int).contains(quantity) { buckets.at(quantity) is Some } else { s.buckets.at(quantity) is Some }),
        decreases quantities.len() - index,
    {
        let quantity = quantities[index];
        let ghost previous = directory_value(changed_directory, s.directory);
        proof { assert(!quantities@.take(index as int).contains(quantity)); }
        let before = s.buckets.get(quantity).is_some();
        let after = buckets.get(quantity).is_some();
        if before != after {
            let mut values = match changed_directory { Some(values) => values, None => copy_keys(&s.directory) };
            let position = lower_bound(&values, quantity);
            if after {
                proof {
                    assert(!values@.contains(quantity));
                    insertion_ordered(values@, position as int, quantity);
                    insert_unique(values@, position as int, quantity);
                }
                values.insert(position, quantity);
            }
            else {
                if position >= values.len() || values[position] != quantity { return Err(2); }
                proof { removal_ordered(values@, position as int); remove_unique(values@, position as int); }
                values.remove(position);
            }
            changed_directory = Some(values);
        }
        proof {
            assert(quantities@.take(index as int + 1) =~= quantities@.take(index as int).push(quantity));
            assert forall|candidate: i128| #[trigger] directory_value(changed_directory, s.directory).contains(candidate) <==>
                (if candidate == quantity { after } else { previous.contains(candidate) }) by {}
            assert forall|candidate: i128| #[trigger] directory_value(changed_directory, s.directory).contains(candidate) <==>
                (if quantities@.take(index as int + 1).contains(candidate) { buckets.at(candidate) is Some } else { s.buckets.at(candidate) is Some }) by {
                broadcast use vstd::seq_lib::group_seq_properties;
                if candidate != quantity {
                    assert(previous.contains(candidate) <==>
                        (if quantities@.take(index as int).contains(candidate) { buckets.at(candidate) is Some } else { s.buckets.at(candidate) is Some }));
                }
            }
        }
        index += 1;
    }
    let directory = match changed_directory { Some(values) => Arc::new(values), None => Arc::clone(&s.directory) };
    proof {
        assert(quantities@.take(index as int) =~= quantities@);
        assert forall|quantity: i128| #[trigger] directory@.contains(quantity) <==> buckets.at(quantity) is Some by {
            if (buckets.at(quantity) is Some) != (s.buckets.at(quantity) is Some) { assert(quantities@.contains(quantity)); }
        }
    }
    let ghost final_orders = orders;
    let ghost final_buckets = buckets;
    let orders = orders.finish();
    let customers = customers.finish();
    let buckets = buckets.finish();
    proof {
        let candidate = State { db: Ghost(db2), orders, customers, buckets, directory };
        assert forall|key: int| #[trigger] candidate.n(key) == sum(db2.lineitem, nw(key)) by {
            if key < i64::MIN || key > i64::MAX { line_delta_range(lineitem_delta@, key); assert(s.n(key) == 0); }
        }
        assert forall|key: int| #[trigger] candidate.quantity(key) == sum(db2.lineitem, qw(key)) by {
            if key < i64::MIN || key > i64::MAX { line_delta_range(lineitem_delta@, key); assert(s.quantity(key) == 0); }
        }
        assert forall|key: int, customer: int, date: int, price: int| #[trigger] candidate.om(key, customer, date, price) == sum(db2.orders, ow(key, customer, date, price)) by {
            if key < i64::MIN || key > i64::MAX { order_delta_range(orders_delta@, key, customer, date, price); assert(s.om(key, customer, date, price) == 0); }
        }
        assert forall|customer: int, name: Seq<u8>| #[trigger] candidate.cm(customer, name) == sum(db2.customer, cw(customer, name)) by {
            if customer < i64::MIN || customer > i64::MAX { customer_delta_range(customer_delta@, customer, name); assert(s.cm(customer, name) == 0); }
        }
        assert forall|quantity: i128, key: i64| #[trigger] candidate.bucket_has(quantity, key) == eligible_order(orders.at(key as i128), quantity) by {
            assert(buckets.at(quantity) == final_buckets.at(quantity));
            assert(orders.at(key as i128) == final_orders.at(key as i128));
            assert(bucket_member(final_buckets, quantity, key) == eligible_order(final_orders.at(key as i128), quantity));
        }
        assert forall|quantity: i128| #[trigger] buckets.at(quantity) matches Some(bucket) ==> bucket.wf() && bucket.len > 0 && bucket.view().no_duplicates() by { assert(bucket_valid(buckets.at(quantity))); }
        assert forall|key: i128| #[trigger] orders.at(key) matches Some(row) ==> i64::MIN <= key <= i64::MAX && row.wf() by {}
        assert forall|key: i128| #[trigger] customers.at(key) matches Some(row) ==> i64::MIN <= key <= i64::MAX && row.wf() by {}
        assert forall|left: int, right: int| 0 <= left < right < directory.len() implies #[trigger] directory@[left] < #[trigger] directory@[right] by {}
        assert(candidate.wf());
    }
    Ok(State { db: Ghost(db2), orders, customers, buckets, directory })
}

}

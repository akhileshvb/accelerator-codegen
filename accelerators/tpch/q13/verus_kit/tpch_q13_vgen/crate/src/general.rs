#![verifier::loop_isolation(false)]
// The general path: signed logs of every order and customer row, replayed on a read
// whose pattern the tracked fast path does not cover.  Each apply appends one chunk.
use vstd::prelude::*;
use std::collections::HashMap;
use std::sync::Arc;
use verus_common::sums::*;
use verus_common::rel::*;
use verus_common::sqlfns::like_match;
use verus_prooflib::like_match_exec;
use crate::gen_spec::*;
use crate::gen_obligations::*;
use crate::aggregate::has_key;
use crate::facts::*;
use crate::dense::lemma_seen_hist;

verus! {

broadcast use vstd::std_specs::hash::group_hash_axioms, vstd::seq_lib::group_seq_properties;

pub fn copy_bytes(source: &Vec<u8>) -> (out: Vec<u8>)
    ensures out@ == source@,
{
    let mut out = Vec::with_capacity(source.len());
    let mut index = 0usize;
    while index < source.len()
        invariant index <= source.len(), out@ == source@.take(index as int),
        decreases source.len() - index,
    {
        out.push(source[index]);
        index += 1;
    }
    proof { assert(source@.take(source.len() as int) =~= source@); }
    out
}

pub struct OrderEntry { pub custkey: i64, pub comment: Arc<Vec<u8>>, pub sign: i64 }

pub struct CustEntry { pub custkey: i64, pub sign: i64 }

pub struct Log<T> { pub chunks: Arc<Vec<Arc<Vec<T>>>> }

pub fn share_chunks<T>(source: &Vec<Arc<Vec<T>>>) -> (out: Vec<Arc<Vec<T>>>)
    ensures out@ == source@,
{
    let mut out = Vec::new();
    let mut index = 0usize;
    while index < source.len()
        invariant index <= source.len(), out@ == source@.take(index as int),
        decreases source.len() - index,
    {
        out.push(Arc::clone(&source[index]));
        index += 1;
    }
    out
}

impl<T> Log<T> {
    pub open spec fn views(self) -> Seq<Seq<T>> {
        (*self.chunks)@.map_values(|chunk: Arc<Vec<T>>| (*chunk)@)
    }

    pub open spec fn entries(self) -> Seq<T> { flat(self.views()) }

    pub fn empty() -> (out: Self)
        ensures out.entries() == Seq::<T>::empty(),
    {
        let out = Log { chunks: Arc::new(Vec::new()) };
        proof { assert(out.views() =~= Seq::<Seq<T>>::empty()); }
        out
    }

    pub fn with_chunk(&self, chunk: Vec<T>) -> (out: Self)
        ensures out.views() == self.views().push(chunk@), out.entries() == self.entries() + chunk@,
    {
        let mut chunks = share_chunks(&self.chunks);
        let ghost before = self.views();
        chunks.push(Arc::new(chunk));
        let out = Log { chunks: Arc::new(chunks) };
        proof {
            assert(out.views() =~= before.push(chunk@));
            assert(before.push(chunk@).drop_last() =~= before);
            assert(flat(before.push(chunk@)) == flat(before) + chunk@);
        }
        out
    }
}

pub open spec fn unit_orders(entries: Seq<OrderEntry>) -> bool {
    forall|index: int| 0 <= index < entries.len() ==> (#[trigger] entries[index]).sign == 1 || entries[index].sign == -1
}

pub open spec fn unit_customers(entries: Seq<CustEntry>) -> bool {
    forall|index: int| 0 <= index < entries.len() ==> (#[trigger] entries[index]).sign == 1 || entries[index].sign == -1
}

/// The signed sum of f over the logged order rows.
pub open spec fn order_log_sum(entries: Seq<OrderEntry>, f: spec_fn((int, Seq<u8>)) -> int) -> int {
    sum(entries, |entry: OrderEntry| entry.sign as int * f((entry.custkey as int, (*entry.comment)@)))
}

pub open spec fn customer_log_sum(entries: Seq<CustEntry>, f: spec_fn(CustomerRow) -> int) -> int {
    sum(entries, |entry: CustEntry| entry.sign as int * f(CustomerRow { c_custkey: entry.custkey as int }))
}

pub open spec fn order_weight(p: Seq<u8>, key: int) -> spec_fn(OrderEntry) -> int {
    |entry: OrderEntry| entry.sign as int * ind(entry.custkey as int == key && !like_match((*entry.comment)@, p))
}

pub open spec fn customer_weight(key: int) -> spec_fn(CustEntry) -> int {
    |entry: CustEntry| entry.sign as int * ind(entry.custkey as int == key)
}

pub open spec fn at(map: Map<i64, i128>, key: i64) -> int {
    if map.contains_key(key) { map[key] as int } else { 0 }
}

pub open spec fn within(map: Map<i64, i128>, bound: int) -> bool {
    forall|key: i64| -bound <= #[trigger] at(map, key) <= bound
}

pub proof fn lemma_unit_sum(entries: Seq<OrderEntry>, p: Seq<u8>, key: int)
    requires unit_orders(entries),
    ensures -(entries.len() as int) <= sum(entries, order_weight(p, key)) <= entries.len() as int,
    decreases entries.len(),
{
    if entries.len() > 0 {
        let rest = entries.drop_last();
        assert forall|index: int| 0 <= index < rest.len() implies (#[trigger] rest[index]).sign == 1 || rest[index].sign == -1 by {
            assert(rest[index] == entries[index]);
        }
        lemma_unit_sum(rest, p, key);
        assert(entries =~= rest.push(entries.last()));
        lemma_sum_push(rest, entries.last(), order_weight(p, key));
    }
}

/// Adds the weights of one chunk of orders into the per-key non-matching counts.
#[verifier::rlimit(20)]
pub fn count_orders(counts: &mut HashMap<i64, i128>, chunk: &Vec<OrderEntry>, p: &[u8], Ghost(done): Ghost<int>)
    requires within(old(counts)@, done), unit_orders(chunk@), 0 <= done, done + chunk@.len() < 0x40000000000000000000000000000000int,
    ensures within(final(counts)@, done + chunk@.len()),
        forall|key: i64| #[trigger] at(final(counts)@, key) == at(old(counts)@, key) + sum(chunk@, order_weight(p@, key as int)),
{
    let mut index = 0usize;
    proof {
        assert forall|key: i64| #[trigger] at(counts@, key) == at(old(counts)@, key) + sum(chunk@.take(0), order_weight(p@, key as int)) by {
            assert(chunk@.take(0) =~= Seq::<OrderEntry>::empty());
            lemma_sum_empty(chunk@.take(0), order_weight(p@, key as int));
        }
    }
    while index < chunk.len()
        invariant index <= chunk.len(), unit_orders(chunk@), 0 <= done, done + chunk@.len() < 0x40000000000000000000000000000000int,
            within(counts@, done + index as int),
            forall|key: i64| #[trigger] at(counts@, key) == at(old(counts)@, key) + sum(chunk@.take(index as int), order_weight(p@, key as int)),
        decreases chunk.len() - index,
    {
        let entry = &chunk[index];
        let ghost before = counts@;
        let hit = like_match_exec(entry.comment.as_slice(), p);
        proof { assert(chunk@[index as int].sign == 1 || chunk@[index as int].sign == -1); }
        if !hit {
            let current = match counts.get(&entry.custkey) { Some(value) => *value, None => 0 };
            proof {
                assert(-(done + index as int) <= at(counts@, entry.custkey) <= done + index as int);
                assert(current as int == at(before, entry.custkey));
            }
            counts.insert(entry.custkey, current + entry.sign as i128);
        }
        proof {
            assert(hit == like_match((*chunk@[index as int].comment)@, p@));
            assert forall|key: i64| #[trigger] at(counts@, key) == at(old(counts)@, key) + sum(chunk@.take(index as int + 1), order_weight(p@, key as int)) by {
                lemma_sum_take(chunk@, index as int, order_weight(p@, key as int));
                let weight = order_weight(p@, key as int)(chunk@[index as int]);
                assert(at(before, key) == at(old(counts)@, key) + sum(chunk@.take(index as int), order_weight(p@, key as int)));
                if !hit && key == entry.custkey {
                    assert(counts@.contains_key(key));
                    assert(at(counts@, key) == at(before, key) + entry.sign as int);
                    assert(weight == entry.sign as int * 1);
                } else {
                    assert(at(counts@, key) == at(before, key));
                    assert(weight == 0);
                }
            }
            assert forall|key: i64| -(done + index as int + 1) <= #[trigger] at(counts@, key) <= done + index as int + 1 by {
                assert(-(done + index as int) <= at(before, key) <= done + index as int);
            }
        }
        index += 1;
    }
    proof { assert(chunk@.take(chunk@.len() as int) =~= chunk@); }
}


pub proof fn lemma_flat_prefix_len<A>(chunks: Seq<Seq<A>>, count: int)
    requires 0 <= count <= chunks.len(),
    ensures flat(chunks.take(count)).len() <= flat(chunks).len(),
    decreases chunks.len() - count,
{
    if count < chunks.len() {
        lemma_flat_prefix_len(chunks, count + 1);
        assert(chunks.take(count + 1).drop_last() =~= chunks.take(count));
        assert(flat(chunks.take(count + 1)) == flat(chunks.take(count)) + chunks[count]);
    }
}

pub proof fn lemma_flat_step<A>(chunks: Seq<Seq<A>>, count: int)
    requires 0 <= count < chunks.len(),
    ensures flat(chunks.take(count + 1)) == flat(chunks.take(count)) + chunks[count],
{
    assert(chunks.take(count + 1).drop_last() =~= chunks.take(count));
}

pub proof fn lemma_chunk_unit_orders(chunks: Seq<Seq<OrderEntry>>, index: int)
    requires 0 <= index < chunks.len(), unit_orders(flat(chunks)),
    ensures unit_orders(chunks[index]),
{
    assert forall|position: int| 0 <= position < chunks[index].len() implies
        (#[trigger] chunks[index][position]).sign == 1 || chunks[index][position].sign == -1 by {
        lemma_flat_contains(chunks, chunks[index][position]);
        assert(chunks[index].contains(chunks[index][position]));
        assert(flat(chunks).contains(chunks[index][position]));
        let found = choose|found: int| 0 <= found < flat(chunks).len() && flat(chunks)[found] == chunks[index][position];
    }
}

pub proof fn lemma_chunk_unit_customers(chunks: Seq<Seq<CustEntry>>, index: int)
    requires 0 <= index < chunks.len(), unit_customers(flat(chunks)),
    ensures unit_customers(chunks[index]),
{
    assert forall|position: int| 0 <= position < chunks[index].len() implies
        (#[trigger] chunks[index][position]).sign == 1 || chunks[index][position].sign == -1 by {
        lemma_flat_contains(chunks, chunks[index][position]);
        assert(chunks[index].contains(chunks[index][position]));
        assert(flat(chunks).contains(chunks[index][position]));
        let found = choose|found: int| 0 <= found < flat(chunks).len() && flat(chunks)[found] == chunks[index][position];
    }
}

/// Per-key signed non-matching order counts of the whole log.
#[verifier::rlimit(30)]
pub fn tally_orders(log: &Log<OrderEntry>, p: &[u8]) -> (counts: HashMap<i64, i128>)
    requires unit_orders(log.entries()), log.entries().len() < 0x1000000000000000000000000int,
    ensures forall|key: i64| #[trigger] at(counts@, key) == sum(log.entries(), order_weight(p@, key as int)),
        within(counts@, log.entries().len() as int),
{
    let mut counts: HashMap<i64, i128> = HashMap::new();
    let ghost views = log.views();
    let mut index = 0usize;
    proof {
        assert(views.take(0) =~= Seq::<Seq<OrderEntry>>::empty());
        assert forall|key: i64| #[trigger] at(counts@, key) == sum(flat(views.take(0)), order_weight(p@, key as int)) by {
            lemma_sum_empty(flat(views.take(0)), order_weight(p@, key as int));
        }
    }
    while index < log.chunks.len()
        invariant index <= log.chunks.len(), views == log.views(), views.len() == log.chunks.len(),
            unit_orders(log.entries()), log.entries().len() < 0x1000000000000000000000000int,
            within(counts@, flat(views.take(index as int)).len() as int),
            forall|key: i64| #[trigger] at(counts@, key) == sum(flat(views.take(index as int)), order_weight(p@, key as int)),
        decreases log.chunks.len() - index,
    {
        let chunk = &log.chunks[index];
        proof {
            lemma_chunk_unit_orders(views, index as int);
            lemma_flat_prefix_len(views, index as int + 1);
            lemma_flat_step(views, index as int);
            assert(views[index as int] == (**chunk)@);
        }
        count_orders(&mut counts, chunk, p, Ghost(flat(views.take(index as int)).len() as int));
        proof {
            let after = flat(views.take(index as int + 1));
            assert forall|key: i64| #[trigger] at(counts@, key) == sum(after, order_weight(p@, key as int)) by {
                lemma_sum_concat(flat(views.take(index as int)), views[index as int], order_weight(p@, key as int));
            }
            assert(after.len() == flat(views.take(index as int)).len() + chunk@.len());
        }
        index += 1;
    }
    proof { assert(views.take(views.len() as int) =~= views); }
    counts
}


#[verifier::rlimit(20)]
pub fn count_customers(mult: &mut HashMap<i64, i128>, keys: &mut Vec<i64>, chunk: &Vec<CustEntry>, Ghost(done): Ghost<int>)
    requires within(old(mult)@, done), old(mult)@.dom() == old(keys)@.to_set(), old(keys)@.no_duplicates(),
        unit_customers(chunk@), 0 <= done, done + chunk@.len() < 0x40000000000000000000000000000000int,
    ensures within(final(mult)@, done + chunk@.len()), final(mult)@.dom() == final(keys)@.to_set(), final(keys)@.no_duplicates(),
        forall|key: i64| #[trigger] at(final(mult)@, key) == at(old(mult)@, key) + sum(chunk@, customer_weight(key as int)),
        final(keys)@.len() <= old(keys)@.len() + chunk@.len(),
{
    let mut index = 0usize;
    proof {
        assert forall|key: i64| #[trigger] at(mult@, key) == at(old(mult)@, key) + sum(chunk@.take(0), customer_weight(key as int)) by {
            assert(chunk@.take(0) =~= Seq::<CustEntry>::empty());
            lemma_sum_empty(chunk@.take(0), customer_weight(key as int));
        }
    }
    while index < chunk.len()
        invariant index <= chunk.len(), unit_customers(chunk@), 0 <= done, done + chunk@.len() < 0x40000000000000000000000000000000int,
            within(mult@, done + index as int), mult@.dom() == keys@.to_set(), keys@.no_duplicates(),
            keys@.len() <= old(keys)@.len() + index,
            forall|key: i64| #[trigger] at(mult@, key) == at(old(mult)@, key) + sum(chunk@.take(index as int), customer_weight(key as int)),
        decreases chunk.len() - index,
    {
        let entry = &chunk[index];
        let ghost before = mult@;
        proof { assert(chunk@[index as int].sign == 1 || chunk@[index as int].sign == -1); keys@.to_set_ensures(); }
        if !mult.contains_key(&entry.custkey) {
            proof { verus_prooflib::lemma_unique_push(keys@, entry.custkey); }
            keys.push(entry.custkey);
        }
        let current = match mult.get(&entry.custkey) { Some(value) => *value, None => 0 };
        proof {
            assert(-(done + index as int) <= at(before, entry.custkey) <= done + index as int);
            assert(current as int == at(before, entry.custkey));
        }
        mult.insert(entry.custkey, current + entry.sign as i128);
        proof {
            keys@.to_set_ensures();
            assert(mult@.dom() =~= keys@.to_set());
            assert forall|key: i64| #[trigger] at(mult@, key) == at(old(mult)@, key) + sum(chunk@.take(index as int + 1), customer_weight(key as int)) by {
                lemma_sum_take(chunk@, index as int, customer_weight(key as int));
                let weight = customer_weight(key as int)(chunk@[index as int]);
                assert(at(before, key) == at(old(mult)@, key) + sum(chunk@.take(index as int), customer_weight(key as int)));
                if key == entry.custkey {
                    assert(at(mult@, key) == at(before, key) + entry.sign as int);
                    assert(weight == entry.sign as int * 1);
                } else {
                    assert(at(mult@, key) == at(before, key));
                    assert(weight == 0);
                }
            }
            assert forall|key: i64| -(done + index as int + 1) <= #[trigger] at(mult@, key) <= done + index as int + 1 by {
                assert(-(done + index as int) <= at(before, key) <= done + index as int);
            }
        }
        index += 1;
    }
    proof { assert(chunk@.take(chunk@.len() as int) =~= chunk@); }
}

/// Per-key signed multiplicities of the whole customer log, with the distinct keys seen.
#[verifier::rlimit(30)]
pub fn tally_customers(log: &Log<CustEntry>) -> (result: (HashMap<i64, i128>, Vec<i64>))
    requires unit_customers(log.entries()), log.entries().len() < 0x1000000000000000000000000int,
    ensures ({ let (mult, keys) = result;
        &&& forall|key: i64| #[trigger] at(mult@, key) == sum(log.entries(), customer_weight(key as int))
        &&& within(mult@, log.entries().len() as int)
        &&& mult@.dom() == keys@.to_set()
        &&& keys@.no_duplicates()
        &&& keys@.len() <= log.entries().len() }),
{
    let mut mult: HashMap<i64, i128> = HashMap::new();
    let mut keys: Vec<i64> = Vec::new();
    let ghost views = log.views();
    let mut index = 0usize;
    proof {
        assert(views.take(0) =~= Seq::<Seq<CustEntry>>::empty());
        assert forall|key: i64| #[trigger] at(mult@, key) == sum(flat(views.take(0)), customer_weight(key as int)) by {
            lemma_sum_empty(flat(views.take(0)), customer_weight(key as int));
        }
        keys@.to_set_ensures();
        assert(mult@.dom() =~= keys@.to_set());
        assert(flat(views.take(0)).len() == 0);
    }
    while index < log.chunks.len()
        invariant index <= log.chunks.len(), views == log.views(), views.len() == log.chunks.len(),
            unit_customers(log.entries()), log.entries().len() < 0x1000000000000000000000000int,
            within(mult@, flat(views.take(index as int)).len() as int),
            mult@.dom() == keys@.to_set(), keys@.no_duplicates(),
            keys@.len() <= flat(views.take(index as int)).len(),
            forall|key: i64| #[trigger] at(mult@, key) == sum(flat(views.take(index as int)), customer_weight(key as int)),
        decreases log.chunks.len() - index,
    {
        let chunk = &log.chunks[index];
        proof {
            lemma_chunk_unit_customers(views, index as int);
            lemma_flat_prefix_len(views, index as int + 1);
            lemma_flat_step(views, index as int);
            assert(views[index as int] == (**chunk)@);
        }
        count_customers(&mut mult, &mut keys, chunk, Ghost(flat(views.take(index as int)).len() as int));
        proof {
            let after = flat(views.take(index as int + 1));
            assert forall|key: i64| #[trigger] at(mult@, key) == sum(after, customer_weight(key as int)) by {
                lemma_sum_concat(flat(views.take(index as int)), views[index as int], customer_weight(key as int));
            }
            assert(after.len() == flat(views.take(index as int)).len() + chunk@.len());
        }
        index += 1;
    }
    proof { assert(views.take(views.len() as int) =~= views); }
    (mult, keys)
}


pub proof fn lemma_product_bound(multiplicity: int, count: int, limit: int)
    requires 0 <= multiplicity <= limit, -limit <= count <= limit, limit == 0x8000000000000000int,
    ensures -0x40000000000000000000000000000000int <= multiplicity * count <= 0x40000000000000000000000000000000int,
{
    assert(multiplicity * count <= limit * limit) by (nonlinear_arith)
        requires 0 <= multiplicity <= limit, -limit <= count <= limit, limit >= 0;
    assert(multiplicity * count >= -(limit * limit)) by (nonlinear_arith)
        requires 0 <= multiplicity <= limit, -limit <= count <= limit, limit >= 0;
}

pub open spec fn hist_val(map: Map<i128, i128>, bucket: i128) -> int {
    if map.contains_key(bucket) { map[bucket] as int } else { 0 }
}

/// A bucket past the result's range that holds a customer makes every correct answer unfit.
pub proof fn lemma_bucket_unfit(db: Db, p: Seq<u8>, bucket: int)
    requires crate::kernel::histogram_count(db, p, bucket) > 0, !(i64::MIN <= bucket <= i64::MAX),
    ensures !answer_fits(db, params(p)),
{
    crate::join::lemma_query_count(db, p, bucket);
    lemma_answer_ok(db, params(p));
    let out = answer(db, params(p));
    let index = choose|index: int| 0 <= index < out.len() && (#[trigger] out[index]).c_count == bucket;
    if answer_fits(db, params(p)) {
        assert(query_result(db, params(p), out));
        assert(out_fits(out[index]));
    }
}

pub proof fn lemma_no_dups_of(keys: Seq<i128>)
    requires keys.no_duplicates(),
    ensures no_dups(keys),
{}

pub proof fn lemma_out_of_range_zero(entries: Seq<CustEntry>, key: int)
    requires key < i64::MIN || key > i64::MAX,
    ensures sum(entries, customer_weight(key)) == 0,
{
    assert forall|index: int| 0 <= index < entries.len() implies customer_weight(key)(#[trigger] entries[index]) == 0 by {}
    lemma_sum_zero(entries, customer_weight(key));
}

/// The answer for pattern p from the logs: tally per key, then histogram the products.
#[verifier::rlimit(100)]
pub fn replay(orders: &Log<OrderEntry>, customers: &Log<CustEntry>, p: &[u8], Ghost(db): Ghost<Db>)
    -> (result: Option<Vec<(i64, i64)>>)
    requires unit_orders(orders.entries()), unit_customers(customers.entries()),
        orders.entries().len() < 0x8000000000000000, customers.entries().len() < 0x8000000000000000,
        forall|key: int| #[trigger] crate::kernel::order_count(db, p@, key) == sum(orders.entries(), order_weight(p@, key)),
        forall|key: int| #[trigger] crate::kernel::customer_count(db, key) == sum(customers.entries(), customer_weight(key)),
    ensures result matches Some(out) ==> query_result(db, params(p@), out_view(out@)),
        answer_fits(db, params(p@)) ==> result is Some,
{
    let counts = tally_orders(orders, p);
    let (mult, keys) = tally_customers(customers);
    let ghost limit = 0x8000000000000000int;
    let mut hist: HashMap<i128, i128> = HashMap::new();
    let mut buckets: Vec<i128> = Vec::new();
    let mut seen: Vec<i128> = Vec::new();
    let mut index = 0usize;
    proof {
        buckets@.to_set_ensures();
        assert(hist@.dom() =~= buckets@.to_set());
        assert(keys@.take(0) =~= Seq::<i64>::empty());
        assert forall|bucket: i128| #[trigger] hist_val(hist@, bucket) == sum(seen@, |key: i128| crate::facts::contribution(db, p@, key as int, bucket as int)) by {
            lemma_sum_empty(seen@, |key: i128| crate::facts::contribution(db, p@, key as int, bucket as int));
        }
    }
    while index < keys.len()
        invariant index <= keys.len(), keys@.no_duplicates(), mult@.dom() == keys@.to_set(),
            keys@.len() <= customers.entries().len(),
            forall|key: i64| #[trigger] at(counts@, key) == sum(orders.entries(), order_weight(p@, key as int)),
            forall|key: i64| #[trigger] at(mult@, key) == sum(customers.entries(), customer_weight(key as int)),
            within(counts@, orders.entries().len() as int), within(mult@, customers.entries().len() as int),
            forall|key: int| #[trigger] crate::kernel::order_count(db, p@, key) == sum(orders.entries(), order_weight(p@, key)),
            forall|key: int| #[trigger] crate::kernel::customer_count(db, key) == sum(customers.entries(), customer_weight(key)),
            hist@.dom() == buckets@.to_set(), buckets@.no_duplicates(),
            seen@.no_duplicates(), seen.len() <= index,
            forall|position: int| 0 <= position < seen.len() ==> i64::MIN <= #[trigger] seen@[position] <= i64::MAX,
            forall|bucket: i128| #[trigger] hist_val(hist@, bucket) == sum(seen@, |key: i128| crate::facts::contribution(db, p@, key as int, bucket as int)),
            forall|bucket: i128| #[trigger] hist@.contains_key(bucket) ==> 1 <= hist@[bucket] <= seen.len(),
            forall|key: i64| #[trigger] keys@.take(index as int).contains(key) && at(mult@, key) > 0 <==> seen@.contains(key as i128),
        decreases keys.len() - index,
    {
        let key = keys[index];
        let ghost before = seen@;
        let ghost before_hist = hist@;
        proof {
            assert(keys@.contains(key));
            keys@.to_set_ensures();
            assert(mult@.contains_key(key));
        }
        let multiplicity = match mult.get(&key) { Some(value) => *value, None => 0 };
        proof { assert(multiplicity as int == at(mult@, key)); }
        if multiplicity > 0 {
            let orders_of = match counts.get(&key) { Some(value) => *value, None => 0 };
            proof {
                assert(orders_of as int == at(counts@, key));
                assert(-(orders.entries().len() as int) <= orders_of as int <= orders.entries().len() as int);
                assert(multiplicity as int <= customers.entries().len() as int);
                lemma_product_bound(multiplicity as int, orders_of as int, 0x8000000000000000int);
            }
            let bucket = multiplicity * orders_of;
            let frequency = match hist.get(&bucket) { Some(value) => *value, None => 0 };
            proof {
                assert(frequency as int == hist_val(hist@, bucket));
                assert(!seen@.contains(key as i128)) by {
                    if seen@.contains(key as i128) {
                        assert(keys@.take(index as int).contains(key));
                        let at_index = choose|at_index: int| 0 <= at_index < index && keys@[at_index] == key;
                    }
                }
                verus_prooflib::lemma_unique_push(seen@, key as i128);
            }
            hist.insert(bucket, frequency + 1);
            if frequency == 0 {
                proof { verus_prooflib::lemma_unique_push(buckets@, bucket); }
                buckets.push(bucket);
            }
            seen.push(key as i128);
            proof {
                buckets@.to_set_ensures();
                assert(hist@.dom() =~= buckets@.to_set());
                assert forall|other: i128| #[trigger] hist_val(hist@, other) == sum(seen@, |k: i128| crate::facts::contribution(db, p@, k as int, other as int)) by {
                    let weight = |k: i128| crate::facts::contribution(db, p@, k as int, other as int);
                    lemma_sum_push(before, key as i128, weight);
                    assert(seen@ =~= before.push(key as i128));
                    assert(crate::kernel::customer_count(db, key as int) == multiplicity as int);
                    assert(crate::kernel::order_count(db, p@, key as int) == orders_of as int);
                    assert(weight(key as i128) == ind(other == bucket));
                    assert(hist_val(before_hist, other) == sum(before, weight));
                    if other == bucket { assert(hist_val(hist@, other) == frequency as int + 1); }
                    else { assert(hist_val(hist@, other) == hist_val(before_hist, other)); }
                }
                assert forall|other: i128| #[trigger] hist@.contains_key(other) implies 1 <= hist@[other] <= seen.len() by {
                    if other == bucket { assert(hist@[other] == frequency + 1); assert(frequency as int <= before.len()); }
                    else { assert(before_hist.contains_key(other)); assert(hist@[other] == before_hist[other]); }
                }
                assert forall|k: i64| #[trigger] keys@.take(index as int + 1).contains(k) && at(mult@, k) > 0 <==> seen@.contains(k as i128) by {
                    assert(keys@.take(index as int + 1) =~= keys@.take(index as int).push(key));
                    verus_prooflib::lemma_push_membership(keys@.take(index as int), key);
                    verus_prooflib::lemma_push_membership(before, key as i128);
                    if k != key { assert(((k as i128) == (key as i128)) == (k == key)); }
                }
            }
        } else {
            proof {
                assert(keys@.take(index as int + 1) =~= keys@.take(index as int).push(key));
                verus_prooflib::lemma_push_membership(keys@.take(index as int), key);
                assert forall|k: i64| #[trigger] keys@.take(index as int + 1).contains(k) && at(mult@, k) > 0 <==> seen@.contains(k as i128) by {
                    if k == key {
                        assert(at(mult@, k) <= 0);
                        assert(!seen@.contains(k as i128)) by {
                            if seen@.contains(k as i128) { assert(keys@.take(index as int).contains(k)); }
                        }
                    } else {
                        assert(keys@.take(index as int + 1).contains(k) == keys@.take(index as int).contains(k));
                    }
                }
            }
        }
        index += 1;
    }
    proof {
        assert(keys@.take(keys@.len() as int) =~= keys@);
        lemma_no_dups_of(seen@);
        assert forall|key: int| #[trigger] crate::kernel::customer_count(db, key) > 0 <==> has_key(seen@, key) by {
            if key < i64::MIN || key > i64::MAX {
                lemma_out_of_range_zero(customers.entries(), key);
                assert(crate::kernel::customer_count(db, key) == 0);
                assert(!has_key(seen@, key)) by {
                    if has_key(seen@, key) {
                        let at_index = choose|at_index: int| 0 <= at_index < seen@.len() && seen@[at_index] == key as i128;
                        assert(i64::MIN <= seen@[at_index] <= i64::MAX);
                    }
                }
            } else {
                let k = key as i64;
                assert(crate::kernel::customer_count(db, key) == at(mult@, k));
                assert(has_key(seen@, key) == seen@.contains(k as i128));
                if at(mult@, k) > 0 { assert(mult@.contains_key(k)); assert(keys@.contains(k)); }
            }
        }
    }
    proof {
        assert forall|bucket: i128| #[trigger] hist_val(hist@, bucket) == crate::kernel::histogram_count(db, p@, bucket as int) by {
            lemma_seen_hist(db, p@, seen@, bucket as int);
        }
    }
    let mut rows: Vec<(i64, i64)> = Vec::new();
    let mut position = 0usize;
    while position < buckets.len()
        invariant position <= buckets.len(), hist@.dom() == buckets@.to_set(), buckets@.no_duplicates(),
            forall|bucket: i128| #[trigger] hist_val(hist@, bucket) == crate::kernel::histogram_count(db, p@, bucket as int),
            forall|bucket: i128| #[trigger] hist@.contains_key(bucket) ==> 1 <= hist@[bucket] <= seen.len(),
            seen.len() <= keys.len(), keys.len() <= customers.entries().len(), customers.entries().len() < 0x8000000000000000,
            forall|index: int| 0 <= index < rows@.len() ==> {
                &&& (#[trigger] rows@[index]).1 > 0
                &&& rows@[index].1 as int == crate::kernel::histogram_count(db, p@, rows@[index].0 as int)
                &&& buckets@.take(position as int).contains(rows@[index].0 as i128)
            },
            forall|first: int, second: int| #![trigger rows@[first], rows@[second]]
                0 <= first < second < rows@.len() ==> rows@[first].0 != rows@[second].0,
            forall|bucket: i128| #[trigger] buckets@.take(position as int).contains(bucket) ==>
                exists|index: int| 0 <= index < rows@.len() && (#[trigger] rows@[index]).0 as int == bucket as int,
        decreases buckets.len() - position,
    {
        let bucket = buckets[position];
        let ghost previous = rows@;
        proof { assert(buckets@.contains(bucket)); buckets@.to_set_ensures(); assert(hist@.contains_key(bucket)); }
        let frequency = match hist.get(&bucket) { Some(value) => *value, None => 0 };
        proof { assert(frequency as int == hist_val(hist@, bucket)); }
        if bucket > i64::MAX as i128 || bucket < i64::MIN as i128 {
            proof { lemma_bucket_unfit(db, p@, bucket as int); }
            return None;
        }
        rows.push((bucket as i64, frequency as i64));
        proof {
            assert(buckets@.take(position as int + 1) =~= buckets@.take(position as int).push(bucket));
            verus_prooflib::lemma_push_membership(buckets@.take(position as int), bucket);
            assert forall|index: int| 0 <= index < rows@.len() implies {
                &&& (#[trigger] rows@[index]).1 > 0
                &&& rows@[index].1 as int == crate::kernel::histogram_count(db, p@, rows@[index].0 as int)
                &&& buckets@.take(position as int + 1).contains(rows@[index].0 as i128)
            } by {
                if index < previous.len() { assert(rows@[index] == previous[index]); }
            }
            assert forall|first: int, second: int| #![trigger rows@[first], rows@[second]]
                0 <= first < second < rows@.len() implies rows@[first].0 != rows@[second].0 by {
                if second < previous.len() { assert(rows@[first] == previous[first] && rows@[second] == previous[second]); }
                else {
                    assert(rows@[first] == previous[first]);
                    assert(!buckets@.take(position as int).contains(bucket)) by {
                        let at_index = choose|at_index: int| 0 <= at_index < position && buckets@[at_index] == bucket;
                    }
                }
            }
            assert forall|other: i128| #[trigger] buckets@.take(position as int + 1).contains(other) implies
                exists|index: int| 0 <= index < rows@.len() && (#[trigger] rows@[index]).0 as int == other as int by {
                if other == bucket { assert(rows@[rows@.len() - 1].0 as int == other as int); }
                else {
                    let index = choose|index: int| 0 <= index < previous.len() && (#[trigger] previous[index]).0 as int == other as int;
                    assert(rows@[index] == previous[index]);
                }
            }
        }
        position += 1;
    }
    proof {
        assert(buckets@.take(buckets@.len() as int) =~= buckets@);
        assert forall|bucket: int| #[trigger] crate::kernel::histogram_count(db, p@, bucket) > 0 implies
            exists|index: int| 0 <= index < rows@.len() && (#[trigger] rows@[index]).0 as int == bucket by {
            lemma_seen_hist(db, p@, seen@, bucket);
            let weight = |key: i128| crate::facts::contribution(db, p@, key as int, bucket);
            lemma_sum_nonzero_witness(seen@, weight);
            let at_index = choose|at_index: int| 0 <= at_index < seen@.len() && weight(seen@[at_index]) != 0;
            let witness = seen@[at_index];
            assert(i64::MIN <= witness <= i64::MAX);
            let k = witness as i64;
            assert(crate::kernel::customer_count(db, k as int) == at(mult@, k));
            assert(crate::kernel::order_count(db, p@, k as int) == at(counts@, k));
            assert(weight(witness) != 0 ==> crate::kernel::customer_count(db, k as int) * crate::kernel::order_count(db, p@, k as int) == bucket);
            assert(at(mult@, k) > 0 && at(mult@, k) <= customers.entries().len() as int);
            lemma_product_bound(at(mult@, k), at(counts@, k), 0x8000000000000000int);
            assert(i128::MIN <= bucket <= i128::MAX);
            let wide = bucket as i128;
            assert(hist_val(hist@, wide) == crate::kernel::histogram_count(db, p@, bucket));
            assert(hist@.contains_key(wide));
            assert(buckets@.contains(wide));
            assert(buckets@.take(buckets@.len() as int).contains(wide));
        }
    }
    let ghost unsorted = rows@;
    crate::output::sort(&mut rows);
    proof {
        assert forall|row: (i64, i64)| #[trigger] rows@.contains(row) == unsorted.contains(row) by {
            lemma_bag_contains(rows@, row);
            lemma_bag_contains(unsorted, row);
        }
        assert forall|index: int| 0 <= index < rows@.len() implies {
            &&& (#[trigger] rows@[index]).1 > 0
            &&& rows@[index].1 as int == crate::kernel::histogram_count(db, p@, rows@[index].0 as int)
        } by {
            assert(rows@.contains(rows@[index]));
            let original = choose|original: int| 0 <= original < unsorted.len() && unsorted[original] == rows@[index];
        }
        assert forall|bucket: int| #[trigger] crate::kernel::histogram_count(db, p@, bucket) > 0 implies
            exists|index: int| 0 <= index < rows@.len() && (#[trigger] rows@[index]).0 as int == bucket by {
            let original = choose|original: int| 0 <= original < unsorted.len() && (#[trigger] unsorted[original]).0 as int == bucket;
            assert(unsorted.contains(unsorted[original]));
            assert(rows@.contains(unsorted[original]));
            let found = choose|found: int| 0 <= found < rows@.len() && rows@[found] == unsorted[original];
        }
        lemma_execute_facts(db, p@, rows@);
    }
    Some(rows)
}

}

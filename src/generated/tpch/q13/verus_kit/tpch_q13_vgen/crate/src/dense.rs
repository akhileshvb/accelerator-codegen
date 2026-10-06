// Lemmas for the dense (flat record / flat histogram) representation.
use vstd::prelude::*;
use verus_common::sums::*;
use verus_common::rel::*;
use crate::gen_spec::*;
use crate::gen_obligations::*;
use crate::kernel::*;
use crate::facts::*;
use crate::recs::*;
use crate::aggregate::has_key;

verus! {

/// Field f of the record of `key` in a flat array of k records.
pub open spec fn flat_at(flat: Seq<u16>, k: int, key: int, f: int) -> int {
    if 0 <= key < k && 0 <= f < 17 && key * 17 + f < flat.len() { flat[key * 17 + f] as int } else { 0 }
}

pub proof fn lemma_flat_set(old: Seq<u16>, k: int, key0: int, f0: int, v: u16)
    requires 0 <= key0 < k, 0 <= f0 < 17, old.len() == k * 17,
    ensures
        0 <= key0 * 17 + f0 < old.len(),
        forall|key: int, f: int| #[trigger] flat_at(old.update(key0 * 17 + f0, v), k, key, f)
            == if key == key0 && f == f0 { v as int } else { flat_at(old, k, key, f) },
{
    assert(key0 * 17 + f0 < k * 17) by (nonlinear_arith)
        requires 0 <= key0 < k, 0 <= f0 < 17;
    assert forall|key: int, f: int| #[trigger] flat_at(old.update(key0 * 17 + f0, v), k, key, f)
        == if key == key0 && f == f0 { v as int } else { flat_at(old, k, key, f) } by {
        if 0 <= key < k && 0 <= f < 17 {
            assert(key * 17 + f < k * 17) by (nonlinear_arith)
                requires 0 <= key < k, 0 <= f < 17;
            if key * 17 + f == key0 * 17 + f0 {
                assert(key == key0 && f == f0) by (nonlinear_arith)
                    requires key * 17 + f == key0 * 17 + f0, 0 <= f < 17, 0 <= f0 < 17;
            }
        }
    }
}

pub proof fn lemma_flat_in_range(flat: Seq<u16>, k: int, key: int, f: int)
    requires 0 <= key < k, 0 <= f < 17, flat.len() == k * 17,
    ensures key * 17 + f < flat.len(),
{
    assert(key * 17 + f < k * 17) by (nonlinear_arith)
        requires 0 <= key < k, 0 <= f < 17;
}

pub proof fn lemma_rng_no_dups(n: int)
    requires n >= 0,
    ensures no_dups(rng(n)), forall|k: int| #[trigger] rng(n).contains(k) <==> 0 <= k < n,
{
    assert forall|k: int| 0 <= k < n implies #[trigger] rng(n).contains(k) by {
        assert(rng(n)[k] == k);
    }
    assert forall|k: int| #[trigger] rng(n).contains(k) implies 0 <= k < n by {
        let i = choose|i: int| 0 <= i < rng(n).len() && rng(n)[i] == k;
    }
}

/// The histogram is the sum over a key range that covers every customer.
pub proof fn lemma_range_hist(db: Db, p: Seq<u8>, k: int, bucket: int)
    requires k >= 0,
        forall|key: int| customer_count(db, key) > 0 ==> 0 <= key < k,
    ensures histogram_count(db, p, bucket) == sum(rng(k), |key: int| contribution(db, p, key, bucket)),
{
    lemma_rng_no_dups(k);
    lemma_dedup(db.customer.map_values(|row: CustomerRow| row.c_custkey));
    let w = |key: int| contribution(db, p, key, bucket);
    assert forall|key: int| #[trigger] w(key) != 0 implies
        customer_keys(db).contains(key) && rng(k).contains(key) by {
        lemma_customer_support(db, key);
        assert(customer_count(db, key) > 0);
    }
    lemma_sum_support(customer_keys(db), rng(k), w);
}

pub proof fn lemma_hist_nonneg(db: Db, p: Seq<u8>, bucket: int)
    ensures histogram_count(db, p, bucket) >= 0,
{
    assert forall|key: int| #[trigger] contribution(db, p, key, bucket) >= 0 by {}
    lemma_sum_nonneg(customer_keys(db), |key: int| contribution(db, p, key, bucket));
}

pub proof fn lemma_hist_set(old: Seq<i64>, j0: int, b0: int, v: i64)
    requires 0 <= j0 < 16, 0 <= b0 < 1024, old.len() == 16384,
    ensures
        0 <= j0 * 1024 + b0 < old.len(),
        forall|j: int, b: int| #[trigger] hist_at(old.update(j0 * 1024 + b0, v), j, b)
            == if j == j0 && b == b0 { v as int } else { hist_at(old, j, b) },
{
    assert forall|j: int, b: int| #[trigger] hist_at(old.update(j0 * 1024 + b0, v), j, b)
        == if j == j0 && b == b0 { v as int } else { hist_at(old, j, b) } by {
        if 0 <= j < 16 && 0 <= b < 1024 && j * 1024 + b == j0 * 1024 + b0 {
            assert(j == j0 && b == b0) by (nonlinear_arith)
                requires j * 1024 + b == j0 * 1024 + b0, 0 <= b < 1024, 0 <= b0 < 1024;
        }
    }
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

} // verus!

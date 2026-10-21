#![verifier::loop_isolation(false)]
// The table of records and the facts relating it to the database.
use vstd::prelude::*;
use std::sync::Arc;
use verus_common::sums::*;
use verus_common::sqlfns::like_match;
use verus_prooflib::wide::Wide;
use verus_prooflib::ordered::{Index, Key, KeyView};
use crate::gen_spec::*;
use crate::gen_obligations::*;
use crate::kernel::*;
use crate::store::*;
use crate::batch::*;
use crate::facts::{cast_keys};
use verus_common::rel::no_dups;

verus! {

broadcast use vstd::std_specs::hash::group_hash_axioms, vstd::seq_lib::group_seq_properties;

pub open spec fn number_key(number: int) -> KeyView { (number, Seq::empty()) }

pub open spec fn tbl_rec(table: Index<Rec>, key: int) -> Option<Rec> { table.at(number_key(key)) }

pub open spec fn oc_comment(db: Db, key: int, comment: Seq<u8>) -> int {
    sum(db.orders, |row: OrdersRow| ind(row.o_custkey == key && row.o_comment == comment))
}

pub open spec fn noncount_term(p: Seq<u8>) -> spec_fn(Variant) -> int {
    |variant: Variant| if !like_match(cview(variant), p) { variant.count@ } else { 0 }
}

pub open spec fn noncount(variants: Seq<Variant>, p: Seq<u8>) -> int { sum(variants, noncount_term(p)) }

pub open spec fn rec_variants(rec: Option<Rec>) -> Seq<Variant> {
    match rec { Some(rec) => rec.orders@, None => Seq::empty() }
}

pub open spec fn rec_noncount_of(rec: Option<Rec>, p: Seq<u8>) -> int { noncount(rec_variants(rec), p) }

/// The customers a record puts into a bucket (one or none).
pub open spec fn rec_bucket_hit(rec: Option<Rec>, p: Seq<u8>, bucket: int) -> int {
    if rec_mult_of(rec) > 0 && rec_mult_of(rec) * rec_noncount_of(rec, p) == bucket { 1 } else { 0 }
}

pub proof fn lemma_oc_rec(db: Db, p: Seq<u8>, key: int, rec: Option<Rec>)
    requires rec matches Some(r) ==> rec_ok(r),
        forall|comment: Seq<u8>| #[trigger] rec_cc_of(rec, comment) == oc_comment(db, key, comment),
    ensures order_count(db, p, key) == rec_noncount_of(rec, p),
{
    let variants = rec_variants(rec);
    let to_comment = |variant: Variant| cview(variant);
    let comments = variants.map_values(to_comment);
    let selected = |row: OrdersRow| ind(c_orders_on_orders(db, params(p), CustomerRow { c_custkey: key }, row));
    let by_comment = |row: OrdersRow| row.o_comment;
    let per = |comment: Seq<u8>| sum(db.orders, |row: OrdersRow| if by_comment(row) == comment { selected(row) } else { 0int });
    assert forall|index: int| 0 <= index < comments.len() implies comments[index] == cview(variants[index]) by {}
    assert(distinct(comments)) by {
        assert forall|left: int, right: int| 0 <= left < comments.len() && 0 <= right < comments.len() && left != right implies comments[left] != comments[right] by {
            if left < right { assert(cview(variants[left]) != cview(variants[right])); } else { assert(cview(variants[right]) != cview(variants[left])); }
        }
    }
    assert forall|index: int| 0 <= index < db.orders.len() && selected(#[trigger] db.orders[index]) != 0 implies comments.contains(by_comment(db.orders[index])) by {
        let row = db.orders[index];
        let hit = |other: OrdersRow| ind(other.o_custkey == key && other.o_comment == row.o_comment);
        assert forall|position: int| 0 <= position < db.orders.len() implies hit(#[trigger] db.orders[position]) >= 0 by {}
        lemma_sum_pos(db.orders, hit, index);
        assert(oc_comment(db, key, row.o_comment) > 0);
        assert(rec_cc_of(rec, row.o_comment) > 0);
        if !comments.contains(row.o_comment) {
            assert forall|position: int| 0 <= position < variants.len() implies comment_term(row.o_comment)(#[trigger] variants[position]) == 0 by {
                assert(comments[position] == cview(variants[position]));
                assert(!comments.contains(row.o_comment));
                assert(comments[position] != row.o_comment);
            }
            lemma_sum_zero(variants, comment_term(row.o_comment));
            assert(rec_cc_of(rec, row.o_comment) == comment_count(variants, row.o_comment));
            assert(false);
        }
    }
    lemma_sum_group(db.orders, comments, by_comment, selected, per);
    assert forall|comment: Seq<u8>| #[trigger] per(comment) == (if !like_match(comment, p) { 1int } else { 0int }) * oc_comment(db, key, comment) by {
        let matching = |row: OrdersRow| ind(row.o_custkey == key && row.o_comment == comment);
        let weight = if !like_match(comment, p) { 1int } else { 0int };
        let term = |row: OrdersRow| if by_comment(row) == comment { selected(row) } else { 0int };
        assert forall|row: OrdersRow| #[trigger] term(row) == weight * matching(row) by {
            if row.o_comment == comment && row.o_custkey == key { assert(c_orders_on_orders(db, params(p), CustomerRow { c_custkey: key }, row) == !like_match(comment, p)); }
        }
        lemma_sum_scale(db.orders, matching, term, weight);
    }
    let by_variant = |variant: Variant| per(cview(variant));
    lemma_sum_map(variants, to_comment, per, by_variant);
    assert forall|index: int| 0 <= index < variants.len() implies by_variant(#[trigger] variants[index]) == noncount_term(p)(variants[index]) by {
        let variant = variants[index];
        let weight = comment_term(cview(variant));
        assert forall|other: int| 0 <= other < variants.len() && other != index implies weight(#[trigger] variants[other]) == 0 by {
            if other < index { assert(cview(variants[other]) != cview(variants[index])); } else { assert(cview(variants[index]) != cview(variants[other])); }
        }
        lemma_sum_single_match(variants, weight, index);
        assert(rec_cc_of(rec, cview(variant)) == comment_count(variants, cview(variant)));
        assert(oc_comment(db, key, cview(variant)) == variant.count@);
    }
    lemma_sum_ext(variants, by_variant, noncount_term(p));
    assert(order_count(db, p, key) == sum(db.orders, selected));
}


pub fn rec_noncount(rec: &Rec, pattern: &[u8]) -> (result: Wide)
    ensures result@ == noncount(rec.orders@, pattern@),
{
    let mut total = Wide::zero();
    let mut index = 0usize;
    proof { assert(rec.orders@.take(0) =~= Seq::<Variant>::empty()); lemma_sum_empty(rec.orders@.take(0), noncount_term(pattern@)); }
    while index < rec.orders.len()
        invariant index <= rec.orders.len(), total@ == noncount(rec.orders@.take(index as int), pattern@),
        decreases rec.orders.len() - index,
    {
        let variant = &rec.orders[index];
        proof { lemma_sum_take(rec.orders@, index as int, noncount_term(pattern@)); }
        if !verus_prooflib::like_match_exec(variant.comment.as_slice(), pattern) { total = total.add(&variant.count); }
        index += 1;
    }
    proof { assert(rec.orders@.take(rec.orders@.len() as int) =~= rec.orders@); }
    total
}

pub open spec fn some_rec(old: Option<&Rec>) -> Option<Rec> {
    match old { Some(rec) => Some(*rec), None => None }
}

/// The bucket (multiplicity times non-matching orders) a record puts its customer in.
pub fn rec_bucket(rec: Option<&Rec>, pattern: &[u8]) -> (result: Option<Wide>)
    ensures forall|bucket: int| (if result matches Some(value) && value@ == bucket { 1int } else { 0int }) == rec_bucket_hit(some_rec(rec), pattern@, bucket),
{
    match rec {
        Some(rec) => {
            if rec.mult.is_pos() {
                let count = rec_noncount(rec, pattern);
                Some(rec.mult.mul(&count))
            } else { None }
        },
        None => None,
    }
}

/// Moves one customer out of its old bucket and into its new one, for each of the 16 patterns.
#[verifier::spinoff_prover]
#[verifier::rlimit(60)]
pub fn update_hists(hists: &mut Vec<Vec<HEntry>>, patterns: &Vec<Vec<u8>>, previous: Option<&Rec>, current: Option<&Rec>)
    requires old(hists)@.len() == 16, patterns@.len() == 16, forall|j: int| 0 <= j < 16 ==> hist_ok((#[trigger] old(hists)@[j])@),
    ensures final(hists)@.len() == 16, forall|j: int| 0 <= j < 16 ==> hist_ok((#[trigger] final(hists)@[j])@),
        forall|j: int, bucket: int| 0 <= j < 16 ==> #[trigger] hist_val(final(hists)@[j]@, bucket)
            == hist_val(old(hists)@[j]@, bucket) - rec_bucket_hit(some_rec(previous), patterns@[j]@, bucket) + rec_bucket_hit(some_rec(current), patterns@[j]@, bucket),
{
    let mut j = 0usize;
    while j < 16
        invariant j <= 16, hists@.len() == 16, patterns@.len() == 16,
            forall|k: int| 0 <= k < 16 ==> hist_ok((#[trigger] hists@[k])@),
            forall|k: int, bucket: int| 0 <= k < j ==> #[trigger] hist_val(hists@[k]@, bucket)
                == hist_val(old(hists)@[k]@, bucket) - rec_bucket_hit(some_rec(previous), patterns@[k]@, bucket) + rec_bucket_hit(some_rec(current), patterns@[k]@, bucket),
            forall|k: int| j <= k < 16 ==> hists@[k] == old(hists)@[k],
        decreases 16 - j,
    {
        let ghost snapshot = hists@;
        let mut entries = hists.remove(j);
        let ghost before = entries@;
        let old_bucket = rec_bucket(previous, patterns[j].as_slice());
        let new_bucket = rec_bucket(current, patterns[j].as_slice());
        if let Some(bucket) = &old_bucket { hist_add(&mut entries, bucket, -1); }
        if let Some(bucket) = &new_bucket { hist_add(&mut entries, bucket, 1); }
        proof {
            assert forall|bucket: int| #[trigger] hist_val(entries@, bucket)
                == hist_val(before, bucket) - rec_bucket_hit(some_rec(previous), patterns@[j as int]@, bucket) + rec_bucket_hit(some_rec(current), patterns@[j as int]@, bucket) by {
            }
        }
        hists.insert(j, entries);
        proof {
            assert forall|k: int| 0 <= k < 16 implies hist_ok((#[trigger] hists@[k])@) by {
                if k != j as int { assert(hists@[k] == snapshot[if k < j as int { k } else { k }]); }
            }
        }
        j += 1;
    }
}


pub open spec fn shape_ok(rec: Option<Rec>, key: KeyView) -> bool {
    match rec { Some(rec) => key.1 == Seq::<u8>::empty() && i64::MIN <= key.0 <= i64::MAX && rec_ok(rec), None => true }
}

pub open spec fn table_ok(table: Index<Rec>, db: Db) -> bool {
    &&& table.wf()
    &&& forall|key: KeyView| shape_ok(#[trigger] table.at(key), key)
    &&& forall|key: int| #[trigger] rec_mult_of(tbl_rec(table, key)) == customer_count(db, key)
    &&& forall|key: int, comment: Seq<u8>| #[trigger] rec_cc_of(tbl_rec(table, key), comment) == oc_comment(db, key, comment)
}

pub open spec fn hists_ok(hists: Seq<Vec<HEntry>>, patterns: Seq<Vec<u8>>, db: Db) -> bool {
    &&& hists.len() == 16 && patterns.len() == 16
    &&& forall|j: int| 0 <= j < 16 ==> hist_ok((#[trigger] hists[j])@)
    &&& forall|j: int, bucket: int| 0 <= j < 16 ==> #[trigger] hist_val(hists[j]@, bucket) == histogram_count(db, patterns[j]@, bucket)
}

pub open spec fn osum(rows: Seq<OrdersRow>, g: spec_fn((int, Seq<u8>)) -> int) -> int {
    sum(rows, |row: OrdersRow| g((row.o_custkey, row.o_comment)))
}

pub open spec fn obsum(batch: Seq<OChange>, g: spec_fn((int, Seq<u8>)) -> int) -> int {
    sum(batch, |change: OChange| change.sign as int * g((change.custkey as int, (*change.comment)@)))
}

pub open spec fn csum(batch: Seq<CChange>, f: spec_fn(CustomerRow) -> int) -> int {
    sum(batch, |change: CChange| change.sign as int * f(CustomerRow { c_custkey: change.custkey as int }))
}

/// The rows of db1 are those of db0 plus the signed changes of the batch.
pub open spec fn batch_ok(db0: Db, db1: Db, ob: Seq<OChange>, cb: Seq<CChange>) -> bool {
    &&& forall|g: spec_fn((int, Seq<u8>)) -> int| #[trigger] osum(db1.orders, g) == osum(db0.orders, g) + obsum(ob, g)
    &&& forall|f: spec_fn(CustomerRow) -> int| #[trigger] sum(db1.customer, f) == sum(db0.customer, f) + csum(cb, f)
}

pub proof fn lemma_batch_counts(db0: Db, db1: Db, ob: Seq<OChange>, cb: Seq<CChange>, p: Seq<u8>, key: int, comment: Seq<u8>)
    requires batch_ok(db0, db1, ob, cb),
    ensures customer_count(db1, key) == customer_count(db0, key) + sum(cb, customer_term(key)),
        oc_comment(db1, key, comment) == oc_comment(db0, key, comment) + sum(ob, order_term(key, comment)),
        order_count(db1, p, key) == order_count(db0, p, key)
            + sum(ob, |change: OChange| change.sign as int * ind(change.custkey as int == key && !like_match((*change.comment)@, p))),
{
    let customer = |row: CustomerRow| ind(row.c_custkey == key);
    assert(sum(db1.customer, customer) == sum(db0.customer, customer) + csum(cb, customer));
    lemma_sum_ext_all(cb, |change: CChange| change.sign as int * customer(CustomerRow { c_custkey: change.custkey as int }), customer_term(key));
    let by_comment = |row: (int, Seq<u8>)| ind(row.0 == key && row.1 == comment);
    assert(osum(db1.orders, by_comment) == osum(db0.orders, by_comment) + obsum(ob, by_comment));
    lemma_sum_ext_all(ob, |change: OChange| change.sign as int * by_comment((change.custkey as int, (*change.comment)@)), order_term(key, comment));
    let non_matching = |row: (int, Seq<u8>)| ind(row.0 == key && !like_match(row.1, p));
    assert(osum(db1.orders, non_matching) == osum(db0.orders, non_matching) + obsum(ob, non_matching));
    lemma_sum_ext_all(ob, |change: OChange| change.sign as int * non_matching((change.custkey as int, (*change.comment)@)),
        |change: OChange| change.sign as int * ind(change.custkey as int == key && !like_match((*change.comment)@, p)));
    lemma_sum_ext_all(db0.orders, |row: OrdersRow| non_matching((row.o_custkey, row.o_comment)),
        |row: OrdersRow| ind(c_orders_on_orders(db0, params(p), CustomerRow { c_custkey: key }, row)));
    lemma_sum_ext_all(db1.orders, |row: OrdersRow| non_matching((row.o_custkey, row.o_comment)),
        |row: OrdersRow| ind(c_orders_on_orders(db1, params(p), CustomerRow { c_custkey: key }, row)));
    lemma_sum_ext_all(db0.orders, |row: OrdersRow| by_comment((row.o_custkey, row.o_comment)),
        |row: OrdersRow| ind(row.o_custkey == key && row.o_comment == comment));
    lemma_sum_ext_all(db1.orders, |row: OrdersRow| by_comment((row.o_custkey, row.o_comment)),
        |row: OrdersRow| ind(row.o_custkey == key && row.o_comment == comment));
}

pub proof fn lemma_batch_untouched(db0: Db, db1: Db, ob: Seq<OChange>, cb: Seq<CChange>, p: Seq<u8>, key: int)
    requires batch_ok(db0, db1, ob, cb),
        forall|position: int| 0 <= position < ob.len() ==> (#[trigger] ob[position]).custkey as int != key,
        forall|position: int| 0 <= position < cb.len() ==> (#[trigger] cb[position]).custkey as int != key,
    ensures customer_count(db1, key) == customer_count(db0, key), order_count(db1, p, key) == order_count(db0, p, key),
        forall|comment: Seq<u8>| #[trigger] oc_comment(db1, key, comment) == oc_comment(db0, key, comment),
{
    lemma_batch_counts(db0, db1, ob, cb, p, key, Seq::empty());
    assert forall|position: int| 0 <= position < cb.len() implies customer_term(key)(#[trigger] cb[position]) == 0 by {}
    lemma_sum_zero(cb, customer_term(key));
    let weight = |change: OChange| change.sign as int * ind(change.custkey as int == key && !like_match((*change.comment)@, p));
    assert forall|position: int| 0 <= position < ob.len() implies weight(#[trigger] ob[position]) == 0 by {}
    lemma_sum_zero(ob, weight);
    assert forall|comment: Seq<u8>| #[trigger] oc_comment(db1, key, comment) == oc_comment(db0, key, comment) by {
        lemma_batch_counts(db0, db1, ob, cb, p, key, comment);
        assert forall|position: int| 0 <= position < ob.len() implies order_term(key, comment)(#[trigger] ob[position]) == 0 by {}
        lemma_sum_zero(ob, order_term(key, comment));
    }
}


pub proof fn lemma_untouched_key(db0: Db, db1: Db, ob: Seq<OChange>, cb: Seq<CChange>, groups: Map<i64, Group>, keys: Seq<i64>, p: Seq<u8>, key: int)
    requires batch_ok(db0, db1, ob, cb), groups.dom() == keys.to_set(),
        forall|position: int| 0 <= position < ob.len() ==> groups.contains_key(#[trigger] ob[position].custkey),
        forall|position: int| 0 <= position < cb.len() ==> groups.contains_key(#[trigger] cb[position].custkey),
        !exists|position: int| 0 <= position < keys.len() && keys[position] as int == key,
    ensures customer_count(db1, key) == customer_count(db0, key), order_count(db1, p, key) == order_count(db0, p, key),
        forall|comment: Seq<u8>| #[trigger] oc_comment(db1, key, comment) == oc_comment(db0, key, comment),
{
    keys.to_set_ensures();
    assert forall|position: int| 0 <= position < ob.len() implies (#[trigger] ob[position]).custkey as int != key by {
        assert(groups.contains_key(ob[position].custkey));
        assert(keys.contains(ob[position].custkey));
        let found = choose|found: int| 0 <= found < keys.len() && keys[found] == ob[position].custkey;
    }
    assert forall|position: int| 0 <= position < cb.len() implies (#[trigger] cb[position]).custkey as int != key by {
        assert(groups.contains_key(cb[position].custkey));
        assert(keys.contains(cb[position].custkey));
        let found = choose|found: int| 0 <= found < keys.len() && keys[found] == cb[position].custkey;
    }
    lemma_batch_untouched(db0, db1, ob, cb, p, key);
}

pub proof fn lemma_no_dups_cast(keys: Seq<i64>, touched: Seq<i128>)
    requires keys.no_duplicates(), touched.len() == keys.len(), forall|position: int| 0 <= position < keys.len() ==> touched[position] == (#[trigger] keys[position]) as i128,
    ensures no_dups(touched),
        forall|key: int| #[trigger] cast_keys(touched).contains(key) <==> exists|position: int| 0 <= position < keys.len() && keys[position] as int == key,
{
    assert forall|left: int, right: int| 0 <= left < right < touched.len() implies touched[left] != touched[right] by {
        assert(keys[left] != keys[right]);
    }
    assert forall|key: int| #[trigger] cast_keys(touched).contains(key) <==> exists|position: int| 0 <= position < keys.len() && keys[position] as int == key by {
        assert(cast_keys(touched).len() == touched.len());
        if cast_keys(touched).contains(key) {
            let position = choose|position: int| 0 <= position < cast_keys(touched).len() && cast_keys(touched)[position] == key;
            assert(cast_keys(touched)[position] == touched[position] as int);
            assert(touched[position] as int == keys[position] as int);
        }
        if exists|position: int| 0 <= position < keys.len() && keys[position] as int == key {
            let position = choose|position: int| 0 <= position < keys.len() && keys[position] as int == key;
            assert(cast_keys(touched)[position] == touched[position] as int);
            assert(cast_keys(touched).contains(cast_keys(touched)[position]));
        }
    }
}


pub proof fn lemma_not_among(keys: Seq<i64>, key: int)
    requires !exists|position: int| 0 <= position < keys.len() && keys[position] as int == key,
    ensures forall|position: int| 0 <= position < keys.len() ==> (#[trigger] keys[position]) as int != key,
{
    assert forall|position: int| 0 <= position < keys.len() implies (#[trigger] keys[position]) as int != key by {
        if keys[position] as int == key { assert(exists|other: int| 0 <= other < keys.len() && keys[other] as int == key); }
    }
}

}

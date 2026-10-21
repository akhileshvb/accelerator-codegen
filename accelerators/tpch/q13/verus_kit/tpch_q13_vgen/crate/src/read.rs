#![verifier::loop_isolation(false)]
// Answers from a histogram of (bucket, customers).
use vstd::prelude::*;
use verus_common::sums::*;
use verus_common::rel::*;
use verus_prooflib::wide::Wide;
use crate::gen_spec::*;
use crate::gen_obligations::*;
use crate::kernel::*;
use crate::facts::*;
use crate::store::*;
use crate::batch::*;
use crate::core::*;
use crate::merge::lemma_contribution_hit;
use verus_common::rel::no_dups;
use verus_prooflib::ordered::{Index, Key, KeyView, lemma_key_order, ordered_rows};

verus! {

pub proof fn lemma_dedup_len<A>(s: Seq<A>)
    ensures dedup(s).len() <= s.len(),
    decreases s.len(),
{
    if s.len() > 0 { lemma_dedup_len(s.drop_last()); }
}

pub proof fn lemma_hist_le(db: Db, p: Seq<u8>, bucket: int)
    ensures histogram_count(db, p, bucket) <= db.customer.len(),
{
    let keys = customer_keys(db);
    let weight = |key: int| contribution(db, p, key, bucket);
    assert forall|key: int| 0 <= #[trigger] weight(key) <= 1 by {}
    let one = |key: int| 1int;
    verus_prooflib::lemma_sum_ones(keys);
    lemma_sum_nonneg(keys, weight);
    lemma_sum_le(keys, weight, one);
    lemma_dedup_len(db.customer.map_values(|row: CustomerRow| row.c_custkey));
}

/// A bucket past the result's range that holds a customer makes every correct answer unfit.
pub proof fn lemma_bucket_unfit(db: Db, p: Seq<u8>, bucket: int)
    requires histogram_count(db, p, bucket) > 0, !(i64::MIN <= bucket <= i64::MAX),
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

/// The sorted answer rows of a histogram that matches the database.
#[verifier::spinoff_prover]
#[verifier::rlimit(100)]
pub fn rows_from_hist(entries: &Vec<HEntry>, pattern: &[u8], Ghost(db): Ghost<Db>) -> (result: Option<Vec<(i64, i64)>>)
    requires hist_ok(entries@), forall|bucket: int| #[trigger] hist_val(entries@, bucket) == histogram_count(db, pattern@, bucket),
    ensures result matches Some(out) ==> query_result(db, params(pattern@), out_view(out@)),
        envelope(db) && answer_fits(db, params(pattern@)) ==> result is Some,
{
    let mut rows: Vec<(i64, i64)> = Vec::new();
    let mut position = 0usize;
    while position < entries.len()
        invariant position <= entries.len(), hist_ok(entries@),
            forall|bucket: int| #[trigger] hist_val(entries@, bucket) == histogram_count(db, pattern@, bucket),
            forall|index: int| 0 <= index < rows@.len() ==> {
                &&& (#[trigger] rows@[index]).1 > 0
                &&& rows@[index].1 as int == histogram_count(db, pattern@, rows@[index].0 as int)
            },
            forall|index: int| 0 <= index < rows@.len() ==> exists|at: int| 0 <= at < position && entries@[at].bucket@ == (#[trigger] rows@[index]).0 as int,
            forall|first: int, second: int| #![trigger rows@[first], rows@[second]]
                0 <= first < second < rows@.len() ==> rows@[first].0 != rows@[second].0,
            forall|at: int| 0 <= at < position ==> exists|index: int| 0 <= index < rows@.len() && (#[trigger] rows@[index]).0 as int == (#[trigger] entries@[at]).bucket@,
        decreases entries.len() - position,
    {
        let entry = &entries[position];
        let ghost previous = rows@;
        proof {
            assert(hist_val(entries@, entry.bucket@) == histogram_count(db, pattern@, entry.bucket@));
            lemma_entry_val(entries@, position as int);
            lemma_hist_nonneg_entry(db, pattern@, entry.bucket@);
            lemma_hist_le(db, pattern@, entry.bucket@);
            assert(entries@[position as int].freq@ != 0);
            assert(histogram_count(db, pattern@, entry.bucket@) > 0);
        }
        let bucket = match entry.bucket.to_i128() {
            Some(value) if value >= i64::MIN as i128 && value <= i64::MAX as i128 => value as i64,
            _ => {
                proof { lemma_bucket_unfit(db, pattern@, entry.bucket@); }
                return None;
            }
        };
        let frequency = match entry.freq.to_i128() {
            Some(value) if value >= 0 && value <= i64::MAX as i128 => value as i64,
            _ => {
                proof {
                    lemma_hist_le(db, pattern@, entry.bucket@);
                    if envelope(db) { assert(false); }
                }
                return None;
            }
        };
        proof {
            assert(frequency as int == entry.freq@);
            assert(entry.freq@ == histogram_count(db, pattern@, entry.bucket@));
            lemma_hist_nonneg_entry(db, pattern@, entry.bucket@);
        }
        proof { assert(frequency > 0 || frequency as int == histogram_count(db, pattern@, bucket as int)); }
        if frequency > 0 {
            rows.push((bucket, frequency));
        }
        proof {
            assert(bucket as int == entry.bucket@);
            assert forall|index: int| 0 <= index < rows@.len() implies #[trigger] rows@[index].1 > 0 && rows@[index].1 as int == histogram_count(db, pattern@, rows@[index].0 as int) by {
                if index < previous.len() { assert(rows@[index] == previous[index]); }
            }
            assert forall|index: int| 0 <= index < rows@.len() implies exists|at: int| 0 <= at < position + 1 && entries@[at].bucket@ == (#[trigger] rows@[index]).0 as int by {
                if index < previous.len() { assert(rows@[index] == previous[index]); let at = choose|at: int| 0 <= at < position && entries@[at].bucket@ == previous[index].0 as int; }
                else { assert(entries@[position as int].bucket@ == rows@[index].0 as int); }
            }
            assert forall|first: int, second: int| #![trigger rows@[first], rows@[second]]
                0 <= first < second < rows@.len() implies rows@[first].0 != rows@[second].0 by {
                if second < previous.len() { assert(rows@[first] == previous[first]); assert(rows@[second] == previous[second]); }
                else {
                    assert(rows@[first] == previous[first]);
                    let at = choose|at: int| 0 <= at < position && entries@[at].bucket@ == previous[first].0 as int;
                    assert(entries@[at].bucket@ != entries@[position as int].bucket@);
                }
            }
            assert forall|at: int| 0 <= at < position + 1 implies exists|index: int| 0 <= index < rows@.len() && (#[trigger] rows@[index]).0 as int == (#[trigger] entries@[at]).bucket@ by {
                if at < position as int { let index = choose|index: int| 0 <= index < previous.len() && previous[index].0 as int == entries@[at].bucket@; assert(rows@[index] == previous[index]); }
                else { assert(rows@[rows@.len() - 1].0 as int == entries@[at].bucket@); }
            }
        }
        position += 1;
    }
    let ghost unsorted = rows@;
    proof {
        assert forall|first: int, second: int| 0 <= first < second < rows@.len() implies rows@[first] != rows@[second] by {
            assert(rows@[first].0 != rows@[second].0);
        }
        assert forall|bucket: int| #[trigger] histogram_count(db, pattern@, bucket) > 0 implies
            exists|index: int| 0 <= index < unsorted.len() && (#[trigger] unsorted[index]).0 as int == bucket by {
            assert(hist_val(entries@, bucket) > 0);
            lemma_hist_support(entries@, bucket);
            let at = choose|at: int| 0 <= at < entries.len() && entries@[at].bucket@ == bucket;
        }
    }
    crate::output::sort(&mut rows);
    proof {
        assert forall|index: int| 0 <= index < rows@.len() implies #[trigger] rows@[index].1 > 0 && rows@[index].1 as int == histogram_count(db, pattern@, rows@[index].0 as int) by {
            assert(rows@.contains(rows@[index]));
            let original = choose|original: int| 0 <= original < unsorted.len() && unsorted[original] == rows@[index];
        }
        assert forall|bucket: int| #[trigger] histogram_count(db, pattern@, bucket) > 0 implies
            exists|index: int| 0 <= index < rows@.len() && (#[trigger] rows@[index]).0 as int == bucket by {
            let original = choose|original: int| 0 <= original < unsorted.len() && (#[trigger] unsorted[original]).0 as int == bucket;
            assert(unsorted.contains(unsorted[original]));
            assert(rows@.contains(unsorted[original]));
            let found = choose|found: int| 0 <= found < rows@.len() && rows@[found] == unsorted[original];
        }
        lemma_execute_facts(db, pattern@, rows@);
    }
    Some(rows)
}

pub proof fn lemma_entry_val(entries: Seq<HEntry>, at: int)
    requires hist_ok(entries), 0 <= at < entries.len(),
    ensures hist_val(entries, entries[at].bucket@) == entries[at].freq@,
{
    let weight = hist_term(entries[at].bucket@);
    assert forall|other: int| 0 <= other < entries.len() && other != at implies weight(#[trigger] entries[other]) == 0 by {
        if other < at { assert(entries[other].bucket@ != entries[at].bucket@); } else { assert(entries[at].bucket@ != entries[other].bucket@); }
    }
    lemma_sum_single_match(entries, weight, at);
}

pub proof fn lemma_hist_support(entries: Seq<HEntry>, bucket: int)
    requires hist_val(entries, bucket) > 0,
    ensures exists|at: int| 0 <= at < entries.len() && entries[at].bucket@ == bucket,
{
    if !exists|at: int| 0 <= at < entries.len() && entries[at].bucket@ == bucket {
        assert forall|at: int| 0 <= at < entries.len() implies hist_term(bucket)(#[trigger] entries[at]) == 0 by {}
        lemma_sum_zero(entries, hist_term(bucket));
    }
}

pub proof fn lemma_hist_nonneg_entry(db: Db, p: Seq<u8>, bucket: int)
    ensures histogram_count(db, p, bucket) >= 0,
{
    let weight = |key: int| contribution(db, p, key, bucket);
    assert forall|index: int| 0 <= index < customer_keys(db).len() implies weight(#[trigger] customer_keys(db)[index]) >= 0 by {}
    lemma_sum_nonneg(customer_keys(db), weight);
}


/// The histogram of a pattern the table does not track, from one pass over the table.
#[verifier::spinoff_prover]
#[verifier::rlimit(200)]
pub fn scan_table(table: &Index<Rec>, pattern: &[u8], Ghost(db): Ghost<Db>) -> (result: Vec<HEntry>)
    requires table_ok(*table, db),
    ensures hist_ok(result@), forall|bucket: int| #[trigger] hist_val(result@, bucket) == histogram_count(db, pattern@, bucket),
{
    let ghost all = table.entries();
    proof { table.model(); }
    let mut cursor = table.cursor();
    let mut hist: Vec<HEntry> = Vec::new();
    let ghost mut seen: Seq<i128> = Seq::empty();
    let ghost mut position = 0int;
    proof {
        assert forall|bucket: int| hist_val(hist@, bucket) == sum(seen, |key: i128| contribution(db, pattern@, key as int, bucket)) by {
            lemma_sum_empty(hist@, hist_term(bucket));
            lemma_sum_empty(seen, |key: i128| contribution(db, pattern@, key as int, bucket));
        }
    }
    loop
        invariant
            cursor.work() >= 0,
            table.wf(), table_ok(*table, db), all == table.entries(), ordered_rows(all),
            forall|index: int| #![trigger all[index]] 0 <= index < all.len() ==> table.at(all[index].0) == Some(all[index].1),
            cursor.remaining() == all.skip(position), 0 <= position <= all.len(),
            hist_ok(hist@), no_dups(seen),
            forall|bucket: int| #[trigger] hist_val(hist@, bucket) == sum(seen, |key: i128| contribution(db, pattern@, key as int, bucket)),
            forall|key: int| has_key(seen, key) <==> (customer_count(db, key) > 0 && exists|index: int| 0 <= index < position && #[trigger] all[index].0 == number_key(key)),
        decreases all.len() - position,
    {
        let ghost before = cursor.remaining();
        match cursor.next() {
            None => {
                proof {
                    assert(position == all.len());
                    assert forall|key: int| customer_count(db, key) > 0 <==> has_key(seen, key) by {
                        if customer_count(db, key) > 0 {
                            assert(rec_mult_of(tbl_rec(*table, key)) > 0);
                            assert(tbl_rec(*table, key).is_some());
                        }
                    }
                    assert forall|bucket: int| #[trigger] hist_val(hist@, bucket) == histogram_count(db, pattern@, bucket) by {
                        lemma_seen_hist(db, pattern@, seen, bucket);
                    }
                }
                break;
            },
            Some((key, shared)) => {
                proof { assert(all.skip(position).len() > 0); }
                let rec: &Rec = &*shared;
                let ghost entry = all[position];
                proof {
                    assert((key.view(), *rec) == entry);
                    assert(table.at(entry.0) == Some(entry.1));
                    assert(shape_ok(table.at(entry.0), entry.0));
                    assert(entry.0 == number_key(key.number as int));
                }
                let bucket = rec_bucket(Some(rec), pattern);
                let positive = rec.mult.is_pos();
                proof {
                    assert(some_rec(Some(rec)) == Some(entry.1));
                    assert(tbl_rec(*table, key.number as int) == Some(entry.1));
                    lemma_oc_rec(db, pattern@, key.number as int, Some(entry.1));
                }
                if let Some(value) = &bucket { hist_add(&mut hist, value, 1); }
                let ghost old_seen = seen;
                proof {
                    if positive { seen = seen.push(key.number as i128); }
                    if positive { assert(seen =~= old_seen.push(key.number as i128)); assert(seen[old_seen.len() as int] == key.number as i128);
                        assert forall|x: i128| old_seen.contains(x) implies seen.contains(x) by {
                            let at = choose|at: int| 0 <= at < old_seen.len() && old_seen[at] == x;
                            assert(seen[at] == x);
                        }
                    }
                    else { assert(seen == old_seen); }
                    assert forall|b: int| #[trigger] hist_val(hist@, b) == sum(seen, |k: i128| contribution(db, pattern@, k as int, b)) by {
                        lemma_contribution_hit(db, pattern@, key.number as int, Some(entry.1), b);
                        if positive { lemma_sum_push(old_seen, key.number as i128, |k: i128| contribution(db, pattern@, k as int, b)); assert(seen =~= old_seen.push(key.number as i128)); }
                    }
                    assert(rec_mult_of(Some(entry.1)) > 0 <==> positive);
                    if positive {
                        assert(!has_key(old_seen, key.number as int)) by {
                            if has_key(old_seen, key.number as int) {
                                let j = choose|j: int| 0 <= j < position && all[j].0 == number_key(key.number as int);
                                lemma_key_order(all[j].0, all[position].0);
                            }
                        }
                        assert(no_dups(seen)) by {
                            assert forall|a: int, c: int| 0 <= a < c < seen.len() implies seen[a] != seen[c] by {
                                if c == seen.len() - 1 { assert(!old_seen.contains(key.number as i128)); }
                            }
                        }
                    }
                    assert(rec_mult_of(Some(entry.1)) == customer_count(db, key.number as int));
                    assert forall|k: int| has_key(seen, k) <==> (customer_count(db, k) > 0 && exists|index: int| 0 <= index < position + 1 && #[trigger] all[index].0 == number_key(k)) by {
                        if has_key(seen, k) {
                            if k == key.number as int { assert(all[position].0 == number_key(k)); }
                            else {
                                assert(has_key(old_seen, k));
                                let index = choose|index: int| 0 <= index < position && all[index].0 == number_key(k);
                                assert(all[index].0 == number_key(k));
                            }
                        }
                        if customer_count(db, k) > 0 && exists|index: int| 0 <= index < position + 1 && #[trigger] all[index].0 == number_key(k) {
                            let index = choose|index: int| 0 <= index < position + 1 && all[index].0 == number_key(k);
                            if index == position { assert(k == key.number as int); assert(positive); assert(has_key(seen, k)); }
                            else { assert(has_key(old_seen, k)); assert(has_key(seen, k)); }
                        }
                    }
                    assert(cursor.remaining() == all.skip(position + 1));
                    position = position + 1;
                }
            },
        }
    }
    hist
}

}

#![verifier::loop_isolation(false)]
// Merging a batch of signed changes into the table and the histograms.
use vstd::prelude::*;
use std::collections::HashMap;
use std::sync::Arc;
use verus_common::sums::*;
use verus_common::rel::*;
use verus_common::sqlfns::like_match;
use verus_prooflib::wide::Wide;
use verus_prooflib::ordered::{Index, Key, KeyView};
use crate::gen_spec::*;
use crate::gen_obligations::*;
use crate::kernel::*;
use crate::facts::*;
use crate::store::*;
use crate::batch::*;
use crate::core::*;

verus! {

broadcast use vstd::std_specs::hash::group_hash_axioms, vstd::seq_lib::group_seq_properties;

pub proof fn lemma_contribution_hit(db: Db, p: Seq<u8>, key: int, rec: Option<Rec>, bucket: int)
    requires rec_mult_of(rec) == customer_count(db, key), order_count(db, p, key) == rec_noncount_of(rec, p),
    ensures contribution(db, p, key, bucket) == rec_bucket_hit(rec, p, bucket),
{}

#[verifier::spinoff_prover]
#[verifier::rlimit(200)]
pub fn merge_batch(table: &Index<Rec>, hists: &mut Vec<Vec<HEntry>>, patterns: &Vec<Vec<u8>>, ob: &Vec<OChange>, cb: &Vec<CChange>,
    Ghost(db0): Ghost<Db>, Ghost(db1): Ghost<Db>) -> (result: Index<Rec>)
    requires table_ok(*table, db0), hists_ok(old(hists)@, patterns@, db0), batch_ok(db0, db1, ob@, cb@),
    ensures table_ok(result, db1), hists_ok(final(hists)@, patterns@, db1),
{
    let mut groups: HashMap<i64, Group> = HashMap::new();
    let mut keys: Vec<i64> = Vec::new();
    proof {
        keys@.to_set_ensures();
        assert(groups@.dom() =~= keys@.to_set());
        assert forall|key: i64| #[trigger] groups@.contains_key(key) implies distinct_comments(groups@[key].changes@) by {}
    }
    group_orders(&mut groups, &mut keys, ob);
    let ghost after_orders = groups@;
    group_customers(&mut groups, &mut keys, cb);
    proof {
        assert forall|position: int| 0 <= position < ob@.len() implies groups@.contains_key(#[trigger] ob@[position].custkey) by {
            assert(after_orders.contains_key(ob@[position].custkey));
        }
        keys@.to_set_ensures();
        assert forall|key: i64, comment: Seq<u8>| #[trigger] group_cc(groups@, key, comment) == sum(ob@, order_term(key as int, comment)) by {
            assert(group_cc(Map::<i64, Group>::empty(), key, comment) == 0);
        }
        assert forall|key: i64| #[trigger] group_mult(groups@, key) == sum(cb@, customer_term(key as int)) by {
            assert(group_mult(Map::<i64, Group>::empty(), key) == 0);
        }
    }
    let mut result = table.share();
    proof { assert forall|kv: KeyView| #[trigger] result.at(kv) == table.at(kv) by {} }
    let ghost mut touched = Seq::<i128>::empty();
    let mut index = 0usize;
    proof {
        assert forall|j: int, bucket: int| 0 <= j < 16 implies #[trigger] hist_val(hists@[j]@, bucket)
            == histogram_count(db0, patterns@[j]@, bucket) + sum(touched, |key: i128| contribution(db1, patterns@[j]@, key as int, bucket) - contribution(db0, patterns@[j]@, key as int, bucket)) by {
            lemma_sum_empty(touched, |key: i128| contribution(db1, patterns@[j]@, key as int, bucket) - contribution(db0, patterns@[j]@, key as int, bucket));
        }
    }
    while index < keys.len()
        invariant index <= keys.len(), keys@.no_duplicates(), groups@.dom() == keys@.to_set(), groups_ok(groups@),
            forall|key: i64, comment: Seq<u8>| #[trigger] group_cc(groups@, key, comment) == sum(ob@, order_term(key as int, comment)),
            forall|key: i64| #[trigger] group_mult(groups@, key) == sum(cb@, customer_term(key as int)),
            table_ok(*table, db0), batch_ok(db0, db1, ob@, cb@), patterns@.len() == 16,
            result.wf(),
            forall|kv: KeyView| shape_ok(#[trigger] result.at(kv), kv),
            forall|kv: KeyView| (forall|position: int| 0 <= position < index ==> kv != number_key(#[trigger] keys@[position] as int)) ==> #[trigger] result.at(kv) == table.at(kv),
            forall|position: int| 0 <= position < index ==> {
                &&& rec_mult_of(tbl_rec(result, #[trigger] keys@[position] as int)) == customer_count(db1, keys@[position] as int)
                &&& forall|comment: Seq<u8>| rec_cc_of(tbl_rec(result, keys@[position] as int), comment) == oc_comment(db1, keys@[position] as int, comment)
            },
            hists@.len() == 16, forall|j: int| 0 <= j < 16 ==> hist_ok((#[trigger] hists@[j])@),
            touched.len() == index, forall|position: int| 0 <= position < index ==> touched[position] == (#[trigger] keys@[position]) as i128,
            forall|j: int, bucket: int| 0 <= j < 16 ==> #[trigger] hist_val(hists@[j]@, bucket)
                == histogram_count(db0, patterns@[j]@, bucket) + sum(touched, |key: i128| contribution(db1, patterns@[j]@, key as int, bucket) - contribution(db0, patterns@[j]@, key as int, bucket)),
        decreases keys.len() - index,
    {
        let key = keys[index];
        let ghost snapshot_hists = hists@;
        let ghost snapshot_touched = touched;
        let ghost snapshot_result = result;
        proof {
            keys@.to_set_ensures();
            assert(groups@.contains_key(key));
            assert forall|position: int| 0 <= position < index implies keys@[position] != key by {
                assert(keys@[position] != keys@[index as int]);
            }
        }
        let group = groups.get(&key).unwrap();
        let probe = Key::number(key);
        let found = result.get(&probe);
        proof {
            assert(forall|position: int| 0 <= position < index ==> probe.view() != number_key(keys@[position] as int));
            assert(result.at(probe.view()) == table.at(probe.view()));
            assert(tbl_rec(*table, key as int) == table.at(probe.view()));
            assert(number_key(key as int) == probe.view());
        }
        let old_ref: Option<&Rec> = match &found { Some(arc) => Some(&**arc), None => None };
        let ghost old_rec: Option<Rec> = some_rec(old_ref);
        proof {
            assert(result.at(probe.view()) == old_rec);
            assert(shape_ok(table.at(probe.view()), probe.view()));
            assert(old_ref matches Some(rec) ==> rec_ok(*rec));
        }
        let merged = merge_rec(old_ref, group);
        let merged_ref: Option<&Rec> = match &merged { Some(rec) => Some(rec), None => None };
        let ghost new_rec: Option<Rec> = some_rec(merged_ref);
        proof { assert(new_rec == merged); assert(*group == groups@[key]); }
        update_hists(hists, patterns, old_ref, merged_ref);
        proof {
            assert(rec_mult_of(old_rec) == customer_count(db0, key as int));
            assert forall|comment: Seq<u8>| #[trigger] rec_cc_of(old_rec, comment) == oc_comment(db0, key as int, comment) by {}
            lemma_oc_rec(db0, patterns@[0]@, key as int, old_rec);
            assert forall|comment: Seq<u8>| #[trigger] rec_cc_of(new_rec, comment) == oc_comment(db1, key as int, comment) by {
                lemma_batch_counts(db0, db1, ob@, cb@, Seq::empty(), key as int, comment);
                assert(rec_cc_of(old_rec, comment) == oc_comment(db0, key as int, comment));
                assert(group_cc(groups@, key, comment) == comment_count(group.changes@, comment));
                assert(group_cc(groups@, key, comment) == sum(ob@, order_term(key as int, comment)));
                assert(rec_cc_of(merged, comment) == rec_cc_of(old_rec, comment) + comment_count(group.changes@, comment));
            }
            lemma_batch_counts(db0, db1, ob@, cb@, Seq::empty(), key as int, Seq::empty());
            assert(group_mult(groups@, key) == group.mult@);
            assert(group_mult(groups@, key) == sum(cb@, customer_term(key as int)));
            assert(rec_mult_of(new_rec) == rec_mult_of(old_rec) + group.mult@);
            assert(rec_mult_of(new_rec) == customer_count(db1, key as int));
            assert forall|j: int, bucket: int| 0 <= j < 16 implies #[trigger] hist_val(hists@[j]@, bucket)
                == histogram_count(db0, patterns@[j]@, bucket) + sum(touched.push(key as i128), |other: i128| contribution(db1, patterns@[j]@, other as int, bucket) - contribution(db0, patterns@[j]@, other as int, bucket)) by {
                let p = patterns@[j]@;
                lemma_oc_rec(db0, p, key as int, old_rec);
                lemma_oc_rec(db1, p, key as int, new_rec);
                lemma_contribution_hit(db0, p, key as int, old_rec, bucket);
                lemma_contribution_hit(db1, p, key as int, new_rec, bucket);
                lemma_sum_push(touched, key as i128, |other: i128| contribution(db1, p, other as int, bucket) - contribution(db0, p, other as int, bucket));
            }
        }
        let probe_again = probe.share();
        match merged {
            Some(rec) => { result.put(probe, Arc::new(rec)); },
            None => { result.remove(&probe); },
        }
        proof {
            touched = touched.push(key as i128);
            assert(touched.len() == index + 1);
            assert forall|kv: KeyView| (forall|position: int| 0 <= position < index + 1 ==> kv != number_key(#[trigger] keys@[position] as int)) implies #[trigger] result.at(kv) == table.at(kv) by {
                assert(kv != probe_again.view());
                assert(result.at(kv) == snapshot_result.at(kv));
                assert(forall|position: int| 0 <= position < index ==> kv != number_key(keys@[position] as int));
            }
            assert(result.at(probe_again.view()) == new_rec);
            assert forall|kv: KeyView| shape_ok(#[trigger] result.at(kv), kv) by {
                if kv == probe_again.view() { assert(result.at(kv) == new_rec); }
                else { assert(result.at(kv) == snapshot_result.at(kv)); }
            }
            assert forall|position: int| 0 <= position < index + 1 implies {
                &&& rec_mult_of(tbl_rec(result, #[trigger] keys@[position] as int)) == customer_count(db1, keys@[position] as int)
                &&& forall|comment: Seq<u8>| rec_cc_of(tbl_rec(result, keys@[position] as int), comment) == oc_comment(db1, keys@[position] as int, comment)
            } by {
                if position == index as int { assert(number_key(key as int) == probe_again.view()); assert(tbl_rec(result, key as int) == new_rec); }
                else {
                    assert(keys@[position] != key);
                    assert(number_key(keys@[position] as int) != probe_again.view());
                    assert(tbl_rec(result, keys@[position] as int) == tbl_rec(snapshot_result, keys@[position] as int));
                }
            }
            assert forall|position: int| 0 <= position < index + 1 implies touched[position] == (#[trigger] keys@[position]) as i128 by {
                if position < index as int { assert(touched[position] == snapshot_touched[position]); }
            }
        }
        index += 1;
    }
    proof {
        keys@.to_set_ensures();
        assert(forall|position: int| 0 <= position < ob@.len() ==> groups@.contains_key(#[trigger] ob@[position].custkey));
        assert(forall|position: int| 0 <= position < cb@.len() ==> groups@.contains_key(#[trigger] cb@[position].custkey));
        assert(index == keys.len());
        assert forall|key: int| #[trigger] rec_mult_of(tbl_rec(result, key)) == customer_count(db1, key) by {
            if exists|position: int| 0 <= position < keys.len() && keys@[position] as int == key {
                let position = choose|position: int| 0 <= position < keys.len() && keys@[position] as int == key;
                assert(rec_mult_of(tbl_rec(result, keys@[position] as int)) == customer_count(db1, keys@[position] as int));
            } else {
                lemma_not_among(keys@, key);
                assert forall|position: int| 0 <= position < index implies number_key(key) != number_key(#[trigger] keys@[position] as int) by {
                    assert(keys@[position] as int != key);
                }
                assert(result.at(number_key(key)) == table.at(number_key(key)));
                assert(tbl_rec(result, key) == tbl_rec(*table, key));
                assert(rec_mult_of(tbl_rec(*table, key)) == customer_count(db0, key));
                lemma_untouched_key(db0, db1, ob@, cb@, groups@, keys@, Seq::empty(), key);
            }
        }
        assert forall|key: int, comment: Seq<u8>| #[trigger] rec_cc_of(tbl_rec(result, key), comment) == oc_comment(db1, key, comment) by {
            if exists|position: int| 0 <= position < keys.len() && keys@[position] as int == key {
                let position = choose|position: int| 0 <= position < keys.len() && keys@[position] as int == key;
                assert(rec_cc_of(tbl_rec(result, keys@[position] as int), comment) == oc_comment(db1, keys@[position] as int, comment));
            } else {
                lemma_not_among(keys@, key);
                assert forall|position: int| 0 <= position < index implies number_key(key) != number_key(#[trigger] keys@[position] as int) by {
                    assert(keys@[position] as int != key);
                }
                assert(result.at(number_key(key)) == table.at(number_key(key)));
                assert(tbl_rec(result, key) == tbl_rec(*table, key));
                assert(rec_cc_of(tbl_rec(*table, key), comment) == oc_comment(db0, key, comment));
                lemma_untouched_key(db0, db1, ob@, cb@, groups@, keys@, Seq::empty(), key);
            }
        }
        assert forall|j: int, bucket: int| 0 <= j < 16 implies #[trigger] hist_val(hists@[j]@, bucket) == histogram_count(db1, patterns@[j]@, bucket) by {
            let p = patterns@[j]@;
            lemma_no_dups_cast(keys@, touched);
            assert forall|key: int| !#[trigger] cast_keys(touched).contains(key) implies
                customer_count(db0, key) == customer_count(db1, key) && order_count(db0, p, key) == order_count(db1, p, key) by {
                assert(!exists|position: int| 0 <= position < keys.len() && keys@[position] as int == key);
                lemma_untouched_key(db0, db1, ob@, cb@, groups@, keys@, p, key);
            }
            lemma_apply_facts(db0, db1, p, touched, bucket);
        }
    }
    result
}

}

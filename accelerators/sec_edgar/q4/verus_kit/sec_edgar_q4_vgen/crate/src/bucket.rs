// Rows of a table bucketed by their join key: Index key -> the rows with that key, held so that
// every weighted sum over a bucket is the table's sum restricted to the key (`bag_holds`).
use vstd::prelude::*;
use std::sync::Arc;
use verus_common::sums::*;
use verus_common::bag::*;
use verus_prooflib::ordered::{Index, Key, KeyView};

verus! {

/// A row with the key it joins on; rows without a key (a NULL join column) match nothing.
pub trait Keyed: BagRow {
    spec fn vkey(v: Self::V) -> Option<KeyView>;

    fn key(&self) -> (k: Option<Key>)
        ensures
            match k {
                Some(k) => Self::vkey(self.rv()) == Some(k.view()),
                None => Self::vkey(self.rv()).is_none(),
            },
    ;
}

pub open spec fn rows_at<R: Keyed>(b: Index<Vec<R>>, k: KeyView) -> Seq<R::V> {
    match b.at(k) {
        Some(v) => rows_v(v@),
        None => Seq::empty(),
    }
}

pub open spec fn key_weight<R: Keyed>(k: KeyView, f: spec_fn(R::V) -> int) -> spec_fn(R::V) -> int {
    |v: R::V| if R::vkey(v) == Some(k) { f(v) } else { 0 }
}

/// The buckets hold exactly the rows of `tab` (as sums), each under its key.
pub open spec fn bag_holds<R: Keyed>(b: Index<Vec<R>>, tab: Seq<R::V>) -> bool {
    &&& b.wf()
    &&& forall|k: KeyView, f: spec_fn(R::V) -> int| #[trigger] sum(rows_at::<R>(b, k), f) == sum(tab, key_weight::<R>(k, f))
}

pub fn dup_rows<R: Keyed>(v: &Vec<R>) -> (out: Vec<R>)
    ensures rows_v(out@) == rows_v(v@),
{
    let mut out: Vec<R> = Vec::new();
    let mut i: usize = 0;
    while i < v.len()
        invariant
            i <= v.len(),
            rows_v(out@) == rows_v(v@.take(i as int)),
        decreases v.len() - i,
    {
        let r = v[i].dup();
        proof {
            assert(v@.take(i as int + 1) =~= v@.take(i as int).push(v@[i as int]));
            assert(rows_v(v@.take(i as int + 1)) =~= rows_v(v@.take(i as int)).push(v@[i as int].rv()));
        }
        out.push(r);
        proof { assert(rows_v(out@) =~= rows_v(v@.take(i as int + 1))); }
        i += 1;
    }
    proof { assert(v@.take(v.len() as int) =~= v@); }
    out
}

pub fn bag_insert<R: Keyed>(b: &mut Index<Vec<R>>, r: &R, Ghost(tab): Ghost<Seq<R::V>>)
    requires bag_holds::<R>(*old(b), tab),
    ensures bag_holds::<R>(*final(b), tab.push(r.rv())),
{
    let ghost before = *b;
    match r.key() {
        None => {
            proof {
                assert forall|k: KeyView, f: spec_fn(R::V) -> int| #[trigger] sum(rows_at::<R>(before, k), f)
                    == sum(tab.push(r.rv()), key_weight::<R>(k, f)) by {
                    lemma_sum_push(tab, r.rv(), key_weight::<R>(k, f));
                }
            }
        },
        Some(key) => {
            let mut v: Vec<R> = match b.get(&key) {
                Some(old) => dup_rows(&*old),
                None => Vec::new(),
            };
            let ghost old_rows = rows_at::<R>(before, key.view());
            proof { assert(rows_v(v@) == old_rows); }
            v.push(r.dup());
            let ghost pushed = rows_v(v@);
            proof { assert(pushed =~= old_rows.push(r.rv())); }
            b.put(key.share(), Arc::new(v));
            proof {
                assert forall|k: KeyView, f: spec_fn(R::V) -> int| #[trigger] sum(rows_at::<R>(*b, k), f)
                    == sum(tab.push(r.rv()), key_weight::<R>(k, f)) by {
                    lemma_sum_push(tab, r.rv(), key_weight::<R>(k, f));
                    if k == key.view() {
                        lemma_sum_push(old_rows, r.rv(), f);
                    } else {
                        assert(rows_at::<R>(*b, k) == rows_at::<R>(before, k));
                    }
                }
            }
        },
    }
}

pub fn bag_build<R: Keyed>(rows: &Vec<R>) -> (b: Index<Vec<R>>)
    ensures bag_holds::<R>(b, rows_v(rows@)),
{
    let mut b: Index<Vec<R>> = Index::new();
    proof {
        assert forall|k: KeyView, f: spec_fn(R::V) -> int| #[trigger] sum(rows_at::<R>(b, k), f)
            == sum(rows_v(rows@.take(0)), key_weight::<R>(k, f)) by {
            assert(rows_at::<R>(b, k) =~= Seq::<R::V>::empty());
            assert(rows_v(rows@.take(0)) =~= Seq::<R::V>::empty());
        }
    }
    let mut i: usize = 0;
    while i < rows.len()
        invariant
            i <= rows.len(),
            bag_holds::<R>(b, rows_v(rows@.take(i as int))),
        decreases rows.len() - i,
    {
        bag_insert(&mut b, &rows[i], Ghost(rows_v(rows@.take(i as int))));
        proof {
            assert(rows@.take(i as int + 1) =~= rows@.take(i as int).push(rows@[i as int]));
            assert(rows_v(rows@.take(i as int + 1)) =~= rows_v(rows@.take(i as int)).push(rows@[i as int].rv()));
        }
        i += 1;
    }
    proof { assert(rows@.take(rows.len() as int) =~= rows@); }
    b
}

/// A weight that is zero off the bucket's key sums to the same over the bucket as over the table.
pub proof fn lemma_bucket_sum<R: Keyed>(b: Index<Vec<R>>, tab: Seq<R::V>, k: KeyView, f: spec_fn(R::V) -> int)
    requires
        bag_holds::<R>(b, tab),
        forall|v: R::V| #[trigger] f(v) != 0 ==> R::vkey(v) == Some(k),
    ensures
        sum(rows_at::<R>(b, k), f) == sum(tab, f),
{
    assert forall|v: R::V| #[trigger] key_weight::<R>(k, f)(v) == f(v) by {}
    lemma_sum_ext_all(tab, key_weight::<R>(k, f), f);
}

}

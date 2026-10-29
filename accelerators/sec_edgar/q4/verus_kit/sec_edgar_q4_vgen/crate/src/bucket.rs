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

/// Buckets that hold one table hold every other table with the same sums.
pub proof fn lemma_bag_holds_transfer<R: Keyed>(b: Index<Vec<R>>, tab: Seq<R::V>, other: Seq<R::V>)
    requires
        bag_holds::<R>(b, tab),
        forall|f: spec_fn(R::V) -> int| #[trigger] sum(tab, f) == sum(other, f),
    ensures
        bag_holds::<R>(b, other),
{
    assert forall|k: KeyView, f: spec_fn(R::V) -> int| #[trigger] sum(rows_at::<R>(b, k), f) == sum(other, key_weight::<R>(k, f)) by {
        assert(sum(rows_at::<R>(b, k), f) == sum(tab, key_weight::<R>(k, f)));
        assert(sum(tab, key_weight::<R>(k, f)) == sum(other, key_weight::<R>(k, f)));
    }
}


/// A row with a key is in the bucket of its key exactly as often as in the table.
proof fn lemma_in_bucket<R: Keyed>(b: Index<Vec<R>>, tab: Seq<R::V>, x: R::V, k: KeyView)
    requires
        bag_holds::<R>(b, tab),
        R::vkey(x) == Some(k),
        tab.contains(x),
    ensures
        rows_at::<R>(b, k).contains(x),
{
    let f = eqv(x);
    broadcast use vstd::seq_lib::group_to_multiset_ensures;
    lemma_count_as_sum(tab, x);
    assert(tab.to_multiset().count(x) > 0);
    assert(sum(tab, key_weight::<R>(k, f)) == sum(tab, f)) by {
        lemma_sum_ext_all(tab, key_weight::<R>(k, f), f);
    }
    assert(sum(tab, f) > 0);
    let bucket = rows_at::<R>(b, k);
    assert(sum(bucket, f) > 0);
    let i = lemma_sum_nonzero_witness(bucket, f);
    assert(bucket[i] == x);
}

/// Remove one occurrence of a row (which the table has) from its bucket.
pub fn bag_remove<R: Keyed>(b: &mut Index<Vec<R>>, r: &R, Ghost(tab): Ghost<Seq<R::V>>)
    requires
        bag_holds::<R>(*old(b), tab),
        tab.contains(r.rv()),
    ensures
        bag_holds::<R>(*final(b), tab.remove_value(r.rv())),
{
    let ghost before = *b;
    let ghost x = r.rv();
    proof {
        tab.index_of_first_ensures(x);
    }
    let ghost i = tab.index_of_first(x)->0;
    let ghost tab2 = tab.remove(i);
    proof {
        assert(tab.remove_value(x) == tab2);
    }
    match r.key() {
        None => {
            proof {
                assert forall|k: KeyView, f: spec_fn(R::V) -> int| #[trigger] sum(rows_at::<R>(before, k), f)
                    == sum(tab2, key_weight::<R>(k, f)) by {
                    lemma_sum_remove(tab, i, key_weight::<R>(k, f));
                    assert(key_weight::<R>(k, f)(x) == 0);
                }
            }
        },
        Some(key) => {
            proof { lemma_in_bucket::<R>(before, tab, x, key.view()); }
            let found = b.get(&key);
            let old_v = found.unwrap();
            let m = old_v.len();
            let mut q: usize = 0;
            let mut pos: usize = m;
            while q < m
                invariant
                    q <= m == old_v.len(),
                    x == r.rv(),
                    pos == m || (pos < m && old_v@[pos as int].rv() == x),
                    pos == m ==> forall|j: int| 0 <= j < q ==> (#[trigger] old_v@[j]).rv() != x,
                decreases m - q,
            {
                if old_v[q].same(r) {
                    pos = q;
                    q = m;
                } else {
                    q += 1;
                }
            }
            proof {
                assert(rows_v(old_v@) == rows_at::<R>(before, key.view()));
                assert(rows_v(old_v@).contains(x));
                if pos == m {
                    let j = choose|j: int| 0 <= j < rows_v(old_v@).len() && rows_v(old_v@)[j] == x;
                    assert(old_v@[j].rv() == x);
                    assert(false);
                }
            }
            let mut v = dup_rows(&*old_v);
            let ghost v0 = rows_v(v@);
            v.remove(pos);
            proof {
                assert(rows_v(v@) =~= v0.remove(pos as int));
                assert(v0[pos as int] == x);
            }
            b.put(key.share(), Arc::new(v));
            proof {
                assert forall|k: KeyView, f: spec_fn(R::V) -> int| #[trigger] sum(rows_at::<R>(*b, k), f)
                    == sum(tab2, key_weight::<R>(k, f)) by {
                    lemma_sum_remove(tab, i, key_weight::<R>(k, f));
                    if k == key.view() {
                        lemma_sum_remove(v0, pos as int, f);
                        assert(key_weight::<R>(k, f)(x) == f(x));
                    } else {
                        assert(rows_at::<R>(*b, k) == rows_at::<R>(before, k));
                        assert(key_weight::<R>(k, f)(x) == 0);
                    }
                }
            }
        },
    }
}


use vstd::multiset::Multiset;

proof fn lemma_dview_step<T: BagRow>(d: Seq<(T, i64)>, t: int)
    requires 0 <= t < d.len(),
    ensures
        dview(d.take(t + 1)) == dview(d.take(t)).push((d[t].0.rv(), d[t].1 as int)),
        plus_rows(dview(d.take(t + 1))) == if d[t].1 == 1 {
            plus_rows(dview(d.take(t))).push(d[t].0.rv())
        } else {
            plus_rows(dview(d.take(t)))
        },
        minus_rows(dview(d.take(t + 1))) == if d[t].1 == -1 {
            minus_rows(dview(d.take(t))).push(d[t].0.rv())
        } else {
            minus_rows(dview(d.take(t)))
        },
{
    reveal_with_fuel(Seq::filter, 2);
    let a = dview(d.take(t));
    let x = (d[t].0.rv(), d[t].1 as int);
    assert(dview(d.take(t + 1)) =~= a.push(x));
    let b = a.push(x);
    assert(b.drop_last() =~= a);
    let pp = |y: (T::V, int)| y.1 == 1;
    let pm = |y: (T::V, int)| y.1 == -1;
    if d[t].1 == 1 {
        assert(b.filter(pp) =~= a.filter(pp).push(x));
        assert(b.filter(pm) =~= a.filter(pm));
    } else if d[t].1 == -1 {
        assert(b.filter(pm) =~= a.filter(pm).push(x));
        assert(b.filter(pp) =~= a.filter(pp));
    } else {
        assert(b.filter(pp) =~= a.filter(pp));
        assert(b.filter(pm) =~= a.filter(pm));
    }
    assert(plus_rows(b) =~= if d[t].1 == 1 { plus_rows(a).push(d[t].0.rv()) } else { plus_rows(a) });
    assert(minus_rows(b) =~= if d[t].1 == -1 { minus_rows(a).push(d[t].0.rv()) } else { minus_rows(a) });
}

proof fn lemma_minus_prefix<T: BagRow>(d: Seq<(T, i64)>, t: int, x: T::V)
    requires 0 <= t <= d.len(),
    ensures minus_rows(dview(d.take(t))).to_multiset().count(x) <= minus_rows(dview(d)).to_multiset().count(x),
    decreases d.len() - t,
{
    broadcast use vstd::seq_lib::group_to_multiset_ensures, vstd::multiset::group_multiset_axioms;
    if t < d.len() {
        lemma_dview_step(d, t);
        lemma_minus_prefix(d, t + 1, x);
    } else {
        assert(d.take(t) =~= d);
    }
}

/// The buckets of a table with the signed rows applied: inserts first, then deletes.
pub fn bag_apply<R: Keyed>(b0: &Index<Vec<R>>, d: &Vec<(R, i64)>, Ghost(before): Ghost<Seq<R::V>>, Ghost(after): Ghost<Seq<R::V>>)
    -> (b: Index<Vec<R>>)
    requires
        bag_holds::<R>(*b0, before),
        bag_step(before, after, dview(d@)),
        unit_signs(dview(d@)),
    ensures
        bag_holds::<R>(b, after),
{
    broadcast use vstd::seq_lib::group_to_multiset_ensures, vstd::multiset::group_multiset_axioms;
    let n = d.len();
    let mut b = b0.share();
    let ghost mut tab = before;
    proof {
        assert forall|k: KeyView| rows_at::<R>(b, k) == rows_at::<R>(*b0, k) by {
            assert(b.at(k) == b0.at(k));
        }
        assert(bag_holds::<R>(b, before));
        assert(dview(d@.take(0)).len() == 0);
        assert(plus_rows(dview(d@.take(0))) =~= Seq::<R::V>::empty());
        assert(tab =~= before + plus_rows(dview(d@.take(0))));
    }
    let mut t: usize = 0;
    while t < n
        invariant
            t <= n == d@.len(),
            bag_holds::<R>(b, tab),
            tab =~= before + plus_rows(dview(d@.take(t as int))),
        decreases n - t,
    {
        proof { lemma_dview_step(d@, t as int); }
        if d[t].1 == 1 {
            bag_insert(&mut b, &d[t].0, Ghost(tab));
            proof { tab = tab.push(d@[t as int].0.rv()); }
        }
        proof {
            assert(tab =~= before + plus_rows(dview(d@.take(t as int + 1))));
        }
        t += 1;
    }
    proof {
        assert(d@.take(n as int) =~= d@);
        lemma_to_multiset_concat(before, plus_rows(dview(d@)));
        assert(minus_rows(dview(d@.take(0))) =~= Seq::<R::V>::empty());
        lemma_bag_step_multiset(before, after, dview(d@));
    }
    let ghost total = before.to_multiset().add(plus_rows(dview(d@)).to_multiset());
    let mut t: usize = 0;
    while t < n
        invariant
            t <= n == d@.len(),
            bag_holds::<R>(b, tab),
            total == before.to_multiset().add(plus_rows(dview(d@)).to_multiset()),
            tab.to_multiset().add(minus_rows(dview(d@.take(t as int))).to_multiset()) =~= total,
            after.to_multiset().add(minus_rows(dview(d@)).to_multiset()) =~= total,
            unit_signs(dview(d@)),
        decreases n - t,
    {
        proof { lemma_dview_step(d@, t as int); }
        if d[t].1 == -1 {
            let ghost x = d@[t as int].0.rv();
            proof {
                lemma_dview_step(d@, t as int);
                lemma_minus_prefix(d@, t as int + 1, x);
                let mn = minus_rows(dview(d@.take(t as int)));
                mn.to_multiset_ensures();
                assert(mn.push(x).to_multiset() =~= mn.to_multiset().insert(x));
                assert(minus_rows(dview(d@.take(t as int + 1))) == mn.push(x));
                let mall = minus_rows(dview(d@));
                assert(total.count(x) == tab.to_multiset().count(x) + mn.to_multiset().count(x));
                assert(total.count(x) == after.to_multiset().count(x) + mall.to_multiset().count(x));
                assert(mn.push(x).to_multiset().count(x) == mn.to_multiset().count(x) + 1);
                assert(tab.to_multiset().count(x) > 0);
                tab.to_multiset_ensures();
                assert(tab.contains(x));
            }
            bag_remove(&mut b, &d[t].0, Ghost(tab));
            proof {
                let mn = minus_rows(dview(d@.take(t as int)));
                tab.index_of_first_ensures(x);
                let i = tab.index_of_first(x)->0;
                let tab2 = tab.remove_value(x);
                tab.to_multiset_ensures();
                assert(tab.to_multiset().count(x) > 0);
                assert(tab2 == tab.remove(i));
                assert(tab2.to_multiset() =~= tab.to_multiset().remove(x));
                assert(minus_rows(dview(d@.take(t as int + 1))).to_multiset() =~= mn.to_multiset().insert(x));
                tab = tab2;
            }
        }
        proof {
            assert(tab.to_multiset().add(minus_rows(dview(d@.take(t as int + 1))).to_multiset()) =~= total);
        }
        t += 1;
    }
    proof {
        assert(d@.take(n as int) =~= d@);
        lemma_multiset_cancel(tab.to_multiset(), after.to_multiset(), minus_rows(dview(d@)).to_multiset());
        assert forall|f: spec_fn(R::V) -> int| #[trigger] sum(tab, f) == sum(after, f) by {
            lemma_sum_perm(tab, after, f);
        }
        lemma_bag_holds_transfer::<R>(b, tab, after);
    }
    b
}

}

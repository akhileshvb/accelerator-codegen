// Applying signed row deltas to a small table held as a vector (e.g. a
// dimension table).  The result is the new table as a bag:
//   new + deleted = old + inserted   (multisets of row views)
use vstd::prelude::*;
use vstd::multiset::Multiset;
use crate::sums::*;

verus! {

/// A row with a spec view, an executable view-equality test, and a copy.
pub trait BagRow: Sized {
    type V;

    spec fn rv(&self) -> Self::V;

    fn same(&self, o: &Self) -> (r: bool)
        ensures
            r == (self.rv() == o.rv()),
    ;

    fn dup(&self) -> (r: Self)
        ensures
            r.rv() == self.rv(),
    ;
}

// Table as seen by the spec: each stored row replaced by its abstract value, so a table becomes
// a list of plain values. This is how the update function's contract talks about tables.
pub open spec fn rows_v<T: BagRow>(s: Seq<T>) -> Seq<T::V> {
    s.map_values(|x: T| x.rv())
}

// A change list (row, +1/-1) viewed abstractly: rows become plain values and signs become
// integers, so it can be reasoned about with the generic sum/inserted-rows/deleted-rows tools.
pub open spec fn dview<T: BagRow>(d: Seq<(T, i64)>) -> Seq<(T::V, int)> {
    d.map_values(|x: (T, i64)| (x.0.rv(), x.1 as int))
}

/// a + c = b + c  ⇒  a = b
pub proof fn lemma_multiset_cancel<V>(a: Multiset<V>, b: Multiset<V>, c: Multiset<V>)
    requires
        a.add(c) =~= b.add(c),
    ensures
        a =~= b,
{
    broadcast use vstd::multiset::group_multiset_axioms;

    assert forall|v: V| a.count(v) == b.count(v) by {
        assert(a.add(c).count(v) == b.add(c).count(v));
    }
}

// Extending the change list by one more entry extends the inserted-rows (or deleted-rows)
// list by that entry exactly when its sign matches; the per-step fact behind both update loops.
proof fn lemma_dview_take<T: BagRow>(d: Seq<(T, i64)>, t: int)
    requires
        0 <= t < d.len(),
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

/// With no deltas, bag_step means equal bags.
pub proof fn lemma_bag_step_empty<V>(before: Seq<V>, after: Seq<V>, d: Seq<(V, int)>)
    requires
        d.len() == 0,
        bag_step(before, after, d),
    ensures
        after.to_multiset() =~= before.to_multiset(),
{
    broadcast use vstd::seq_lib::group_to_multiset_ensures, vstd::multiset::group_multiset_axioms;

    reveal_with_fuel(Seq::filter, 1);
    lemma_bag_step_multiset(before, after, d);
    assert(plus_rows(d).len() == 0);
    assert(minus_rows(d).len() == 0);
    assert(plus_rows(d).to_multiset() =~= Multiset::<V>::empty());
    assert(minus_rows(d).to_multiset() =~= Multiset::<V>::empty());
    lemma_multiset_cancel(after.to_multiset(), before.to_multiset(), Multiset::<V>::empty());
}

/// apply_bag's result, with the capture contract, is the new table as a bag.
pub proof fn lemma_apply_bag_step<T: BagRow>(
    old: Seq<T>,
    new: Seq<T>,
    d: Seq<(T, i64)>,
    before: Seq<T::V>,
    after: Seq<T::V>,
)
    requires
        unit_signs(dview(d)),
        rows_v(new).to_multiset().add(minus_rows(dview(d)).to_multiset())
            =~= rows_v(old).to_multiset().add(plus_rows(dview(d)).to_multiset()),
        rows_v(old).to_multiset() == before.to_multiset(),
        bag_step(before, after, dview(d)),
    ensures
        rows_v(new).to_multiset() =~= after.to_multiset(),
{
    lemma_bag_step_multiset(before, after, dview(d));
    lemma_multiset_cancel(rows_v(new).to_multiset(), after.to_multiset(), minus_rows(dview(d)).to_multiset());
}

/// old + inserted − deleted.  None if a sign is not ±1 or a deleted row is
/// not present (both impossible for a faithful capture).
pub fn apply_bag<T: BagRow>(old: &Vec<T>, d: &Vec<(T, i64)>) -> (r: Option<Vec<T>>)
    ensures
        r matches Some(v) ==> {
            &&& unit_signs(dview(d@))
            &&& rows_v(v@).to_multiset().add(minus_rows(dview(d@)).to_multiset())
                =~= rows_v(old@).to_multiset().add(plus_rows(dview(d@)).to_multiset())
        },
{
    broadcast use vstd::seq_lib::group_to_multiset_ensures, vstd::multiset::group_multiset_axioms;

    let n = d.len();
    let mut t: usize = 0;
    while t < n
        invariant
            0 <= t <= n == d@.len(),
            forall|i: int| 0 <= i < t ==> (#[trigger] d@[i]).1 == 1 || d@[i].1 == -1,
        decreases n - t,
    {
        if d[t].1 != 1 && d[t].1 != -1 {
            return None;
        }
        t += 1;
    }
    // Every sign in the change list was checked to be +1 or -1, so the abstract list has unit signs.
    proof {
        assert forall|i: int| 0 <= i < dview(d@).len() implies (#[trigger] dview(d@)[i]).1 == 1
            || dview(d@)[i].1 == -1 by {
            assert(d@[i].1 == 1 || d@[i].1 == -1);
        }
    }
    // old rows, then the inserted rows
    let mut r: Vec<T> = Vec::new();
    let mut i: usize = 0;
    while i < old.len()
        invariant
            0 <= i <= old@.len(),
            rows_v(r@) =~= rows_v(old@.take(i as int)),
        decreases old@.len() - i,
    {
        let ghost r0 = r@;
        let y = old[i].dup();
        r.push(y);
        // Copying one more old row grows the result's abstract view by that row, matching the old prefix.
        proof {
            assert(r@ =~= r0.push(y));
            assert(rows_v(r@) =~= rows_v(r0).push(y.rv()));
            assert(old@.take(i as int + 1) =~= old@.take(i as int).push(old@[i as int]));
            assert(rows_v(old@.take(i as int + 1)) =~= rows_v(old@.take(i as int)).push(old@[i as int].rv()));
        }
        i += 1;
    }
    // The whole old table is copied; also set up the empty-prefix facts for the insertion loop.
    proof {
        assert(old@.take(old@.len() as int) =~= old@);
        assert(dview(d@.take(0)).len() == 0);
        assert(plus_rows(dview(d@.take(0))) =~= Seq::<T::V>::empty());
    }
    let mut t: usize = 0;
    while t < n
        invariant
            0 <= t <= n == d@.len(),
            rows_v(r@) =~= rows_v(old@) + plus_rows(dview(d@.take(t as int))),
        decreases n - t,
    {
        // Insertion step: the result gains this entry's row only if it is an insert (+1).
        proof {
            lemma_dview_take(d@, t as int);
        }
        if d[t].1 == 1 {
            let ghost r0 = r@;
            let y = d[t].0.dup();
            r.push(y);
            proof {
                assert(r@ =~= r0.push(y));
                assert(rows_v(r@) =~= rows_v(r0).push(y.rv()));
            }
        }
        t += 1;
    }
    // After all inserts, result = old table + inserted rows, which as a multiset is the sum of both;
    // also set up the deleted-rows bookkeeping for the deletion loop.
    proof {
        assert(d@.take(n as int) =~= d@);
        lemma_to_multiset_concat(rows_v(old@), plus_rows(dview(d@)));
        assert(minus_rows(dview(d@.take(0))) =~= Seq::<T::V>::empty());
    }
    let ghost total = rows_v(old@).to_multiset().add(plus_rows(dview(d@)).to_multiset());
    // remove the deleted rows
    let mut t: usize = 0;
    while t < n
        invariant
            0 <= t <= n == d@.len(),
            total == rows_v(old@).to_multiset().add(plus_rows(dview(d@)).to_multiset()),
            rows_v(r@).to_multiset().add(minus_rows(dview(d@.take(t as int))).to_multiset()) =~= total,
        decreases n - t,
    {
        // Deletion step: the deleted-rows list grows by this entry's row only if it is a delete (-1).
        proof {
            lemma_dview_take(d@, t as int);
        }
        if d[t].1 == -1 {
            let m = r.len();
            let mut k: usize = 0;
            let mut pos: usize = m;
            while k < m
                invariant
                    0 <= k <= m == r@.len(),
                    t < n == d@.len(),
                    pos == m || (pos < m && r@[pos as int].rv() == d@[t as int].0.rv()),
                decreases m - k,
            {
                if r[k].same(&d[t].0) {
                    pos = k;
                    k = m;
                } else {
                    k += 1;
                }
            }
            if pos == m {
                return None;
            }
            let k = pos;
            let ghost r0 = r@;
            let ghost x = d@[t as int].0.rv();
            r.remove(k);
            // Removing the matched row drops exactly one occurrence of that value from the result's multiset,
            // the same as adding it to the deleted-rows multiset, so the loop invariant is preserved.
            proof {
                assert(r0[k as int].rv() == x);
                assert(rows_v(r@) =~= rows_v(r0).remove(k as int));
                assert(rows_v(r0)[k as int] == x);
                let mn = minus_rows(dview(d@.take(t as int)));
                rows_v(r0).to_multiset_ensures();
                mn.to_multiset_ensures();
                assert(rows_v(r@).to_multiset() =~= rows_v(r0).to_multiset().remove(x));
                assert(mn.push(x).to_multiset() =~= mn.to_multiset().insert(x));
                assert(rows_v(r0).to_multiset().count(x) > 0) by {
                    assert(rows_v(r0).contains(x));
                }
            }
        }
        t += 1;
    }
    // All entries processed: the invariant is the final guarantee, new + deleted rows = old + inserted rows.
    proof {
        assert(d@.take(n as int) =~= d@);
    }
    Some(r)
}

} // verus!

// Sorted sequences under a strict order: a sequence strictly sorted by `lt`
// is determined by its elements (uniqueness), and every finite set of
// pairwise-comparable elements has one (existence, by insertion sort).  The
// generated specs use these to show that ORDER BY pins down one answer.
use vstd::prelude::*;

verus! {

/// lt is irreflexive and transitive.
pub open spec fn strict_order<T>(lt: spec_fn(T, T) -> bool) -> bool {
    &&& forall|x: T| !#[trigger] lt(x, x)
    &&& forall|x: T, y: T, z: T| #[trigger] lt(x, y) && #[trigger] lt(y, z) ==> lt(x, z)
}

/// Any two different elements of xs are ordered one way or the other.
pub open spec fn total_on<T>(lt: spec_fn(T, T) -> bool, xs: Seq<T>) -> bool {
    forall|x: T, y: T| xs.contains(x) && xs.contains(y) && x != y ==> #[trigger] lt(x, y) || #[trigger] lt(y, x)
}

pub open spec fn sorted_by<T>(s: Seq<T>, lt: spec_fn(T, T) -> bool) -> bool {
    forall|i: int, j: int| 0 <= i < j < s.len() ==> lt(#[trigger] s[i], #[trigger] s[j])
}

/// Two sequences strictly sorted by the same strict order, with the same
/// elements, are equal.
pub proof fn lemma_sorted_unique<T>(a: Seq<T>, b: Seq<T>, lt: spec_fn(T, T) -> bool)
    requires
        strict_order(lt),
        sorted_by(a, lt),
        sorted_by(b, lt),
        forall|x: T| a.contains(x) <==> b.contains(x),
    ensures
        a == b,
    decreases a.len(),
{
    if a.len() == 0 {
        if b.len() > 0 {
            assert(b.contains(b[0]));
        }
        assert(a =~= b);
    } else {
        assert(a.contains(a[0]));
        let j = choose|j: int| 0 <= j < b.len() && b[j] == a[0];
        assert(b.contains(b[0]));
        let i = choose|i: int| 0 <= i < a.len() && a[i] == b[0];
        if i > 0 && j > 0 {
            assert(lt(a[0], a[i]));
            assert(lt(b[0], b[j]));
        } else if i > 0 {
            assert(lt(a[0], a[i]));
        } else if j > 0 {
            assert(lt(b[0], b[j]));
        }
        assert(a[0] == b[0]);
        let (a1, b1) = (a.drop_first(), b.drop_first());
        assert(sorted_by(a1, lt)) by {
            assert forall|p: int, q: int| 0 <= p < q < a1.len() implies lt(#[trigger] a1[p], #[trigger] a1[q]) by {
                assert(a1[p] == a[p + 1] && a1[q] == a[q + 1]);
            }
        }
        assert(sorted_by(b1, lt)) by {
            assert forall|p: int, q: int| 0 <= p < q < b1.len() implies lt(#[trigger] b1[p], #[trigger] b1[q]) by {
                assert(b1[p] == b[p + 1] && b1[q] == b[q + 1]);
            }
        }
        assert forall|x: T| a1.contains(x) <==> b1.contains(x) by {
            if a1.contains(x) {
                let p = choose|p: int| 0 <= p < a1.len() && a1[p] == x;
                assert(a[p + 1] == x);
                assert(lt(a[0], a[p + 1]));
                assert(b.contains(x));
                let q = choose|q: int| 0 <= q < b.len() && b[q] == x;
                assert(q != 0);
                assert(b1[q - 1] == x);
            }
            if b1.contains(x) {
                let p = choose|p: int| 0 <= p < b1.len() && b1[p] == x;
                assert(b[p + 1] == x);
                assert(lt(b[0], b[p + 1]));
                assert(a.contains(x));
                let q = choose|q: int| 0 <= q < a.len() && a[q] == x;
                assert(q != 0);
                assert(a1[q - 1] == x);
            }
        }
        lemma_sorted_unique(a1, b1, lt);
        assert(a =~= seq![a[0]] + a1);
        assert(b =~= seq![b[0]] + b1);
    }
}

/// Insert x into a strictly sorted s.
pub open spec fn insert_by<T>(s: Seq<T>, x: T, lt: spec_fn(T, T) -> bool) -> Seq<T>
    decreases s.len(),
{
    if s.len() == 0 {
        seq![x]
    } else if lt(x, s[0]) {
        seq![x] + s
    } else {
        seq![s[0]] + insert_by(s.drop_first(), x, lt)
    }
}

pub proof fn lemma_insert_by<T>(s: Seq<T>, x: T, lt: spec_fn(T, T) -> bool)
    requires
        strict_order(lt),
        sorted_by(s, lt),
        !s.contains(x),
        forall|y: T| s.contains(y) ==> #[trigger] lt(x, y) || lt(y, x),
    ensures
        sorted_by(insert_by(s, x, lt), lt),
        forall|y: T| #[trigger] insert_by(s, x, lt).contains(y) <==> s.contains(y) || y == x,
    decreases s.len(),
{
    let r = insert_by(s, x, lt);
    if s.len() == 0 {
        assert(r =~= seq![x]);
        assert forall|y: T| #[trigger] r.contains(y) <==> s.contains(y) || y == x by {
            if r.contains(y) {
                let k = choose|k: int| 0 <= k < r.len() && r[k] == y;
            }
            if y == x {
                assert(r[0] == x);
            }
        }
    } else if lt(x, s[0]) {
        assert forall|i: int, j: int| 0 <= i < j < r.len() implies lt(#[trigger] r[i], #[trigger] r[j]) by {
            if i == 0 {
                assert(r[j] == s[j - 1]);
                if j - 1 > 0 {
                    assert(lt(s[0], s[j - 1]));
                }
            } else {
                assert(r[i] == s[i - 1] && r[j] == s[j - 1]);
            }
        }
        assert forall|y: T| #[trigger] r.contains(y) <==> s.contains(y) || y == x by {
            if r.contains(y) {
                let k = choose|k: int| 0 <= k < r.len() && r[k] == y;
                if k > 0 {
                    assert(s[k - 1] == y);
                }
            }
            if s.contains(y) {
                let k = choose|k: int| 0 <= k < s.len() && s[k] == y;
                assert(r[k + 1] == y);
            }
            if y == x {
                assert(r[0] == x);
            }
        }
    } else {
        let s1 = s.drop_first();
        assert(s.contains(s[0]));
        assert(lt(s[0], x));
        assert(sorted_by(s1, lt)) by {
            assert forall|p: int, q: int| 0 <= p < q < s1.len() implies lt(#[trigger] s1[p], #[trigger] s1[q]) by {
                assert(s1[p] == s[p + 1] && s1[q] == s[q + 1]);
            }
        }
        assert forall|y: T| s1.contains(y) implies #[trigger] lt(x, y) || lt(y, x) by {
            let k = choose|k: int| 0 <= k < s1.len() && s1[k] == y;
            assert(s[k + 1] == y);
        }
        assert(!s1.contains(x)) by {
            if s1.contains(x) {
                let k = choose|k: int| 0 <= k < s1.len() && s1[k] == x;
                assert(s[k + 1] == x);
            }
        }
        lemma_insert_by(s1, x, lt);
        let r1 = insert_by(s1, x, lt);
        assert(r =~= seq![s[0]] + r1);
        assert forall|i: int, j: int| 0 <= i < j < r.len() implies lt(#[trigger] r[i], #[trigger] r[j]) by {
            if i == 0 {
                assert(r[j] == r1[j - 1]);
                assert(r1.contains(r1[j - 1]));
                if r1[j - 1] != x {
                    let k = choose|k: int| 0 <= k < s1.len() && s1[k] == r1[j - 1];
                    assert(s[k + 1] == r1[j - 1]);
                }
            } else {
                assert(r[i] == r1[i - 1] && r[j] == r1[j - 1]);
            }
        }
        assert forall|y: T| #[trigger] r.contains(y) <==> s.contains(y) || y == x by {
            if r.contains(y) {
                let k = choose|k: int| 0 <= k < r.len() && r[k] == y;
                if k == 0 {
                    assert(s[0] == y);
                } else {
                    assert(r1[k - 1] == y);
                    assert(r1.contains(y));
                    if s1.contains(y) {
                        let m = choose|m: int| 0 <= m < s1.len() && s1[m] == y;
                        assert(s[m + 1] == y);
                    }
                }
            }
            if s.contains(y) {
                let k = choose|k: int| 0 <= k < s.len() && s[k] == y;
                if k == 0 {
                    assert(r[0] == y);
                } else {
                    assert(s1[k - 1] == y);
                    assert(r1.contains(y));
                    let m = choose|m: int| 0 <= m < r1.len() && r1[m] == y;
                    assert(r[m + 1] == y);
                }
            }
            if y == x {
                assert(r1.contains(x));
                let m = choose|m: int| 0 <= m < r1.len() && r1[m] == x;
                assert(r[m + 1] == x);
            }
        }
    }
}

/// The elements of xs, without repeats, strictly sorted by lt.
pub open spec fn sort_by<T>(xs: Seq<T>, lt: spec_fn(T, T) -> bool) -> Seq<T>
    decreases xs.len(),
{
    if xs.len() == 0 {
        Seq::empty()
    } else {
        let s = sort_by(xs.drop_last(), lt);
        if s.contains(xs.last()) {
            s
        } else {
            insert_by(s, xs.last(), lt)
        }
    }
}

pub proof fn lemma_sort_by<T>(xs: Seq<T>, lt: spec_fn(T, T) -> bool)
    requires
        strict_order(lt),
        total_on(lt, xs),
    ensures
        sorted_by(sort_by(xs, lt), lt),
        forall|y: T| #[trigger] sort_by(xs, lt).contains(y) <==> xs.contains(y),
    decreases xs.len(),
{
    if xs.len() > 0 {
        let xs0 = xs.drop_last();
        assert forall|x: T, y: T| xs0.contains(x) && xs0.contains(y) && x != y implies #[trigger] lt(x, y) || #[trigger] lt(y, x) by {
            let i = choose|i: int| 0 <= i < xs0.len() && xs0[i] == x;
            let j = choose|j: int| 0 <= j < xs0.len() && xs0[j] == y;
            assert(xs[i] == x && xs[j] == y);
        }
        lemma_sort_by(xs0, lt);
        let s = sort_by(xs0, lt);
        let x = xs.last();
        assert(xs.contains(x));
        assert forall|y: T| #[trigger] xs.contains(y) <==> xs0.contains(y) || y == x by {
            if xs.contains(y) && y != x {
                let k = choose|k: int| 0 <= k < xs.len() && xs[k] == y;
                assert(k < xs.len() - 1);
                assert(xs0[k] == y);
            }
            if xs0.contains(y) {
                let k = choose|k: int| 0 <= k < xs0.len() && xs0[k] == y;
                assert(xs[k] == y);
            }
        }
        if !s.contains(x) {
            assert forall|y: T| s.contains(y) implies #[trigger] lt(x, y) || lt(y, x) by {
                assert(xs.contains(y));
            }
            lemma_insert_by(s, x, lt);
        }
    }
}

/// [f(a) for a in s if keep(a)]
pub open spec fn map_where<A, T>(s: Seq<A>, keep: spec_fn(A) -> bool, f: spec_fn(A) -> T) -> Seq<T>
    decreases s.len(),
{
    if s.len() == 0 {
        Seq::empty()
    } else {
        let r = map_where(s.drop_last(), keep, f);
        if keep(s.last()) {
            r.push(f(s.last()))
        } else {
            r
        }
    }
}

pub proof fn lemma_map_where<A, T>(s: Seq<A>, keep: spec_fn(A) -> bool, f: spec_fn(A) -> T)
    ensures
        forall|y: T| #[trigger] map_where(s, keep, f).contains(y) <==> exists|i: int|
            0 <= i < s.len() && keep(s[i]) && f(s[i]) == y,
    decreases s.len(),
{
    if s.len() > 0 {
        let s0 = s.drop_last();
        lemma_map_where(s0, keep, f);
        let r0 = map_where(s0, keep, f);
        let r = map_where(s, keep, f);
        assert forall|y: T| #[trigger] r.contains(y) <==> exists|i: int|
            0 <= i < s.len() && keep(s[i]) && f(s[i]) == y by {
            if r.contains(y) {
                let k = choose|k: int| 0 <= k < r.len() && r[k] == y;
                if k < r0.len() {
                    assert(r0[k] == y);
                    assert(r0.contains(y));
                    let i = choose|i: int| 0 <= i < s0.len() && keep(s0[i]) && f(s0[i]) == y;
                    assert(s[i] == s0[i]);
                } else {
                    assert(keep(s.last()) && f(s.last()) == y);
                    assert(s[s.len() - 1] == s.last());
                }
            }
            if exists|i: int| 0 <= i < s.len() && keep(s[i]) && f(s[i]) == y {
                let i = choose|i: int| 0 <= i < s.len() && keep(s[i]) && f(s[i]) == y;
                if i < s.len() - 1 {
                    assert(s0[i] == s[i]);
                    assert(r0.contains(y));
                    let k = choose|k: int| 0 <= k < r0.len() && r0[k] == y;
                    if keep(s.last()) {
                        assert(r[k] == y);
                    }
                } else {
                    assert(r[r.len() - 1] == y);
                }
            }
        }
    }
}

// ── Bags (duplicates kept) ─────────────────────────────────────────────────
/// No later element sorts strictly before an earlier one (ties in any order).
pub open spec fn nondesc<T>(s: Seq<T>, lt: spec_fn(T, T) -> bool) -> bool {
    forall|i: int, j: int| 0 <= i < j < s.len() ==> !lt(#[trigger] s[j], #[trigger] s[i])
}

/// Insertion sort keeping duplicates.
pub open spec fn sort_dups<T>(xs: Seq<T>, lt: spec_fn(T, T) -> bool) -> Seq<T>
    decreases xs.len(),
{
    if xs.len() == 0 {
        Seq::empty()
    } else {
        insert_by(sort_dups(xs.drop_last(), lt), xs.last(), lt)
    }
}

proof fn lemma_prepend_multiset<T>(a: T, t: Seq<T>)
    ensures
        (seq![a] + t).to_multiset() =~= t.to_multiset().insert(a),
{
    broadcast use vstd::seq_lib::group_to_multiset_ensures;
    let u = seq![a] + t;
    assert(u.remove(0) =~= t);
    assert(u.contains(a)) by {
        assert(u[0] == a);
    }
    assert(u.to_multiset().count(a) > 0);
}

pub proof fn lemma_insert_by_dups<T>(s: Seq<T>, x: T, lt: spec_fn(T, T) -> bool)
    requires
        strict_order(lt),
        nondesc(s, lt),
        forall|y: T| s.contains(y) && y != x ==> #[trigger] lt(x, y) || lt(y, x),
    ensures
        nondesc(insert_by(s, x, lt), lt),
        insert_by(s, x, lt).to_multiset() =~= s.to_multiset().insert(x),
        forall|y: T| #[trigger] insert_by(s, x, lt).contains(y) <==> s.contains(y) || y == x,
    decreases s.len(),
{
    broadcast use vstd::seq_lib::group_to_multiset_ensures;
    let r = insert_by(s, x, lt);
    if s.len() == 0 {
        assert(r =~= seq![x] + s);
        lemma_prepend_multiset(x, s);
        assert forall|y: T| #[trigger] r.contains(y) <==> s.contains(y) || y == x by {
            if r.contains(y) {
                let k = choose|k: int| 0 <= k < r.len() && r[k] == y;
            }
            if y == x {
                assert(r[0] == x);
            }
        }
    } else if lt(x, s[0]) {
        lemma_prepend_multiset(x, s);
        assert forall|i: int, j: int| 0 <= i < j < r.len() implies !lt(#[trigger] r[j], #[trigger] r[i]) by {
            if i == 0 {
                assert(r[j] == s[j - 1]);
                if lt(s[j - 1], x) {
                    assert(lt(s[j - 1], s[0]));
                    if j - 1 > 0 {
                        assert(!lt(s[j - 1], s[0]));
                    }
                }
            } else {
                assert(r[i] == s[i - 1] && r[j] == s[j - 1]);
            }
        }
        assert forall|y: T| #[trigger] r.contains(y) <==> s.contains(y) || y == x by {
            if r.contains(y) {
                let k = choose|k: int| 0 <= k < r.len() && r[k] == y;
                if k > 0 {
                    assert(s[k - 1] == y);
                }
            }
            if s.contains(y) {
                let k = choose|k: int| 0 <= k < s.len() && s[k] == y;
                assert(r[k + 1] == y);
            }
            if y == x {
                assert(r[0] == x);
            }
        }
    } else {
        let s1 = s.drop_first();
        assert(nondesc(s1, lt)) by {
            assert forall|p: int, q: int| 0 <= p < q < s1.len() implies !lt(#[trigger] s1[q], #[trigger] s1[p]) by {
                assert(s1[p] == s[p + 1] && s1[q] == s[q + 1]);
            }
        }
        assert forall|y: T| s1.contains(y) && y != x implies #[trigger] lt(x, y) || lt(y, x) by {
            let k = choose|k: int| 0 <= k < s1.len() && s1[k] == y;
            assert(s[k + 1] == y);
        }
        lemma_insert_by_dups(s1, x, lt);
        let r1 = insert_by(s1, x, lt);
        assert(r =~= seq![s[0]] + r1);
        lemma_prepend_multiset(s[0], r1);
        lemma_prepend_multiset(s[0], s1);
        assert(s =~= seq![s[0]] + s1);
        assert forall|y: T| #[trigger] r.contains(y) <==> s.contains(y) || y == x by {
            if r.contains(y) {
                let k = choose|k: int| 0 <= k < r.len() && r[k] == y;
                if k == 0 {
                    assert(s[0] == y);
                } else {
                    assert(r1[k - 1] == y);
                    assert(r1.contains(y));
                    if s1.contains(y) {
                        let m = choose|m: int| 0 <= m < s1.len() && s1[m] == y;
                        assert(s[m + 1] == y);
                    }
                }
            }
            if s.contains(y) {
                let k = choose|k: int| 0 <= k < s.len() && s[k] == y;
                if k == 0 {
                    assert(r[0] == y);
                } else {
                    assert(s1[k - 1] == y);
                    assert(r1.contains(y));
                    let m = choose|m: int| 0 <= m < r1.len() && r1[m] == y;
                    assert(r[m + 1] == y);
                }
            }
            if y == x {
                assert(r1.contains(x));
                let m = choose|m: int| 0 <= m < r1.len() && r1[m] == x;
                assert(r[m + 1] == x);
            }
        }
        assert forall|i: int, j: int| 0 <= i < j < r.len() implies !lt(#[trigger] r[j], #[trigger] r[i]) by {
            if i == 0 {
                assert(r[j] == r1[j - 1]);
                assert(r1.contains(r1[j - 1]));
                if r1[j - 1] == x {
                    // !lt(x, s[0]) is the branch condition
                } else {
                    let k = choose|k: int| 0 <= k < s1.len() && s1[k] == r1[j - 1];
                    assert(s[k + 1] == r1[j - 1]);
                }
            } else {
                assert(r[i] == r1[i - 1] && r[j] == r1[j - 1]);
            }
        }
    }
}

pub proof fn lemma_sort_dups<T>(xs: Seq<T>, lt: spec_fn(T, T) -> bool)
    requires
        strict_order(lt),
        total_on(lt, xs),
    ensures
        nondesc(sort_dups(xs, lt), lt),
        sort_dups(xs, lt).to_multiset() =~= xs.to_multiset(),
        forall|y: T| #[trigger] sort_dups(xs, lt).contains(y) <==> xs.contains(y),
    decreases xs.len(),
{
    broadcast use vstd::seq_lib::group_to_multiset_ensures;
    if xs.len() > 0 {
        let xs0 = xs.drop_last();
        assert forall|x: T, y: T| xs0.contains(x) && xs0.contains(y) && x != y implies #[trigger] lt(x, y) || #[trigger] lt(y, x) by {
            let i = choose|i: int| 0 <= i < xs0.len() && xs0[i] == x;
            let j = choose|j: int| 0 <= j < xs0.len() && xs0[j] == y;
            assert(xs[i] == x && xs[j] == y);
        }
        lemma_sort_dups(xs0, lt);
        let s = sort_dups(xs0, lt);
        let x = xs.last();
        assert(xs.contains(x)) by {
            assert(xs[xs.len() - 1] == x);
        }
        assert forall|y: T| #[trigger] xs.contains(y) <==> xs0.contains(y) || y == x by {
            if xs.contains(y) && y != x {
                let k = choose|k: int| 0 <= k < xs.len() && xs[k] == y;
                assert(xs0[k] == y);
            }
            if xs0.contains(y) {
                let k = choose|k: int| 0 <= k < xs0.len() && xs0[k] == y;
                assert(xs[k] == y);
            }
        }
        assert forall|y: T| s.contains(y) && y != x implies #[trigger] lt(x, y) || lt(y, x) by {
            assert(xs.contains(y));
        }
        lemma_insert_by_dups(s, x, lt);
        assert(xs =~= xs0.push(x));
    }
}

/// Two bags with the same elements, both without inversions under a strict
/// order that is total on their distinct elements, are the same sequence.
pub proof fn lemma_nondesc_unique<T>(a: Seq<T>, b: Seq<T>, lt: spec_fn(T, T) -> bool)
    requires
        strict_order(lt),
        total_on(lt, a),
        nondesc(a, lt),
        nondesc(b, lt),
        a.to_multiset() == b.to_multiset(),
    ensures
        a == b,
    decreases a.len(),
{
    broadcast use vstd::seq_lib::group_to_multiset_ensures;
    assert(a.to_multiset().len() == a.len());
    assert(b.to_multiset().len() == b.len());
    if a.len() == 0 {
        assert(b.len() == 0);
        assert(a =~= b);
    } else {
        assert(b.len() == a.len());
        let (a0, b0) = (a[0], b[0]);
        assert(a.contains(a0));
        assert(b.contains(b0));
        assert(b.to_multiset().count(a0) > 0);
        assert(a.to_multiset().count(b0) > 0);
        if a0 != b0 {
            let j = choose|j: int| 0 <= j < b.len() && b[j] == a0;
            let i = choose|i: int| 0 <= i < a.len() && a[i] == b0;
            if lt(a0, b0) {
                assert(j > 0);
                assert(!lt(b[j], b[0]));
            } else {
                assert(lt(b0, a0));
                assert(i > 0);
                assert(!lt(a[i], a[0]));
            }
        }
        let (a1, b1) = (a.remove(0), b.remove(0));
        assert(a1.to_multiset() == b1.to_multiset());
        assert(nondesc(a1, lt)) by {
            assert forall|p: int, q: int| 0 <= p < q < a1.len() implies !lt(#[trigger] a1[q], #[trigger] a1[p]) by {
                assert(a1[p] == a[p + 1] && a1[q] == a[q + 1]);
            }
        }
        assert(nondesc(b1, lt)) by {
            assert forall|p: int, q: int| 0 <= p < q < b1.len() implies !lt(#[trigger] b1[q], #[trigger] b1[p]) by {
                assert(b1[p] == b[p + 1] && b1[q] == b[q + 1]);
            }
        }
        assert forall|x: T, y: T| a1.contains(x) && a1.contains(y) && x != y implies #[trigger] lt(x, y) || #[trigger] lt(y, x) by {
            let p = choose|p: int| 0 <= p < a1.len() && a1[p] == x;
            let q = choose|q: int| 0 <= q < a1.len() && a1[q] == y;
            assert(a[p + 1] == x && a[q + 1] == y);
        }
        lemma_nondesc_unique(a1, b1, lt);
        assert(a =~= seq![a0] + a1);
        assert(b =~= seq![b0] + b1);
    }
}

/// Bags without repeats with the same elements have the same multiset.
pub proof fn lemma_same_set_multiset<T>(a: Seq<T>, b: Seq<T>)
    requires
        forall|i: int, j: int| 0 <= i < j < a.len() ==> a[i] != a[j],
        forall|i: int, j: int| 0 <= i < j < b.len() ==> b[i] != b[j],
        forall|x: T| a.contains(x) <==> b.contains(x),
    ensures
        a.to_multiset() == b.to_multiset(),
    decreases a.len(),
{
    broadcast use vstd::seq_lib::group_to_multiset_ensures;
    if a.len() == 0 {
        if b.len() > 0 {
            assert(b.contains(b[0]));
        }
        assert(b.len() == 0);
        assert(a =~= b);
    } else {
        let x = a.last();
        assert(a.contains(x)) by {
            assert(a[a.len() - 1] == x);
        }
        let j = choose|j: int| 0 <= j < b.len() && b[j] == x;
        let (a1, b1) = (a.remove(a.len() - 1), b.remove(j));
        assert forall|y: T| a1.contains(y) <==> b1.contains(y) by {
            if a1.contains(y) {
                let k = choose|k: int| 0 <= k < a1.len() && a1[k] == y;
                assert(a[k] == y);
                assert(a[k] != a[a.len() - 1]);
                assert(y != x);
                assert(a.contains(y));
                assert(b.contains(y));
                let m = choose|m: int| 0 <= m < b.len() && b[m] == y;
                assert(m != j);
                if m < j {
                    assert(b1[m] == y);
                } else {
                    assert(b1[m - 1] == y);
                }
            }
            if b1.contains(y) {
                let k = choose|k: int| 0 <= k < b1.len() && b1[k] == y;
                let m = if k < j { k } else { k + 1 };
                assert(b[m] == y);
                if m < j {
                    assert(b[m] != b[j]);
                } else {
                    assert(b[j] != b[m]);
                }
                assert(y != x);
                assert(b.contains(y));
                assert(a.contains(y));
                let q = choose|q: int| 0 <= q < a.len() && a[q] == y;
                assert(q != a.len() - 1);
                assert(a1[q] == y);
            }
        }
        assert forall|p: int, q: int| 0 <= p < q < a1.len() implies a1[p] != a1[q] by {
            assert(a1[p] == a[p] && a1[q] == a[q]);
        }
        assert forall|p: int, q: int| 0 <= p < q < b1.len() implies b1[p] != b1[q] by {
            let pp = if p < j { p } else { p + 1 };
            let qq = if q < j { q } else { q + 1 };
            assert(b1[p] == b[pp] && b1[q] == b[qq]);
        }
        lemma_same_set_multiset(a1, b1);
        assert(a.to_multiset() =~= a1.to_multiset().insert(x));
        assert(b.to_multiset() =~= b1.to_multiset().insert(x));
    }
}

} // verus!

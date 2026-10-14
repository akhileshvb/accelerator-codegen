// Generic bag-sum combinators over sequences, plus the lemmas the Q21 proofs
// use.  A SQL table is a sequence of rows; duplicates are distinct positions,
// so `sum` has bag (multiset) semantics.
use vstd::prelude::*;
use vstd::multiset::Multiset;

verus! {

/// Σ_{x ∈ s} f(x), duplicates included.
pub open spec fn sum<A>(s: Seq<A>, f: spec_fn(A) -> int) -> int
    decreases s.len(),
{
    if s.len() == 0 {
        0
    } else {
        sum(s.drop_last(), f) + f(s.last())
    }
}

/// SQL truth value of a predicate as a count (1 or 0).
pub open spec fn ind(b: bool) -> int {
    if b {
        1
    } else {
        0
    }
}

/// The indices 0..n as a sequence.
pub open spec fn rng(n: int) -> Seq<int> {
    Seq::new(n as nat, |i: int| i)
}

// An empty list sums to 0; the base case that other sum facts build on.
pub proof fn lemma_sum_empty<A>(s: Seq<A>, f: spec_fn(A) -> int)
    requires
        s.len() == 0,
    ensures
        sum(s, f) == 0,
{
}

// Adding one item at the end raises the sum by that item's value: the step used when a loop
// builds a list row by row and keeps a running total (as in group-by/aggregate loops).
pub proof fn lemma_sum_push<A>(s: Seq<A>, x: A, f: spec_fn(A) -> int)
    ensures
        sum(s.push(x), f) == sum(s, f) + f(x),
{
    assert(s.push(x).drop_last() =~= s);
}

// The sum of the first i+1 items is the sum of the first i items plus item i: the running-total
// step for loops that scan a table one row at a time.
pub proof fn lemma_sum_take<A>(s: Seq<A>, i: int, f: spec_fn(A) -> int)
    requires
        0 <= i < s.len(),
    ensures
        sum(s.take(i + 1), f) == sum(s.take(i), f) + f(s[i]),
{
    assert(s.take(i + 1).drop_last() =~= s.take(i));
}

// The prefix covering the whole list is the list itself, so its sum is the full sum.
pub proof fn lemma_sum_take_full<A>(s: Seq<A>, f: spec_fn(A) -> int)
    ensures
        sum(s.take(s.len() as int), f) == sum(s, f),
{
    assert(s.take(s.len() as int) =~= s);
}

// A one-item list sums to that item's value.
pub proof fn lemma_sum_singleton<A>(x: A, f: spec_fn(A) -> int)
    ensures
        sum(seq![x], f) == f(x),
{
    assert(seq![x].drop_last() =~= Seq::<A>::empty());
    lemma_sum_empty(seq![x].drop_last(), f);
    assert(seq![x].last() == x);
}

// A sum over two lists concatenated is the sum over the first plus the sum over the second;
// lets aggregates over a table be computed piecewise (e.g. per half in merge sort, or old + new rows).
pub proof fn lemma_sum_concat<A>(a: Seq<A>, b: Seq<A>, f: spec_fn(A) -> int)
    ensures
        sum(a + b, f) == sum(a, f) + sum(b, f),
    decreases b.len(),
{
    if b.len() == 0 {
        assert(a + b =~= a);
    } else {
        assert((a + b).drop_last() =~= a + b.drop_last());
        lemma_sum_concat(a, b.drop_last(), f);
    }
}

// Putting one item at the front raises the sum by that item's value.
pub proof fn lemma_sum_prepend<A>(x: A, s: Seq<A>, f: spec_fn(A) -> int)
    ensures
        sum(seq![x] + s, f) == f(x) + sum(s, f),
{
    lemma_sum_concat(seq![x], s, f);
    lemma_sum_singleton(x, f);
}

// A non-empty list's sum is its first item's value plus the sum of the rest.
pub proof fn lemma_sum_drop_first<A>(s: Seq<A>, f: spec_fn(A) -> int)
    requires
        s.len() > 0,
    ensures
        sum(s, f) == f(s[0]) + sum(s.drop_first(), f),
{
    assert(s =~= seq![s[0]] + s.drop_first());
    lemma_sum_prepend(s[0], s.drop_first(), f);
}

/// Pointwise-equal summands give equal sums.
pub proof fn lemma_sum_ext<A>(s: Seq<A>, f: spec_fn(A) -> int, g: spec_fn(A) -> int)
    requires
        forall|i: int| 0 <= i < s.len() ==> f(#[trigger] s[i]) == g(s[i]),
    ensures
        sum(s, f) == sum(s, g),
    decreases s.len(),
{
    if s.len() > 0 {
        let t = s.drop_last();
        assert forall|i: int| 0 <= i < t.len() implies f(#[trigger] t[i]) == g(t[i]) by {
            assert(t[i] == s[i]);
        }
        lemma_sum_ext(t, f, g);
    }
}

/// Summands that are equal as functions give equal sums (extensional form).
pub proof fn lemma_sum_ext_all<A>(s: Seq<A>, f: spec_fn(A) -> int, g: spec_fn(A) -> int)
    requires
        forall|x: A| #[trigger] f(x) == g(x),
    ensures
        sum(s, f) == sum(s, g),
{
    lemma_sum_ext(s, f, g);
}

// If every item contributes 0 the total is 0 (e.g. no row matches a filter, so a count is 0);
// used to show empty or non-matching groups produce no output.
pub proof fn lemma_sum_zero<A>(s: Seq<A>, f: spec_fn(A) -> int)
    requires
        forall|i: int| 0 <= i < s.len() ==> f(#[trigger] s[i]) == 0,
    ensures
        sum(s, f) == 0,
    decreases s.len(),
{
    if s.len() > 0 {
        let t = s.drop_last();
        assert forall|i: int| 0 <= i < t.len() implies f(#[trigger] t[i]) == 0 by {
            assert(t[i] == s[i]);
        }
        lemma_sum_zero(t, f);
    }
}

// If every item contributes a value that is at least 0, so does the total; e.g. counts and
// multiplicities never go negative, which is needed to reason about aggregate results.
pub proof fn lemma_sum_nonneg<A>(s: Seq<A>, f: spec_fn(A) -> int)
    requires
        forall|i: int| 0 <= i < s.len() ==> f(#[trigger] s[i]) >= 0,
    ensures
        sum(s, f) >= 0,
    decreases s.len(),
{
    if s.len() > 0 {
        let t = s.drop_last();
        assert forall|i: int| 0 <= i < t.len() implies f(#[trigger] t[i]) >= 0 by {
            assert(t[i] == s[i]);
        }
        lemma_sum_nonneg(t, f);
    }
}

/// Nonnegative summands with one positive summand give a positive sum.
pub proof fn lemma_sum_pos<A>(s: Seq<A>, f: spec_fn(A) -> int, k: int)
    requires
        forall|i: int| 0 <= i < s.len() ==> f(#[trigger] s[i]) >= 0,
        0 <= k < s.len(),
        f(s[k]) > 0,
    ensures
        sum(s, f) > 0,
    decreases s.len(),
{
    let t = s.drop_last();
    assert forall|i: int| 0 <= i < t.len() implies f(#[trigger] t[i]) >= 0 by {
        assert(t[i] == s[i]);
    }
    if k == s.len() - 1 {
        lemma_sum_nonneg(t, f);
    } else {
        assert(t[k] == s[k]);
        lemma_sum_pos(t, f, k);
    }
}

/// A sum of nonnegative terms is at least any one of them.
pub proof fn lemma_sum_ge_term<A>(s: Seq<A>, f: spec_fn(A) -> int, k: int)
    requires
        forall|i: int| 0 <= i < s.len() ==> f(#[trigger] s[i]) >= 0,
        0 <= k < s.len(),
    ensures
        sum(s, f) >= f(s[k]),
    decreases s.len(),
{
    let t = s.drop_last();
    assert forall|i: int| 0 <= i < t.len() implies f(#[trigger] t[i]) >= 0 by {
        assert(t[i] == s[i]);
    }
    if k == s.len() - 1 {
        lemma_sum_nonneg(t, f);
    } else {
        assert(t[k] == s[k]);
        lemma_sum_ge_term(t, f, k);
    }
}

/// Monotonicity: f ≤ g pointwise gives Σ f ≤ Σ g.
pub proof fn lemma_sum_le<A>(s: Seq<A>, f: spec_fn(A) -> int, g: spec_fn(A) -> int)
    requires
        forall|x: A| #[trigger] f(x) <= g(x),
    ensures
        sum(s, f) <= sum(s, g),
    decreases s.len(),
{
    if s.len() > 0 {
        lemma_sum_le(s.drop_last(), f, g);
    }
}

/// A nonzero sum has a nonzero summand.
pub proof fn lemma_sum_nonzero_witness<A>(s: Seq<A>, f: spec_fn(A) -> int) -> (i: int)
    requires
        sum(s, f) != 0,
    ensures
        0 <= i < s.len(),
        f(s[i]) != 0,
{
    if forall|i: int| 0 <= i < s.len() ==> f(#[trigger] s[i]) == 0 {
        lemma_sum_zero(s, f);
        assert(false);
        0
    } else {
        choose|i: int| 0 <= i < s.len() && f(#[trigger] s[i]) != 0
    }
}

/// Σ (c · f) = c · Σ f.
pub proof fn lemma_sum_scale<A>(s: Seq<A>, f: spec_fn(A) -> int, g: spec_fn(A) -> int, c: int)
    requires
        forall|x: A| #[trigger] g(x) == c * f(x),
    ensures
        sum(s, g) == c * sum(s, f),
    decreases s.len(),
{
    if s.len() > 0 {
        lemma_sum_scale(s.drop_last(), f, g, c);
        let a = sum(s.drop_last(), f);
        let b = f(s.last());
        assert(c * a + c * b == c * (a + b)) by (nonlinear_arith);
    }
}

/// Σ (f + g) = Σ f + Σ g.
pub proof fn lemma_sum_add<A>(
    s: Seq<A>,
    f: spec_fn(A) -> int,
    g: spec_fn(A) -> int,
    h: spec_fn(A) -> int,
)
    requires
        forall|x: A| #[trigger] h(x) == f(x) + g(x),
    ensures
        sum(s, h) == sum(s, f) + sum(s, g),
    decreases s.len(),
{
    if s.len() > 0 {
        lemma_sum_add(s.drop_last(), f, g, h);
    }
}

/// Σ (f - g) = Σ f - Σ g.
pub proof fn lemma_sum_sub<A>(
    s: Seq<A>,
    f: spec_fn(A) -> int,
    g: spec_fn(A) -> int,
    h: spec_fn(A) -> int,
)
    requires
        forall|x: A| #[trigger] h(x) == f(x) - g(x),
    ensures
        sum(s, h) == sum(s, f) - sum(s, g),
    decreases s.len(),
{
    if s.len() > 0 {
        lemma_sum_sub(s.drop_last(), f, g, h);
    }
}

/// Removing position j.
pub proof fn lemma_sum_remove<A>(t: Seq<A>, j: int, f: spec_fn(A) -> int)
    requires
        0 <= j < t.len(),
    ensures
        sum(t, f) == sum(t.remove(j), f) + f(t[j]),
{
    let l = t.subrange(0, j);
    let r = t.subrange(j + 1, t.len() as int);
    assert(t =~= l + (seq![t[j]] + r));
    assert(t.remove(j) =~= l + r);
    lemma_sum_concat(l, seq![t[j]] + r, f);
    lemma_sum_prepend(t[j], r, f);
    lemma_sum_concat(l, r, f);
}

/// Bag equality (equal multisets) gives equal sums: sums are
/// permutation-invariant.
pub proof fn lemma_sum_perm<A>(s: Seq<A>, t: Seq<A>, f: spec_fn(A) -> int)
    requires
        s.to_multiset() =~= t.to_multiset(),
    ensures
        sum(s, f) == sum(t, f),
    decreases s.len(),
{
    broadcast use vstd::seq_lib::group_to_multiset_ensures;

    if s.len() == 0 {
        assert(t.to_multiset().len() == 0);
        assert(t.len() == 0);
    } else {
        let x = s.last();
        assert(s.contains(x)) by {
            assert(s[s.len() - 1] == x);
        }
        assert(s.to_multiset().count(x) > 0);
        assert(t.contains(x));
        let j = choose|j: int| 0 <= j < t.len() && t[j] == x;
        let sd = s.drop_last();
        assert(sd =~= s.remove(s.len() - 1));
        assert(sd.to_multiset() =~= s.to_multiset().remove(x));
        assert(t.remove(j).to_multiset() =~= t.to_multiset().remove(x));
        lemma_sum_perm(sd, t.remove(j), f);
        lemma_sum_remove(t, j, f);
    }
}

/// Σ_{x ∈ s} f(x) = Σ_{i < |s|} f(s[i]).
pub proof fn lemma_sum_index<A>(s: Seq<A>, f: spec_fn(A) -> int, g: spec_fn(int) -> int)
    requires
        forall|i: int| 0 <= i < s.len() ==> #[trigger] g(i) == f(s[i]),
    ensures
        sum(s, f) == sum(rng(s.len() as int), g),
    decreases s.len(),
{
    if s.len() > 0 {
        let n = s.len() as int;
        let t = s.drop_last();
        assert forall|i: int| 0 <= i < t.len() implies #[trigger] g(i) == f(t[i]) by {
            assert(t[i] == s[i]);
        }
        lemma_sum_index(t, f, g);
        assert(rng(n).drop_last() =~= rng(n - 1));
    }
}

/// Filtering a sequence is the same as zeroing the filtered-out summands.
pub proof fn lemma_sum_filter<A>(
    s: Seq<A>,
    p: spec_fn(A) -> bool,
    f: spec_fn(A) -> int,
    g: spec_fn(A) -> int,
)
    requires
        forall|x: A| #[trigger] g(x) == if p(x) { f(x) } else { 0 },
    ensures
        sum(s.filter(p), f) == sum(s, g),
    decreases s.len(),
{
    reveal_with_fuel(Seq::filter, 2);
    if s.len() > 0 {
        lemma_sum_filter(s.drop_last(), p, f, g);
        let sub = s.drop_last().filter(p);
        if p(s.last()) {
            assert(s.filter(p) =~= sub.push(s.last()));
            lemma_sum_push(sub, s.last(), f);
        } else {
            assert(s.filter(p) =~= sub);
        }
    }
}

/// Exchange of summation order for a double sum.
pub proof fn lemma_sum_swap<A, B>(
    a: Seq<A>,
    b: Seq<B>,
    g: spec_fn(A, B) -> int,
    ra: spec_fn(A) -> int,
    cb: spec_fn(B) -> int,
)
    requires
        forall|x: A| #[trigger] ra(x) == sum(b, |y: B| g(x, y)),
        forall|y: B| #[trigger] cb(y) == sum(a, |x: A| g(x, y)),
    ensures
        sum(a, ra) == sum(b, cb),
    decreases a.len(),
{
    if a.len() == 0 {
        lemma_sum_zero(b, cb);
    } else {
        let a0 = a.drop_last();
        let x = a.last();
        let cb0 = |y: B| sum(a0, |x: A| g(x, y));
        assert forall|y: B| #[trigger] cb0(y) == sum(a0, |x: A| g(x, y)) by {}
        lemma_sum_swap(a0, b, g, ra, cb0);
        // sum(b, cb) = sum(b, cb0) + sum(b, g(x, .))
        let gx = |y: B| g(x, y);
        assert forall|y: B| #[trigger] cb(y) == cb0(y) + gx(y) by {
            assert(a.drop_last() == a0);
        }
        lemma_sum_add(b, cb0, gx, cb);
        assert(ra(x) == sum(b, gx));
    }
}

/// Σ over a mapped sequence.
pub proof fn lemma_sum_map<A, B>(s: Seq<A>, g: spec_fn(A) -> B, f: spec_fn(B) -> int, h: spec_fn(A) -> int)
    requires
        forall|x: A| #[trigger] h(x) == f(g(x)),
    ensures
        sum(s.map_values(g), f) == sum(s, h),
    decreases s.len(),
{
    if s.len() > 0 {
        assert(s.map_values(g).drop_last() =~= s.drop_last().map_values(g));
        lemma_sum_map(s.drop_last(), g, f, h);
    }
}

/// Exactly one position matches: the sum is that position's value.
pub proof fn lemma_sum_single_match<A>(s: Seq<A>, f: spec_fn(A) -> int, k: int)
    requires
        0 <= k < s.len(),
        forall|i: int| 0 <= i < s.len() && i != k ==> f(#[trigger] s[i]) == 0,
    ensures
        sum(s, f) == f(s[k]),
{
    lemma_sum_remove(s, k, f);
    let t = s.remove(k);
    assert forall|i: int| 0 <= i < t.len() implies f(#[trigger] t[i]) == 0 by {
        if i < k {
            assert(t[i] == s[i]);
        } else {
            assert(t[i] == s[i + 1]);
        }
    }
    lemma_sum_zero(t, f);
}

/// Updating one position changes the sum by the difference.
pub proof fn lemma_sum_update<A>(s: Seq<A>, k: int, x: A, f: spec_fn(A) -> int)
    requires
        0 <= k < s.len(),
    ensures
        sum(s.update(k, x), f) == sum(s, f) - f(s[k]) + f(x),
{
    lemma_sum_remove(s, k, f);
    lemma_sum_remove(s.update(k, x), k, f);
    assert(s.update(k, x).remove(k) =~= s.remove(k));
}

/// A distinct key list: no duplicates.
pub open spec fn distinct<K>(ks: Seq<K>) -> bool {
    forall|i: int, j: int| 0 <= i < ks.len() && 0 <= j < ks.len() && i != j ==> ks[i] != ks[j]
}

/// Group-by decomposition: if every row with a nonzero summand has its key in
/// the distinct list `ks`, then Σ_x f(x) = Σ_{k ∈ ks} Σ_{x : key(x) = k} f(x).
pub proof fn lemma_sum_group<A, K>(
    s: Seq<A>,
    ks: Seq<K>,
    key: spec_fn(A) -> K,
    f: spec_fn(A) -> int,
    per: spec_fn(K) -> int,
)
    requires
        distinct(ks),
        forall|i: int| 0 <= i < s.len() && f(s[i]) != 0 ==> ks.contains(key(#[trigger] s[i])),
        forall|k: K| #[trigger] per(k) == sum(s, |x: A| if key(x) == k { f(x) } else { 0 }),
    ensures
        sum(s, f) == sum(ks, per),
{
    // Exchange: Σ_k Σ_x [key x = k] f x = Σ_x Σ_k [key x = k] f x.
    let g = |k: K, x: A| if key(x) == k { f(x) } else { 0 };
    let cx = |x: A| sum(ks, |k: K| g(k, x));
    assert forall|k: K| #[trigger] per(k) == sum(s, |x: A| g(k, x)) by {
        lemma_sum_ext_all(s, |x: A| if key(x) == k { f(x) } else { 0 }, |x: A| g(k, x));
    }
    assert forall|x: A| #[trigger] cx(x) == sum(ks, |k: K| g(k, x)) by {}
    lemma_sum_swap(ks, s, g, per, cx);
    // For each row, Σ_k [key x = k] f x = f x (key present, once) or 0 (then f x = 0).
    assert forall|i: int| 0 <= i < s.len() implies cx(#[trigger] s[i]) == f(s[i]) by {
        let x = s[i];
        lemma_sum_indicator_distinct(ks, key(x), f(x), |k: K| g(k, x));
    }
    lemma_sum_ext(s, cx, f);
}

/// Σ_{k ∈ ks} [k = k0] · v = v if k0 ∈ ks (once, distinct) else 0 — here in
/// the weaker form: equals v when k0 ∈ ks, and equals 0 when v = 0.
pub proof fn lemma_sum_indicator_distinct<K>(ks: Seq<K>, k0: K, v: int, h: spec_fn(K) -> int)
    requires
        distinct(ks),
        forall|k: K| #[trigger] h(k) == if k == k0 { v } else { 0 },
        ks.contains(k0) || v == 0,
    ensures
        sum(ks, h) == v,
    decreases ks.len(),
{
    if ks.len() == 0 {
    } else {
        let t = ks.drop_last();
        assert(distinct(t)) by {
            assert forall|i: int, j: int|
                0 <= i < t.len() && 0 <= j < t.len() && i != j implies t[i] != t[j] by {
                assert(t[i] == ks[i] && t[j] == ks[j]);
            }
        }
        if ks.last() == k0 {
            // k0 not in t
            assert forall|i: int| 0 <= i < t.len() implies h(#[trigger] t[i]) == 0 by {
                assert(t[i] == ks[i]);
                assert(ks[i] != ks[ks.len() - 1]);
            }
            lemma_sum_zero(t, h);
        } else {
            if v != 0 {
                let j = choose|j: int| 0 <= j < ks.len() && ks[j] == k0;
                assert(j != ks.len() - 1);
                assert(t[j] == k0);
                assert(t.contains(k0));
            }
            lemma_sum_indicator_distinct(t, k0, v, h);
        }
    }
}

/// Two bags related by signed deltas: Σ over `after` equals Σ over `before`
/// plus Σ sign·f over the deltas, for every row weight f.  (This is "after =
/// before + deltas" as bags, stated without multisets.)
pub open spec fn bag_step<A>(before: Seq<A>, after: Seq<A>, deltas: Seq<(A, int)>) -> bool {
    forall|f: spec_fn(A) -> int|
        #[trigger] sum(after, f) == sum(before, f) + sum(deltas, |d: (A, int)| d.1 * f(d.0))
}

} // verus!

// ── bag_step in its textbook multiset form ────────────────────────────────
verus! {

/// Rows inserted (+1) / deleted (-1) by a delta list.
pub open spec fn plus_rows<A>(d: Seq<(A, int)>) -> Seq<A> {
    d.filter(|x: (A, int)| x.1 == 1).map_values(|x: (A, int)| x.0)
}

pub open spec fn minus_rows<A>(d: Seq<(A, int)>) -> Seq<A> {
    d.filter(|x: (A, int)| x.1 == -1).map_values(|x: (A, int)| x.0)
}

/// Every delta is a single-row insert or delete.
pub open spec fn unit_signs<A>(d: Seq<(A, int)>) -> bool {
    forall|i: int| 0 <= i < d.len() ==> (#[trigger] d[i]).1 == 1 || d[i].1 == -1
}

// The 'is it this value?' weight: 1 for rows equal to v and 0 otherwise. Summing it over a list
// counts how many times v occurs, tying multiset counts to sums.
pub open spec fn eqv<A>(v: A) -> spec_fn(A) -> int {
    |x: A| ind(x == v)
}

// How many times v occurs in a list (its multiset count) equals the sum of the is-it-v weight over
// the list; proved by adding one row at a time. It turns sum equations into per-value counts.
pub proof fn lemma_count_as_sum<A>(s: Seq<A>, v: A)
    ensures
        s.to_multiset().count(v) == sum(s, eqv(v)),
    decreases s.len(),
{
    broadcast use vstd::seq_lib::group_to_multiset_ensures, vstd::multiset::group_multiset_axioms;

    if s.len() > 0 {
        let t = s.drop_last();
        lemma_count_as_sum(t, v);
        assert(s =~= t.push(s.last()));
    } else {
        assert(s.to_multiset() =~= Multiset::<A>::empty());
    }
}

// Turning a list into a multiset commutes with concatenation: the multiset of a joined list is the
// union (sum) of the two multisets; used to state update results as multiset equations.
pub proof fn lemma_to_multiset_concat<A>(a: Seq<A>, b: Seq<A>)
    ensures
        (a + b).to_multiset() =~= a.to_multiset().add(b.to_multiset()),
    decreases b.len(),
{
    broadcast use vstd::seq_lib::group_to_multiset_ensures, vstd::multiset::group_multiset_axioms;

    if b.len() == 0 {
        assert(a + b =~= a);
        assert(b.to_multiset() =~= Multiset::<A>::empty());
    } else {
        let b0 = b.drop_last();
        lemma_to_multiset_concat(a, b0);
        assert(a + b =~= (a + b0).push(b.last()));
        assert(b =~= b0.push(b.last()));
    }
}

/// With unit signs, Σ sign·f(row) = Σ_inserted f − Σ_deleted f.
pub proof fn lemma_delta_split<A>(d: Seq<(A, int)>, f: spec_fn(A) -> int)
    requires
        unit_signs(d),
    ensures
        sum(d, |x: (A, int)| x.1 * f(x.0)) == sum(plus_rows(d), f) - sum(minus_rows(d), f),
    decreases d.len(),
{
    reveal_with_fuel(Seq::filter, 2);
    if d.len() > 0 {
        let t = d.drop_last();
        let x = d.last();
        assert(unit_signs(t)) by {
            assert forall|i: int| 0 <= i < t.len() implies (#[trigger] t[i]).1 == 1 || t[i].1 == -1 by {
                assert(t[i] == d[i]);
            }
        }
        lemma_delta_split(t, f);
        let pp = |y: (A, int)| y.1 == 1;
        let pm = |y: (A, int)| y.1 == -1;
        let g = |y: (A, int)| y.0;
        assert(d.len() - 1 < d.len());
        assert(x == d[d.len() - 1]);
        if x.1 == 1 {
            assert(d.filter(pp) =~= t.filter(pp).push(x));
            assert(d.filter(pm) =~= t.filter(pm));
            assert(plus_rows(d) =~= plus_rows(t).push(x.0));
            lemma_sum_push(plus_rows(t), x.0, f);
        } else {
            assert(d.filter(pm) =~= t.filter(pm).push(x));
            assert(d.filter(pp) =~= t.filter(pp));
            assert(minus_rows(d) =~= minus_rows(t).push(x.0));
            lemma_sum_push(minus_rows(t), x.0, f);
        }
    }
}

/// bag_step is exactly the multiset equation after + deleted = before + inserted.
pub proof fn lemma_bag_step_multiset<A>(before: Seq<A>, after: Seq<A>, d: Seq<(A, int)>)
    requires
        unit_signs(d),
    ensures
        bag_step(before, after, d) <==> after.to_multiset().add(minus_rows(d).to_multiset())
            =~= before.to_multiset().add(plus_rows(d).to_multiset()),
{
    broadcast use vstd::multiset::group_multiset_axioms;

    let lhs = after.to_multiset().add(minus_rows(d).to_multiset());
    let rhs = before.to_multiset().add(plus_rows(d).to_multiset());
    lemma_to_multiset_concat(after, minus_rows(d));
    lemma_to_multiset_concat(before, plus_rows(d));
    if lhs =~= rhs {
        assert forall|f: spec_fn(A) -> int| #[trigger]
            sum(after, f) == sum(before, f) + sum(d, |x: (A, int)| x.1 * f(x.0)) by {
            lemma_sum_perm(after + minus_rows(d), before + plus_rows(d), f);
            lemma_sum_concat(after, minus_rows(d), f);
            lemma_sum_concat(before, plus_rows(d), f);
            lemma_delta_split(d, f);
        }
    }
    if bag_step(before, after, d) {
        assert forall|v: A| lhs.count(v) == rhs.count(v) by {
            let f = eqv(v);
            assert(sum(after, f) == sum(before, f) + sum(d, |x: (A, int)| x.1 * f(x.0)));
            lemma_delta_split(d, f);
            lemma_count_as_sum(after, v);
            lemma_count_as_sum(before, v);
            lemma_count_as_sum(plus_rows(d), v);
            lemma_count_as_sum(minus_rows(d), v);
        }
        assert(lhs =~= rhs);
    }
}

/// Two lists whose sums agree for every weight are the same bag.
pub proof fn lemma_sums_eq_multiset<A>(a: Seq<A>, b: Seq<A>)
    requires
        forall|f: spec_fn(A) -> int| #[trigger] sum(a, f) == sum(b, f),
    ensures
        a.to_multiset() =~= b.to_multiset(),
{
    broadcast use vstd::multiset::group_multiset_axioms;

    assert forall|v: A| a.to_multiset().count(v) == b.to_multiset().count(v) by {
        lemma_count_as_sum(a, v);
        lemma_count_as_sum(b, v);
        assert(sum(a, eqv(v)) == sum(b, eqv(v)));
    }
}

} // verus!

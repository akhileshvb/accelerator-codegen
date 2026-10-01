// Bags of rows for the generated specs: concatenation of nested bags
// (the FROM-clause product), duplicate removal (GROUP BY, DISTINCT),
// MIN / MAX, and the error bound for floating-point aggregates.
use vstd::prelude::*;
use crate::sums::*;

verus! {

/// s[0] ++ s[1] ++ ... (a bag of bags, flattened).
pub open spec fn flat<A>(s: Seq<Seq<A>>) -> Seq<A>
    decreases s.len(),
{
    if s.len() == 0 {
        Seq::empty()
    } else {
        flat(s.drop_last()) + s.last()
    }
}

pub proof fn lemma_flat_contains<A>(s: Seq<Seq<A>>, x: A)
    ensures
        flat(s).contains(x) <==> exists|i: int| 0 <= i < s.len() && (#[trigger] s[i]).contains(x),
    decreases s.len(),
{
    if s.len() > 0 {
        let s0 = s.drop_last();
        lemma_flat_contains(s0, x);
        let a = flat(s0);
        let b = s.last();
        if flat(s).contains(x) {
            let k = choose|k: int| 0 <= k < flat(s).len() && flat(s)[k] == x;
            if k < a.len() {
                assert(a[k] == x);
                let i = choose|i: int| 0 <= i < s0.len() && (#[trigger] s0[i]).contains(x);
                assert(s[i] == s0[i]);
            } else {
                assert(b[k - a.len()] == x);
                assert(s[s.len() - 1] == b);
            }
        }
        if exists|i: int| 0 <= i < s.len() && (#[trigger] s[i]).contains(x) {
            let i = choose|i: int| 0 <= i < s.len() && (#[trigger] s[i]).contains(x);
            if i < s.len() - 1 {
                assert(s0[i] == s[i]);
                let k = choose|k: int| 0 <= k < a.len() && a[k] == x;
                assert(flat(s)[k] == x);
            } else {
                let k = choose|k: int| 0 <= k < b.len() && b[k] == x;
                assert(flat(s)[a.len() + k] == x);
            }
        }
    }
}

/// Σ over a flattened bag is Σ over the bags of their sums.
pub proof fn lemma_sum_flat<A>(s: Seq<Seq<A>>, f: spec_fn(A) -> int)
    ensures
        sum(flat(s), f) == sum(s, |x: Seq<A>| sum(x, f)),
    decreases s.len(),
{
    if s.len() > 0 {
        lemma_sum_flat(s.drop_last(), f);
        lemma_sum_concat(flat(s.drop_last()), s.last(), f);
    }
}

/// Σ over a flattened map is the nested Σ.
pub proof fn lemma_sum_flat_map<A, B>(s: Seq<A>, g: spec_fn(A) -> Seq<B>, f: spec_fn(B) -> int)
    ensures
        sum(flat(s.map_values(g)), f) == sum(s, |a: A| sum(g(a), f)),
{
    lemma_sum_flat(s.map_values(g), f);
    lemma_sum_map(s, g, |x: Seq<B>| sum(x, f), |a: A| sum(g(a), f));
}

/// Σ over [x] or [] (a tuple that does or does not satisfy WHERE).
pub proof fn lemma_sum_opt<B>(c: bool, x: B, f: spec_fn(B) -> int)
    ensures
        sum(if c { seq![x] } else { Seq::<B>::empty() }, f) == if c { f(x) } else { 0 },
{
    if c {
        lemma_sum_singleton(x, f);
    } else {
        lemma_sum_empty(Seq::<B>::empty(), f);
    }
}

/// The distinct elements of s (first occurrences, in order).
pub open spec fn dedup<A>(s: Seq<A>) -> Seq<A>
    decreases s.len(),
{
    if s.len() == 0 {
        Seq::empty()
    } else {
        let r = dedup(s.drop_last());
        if r.contains(s.last()) {
            r
        } else {
            r.push(s.last())
        }
    }
}

pub open spec fn no_dups<A>(s: Seq<A>) -> bool {
    forall|i: int, j: int| 0 <= i < j < s.len() ==> s[i] != s[j]
}

pub proof fn lemma_dedup<A>(s: Seq<A>)
    ensures
        no_dups(dedup(s)),
        forall|x: A| #[trigger] dedup(s).contains(x) <==> s.contains(x),
    decreases s.len(),
{
    if s.len() > 0 {
        let s0 = s.drop_last();
        lemma_dedup(s0);
        let r = dedup(s0);
        let x = s.last();
        assert forall|y: A| #[trigger] s.contains(y) <==> s0.contains(y) || y == x by {
            if s.contains(y) && y != x {
                let k = choose|k: int| 0 <= k < s.len() && s[k] == y;
                assert(s0[k] == y);
            }
            if s0.contains(y) {
                let k = choose|k: int| 0 <= k < s0.len() && s0[k] == y;
                assert(s[k] == y);
            }
            if y == x {
                assert(s[s.len() - 1] == x);
            }
        }
        if !r.contains(x) {
            let r2 = r.push(x);
            assert forall|y: A| #[trigger] r2.contains(y) <==> r.contains(y) || y == x by {
                if r2.contains(y) {
                    let k = choose|k: int| 0 <= k < r2.len() && r2[k] == y;
                    if k < r.len() {
                        assert(r[k] == y);
                    }
                }
                if r.contains(y) {
                    let k = choose|k: int| 0 <= k < r.len() && r[k] == y;
                    assert(r2[k] == y);
                }
                if y == x {
                    assert(r2[r.len() as int] == x);
                }
            }
            assert forall|i: int, j: int| 0 <= i < j < r2.len() implies r2[i] != r2[j] by {
                if j == r.len() {
                    assert(r.contains(r[i]));
                }
            }
        }
    }
}

/// COUNT(DISTINCT e) over a bag of values.
pub open spec fn distinct_count<A>(s: Seq<A>) -> int {
    dedup(s).len() as int
}

/// MIN over a non-empty bag (0 for an empty one, never used).
pub open spec fn min_of(s: Seq<int>) -> int
    decreases s.len(),
{
    if s.len() == 0 {
        0
    } else if s.len() == 1 {
        s[0]
    } else {
        let m = min_of(s.drop_last());
        if s.last() < m { s.last() } else { m }
    }
}

/// MAX over a non-empty bag (0 for an empty one, never used).
pub open spec fn max_of(s: Seq<int>) -> int
    decreases s.len(),
{
    if s.len() == 0 {
        0
    } else if s.len() == 1 {
        s[0]
    } else {
        let m = max_of(s.drop_last());
        if s.last() > m { s.last() } else { m }
    }
}

/// MIN over a non-empty bag of text values (byte order).
pub open spec fn min_text(s: Seq<Seq<u8>>) -> Seq<u8>
    decreases s.len(),
{
    if s.len() <= 1 {
        if s.len() == 0 { Seq::empty() } else { s[0] }
    } else {
        let m = min_text(s.drop_last());
        if crate::bytes::bytes_lt(s.last(), m) { s.last() } else { m }
    }
}

/// MAX over a non-empty bag of text values (byte order).
pub open spec fn max_text(s: Seq<Seq<u8>>) -> Seq<u8>
    decreases s.len(),
{
    if s.len() <= 1 {
        if s.len() == 0 { Seq::empty() } else { s[0] }
    } else {
        let m = max_text(s.drop_last());
        if crate::bytes::bytes_lt(m, s.last()) { s.last() } else { m }
    }
}

pub open spec fn abs(x: int) -> int {
    if x < 0 { -x } else { x }
}

pub open spec fn pow2(n: nat) -> int
    decreases n,
{
    if n == 0 { 1 } else { 2 * pow2((n - 1) as nat) }
}

/// The error bound for a floating-point (IEEE binary64) aggregate: v, the
/// engine's result, is within gamma_m * a of the exact value e, where a is
/// the same computation with every operand replaced by its absolute value,
/// m is the number of rounded operations along any path of the computation,
/// and gamma_m = m u / (1 - m u), u = 2^-53.  This holds for every order of
/// the additions (any summation tree has depth <= n - 1).  v, e and a are at
/// the same scale.  When m u >= 1 the bound says nothing.
pub open spec fn float_within(v: int, e: int, a: int, m: int) -> bool {
    v == e || m >= pow2(53) || (pow2(53) - m) * abs(v - e) <= m * a
}

} // verus!

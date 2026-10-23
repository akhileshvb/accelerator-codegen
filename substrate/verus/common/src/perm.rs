// Permutation invariance of the relational operators.  A table is a
// sequence, but a SQL table is a bag: the generated specs (sql2verus) must
// mean the same thing for every ordering of the rows.  `bag_eq(s, t)` says
// two sequences hold the same rows with the same multiplicities; the lemmas
// here say each operator the generated specs are built from (map, filter-map,
// flattened map = join, dedup, EXISTS-style quantifiers, MIN / MAX) takes
// bag-equal inputs to bag-equal (or equal) outputs.  Each takes two versions
// of the function argument that agree pointwise: the generated definitions
// mention the database, so the closure over `db` and the closure over the
// permuted `db2` are different terms with equal values.
use vstd::prelude::*;
use vstd::multiset::Multiset;
use crate::sums::*;
use crate::rel::*;

verus! {

/// s and t hold the same rows, each the same number of times.
pub open spec fn bag_eq<A>(s: Seq<A>, t: Seq<A>) -> bool {
    s.to_multiset() =~= t.to_multiset()
}

pub proof fn lemma_bag_eq_len<A>(s: Seq<A>, t: Seq<A>)
    requires
        bag_eq(s, t),
    ensures
        s.len() == t.len(),
{
    broadcast use vstd::seq_lib::group_to_multiset_ensures;
    assert(s.to_multiset().len() == t.to_multiset().len());
}

pub proof fn lemma_bag_eq_contains<A>(s: Seq<A>, t: Seq<A>, x: A)
    requires
        bag_eq(s, t),
    ensures
        s.contains(x) == t.contains(x),
{
    broadcast use vstd::seq_lib::group_to_multiset_ensures;
    assert(s.to_multiset().count(x) == t.to_multiset().count(x));
    lemma_contains_count(s, x);
    lemma_contains_count(t, x);
}

/// A row is in the sequence exactly when its multiset count is positive.
pub proof fn lemma_contains_count<A>(s: Seq<A>, x: A)
    ensures
        s.contains(x) <==> s.to_multiset().count(x) > 0,
    decreases s.len(),
{
    broadcast use vstd::seq_lib::group_to_multiset_ensures, vstd::multiset::group_multiset_axioms;
    if s.len() == 0 {
        assert(s.to_multiset() =~= Multiset::<A>::empty());
    } else {
        let t = s.drop_last();
        lemma_contains_count(t, x);
        assert(s =~= t.push(s.last()));
        if s.last() == x {
            assert(s[s.len() - 1] == x);
        }
        if t.contains(x) {
            let k = choose|k: int| 0 <= k < t.len() && t[k] == x;
            assert(s[k] == x);
        }
        if s.contains(x) && s.last() != x {
            let k = choose|k: int| 0 <= k < s.len() && s[k] == x;
            assert(t[k] == x);
        }
    }
}

/// Mapping bag-equal sequences by pointwise-equal functions.
pub proof fn lemma_bag_eq_map<A, B>(s: Seq<A>, t: Seq<A>, f: spec_fn(A) -> B, g: spec_fn(A) -> B)
    requires
        bag_eq(s, t),
        forall|a: A| #[trigger] f(a) == g(a),
    ensures
        bag_eq(s.map_values(f), t.map_values(g)),
{
    assert forall|h: spec_fn(B) -> int| #[trigger] sum(s.map_values(f), h) == sum(t.map_values(g), h) by {
        let hf = |a: A| h(f(a));
        let hg = |a: A| h(g(a));
        lemma_sum_map(s, f, h, hf);
        lemma_sum_map(t, g, h, hg);
        lemma_sum_ext_all(t, hf, hg);
        lemma_sum_perm(s, t, hf);
    }
    lemma_sums_eq_multiset(s.map_values(f), t.map_values(g));
}

/// Σ over a filtered map.
pub proof fn lemma_sum_map_where<A, B>(s: Seq<A>, keep: spec_fn(A) -> bool, f: spec_fn(A) -> B, h: spec_fn(B) -> int, k: spec_fn(A) -> int)
    requires
        forall|a: A| #[trigger] k(a) == if keep(a) { h(f(a)) } else { 0 },
    ensures
        sum(crate::order::map_where(s, keep, f), h) == sum(s, k),
    decreases s.len(),
{
    if s.len() > 0 {
        let s0 = s.drop_last();
        lemma_sum_map_where(s0, keep, f, h, k);
        let r = crate::order::map_where(s0, keep, f);
        if keep(s.last()) {
            lemma_sum_push(r, f(s.last()), h);
        }
    }
}

/// Filter-mapping bag-equal sequences by pointwise-equal predicates and functions.
pub proof fn lemma_bag_eq_map_where<A, B>(
    s: Seq<A>,
    t: Seq<A>,
    keep1: spec_fn(A) -> bool,
    keep2: spec_fn(A) -> bool,
    f1: spec_fn(A) -> B,
    f2: spec_fn(A) -> B,
)
    requires
        bag_eq(s, t),
        forall|a: A| #[trigger] keep1(a) == keep2(a),
        forall|a: A| #[trigger] f1(a) == f2(a),
    ensures
        bag_eq(crate::order::map_where(s, keep1, f1), crate::order::map_where(t, keep2, f2)),
{
    assert forall|h: spec_fn(B) -> int| #[trigger] sum(crate::order::map_where(s, keep1, f1), h) == sum(crate::order::map_where(t, keep2, f2), h) by {
        let k1 = |a: A| if keep1(a) { h(f1(a)) } else { 0 };
        let k2 = |a: A| if keep2(a) { h(f2(a)) } else { 0 };
        lemma_sum_map_where(s, keep1, f1, h, k1);
        lemma_sum_map_where(t, keep2, f2, h, k2);
        lemma_sum_ext_all(t, k1, k2);
        lemma_sum_perm(s, t, k1);
    }
    lemma_sums_eq_multiset(crate::order::map_where(s, keep1, f1), crate::order::map_where(t, keep2, f2));
}

/// A join: the flattened map over bag-equal sequences with bag-equal inner results.
pub proof fn lemma_bag_eq_flat_map<A, B>(s: Seq<A>, t: Seq<A>, g1: spec_fn(A) -> Seq<B>, g2: spec_fn(A) -> Seq<B>)
    requires
        bag_eq(s, t),
        forall|a: A| #[trigger] bag_eq(g1(a), g2(a)),
    ensures
        bag_eq(flat(s.map_values(g1)), flat(t.map_values(g2))),
{
    assert forall|h: spec_fn(B) -> int| #[trigger] sum(flat(s.map_values(g1)), h) == sum(flat(t.map_values(g2)), h) by {
        let k1 = |a: A| sum(g1(a), h);
        let k2 = |a: A| sum(g2(a), h);
        lemma_sum_flat_map(s, g1, h);
        lemma_sum_flat_map(t, g2, h);
        assert forall|a: A| #[trigger] k1(a) == k2(a) by {
            assert(bag_eq(g1(a), g2(a)));
            lemma_sum_perm(g1(a), g2(a), h);
        }
        lemma_sum_ext_all(t, k1, k2);
        lemma_sum_perm(s, t, k1);
    }
    lemma_sums_eq_multiset(flat(s.map_values(g1)), flat(t.map_values(g2)));
}

/// Removing repeats.
pub proof fn lemma_bag_eq_dedup<A>(s: Seq<A>, t: Seq<A>)
    requires
        bag_eq(s, t),
    ensures
        bag_eq(dedup(s), dedup(t)),
{
    lemma_dedup(s);
    lemma_dedup(t);
    assert forall|x: A| dedup(s).contains(x) <==> dedup(t).contains(x) by {
        lemma_bag_eq_contains(s, t, x);
    }
    crate::order::lemma_same_set_multiset(dedup(s), dedup(t));
}

/// min_of is a member that is at most every member.
pub proof fn lemma_min_of_props(s: Seq<int>)
    requires
        s.len() > 0,
    ensures
        s.contains(min_of(s)),
        forall|i: int| 0 <= i < s.len() ==> min_of(s) <= #[trigger] s[i],
    decreases s.len(),
{
    if s.len() == 1 {
        assert(s[0] == min_of(s));
    } else {
        let t = s.drop_last();
        lemma_min_of_props(t);
        let m = min_of(t);
        let k = choose|k: int| 0 <= k < t.len() && t[k] == m;
        assert(s[k] == m);
        assert(s[s.len() - 1] == s.last());
        assert forall|i: int| 0 <= i < s.len() implies min_of(s) <= #[trigger] s[i] by {
            if i < t.len() {
                assert(t[i] == s[i]);
            }
        }
    }
}

pub proof fn lemma_max_of_props(s: Seq<int>)
    requires
        s.len() > 0,
    ensures
        s.contains(max_of(s)),
        forall|i: int| 0 <= i < s.len() ==> max_of(s) >= #[trigger] s[i],
    decreases s.len(),
{
    if s.len() == 1 {
        assert(s[0] == max_of(s));
    } else {
        let t = s.drop_last();
        lemma_max_of_props(t);
        let m = max_of(t);
        let k = choose|k: int| 0 <= k < t.len() && t[k] == m;
        assert(s[k] == m);
        assert(s[s.len() - 1] == s.last());
        assert forall|i: int| 0 <= i < s.len() implies max_of(s) >= #[trigger] s[i] by {
            if i < t.len() {
                assert(t[i] == s[i]);
            }
        }
    }
}

/// MIN of bag-equal sequences.
pub proof fn lemma_min_of_perm(s: Seq<int>, t: Seq<int>)
    requires
        bag_eq(s, t),
    ensures
        min_of(s) == min_of(t),
{
    lemma_bag_eq_len(s, t);
    if s.len() > 0 {
        lemma_min_of_props(s);
        lemma_min_of_props(t);
        lemma_bag_eq_contains(s, t, min_of(s));
        lemma_bag_eq_contains(s, t, min_of(t));
        let i = choose|i: int| 0 <= i < t.len() && t[i] == min_of(s);
        let j = choose|j: int| 0 <= j < s.len() && s[j] == min_of(t);
    }
}

/// MAX of bag-equal sequences.
pub proof fn lemma_max_of_perm(s: Seq<int>, t: Seq<int>)
    requires
        bag_eq(s, t),
    ensures
        max_of(s) == max_of(t),
{
    lemma_bag_eq_len(s, t);
    if s.len() > 0 {
        lemma_max_of_props(s);
        lemma_max_of_props(t);
        lemma_bag_eq_contains(s, t, max_of(s));
        lemma_bag_eq_contains(s, t, max_of(t));
        let i = choose|i: int| 0 <= i < t.len() && t[i] == max_of(s);
        let j = choose|j: int| 0 <= j < s.len() && s[j] == max_of(t);
    }
}

/// COUNT(DISTINCT) of bag-equal sequences.
pub proof fn lemma_distinct_count_perm<A>(s: Seq<A>, t: Seq<A>)
    requires
        bag_eq(s, t),
    ensures
        distinct_count(s) == distinct_count(t),
{
    lemma_bag_eq_dedup(s, t);
    lemma_bag_eq_len(dedup(s), dedup(t));
}

} // verus!

// Size bounds on sums: what a bag of at most 2^40 rows can add up to.
use vstd::prelude::*;
use verus_common::sums::*;

verus! {

/// Summands within [-b, b] add up to within [-len * b, len * b].
pub proof fn lemma_sum_bounded<A>(s: Seq<A>, f: spec_fn(A) -> int, b: int)
    requires b >= 0, forall|i: int| 0 <= i < s.len() ==> -b <= f(#[trigger] s[i]) <= b,
    ensures -(s.len() as int * b) <= sum(s, f) <= s.len() as int * b,
    decreases s.len(),
{
    if s.len() > 0 {
        let t = s.drop_last();
        assert forall|i: int| 0 <= i < t.len() implies -b <= f(#[trigger] t[i]) <= b by { assert(t[i] == s[i]); }
        lemma_sum_bounded(t, f, b);
        assert(s.len() as int * b == t.len() as int * b + b) by (nonlinear_arith) requires s.len() == t.len() + 1;
        assert(s =~= t.push(s.last()));
        lemma_sum_push(t, s.last(), f);
    }
}

/// A sum of 0/1 indicators is at most the number of rows.
pub proof fn lemma_indicator_le_len<A>(s: Seq<A>, f: spec_fn(A) -> int)
    requires forall|i: int| 0 <= i < s.len() ==> 0 <= f(#[trigger] s[i]) <= 1,
    ensures 0 <= sum(s, f) <= s.len(),
{
    lemma_sum_bounded(s, f, 1);
    lemma_sum_nonneg(s, f);
}

/// Over nonnegative summands a prefix sums to no more than the whole.
pub proof fn lemma_prefix_le<A>(s: Seq<A>, f: spec_fn(A) -> int, count: int)
    requires 0 <= count <= s.len(), forall|i: int| 0 <= i < s.len() ==> f(#[trigger] s[i]) >= 0,
    ensures sum(s.take(count), f) <= sum(s, f),
{
    let rest = s.skip(count);
    assert forall|i: int| 0 <= i < rest.len() implies f(#[trigger] rest[i]) >= 0 by { assert(rest[i] == s[count + i]); }
    lemma_sum_nonneg(rest, f);
    assert(s =~= s.take(count) + rest);
    lemma_sum_concat(s.take(count), rest, f);
}

/// Summands equal to 1 add up to the number of rows.
pub proof fn lemma_sum_ones<A>(s: Seq<A>, f: spec_fn(A) -> int)
    requires forall|i: int| 0 <= i < s.len() ==> f(#[trigger] s[i]) == 1,
    ensures sum(s, f) == s.len(),
    decreases s.len(),
{
    if s.len() > 0 {
        let t = s.drop_last();
        assert forall|i: int| 0 <= i < t.len() implies f(#[trigger] t[i]) == 1 by { assert(t[i] == s[i]); }
        lemma_sum_ones(t, f);
        assert(s =~= t.push(s.last()));
        lemma_sum_push(t, s.last(), f);
    }
}

}
